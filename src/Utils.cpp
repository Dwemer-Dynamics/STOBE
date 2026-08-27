#include "Utils.h"
#include "ChatBox.h"
#include "ChatUIGlobals.h"
#include "Comm.h"
#include "Context.h"
#include "Globals.h"
#include "StobeChatMode.h"
#include "StobeText.h"
#include <fstream>
#include <iomanip>
#include <kenshi/Character.h>
#include <kenshi/Enums.h>
#include <kenshi/GameWorld.h>
#include <kenshi/RootObject.h>
#include <kenshi/util/hand.h>
#include <sstream>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <windows.h>


using namespace Stobe::UI;

namespace {
std::string TrimCopy(const std::string &value) {
  if (value.empty()) {
    return "";
  }
  size_t start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

bool ShouldSuppressNoisyRuntimeLog(const std::string &msg) {
  if (msg.empty()) {
    return false;
  }

  static const char *kSuppressedPrefixes[] = {
      "HOOK_FRAME:",
      "GEO_DEBUG_CTX:",
      "GEO_DEBUG_QUERY:",
      "GEO_DEBUG_INFOLOC:",
      "CONTEXT_NEARBY_ITEMS:",
  };

  for (size_t i = 0; i < sizeof(kSuppressedPrefixes) / sizeof(kSuppressedPrefixes[0]);
       ++i) {
    const std::string prefix = kSuppressedPrefixes[i];
    if (msg.rfind(prefix, 0) == 0) {
      return true;
    }
  }

  // Suppress the high-frequency idle UI heartbeat while retaining other HOOK_UI
  // diagnostics.
  if (msg == "HOOK_UI: active, world stable, waiting for UI input.") {
    return true;
  }

  // Legacy geo-debug fragments occasionally appear without a stable prefix.
  if (msg.rfind("building=", 0) == 0 &&
      msg.find(" town=") != std::string::npos &&
      msg.find(" zone=") != std::string::npos &&
      msg.find(" region=") != std::string::npos) {
    return true;
  }

  return false;
}

std::string ToLowerAsciiCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string SanitizeDialogueForEventStreamImpl(std::string value) {
  value = TrimCopy(value);
  if (value.empty()) {
    return "";
  }

  // Drop embedded NUL tails if engine-provided text contains binary remnants.
  size_t nulPos = value.find('\0');
  if (nulPos != std::string::npos) {
    value = TrimCopy(value.substr(0, nulPos));
  }

  // Strip trailing "??" corruption while keeping intentional single '?'.
  size_t end = value.size();
  size_t questionCount = 0;
  while (end > 0 && value[end - 1] == '?') {
    --end;
    ++questionCount;
  }
  if (questionCount >= 2) {
    while (end > 0 &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
      --end;
    }
    value = TrimCopy(value.substr(0, end));
  }

  // Strip the common leaked trailing token pattern: "... 7".
  if (value.length() >= 2 && value[value.length() - 1] == '7' &&
      std::isspace(static_cast<unsigned char>(value[value.length() - 2])) != 0) {
    size_t prefixEnd = value.length() - 2;
    while (prefixEnd > 0 &&
           std::isspace(static_cast<unsigned char>(value[prefixEnd - 1])) != 0) {
      --prefixEnd;
    }
    if (prefixEnd > 0) {
      value = TrimCopy(value.substr(0, prefixEnd));
    }
  }

  return TrimCopy(value);
}

bool IsDialogueLikeEventType(const std::string &type) {
  std::string normalized = ToLowerAsciiCopy(TrimCopy(type));
  return normalized == "chat" || normalized == "rechat" ||
         normalized == "inputtext" || normalized == "bored";
}

static std::map<std::string, DWORD> g_recentLimbLossEventTicks;
static const DWORD kLimbLossDuplicateWindowMs = 15000;

std::string ExtractLimbLossKeyToken(const std::string &message) {
  std::string normalized = ToLowerAsciiCopy(TrimCopy(message));
  if (normalized.empty()) {
    return "unknown";
  }

  size_t severedPos = normalized.find("severed ");
  if (severedPos != std::string::npos) {
    size_t limbStart = severedPos + 8;
    if (limbStart < normalized.size()) {
      size_t fromPos = normalized.find(" from ", limbStart);
      if (fromPos != std::string::npos && fromPos > limbStart) {
        std::string limbToken =
            TrimCopy(normalized.substr(limbStart, fromPos - limbStart));
        if (!limbToken.empty()) {
          return limbToken;
        }
      }
    }
  }

  return normalized;
}

bool ShouldDropDuplicateLimbLossEvent(const std::string &target,
                                      unsigned int targetSerial,
                                      const std::string &message) {
  DWORD nowTick = GetTickCount();
  std::string victimKey = "";
  if (targetSerial != 0) {
    victimKey = ToString((int)targetSerial);
  } else {
    victimKey = ToLowerAsciiCopy(TrimCopy(target));
  }
  if (victimKey.empty()) {
    victimKey = "unknown";
  }
  std::string limbKey = ExtractLimbLossKeyToken(message);
  std::string dedupeKey = victimKey + "|" + limbKey;

  auto existing = g_recentLimbLossEventTicks.find(dedupeKey);
  if (existing != g_recentLimbLossEventTicks.end() &&
      nowTick - existing->second < kLimbLossDuplicateWindowMs) {
    return true;
  }
  g_recentLimbLossEventTicks[dedupeKey] = nowTick;

  if (g_recentLimbLossEventTicks.size() > 256) {
    for (auto it = g_recentLimbLossEventTicks.begin();
         it != g_recentLimbLossEventTicks.end();) {
      if (nowTick - it->second > (kLimbLossDuplicateWindowMs * 4)) {
        it = g_recentLimbLossEventTicks.erase(it);
      } else {
        ++it;
      }
    }
  }

  return false;
}

bool ShouldFilterChatEventLine(const std::string &type,
                               const std::string &message) {
  if (ToLowerAsciiCopy(TrimCopy(type)) != "chat") {
    return false;
  }

  std::string normalized = ToLowerAsciiCopy(TrimCopy(message));
  while (!normalized.empty()) {
    char tail = normalized[normalized.size() - 1];
    if (tail == '.' || tail == '!' || tail == '?' || tail == ',' ||
        tail == ';' || tail == ':') {
      normalized.pop_back();
      continue;
    }
    break;
  }
  normalized = TrimCopy(normalized);

  if (normalized == "i can't afford that" || normalized == "i cant afford that") {
    return true;
  }

  if (normalized.size() > 18) {
    const std::string suffixA = ": i can't afford that";
    const std::string suffixB = ": i cant afford that";
    if (normalized.size() >= suffixA.size() &&
        normalized.compare(normalized.size() - suffixA.size(), suffixA.size(),
                           suffixA) == 0) {
      return true;
    }
    if (normalized.size() >= suffixB.size() &&
        normalized.compare(normalized.size() - suffixB.size(), suffixB.size(),
                           suffixB) == 0) {
      return true;
    }
  }

  return false;
}

std::string GetExecutableDir() {
  char path[MAX_PATH];
  GetModuleFileNameA(NULL, path, MAX_PATH);
  std::string dir = path;
  size_t lastBackslash = dir.find_last_of("\\/");
  if (lastBackslash != std::string::npos) {
    dir = dir.substr(0, lastBackslash);
  }
  return dir;
}

std::string GetStobeConfigDir(bool ensureModDir) {
  std::string exeDir = GetExecutableDir();
  std::string modsDir = exeDir + "\\mods";
  std::string stobeDir = modsDir + "\\Stobe";
  if (ensureModDir) {
    CreateDirectoryA(modsDir.c_str(), NULL);
    CreateDirectoryA(stobeDir.c_str(), NULL);
  }
  return stobeDir;
}

std::string GetStobeIniPath(bool ensureModDir) {
  std::string stobeDir = GetStobeConfigDir(ensureModDir);
  return stobeDir + "\\Stobe.ini";
}

std::string GetStobeCustomIniPath(bool ensureModDir) {
  std::string stobeDir = GetStobeConfigDir(ensureModDir);
  return stobeDir + "\\StobeCustom.ini";
}

std::string GetStobeLogPath(bool ensureModDir) {
  std::string stobeDir = GetStobeConfigDir(ensureModDir);
  return stobeDir + "\\stobe.log";
}

bool FileExists(const std::string &path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void EnsureCustomIniSeeded(const std::string &baseIniPath,
                           const std::string &customIniPath) {
  if (FileExists(customIniPath)) {
    return;
  }

  if (FileExists(baseIniPath)) {
    if (CopyFileA(baseIniPath.c_str(), customIniPath.c_str(), TRUE)) {
      Log("CONFIG: Seeded StobeCustom.ini from Stobe.ini.");
      return;
    }
    Log("CONFIG_WARN: Failed to seed StobeCustom.ini from Stobe.ini.");
  }

  std::ofstream out(customIniPath.c_str(), std::ios::out | std::ios::trunc);
  if (out.is_open()) {
    out << "[Settings]" << std::endl;
    out.close();
    Log("CONFIG: Created empty StobeCustom.ini.");
  } else {
    Log("CONFIG_WARN: Unable to create StobeCustom.ini.");
  }
}

std::string ReadLayeredIniString(const std::string &baseIniPath,
                                 const std::string &customIniPath,
                                 const char *section, const char *key,
                                 const char *defaultValue) {
  char baseBuf[512];
  char customBuf[512];
  GetPrivateProfileStringA(section, key, defaultValue, baseBuf,
                           sizeof(baseBuf), baseIniPath.c_str());
  GetPrivateProfileStringA(section, key, baseBuf, customBuf, sizeof(customBuf),
                           customIniPath.c_str());
  return std::string(customBuf);
}

int ReadLayeredIniInt(const std::string &baseIniPath,
                      const std::string &customIniPath, const char *section,
                      const char *key, int defaultValue) {
  int baseValue =
      GetPrivateProfileIntA(section, key, defaultValue, baseIniPath.c_str());
  return GetPrivateProfileIntA(section, key, baseValue, customIniPath.c_str());
}

} // namespace

std::string SanitizeDialogueForEventStream(const std::string &value) {
  return Stobe::Text::SanitizeDialogueForEventStream(value);
}

std::wstring WideFromUtf8(const std::string &str) {
  if (str.empty())
    return L"";
  int size_needed =
      MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
  std::wstring wstrTo(size_needed, 0);
  MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0],
                      size_needed);
  return wstrTo;
}

void Log(const std::string &msg) {
  if (ShouldSuppressNoisyRuntimeLog(msg)) {
    return;
  }

  SYSTEMTIME st;
  GetLocalTime(&st);
  char timestamp[64];
  sprintf_s(timestamp, "%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear,
            st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
  std::string line = "[" + std::string(timestamp) + "] [Stobe] " + msg;

  EnterCriticalSection(&g_LogMutex);
  std::ofstream legacyLogFile("Stobe_SDK.log", std::ios::app);
  if (legacyLogFile.is_open())
    legacyLogFile << line << std::endl;

  std::string rootStobeLogPath = GetExecutableDir() + "\\stobe.log";
  std::ofstream rootStobeLogFile(rootStobeLogPath.c_str(), std::ios::app);
  if (rootStobeLogFile.is_open())
    rootStobeLogFile << line << std::endl;

  std::string modStobeLogPath = GetStobeLogPath(true);
  std::ofstream modStobeLogFile(modStobeLogPath.c_str(), std::ios::app);
  if (modStobeLogFile.is_open())
    modStobeLogFile << line << std::endl;

  LeaveCriticalSection(&g_LogMutex);
  OutputDebugStringA((line + "\n").c_str());
}

void ResetRuntimeLogsForSession() {
  // Clear logs once at game startup so each run has a fresh trace.
  std::ofstream legacyLogFile("Stobe_SDK.log", std::ios::trunc);
  legacyLogFile.close();

  std::string rootStobeLogPath = GetExecutableDir() + "\\stobe.log";
  std::ofstream rootStobeLogFile(rootStobeLogPath.c_str(), std::ios::trunc);
  rootStobeLogFile.close();

  std::string modStobeLogPath = GetStobeLogPath(true);
  std::ofstream modStobeLogFile(modStobeLogPath.c_str(), std::ios::trunc);
  modStobeLogFile.close();
}

template <typename T> std::string ToStringT(T val) {
  std::ostringstream ss;
  ss << val;
  return ss.str();
}

std::string ToString(int val) { return ToStringT(val); }
std::string ToString(unsigned int val) { return ToStringT(val); }
std::string ToString(float val) { return ToStringT(val); }

std::string EscapeJSON(const std::string &s) {
  return Stobe::Text::EscapeJSON(s);
}

std::string UnescapeJSON(const std::string &s) {
  return Stobe::Text::UnescapeJSON(s);
}

std::string JsonReadField(const std::string &json, const std::string &key) {
  return Stobe::Text::JsonReadField(json, key);
}

void SetHotkeyFromString(const std::string &keyStr) {
  std::string normalized = keyStr;
  if (!normalized.empty()) {
    if (normalized[0] >= 'a' && normalized[0] <= 'z') {
      normalized[0] = (char)(normalized[0] - ('a' - 'A'));
    }
  }
  g_chatHotkeyStr = normalized;
  if (normalized == "/")
    g_chatHotkey = VK_OEM_2;
  else if (normalized == "-")
    g_chatHotkey = VK_OEM_MINUS;
  else if (normalized == ".")
    g_chatHotkey = VK_OEM_PERIOD;
  else if (normalized == "\\")
    g_chatHotkey = VK_OEM_5;
  else if (normalized == "[")
    g_chatHotkey = VK_OEM_4;
  else if (normalized == "]")
    g_chatHotkey = VK_OEM_6;
  else if (normalized == "O")
    g_chatHotkey = 'O';
  else if (normalized == "P")
    g_chatHotkey = 'P';
  else if (normalized == "J")
    g_chatHotkey = 'J';
  else if (normalized == "U")
    g_chatHotkey = 'U';
  else if (normalized == "K")
    g_chatHotkey = 'K';
  else {
    g_chatHotkey = VK_OEM_2;
    g_chatHotkeyStr = "/";
  }
}

void SetGeneralHotkeyFromString(const std::string &keyStr) {
  std::string normalized = keyStr;
  for (size_t i = 0; i < normalized.size(); ++i) {
    if (normalized[i] >= 'a' && normalized[i] <= 'z') {
      normalized[i] = (char)(normalized[i] - ('a' - 'A'));
    }
  }
  normalized = TrimCopy(normalized);

  g_generalHotkeyStr = normalized;
  if (normalized == "=")
    g_generalHotkey = VK_OEM_PLUS;
  else if (normalized == "F7")
    g_generalHotkey = VK_F7;
  else if (normalized == "F8")
    g_generalHotkey = VK_F8;
  else if (normalized == "F11")
    g_generalHotkey = VK_F11;
  else if (normalized == "F12")
    g_generalHotkey = VK_F12;
  else if (normalized == "O")
    g_generalHotkey = 'O';
  else if (normalized == "[")
    g_generalHotkey = VK_OEM_4;
  else if (normalized == "}" || normalized == "]") {
    g_generalHotkey = VK_OEM_6;
    g_generalHotkeyStr = "}";
  } else {
    g_generalHotkey = VK_OEM_PLUS;
    g_generalHotkeyStr = "=";
  }
}

void SetPushToTalkHotkeyFromString(const std::string &keyStr) {
  std::string normalized = TrimCopy(keyStr);
  if (normalized.size() == 1 && normalized[0] >= 'a' && normalized[0] <= 'z')
    normalized[0] = static_cast<char>(normalized[0] - ('a' - 'A'));
  if (normalized.size() == 1 && normalized[0] >= 'A' && normalized[0] <= 'Z') {
    g_pushToTalkHotkey = normalized[0];
    g_pushToTalkHotkeyStr = normalized;
  } else {
    g_pushToTalkHotkey = 'V';
    g_pushToTalkHotkeyStr = "V";
  }
}

void LoadStobeRuntimeConfig() {
  std::string baseIniPath = GetStobeIniPath(false);
  std::string customIniPath = GetStobeCustomIniPath(true);
  EnsureCustomIniSeeded(baseIniPath, customIniPath);

  g_serverHost = TrimCopy(ReadLayeredIniString(baseIniPath, customIniPath,
                                               "Settings", "ServerHost",
                                               "127.0.0.1"));
  if (g_serverHost.empty()) {
    g_serverHost = "127.0.0.1";
  }
  g_serverPort =
      ReadLayeredIniInt(baseIniPath, customIniPath, "Settings", "ServerPort", 8083);
  if (g_serverPort < 1 || g_serverPort > 65535) {
    g_serverPort = 8083;
  }

  SetGeneralHotkeyFromString(ReadLayeredIniString(
      baseIniPath, customIniPath, "Settings", "GeneralHotkey", "="));
  SetHotkeyFromString(ReadLayeredIniString(baseIniPath, customIniPath,
                                           "Settings", "ChatHotkey", "/"));
  SetPushToTalkHotkeyFromString(ReadLayeredIniString(
      baseIniPath, customIniPath, "Settings", "PushToTalkHotkey", "V"));
  g_chatMode = Stobe::ChatMode::Normalize(ReadLayeredIniString(
      baseIniPath, customIniPath, "Settings", "ChatMode", "chat"));
  g_autoChatEnabled =
      ReadLayeredIniInt(baseIniPath, customIniPath, "Settings", "AutoChat", 0) !=
      0;
  g_enableAnimalTalks =
      ReadLayeredIniInt(baseIniPath, customIniPath, "Settings", "AnimalTalks", 0) !=
      0;
  g_useNearestPlayerSpeaker =
      ReadLayeredIniInt(baseIniPath, customIniPath, "Settings",
                        "UseNearestPlayerSpeaker", 1) != 0;
  g_lastChatModeIndex = Stobe::ChatMode::ToIndex(g_chatMode);

  g_boredEventRange = (float)ReadLayeredIniInt(baseIniPath, customIniPath,
                                                "Settings", "BoredEventRange", 200);
  g_proximityRadius = (float)ReadLayeredIniInt(baseIniPath, customIniPath,
                                               "Settings", "TalkRadius", 80);
  g_shoutRadius = (float)ReadLayeredIniInt(baseIniPath, customIniPath,
                                           "Settings", "ShoutRadius", 200);
  g_ttsVolumePercent =
      ReadLayeredIniInt(baseIniPath, customIniPath, "Settings", "TTSVolume", 100);
  if (g_ttsVolumePercent < 0) {
    g_ttsVolumePercent = 0;
  } else if (g_ttsVolumePercent > 100) {
    g_ttsVolumePercent = 100;
  }
  g_ttsEnabled =
      ReadLayeredIniInt(baseIniPath, customIniPath, "Settings", "TtsEnabled", 1) !=
      0;
  g_enableDialogueMenuTts =
      ReadLayeredIniInt(baseIniPath, customIniPath, "Settings",
                        "EnableDialogueMenuTTS", 1) != 0;
  g_speedDialogue = ReadLayeredIniInt(baseIniPath, customIniPath, "Settings",
                                      "Speed Dialogue", 1) != 0;
  g_enableRegularDialogueCapture =
      ReadLayeredIniInt(baseIniPath, customIniPath, "Settings",
                        "EnableRegularDialogueCapture", 1) != 0;
  g_enableItemImageSync =
      ReadLayeredIniInt(baseIniPath, customIniPath, "Settings",
                        "EnableItemImageSync", 0) != 0;
  g_enableStatusHud =
      ReadLayeredIniInt(baseIniPath, customIniPath, "Settings",
                        "EnableStatusHud", 0) != 0;
  int boredEventIntervalHours = ReadLayeredIniInt(
      baseIniPath, customIniPath, "Settings", "BoredEventTimerHours", -1);
  if (boredEventIntervalHours < 1) {
    boredEventIntervalHours = ReadLayeredIniInt(
        baseIniPath, customIniPath, "Settings", "BoardEventIntervalHours", -1);
  }
  if (boredEventIntervalHours < 1) {
    int legacyIntervalSeconds = ReadLayeredIniInt(
        baseIniPath, customIniPath, "Settings", "BoardEventIntervalSeconds",
        3 * 3600);
    if (legacyIntervalSeconds < 1) {
      legacyIntervalSeconds = 3600;
    }
    if (legacyIntervalSeconds <= 300) {
      // Treat old second-based defaults (240s) as "unset" and migrate to the
      // new default rather than collapsing to 1h.
      boredEventIntervalHours = 3;
    } else {
      boredEventIntervalHours = (legacyIntervalSeconds + 3599) / 3600;
    }
  }
  g_boredEventIntervalHours = boredEventIntervalHours;
  if (g_boredEventIntervalHours < 1) {
    g_boredEventIntervalHours = 1;
  } else if (g_boredEventIntervalHours > 720) {
    g_boredEventIntervalHours = 720;
  }
  int dynamicProfileIntervalHours = ReadLayeredIniInt(
      baseIniPath, customIniPath, "Settings", "DynamicProfileIntervalHours", -1);
  if (dynamicProfileIntervalHours < 1) {
    int legacyIntervalMinutes =
        ReadLayeredIniInt(baseIniPath, customIniPath, "Settings",
                          "DynamicProfileIntervalMinutes", 24 * 60);
    if (legacyIntervalMinutes < 1) {
      legacyIntervalMinutes = 60;
    }
    dynamicProfileIntervalHours = (legacyIntervalMinutes + 59) / 60;
  }
  g_dynamicProfileIntervalHours = dynamicProfileIntervalHours;
  if (g_dynamicProfileIntervalHours < 1) {
    g_dynamicProfileIntervalHours = 1;
  } else if (g_dynamicProfileIntervalHours > 720) {
    g_dynamicProfileIntervalHours = 720;
  }

  g_enableBoredEvents =
      ReadLayeredIniInt(baseIniPath, customIniPath, "Settings",
                        "EnableBoredEventConversations", 1) != 0;

  int enableWelcomeFallback = ReadLayeredIniInt(
      baseIniPath, customIniPath, "Settings", "EnableWelcomePopup", 1);
  g_enableWelcome = ReadLayeredIniInt(baseIniPath, customIniPath, "Settings",
                                      "EnableMOTD", enableWelcomeFallback) != 0;

  Log("CONFIG: Loaded ProximityRadius=" + ToString(g_proximityRadius) +
      ", BoredEventRange=" + ToString(g_boredEventRange) +
      ", TTSVolume=" + ToString(g_ttsVolumePercent) +
      ", ServerHost=" + g_serverHost +
      ", ServerPort=" + ToString(g_serverPort) +
       ", TtsEnabled=" + (g_ttsEnabled ? "true" : "false") +
       ", DialogueMenuTTS=" + (g_enableDialogueMenuTts ? "true" : "false") +
      ", SpeedDialogue=" + (g_speedDialogue ? "true" : "false") +
      ", RegularDialogueCapture=" +
      (g_enableRegularDialogueCapture ? "true" : "false") +
       ", ItemImageSync=" + (g_enableItemImageSync ? "true" : "false") +
       ", StatusHud=" + (g_enableStatusHud ? "true" : "false") +
      ", BoredEventTimer=" + ToString(g_boredEventIntervalHours) + "h" +
      ", DynamicProfileInterval=" + ToString(g_dynamicProfileIntervalHours) +
      "h" +
      ", AnimalTalks=" + (g_enableAnimalTalks ? "true" : "false") +
      ", NearestSpeaker=" +
      (g_useNearestPlayerSpeaker ? "true" : "false") +
      ", EnableBoredEvents=" + (g_enableBoredEvents ? "true" : "false") +
      ", EnableWelcome=" + (g_enableWelcome ? "true" : "false"));

  // Ensure StobeCustom.ini exists with all current keys while preserving
  // baseline defaults in Stobe.ini.
  SaveStobeRuntimeConfig();
}

void SaveStobeRuntimeConfig() {
  std::string iniPath = GetStobeCustomIniPath(true);

  WritePrivateProfileStringA("Settings", "ServerHost", g_serverHost.c_str(),
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "ServerPort",
                             ToString(g_serverPort).c_str(), iniPath.c_str());
  WritePrivateProfileStringA("Settings", "GeneralHotkey",
                             g_generalHotkeyStr.c_str(), iniPath.c_str());
  WritePrivateProfileStringA("Settings", "ChatHotkey", g_chatHotkeyStr.c_str(),
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "PushToTalkHotkey",
                             g_pushToTalkHotkeyStr.c_str(), iniPath.c_str());
  WritePrivateProfileStringA("Settings", "ChatMode", g_chatMode.c_str(),
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "AutoChat",
                             g_autoChatEnabled ? "1" : "0", iniPath.c_str());
  WritePrivateProfileStringA("Settings", "AnimalTalks",
                             g_enableAnimalTalks ? "1" : "0",
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "UseNearestPlayerSpeaker",
                             g_useNearestPlayerSpeaker ? "1" : "0",
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "BoredEventRange",
                             ToString((int)g_boredEventRange).c_str(),
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "TalkRadius",
                             ToString((int)g_proximityRadius).c_str(),
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "ShoutRadius",
                             ToString((int)g_shoutRadius).c_str(),
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "TTSVolume",
                             ToString(g_ttsVolumePercent).c_str(),
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "TtsEnabled",
                              g_ttsEnabled ? "1" : "0", iniPath.c_str());
  WritePrivateProfileStringA("Settings", "EnableDialogueMenuTTS",
                             g_enableDialogueMenuTts ? "1" : "0",
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "Speed Dialogue",
                             g_speedDialogue ? "1" : "0", iniPath.c_str());
  WritePrivateProfileStringA("Settings", "EnableRegularDialogueCapture",
                             g_enableRegularDialogueCapture ? "1" : "0",
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "EnableItemImageSync",
                             g_enableItemImageSync ? "1" : "0",
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "EnableStatusHud",
                             g_enableStatusHud ? "1" : "0",
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "BoredEventTimerHours",
                             ToString(g_boredEventIntervalHours).c_str(),
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "DynamicProfileIntervalHours",
                             ToString(g_dynamicProfileIntervalHours).c_str(),
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "EnableBoredEventConversations",
                             g_enableBoredEvents ? "1" : "0", iniPath.c_str());
  WritePrivateProfileStringA("Settings", "EnableMOTD",
                             g_enableWelcome ? "1" : "0", iniPath.c_str());

  // Remove deprecated/renamed keys from legacy configs.
  WritePrivateProfileStringA("Settings", "EnableWelcomePopup", NULL,
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "GlobalEventsCount", NULL,
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "DialogueSpeedSeconds", NULL,
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "DialogueSpeed", NULL,
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "BoardEventIntervalHours", NULL,
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "BoardEventIntervalSeconds", NULL,
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "DynamicProfileIntervalMinutes", NULL,
                             iniPath.c_str());
  WritePrivateProfileStringA("Settings", "Language", NULL, iniPath.c_str());
  WritePrivateProfileStringA("Settings", "WhisperRadius", NULL,
                             iniPath.c_str());

  Log("CONFIG: Saved full settings state to StobeCustom.ini.");
}

static int ResolveCurrentGameTsForEvent() {
  GameWorld *world = GetWorldSafe();
  if (!world || reinterpret_cast<uintptr_t>(world) < 0x1000) {
    return 0;
  }
  int gameTs = 0;
  try {
    TimeOfDay tod = world->getTimeStamp_inGameHours();
    gameTs = static_cast<int>(tod.getTotalSeconds());
  } catch (...) {
    gameTs = 0;
  }
  if (gameTs < 0) {
    gameTs = 0;
  }
  return gameTs;
}

static std::string NormalizeEventName(const std::string &value) {
  std::string out = value;
  out.erase(0, out.find_first_not_of(" \t\r\n"));
  size_t last = out.find_last_not_of(" \t\r\n");
  if (last == std::string::npos) {
    return "";
  }
  out.erase(last + 1);
  if (out == "None" || out == "Unknown" || out == "null") {
    return "";
  }
  return out;
}

static bool EqualsIgnoreCaseToken(const std::string &a, const std::string &b) {
  if (a.length() != b.length()) {
    return false;
  }
  for (size_t i = 0; i < a.length(); ++i) {
    unsigned char ac = static_cast<unsigned char>(a[i]);
    unsigned char bc = static_cast<unsigned char>(b[i]);
    if (tolower(ac) != tolower(bc)) {
      return false;
    }
  }
  return true;
}

static std::string BuildEventPeopleJsonArray(
    const std::vector<std::string> &people) {
  std::string json = "[";
  for (size_t i = 0; i < people.size(); ++i) {
    if (i > 0) {
      json += ",";
    }
    json += "\"" + EscapeJSON(people[i]) + "\"";
  }
  json += "]";
  return json;
}

static bool IsValidIndoorsHandleForEvent(const hand &indoorsHandle) {
  return indoorsHandle.isValid() && !indoorsHandle.isNull();
}

static bool TryGetEventSpatialState(Character *character, bool &hasBuilding,
                                    unsigned int &buildingSerial,
                                    int &floorValue) {
  hasBuilding = false;
  buildingSerial = 0;
  floorValue = 0;
  if (!character || reinterpret_cast<uintptr_t>(character) <= 0x1000) {
    return false;
  }
#if defined(_MSC_VER)
  __try {
#endif
    const hand &indoorsHandle = character->isIndoors();
    hasBuilding = IsValidIndoorsHandleForEvent(indoorsHandle);
    buildingSerial = hasBuilding ? indoorsHandle.serial : 0;
    floorValue = character->getFloor();
    return true;
#if defined(_MSC_VER)
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#endif
}

static bool IsEventAreaCompatible(Character *anchor, Character *candidate) {
  if (!anchor || !candidate || reinterpret_cast<uintptr_t>(anchor) <= 0x1000 ||
      reinterpret_cast<uintptr_t>(candidate) <= 0x1000) {
    return false;
  }

  bool anchorHasBuilding = false;
  bool candidateHasBuilding = false;
  unsigned int anchorBuildingSerial = 0;
  unsigned int candidateBuildingSerial = 0;
  int anchorFloor = 0;
  int candidateFloor = 0;
  if (!TryGetEventSpatialState(anchor, anchorHasBuilding, anchorBuildingSerial,
                               anchorFloor)) {
    return false;
  }
  if (!TryGetEventSpatialState(candidate, candidateHasBuilding,
                               candidateBuildingSerial, candidateFloor)) {
    return false;
  }

  if (anchorHasBuilding) {
    if (!candidateHasBuilding) {
      return false;
    }
    if (anchorBuildingSerial == 0 || candidateBuildingSerial == 0 ||
        anchorBuildingSerial != candidateBuildingSerial) {
      return false;
    }
    int floorDelta = anchorFloor - candidateFloor;
    if (floorDelta < 0) {
      floorDelta = -floorDelta;
    }
    return floorDelta <= 1;
  }

  if (candidateHasBuilding) {
    return false;
  }
  if (candidateFloor > anchorFloor + 1) {
    return false;
  }
  return true;
}

static std::string NormalizeEventGeoToken(const std::string &rawValue) {
  std::string value = TrimCopy(rawValue);
  if (value.empty()) {
    return "";
  }
  std::string lowered = ToLowerAsciiCopy(value);
  if (lowered == "unknown" || lowered == "none" || lowered == "null" ||
      lowered == "n/a") {
    return "";
  }
  return value;
}

static bool TryParseEventGeoBool(const std::string &rawValue, bool &valueOut) {
  std::string value = TrimCopy(rawValue);
  if (value.empty()) {
    return false;
  }
  std::string lowered = ToLowerAsciiCopy(value);
  if (lowered == "1" || lowered == "true" || lowered == "yes" ||
      lowered == "on") {
    valueOut = true;
    return true;
  }
  if (lowered == "0" || lowered == "false" || lowered == "no" ||
      lowered == "off") {
    valueOut = false;
    return true;
  }
  return false;
}

static void AppendEventGeoQueryFromCharacter(std::wstring &endpoint,
                                             Character *character) {
  if (!character || reinterpret_cast<uintptr_t>(character) <= 0x1000) {
    return;
  }

  std::string contextJson = BuildNpcContextEnvelope(character, "player");
  if (contextJson.empty() || contextJson == "{}") {
    return;
  }

  std::string rawTown = JsonReadField(contextJson, "town");
  std::string rawZone = JsonReadField(contextJson, "zone");
  std::string rawRegion = JsonReadField(contextJson, "region");
  std::string town = NormalizeEventGeoToken(rawTown);
  std::string zone = NormalizeEventGeoToken(rawZone);
  std::string region = NormalizeEventGeoToken(rawRegion);
  std::string environmentJson = JsonReadField(contextJson, "environment");
  std::string rawBuilding = JsonReadField(environmentJson, "building_name");
  std::string rawEnvZone = JsonReadField(environmentJson, "zone_name");
  std::string rawEnvRegionName = JsonReadField(environmentJson, "region_name");
  std::string rawEnvRegion = JsonReadField(environmentJson, "region");
  std::string building = NormalizeEventGeoToken(rawBuilding);
  if (zone.empty()) {
    zone = NormalizeEventGeoToken(rawEnvZone);
  }
  if (region.empty()) {
    region = NormalizeEventGeoToken(rawEnvRegionName);
  }
  if (region.empty()) {
    region = NormalizeEventGeoToken(rawEnvRegion);
  }
  if (zone.empty()) {
    zone = town;
  }
  auto safeGeoToken = [](const std::string &value) {
    return value.empty() ? std::string("(empty)") : value;
  };

  bool indoorsValue = false;
  bool outdoorsValue = false;
  bool inTownValue = false;
  bool indoorsKnown = TryParseEventGeoBool(
      JsonReadField(environmentJson, "indoors"), indoorsValue);
  bool outdoorsKnown = TryParseEventGeoBool(
      JsonReadField(environmentJson, "outdoors"), outdoorsValue);
  bool inTownKnown = TryParseEventGeoBool(
      JsonReadField(environmentJson, "in_town"), inTownValue);

  bool useBuilding = IsValidIndoorsHandleForEvent(character->isIndoors());
  if (indoorsKnown && indoorsValue) {
    useBuilding = true;
  }
  if ((outdoorsKnown && outdoorsValue) || (indoorsKnown && !indoorsValue)) {
    useBuilding = false;
  }
  if (!useBuilding) {
    building.clear();
  }

  std::string location = "";
  if (useBuilding && !building.empty() && !zone.empty()) {
    location = building + ", " + zone;
  } else if (useBuilding && !building.empty()) {
    location = building;
  } else if (!zone.empty()) {
    location = zone;
  } else if (!region.empty()) {
    location = region;
  }

  if (!location.empty()) {
    endpoint += L"&location=" + ToWide(UrlEncode(location));
  }
  if (!zone.empty()) {
    endpoint += L"&city=" + ToWide(UrlEncode(zone));
  }
  if (!region.empty()) {
    endpoint += L"&region=" + ToWide(UrlEncode(region));
  }
  if (!building.empty()) {
    endpoint += L"&loc_building=" + ToWide(UrlEncode(building));
  }
  if (!zone.empty()) {
    endpoint += L"&loc_zone=" + ToWide(UrlEncode(zone));
  }
  if (!region.empty()) {
    endpoint += L"&loc_region=" + ToWide(UrlEncode(region));
  }
  endpoint += L"&loc_indoors=" + std::wstring(useBuilding ? L"1" : L"0");

  std::string floorToken =
      NormalizeEventGeoToken(JsonReadField(environmentJson, "floor"));
  std::string xToken = NormalizeEventGeoToken(JsonReadField(environmentJson, "x"));
  std::string yToken = NormalizeEventGeoToken(JsonReadField(environmentJson, "y"));
  std::string zToken = NormalizeEventGeoToken(JsonReadField(environmentJson, "z"));
  if (!floorToken.empty()) {
    endpoint += L"&loc_floor=" + ToWide(UrlEncode(floorToken));
  }
  if (!xToken.empty()) {
    endpoint += L"&loc_x=" + ToWide(UrlEncode(xToken));
  }
  if (!yToken.empty()) {
    endpoint += L"&loc_y=" + ToWide(UrlEncode(yToken));
  }
  if (!zToken.empty()) {
    endpoint += L"&loc_z=" + ToWide(UrlEncode(zToken));
  }
  std::string actorName = "";
  try {
    actorName = character->getName();
  } catch (...) {
    actorName.clear();
  }
  if (actorName.empty()) {
    actorName = "Unknown";
  }
  Log("GEO_DEBUG_QUERY: source=event actor=" + actorName +
      " raw_town=" + safeGeoToken(rawTown) + " raw_zone=" + safeGeoToken(rawZone) +
      " raw_region=" + safeGeoToken(rawRegion) +
      " raw_building=" + safeGeoToken(rawBuilding) +
      " raw_env_zone=" + safeGeoToken(rawEnvZone) +
      " raw_env_region_name=" + safeGeoToken(rawEnvRegionName) +
      " raw_env_region=" + safeGeoToken(rawEnvRegion) +
      " location=" + safeGeoToken(location) +
      " building=" + safeGeoToken(building) + " zone=" + safeGeoToken(zone) +
      " region=" + safeGeoToken(region) +
      " indoors=" + std::string(useBuilding ? "1" : "0") +
      " in_town=" +
      std::string((inTownKnown && inTownValue) ? "1" : (inTownKnown ? "0" : "?")) +
      " floor=" + safeGeoToken(floorToken) + " x=" + safeGeoToken(xToken) +
      " y=" + safeGeoToken(yToken) + " z=" + safeGeoToken(zToken));
}

static unsigned int ResolveCharacterSerialSafe(Character *npc) {
  if (!npc || reinterpret_cast<uintptr_t>(npc) <= 0x1000) {
    return 0;
  }
  unsigned int serial = 0;
  try {
    serial = npc->getHandle().serial;
  } catch (...) {
    serial = 0;
  }
  return serial;
}

static std::string ResolveCharacterNameForEvent(Character *npc) {
  if (!npc || reinterpret_cast<uintptr_t>(npc) <= 0x1000) {
    return "";
  }
  std::string name = "";
  try {
    name = npc->getName();
  } catch (...) {
    name = "";
  }
  return NormalizeEventName(name);
}

static Character *FindCharacterBySerialForEvent(GameWorld *world,
                                                unsigned int serial) {
  if (!world || serial == 0) {
    return nullptr;
  }
  const ogre_unordered_set<Character *>::type &chars =
      world->getCharacterUpdateList();
  for (auto it = chars.begin(); it != chars.end(); ++it) {
    Character *candidate = *it;
    if (!candidate || reinterpret_cast<uintptr_t>(candidate) <= 0x1000) {
      continue;
    }
    if (ResolveCharacterSerialSafe(candidate) == serial) {
      return candidate;
    }
  }
  return nullptr;
}

static Character *FindCharacterByNameForEvent(GameWorld *world,
                                              const std::string &name) {
  if (!world) {
    return nullptr;
  }
  std::string wanted = NormalizeEventName(name);
  if (wanted.empty()) {
    return nullptr;
  }
  const ogre_unordered_set<Character *>::type &chars =
      world->getCharacterUpdateList();
  for (auto it = chars.begin(); it != chars.end(); ++it) {
    Character *candidate = *it;
    if (!candidate || reinterpret_cast<uintptr_t>(candidate) <= 0x1000) {
      continue;
    }
    std::string candidateName = ResolveCharacterNameForEvent(candidate);
    if (!candidateName.empty() && EqualsIgnoreCaseToken(candidateName, wanted)) {
      return candidate;
    }
  }
  return nullptr;
}

static bool TryAppendEventPersonBySerial(std::vector<std::string> &people,
                                         std::set<unsigned int> &seenSerials,
                                         unsigned int serial,
                                         const std::string &name,
                                         size_t totalCap,
                                         int &droppedByCapOut) {
  if (serial == 0 || name.empty()) {
    return false;
  }
  if (seenSerials.count(serial) != 0) {
    return false;
  }
  if (people.size() >= totalCap) {
    ++droppedByCapOut;
    return false;
  }
  seenSerials.insert(serial);
  people.push_back(name + "|" + ToString((int)serial));
  return true;
}

static void CollectEventPeopleFromAnchor(
    GameWorld *world, Character *anchor, float searchRadius, size_t perAnchorCap,
    size_t totalCap, std::vector<std::string> &people,
    std::set<unsigned int> &seenSerials, int &addedCountOut,
    int &droppedByCapOut) {
  addedCountOut = 0;
  if (!world || !anchor || reinterpret_cast<uintptr_t>(anchor) <= 0x1000) {
    return;
  }

  lektor<RootObject *> nearbyResults;
  try {
    world->getCharactersWithinSphere(nearbyResults, anchor->getPosition(),
                                     searchRadius, 0.0f, 0.0f, 0x10, 0,
                                     anchor);
  } catch (...) {
    return;
  }

  for (uint32_t i = 0; i < nearbyResults.size(); ++i) {
    if ((size_t)addedCountOut >= perAnchorCap) {
      break;
    }
    if (people.size() >= totalCap) {
      ++droppedByCapOut;
      break;
    }
    Character *other = reinterpret_cast<Character *>(nearbyResults.stuff[i]);
    if (!other || reinterpret_cast<uintptr_t>(other) <= 0x1000) {
      continue;
    }
    if (!IsEventAreaCompatible(anchor, other)) {
      continue;
    }
    float dist = 0.0f;
    try {
      dist = anchor->getPosition().distance(other->getPosition());
    } catch (...) {
      continue;
    }
    if (dist > searchRadius) {
      continue;
    }

    unsigned int otherSerial = ResolveCharacterSerialSafe(other);
    std::string otherName = ResolveCharacterNameForEvent(other);
    if (TryAppendEventPersonBySerial(people, seenSerials, otherSerial, otherName,
                                     totalCap, droppedByCapOut)) {
      ++addedCountOut;
    }
  }
}

static bool IsPlayerDialogueScopedEvent(GameWorld *world,
                                        const std::string &eventType,
                                        Character *actorNpc,
                                        Character *targetNpc) {
  if (!world || !actorNpc || !targetNpc) {
    return false;
  }

  std::string normalizedType = ToLowerAsciiCopy(TrimCopy(eventType));
  if (!IsDialogueLikeEventType(normalizedType)) {
    return false;
  }

  bool actorIsPlayerCharacter = false;
  bool targetIsPlayerCharacter = false;
  try {
    actorIsPlayerCharacter = actorNpc->isPlayerCharacter();
  } catch (...) {
    actorIsPlayerCharacter = false;
  }
  try {
    targetIsPlayerCharacter = targetNpc->isPlayerCharacter();
  } catch (...) {
    targetIsPlayerCharacter = false;
  }

  return actorIsPlayerCharacter != targetIsPlayerCharacter;
}

static std::string BuildEventPeopleJson(GameWorld *world,
                                        const std::string &eventType,
                                        const std::string &actor,
                                        const std::string &target,
                                        unsigned int actorSerial,
                                        unsigned int targetSerial,
                                        int &peopleCountOut,
                                        int &anchorACountOut,
                                        int &anchorBCountOut,
                                        int &droppedByCapOut,
                                        bool &usedSecondAnchorOut) {
  std::vector<std::string> people;
  std::set<unsigned int> seenSerials;
  peopleCountOut = 0;
  anchorACountOut = 0;
  anchorBCountOut = 0;
  droppedByCapOut = 0;
  usedSecondAnchorOut = false;

  Character *actorNpc = FindCharacterBySerialForEvent(world, actorSerial);
  Character *targetNpc = FindCharacterBySerialForEvent(world, targetSerial);
  if (!actorNpc && !NormalizeEventName(actor).empty()) {
    actorNpc = FindCharacterByNameForEvent(world, actor);
  }
  if (!targetNpc && !NormalizeEventName(target).empty()) {
    targetNpc = FindCharacterByNameForEvent(world, target);
  }

  std::string actorName = NormalizeEventName(actor);
  std::string targetName = NormalizeEventName(target);
  if (actorNpc) {
    actorName = ResolveCharacterNameForEvent(actorNpc);
  }
  if (targetNpc) {
    targetName = ResolveCharacterNameForEvent(targetNpc);
  }

  const size_t kPerAnchorCap = 12;
  const size_t kTotalPeopleCap = 24;
  unsigned int resolvedActorSerial =
      actorNpc ? ResolveCharacterSerialSafe(actorNpc) : actorSerial;
  unsigned int resolvedTargetSerial =
      targetNpc ? ResolveCharacterSerialSafe(targetNpc) : targetSerial;
  TryAppendEventPersonBySerial(people, seenSerials, resolvedActorSerial, actorName,
                               kTotalPeopleCap, droppedByCapOut);
  TryAppendEventPersonBySerial(people, seenSerials, resolvedTargetSerial,
                               targetName, kTotalPeopleCap, droppedByCapOut);

  if (IsPlayerDialogueScopedEvent(world, eventType, actorNpc, targetNpc)) {
    peopleCountOut = static_cast<int>(people.size());
    return BuildEventPeopleJsonArray(people);
  }

  Character *anchorA = targetNpc ? targetNpc : actorNpc;
  Character *anchorB = nullptr;
  if (targetNpc && actorNpc) {
    anchorB = (anchorA == targetNpc) ? actorNpc : targetNpc;
    unsigned int anchorASerial = ResolveCharacterSerialSafe(anchorA);
    unsigned int anchorBSerial = ResolveCharacterSerialSafe(anchorB);
    if (anchorASerial == 0 || anchorBSerial == 0 || anchorASerial == anchorBSerial) {
      anchorB = nullptr;
    }
  }
  usedSecondAnchorOut = (anchorB != nullptr);

  if (world && anchorA && reinterpret_cast<uintptr_t>(anchorA) > 0x1000) {
    float searchRadius = g_shoutRadius;
    if (searchRadius < g_proximityRadius) {
      searchRadius = g_proximityRadius;
    }
    if (searchRadius < 30.0f) {
      searchRadius = 30.0f;
    } else if (searchRadius > 600.0f) {
      searchRadius = 600.0f;
    }
    CollectEventPeopleFromAnchor(world, anchorA, searchRadius, kPerAnchorCap,
                                 kTotalPeopleCap, people, seenSerials,
                                 anchorACountOut, droppedByCapOut);
    if (anchorB && people.size() < kTotalPeopleCap) {
      CollectEventPeopleFromAnchor(world, anchorB, searchRadius, kPerAnchorCap,
                                   kTotalPeopleCap, people, seenSerials,
                                   anchorBCountOut, droppedByCapOut);
    }
  }

  peopleCountOut = static_cast<int>(people.size());
  return BuildEventPeopleJsonArray(people);
}

static std::string BuildEventStreamData(const std::string &type,
                                        const std::string &actor,
                                        const std::string &target,
                                        const std::string &message) {
  std::string speaker = NormalizeEventName(actor);
  std::string listener = NormalizeEventName(target);
  std::string normalizedType = ToLowerAsciiCopy(TrimCopy(type));
  if (speaker.empty()) {
    speaker = listener;
    listener.clear();
  }
  if (speaker.empty()) {
    speaker = "Unknown";
  }

  std::string body = message;
  if (IsDialogueLikeEventType(type)) {
    body = Stobe::Text::SanitizeDialogueForEventStream(body);
  }
  if (body.empty()) {
    body = type;
  }
  if (normalizedType == "init") {
    return body;
  }

  std::string line = "";
  if (normalizedType == "combat_start" && !listener.empty() &&
      listener != speaker) {
    line = speaker + ": " + body + " with " + listener;
  } else if (normalizedType == "combat_end" && !listener.empty() &&
             listener != speaker) {
    const std::string combatEndedPrefix = "combat ended";
    std::string trimmedBody = TrimCopy(body);
    std::string normalizedBody = ToLowerAsciiCopy(trimmedBody);
    if (normalizedBody.rfind(combatEndedPrefix, 0) == 0) {
      line = speaker + ": combat with " + listener +
             trimmedBody.substr(std::string("combat").length());
    } else {
      line = speaker + ": " + body + " with " + listener;
    }
  } else if (normalizedType == "knockout" &&
      ToLowerAsciiCopy(TrimCopy(body)).rfind("was ", 0) == 0) {
    line = speaker + " " + body;
  } else {
    line = speaker + ": " + body;
  }
  if (!listener.empty() && listener != speaker &&
      normalizedType != "action" && normalizedType != "infoaction" &&
      normalizedType != "trade" && normalizedType != "healing" &&
      normalizedType != "combat_start" && normalizedType != "combat_end" &&
      normalizedType != "limb_loss" && normalizedType != "horn_cut") {
    line += " (talking to: " + listener + ")";
  }
  return line;
}

void LogGameEvent(const std::string &type, const std::string &actor,
                  const std::string &actorFaction, const std::string &target,
                  const std::string &targetFaction,
                  const std::string &message, unsigned int actorSerial,
                  unsigned int targetSerial) {
  std::string normalizedType = ToLowerAsciiCopy(TrimCopy(type));
  if (normalizedType == "limb_loss" &&
      ShouldDropDuplicateLimbLossEvent(target, targetSerial, message)) {
    Log("EVENT_STREAM: dropped duplicate limb_loss target_serial=" +
        ToString((int)targetSerial) + " target=" + target +
        " message=" + message);
    return;
  }

  if (ShouldFilterChatEventLine(type, message)) {
    Log("EVENT_STREAM: dropped filtered chat line actor=" + actor +
        " message=" + message);
    return;
  }

  EnterCriticalSection(&g_eventMutex);
  GameEvent ev;
  ev.type = type;
  ev.actor = actor;
  ev.actorFaction = actorFaction;
  ev.target = target;
  ev.targetFaction = targetFaction;
  ev.message = message;
  ev.timestamp = GetTickCount();
  g_gameEvents.push_back(ev);
  if (g_gameEvents.size() > 100) {
    g_gameEvents.pop_front();
  }
  LeaveCriticalSection(&g_eventMutex);

  std::string logMsg = "[EVENT] " + type + ": " + actor;
  if (!actorFaction.empty() && actorFaction != "None")
    logMsg += " (" + actorFaction + ")";
  logMsg += " -> " + target;
  if (!targetFaction.empty() && targetFaction != "None")
    logMsg += " (" + targetFaction + ")";
  logMsg += " (" + message + ")";
  Log(logMsg);

  std::string eventType = NormalizeEventName(type);
  if (eventType.empty()) {
    return;
  }
  std::string eventData = BuildEventStreamData(eventType, actor, target, message);
  int peopleCount = 0;
  int anchorACount = 0;
  int anchorBCount = 0;
  int droppedByCap = 0;
  bool usedSecondAnchor = false;
  std::string peopleJson = "[]";
  if (normalizedType != "init") {
    peopleJson = BuildEventPeopleJson(GetWorldSafe(), eventType, actor, target, actorSerial,
                                      targetSerial, peopleCount, anchorACount,
                                      anchorBCount, droppedByCap,
                                      usedSecondAnchor);
  }
  int gameTs = ResolveCurrentGameTsForEvent();
  std::wstring endpoint = L"/StobeServer/stream.php?DATA=" +
                          ToWide(BuildStreamQueryData(eventType, eventData, gameTs));
  endpoint += L"&people=" + ToWide(UrlEncode(peopleJson));

  GameWorld *world = GetWorldSafe();
  Character *geoAnchor = FindCharacterBySerialForEvent(world, actorSerial);
  if (!geoAnchor && targetSerial != 0) {
    geoAnchor = FindCharacterBySerialForEvent(world, targetSerial);
  }
  if (!geoAnchor && !NormalizeEventName(actor).empty()) {
    geoAnchor = FindCharacterByNameForEvent(world, actor);
  }
  if (!geoAnchor && !NormalizeEventName(target).empty()) {
    geoAnchor = FindCharacterByNameForEvent(world, target);
  }
  AppendEventGeoQueryFromCharacter(endpoint, geoAnchor);

  AsyncPostToStobeSerial(endpoint, "");
  Log("EVENT_STREAM: queued type=" + eventType +
      " gamets=" + ToString(gameTs) +
      " people_count=" + ToString(peopleCount) +
      " anchor_a_count=" + ToString(anchorACount) +
      " anchor_b_enabled=" + std::string(usedSecondAnchor ? "1" : "0") +
      " anchor_b_count=" + ToString(anchorBCount) +
      " dropped_by_cap=" + ToString(droppedByCap) +
      " actor_serial=" + ToString((int)actorSerial) +
      " target_serial=" + ToString((int)targetSerial) +
      " data=" + eventData);
}
float ResolveDialogueGameSpeedMultiplier(GameWorld *world) {
  if (!g_speedDialogue) {
    return 1.0f;
  }

  float speed = 1.0f;
  if (world) {
    try {
      speed = world->getFrameSpeedMultiplier();
    } catch (...) {
      speed = 1.0f;
    }
  }
  if (!(speed > 0.0f)) {
    speed = 1.0f;
  }
  if (speed < 1.0f) {
    speed = 1.0f;
  } else if (speed > 3.0f) {
    speed = 3.0f;
  }
  return speed;
}
void SleepIfPaused(DWORD ms) {
  if (ms == 0) {
    return;
  }

  double remainingScaledMs = static_cast<double>(ms);
  DWORD lastTick = GetTickCount();

  while (remainingScaledMs > 0.0) {
    GameWorld *world = GetWorldSafe();
    bool paused = false;
    if (world) {
      try {
        paused = world->isPaused();
      } catch (...) {
        paused = false;
      }
    }

    float speed = paused ? 1.0f : ResolveDialogueGameSpeedMultiplier(world);

    DWORD nowTick = GetTickCount();
    DWORD elapsed = nowTick - lastTick;
    lastTick = nowTick;

    if (!paused && elapsed > 0) {
      remainingScaledMs -=
          static_cast<double>(elapsed) * static_cast<double>(speed);
      if (remainingScaledMs <= 0.0) {
        break;
      }
    }

    DWORD sleepSliceMs = 100;
    if (!paused) {
      double remainingRealMs = remainingScaledMs /
                               static_cast<double>(speed > 0.0f ? speed : 1.0f);
      if (remainingRealMs <= 2.0) {
        sleepSliceMs = 1;
      } else if (remainingRealMs <= 8.0) {
        sleepSliceMs = 2;
      } else if (remainingRealMs <= 20.0) {
        sleepSliceMs = 5;
      } else if (remainingRealMs <= 50.0) {
        sleepSliceMs = 10;
      } else {
        sleepSliceMs = 25;
      }
    }

    Sleep(sleepSliceMs);
  }
}
