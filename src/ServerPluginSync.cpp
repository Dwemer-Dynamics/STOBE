#include "ServerPluginSync.h"

#include <kenshi/GameWorld.h>
#include <kenshi/ModInfo.h>

#include "AddonProtocol.h"
#include "Comm.h"
#include "Interaction.h"
#include "Utils.h"

#include <winhttp.h>

#include <map>
#include <string>
#include <vector>

namespace Stobe {
namespace ServerPluginSync {
namespace {

using Stobe::AddonProtocol::JsonBooleanValue;
using Stobe::AddonProtocol::JsonEscape;
using Stobe::AddonProtocol::JsonStringValue;

const unsigned __int64 kMaximumPackageBytes = 512ULL * 1024ULL * 1024ULL;
const DWORD kUploadChunkBytes = 1024 * 1024; // Server accepts up to 1.5 MiB.
const size_t kMaximumPackages = 32;
const size_t kMaximumEntriesPerFolder = 256;
const int kStatusPolls = 30;
const DWORD kStatusPollDelayMs = 2000;
const DWORD kRetryDelayMs = 30000;
const char kApiPath[] = "/StobeServer/ui/api/plugin_packages.php?action=";

volatile LONG g_started = 0;

struct ActiveMod {
  std::string name;
  std::string path;
  std::string file;
};

struct Package {
  std::string name;
  std::string version;
  std::string archivePath;
  std::string archiveName;
  std::string modName;
  unsigned __int64 size;
};

struct HttpResponse {
  DWORD status;
  std::string body;
  HttpResponse() : status(0) {}
  bool Ok() const { return status >= 200 && status < 300; }
};

enum SyncOutcome { SYNC_COMPLETE, SYNC_RETRY, SYNC_STOP };

bool IsDirectory(const std::string &path) {
  const DWORD attributes = GetFileAttributesA(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string TrimSlashes(std::string value) {
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '/') {
      value[i] = '\\';
    }
  }
  while (value.size() > 1 && value[value.size() - 1] == '\\') {
    value.erase(value.size() - 1);
  }
  return value;
}

// ModInfo::path is the mod folder; fall back to the .mod file's folder.
std::string ResolveModDirectory(const ActiveMod &mod) {
  const std::string path = TrimSlashes(mod.path);
  if (!path.empty() && IsDirectory(path)) {
    return path;
  }
  const std::string file = TrimSlashes(mod.file);
  const size_t slash = file.rfind('\\');
  if (slash != std::string::npos && slash > 0) {
    const std::string folder = file.substr(0, slash);
    if (IsDirectory(folder)) {
      return folder;
    }
  }
  return "";
}

bool IsNewer(const FILETIME &left, const FILETIME &right) {
  return CompareFileTime(&left, &right) > 0;
}

void ScanModFolder(const ActiveMod &mod, std::map<std::string, Package> &found) {
  const std::string modDirectory = ResolveModDirectory(mod);
  if (modDirectory.empty()) {
    return;
  }
  const std::string root = modDirectory + "\\Stobe\\server-plugins";
  if (!IsDirectory(root)) {
    return;
  }

  WIN32_FIND_DATAA folder;
  HANDLE folders = FindFirstFileA((root + "\\*").c_str(), &folder);
  if (folders == INVALID_HANDLE_VALUE) {
    return;
  }
  size_t folderCount = 0;
  do {
    const std::string name = folder.cFileName;
    if ((folder.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        name == "." || name == "..") {
      continue;
    }
    if (++folderCount > kMaximumEntriesPerFolder) {
      Log("SERVER_PLUGIN_SYNC: too many package folders in mod " + mod.name);
      break;
    }
    if (!Stobe::AddonProtocol::IsSafePackageName(name)) {
      Log("SERVER_PLUGIN_SYNC: ignoring unsafe package folder name in mod " +
          mod.name);
      continue;
    }

    const std::string packageFolder = root + "\\" + name;
    WIN32_FIND_DATAA file;
    HANDLE files = FindFirstFileA((packageFolder + "\\*").c_str(), &file);
    if (files == INVALID_HANDLE_VALUE) {
      continue;
    }
    WIN32_FIND_DATAA newest;
    bool haveNewest = false;
    int archiveCount = 0;
    size_t fileCount = 0;
    do {
      std::string stem;
      if ((file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
          !Stobe::AddonProtocol::SplitPackageArchiveName(file.cFileName, stem)) {
        continue;
      }
      if (++fileCount > kMaximumEntriesPerFolder) {
        break;
      }
      ++archiveCount;
      if (!haveNewest || IsNewer(file.ftLastWriteTime, newest.ftLastWriteTime)) {
        newest = file;
        haveNewest = true;
      }
    } while (FindNextFileA(files, &file));
    FindClose(files);
    if (!haveNewest) {
      continue;
    }
    if (archiveCount > 1) {
      Log("SERVER_PLUGIN_SYNC: " + name +
          " has multiple packages; using newest file " + newest.cFileName);
    }

    Package package;
    package.name = name;
    package.archiveName = newest.cFileName;
    Stobe::AddonProtocol::SplitPackageArchiveName(package.archiveName,
                                                  package.version);
    package.archivePath = packageFolder + "\\" + package.archiveName;
    package.modName = mod.name;
    package.size = (static_cast<unsigned __int64>(newest.nFileSizeHigh) << 32) |
                   newest.nFileSizeLow;
    if (!Stobe::AddonProtocol::IsSafePackageVersion(package.version)) {
      Log("SERVER_PLUGIN_SYNC: ignoring " + name +
          " because its file name is not a valid version: " +
          package.archiveName);
      continue;
    }
    if (package.size == 0 || package.size > kMaximumPackageBytes) {
      Log("SERVER_PLUGIN_SYNC: ignoring " + name + " " + package.version +
          " because its size is invalid");
      continue;
    }

    const std::string key = Stobe::AddonProtocol::LowerAscii(name);
    std::map<std::string, Package>::iterator existing = found.find(key);
    if (existing != found.end()) {
      // Active mods are in load order; the later mod wins, as in Kenshi.
      Log("SERVER_PLUGIN_SYNC: package " + name + " from mod " + mod.name +
          " overrides mod " + existing->second.modName);
    } else if (found.size() >= kMaximumPackages) {
      Log("SERVER_PLUGIN_SYNC: package limit reached; ignoring " + name);
      continue;
    }
    found[key] = package;
  } while (FindNextFileA(folders, &folder));
  FindClose(folders);
}

HttpResponse Request(const std::wstring &host, INTERNET_PORT port,
                     const wchar_t *method, const std::string &action,
                     const char *contentType, const char *data, DWORD size) {
  HttpResponse response;
  HINTERNET session =
      WinHttpOpen(L"Stobe Server Plugin Sync/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    return response;
  }
  WinHttpSetTimeouts(session, 5000, 5000, 30000, 30000);
  HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
  HINTERNET request = NULL;
  if (connection) {
    const std::wstring path = ToWide(std::string(kApiPath) + action);
    request = WinHttpOpenRequest(connection, method, path.c_str(), NULL,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  }
  if (request) {
    std::wstring headers = L"Accept: application/json\r\n";
    if (contentType) {
      headers += L"Content-Type: " + ToWide(contentType) + L"\r\n";
    }
    const BOOL sent = WinHttpSendRequest(
        request, headers.c_str(), static_cast<DWORD>(-1L),
        size > 0 ? const_cast<char *>(data) : WINHTTP_NO_REQUEST_DATA, size,
        size, 0);
    if (sent && WinHttpReceiveResponse(request, NULL)) {
      DWORD statusSize = sizeof(response.status);
      WinHttpQueryHeaders(request,
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &response.status,
                          &statusSize, WINHTTP_NO_HEADER_INDEX);
      DWORD available = 0;
      while (WinHttpQueryDataAvailable(request, &available) && available > 0 &&
             response.body.size() < 65536) {
        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, &buffer[0], available, &read) ||
            read == 0) {
          break;
        }
        response.body.append(&buffer[0], read);
      }
    }
    WinHttpCloseHandle(request);
  }
  if (connection) {
    WinHttpCloseHandle(connection);
  }
  WinHttpCloseHandle(session);
  return response;
}

std::string ServerError(const HttpResponse &response) {
  const std::string error = JsonStringValue(response.body, "error");
  if (!error.empty()) {
    return error.size() > 200 ? error.substr(0, 200) : error;
  }
  return response.status == 0 ? "server unreachable"
                              : "HTTP " + ToString(static_cast<int>(response.status));
}

bool Succeeded(const HttpResponse &response) {
  return response.Ok() && JsonBooleanValue(response.body, "ok", false);
}

// Missing endpoint or a server that predates the package API.
bool IsUnsupportedEndpoint(const HttpResponse &response) {
  return response.status == 404 || response.status == 405 ||
         (response.Ok() && response.body.find("\"ok\"") == std::string::npos);
}

SyncOutcome WaitForJob(const std::wstring &host, INTERNET_PORT port,
                       const Package &package, const std::string &jobId) {
  for (int poll = 0; poll < kStatusPolls; ++poll) {
    Sleep(kStatusPollDelayMs);
    const HttpResponse status =
        Request(host, port, L"GET", "status&job_id=" + UrlEncode(jobId), NULL,
                NULL, 0);
    if (!Succeeded(status)) {
      Log("SERVER_PLUGIN_SYNC: status check for " + package.name +
          " failed: " + ServerError(status));
      return status.status == 0 ? SYNC_RETRY : SYNC_STOP;
    }
    const std::string state = JsonStringValue(status.body, "status");
    if (state == "completed") {
      Log("SERVER_PLUGIN_SYNC: installed " + package.name + " " +
          package.version);
      return SYNC_COMPLETE;
    }
    if (state == "failed") {
      Log("SERVER_PLUGIN_SYNC: " + package.name + " " + package.version +
          " activation failed: " + ServerError(status));
      return SYNC_STOP;
    }
  }
  Log("SERVER_PLUGIN_SYNC: " + package.name + " " + package.version +
      " is still activating; check the server's plugin page");
  return SYNC_STOP;
}

SyncOutcome UploadPackage(const std::wstring &host, INTERNET_PORT port,
                          const Package &package, bool &endpointMissing) {
  const std::string probeBody = "{\"name\":\"" + JsonEscape(package.name) +
                                "\",\"version\":\"" +
                                JsonEscape(package.version) + "\"}";
  const HttpResponse probe =
      Request(host, port, L"POST", "probe", "application/json",
              probeBody.data(), static_cast<DWORD>(probeBody.size()));
  if (IsUnsupportedEndpoint(probe)) {
    endpointMissing = true;
    return SYNC_STOP;
  }
  if (!Succeeded(probe)) {
    Log("SERVER_PLUGIN_SYNC: probe " + package.name + " failed: " +
        ServerError(probe));
    return probe.status == 0 || probe.status >= 500 ? SYNC_RETRY : SYNC_STOP;
  }
  if (!JsonBooleanValue(probe.body, "upload_required", true)) {
    Log("SERVER_PLUGIN_SYNC: " + package.name + " " + package.version +
        " is already current");
    return SYNC_COMPLETE;
  }

  const unsigned __int64 totalChunks =
      (package.size + kUploadChunkBytes - 1) / kUploadChunkBytes;
  const std::string startBody =
      "{\"name\":\"" + JsonEscape(package.name) + "\",\"version\":\"" +
      JsonEscape(package.version) + "\",\"archive_name\":\"" +
      JsonEscape(package.archiveName) + "\",\"size\":" +
      ToString(static_cast<unsigned int>(package.size)) +
      ",\"total_chunks\":" + ToString(static_cast<unsigned int>(totalChunks)) +
      "}";
  const HttpResponse started =
      Request(host, port, L"POST", "start-upload", "application/json",
              startBody.data(), static_cast<DWORD>(startBody.size()));
  const std::string uploadId = JsonStringValue(started.body, "upload_id");
  if (!Succeeded(started) || uploadId.empty()) {
    Log("SERVER_PLUGIN_SYNC: start upload " + package.name + " failed: " +
        ServerError(started));
    return started.status == 0 || started.status >= 500 ? SYNC_RETRY : SYNC_STOP;
  }

  HANDLE file = CreateFileA(package.archivePath.c_str(), GENERIC_READ,
                            FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    Log("SERVER_PLUGIN_SYNC: cannot read package " + package.archiveName);
    return SYNC_STOP;
  }
  std::vector<char> buffer(kUploadChunkBytes);
  SyncOutcome outcome = SYNC_STOP;
  for (unsigned __int64 index = 0; index < totalChunks; ++index) {
    DWORD read = 0;
    if (!ReadFile(file, &buffer[0], kUploadChunkBytes, &read, NULL) ||
        read == 0) {
      Log("SERVER_PLUGIN_SYNC: package " + package.archiveName +
          " changed while uploading");
      break;
    }
    const std::string action = "upload-chunk&upload_id=" + UrlEncode(uploadId) +
                               "&index=" +
                               ToString(static_cast<unsigned int>(index));
    const HttpResponse chunk = Request(host, port, L"POST", action,
                                       "application/octet-stream", &buffer[0],
                                       read);
    if (!Succeeded(chunk)) {
      Log("SERVER_PLUGIN_SYNC: upload chunk " +
          ToString(static_cast<unsigned int>(index + 1)) + " for " +
          package.name + " failed: " + ServerError(chunk));
      outcome = chunk.status == 0 || chunk.status >= 500 ? SYNC_RETRY : SYNC_STOP;
      break;
    }
    if (index + 1 < totalChunks) {
      continue;
    }
    const std::string state = JsonStringValue(chunk.body, "status");
    if (!JsonBooleanValue(chunk.body, "complete", false)) {
      Log("SERVER_PLUGIN_SYNC: server did not complete upload of " +
          package.name);
    } else if (state == "completed") {
      Log("SERVER_PLUGIN_SYNC: installed " + package.name + " " +
          package.version);
      outcome = SYNC_COMPLETE;
    } else if (state == "failed") {
      Log("SERVER_PLUGIN_SYNC: " + package.name + " " + package.version +
          " activation failed: " + ServerError(chunk));
    } else {
      const std::string jobId = JsonStringValue(chunk.body, "id");
      outcome = jobId.empty() ? SYNC_STOP
                              : WaitForJob(host, port, package, jobId);
    }
  }
  CloseHandle(file);
  return outcome;
}

SyncOutcome SyncPackages(const std::vector<Package> &packages) {
  std::wstring host;
  unsigned short port = 0;
  if (!ResolveStobeServerTarget(host, port)) {
    return SYNC_RETRY;
  }
  bool retry = false;
  for (size_t i = 0; i < packages.size(); ++i) {
    bool endpointMissing = false;
    const SyncOutcome outcome =
        UploadPackage(host, static_cast<INTERNET_PORT>(port), packages[i],
                      endpointMissing);
    if (endpointMissing) {
      Log("SERVER_PLUGIN_SYNC: the selected StobeServer has no package API; "
          "skipping addon server packages");
      return SYNC_STOP;
    }
    if (outcome == SYNC_RETRY) {
      retry = true;
    }
  }
  return retry ? SYNC_RETRY : SYNC_COMPLETE;
}

DWORD WINAPI SyncThread(LPVOID parameter) {
  std::vector<ActiveMod> *mods = static_cast<std::vector<ActiveMod> *>(parameter);
  std::map<std::string, Package> found;
  for (size_t i = 0; i < mods->size(); ++i) {
    ScanModFolder((*mods)[i], found);
  }
  const size_t modCount = mods->size();
  delete mods;

  if (found.empty()) {
    Log("SERVER_PLUGIN_SYNC: no addon server packages in " +
        ToString(static_cast<unsigned int>(modCount)) + " active mod(s)");
    return 0;
  }
  std::vector<Package> packages;
  for (std::map<std::string, Package>::const_iterator it = found.begin();
       it != found.end(); ++it) {
    packages.push_back(it->second);
    Log("SERVER_PLUGIN_SYNC: found " + it->second.name + " " +
        it->second.version + " in mod " + it->second.modName);
  }
  if (SyncPackages(packages) != SYNC_RETRY) {
    return 0;
  }
  Log("SERVER_PLUGIN_SYNC: sync incomplete; retrying once");
  Sleep(kRetryDelayMs);
  if (SyncPackages(packages) == SYNC_RETRY) {
    Log("SERVER_PLUGIN_SYNC: automatic sync remains incomplete");
  }
  return 0;
}

} // namespace

void OnGameThreadTick(GameWorld *world) {
  if (!world || InterlockedCompareExchange(&g_started, 0, 0) != 0 ||
      !Stobe::Interaction::Allowed()) {
    return;
  }
  InterlockedExchange(&g_started, 1);

  std::vector<ActiveMod> *mods = new std::vector<ActiveMod>();
  try {
    const lektor<ModInfo *> &active = world->activeMods;
    for (uint32_t i = 0; i < active.size(); ++i) {
      ModInfo *info = active[i];
      if (!info || reinterpret_cast<uintptr_t>(info) <= 0x1000 ||
          info->isBaseMod) {
        continue;
      }
      ActiveMod mod;
      mod.name = info->name;
      mod.path = info->path;
      mod.file = info->file;
      mods->push_back(mod);
    }
  } catch (...) {
    Log("SERVER_PLUGIN_SYNC: could not read Kenshi's active mod list");
    delete mods;
    return;
  }

  HANDLE thread = CreateThread(NULL, 0, SyncThread, mods, 0, NULL);
  if (!thread) {
    Log("SERVER_PLUGIN_SYNC: could not start sync thread");
    delete mods;
    return;
  }
  CloseHandle(thread);
}

} // namespace ServerPluginSync
} // namespace Stobe
