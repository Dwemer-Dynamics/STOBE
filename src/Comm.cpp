#include "Comm.h"
#include "Globals.h"
#include "Utils.h"
#include <algorithm>
#include <cstdint>
#include <cctype>
#include <ctime>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include <winhttp.h>

void PostToStobe(const std::wstring &endpoint, const std::string &jsonData);

namespace {
static const INTERNET_PORT kDefaultServerPort = 8083;
static const INTERNET_PORT kDiscoveryPort = 7135;
const wchar_t *kDefaultServerHost = L"127.0.0.1";

std::wstring g_stobeHost = kDefaultServerHost;
INTERNET_PORT g_stobePort = kDefaultServerPort;
LONG g_discoveryDone = 0; // 0 = not done, 1 = done (interlocked)
LONG g_dwemerDistroConnected = 0; // 0 = disconnected, 1 = connected
DWORD g_dwemerDistroLastSuccessTick = 0;

struct SerialHttpTask {
  std::wstring endpoint;
  std::string data;
};

std::deque<SerialHttpTask> g_serialHttpQueue;
CRITICAL_SECTION g_serialHttpQueueMutex;
HANDLE g_serialHttpQueueSignal = NULL;
HANDLE g_serialHttpThread = NULL;
LONG g_serialHttpInitDone = 0;
LONG g_serialHttpDropped = 0;
LONG g_serialHttpLastDropLogTick = 0;
CRITICAL_SECTION g_asyncPostDedupMutex;
LONG g_asyncPostDedupInitDone = 0;
DWORD g_asyncPostDedupLastPruneTick = 0;

const size_t kSerialHttpQueueCap = 1024;
const DWORD kSerialHttpDropLogCooldownMs = 5000;
const DWORD kAsyncPostDedupRetentionMs = 60000;
struct AsyncPostDedupState {
  DWORD lastSentTick;
};

std::map<std::string, AsyncPostDedupState> g_asyncPostDedupByFingerprint;

std::string DecodeBase64(const std::string &input) {
  static const std::string alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  unsigned int value = 0;
  int bits = -8;
  for (size_t i = 0; i < input.size(); ++i) {
    unsigned char ch = static_cast<unsigned char>(input[i]);
    if (ch == '=') {
      break;
    }
    size_t position = alphabet.find(static_cast<char>(ch));
    if (position == std::string::npos) {
      continue;
    }
    value = (value << 6) + static_cast<unsigned int>(position);
    bits += 6;
    if (bits >= 0) {
      output.push_back(static_cast<char>((value >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return output;
}

void UpdateNarratorDisplayNameFromResponse(HINTERNET request) {
  DWORD headerSize = 0;
  WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM,
                      L"X-Narrator-Display-Name",
                      WINHTTP_NO_OUTPUT_BUFFER, &headerSize,
                      WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || headerSize < sizeof(wchar_t)) {
    return;
  }
  std::vector<wchar_t> header((headerSize / sizeof(wchar_t)) + 1, L'\0');
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM,
                           L"X-Narrator-Display-Name", header.data(),
                           &headerSize, WINHTTP_NO_HEADER_INDEX)) {
    return;
  }
  std::wstring encodedWide(header.data());
  std::string encoded(encodedWide.begin(), encodedWide.end());
  std::string decoded = DecodeBase64(encoded);
  decoded.erase(std::remove_if(decoded.begin(), decoded.end(),
                               [](unsigned char ch) {
                                 return ch < 0x20 || ch == 0x7F;
                               }),
                decoded.end());
  if (decoded.empty() || decoded.size() > 256) {
    return;
  }
  if (decoded != GetNarratorDisplayName()) {
    SetNarratorDisplayName(decoded);
    Log("NARRATOR: display name updated from server.");
  }
}

struct RequestPlan {
  std::wstring method;
  std::wstring path;
  std::string body;
  bool isStub;
  std::string stubResponse;
};

std::string Trim(const std::string &value) {
  if (value.empty())
    return "";
  size_t start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string ToLowerAscii(std::string value) {
  for (size_t i = 0; i < value.length(); ++i) {
    value[i] = static_cast<char>(
        std::tolower(static_cast<unsigned char>(value[i])));
  }
  return value;
}

std::string ToUtf8(const std::wstring &value) {
  if (value.empty())
    return "";
  int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                       (int)value.size(), NULL, 0, NULL, NULL);
  if (sizeNeeded <= 0)
    return "";
  std::string out(sizeNeeded, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), &out[0],
                      sizeNeeded, NULL, NULL);
  return out;
}

std::string ReplaceAll(std::string value, const std::string &from,
                       const std::string &to) {
  if (from.empty())
    return value;
  size_t startPos = 0;
  while ((startPos = value.find(from, startPos)) != std::string::npos) {
    value.replace(startPos, from.length(), to);
    startPos += to.length();
  }
  return value;
}

std::uint64_t HashFnv1a64(const std::string &value) {
  const std::uint64_t kOffset = 1469598103934665603ull;
  const std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffset;
  for (size_t i = 0; i < value.length(); ++i) {
    hash ^= static_cast<std::uint64_t>(
        static_cast<unsigned char>(value[i]));
    hash *= kPrime;
  }
  return hash;
}

std::string HexFromU64(std::uint64_t value) {
  static const char *kHex = "0123456789abcdef";
  std::string hex(16, '0');
  for (int i = 15; i >= 0; --i) {
    hex[static_cast<size_t>(i)] =
        kHex[static_cast<size_t>(value & 0x0full)];
    value >>= 4;
  }
  return hex;
}

std::string Base64Encode(const std::string &input) {
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(((input.size() + 2) / 3) * 4);

  size_t i = 0;
  while (i + 2 < input.size()) {
    unsigned int chunk = (static_cast<unsigned char>(input[i]) << 16) |
                         (static_cast<unsigned char>(input[i + 1]) << 8) |
                         (static_cast<unsigned char>(input[i + 2]));
    output.push_back(table[(chunk >> 18) & 0x3F]);
    output.push_back(table[(chunk >> 12) & 0x3F]);
    output.push_back(table[(chunk >> 6) & 0x3F]);
    output.push_back(table[chunk & 0x3F]);
    i += 3;
  }

  if (i < input.size()) {
    unsigned int chunk = static_cast<unsigned char>(input[i]) << 16;
    output.push_back(table[(chunk >> 18) & 0x3F]);
    if (i + 1 < input.size()) {
      chunk |= static_cast<unsigned char>(input[i + 1]) << 8;
      output.push_back(table[(chunk >> 12) & 0x3F]);
      output.push_back(table[(chunk >> 6) & 0x3F]);
      output.push_back('=');
    } else {
      output.push_back(table[(chunk >> 12) & 0x3F]);
      output.push_back('=');
      output.push_back('=');
    }
  }

  return output;
}

std::string ParseTtsHashToken(const std::string &token) {
  std::string trimmed = Trim(token);
  if (trimmed.find("tts=") != 0) {
    return "";
  }
  std::string hash = Trim(trimmed.substr(4));
  if (hash.length() != 32) {
    return "";
  }
  for (size_t i = 0; i < hash.length(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(hash[i]);
    bool isHex =
        (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
        (ch >= 'A' && ch <= 'F');
    if (!isHex) {
      return "";
    }
  }
  return hash;
}

int ParseTtsDurationToken(const std::string &token) {
  std::string trimmed = Trim(token);
  if (trimmed.find("ttsd=") != 0) {
    return 0;
  }
  std::string durationStr = Trim(trimmed.substr(5));
  if (durationStr.empty()) {
    return 0;
  }
  for (size_t i = 0; i < durationStr.length(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(durationStr[i]);
    if (ch < '0' || ch > '9') {
      return 0;
    }
  }
  int durationMs = atoi(durationStr.c_str());
  if (durationMs <= 0) {
    return 0;
  }
  if (durationMs > 600000) {
    return 600000;
  }
  return durationMs;
}

bool StartsWith(const std::wstring &value, const std::wstring &prefix) {
  if (value.size() < prefix.size())
    return false;
  return value.compare(0, prefix.size(), prefix) == 0;
}

bool IsAsyncPostDedupEndpoint(const std::wstring &endpoint) {
  return endpoint == L"/context" || endpoint == L"/gamedata";
}

DWORD ResolveAsyncPostDedupWindowMs(const std::wstring &endpoint) {
  if (endpoint == L"/gamedata") {
    return 8000;
  }
  if (endpoint == L"/context") {
    return 8000;
  }
  return 0;
}

void EnsureAsyncPostDedupReady() {
  if (InterlockedCompareExchange(&g_asyncPostDedupInitDone, 1, 0) != 0) {
    return;
  }
  InitializeCriticalSection(&g_asyncPostDedupMutex);
}

bool ShouldSuppressDuplicateAsyncPost(const std::wstring &endpoint,
                                      const std::string &jsonData) {
  if (!IsAsyncPostDedupEndpoint(endpoint) || jsonData.empty()) {
    return false;
  }

  const DWORD windowMs = ResolveAsyncPostDedupWindowMs(endpoint);
  if (windowMs == 0) {
    return false;
  }

  EnsureAsyncPostDedupReady();
  if (InterlockedCompareExchange(&g_asyncPostDedupInitDone, 0, 0) == 0) {
    return false;
  }

  const DWORD nowTick = GetTickCount();
  const std::uint64_t payloadHash = HashFnv1a64(jsonData);
  const std::string fingerprint =
      ToUtf8(endpoint) + "|" + HexFromU64(payloadHash);
  bool suppress = false;

  EnterCriticalSection(&g_asyncPostDedupMutex);
  if (g_asyncPostDedupLastPruneTick == 0 ||
      (nowTick - g_asyncPostDedupLastPruneTick) >= kAsyncPostDedupRetentionMs) {
    g_asyncPostDedupLastPruneTick = nowTick;
    for (std::map<std::string, AsyncPostDedupState>::iterator it =
             g_asyncPostDedupByFingerprint.begin();
         it != g_asyncPostDedupByFingerprint.end();) {
      if (it->second.lastSentTick == 0 ||
          (nowTick - it->second.lastSentTick) >= kAsyncPostDedupRetentionMs) {
        it = g_asyncPostDedupByFingerprint.erase(it);
      } else {
        ++it;
      }
    }
  }

  AsyncPostDedupState &state = g_asyncPostDedupByFingerprint[fingerprint];
  if (state.lastSentTick != 0 &&
      static_cast<std::int32_t>(nowTick - state.lastSentTick) <
          static_cast<std::int32_t>(windowMs)) {
    suppress = true;
  } else {
    state.lastSentTick = nowTick;
  }
  LeaveCriticalSection(&g_asyncPostDedupMutex);
  return suppress;
}

bool PathContains(const std::wstring &path, const wchar_t *token) {
  if (!token)
    return false;
  return path.find(token) != std::wstring::npos;
}

std::string SanitizeUploadFilename(const std::string &rawValue) {
  std::string cleaned = rawValue;
  if (cleaned.empty()) {
    cleaned = "import.csv";
  }

  size_t slashPos = cleaned.find_last_of("\\/");
  if (slashPos != std::string::npos) {
    cleaned = cleaned.substr(slashPos + 1);
  }

  if (cleaned.empty()) {
    cleaned = "import.csv";
  }

  for (size_t i = 0; i < cleaned.length(); ++i) {
    unsigned char ch = static_cast<unsigned char>(cleaned[i]);
    bool isSafe =
        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
    if (!isSafe) {
      cleaned[i] = '_';
    }
  }

  if (cleaned.length() > 128) {
    cleaned = cleaned.substr(0, 128);
  }

  if (cleaned.find(".csv") == std::string::npos) {
    cleaned += ".csv";
  }

  return cleaned;
}

std::string ReadHttpResponse(HINTERNET hRequest) {
  std::string responseBody;
  DWORD dwSize = 0;
  do {
    if (!WinHttpQueryDataAvailable(hRequest, &dwSize))
      break;
    if (dwSize == 0)
      break;

    std::vector<char> buffer(dwSize + 1, 0);
    DWORD downloaded = 0;
    if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &downloaded))
      break;

    responseBody.append(buffer.data(), downloaded);
  } while (dwSize > 0);

  return responseBody;
}

bool ReadHttpResponseWithLineCallback(HINTERNET hRequest,
                                      std::string *responseOut,
                                      StobeStreamLineCallback callback,
                                      void *userData) {
  std::string responseBody;
  std::string lineBuffer;
  bool readOk = true;
  bool continueStreaming = true;

  DWORD dwSize = 0;
  do {
    if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
      readOk = false;
      break;
    }
    if (dwSize == 0)
      break;

    std::vector<char> buffer(dwSize + 1, 0);
    DWORD downloaded = 0;
    if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &downloaded)) {
      readOk = false;
      break;
    }

    if (downloaded == 0) {
      continue;
    }

    std::string chunk(buffer.data(), downloaded);
    responseBody.append(chunk);

    if (callback) {
      lineBuffer.append(chunk);
      while (continueStreaming) {
        size_t newlinePos = lineBuffer.find('\n');
        if (newlinePos == std::string::npos) {
          break;
        }
        std::string line = lineBuffer.substr(0, newlinePos);
        if (!line.empty() && line[line.length() - 1] == '\r') {
          line.erase(line.length() - 1);
        }
        lineBuffer.erase(0, newlinePos + 1);
        continueStreaming = callback(line, userData);
      }
      if (!continueStreaming) {
        break;
      }
    }
  } while (dwSize > 0);

  if (callback && continueStreaming && !lineBuffer.empty()) {
    std::string trailing = lineBuffer;
    if (!trailing.empty() && trailing[trailing.length() - 1] == '\r') {
      trailing.erase(trailing.length() - 1);
    }
    continueStreaming = callback(trailing, userData);
  }

  if (responseOut) {
    *responseOut = responseBody;
  }

  return readOk && continueStreaming;
}

bool TryParseHostPort(const std::string &rawValue, std::wstring &host,
                      INTERNET_PORT &port) {
  std::string value = Trim(rawValue);
  if (value.empty())
    return false;

  size_t colonPos = value.rfind(':');
  if (colonPos == std::string::npos || colonPos == 0 ||
      colonPos == value.length() - 1) {
    return false;
  }

  std::string hostPart = Trim(value.substr(0, colonPos));
  std::string portPart = Trim(value.substr(colonPos + 1));
  if (hostPart.empty() || portPart.empty())
    return false;

  int parsedPort = atoi(portPart.c_str());
  if (parsedPort <= 0 || parsedPort > 65535)
    return false;

  host = ToWide(hostPart);
  port = static_cast<INTERNET_PORT>(parsedPort);
  return true;
}

bool IsLoopbackHost(const std::string &rawHost) {
  std::string host = ToLowerAscii(Trim(rawHost));
  return host.empty() || host == "127.0.0.1" || host == "localhost" ||
         host == "::1";
}

bool TryApplyConfiguredServerTarget() {
  std::string configuredHost = Trim(g_serverHost);
  INTERNET_PORT configuredPort =
      (g_serverPort >= 1 && g_serverPort <= 65535)
          ? static_cast<INTERNET_PORT>(g_serverPort)
          : kDefaultServerPort;

  if (!IsLoopbackHost(configuredHost) || configuredPort != kDefaultServerPort) {
    g_stobeHost = ToWide(configuredHost.empty() ? "127.0.0.1" : configuredHost);
    g_stobePort = configuredPort;
    Log("DISCOVERY: Using configured Stobe server " + ToUtf8(g_stobeHost) + ":" +
        ToString((int)g_stobePort) + " from Stobe.ini.");
    return true;
  }

  g_stobeHost = ToWide(configuredHost.empty() ? "127.0.0.1" : configuredHost);
  g_stobePort = configuredPort;
  return false;
}

bool SendRawHttp(const RequestPlan &request, bool expectResponse,
                 std::string *responseOut,
                 StobeStreamLineCallback lineCallback, void *lineUserData) {
  HINTERNET hSession = NULL;
  HINTERNET hConnect = NULL;
  HINTERNET hRequest = NULL;
  BOOL sendOk = FALSE;
  hSession = WinHttpOpen(L"Stobe/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("NETWORK_ERROR: Failed to create WinHTTP session.");
    return false;
  }

  int timeoutMs = expectResponse ? (lineCallback ? 120000 : 60000) : 8000;
  if (PathContains(request.path, L"/autonomy_")) {
    timeoutMs = 5000;
  }
  WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

  hConnect = WinHttpConnect(hSession, g_stobeHost.c_str(), g_stobePort, 0);
  if (!hConnect) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("NETWORK_ERROR: Failed to connect to resolved Stobe server.");
    WinHttpCloseHandle(hSession);
    return false;
  }

  hRequest = WinHttpOpenRequest(hConnect, request.method.c_str(),
                                request.path.c_str(), NULL,
                                WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, 0);

  if (!hRequest) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("NETWORK_ERROR: Failed to create HTTP request handle.");
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  if (request.method == L"POST") {
    sendOk = WinHttpSendRequest(
        hRequest, L"Content-Type: application/json\r\n", (DWORD)-1L,
        (LPVOID)request.body.c_str(), (DWORD)request.body.length(),
        (DWORD)request.body.length(), 0);
  } else {
    sendOk = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
  }

  bool success = false;
  if (sendOk) {
    if (WinHttpReceiveResponse(hRequest, NULL)) {
      UpdateNarratorDisplayNameFromResponse(hRequest);
      if (expectResponse) {
        bool readResult = true;
        if (lineCallback) {
          readResult =
              ReadHttpResponseWithLineCallback(hRequest, responseOut,
                                               lineCallback, lineUserData);
        } else if (responseOut) {
          *responseOut = ReadHttpResponse(hRequest);
        } else {
          std::string ignoredBody = ReadHttpResponse(hRequest);
        }
        if (!readResult) {
          success = false;
        } else {
          success = true;
        }
      } else {
        success = true;
      }
    }
  }

  if (!success) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("NETWORK_ERROR: HTTP request failed with error " +
        ToString((int)GetLastError()));
  } else {
    g_dwemerDistroLastSuccessTick = GetTickCount();
    InterlockedExchange(&g_dwemerDistroConnected, 1);
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return success;
}

void DiscoverStobeServer() {
  if (TryApplyConfiguredServerTarget()) {
    return;
  }

  RequestPlan discoverRequest;
  discoverRequest.method = L"GET";
  discoverRequest.path = L"/discover?game=kenshi";
  discoverRequest.body = "";
  discoverRequest.isStub = false;

  HINTERNET hSession = WinHttpOpen(L"Stobe/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    Log("DISCOVERY: WinHTTP session failed, using fallback 127.0.0.1:8083.");
    return;
  }

  WinHttpSetTimeouts(hSession, 3000, 3000, 3000, 3000);
  HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", kDiscoveryPort, 0);
  if (!hConnect) {
    Log("DISCOVERY: Could not connect to launcher discovery, using fallback.");
    WinHttpCloseHandle(hSession);
    return;
  }

  HINTERNET hRequest = WinHttpOpenRequest(
      hConnect, L"GET", discoverRequest.path.c_str(), NULL, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  if (!hRequest) {
    Log("DISCOVERY: Could not create discovery request, using fallback.");
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return;
  }

  bool ok = false;
  std::string response;
  if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(hRequest, NULL)) {
    response = ReadHttpResponse(hRequest);
    ok = true;
  }

  if (ok) {
    std::wstring discoveredHost;
    INTERNET_PORT discoveredPort = kDefaultServerPort;
    if (TryParseHostPort(response, discoveredHost, discoveredPort)) {
      g_stobeHost = discoveredHost;
      g_stobePort = discoveredPort;
      std::string hostNarrow = ToUtf8(g_stobeHost);
      Log("DISCOVERY: Resolved Stobe server to " + hostNarrow + ":" +
          ToString((int)g_stobePort));
    } else {
      Log("DISCOVERY: Invalid response '" + Trim(response) +
          "', using fallback 127.0.0.1:8083.");
    }
  } else {
    Log("DISCOVERY: Request failed, using fallback 127.0.0.1:8083.");
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
}

void EnsureDiscovered() {
  if (InterlockedCompareExchange(&g_discoveryDone, 1, 0) == 0) {
    DiscoverStobeServer();
  }
}

RequestPlan ResolveRequest(const std::wstring &endpoint,
                           const std::string &jsonData, bool withResponse) {
  RequestPlan request;
  request.method = L"POST";
  request.path = endpoint;
  request.body = jsonData;
  request.isStub = false;
  request.stubResponse = "{\"ok\":false,\"error\":\"unsupported_endpoint\"}";

  if (endpoint == L"/chat") {
    request.path = L"/StobeServer/chat.php";
    return request;
  }

  if (endpoint == L"/bored") {
    request.method = L"GET";
    request.path = L"/StobeServer/stream.php?DATA=" +
                   ToWide(BuildStreamQueryData("bored", jsonData)) +
                   L"&tts_enabled=" + (g_ttsEnabled ? L"1" : L"0");
    request.body.clear();
    return request;
  }

  if (endpoint == L"/context") {
    // Prefer direct structured snapshot ingest when JSON context is provided.
    if (!jsonData.empty() && jsonData[0] == '{') {
      request.method = L"POST";
      request.path = L"/StobeServer/npc_snapshot.php";
      request.body = jsonData;
      Log("CONTEXT_ROUTE: JSON context -> npc_snapshot.php (len " +
          ToString((int)jsonData.size()) + ")");
    } else {
      // Legacy fallback: context marker eventlog entry.
      request.method = L"GET";
      request.path = L"/StobeServer/stream.php?DATA=" +
                     ToWide(BuildStreamQueryData("context", "context_update"));
      request.body.clear();
      std::string firstByte = jsonData.empty() ? "<empty>" : jsonData.substr(0, 1);
      Log("CONTEXT_ROUTE: legacy stream fallback (len " +
          ToString((int)jsonData.size()) + ", first_byte=" + firstByte + ")");
    }
    return request;
  }

  if (endpoint == L"/rename") {
    request.path = L"/StobeServer/rename.php";
    return request;
  }

  if (endpoint == L"/get_batch_identities") {
    request.path = L"/StobeServer/get_batch_identities.php";
    return request;
  }

  if (endpoint == L"/gamedata") {
    request.path = L"/StobeServer/gamedata.php";
    return request;
  }

  if (endpoint == L"/autonomy_state") {
    request.method = L"GET";
    request.path = L"/StobeServer/autonomy_state.php";
    request.body.clear();
    return request;
  }

  if (endpoint == L"/autonomy_observation") {
    request.path = L"/StobeServer/autonomy_observation.php";
    return request;
  }

  if (endpoint == L"/autonomy_tick") {
    request.path = L"/StobeServer/autonomy_tick.php";
    return request;
  }

  if (endpoint == L"/portrait_upload") {
    request.path = L"/StobeServer/portrait_upload.php";
    return request;
  }

  if (endpoint == L"/item_image_upload") {
    request.path = L"/StobeServer/item_image_upload.php";
    return request;
  }

  if (endpoint == L"/world_state") {
    request.path = L"/StobeServer/world_state.php";
    return request;
  }

  if (endpoint == L"/faction_relations") {
    request.path = L"/StobeServer/faction_relations.php";
    return request;
  }

  if (endpoint == L"/town_knowledge") {
    request.path = L"/StobeServer/town_knowledge.php";
    return request;
  }

  if (endpoint == L"/player_base_state") {
    request.path = L"/StobeServer/player_base_state.php";
    return request;
  }

  if (endpoint == L"/conf_opts") {
    request.path = L"/StobeServer/conf_opts.php";
    return request;
  }

  if (endpoint.find(L"/conf_opts?") == 0) {
    request.method = L"GET";
    request.path = L"/StobeServer/conf_opts.php" +
                   endpoint.substr(std::wstring(L"/conf_opts").length());
    request.body.clear();
    return request;
  }

  if (endpoint == L"/speech_delivery") {
    request.path = L"/StobeServer/speech_delivery.php";
    return request;
  }

  if (endpoint == L"/ai_npcs/list" || endpoint == L"/ai_npcs/detail") {
    request.path = L"/StobeServer/ai_npcs.php";
    return request;
  }

  if (endpoint == L"/ai_diaries/list" || endpoint == L"/ai_diaries/detail" ||
      endpoint == L"/ai_diaries/entries" || endpoint == L"/ai_diaries/entry") {
    request.path = L"/StobeServer/ai_diaries.php";
    return request;
  }

  if (endpoint == L"/ai_history") {
    request.path = L"/StobeServer/ai_history.php";
    return request;
  }

  if (endpoint == L"/settings" || endpoint == L"/test_llm" ||
      endpoint == L"/favorite" || endpoint == L"/characters" ||
      endpoint == L"/history" || endpoint == L"/synthesize" ||
      endpoint == L"/events/content" || endpoint == L"/events" ||
      endpoint == L"/campaigns/create" || endpoint == L"/campaigns/switch" ||
      endpoint == L"/campaigns/cull" || endpoint == L"/player_profile") {
    request.isStub = true;
    request.stubResponse = "{}";
    return request;
  }

  if (StartsWith(endpoint, L"/StobeServer/")) {
    if (jsonData.empty()) {
      request.method = L"GET";
      request.body.clear();
    }
    return request;
  }

  request.isStub = true;
  if (!withResponse) {
    request.stubResponse = "";
  }
  return request;
}

bool PopSerialHttpTask(SerialHttpTask &taskOut) {
  bool hasTask = false;
  EnterCriticalSection(&g_serialHttpQueueMutex);
  if (!g_serialHttpQueue.empty()) {
    taskOut = g_serialHttpQueue.front();
    g_serialHttpQueue.pop_front();
    hasTask = true;
  }
  LeaveCriticalSection(&g_serialHttpQueueMutex);
  return hasTask;
}

DWORD WINAPI SerialHttpWorkerThread(LPVOID) {
  while (true) {
    if (!g_serialHttpQueueSignal) {
      Sleep(100);
      continue;
    }

    WaitForSingleObject(g_serialHttpQueueSignal, INFINITE);

    SerialHttpTask task;
    while (PopSerialHttpTask(task)) {
      PostToStobe(task.endpoint, task.data);
    }
  }
  return 0;
}

void EnsureSerialHttpWorkerStarted() {
  if (InterlockedCompareExchange(&g_serialHttpInitDone, 1, 0) != 0) {
    return;
  }

  InitializeCriticalSection(&g_serialHttpQueueMutex);

  g_serialHttpQueueSignal = CreateEventA(NULL, FALSE, FALSE, NULL);
  if (!g_serialHttpQueueSignal) {
    DeleteCriticalSection(&g_serialHttpQueueMutex);
    InterlockedExchange(&g_serialHttpInitDone, 0);
    Log("SERIAL_HTTP: failed to create queue signal event");
    return;
  }

  g_serialHttpThread = CreateThread(NULL, 0, SerialHttpWorkerThread, NULL, 0, NULL);
  if (!g_serialHttpThread) {
    CloseHandle(g_serialHttpQueueSignal);
    g_serialHttpQueueSignal = NULL;
    DeleteCriticalSection(&g_serialHttpQueueMutex);
    InterlockedExchange(&g_serialHttpInitDone, 0);
    Log("SERIAL_HTTP: failed to create worker thread");
    return;
  }
}

void EnqueueSerialHttpPost(const std::wstring &endpoint,
                           const std::string &jsonData) {
  EnsureSerialHttpWorkerStarted();

  if (!g_serialHttpQueueSignal ||
      InterlockedCompareExchange(&g_serialHttpInitDone, 0, 0) == 0) {
    // Fallback should be exceptionally rare.
    AsyncPostToStobe(endpoint, jsonData);
    return;
  }

  DWORD nowTick = GetTickCount();
  bool dropped = false;
  bool shouldLogDrop = false;
  size_t queueDepth = 0;

  EnterCriticalSection(&g_serialHttpQueueMutex);
  if (g_serialHttpQueue.size() >= kSerialHttpQueueCap) {
    g_serialHttpQueue.pop_front();
    dropped = true;
    InterlockedIncrement(&g_serialHttpDropped);
  }
  SerialHttpTask task;
  task.endpoint = endpoint;
  task.data = jsonData;
  g_serialHttpQueue.push_back(task);
  queueDepth = g_serialHttpQueue.size();
  if (dropped &&
      (nowTick - (DWORD)g_serialHttpLastDropLogTick) >=
          kSerialHttpDropLogCooldownMs) {
    g_serialHttpLastDropLogTick = (LONG)nowTick;
    shouldLogDrop = true;
  }
  LeaveCriticalSection(&g_serialHttpQueueMutex);

  if (shouldLogDrop) {
    LONG totalDropped = InterlockedCompareExchange(&g_serialHttpDropped, 0, 0);
    Log("SERIAL_HTTP: queue overflow; dropped oldest event posts total_dropped=" +
        ToString((int)totalDropped) + " queue_depth=" + ToString((int)queueDepth));
  }

  SetEvent(g_serialHttpQueueSignal);
}
} // namespace

std::wstring ToWide(const std::string &value) {
  return std::wstring(value.begin(), value.end());
}

std::string UrlEncode(const std::string &input) {
  static const char *hex = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(input.size() * 3);
  for (size_t ci = 0; ci < input.size(); ++ci) {
    unsigned char c = static_cast<unsigned char>(input[ci]);
    bool isSafe =
        ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
         c == '~');
    if (isSafe) {
      encoded.push_back(static_cast<char>(c));
      continue;
    }

    encoded.push_back('%');
    encoded.push_back(hex[(c >> 4) & 0x0F]);
    encoded.push_back(hex[c & 0x0F]);
  }
  return encoded;
}

std::string BuildStreamQueryData(const std::string &eventType,
                                 const std::string &eventData,
                                 int gameTs) {
  if (gameTs < 0) {
    gameTs = 0;
  }
  std::string packet = eventType + "|" + ToString((int)time(NULL)) + "|" +
                       ToString(gameTs) + "|" + eventData;
  return UrlEncode(Base64Encode(packet));
}

void PostToStobe(const std::wstring &endpoint, const std::string &jsonData) {
  EnsureDiscovered();
  RequestPlan request = ResolveRequest(endpoint, jsonData, false);
  if (request.isStub) {
    std::string endpointNarrow = ToUtf8(endpoint);
    Log("NETWORK: Skipping unsupported async endpoint " + endpointNarrow);
    return;
  }

  std::string ignoredResponse;
  SendRawHttp(request, false, &ignoredResponse, NULL, NULL);
}

std::string PostToStobeWithResponse(const std::wstring &endpoint,
                                    const std::string &jsonData) {
  EnsureDiscovered();
  RequestPlan request = ResolveRequest(endpoint, jsonData, true);
  if (request.isStub) {
    std::string endpointNarrow = ToUtf8(endpoint);
    Log("NETWORK: Returning stub for endpoint " + endpointNarrow);
    return request.stubResponse;
  }

  std::string responseBody;
  if (!SendRawHttp(request, true, &responseBody, NULL, NULL)) {
    return request.stubResponse;
  }
  return responseBody;
}

bool PostToStobeWithResponseStream(const std::wstring &endpoint,
                                   const std::string &jsonData,
                                   StobeStreamLineCallback callback,
                                   void *userData) {
  EnsureDiscovered();
  RequestPlan request = ResolveRequest(endpoint, jsonData, true);
  if (request.isStub) {
    std::string endpointNarrow = ToUtf8(endpoint);
    Log("NETWORK: Stream callback using stub for endpoint " + endpointNarrow);
    if (callback && !request.stubResponse.empty()) {
      callback(request.stubResponse, userData);
    }
    return true;
  }

  std::string ignoredBody;
  return SendRawHttp(request, true, &ignoredBody, callback, userData);
}

void PostSpeechDeliveryState(const std::string &utteranceId,
                             const std::string &deliveryState) {
  std::vector<std::string> utteranceIds;
  if (!utteranceId.empty()) {
    utteranceIds.push_back(utteranceId);
  }
  PostSpeechDeliveryStates(utteranceIds, deliveryState);
}

void PostSpeechDeliveryStates(const std::vector<std::string> &utteranceIds,
                              const std::string &deliveryState) {
  std::string normalizedState = Trim(deliveryState);
  std::transform(normalizedState.begin(), normalizedState.end(),
                 normalizedState.begin(), ::tolower);
  if (normalizedState != "spoken" && normalizedState != "cancelled") {
    return;
  }

  std::set<std::string> uniqueIds;
  for (size_t i = 0; i < utteranceIds.size(); ++i) {
    std::string candidate = Trim(utteranceIds[i]);
    if (!candidate.empty()) {
      uniqueIds.insert(candidate);
    }
  }
  if (uniqueIds.empty()) {
    return;
  }

  std::string payload = "{\"updates\":[";
  bool first = true;
  for (std::set<std::string>::const_iterator it = uniqueIds.begin();
       it != uniqueIds.end(); ++it) {
    if (!first) {
      payload += ",";
    }
    first = false;
    payload += "{\"utterance_id\":\"" + EscapeJSON(*it) +
               "\",\"delivery_state\":\"" + EscapeJSON(normalizedState) + "\"}";
  }
  payload += "]}";

  AsyncPostToStobe(L"/speech_delivery", payload);
}

std::string UploadCsvImportToStobe(const std::string &csvData,
                                   const std::string &filename,
                                   const std::string &importType) {
  EnsureDiscovered();

  if (csvData.empty()) {
    Log("CSV_IMPORT: skipped empty payload for file=" + filename);
    return "";
  }
  if (importType.empty()) {
    Log("CSV_IMPORT: missing import type for file=" + filename);
    return "";
  }

  std::string safeFilename = SanitizeUploadFilename(filename);
  std::string safeType = importType;
  for (size_t i = 0; i < safeType.length(); ++i) {
    unsigned char ch = static_cast<unsigned char>(safeType[i]);
    bool isSafe =
        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    if (!isSafe) {
      safeType[i] = '_';
    }
  }
  if (safeType.empty()) {
    return "";
  }

  const std::string boundary = "----StobeCsvBoundary7MA4YWxkTrZu0gW";
  const std::string partHeader =
      "--" + boundary +
      "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" +
      safeFilename + "\"\r\nContent-Type: text/csv\r\n\r\n";
  const std::string partFooter = "\r\n--" + boundary + "--\r\n";

  const size_t totalSize = partHeader.size() + csvData.size() + partFooter.size();
  if (totalSize > 0xFFFFFFFFu) {
    Log("CSV_IMPORT: payload too large for WinHTTP file=" + safeFilename);
    return "";
  }

  std::string ts = ToString(static_cast<int>(time(NULL)));
  std::string requestPath = "/StobeServer/csv_import.php?type=" +
                            UrlEncode(safeType) + "&filename=" +
                            UrlEncode(safeFilename) + "&ts=" + UrlEncode(ts);
  std::wstring widePath = ToWide(requestPath);

  std::wstring headerWide =
      ToWide("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");

  HINTERNET hSession = WinHttpOpen(L"Stobe/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("CSV_IMPORT: WinHttpOpen failed for file=" + safeFilename);
    return "";
  }
  WinHttpSetTimeouts(hSession, 15000, 15000, 45000, 45000);

  HINTERNET hConnect =
      WinHttpConnect(hSession, g_stobeHost.c_str(), g_stobePort, 0);
  if (!hConnect) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("CSV_IMPORT: WinHttpConnect failed for file=" + safeFilename);
    WinHttpCloseHandle(hSession);
    return "";
  }

  HINTERNET hRequest =
      WinHttpOpenRequest(hConnect, L"POST", widePath.c_str(), NULL,
                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  if (!hRequest) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("CSV_IMPORT: WinHttpOpenRequest failed for file=" + safeFilename);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return "";
  }

  if (!WinHttpAddRequestHeaders(hRequest, headerWide.c_str(), -1L,
                                WINHTTP_ADDREQ_FLAG_ADD |
                                    WINHTTP_ADDREQ_FLAG_REPLACE)) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("CSV_IMPORT: failed to set multipart header for file=" + safeFilename);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return "";
  }

  DWORD totalSizeDword = static_cast<DWORD>(totalSize);
  if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, totalSizeDword, 0)) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("CSV_IMPORT: WinHttpSendRequest failed file=" + safeFilename +
        " err=" + ToString((int)GetLastError()));
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return "";
  }

  DWORD bytesWritten = 0;
  if (!WinHttpWriteData(hRequest, partHeader.data(),
                        static_cast<DWORD>(partHeader.size()), &bytesWritten) ||
      bytesWritten != partHeader.size()) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("CSV_IMPORT: failed writing multipart header file=" + safeFilename);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return "";
  }

  bytesWritten = 0;
  if (!WinHttpWriteData(hRequest, csvData.data(), static_cast<DWORD>(csvData.size()),
                        &bytesWritten) ||
      bytesWritten != csvData.size()) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("CSV_IMPORT: failed writing CSV body file=" + safeFilename);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return "";
  }

  bytesWritten = 0;
  if (!WinHttpWriteData(hRequest, partFooter.data(),
                        static_cast<DWORD>(partFooter.size()), &bytesWritten) ||
      bytesWritten != partFooter.size()) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("CSV_IMPORT: failed writing multipart footer file=" + safeFilename);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return "";
  }

  if (!WinHttpReceiveResponse(hRequest, NULL)) {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("CSV_IMPORT: WinHttpReceiveResponse failed file=" + safeFilename +
        " err=" + ToString((int)GetLastError()));
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return "";
  }

  DWORD statusCode = 0;
  DWORD statusSize = sizeof(statusCode);
  if (!WinHttpQueryHeaders(
          hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
          WINHTTP_NO_HEADER_INDEX)) {
    statusCode = 0;
  }

  std::string response = ReadHttpResponse(hRequest);
  if (statusCode >= 200 && statusCode < 300) {
    g_dwemerDistroLastSuccessTick = GetTickCount();
    InterlockedExchange(&g_dwemerDistroConnected, 1);
  } else {
    InterlockedExchange(&g_dwemerDistroConnected, 0);
    Log("CSV_IMPORT: server returned status=" + ToString((int)statusCode) +
        " file=" + safeFilename);
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return response;
}

struct HttpTask {
  std::wstring endpoint;
  std::string data;
};

DWORD WINAPI AsyncHttpThread(LPVOID lpParam) {
  HttpTask *task = (HttpTask *)lpParam;
  PostToStobe(task->endpoint, task->data);
  delete task;
  return 0;
}

void AsyncPostToStobe(const std::wstring &endpoint,
                      const std::string &jsonData) {
  if (ShouldSuppressDuplicateAsyncPost(endpoint, jsonData)) {
    return;
  }
  HttpTask *task = new HttpTask();
  task->endpoint = endpoint;
  task->data = jsonData;
  CreateThread(NULL, 0, AsyncHttpThread, task, 0, NULL);
}

void AsyncPostToStobeSerial(const std::wstring &endpoint,
                            const std::string &jsonData) {
  EnqueueSerialHttpPost(endpoint, jsonData);
}

DWORD WINAPI BoredEventPollThread(LPVOID lpParam) {
  std::string *pJson = (std::string *)lpParam;
  Log("BORED_NET: Sending request to server...");
  std::string response = PostToStobeWithResponse(L"/bored", *pJson);
  delete pJson;

  if (response.empty()) {
    Log("BORED_NET: Empty response or timeout from server.");
    return 0;
  }

  // Ensure we don't spam if server is slow
  g_lastBoredEventTick = GetTickCount();

  std::string content = JsonReadField(response, "text");
  if (content.empty()) {
    content = response;
  }

  std::stringstream ss(content);
  std::string line;
  bool first = true;
  int lineCount = 0;
  while (std::getline(ss, line)) {
    line = Trim(line);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
      line = Trim(line);
    }
    if (line.empty())
      continue;

    std::string queueMessage;
    std::string ttsHash = "";
    int ttsDurationMs = 0;
    size_t bar1 = line.find('|');
    size_t bar2 = (bar1 == std::string::npos) ? std::string::npos
                                               : line.find('|', bar1 + 1);
    size_t bar3 = (bar2 == std::string::npos) ? std::string::npos
                                               : line.find('|', bar2 + 1);
    if (bar1 != std::string::npos && bar2 != std::string::npos) {
      std::string actor = Trim(line.substr(0, bar1));
      std::string subtitle = "";
      std::string payload = line.substr(bar2 + 1);
      size_t metaSep = payload.find('|');
      if (metaSep == std::string::npos) {
        subtitle = Trim(payload);
      } else {
        subtitle = Trim(payload.substr(0, metaSep));
        std::string metadata = payload.substr(metaSep + 1);
        size_t tokenStart = 0;
        while (tokenStart <= metadata.length()) {
          size_t tokenEnd = metadata.find('|', tokenStart);
          std::string token =
              (tokenEnd == std::string::npos)
                  ? metadata.substr(tokenStart)
                  : metadata.substr(tokenStart, tokenEnd - tokenStart);
          std::string parsedHash = ParseTtsHashToken(token);
          if (!parsedHash.empty()) {
            ttsHash = parsedHash;
          } else {
            int parsedDuration = ParseTtsDurationToken(token);
            if (parsedDuration > 0) {
              ttsDurationMs = parsedDuration;
            }
          }
          if (tokenEnd == std::string::npos) {
            break;
          }
          tokenStart = tokenEnd + 1;
        }
      }
      size_t slashPos = subtitle.find('/');
      if (slashPos != std::string::npos) {
        subtitle = Trim(subtitle.substr(0, slashPos));
      }

      if (actor.empty() || subtitle.empty())
        continue;

      queueMessage = "NPC_SAY: " + actor + ": " + subtitle;
      if (g_ttsEnabled && !ttsHash.empty()) {
        queueMessage += " [TTSHASH:" + ttsHash + "]";
      }
      if (g_ttsEnabled && ttsDurationMs > 0) {
        queueMessage += " [TTSDUR:" + ToString(ttsDurationMs) + "]";
      }
    } else {
      queueMessage = "NPC_SAY: " + line;
    }

    if (!first) {
      SleepIfPaused(g_dialogueSpeedSeconds * 1000);
    }

    EnterCriticalSection(&g_msgMutex);
    g_messageQueue.push_back(queueMessage);
    g_lastDialogueTick = GetTickCount();
    LeaveCriticalSection(&g_msgMutex);
    first = false;
    lineCount++;
  }

  if (lineCount > 0) {
    Log("BORED_POLL: Queued " + ToString(lineCount) + " banter lines.");
  } else {
    Log("BORED_NET: No parsable lines in bored response.");
  }
  return 0;
}

bool IsDwemerDistroConnected() {
  return InterlockedCompareExchange(&g_dwemerDistroConnected, 0, 0) != 0;
}

DWORD GetDwemerDistroLastSuccessAgeMs() {
  DWORD lastTick = g_dwemerDistroLastSuccessTick;
  if (lastTick == 0) {
    return 0xFFFFFFFF;
  }
  DWORD now = GetTickCount();
  if (now >= lastTick) {
    return now - lastTick;
  }
  return 0;
}

std::string GetStobeServerHomeUrl() {
  EnsureDiscovered();
  std::string host = ToUtf8(g_stobeHost);
  if (host.empty()) {
    host = "127.0.0.1";
  }
  return "http://" + host + ":" + ToString((int)g_stobePort) + "/StobeServer/";
}
