// ???? AGENT PROTOCOL: Before editing this file, you MUST read PROJECT_CONTEXT.md
// ???? This project has strict threading and memory safety rules.
#include <string>
#include <vector>
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>

#include "Comm.h"
#include "Context.h"
// ???? AGENT PROTOCOL: Before editing this file, you MUST read PROJECT_CONTEXT.md
// ???? Kenshi engine writes MUST occur on the main thread inside hooks.
#include <core/Functions.h>
#include "Functions.h"
#include "AudioPlayback.h"
#include "Globals.h"
#include "Utils.h"

#include <kenshi/CharStats.h>
#include <kenshi/Character.h>
#include <kenshi/CharMovement.h>
#include <kenshi/Dialogue.h>
#include <kenshi/Faction.h>
#include <kenshi/GameData.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Inventory.h>
#include <kenshi/Item.h>
#include <kenshi/Kenshi.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/Platoon.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/util/hand.h>
#include <kenshi/Damages.h>

#include <kenshi/RaceData.h>
#include <kenshi/RootObject.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/WorldEventStateQuery.h>

// Helper to safely get faction names for logging
inline std::string SafeFaction(RootObjectBase *obj) {
  if (!obj || (uintptr_t)obj < 0x1000)
    return "None";
  try {
    Faction *f = obj->getFaction();
    if (f && (uintptr_t)f > 0x1000)
      return f->getName();
  } catch (...) {
  }
  return "None";
}

void (*playerUpdate_orig)(PlayerInterface *) = nullptr;
void (*attackingYou_orig)(Character *, Character *, bool, bool) = nullptr;
void (*applyDamage_orig)(MedicalSystem::HealthPartStatus *,
                         const Damages &) = nullptr;
bool (*applyFirstAid_orig)(MedicalSystem *, float, Item *, float,
                           Character *) = nullptr;
Item *(*buyItem_orig)(Inventory *, Item *, RootObject *) = nullptr;

// New World Event Hooks
void (*declareDead_orig)(Character *) = nullptr;
void (*setPrisonMode_orig)(Character *, bool, UseableStuff *) = nullptr;
void (*setProneState_orig)(Character *, ProneState) = nullptr;
bool (*isItOkForMeToLoot_orig)(Character *, RootObject *, Item *) = nullptr;
void (*setChainedMode_orig)(Character *, bool, const hand &) = nullptr;
void (*sayALine_orig)(Character *, const std::string &, bool) = nullptr;
void (*characterSay_orig)(Character *, const std::string &) = nullptr;
bool (*dialogueSayLine_orig)(Dialogue *, DialogLineData *) = nullptr;
void (*dialogueSayText_orig)(Dialogue *, const std::string &, DialogLineData *) =
    nullptr;
void (*dialogueReplyClickedInt_orig)(Dialogue *, int) = nullptr;
void (*dialogueReplyClickedString_orig)(Dialogue *, const std::string &) = nullptr;

#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_ComboBox.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_InputManager.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Window.h>

#include "ChatUI.h"

// --- Main Hook Core ---

static std::string ToLowerAsciiCopy(const std::string &value) {
  std::string lowered = value;
  for (size_t i = 0; i < lowered.size(); ++i) {
    lowered[i] = (char)std::tolower((unsigned char)lowered[i]);
  }
  return lowered;
}

static bool EndsWithAsciiInsensitive(const std::string &value,
                                     const std::string &suffix) {
  if (value.length() < suffix.length()) {
    return false;
  }
  std::string left = ToLowerAsciiCopy(value);
  std::string right = ToLowerAsciiCopy(suffix);
  return left.compare(left.length() - right.length(), right.length(), right) ==
         0;
}

static bool ContainsAsciiInsensitive(const std::string &value,
                                     const std::string &needle) {
  if (needle.empty()) {
    return false;
  }
  return ToLowerAsciiCopy(value).find(ToLowerAsciiCopy(needle)) !=
         std::string::npos;
}

static bool IsLikelyTradeDialogueReply(const std::string &line) {
  std::string trimmed = line;
  size_t first = trimmed.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return false;
  }
  size_t last = trimmed.find_last_not_of(" \t\r\n");
  trimmed = trimmed.substr(first, last - first + 1);
  std::string lowered = ToLowerAsciiCopy(trimmed);
  if (lowered.empty()) {
    return false;
  }
  if (lowered.find("trade") != std::string::npos ||
      lowered.find("shop") != std::string::npos ||
      lowered.find("buy") != std::string::npos ||
      lowered.find("sell") != std::string::npos ||
      lowered.find("wares") != std::string::npos ||
      lowered.find("goods") != std::string::npos ||
      lowered.find("show me what you have") != std::string::npos ||
      lowered.find("show me your goods") != std::string::npos) {
    return true;
  }
  return false;
}

static bool DirectoryExists(const std::string &path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static std::string GetKenshiModsDirectoryPath(bool ensureExists) {
  char path[MAX_PATH];
  GetModuleFileNameA(NULL, path, MAX_PATH);
  std::string exeDir = path;
  size_t lastBackslash = exeDir.find_last_of("\\/");
  if (lastBackslash != std::string::npos) {
    exeDir = exeDir.substr(0, lastBackslash);
  }
  std::string modsDir = exeDir + "\\mods";
  if (ensureExists) {
    CreateDirectoryA(modsDir.c_str(), NULL);
  }
  return modsDir;
}

static std::vector<std::string> GetStobeImportDirectories() {
  std::vector<std::string> directories;
  std::set<std::string> seenLower;

  std::string modsDir = GetKenshiModsDirectoryPath(true);
  std::string rootStobeDir = modsDir + "\\Stobe";
  CreateDirectoryA(rootStobeDir.c_str(), NULL);
  if (DirectoryExists(rootStobeDir)) {
    std::string key = ToLowerAsciiCopy(rootStobeDir);
    if (seenLower.insert(key).second) {
      directories.push_back(rootStobeDir);
    }
  }

  std::string wildcardPath = modsDir + "\\*";
  WIN32_FIND_DATAA findData;
  HANDLE handle = FindFirstFileA(wildcardPath.c_str(), &findData);
  if (handle == INVALID_HANDLE_VALUE) {
    return directories;
  }

  do {
    if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      continue;
    }
    std::string dirName = findData.cFileName;
    if (dirName == "." || dirName == "..") {
      continue;
    }

    std::string nestedStobe = modsDir + "\\" + dirName + "\\Stobe";
    if (!DirectoryExists(nestedStobe)) {
      continue;
    }

    std::string key = ToLowerAsciiCopy(nestedStobe);
    if (seenLower.insert(key).second) {
      directories.push_back(nestedStobe);
    }
  } while (FindNextFileA(handle, &findData));

  FindClose(handle);
  return directories;
}

static std::vector<std::string> FindCsvFilesInStobeModFolder(
    const std::string &modFolder) {
  std::vector<std::string> files;
  std::string wildcardPath = modFolder + "\\*.csv";
  WIN32_FIND_DATAA findData;
  HANDLE handle = FindFirstFileA(wildcardPath.c_str(), &findData);
  if (handle == INVALID_HANDLE_VALUE) {
    return files;
  }

  do {
    if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    files.push_back(modFolder + "\\" + findData.cFileName);
  } while (FindNextFileA(handle, &findData));
  FindClose(handle);

  std::sort(files.begin(), files.end());
  return files;
}

static std::string BasenameFromPath(const std::string &fullPath) {
  size_t pos = fullPath.find_last_of("\\/");
  if (pos == std::string::npos) {
    return fullPath;
  }
  return fullPath.substr(pos + 1);
}

static bool ReadSmallFileBinary(const std::string &fullPath, size_t maxBytes,
                                std::string &outData) {
  outData.clear();
  std::ifstream in(fullPath.c_str(), std::ios::binary);
  if (!in.is_open()) {
    return false;
  }

  in.seekg(0, std::ios::end);
  std::streamoff fileSize = in.tellg();
  in.seekg(0, std::ios::beg);
  if (fileSize < 0) {
    return false;
  }
  if (static_cast<size_t>(fileSize) > maxBytes) {
    return false;
  }
  if (fileSize == 0) {
    return true;
  }

  outData.resize(static_cast<size_t>(fileSize));
  in.read(&outData[0], fileSize);
  return in.good() || in.eof();
}

static std::string ExtractFirstCsvLine(const std::string &csvData) {
  size_t end = csvData.find_first_of("\r\n");
  if (end == std::string::npos) {
    return csvData;
  }
  return csvData.substr(0, end);
}

static std::string DetectCsvImportType(const std::string &filename,
                                       const std::string &csvData) {
  std::string lowerName = ToLowerAsciiCopy(filename);
  std::string firstLine = ToLowerAsciiCopy(ExtractFirstCsvLine(csvData));

  if (ContainsAsciiInsensitive(lowerName, "world_knowledge") ||
      ContainsAsciiInsensitive(lowerName, "worldknowledge") ||
      ContainsAsciiInsensitive(lowerName, "worldstate") ||
      ContainsAsciiInsensitive(lowerName, "oghma")) {
    return "world_knowledge_import";
  }
  if (ContainsAsciiInsensitive(lowerName, "bio_unique")) {
    return "bio_unique_import";
  }
  if (ContainsAsciiInsensitive(lowerName, "bio_random") ||
      EndsWithAsciiInsensitive(lowerName, "_bios.csv")) {
    return "bio_random_import";
  }
  if (ContainsAsciiInsensitive(lowerName, "bio_token") ||
      ContainsAsciiInsensitive(lowerName, "rename_token")) {
    return "bio_token_import";
  }
  if (ContainsAsciiInsensitive(lowerName, "description")) {
    return "description_import";
  }

  if (firstLine.find("topic") != std::string::npos &&
      (firstLine.find("topic_desc") != std::string::npos ||
       firstLine.find("topic_desc_basic") != std::string::npos)) {
    return "world_knowledge_import";
  }
  if (firstLine.find("token") != std::string::npos) {
    return "bio_token_import";
  }
  if (firstLine.find("stringid") != std::string::npos &&
      firstLine.find("name") != std::string::npos &&
      firstLine.find("description") != std::string::npos) {
    if (ContainsAsciiInsensitive(lowerName, "unique")) {
      return "bio_unique_import";
    }
    if (ContainsAsciiInsensitive(lowerName, "bio")) {
      return "bio_random_import";
    }
    return "description_import";
  }

  return "";
}

static std::string LogSnippet(const std::string &value, size_t maxLen) {
  if (value.length() <= maxLen) {
    return value;
  }
  return value.substr(0, maxLen) + "...";
}

DWORD WINAPI CsvImportStartupThread(LPVOID) {
  // Wait for StobeServer startup/discovery before posting uploads.
  Sleep(10000);

  std::vector<std::string> importDirs = GetStobeImportDirectories();
  std::vector<std::string> csvFiles;
  std::set<std::string> seenFiles;
  for (size_t i = 0; i < importDirs.size(); ++i) {
    std::vector<std::string> dirFiles =
        FindCsvFilesInStobeModFolder(importDirs[i]);
    for (size_t f = 0; f < dirFiles.size(); ++f) {
      std::string key = ToLowerAsciiCopy(dirFiles[f]);
      if (seenFiles.insert(key).second) {
        csvFiles.push_back(dirFiles[f]);
      }
    }
  }
  if (csvFiles.empty()) {
    Log("CSV_IMPORT: no CSV files detected in configured Stobe import folders.");
    return 0;
  }

  Log("CSV_IMPORT: scanning " + ToString((int)csvFiles.size()) +
      " CSV file(s) across " + ToString((int)importDirs.size()) +
      " Stobe import folder(s)");

  const size_t maxCsvBytes = 10u * 1024u * 1024u;
  int uploadedCount = 0;
  int skippedCount = 0;
  int failedCount = 0;

  for (size_t i = 0; i < csvFiles.size(); ++i) {
    const std::string fullPath = csvFiles[i];
    const std::string filename = BasenameFromPath(fullPath);

    std::string csvData;
    if (!ReadSmallFileBinary(fullPath, maxCsvBytes, csvData)) {
      failedCount++;
      Log("CSV_IMPORT: failed to read file (missing or too large): " +
          filename);
      continue;
    }
    if (csvData.empty()) {
      skippedCount++;
      Log("CSV_IMPORT: skipped empty file " + filename);
      continue;
    }

    std::string importType = DetectCsvImportType(filename, csvData);
    if (importType.empty()) {
      skippedCount++;
      Log("CSV_IMPORT: skipped unknown CSV pattern " + filename);
      continue;
    }

    std::string response = "";
    bool ok = false;
    for (int attempt = 1; attempt <= 3; ++attempt) {
      response = UploadCsvImportToStobe(csvData, filename, importType);
      ok = (!response.empty() &&
            response.find("\"success\":true") != std::string::npos);
      if (ok) {
        break;
      }
      if (attempt < 3) {
        Sleep(2000);
      }
    }
    if (ok) {
      uploadedCount++;
      Log("CSV_IMPORT: uploaded file=" + filename + " type=" + importType);
    } else {
      failedCount++;
      Log("CSV_IMPORT: upload failed file=" + filename + " type=" + importType +
          " response=" + LogSnippet(response, 220));
    }
  }

  Log("CSV_IMPORT: completed uploaded=" + ToString(uploadedCount) +
      " skipped=" + ToString(skippedCount) +
      " failed=" + ToString(failedCount));
  return 0;
}

static bool IsAnimalCharacterSafe(Character *npc) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return false;
  }
  try {
    return npc->isAnimal() != 0;
  } catch (...) {
    return false;
  }
}

static bool ShouldProcessAnimalCharacter(Character *npc) {
  if (!IsAnimalCharacterSafe(npc)) {
    return true;
  }
  if (!g_enableAnimalTalks) {
    return false;
  }
  unsigned int serial = 0;
  try {
    serial = npc->getHandle().serial;
  } catch (...) {
    serial = 0;
  }
  if (serial == 0) {
    return false;
  }
  return IsAnimalActivated(serial);
}

static void QueueIdentityRenameCandidate(Character *other,
                                         const std::string &reason) {
  if (!other || (uintptr_t)other < 0x1000) {
    return;
  }

  unsigned int serial = 0;
  try {
    serial = other->getHandle().serial;
  } catch (...) {
    serial = 0;
  }
  if (serial == 0) {
    return;
  }

  if (!ShouldProcessAnimalCharacter(other)) {
    return;
  }

  std::string otherName;
  try {
    otherName = other->getName();
  } catch (...) {
    return;
  }
  if (otherName.empty()) {
    return;
  }

  EnterCriticalSection(&g_nameCheckMutex);
  bool alreadyDone = (g_renamedSerials.count(serial) > 0);
  if (alreadyDone) {
    LeaveCriticalSection(&g_nameCheckMutex);
    return;
  }
  for (uint32_t q = 0; q < g_nameCheckQueue.size(); ++q) {
    if (g_nameCheckQueue[q].serial == serial) {
      LeaveCriticalSection(&g_nameCheckMutex);
      return;
    }
  }
  LeaveCriticalSection(&g_nameCheckMutex);

  std::string otherGender = other->isFemale() ? "Female" : "Male";
  RaceData *otherRace = other->getRace() ? other->getRace() : other->myRace;
  std::string otherRaceName = "Human";
  if (otherRace && (uintptr_t)otherRace > 0x1000 && otherRace->data &&
      !otherRace->data->name.empty()) {
    otherRaceName = otherRace->data->name;
  }

  std::string otherFactionName;
  if (g_originFactions.count(serial)) {
    otherFactionName = g_originFactions[serial];
  }
  if (otherFactionName.empty()) {
    Faction *otherFaction = other->getFaction() ? other->getFaction() : other->owner;
    if (otherFaction && (uintptr_t)otherFaction > 0x1000) {
      std::string fn = otherFaction->getName();
      if ((fn.empty() || fn == "Unknown") && otherFaction->data &&
          !otherFaction->data->name.empty()) {
        fn = otherFaction->data->name;
      }
      if (fn != "Unknown" && fn != "Neutral" && fn != "None") {
        otherFactionName = fn;
      }
    }
  }

  NameCheckItem item;
  item.serial = serial;
  item.name = otherName;
  item.gender = otherGender;
  item.race = otherRaceName;
  item.faction = otherFactionName;
  item.contextJson = BuildIdentityBootstrapContext(other);

  EnterCriticalSection(&g_nameCheckMutex);
  g_nameCheckQueue.push_back(item);
  LeaveCriticalSection(&g_nameCheckMutex);

  Log("NAME_ASSIGN: queued identity check reason=" + reason + " serial=" +
      ToString(serial) + " name=" + otherName);
}

static std::string TrimCopy(const std::string &value) {
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

static bool LooksLikeDialogueTemplateToken(const std::string &value) {
  std::string trimmed = TrimCopy(value);
  if (trimmed.empty()) {
    return false;
  }

  std::string lower = ToLowerAsciiCopy(trimmed);
  if (lower.find("/factioncomment/") != std::string::npos ||
      lower.find("/racecomment/") != std::string::npos ||
      lower.find("/towncomment/") != std::string::npos ||
      lower.find("/genericcomment/") != std::string::npos ||
      lower.find("/personalitycomment/") != std::string::npos) {
    return true;
  }

  if (trimmed.front() != '/' || trimmed.back() != '/') {
    return false;
  }

  bool hasAlpha = false;
  for (size_t i = 0; i < trimmed.size(); ++i) {
    unsigned char ch = static_cast<unsigned char>(trimmed[i]);
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
      hasAlpha = true;
    }
    bool allowed =
        (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
        (ch >= 'A' && ch <= 'Z') || ch == '/' || ch == '_' || ch == '-' ||
        ch == ':' || ch == ' ' || ch == '\t';
    if (!allowed) {
      return false;
    }
  }

  return hasAlpha;
}

static std::string SanitizeCapturedDialogueLine(std::string value) {
  value = TrimCopy(value);
  if (value.empty()) {
    return "";
  }

  // Drop embedded NUL tails if engine-provided text contains binary remnants.
  size_t nulPos = value.find('\0');
  if (nulPos != std::string::npos) {
    value = value.substr(0, nulPos);
    value = TrimCopy(value);
  }

  // Strip trailing " ??" corruption while preserving intentional single '?' usage.
  size_t suffixStart = value.find_last_not_of('?');
  if (suffixStart != std::string::npos && suffixStart + 1 < value.size()) {
    size_t qCount = value.size() - (suffixStart + 1);
    if (qCount >= 2) {
      std::string prefix = value.substr(0, suffixStart + 1);
      if (!prefix.empty() &&
          std::isspace(static_cast<unsigned char>(prefix[prefix.size() - 1]))) {
        value = TrimCopy(prefix);
      }
    }
  }

  // Strip garbage suffixes such as "...night6??х??????Ѽ?..."
  // Keep this conservative: only trim when a trailing suspect run is long.
  if (!value.empty()) {
    size_t scan = value.size();
    size_t suffixRun = 0;
    size_t hardSuspectCount = 0;
    bool seenSuspect = false;

    while (scan > 0) {
      const unsigned char ch = static_cast<unsigned char>(value[scan - 1]);
      const bool isControl = (ch < 0x20 && ch != '\t' && ch != '\r' && ch != '\n');
      const bool isNonAscii = (ch >= 0x80);
      const bool isQuestion = (ch == '?');
      const bool isSoftJoiner =
          std::isspace(ch) || std::isdigit(ch) || ch == '=' || ch == '+' ||
          ch == '-' || ch == '_' || ch == '.' || ch == ',' || ch == ':' ||
          ch == ';' || ch == '!' || ch == ')' || ch == ']' || ch == '(' ||
          ch == '[' || ch == '|' || ch == '\\' || ch == '/';

      if (isControl || isNonAscii || isQuestion) {
        seenSuspect = true;
        ++suffixRun;
        if (isControl || isNonAscii) {
          ++hardSuspectCount;
        }
        --scan;
        continue;
      }

      if (seenSuspect && isSoftJoiner) {
        ++suffixRun;
        --scan;
        continue;
      }

      break;
    }

    if (seenSuspect && suffixRun >= 6 && hardSuspectCount >= 1 &&
        scan < value.size()) {
      value = TrimCopy(value.substr(0, scan));
    }
  }

  // Strip trailing numeric corruption like "...7" or ")7".
  if (!value.empty() &&
      std::isdigit(static_cast<unsigned char>(value[value.size() - 1]))) {
    size_t digitStart = value.size();
    while (digitStart > 0 &&
           std::isdigit(static_cast<unsigned char>(value[digitStart - 1]))) {
      --digitStart;
    }
    if (digitStart < value.size() && digitStart > 0) {
      char prev = value[digitStart - 1];
      if (prev == '.' || prev == '!' || prev == '?' || prev == ')' ||
          prev == ']') {
        value = TrimCopy(value.substr(0, digitStart));
      }
    }
  }

  return TrimCopy(value);
}

static bool HasActiveSpeechBubbleSafe(Character *npc);
static std::string ReadNpcSpeechLineSafe(Character *npc);
static bool IsNpcInSpeechFlowBySerial(unsigned int serial);

static bool ShouldDropDuplicateNpcAction(unsigned int actorSerial,
                                         const std::string &actionCommand,
                                         const std::string &actionArgument) {
  static std::map<std::string, DWORD> s_recentActions;
  const DWORD nowTick = GetTickCount();
  const DWORD windowMs = 2500;
  const DWORD pruneMs = 12000;

  for (auto it = s_recentActions.begin(); it != s_recentActions.end();) {
    if ((nowTick - it->second) > pruneMs) {
      it = s_recentActions.erase(it);
    } else {
      ++it;
    }
  }

  std::string normalizedArg = ToLowerAsciiCopy(TrimCopy(actionArgument));
  std::string normalizedCmd = ToLowerAsciiCopy(TrimCopy(actionCommand));
  std::string dedupeKey = ToString((int)GetChatInterruptGeneration()) + "|" +
                          ToString((int)actorSerial) + "|" + normalizedCmd +
                          "|" + normalizedArg;

  auto existing = s_recentActions.find(dedupeKey);
  if (existing != s_recentActions.end() &&
      (nowTick - existing->second) <= windowMs) {
    existing->second = nowTick;
    return true;
  }

  s_recentActions[dedupeKey] = nowTick;
  return false;
}

static bool ShouldDropDuplicateNonAiDialogue(unsigned int actorSerial,
                                             bool isPlayerLine,
                                             const std::string &line) {
  static std::map<std::string, DWORD> s_recentDialogueLines;
  const DWORD nowTick = GetTickCount();
  const DWORD windowMs = 1800;
  const DWORD pruneMs = 20000;

  for (auto it = s_recentDialogueLines.begin();
       it != s_recentDialogueLines.end();) {
    if ((nowTick - it->second) > pruneMs) {
      it = s_recentDialogueLines.erase(it);
    } else {
      ++it;
    }
  }

  std::string normalizedLine = ToLowerAsciiCopy(TrimCopy(line));
  if (normalizedLine.length() > 240) {
    normalizedLine = normalizedLine.substr(0, 240);
  }

  std::string dedupeKey = ToString((int)GetChatInterruptGeneration()) + "|" +
                          (isPlayerLine ? "p" : "n") + "|" +
                          ToString((unsigned int)actorSerial) + "|" +
                          normalizedLine;
  auto existing = s_recentDialogueLines.find(dedupeKey);
  if (existing != s_recentDialogueLines.end() &&
      (nowTick - existing->second) <= windowMs) {
    existing->second = nowTick;
    return true;
  }

  s_recentDialogueLines[dedupeKey] = nowTick;
  return false;
}

static std::string ExtractTrailingTtsHash(std::string &message) {
  std::string trimmed = TrimCopy(message);
  size_t markerPos = trimmed.rfind("[TTSHASH:");
  if (markerPos == std::string::npos) {
    message = trimmed;
    return "";
  }

  size_t endPos = trimmed.find(']', markerPos);
  if (endPos == std::string::npos || endPos != trimmed.size() - 1) {
    message = trimmed;
    return "";
  }

  size_t valuePos = markerPos + 9;
  std::string hash = TrimCopy(trimmed.substr(valuePos, endPos - valuePos));
  if (hash.empty()) {
    message = trimmed;
    return "";
  }

  for (size_t i = 0; i < hash.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(hash[i]);
    bool isHex =
        (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    if (!isHex) {
      message = trimmed;
      return "";
    }
    hash[i] = static_cast<char>(tolower(ch));
  }

  trimmed.erase(markerPos);
  message = TrimCopy(trimmed);
  return hash;
}

static int ExtractTrailingTtsDurationMs(std::string &message) {
  std::string trimmed = TrimCopy(message);
  size_t markerPos = trimmed.rfind("[TTSDUR:");
  if (markerPos == std::string::npos) {
    message = trimmed;
    return 0;
  }

  size_t endPos = trimmed.find(']', markerPos);
  if (endPos == std::string::npos || endPos != trimmed.size() - 1) {
    message = trimmed;
    return 0;
  }

  size_t valuePos = markerPos + 8;
  std::string durationStr = TrimCopy(trimmed.substr(valuePos, endPos - valuePos));
  if (durationStr.empty()) {
    message = trimmed;
    return 0;
  }

  for (size_t i = 0; i < durationStr.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(durationStr[i]);
    if (ch < '0' || ch > '9') {
      message = trimmed;
      return 0;
    }
  }

  int durationMs = atoi(durationStr.c_str());
  if (durationMs <= 0) {
    message = trimmed;
    return 0;
  }
  if (durationMs > 600000) {
    durationMs = 600000;
  }

  trimmed.erase(markerPos);
  message = TrimCopy(trimmed);
  return durationMs;
}

static std::string ExtractDialogueMessageFromStructuredText(
    const std::string &rawText) {
  std::string candidate = TrimCopy(rawText);
  if (candidate.empty()) {
    return "";
  }

  if (candidate.find("```") == 0) {
    size_t firstNewline = candidate.find('\n');
    if (firstNewline != std::string::npos) {
      candidate = candidate.substr(firstNewline + 1);
    }
    size_t lastFence = candidate.rfind("```");
    if (lastFence != std::string::npos) {
      candidate = candidate.substr(0, lastFence);
    }
    candidate = TrimCopy(candidate);
  }

  if (candidate.empty() || candidate[0] != '{') {
    return "";
  }

  auto tryExtractCoreMessage = [](const std::string &jsonPayload) -> std::string {
    std::string extracted = TrimCopy(JsonReadField(jsonPayload, "message"));
    if (extracted.empty()) {
      extracted = TrimCopy(JsonReadField(jsonPayload, "text"));
    }
    if (extracted.empty()) {
      extracted = TrimCopy(JsonReadField(jsonPayload, "content"));
    }
    return extracted;
  };

  std::string directMessage = tryExtractCoreMessage(candidate);
  if (!directMessage.empty()) {
    return directMessage;
  }

  const char *wrapperKeys[] = {"response", "data", "output", "result",
                               "payload"};
  for (size_t i = 0; i < sizeof(wrapperKeys) / sizeof(wrapperKeys[0]); ++i) {
    std::string nested = TrimCopy(JsonReadField(candidate, wrapperKeys[i]));
    if (nested.empty()) {
      continue;
    }
    std::string nestedMessage = tryExtractCoreMessage(nested);
    if (!nestedMessage.empty()) {
      return nestedMessage;
    }
  }

  return "";
}

struct InventorySyncState {
  std::string lastHash;
  DWORD lastSentTick;
  DWORD lastSeenTick;
  bool hasSent;

  InventorySyncState()
      : lastHash(""), lastSentTick(0), lastSeenTick(0), hasSent(false) {}
};

struct InventoryEventSnapshot {
  std::map<std::string, int> countsByKey;
  std::map<std::string, int> stolenByKey;
  std::map<std::string, std::string> displayNameByKey;
  int totalCount;

  InventoryEventSnapshot() : totalCount(0) {}
};

struct NpcWorldEventState {
  bool initialized;
  bool dead;
  bool unconscious;
  bool enslaved;
  bool speechActive;
  bool leftArmPresent;
  bool rightArmPresent;
  bool leftLegPresent;
  bool rightLegPresent;
  int leftArmState;
  int rightArmState;
  int leftLegState;
  int rightLegState;
  int lockpickingSkill;
  std::string lastSpeechLine;
  InventoryEventSnapshot inventory;
  DWORD lastSeenTick;

  NpcWorldEventState()
      : initialized(false), dead(false), unconscious(false), enslaved(false),
        speechActive(false), leftArmPresent(true), rightArmPresent(true), leftLegPresent(true),
        rightLegPresent(true),
        leftArmState((int)LIMB_ORIGINAL), rightArmState((int)LIMB_ORIGINAL),
        leftLegState((int)LIMB_ORIGINAL), rightLegState((int)LIMB_ORIGINAL),
        lockpickingSkill(0), lastSpeechLine(""),
        lastSeenTick(0) {}
};

static std::map<unsigned int, InventorySyncState> g_inventorySyncStateBySerial;
static DWORD g_lastInventorySweepTick = 0;
static const DWORD kInventorySweepIntervalMs = 1500;
static const DWORD kInventoryMinResendMs = 1200;
static const size_t kInventorySweepCandidateLimit = 24;
static const DWORD kInventoryStateRetentionMs = 15 * 60 * 1000;
static std::map<unsigned int, NpcWorldEventState> g_npcWorldEventStateBySerial;
static DWORD g_lastNpcWorldEventSweepTick = 0;
static const DWORD kNpcWorldEventSweepIntervalMs = 900;
static const size_t kNpcWorldEventCandidateLimit = 96;
static const DWORD kNpcWorldEventStateRetentionMs = 10 * 60 * 1000;
static DWORD g_lastInfoNpcTelemetryCheckTick = 0;
static DWORD g_lastInfoNpcTelemetrySentTick = 0;
static std::string g_lastInfoNpcTelemetryDigest = "";
static const DWORD kInfoNpcTelemetryCheckIntervalMs = 8000;
static const DWORD kInfoNpcTelemetryResendIntervalMs = 90000;
static DWORD g_lastInfoLocTelemetryCheckTick = 0;
static DWORD g_lastInfoLocTelemetrySentTick = 0;
static std::string g_lastInfoLocTelemetryDigest = "";
static const DWORD kInfoLocTelemetryCheckIntervalMs = 5000;
static const DWORD kInfoLocTelemetryResendIntervalMs = 120000;
static bool g_playerCatsSyncHasValue = false;
static int g_playerCatsSyncLastValue = 0;
static DWORD g_playerCatsSyncLastSentTick = 0;
static const DWORD kPlayerCatsResendIntervalMs = 5 * 60 * 1000;
static const char *kStobePluginVersion = "0.5.0";
static bool g_pluginVersionSyncHasValue = false;
static std::string g_pluginVersionSyncLastValue = "";
static DWORD g_pluginVersionSyncLastSentTick = 0;
static const DWORD kPluginVersionResendIntervalMs = 10 * 60 * 1000;
static bool g_dynamicProfileIntervalSyncHasValue = false;
static int g_dynamicProfileIntervalSyncLastValue = 0;
static DWORD g_dynamicProfileIntervalSyncLastSentTick = 0;
static const DWORD kDynamicProfileIntervalResendIntervalMs = 5 * 60 * 1000;
static bool g_playerSquadsSyncHasValue = false;
static std::string g_playerSquadsSyncLastDigest = "";
static DWORD g_playerSquadsSyncLastSentTick = 0;
static std::set<std::string> g_playerSquadsLastKeys;
static const DWORD kPlayerSquadsResendIntervalMs = 60 * 1000;

static std::string ShortInventoryHashForLog(const std::string &hash) {
  if (hash.length() <= 12) {
    return hash;
  }
  return hash.substr(0, 12);
}

static void ResetPlayerCatsSyncState() {
  EnterCriticalSection(&g_stateMutex);
  g_playerCatsSyncHasValue = false;
  g_playerCatsSyncLastValue = 0;
  g_playerCatsSyncLastSentTick = 0;
  LeaveCriticalSection(&g_stateMutex);
}

static void ResetDynamicProfileIntervalSyncState() {
  EnterCriticalSection(&g_stateMutex);
  g_dynamicProfileIntervalSyncHasValue = false;
  g_dynamicProfileIntervalSyncLastValue = 0;
  g_dynamicProfileIntervalSyncLastSentTick = 0;
  LeaveCriticalSection(&g_stateMutex);
}

static void ResetPlayerSquadsSyncState() {
  EnterCriticalSection(&g_stateMutex);
  g_playerSquadsSyncHasValue = false;
  g_playerSquadsSyncLastDigest = "";
  g_playerSquadsSyncLastSentTick = 0;
  g_playerSquadsLastKeys.clear();
  LeaveCriticalSection(&g_stateMutex);
}

static std::string BuildJsonStringArray(const std::vector<std::string> &values) {
  std::string json = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      json += ",";
    }
    json += "\"" + EscapeJSON(values[i]) + "\"";
  }
  json += "]";
  return json;
}

static std::string ResolvePlayerSquadName(Character *member) {
  if (!member || (uintptr_t)member < 0x1000) {
    return "Unknown Squad";
  }

  ActivePlatoon *activePlatoon = nullptr;
  try {
    activePlatoon = member->getPlatoon();
  } catch (...) {
    activePlatoon = nullptr;
  }

  std::string squadName = "";
  if (activePlatoon && (uintptr_t)activePlatoon > 0x1000) {
    try {
      squadName = TrimCopy(activePlatoon->getName());
    } catch (...) {
      squadName = "";
    }
    if (squadName.empty() && activePlatoon->me &&
        (uintptr_t)activePlatoon->me > 0x1000) {
      try {
        squadName = TrimCopy(activePlatoon->me->getPlatoonStringID());
      } catch (...) {
      }
      if (squadName.empty()) {
        squadName = TrimCopy(activePlatoon->me->stringID);
      }
    }
  }

  if (squadName.empty()) {
    squadName = "Unknown Squad";
  }
  return squadName;
}

static bool SyncPlayerSquadsToConfOpts(GameWorld *world, bool force,
                                       const std::string &reason) {
  if (!world || !world->player) {
    return false;
  }

  std::map<std::string, std::set<std::string> > squadMembers;
  for (uint32_t i = 0; i < world->player->playerCharacters.size(); ++i) {
    Character *member = world->player->playerCharacters[i];
    if (!member || (uintptr_t)member < 0x1000) {
      continue;
    }

    std::string memberName = "";
    try {
      memberName = TrimCopy(member->getName());
    } catch (...) {
      memberName = "";
    }
    if (memberName.empty()) {
      memberName = "Unknown Member";
    }

    std::string squadName = ResolvePlayerSquadName(member);
    squadMembers[squadName].insert(memberName);
  }

  std::map<std::string, std::string> squadPayloads;
  std::set<std::string> currentSquadKeys;
  std::string digest = "";
  for (std::map<std::string, std::set<std::string> >::const_iterator it =
           squadMembers.begin();
       it != squadMembers.end(); ++it) {
    const std::string &squadName = it->first;
    std::vector<std::string> members(it->second.begin(), it->second.end());
    std::sort(members.begin(), members.end());
    std::string memberJson = BuildJsonStringArray(members);
    squadPayloads[squadName] = memberJson;
    currentSquadKeys.insert(squadName);
    digest += squadName + "=" + memberJson + ";";
  }

  DWORD nowTick = GetTickCount();
  bool shouldSend = false;
  bool changed = false;
  DWORD sinceLastSent = 0;
  std::vector<std::string> staleSquadKeys;
  EnterCriticalSection(&g_stateMutex);
  changed =
      (!g_playerSquadsSyncHasValue || digest != g_playerSquadsSyncLastDigest);
  sinceLastSent = g_playerSquadsSyncHasValue
                      ? (nowTick - g_playerSquadsSyncLastSentTick)
                      : 0;
  if (force || !g_playerSquadsSyncHasValue || changed ||
      sinceLastSent >= kPlayerSquadsResendIntervalMs) {
    shouldSend = true;
    for (std::set<std::string>::const_iterator it =
             g_playerSquadsLastKeys.begin();
         it != g_playerSquadsLastKeys.end(); ++it) {
      if (currentSquadKeys.count(*it) == 0) {
        staleSquadKeys.push_back(*it);
      }
    }
    g_playerSquadsSyncHasValue = true;
    g_playerSquadsSyncLastDigest = digest;
    g_playerSquadsSyncLastSentTick = nowTick;
    g_playerSquadsLastKeys = currentSquadKeys;
  }
  LeaveCriticalSection(&g_stateMutex);

  if (!shouldSend) {
    return false;
  }

  for (std::map<std::string, std::string>::const_iterator it =
           squadPayloads.begin();
       it != squadPayloads.end(); ++it) {
    std::string payload =
        "{\"id\":\"" + EscapeJSON(it->first) + "\",\"value\":\"" +
        EscapeJSON(it->second) + "\",\"only_if_changed\":true}";
    AsyncPostToStobe(L"/conf_opts", payload);
  }

  for (size_t i = 0; i < staleSquadKeys.size(); ++i) {
    std::string payload = "{\"id\":\"" + EscapeJSON(staleSquadKeys[i]) +
                          "\",\"value\":\"[]\",\"only_if_changed\":true}";
    AsyncPostToStobe(L"/conf_opts", payload);
  }

  std::vector<std::string> squadNames(currentSquadKeys.begin(),
                                      currentSquadKeys.end());
  std::string squadNamesJson = BuildJsonStringArray(squadNames);
  std::string indexPayload =
      "{\"id\":\"PLAYER_SQUADS\",\"value\":\"" + EscapeJSON(squadNamesJson) +
      "\",\"only_if_changed\":true}";
  AsyncPostToStobe(L"/conf_opts", indexPayload);

  Log("SQUAD_SYNC: sent squads=" + ToString((int)squadPayloads.size()) +
      " stale=" + ToString((int)staleSquadKeys.size()) + " changed=" +
      std::string(changed ? "1" : "0") + " reason=" + reason);
  return true;
}

static bool SyncPlayerCatsValue(Character *player, bool force,
                                const std::string &reason) {
  if (!player || (uintptr_t)player < 0x1000) {
    return false;
  }

  int cats = 0;
  try {
    cats = player->getMoney();
    if (cats <= 0 && player->getOwnerships()) {
      cats = player->getOwnerships()->getMoney();
    }
  } catch (...) {
    return false;
  }
  if (cats < 0) {
    cats = 0;
  }

  DWORD nowTick = GetTickCount();
  bool shouldSend = false;
  bool changed = false;
  DWORD sinceLastSent = 0;
  EnterCriticalSection(&g_stateMutex);
  changed = (!g_playerCatsSyncHasValue || cats != g_playerCatsSyncLastValue);
  sinceLastSent =
      g_playerCatsSyncHasValue ? (nowTick - g_playerCatsSyncLastSentTick) : 0;
  if (force || !g_playerCatsSyncHasValue || changed ||
      sinceLastSent >= kPlayerCatsResendIntervalMs) {
    shouldSend = true;
    g_playerCatsSyncHasValue = true;
    g_playerCatsSyncLastValue = cats;
    g_playerCatsSyncLastSentTick = nowTick;
  }
  LeaveCriticalSection(&g_stateMutex);

  if (!shouldSend) {
    return false;
  }

  std::string payload = "{\"id\":\"PLAYER_CATS\",\"value\":\"" +
                        ToString(cats) + "\",\"only_if_changed\":true}";
  AsyncPostToStobe(L"/conf_opts", payload);
  Log("CATS_SYNC: sent value=" + ToString(cats) + " changed=" +
      std::string(changed ? "1" : "0") + " reason=" + reason);
  return true;
}

static bool SyncDynamicProfileIntervalToConfOpts(bool force,
                                                 const std::string &reason) {
  int intervalMinutes = g_dynamicProfileIntervalMinutes;
  if (intervalMinutes < 1) {
    intervalMinutes = 1;
  } else if (intervalMinutes > 720) {
    intervalMinutes = 720;
  }

  DWORD nowTick = GetTickCount();
  bool shouldSend = false;
  bool changed = false;
  DWORD sinceLastSent = 0;
  EnterCriticalSection(&g_stateMutex);
  changed = (!g_dynamicProfileIntervalSyncHasValue ||
             intervalMinutes != g_dynamicProfileIntervalSyncLastValue);
  sinceLastSent = g_dynamicProfileIntervalSyncHasValue
                      ? (nowTick - g_dynamicProfileIntervalSyncLastSentTick)
                      : 0;
  if (force || !g_dynamicProfileIntervalSyncHasValue || changed ||
      sinceLastSent >= kDynamicProfileIntervalResendIntervalMs) {
    shouldSend = true;
    g_dynamicProfileIntervalSyncHasValue = true;
    g_dynamicProfileIntervalSyncLastValue = intervalMinutes;
    g_dynamicProfileIntervalSyncLastSentTick = nowTick;
  }
  LeaveCriticalSection(&g_stateMutex);

  if (!shouldSend) {
    return false;
  }

  std::string payload =
      "{\"id\":\"DYNAMIC_PROFILE_INTERVAL_MINUTES\",\"value\":\"" +
      ToString(intervalMinutes) + "\",\"only_if_changed\":true}";
  AsyncPostToStobe(L"/conf_opts", payload);
  Log("DYNAMIC_PROFILE_SYNC: sent interval_minutes=" +
      ToString(intervalMinutes) + " changed=" +
      std::string(changed ? "1" : "0") + " reason=" + reason);
  return true;
}

static bool SyncPluginVersionToConfOpts(bool force, const std::string &reason) {
  const std::string pluginVersion =
      kStobePluginVersion ? std::string(kStobePluginVersion) : "";
  if (pluginVersion.empty()) {
    return false;
  }

  DWORD nowTick = GetTickCount();
  bool shouldSend = false;
  bool changed = false;
  DWORD sinceLastSent = 0;
  EnterCriticalSection(&g_stateMutex);
  changed = (!g_pluginVersionSyncHasValue ||
             pluginVersion != g_pluginVersionSyncLastValue);
  sinceLastSent = g_pluginVersionSyncHasValue
                      ? (nowTick - g_pluginVersionSyncLastSentTick)
                      : 0;
  if (force || !g_pluginVersionSyncHasValue || changed ||
      sinceLastSent >= kPluginVersionResendIntervalMs) {
    shouldSend = true;
    g_pluginVersionSyncHasValue = true;
    g_pluginVersionSyncLastValue = pluginVersion;
    g_pluginVersionSyncLastSentTick = nowTick;
  }
  LeaveCriticalSection(&g_stateMutex);

  if (!shouldSend) {
    return false;
  }

  std::string payload = "{\"id\":\"plugin_dll_version\",\"value\":\"" +
                        EscapeJSON(pluginVersion) +
                        "\",\"only_if_changed\":true}";
  AsyncPostToStobe(L"/conf_opts", payload);
  Log("PLUGIN_VERSION_SYNC: sent version=" + pluginVersion +
      " changed=" + std::string(changed ? "1" : "0") + " reason=" + reason);
  return true;
}

static void RefreshInventoryContextCache(Character *npc,
                                         const std::string &inventoryJson,
                                         bool isPlayerCharacter) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return;
  }
  EnterCriticalSection(&g_stateMutex);
  if (isPlayerCharacter) {
    g_playerInventoryJson = inventoryJson;
    g_playerHand = npc->getHandle();
  } else {
    g_activeInventoryJson = inventoryJson;
    g_lastInventoryHand = npc->getHandle();
  }
  LeaveCriticalSection(&g_stateMutex);
}

static std::string BuildInventorySyncPayload(Character *npc,
                                             const std::string &inventoryJson,
                                             int inventoryItemCount,
                                             const std::string &reason) {
  std::string payload = BuildIdentityBootstrapContext(npc);
  if (payload.empty() || payload[0] != '{' || payload.back() != '}') {
    payload = "{}";
  }

  std::string npcName = "Unknown";
  bool isPlayerCharacter = false;
  try {
    npcName = npc->getName();
    isPlayerCharacter = npc->isPlayerCharacter();
  } catch (...) {
  }

  payload.pop_back(); // remove closing '}'
  if (payload.length() > 1) {
    payload += ",";
  }
  payload += "\"name\":\"" + EscapeJSON(npcName) + "\",";
  payload += "\"type\":\"" + std::string(isPlayerCharacter ? "player" : "npc") + "\",";
  payload += "\"is_player_character\":" +
             std::string(isPlayerCharacter ? "true" : "false") + ",";
  payload += "\"inventory\":" + inventoryJson + ",";
  payload += "\"inventory_item_count\":" + ToString(inventoryItemCount) + ",";
  payload += "\"source\":\"inventory_live_sync\",";
  payload += "\"sync_reason\":\"" + EscapeJSON(reason) + "\"";
  payload += "}";
  return payload;
}

static bool SyncInventoryForCharacter(Character *npc, bool force,
                                      const std::string &reason) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return false;
  }
  if (!ShouldProcessAnimalCharacter(npc)) {
    return false;
  }

  unsigned int serial = 0;
  std::string npcName = "Unknown";
  bool isPlayerCharacter = false;
  try {
    serial = npc->getHandle().serial;
    npcName = npc->getName();
    isPlayerCharacter = npc->isPlayerCharacter();
  } catch (...) {
    return false;
  }
  if (serial == 0) {
    return false;
  }

  std::string inventoryJson = "[]";
  std::string inventoryHash = "";
  int inventoryItemCount = 0;
  if (!BuildInventorySnapshot(npc, inventoryJson, inventoryHash, inventoryItemCount)) {
    Log("INV_SYNC: sample failed serial=" + ToString(serial) + " reason=" +
        reason);
    return false;
  }
  RefreshInventoryContextCache(npc, inventoryJson, isPlayerCharacter);

  DWORD nowTick = GetTickCount();
  bool shouldSend = false;
  bool hashChanged = false;
  bool firstSync = false;
  DWORD sinceLastSent = 0;
  {
    EnterCriticalSection(&g_stateMutex);
    InventorySyncState &state = g_inventorySyncStateBySerial[serial];
    state.lastSeenTick = nowTick;
    hashChanged = (state.lastHash != inventoryHash);
    firstSync = !state.hasSent;
    sinceLastSent = state.hasSent ? (nowTick - state.lastSentTick) : 0;

    if (force) {
      shouldSend = firstSync || hashChanged || sinceLastSent >= kInventoryMinResendMs;
    } else {
      if (firstSync || hashChanged) {
        shouldSend = firstSync || sinceLastSent >= kInventoryMinResendMs;
      }
    }

    if (shouldSend) {
      state.lastHash = inventoryHash;
      state.lastSentTick = nowTick;
      state.hasSent = true;
    }
    LeaveCriticalSection(&g_stateMutex);
  }

  if (!shouldSend) {
    return false;
  }

  std::string payload =
      BuildInventorySyncPayload(npc, inventoryJson, inventoryItemCount, reason);
  AsyncPostToStobe(L"/context", payload);
  Log("INV_SYNC: sent name=" + npcName + " serial=" + ToString(serial) +
      " reason=" + reason +
      " force=" + std::string(force ? "1" : "0") +
      " hash=" + ShortInventoryHashForLog(inventoryHash) +
      " items=" + ToString(inventoryItemCount));
  return true;
}

static void PushImmediateContextSnapshot(Character *npc,
                                         const std::string &reason,
                                         bool synchronous) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return;
  }

  std::string contextType = "npc";
  std::string npcName = "Unknown";
  try {
    if (npc->isPlayerCharacter()) {
      contextType = "player";
    }
    npcName = npc->getName();
  } catch (...) {
  }

  std::string contextJson = BuildNpcContextEnvelope(npc, contextType);
  if (contextJson.empty() || contextJson.front() != '{' ||
      contextJson.back() != '}') {
    Log("CONTEXT_PUSH: skipped invalid immediate snapshot reason=" + reason +
        " name=" + npcName);
    return;
  }

  if (synchronous) {
    PostToStobe(L"/context", contextJson);
  } else {
    AsyncPostToStobe(L"/context", contextJson);
  }
  Log("CONTEXT_PUSH: sent immediate snapshot reason=" + reason +
      " name=" + npcName + " type=" + contextType +
      " len=" + ToString((int)contextJson.length()) +
      " sync=" + std::string(synchronous ? "1" : "0"));
}

static void PruneInventorySyncState() {
  DWORD nowTick = GetTickCount();
  int pruned = 0;
  EnterCriticalSection(&g_stateMutex);
  for (auto it = g_inventorySyncStateBySerial.begin();
       it != g_inventorySyncStateBySerial.end();) {
    DWORD age = nowTick - it->second.lastSeenTick;
    if (it->second.lastSeenTick == 0 || age > kInventoryStateRetentionMs) {
      it = g_inventorySyncStateBySerial.erase(it);
      ++pruned;
    } else {
      ++it;
    }
  }
  LeaveCriticalSection(&g_stateMutex);
  if (pruned > 0) {
    Log("INV_SYNC: pruned stale state entries=" + ToString(pruned));
  }
}

static void AddInventorySyncCandidate(Character *npc, std::vector<Character *> &out,
                                      std::set<unsigned int> &seen) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return;
  }
  if (!ShouldProcessAnimalCharacter(npc)) {
    return;
  }
  unsigned int serial = 0;
  try {
    serial = npc->getHandle().serial;
  } catch (...) {
    return;
  }
  if (serial == 0) {
    return;
  }
  if (!seen.insert(serial).second) {
    return;
  }
  out.push_back(npc);
}

static std::string NormalizeInventoryKey(const std::string &name) {
  std::string key = TrimCopy(name);
  for (size_t i = 0; i < key.length(); ++i) {
    key[i] = static_cast<char>(tolower((unsigned char)key[i]));
  }
  return key;
}

static Character *ResolveCharacterFromHandSafe(const hand &h) {
  if (!h.isValid() || h.isNull()) {
    return nullptr;
  }
  Character *npc = nullptr;
  try {
    npc = h.getCharacter();
  } catch (...) {
    npc = nullptr;
  }
  if (!npc || (uintptr_t)npc < 0x1000) {
    return nullptr;
  }
  return npc;
}

static Character *ResolveDialogueListenerForSpeech(Character *speaker,
                                                   Dialogue *dialogueHint,
                                                   std::string &sourceOut) {
  sourceOut.clear();
  if (!speaker || (uintptr_t)speaker < 0x1000) {
    return nullptr;
  }

  Dialogue *dialogue = dialogueHint;
  if (!dialogue || (uintptr_t)dialogue < 0x1000) {
    try {
      dialogue = speaker->dialogue;
    } catch (...) {
      dialogue = nullptr;
    }
  }
  if (!dialogue || (uintptr_t)dialogue < 0x1000) {
    return nullptr;
  }

  bool conversationActive = false;
  try {
    conversationActive = !dialogue->conversationHasEndedPrettyMuch();
  } catch (...) {
    conversationActive = false;
  }

  const bool allowInactiveDialogueState = (dialogueHint != nullptr);
  if (!conversationActive && !allowInactiveDialogueState) {
    return nullptr;
  }

  auto tryResolveHand = [&](const hand &candidateHand,
                            const char *label) -> Character * {
    Character *candidate = ResolveCharacterFromHandSafe(candidateHand);
    if (!candidate || (uintptr_t)candidate < 0x1000 || candidate == speaker) {
      return nullptr;
    }
    sourceOut = label;
    return candidate;
  };

  try {
    hand targetHand = dialogue->getConversationTarget();
    Character *target = tryResolveHand(targetHand, "getConversationTarget");
    if (target) {
      return target;
    }
  } catch (...) {
  }

  try {
    Character *target =
        tryResolveHand(dialogue->conversationTarget, "conversationTarget");
    if (target) {
      return target;
    }
  } catch (...) {
  }

  try {
    Character *target =
        tryResolveHand(dialogue->waitingForReplyFrom, "waitingForReplyFrom");
    if (target) {
      return target;
    }
  } catch (...) {
  }

  try {
    Character *target =
        tryResolveHand(dialogue->conversationMaster, "conversationMaster");
    if (target) {
      return target;
    }
  } catch (...) {
  }

  Character *fallback = ResolveCharacterFromHandSafe(g_talkTargetHand);
  if (fallback && (uintptr_t)fallback > 0x1000 && fallback != speaker) {
    sourceOut = "g_talkTargetHand";
    return fallback;
  }

  return nullptr;
}

static Character *ResolveNearestPlayerSpeakerForTarget(GameWorld *world,
                                                        Character *target) {
  if (!world || !world->player || world->player->playerCharacters.size() == 0) {
    return nullptr;
  }
  Character *fallback = world->player->playerCharacters[0];
  if (!g_useNearestPlayerSpeaker) {
    return fallback;
  }
  if (!target || (uintptr_t)target < 0x1000) {
    return fallback;
  }

  Character *best = nullptr;
  float bestDist = 1e30f;
  for (uint32_t i = 0; i < world->player->playerCharacters.size(); ++i) {
    Character *candidate = world->player->playerCharacters[i];
    if (!candidate || (uintptr_t)candidate < 0x1000) {
      continue;
    }
    float dist = candidate->getPosition().distance(target->getPosition());
    if (!best || dist < bestDist) {
      best = candidate;
      bestDist = dist;
    }
  }
  return best ? best : fallback;
}

static Character *ResolveNearestSquadmateTargetForSelection(GameWorld *world,
                                                            Character *selected) {
  if (!world || !world->player || world->player->playerCharacters.size() == 0) {
    return nullptr;
  }
  if (!selected || (uintptr_t)selected < 0x1000) {
    return nullptr;
  }

  Character *best = nullptr;
  float bestDist = 1e30f;
  for (uint32_t i = 0; i < world->player->playerCharacters.size(); ++i) {
    Character *candidate = world->player->playerCharacters[i];
    if (!candidate || (uintptr_t)candidate < 0x1000 || candidate == selected) {
      continue;
    }
    float dist = candidate->getPosition().distance(selected->getPosition());
    if (!best || dist < bestDist) {
      best = candidate;
      bestDist = dist;
    }
  }
  return best;
}

static Character *ResolvePlayerSpeakerForCurrentTalk(GameWorld *world) {
  Character *target = ResolveCharacterFromHandSafe(g_talkTargetHand);
  if (!target) {
    target = ResolveCharacterFromHandSafe(g_lastSelectionHand);
  }
  return ResolveNearestPlayerSpeakerForTarget(world, target);
}

static unsigned int ResolveCharacterSerialForEvent(Character *npc) {
  if (!npc || (uintptr_t)npc < 0x1000) {
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

static std::string ResolvePrimaryWeaponName(Character *npc) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return "Unknown";
  }
  Inventory *inv = nullptr;
  try {
    inv = npc->getInventory();
  } catch (...) {
    inv = nullptr;
  }
  if (!inv || (uintptr_t)inv < 0x1000) {
    return "Unarmed";
  }

  lektor<Item *> weapons;
  try {
    inv->getEquippedWeapons(weapons);
  } catch (...) {
    return "Unarmed";
  }

  for (uint32_t i = 0; i < weapons.size(); ++i) {
    Item *weapon = weapons.stuff[i];
    if (!weapon || (uintptr_t)weapon < 0x1000) {
      continue;
    }
    try {
      std::string weaponName = TrimCopy(weapon->getName());
      if (!weaponName.empty()) {
        return weaponName;
      }
    } catch (...) {
    }
  }
  return "Unarmed";
}

struct CombatAttribution {
  std::string actorName;
  std::string actorFaction;
  std::string weaponName;
  unsigned int actorSerial;

  CombatAttribution()
      : actorName("Unknown"), actorFaction("None"), weaponName("Unknown"),
        actorSerial(0) {}
};

static CombatAttribution ResolveCombatAttribution(Character *target) {
  CombatAttribution out;
  if (!target || (uintptr_t)target < 0x1000) {
    return out;
  }

  Character *attacker = nullptr;
  try {
    attacker = ResolveCharacterFromHandSafe(target->lastGuyWhoDefeatedMe);
  } catch (...) {
    attacker = nullptr;
  }

  if (!attacker) {
    lektor<hand> attackers;
    try {
      target->getAllAttackers(attackers);
    } catch (...) {
    }
    for (uint32_t i = 0; i < attackers.size(); ++i) {
      Character *candidate = ResolveCharacterFromHandSafe(attackers.stuff[i]);
      if (!candidate || candidate == target) {
        continue;
      }
      attacker = candidate;
      break;
    }
  }

  if (!attacker) {
    return out;
  }

  try {
    out.actorName = TrimCopy(attacker->getName());
  } catch (...) {
    out.actorName = "Unknown";
  }
  if (out.actorName.empty()) {
    out.actorName = "Unknown";
  }
  out.actorFaction = SafeFaction(attacker);
  out.weaponName = ResolvePrimaryWeaponName(attacker);
  if ((out.weaponName.empty() || out.weaponName == "Unknown") &&
      CharacterHasHacksaw(attacker)) {
    out.weaponName = "Hacksaw";
  }
  if (out.weaponName.empty() || out.weaponName == "Unknown") {
    out.weaponName = "Unarmed";
  }
  out.actorSerial = ResolveCharacterSerialForEvent(attacker);
  return out;
}

static bool IsLimbLostState(int limbState) {
  return limbState == (int)LIMB_STUMP || limbState == (int)LIMB_CRUSHED;
}

static int ResolveLimbState(Character *npc, RobotLimbs::Limb limb) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return (int)LIMB_ORIGINAL;
  }
  MedicalSystem *med = nullptr;
  try {
    med = npc->getMedical();
  } catch (...) {
    med = nullptr;
  }
  if (!med || (uintptr_t)med < 0x1000) {
    return (int)LIMB_ORIGINAL;
  }
  int state = (int)LIMB_ORIGINAL;
  try {
    state = (int)med->getLimbState(limb);
  } catch (...) {
    state = (int)LIMB_ORIGINAL;
  }
  return state;
}

static bool ResolveLimbPartPresent(Character *npc, RobotLimbs::Limb limb) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return true;
  }
  MedicalSystem *med = nullptr;
  try {
    med = npc->getMedical();
  } catch (...) {
    med = nullptr;
  }
  if (!med || (uintptr_t)med < 0x1000) {
    return true;
  }
  MedicalSystem::HealthPartStatus *part = nullptr;
  try {
    part = med->getPart(limb);
  } catch (...) {
    part = nullptr;
  }
  return part && (uintptr_t)part >= 0x1000;
}

static bool CollectInventoryEventSnapshot(Character *npc,
                                          InventoryEventSnapshot &out) {
  out = InventoryEventSnapshot();
  if (!npc || (uintptr_t)npc < 0x1000) {
    return false;
  }

  std::vector<Item *> rawItems;
  try {
    GetAllCharacterItems(npc, rawItems);
  } catch (...) {
    return false;
  }

  for (uint32_t i = 0; i < rawItems.size(); ++i) {
    Item *item = rawItems[i];
    if (!item || (uintptr_t)item < 0x1000) {
      continue;
    }

    std::string itemName;
    int count = 1;
    bool isStolen = false;
    try {
      itemName = TrimCopy(item->getName());
      count = item->quantity;
      isStolen = item->isStolen(true);
    } catch (...) {
      continue;
    }
    if (itemName.empty()) {
      continue;
    }
    if (count <= 0) {
      count = 1;
    }
    std::string key = NormalizeInventoryKey(itemName);
    if (key.empty()) {
      key = itemName;
    }
    out.countsByKey[key] += count;
    if (isStolen) {
      out.stolenByKey[key] += count;
    }
    if (out.displayNameByKey.count(key) == 0) {
      out.displayNameByKey[key] = itemName;
    }
    out.totalCount += count;
  }

  return true;
}

static std::string ResolveCharacterNameSafe(Character *npc) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return "Unknown";
  }
  std::string name;
  try {
    name = TrimCopy(npc->getName());
  } catch (...) {
    name = "";
  }
  if (name.empty()) {
    name = "Unknown";
  }
  return name;
}

static int ResolveLockpickingSkillLevel(Character *npc) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return 0;
  }
  try {
    CharStats *stats = npc->getStats();
    if (!stats || (uintptr_t)stats < 0x1000) {
      return 0;
    }
    int level = (int)(stats->lockpicking + 0.5f);
    if (level < 0) {
      level = 0;
    } else if (level > 200) {
      level = 200;
    }
    return level;
  } catch (...) {
    return 0;
  }
}

static void EmitLimbLossEvent(Character *victim, const std::string &limbLabel) {
  CombatAttribution attribution = ResolveCombatAttribution(victim);
  std::string victimName = ResolveCharacterNameSafe(victim);
  std::string victimFaction = SafeFaction(victim);
  std::string message = "severed " + limbLabel + " from " + victimName +
                        " with " + attribution.weaponName;
  LogGameEvent("limb_loss", attribution.actorName, attribution.actorFaction,
               victimName, victimFaction, message, attribution.actorSerial,
               ResolveCharacterSerialForEvent(victim));
}

static void EmitKnockoutEvent(Character *victim) {
  CombatAttribution attribution = ResolveCombatAttribution(victim);
  std::string victimName = ResolveCharacterNameSafe(victim);
  std::string victimFaction = SafeFaction(victim);
  std::string message =
      "knocked out with " + attribution.weaponName;
  LogGameEvent("knockout", attribution.actorName, attribution.actorFaction,
               victimName, victimFaction, message, attribution.actorSerial,
               ResolveCharacterSerialForEvent(victim));
}

static void EmitRecoveredEvent(Character *victim) {
  std::string victimName = ResolveCharacterNameSafe(victim);
  std::string victimFaction = SafeFaction(victim);
  LogGameEvent("recovered", victimName, victimFaction, "None", "None",
               "regained consciousness",
               ResolveCharacterSerialForEvent(victim), 0);
}

static void EmitDeathEvent(Character *victim) {
  std::string victimName = ResolveCharacterNameSafe(victim);
  std::string victimFaction = SafeFaction(victim);
  LogGameEvent("death", victimName, victimFaction, "None", "None", "has died",
               ResolveCharacterSerialForEvent(victim), 0);
}

static void EmitSlaveryEvent(Character *victim, bool enslavedNow) {
  std::string victimName = ResolveCharacterNameSafe(victim);
  std::string victimFaction = SafeFaction(victim);
  Character *owner = nullptr;
  try {
    owner = ResolveCharacterFromHandSafe(victim->getMySlaveOwner());
    if (!owner) {
      owner = ResolveCharacterFromHandSafe(victim->slaveOwner);
    }
  } catch (...) {
    owner = nullptr;
  }
  std::string ownerName = owner ? ResolveCharacterNameSafe(owner) : "Unknown";
  std::string ownerFaction = owner ? SafeFaction(owner) : "None";
  std::string message = enslavedNow ? "enslaved as a slave" : "freed from slavery";
  LogGameEvent("slavery", ownerName, ownerFaction, victimName, victimFaction,
               message, ResolveCharacterSerialForEvent(owner),
               ResolveCharacterSerialForEvent(victim));
}

static void EmitLockpickedEvent(Character *npc, int previousSkill,
                                int currentSkill) {
  std::string actorName = ResolveCharacterNameSafe(npc);
  std::string actorFaction = SafeFaction(npc);
  std::string skillDelta = ToString(previousSkill) + "->" + ToString(currentSkill);
  std::string message = "picked a lock (lockpicking " + skillDelta + ")";
  LogGameEvent("lockpicked", actorName, actorFaction, "None", "None", message,
               ResolveCharacterSerialForEvent(npc), 0);
}

static void EmitPickupEvent(Character *npc, const NpcWorldEventState &state,
                            const InventoryEventSnapshot &currentSnapshot) {
  int addedCount = 0;
  int addedStolenCount = 0;
  std::string itemName = "Unknown Item";

  for (std::map<std::string, int>::const_iterator it =
           currentSnapshot.countsByKey.begin();
       it != currentSnapshot.countsByKey.end(); ++it) {
    const std::string &key = it->first;
    int currentCount = it->second;
    int previousCount = 0;
    std::map<std::string, int>::const_iterator prevIt =
        state.inventory.countsByKey.find(key);
    if (prevIt != state.inventory.countsByKey.end()) {
      previousCount = prevIt->second;
    }
    int delta = currentCount - previousCount;
    if (delta <= 0) {
      continue;
    }
    addedCount += delta;
    if (itemName == "Unknown Item") {
      std::map<std::string, std::string>::const_iterator displayIt =
          currentSnapshot.displayNameByKey.find(key);
      if (displayIt != currentSnapshot.displayNameByKey.end() &&
          !displayIt->second.empty()) {
        itemName = displayIt->second;
      }
    }

    int currentStolen = 0;
    int previousStolen = 0;
    std::map<std::string, int>::const_iterator stolenNowIt =
        currentSnapshot.stolenByKey.find(key);
    if (stolenNowIt != currentSnapshot.stolenByKey.end()) {
      currentStolen = stolenNowIt->second;
    }
    std::map<std::string, int>::const_iterator stolenPrevIt =
        state.inventory.stolenByKey.find(key);
    if (stolenPrevIt != state.inventory.stolenByKey.end()) {
      previousStolen = stolenPrevIt->second;
    }
    int stolenDelta = currentStolen - previousStolen;
    if (stolenDelta > 0) {
      addedStolenCount += stolenDelta;
    }
  }

  if (addedCount <= 0) {
    return;
  }

  std::string actorName = ResolveCharacterNameSafe(npc);
  std::string actorFaction = SafeFaction(npc);
  std::string mode = addedStolenCount > 0 ? "theft" : "normal";
  std::string message = "picked up " + ToString(addedCount) + "x " + itemName +
                        " (" + mode + ")";
  LogGameEvent("item_pickup", actorName, actorFaction, itemName, "Item",
               message, ResolveCharacterSerialForEvent(npc), 0);
}

static void PruneNpcWorldEventState() {
  DWORD nowTick = GetTickCount();
  int pruned = 0;
  for (std::map<unsigned int, NpcWorldEventState>::iterator it =
           g_npcWorldEventStateBySerial.begin();
       it != g_npcWorldEventStateBySerial.end();) {
    DWORD age = nowTick - it->second.lastSeenTick;
    if (it->second.lastSeenTick == 0 || age > kNpcWorldEventStateRetentionMs) {
      it = g_npcWorldEventStateBySerial.erase(it);
      ++pruned;
    } else {
      ++it;
    }
  }
  if (pruned > 0) {
    Log("EVENT_SCAN: pruned npc event state entries=" + ToString(pruned));
  }
}

static std::string NormalizeInfoTelemetryToken(const std::string &rawValue) {
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

static bool BuildInfolocPayload(Character *player, std::string &digestOut,
                                std::string &messageOut) {
  digestOut = "";
  messageOut = "";
  if (!player || (uintptr_t)player < 0x1000) {
    return false;
  }

  std::string contextJson = BuildNpcContextEnvelope(player, "player");
  if (contextJson.empty() || contextJson == "{}") {
    return false;
  }

  std::string town = NormalizeInfoTelemetryToken(JsonReadField(contextJson, "town"));
  std::string environmentJson = JsonReadField(contextJson, "environment");
  std::string building = NormalizeInfoTelemetryToken(
      JsonReadField(environmentJson, "building_name"));
  std::string zone =
      NormalizeInfoTelemetryToken(JsonReadField(environmentJson, "zone_name"));
  std::string region =
      NormalizeInfoTelemetryToken(JsonReadField(environmentJson, "region"));
  std::string resolvedRegion = zone.empty() ? region : zone;

  std::string location = "";
  if (!building.empty() && !town.empty()) {
    location = building + ", " + town;
  } else if (!town.empty()) {
    location = town;
  } else if (!building.empty()) {
    location = building;
  } else if (!resolvedRegion.empty()) {
    location = resolvedRegion;
  }
  if (location.empty()) {
    return false;
  }

  std::string locationWithRegion = location;
  if (!resolvedRegion.empty() &&
      ToLowerAsciiCopy(locationWithRegion).find(ToLowerAsciiCopy(resolvedRegion)) ==
          std::string::npos) {
    locationWithRegion += ", " + resolvedRegion;
  }

  messageOut = "location update: " + locationWithRegion;
  digestOut = ToLowerAsciiCopy(location + "|" + town + "|" + resolvedRegion);
  return !digestOut.empty();
}

struct InfoNearbyNpcCandidate {
  Character *npc;
  unsigned int serial;
  std::string name;
  float distance;

  InfoNearbyNpcCandidate()
      : npc(nullptr), serial(0), name(""), distance(0.0f) {}
};

static void AppendInfonpcCandidate(std::vector<InfoNearbyNpcCandidate> &out,
                                   std::set<unsigned int> &seen,
                                   Character *candidate, Character *player) {
  if (!candidate || !player || (uintptr_t)candidate < 0x1000 ||
      (uintptr_t)player < 0x1000 || candidate == player) {
    return;
  }
  if (!ShouldProcessAnimalCharacter(candidate)) {
    return;
  }

  unsigned int serial = 0;
  try {
    serial = candidate->getHandle().serial;
  } catch (...) {
    serial = 0;
  }
  if (serial == 0 || seen.count(serial) > 0) {
    return;
  }

  std::string name = ResolveCharacterNameSafe(candidate);
  if (name.empty() || ToLowerAsciiCopy(name) == "unknown") {
    return;
  }

  float dist = 0.0f;
  try {
    Ogre::Vector3 a = player->getPosition();
    Ogre::Vector3 b = candidate->getPosition();
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    dist = std::sqrt(dx * dx + dy * dy + dz * dz);
  } catch (...) {
    dist = 0.0f;
  }

  InfoNearbyNpcCandidate row;
  row.npc = candidate;
  row.serial = serial;
  row.name = name;
  row.distance = dist;
  out.push_back(row);
  seen.insert(serial);
}

static bool BuildInfonpcPayload(GameWorld *world, Character *player,
                                Character *selection, std::string &digestOut,
                                std::string &messageOut, std::string &targetOut,
                                std::string &targetFactionOut,
                                unsigned int &targetSerialOut) {
  digestOut = "";
  messageOut = "";
  targetOut = "None";
  targetFactionOut = "None";
  targetSerialOut = 0;
  if (!world || !player || (uintptr_t)player < 0x1000) {
    return false;
  }

  float scanRange = g_boredEventRange;
  if (scanRange < 80.0f) {
    scanRange = 80.0f;
  } else if (scanRange > 600.0f) {
    scanRange = 600.0f;
  }

  std::vector<InfoNearbyNpcCandidate> candidates;
  std::set<unsigned int> seen;
  candidates.reserve(32);

  AppendInfonpcCandidate(candidates, seen, selection, player);
  Character *talkTarget = nullptr;
  try {
    talkTarget = g_talkTargetHand.getCharacter();
  } catch (...) {
    talkTarget = nullptr;
  }
  AppendInfonpcCandidate(candidates, seen, talkTarget, player);

  lektor<RootObject *> nearby;
  world->getCharactersWithinSphere(nearby, player->getPosition(), scanRange, 0.0f,
                                   0.0f, 16, 0, player);
  for (uint32_t i = 0; i < nearby.size(); ++i) {
    Character *other = (Character *)nearby.stuff[i];
    AppendInfonpcCandidate(candidates, seen, other, player);
    if (candidates.size() >= 48) {
      break;
    }
  }

  if (candidates.empty()) {
    return false;
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const InfoNearbyNpcCandidate &a,
               const InfoNearbyNpcCandidate &b) -> bool {
              return a.distance < b.distance;
            });

  std::vector<unsigned int> digestSerials;
  digestSerials.reserve(candidates.size());
  for (size_t i = 0; i < candidates.size(); ++i) {
    digestSerials.push_back(candidates[i].serial);
  }
  std::sort(digestSerials.begin(), digestSerials.end());
  for (size_t i = 0; i < digestSerials.size(); ++i) {
    if (i > 0) {
      digestOut += ",";
    }
    digestOut += ToString(digestSerials[i]);
  }
  if (digestOut.empty()) {
    return false;
  }

  messageOut = "nearby NPC roster (" + ToString((int)candidates.size()) + "): ";
  size_t listed = 0;
  for (size_t i = 0; i < candidates.size() && listed < 8; ++i) {
    if (listed > 0) {
      messageOut += ", ";
    }
    messageOut += candidates[i].name;
    ++listed;
  }
  if (candidates.size() > listed) {
    messageOut += ", ...";
  }

  targetOut = candidates[0].name;
  targetFactionOut = SafeFaction(candidates[0].npc);
  targetSerialOut = candidates[0].serial;
  return true;
}

static void RunInfoTelemetrySweep(GameWorld *world, Character *selection) {
  if (!world || !world->player || world->player->playerCharacters.size() == 0) {
    return;
  }
  Character *player = world->player->playerCharacters[0];
  if (!player || (uintptr_t)player < 0x1000) {
    return;
  }

  DWORD nowTick = GetTickCount();
  if (nowTick - g_lastInfoLocTelemetryCheckTick >=
      kInfoLocTelemetryCheckIntervalMs) {
    g_lastInfoLocTelemetryCheckTick = nowTick;
    std::string digest;
    std::string message;
    if (BuildInfolocPayload(player, digest, message)) {
      bool changed = digest != g_lastInfoLocTelemetryDigest;
      bool resendDue = g_lastInfoLocTelemetrySentTick == 0 ||
                       (nowTick - g_lastInfoLocTelemetrySentTick) >=
                           kInfoLocTelemetryResendIntervalMs;
      if (changed || resendDue) {
        LogGameEvent("infoloc", ResolveCharacterNameSafe(player), SafeFaction(player),
                     "None", "None", message, ResolveCharacterSerialForEvent(player),
                     0);
        g_lastInfoLocTelemetryDigest = digest;
        g_lastInfoLocTelemetrySentTick = nowTick;
      }
    }
  }

  if (nowTick - g_lastInfoNpcTelemetryCheckTick >=
      kInfoNpcTelemetryCheckIntervalMs) {
    g_lastInfoNpcTelemetryCheckTick = nowTick;
    std::string digest;
    std::string message;
    std::string targetName;
    std::string targetFaction;
    unsigned int targetSerial = 0;
    if (BuildInfonpcPayload(world, player, selection, digest, message, targetName,
                            targetFaction, targetSerial)) {
      bool changed = digest != g_lastInfoNpcTelemetryDigest;
      bool resendDue = g_lastInfoNpcTelemetrySentTick == 0 ||
                       (nowTick - g_lastInfoNpcTelemetrySentTick) >=
                           kInfoNpcTelemetryResendIntervalMs;
      if (changed || resendDue) {
        LogGameEvent("infonpc", ResolveCharacterNameSafe(player), SafeFaction(player),
                     targetName, targetFaction, message,
                     ResolveCharacterSerialForEvent(player), targetSerial);
        g_lastInfoNpcTelemetryDigest = digest;
        g_lastInfoNpcTelemetrySentTick = nowTick;
      }
    }
  }
}

static void RunNpcWorldEventSweep(GameWorld *world, Character *selection) {
  if (!world || !world->player || world->player->playerCharacters.size() == 0) {
    return;
  }

  DWORD nowTick = GetTickCount();
  if (nowTick - g_lastNpcWorldEventSweepTick < kNpcWorldEventSweepIntervalMs) {
    return;
  }
  g_lastNpcWorldEventSweepTick = nowTick;

  Character *player = world->player->playerCharacters[0];
  if (!player || (uintptr_t)player < 0x1000) {
    return;
  }

  std::vector<Character *> candidates;
  std::set<unsigned int> seen;
  candidates.reserve(kNpcWorldEventCandidateLimit + 4);

  AddInventorySyncCandidate(selection, candidates, seen);
  Character *talkTarget = nullptr;
  try {
    talkTarget = g_talkTargetHand.getCharacter();
  } catch (...) {
    talkTarget = nullptr;
  }
  AddInventorySyncCandidate(talkTarget, candidates, seen);

  float eventRange = g_shoutRadius;
  if (eventRange < 120.0f) {
    eventRange = 120.0f;
  } else if (eventRange > 600.0f) {
    eventRange = 600.0f;
  }

  lektor<RootObject *> nearby;
  world->getCharactersWithinSphere(nearby, player->getPosition(), eventRange, 0.0f,
                                   0.0f, 16, 0, player);
  for (uint32_t i = 0; i < nearby.size(); ++i) {
    Character *other = (Character *)nearby.stuff[i];
    AddInventorySyncCandidate(other, candidates, seen);
    if (candidates.size() >= kNpcWorldEventCandidateLimit) {
      break;
    }
  }

  for (size_t i = 0; i < candidates.size(); ++i) {
    Character *npc = candidates[i];
    if (!npc || (uintptr_t)npc < 0x1000 || npc == player) {
      continue;
    }

    unsigned int serial = 0;
    try {
      serial = npc->getHandle().serial;
    } catch (...) {
      serial = 0;
    }
    if (serial == 0) {
      continue;
    }

    bool deadNow = false;
    bool unconsciousNow = false;
    bool enslavedNow = false;
    int lockpickingNow = 0;
    try {
      deadNow = npc->isDead();
      unconsciousNow = npc->isUnconcious();
      SlaveStateEnum slaveState = npc->isSlave();
      enslavedNow = (slaveState != 0) || npc->isChainedMode();
    } catch (...) {
      continue;
    }

    int leftArmState = ResolveLimbState(npc, RobotLimbs::LEFT_ARM);
    int rightArmState = ResolveLimbState(npc, RobotLimbs::RIGHT_ARM);
    int leftLegState = ResolveLimbState(npc, RobotLimbs::LEFT_LEG);
    int rightLegState = ResolveLimbState(npc, RobotLimbs::RIGHT_LEG);
    bool leftArmPresent = ResolveLimbPartPresent(npc, RobotLimbs::LEFT_ARM);
    bool rightArmPresent = ResolveLimbPartPresent(npc, RobotLimbs::RIGHT_ARM);
    bool leftLegPresent = ResolveLimbPartPresent(npc, RobotLimbs::LEFT_LEG);
    bool rightLegPresent = ResolveLimbPartPresent(npc, RobotLimbs::RIGHT_LEG);

    InventoryEventSnapshot inventorySnapshot;
    CollectInventoryEventSnapshot(npc, inventorySnapshot);
    lockpickingNow = ResolveLockpickingSkillLevel(npc);
    bool speechActiveNow = HasActiveSpeechBubbleSafe(npc);
    std::string speechLine = "";
    bool speechHadTtsMetadata = false;
    if (speechActiveNow) {
      speechLine = ReadNpcSpeechLineSafe(npc);
        if (!speechLine.empty()) {
          std::string structuredSpeech =
              ExtractDialogueMessageFromStructuredText(speechLine);
          if (!structuredSpeech.empty()) {
            speechLine = structuredSpeech;
        }
        int speechTtsDurationMs = ExtractTrailingTtsDurationMs(speechLine);
        std::string speechTtsHash = ExtractTrailingTtsHash(speechLine);
        speechHadTtsMetadata = !speechTtsHash.empty() || speechTtsDurationMs > 0;
        if (!speechHadTtsMetadata && LooksLikeDialogueTemplateToken(speechLine)) {
          std::string resolvedSpeech = ReadNpcSpeechLineSafe(npc);
          if (!resolvedSpeech.empty()) {
            std::string structuredResolved =
                ExtractDialogueMessageFromStructuredText(resolvedSpeech);
            if (!structuredResolved.empty()) {
              resolvedSpeech = structuredResolved;
            }
            int resolvedTtsDurationMs = ExtractTrailingTtsDurationMs(resolvedSpeech);
            std::string resolvedTtsHash = ExtractTrailingTtsHash(resolvedSpeech);
            resolvedSpeech = TrimCopy(resolvedSpeech);
            if (resolvedTtsHash.empty() && resolvedTtsDurationMs <= 0 &&
                !resolvedSpeech.empty() &&
                !LooksLikeDialogueTemplateToken(resolvedSpeech)) {
              speechLine = resolvedSpeech;
            } else {
              speechLine.clear();
            }
          } else {
            speechLine.clear();
          }
        }
      }
      speechLine = SanitizeCapturedDialogueLine(speechLine);
    }
    std::string normalizedSpeechLine = ToLowerAsciiCopy(TrimCopy(speechLine));
    if (normalizedSpeechLine.length() > 240) {
      normalizedSpeechLine = normalizedSpeechLine.substr(0, 240);
    }

    NpcWorldEventState &state = g_npcWorldEventStateBySerial[serial];
    if (!state.initialized) {
      state.initialized = true;
      state.dead = deadNow;
      state.unconscious = unconsciousNow;
      state.enslaved = enslavedNow;
      state.speechActive = speechActiveNow;
      state.leftArmPresent = leftArmPresent;
      state.rightArmPresent = rightArmPresent;
      state.leftLegPresent = leftLegPresent;
      state.rightLegPresent = rightLegPresent;
      state.leftArmState = leftArmState;
      state.rightArmState = rightArmState;
      state.leftLegState = leftLegState;
      state.rightLegState = rightLegState;
      state.lockpickingSkill = lockpickingNow;
      state.lastSpeechLine = normalizedSpeechLine;
      state.inventory = inventorySnapshot;
      state.lastSeenTick = nowTick;
      if (IsLimbLostState(leftArmState) || !leftArmPresent ||
          IsLimbLostState(rightArmState) || !rightArmPresent ||
          IsLimbLostState(leftLegState) || !leftLegPresent ||
          IsLimbLostState(rightLegState) || !rightLegPresent) {
        Log("EVENT_SCAN: init limb state serial=" + ToString(serial) +
            " name=" + ResolveCharacterNameSafe(npc) +
            " la=" + ToString(leftArmState) + "/" +
            std::string(leftArmPresent ? "1" : "0") + " ra=" +
            ToString(rightArmState) + "/" +
            std::string(rightArmPresent ? "1" : "0") + " ll=" +
            ToString(leftLegState) + "/" +
            std::string(leftLegPresent ? "1" : "0") + " rl=" +
            ToString(rightLegState) + "/" +
            std::string(rightLegPresent ? "1" : "0"));
      }
      continue;
    }

    if (!state.unconscious && unconsciousNow) {
      EmitKnockoutEvent(npc);
    } else if (state.unconscious && !unconsciousNow && !deadNow) {
      EmitRecoveredEvent(npc);
    }
    if (!state.dead && deadNow) {
      EmitDeathEvent(npc);
    }
    if (!state.enslaved && enslavedNow) {
      EmitSlaveryEvent(npc, true);
    } else if (state.enslaved && !enslavedNow) {
      EmitSlaveryEvent(npc, false);
    }

    if (!IsLimbLostState(state.leftArmState) && IsLimbLostState(leftArmState)) {
      Log("EVENT_SCAN: limb loss transition serial=" + ToString(serial) +
          " limb=left_arm state " + ToString(state.leftArmState) + "->" +
          ToString(leftArmState));
      EmitLimbLossEvent(npc, "left arm");
    } else if (state.leftArmPresent && !leftArmPresent) {
      Log("EVENT_SCAN: limb missing transition serial=" + ToString(serial) +
          " limb=left_arm part_present 1->0");
      EmitLimbLossEvent(npc, "left arm");
    }
    if (!IsLimbLostState(state.rightArmState) && IsLimbLostState(rightArmState)) {
      Log("EVENT_SCAN: limb loss transition serial=" + ToString(serial) +
          " limb=right_arm state " + ToString(state.rightArmState) + "->" +
          ToString(rightArmState));
      EmitLimbLossEvent(npc, "right arm");
    } else if (state.rightArmPresent && !rightArmPresent) {
      Log("EVENT_SCAN: limb missing transition serial=" + ToString(serial) +
          " limb=right_arm part_present 1->0");
      EmitLimbLossEvent(npc, "right arm");
    }
    if (!IsLimbLostState(state.leftLegState) && IsLimbLostState(leftLegState)) {
      Log("EVENT_SCAN: limb loss transition serial=" + ToString(serial) +
          " limb=left_leg state " + ToString(state.leftLegState) + "->" +
          ToString(leftLegState));
      EmitLimbLossEvent(npc, "left leg");
    } else if (state.leftLegPresent && !leftLegPresent) {
      Log("EVENT_SCAN: limb missing transition serial=" + ToString(serial) +
          " limb=left_leg part_present 1->0");
      EmitLimbLossEvent(npc, "left leg");
    }
    if (!IsLimbLostState(state.rightLegState) && IsLimbLostState(rightLegState)) {
      Log("EVENT_SCAN: limb loss transition serial=" + ToString(serial) +
          " limb=right_leg state " + ToString(state.rightLegState) + "->" +
          ToString(rightLegState));
      EmitLimbLossEvent(npc, "right leg");
    } else if (state.rightLegPresent && !rightLegPresent) {
      Log("EVENT_SCAN: limb missing transition serial=" + ToString(serial) +
          " limb=right_leg part_present 1->0");
      EmitLimbLossEvent(npc, "right leg");
    }

    if (inventorySnapshot.totalCount > state.inventory.totalCount) {
      EmitPickupEvent(npc, state, inventorySnapshot);
    }
    if (lockpickingNow > state.lockpickingSkill) {
      EmitLockpickedEvent(npc, state.lockpickingSkill, lockpickingNow);
    }

    bool hasAmbientSpeech = speechActiveNow && !normalizedSpeechLine.empty();
    bool speechChanged =
        hasAmbientSpeech &&
        (!state.speechActive || normalizedSpeechLine != state.lastSpeechLine);
    if (speechChanged && !speechHadTtsMetadata &&
        !IsNpcInSpeechFlowBySerial(serial) &&
        !ShouldDropDuplicateNonAiDialogue(serial, false, speechLine)) {
      // Ambient sweep capture stores base dialogue with no explicit target.
      unsigned int listenerSerial = 0;
      std::string listenerName = "None";
      std::string listenerFaction = "None";
      LogGameEvent("chat", ResolveCharacterNameSafe(npc), SafeFaction(npc),
                   listenerName, listenerFaction, speechLine, serial,
                   listenerSerial);
      Log("EVENT_SCAN: ambient speech captured serial=" + ToString(serial) +
          " speaker=" + ResolveCharacterNameSafe(npc));
    }

    state.dead = deadNow;
    state.unconscious = unconsciousNow;
    state.enslaved = enslavedNow;
    state.speechActive = speechActiveNow;
    state.leftArmPresent = leftArmPresent;
    state.rightArmPresent = rightArmPresent;
    state.leftLegPresent = leftLegPresent;
    state.rightLegPresent = rightLegPresent;
    state.leftArmState = leftArmState;
    state.rightArmState = rightArmState;
    state.leftLegState = leftLegState;
    state.rightLegState = rightLegState;
    state.lockpickingSkill = lockpickingNow;
    state.lastSpeechLine = speechActiveNow ? normalizedSpeechLine : "";
    state.inventory = inventorySnapshot;
    state.lastSeenTick = nowTick;
  }

  static DWORD lastPruneTick = 0;
  if (nowTick - lastPruneTick >= 60000) {
    lastPruneTick = nowTick;
    PruneNpcWorldEventState();
  }
}

static void RunInventorySyncSweep(GameWorld *world, Character *selection) {
  if (!world || !world->player || world->player->playerCharacters.size() == 0) {
    return;
  }

  DWORD nowTick = GetTickCount();
  if (nowTick - g_lastInventorySweepTick < kInventorySweepIntervalMs) {
    return;
  }
  g_lastInventorySweepTick = nowTick;

  Character *player = world->player->playerCharacters[0];
  if (!player || (uintptr_t)player < 0x1000) {
    return;
  }

  std::vector<Character *> candidates;
  std::set<unsigned int> seen;
  candidates.reserve(kInventorySweepCandidateLimit + 3);

  AddInventorySyncCandidate(player, candidates, seen);
  AddInventorySyncCandidate(selection, candidates, seen);
  Character *talkTarget = nullptr;
  try {
    talkTarget = g_talkTargetHand.getCharacter();
  } catch (...) {
    talkTarget = nullptr;
  }
  AddInventorySyncCandidate(talkTarget, candidates, seen);

  float syncRange = g_proximityRadius;
  if (syncRange < 25.0f) {
    syncRange = 25.0f;
  } else if (syncRange > 120.0f) {
    syncRange = 120.0f;
  }

  lektor<RootObject *> nearby;
  world->getCharactersWithinSphere(nearby, player->getPosition(), syncRange, 0.0f,
                                   0.0f, 16, 0, player);
  for (uint32_t i = 0; i < nearby.size(); ++i) {
    Character *other = (Character *)nearby.stuff[i];
    AddInventorySyncCandidate(other, candidates, seen);
    if (candidates.size() >= kInventorySweepCandidateLimit) {
      break;
    }
  }

  for (size_t i = 0; i < candidates.size(); ++i) {
    SyncInventoryForCharacter(candidates[i], false, "periodic");
  }

  static DWORD lastPruneTick = 0;
  if (nowTick - lastPruneTick >= 60000) {
    lastPruneTick = nowTick;
    PruneInventorySyncState();
  }
}

static Character *FindCharacterBySerial(GameWorld *world, unsigned int serial) {
  if (!world || serial == 0) {
    return nullptr;
  }
  const ogre_unordered_set<Character *>::type &chars =
      world->getCharacterUpdateList();
  for (auto it = chars.begin(); it != chars.end(); ++it) {
    Character *c = *it;
    if (!c || (uintptr_t)c < 0x1000) {
      continue;
    }
    try {
      if (c->getHandle().serial == serial) {
        return c;
      }
    } catch (...) {
    }
  }
  return nullptr;
}

static Character *ResolveCharacterFromHandSafe(GameWorld *world,
                                               const hand &characterHand) {
  if (!world || !characterHand.isValid()) {
    return nullptr;
  }

  Character *resolved = FindCharacterBySerial(world, characterHand.serial);
  if (resolved && (uintptr_t)resolved > 0x1000) {
    return resolved;
  }

  try {
    resolved = characterHand.getCharacter();
  } catch (...) {
    resolved = nullptr;
  }

  if (!resolved || (uintptr_t)resolved <= 0x1000) {
    return nullptr;
  }
  return resolved;
}

static void ClearAllJobsForFollow(Character *follower) {
  if (!follower || (uintptr_t)follower <= 0x1000) {
    return;
  }
  try {
    follower->clearPermajobs();
  } catch (...) {
  }
  static const TaskType kFollowCancelledJobs[] = {
      GO_HOMEBUILDING,
      STAND_AT_SHOPKEEPER_NODE,
      IDLE,
      WANDERER,
      WANDER_TOWN,
      PATROL_TOWN,
      STAND_AT_GUARD_NODE_HOMEBUILDING_IN_OUT,
      STAND_AT_GENERAL_NODE,
      STAND_AT_DEFENSIVE_NODE,
      STAND_AT_BUILDING_GUARD_NODE,
      STAND_AT_BUILDING_DEFENSIVE_NODE,
      STAND_AT_NODE,
      STAND_AT_GUARD_NODE_HOMETOWN_OUTSIDE,
      STAND_AT_GUARD_NODE_HOMEBUILDING_INDOORS_ONLY,
      STAY_IN_HOME,
      MAN_THE_GATE,
      OPEN_UP_SHOP_DOORS,
      RELAX_IN_TOWN_PACKAGE,
      TRAVEL_TO_TARGET_PACKAGE,
      TRAVEL_TO_TARGET_TOWN,
      TRAVEL_TO_TARGET_TOWN_FAST,
      RUN_AWAY,
      JOB_KEEP_EVERYTHING_RUNNING,
      DELIVER_RESOURCES,
      COLLECT_OUTPUT_RESOURCE,
      FILL_MACHINE,
      OPERATE_MACHINERY,
      OPERATE_AUTOMATIC_MACHINERY,
      GET_RID_OF_RESOURCES_IN_MY_INVENTORY,
      DITCH_ALL_RESOURCES,
      AUTO_LABOURING_MINES,
      JOB_MEDIC,
      FIRST_AID_ORDER,
      FIRST_AID_ROBOT,
      SPLINT_ORDER,
      SPLINT_JOB,
      JOB_REPAIR_ROBOT,
  };
  for (size_t i = 0; i < sizeof(kFollowCancelledJobs) / sizeof(kFollowCancelledJobs[0]); ++i) {
    try {
      follower->removeJob(kFollowCancelledJobs[i]);
    } catch (...) {
    }
  }
}

static void ApplyFollowTargets(GameWorld *world) {
  if (!world) {
    return;
  }

  static DWORD lastFollowTick = 0;
  DWORD nowTick = GetTickCount();
  if (nowTick - lastFollowTick < 250) {
    return;
  }
  lastFollowTick = nowTick;

  std::map<unsigned int, hand> followSnapshot = SnapshotFollowTargets();
  if (followSnapshot.empty()) {
    return;
  }

  static std::map<unsigned int, DWORD> s_followScrubTick;
  static std::map<unsigned int, DWORD> s_followPermajobScrubTick;
  static std::map<unsigned int, DWORD> s_followGoalRefreshTick;
  for (auto it = followSnapshot.begin(); it != followSnapshot.end(); ++it) {
    unsigned int followerSerial = it->first;
    hand targetHand = it->second;
    if (followerSerial == 0 || !targetHand.isValid()) {
      ClearFollowTarget(followerSerial);
      continue;
    }

    Character *follower = FindCharacterBySerial(world, followerSerial);
    Character *target = nullptr;
    try {
      target = targetHand.getCharacter();
    } catch (...) {
      target = nullptr;
    }

    if (!follower || (uintptr_t)follower < 0x1000 || !target ||
        (uintptr_t)target < 0x1000) {
      continue;
    }
    if (follower == target) {
      ClearFollowTarget(followerSerial);
      continue;
    }

    Ogre::Vector3 from = follower->getPosition();
    Ogre::Vector3 to = target->getPosition();
    float dx = from.x - to.x;
    float dy = from.y - to.y;
    float dz = from.z - to.z;
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    try {
      DWORD &lastGoalScrubTick = s_followScrubTick[followerSerial];
      if (nowTick - lastGoalScrubTick >= 700) {
        lastGoalScrubTick = nowTick;
        try {
          follower->setStandingOrder((MessageForB::StandingOrder)13 /* PASSIVE */,
                                     false);
          follower->setStandingOrder((MessageForB::StandingOrder)12 /* HOLD */,
                                     false);
        } catch (...) {
        }
      }
      DWORD &lastGoalRefreshTick = s_followGoalRefreshTick[followerSerial];
      if (nowTick - lastGoalRefreshTick >= 900) {
        lastGoalRefreshTick = nowTick;
        try {
          follower->clearAllAIGoals();
        } catch (...) {
        }
        try {
          follower->addGoal(STAY_CLOSE_TO_TARGET, (RootObjectBase *)target);
        } catch (...) {
        }
        try {
          follower->reThinkCurrentAIAction();
        } catch (...) {
        }
      }
      DWORD &lastPermajobScrubTick = s_followPermajobScrubTick[followerSerial];
      if (nowTick - lastPermajobScrubTick >= 1400) {
        lastPermajobScrubTick = nowTick;
        ClearAllJobsForFollow(follower);
      }

      bool moved = false;
      CharMovement *movement = nullptr;
      try {
        movement = follower->getMovement();
      } catch (...) {
        movement = nullptr;
      }
      if (movement && (uintptr_t)movement > 0x1000) {
        try {
          movement->setDesiredSpeedOrders(RUN);
          movement->setDesiredSpeed(RUN);
        } catch (...) {
        }
      }
      if (dist > 3.0f) {
        if (movement && (uintptr_t)movement > 0x1000) {
          movement->setDestination(target, HIGH_PRIORITY);
          moved = true;
        } else {
          follower->setDestination(to, false);
          moved = true;
        }
      }
      if (moved) {
        static std::map<unsigned int, DWORD> s_followLogTick;
        DWORD &lastLogTick = s_followLogTick[followerSerial];
        if (nowTick - lastLogTick >= 2000) {
          lastLogTick = nowTick;
          Log("FOLLOW_TICK: follower=" + follower->getName() +
              " target=" + target->getName() + " dist=" + ToString(dist));
        }
      }
    } catch (...) {
      Log("FOLLOW: setDestination failed for follower serial " +
          ToString(followerSerial));
      ClearFollowTarget(followerSerial);
    }
  }
}

static void ApplyTravelTargets(GameWorld *world) {
  if (!world) {
    return;
  }

  static DWORD lastTravelTick = 0;
  DWORD nowTick = GetTickCount();
  if (nowTick - lastTravelTick < 250) {
    return;
  }
  lastTravelTick = nowTick;

  std::map<unsigned int, TravelTarget> travelSnapshot = SnapshotTravelTargets();
  if (travelSnapshot.empty()) {
    return;
  }

  static std::map<unsigned int, DWORD> s_travelOrderScrubTick;
  static std::map<unsigned int, DWORD> s_travelPermajobScrubTick;
  static std::map<unsigned int, DWORD> s_travelGoalRefreshTick;
  static std::map<unsigned int, DWORD> s_travelLogTick;

  for (auto it = travelSnapshot.begin(); it != travelSnapshot.end(); ++it) {
    unsigned int actorSerial = it->first;
    const TravelTarget &target = it->second;
    if (actorSerial == 0) {
      ClearTravelTarget(actorSerial);
      continue;
    }

    Character *actor = FindCharacterBySerial(world, actorSerial);
    if (!actor || (uintptr_t)actor < 0x1000) {
      continue;
    }

    Ogre::Vector3 from = actor->getPosition();
    Ogre::Vector3 to(target.x, target.y, target.z);
    float dx = from.x - to.x;
    float dy = from.y - to.y;
    float dz = from.z - to.z;
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (dist <= 8.0f) {
      ClearTravelTarget(actorSerial);
      s_travelOrderScrubTick.erase(actorSerial);
      s_travelPermajobScrubTick.erase(actorSerial);
      s_travelGoalRefreshTick.erase(actorSerial);
      s_travelLogTick.erase(actorSerial);
      Log("TRAVEL_TICK: reached destination actor=" + actor->getName() +
          " destination='" + target.label + "'");
      continue;
    }

    try {
      ClearFollowTarget(actorSerial);

      DWORD &lastOrderTick = s_travelOrderScrubTick[actorSerial];
      if (nowTick - lastOrderTick >= 700) {
        lastOrderTick = nowTick;
        try {
          actor->setStandingOrder((MessageForB::StandingOrder)13 /* PASSIVE */,
                                  false);
          actor->setStandingOrder((MessageForB::StandingOrder)12 /* HOLD */,
                                  false);
        } catch (...) {
        }
      }

      DWORD &lastGoalRefreshTick = s_travelGoalRefreshTick[actorSerial];
      if (nowTick - lastGoalRefreshTick >= 900) {
        lastGoalRefreshTick = nowTick;
        try {
          actor->clearAllAIGoals();
        } catch (...) {
        }
        try {
          actor->reThinkCurrentAIAction();
        } catch (...) {
        }
      }

      DWORD &lastPermajobTick = s_travelPermajobScrubTick[actorSerial];
      if (nowTick - lastPermajobTick >= 1400) {
        lastPermajobTick = nowTick;
        ClearAllJobsForFollow(actor);
      }

      try {
        if (actor->dialogue && (uintptr_t)actor->dialogue > 0x1000) {
          actor->dialogue->endDialogue(true);
          actor->dialogue->setInDialog(false);
        }
      } catch (...) {
      }

      bool moved = false;
      CharMovement *movement = nullptr;
      try {
        movement = actor->getMovement();
      } catch (...) {
        movement = nullptr;
      }
      if (movement && (uintptr_t)movement > 0x1000) {
        try {
          movement->setDesiredSpeedOrders(RUN);
          movement->setDesiredSpeed(RUN);
        } catch (...) {
        }
        try {
          movement->setDestination(to, HIGH_PRIORITY, false);
          moved = true;
        } catch (...) {
          moved = false;
        }
        if (!moved) {
          try {
            movement->setDestination(to, HIGH_PRIORITY, true);
            moved = true;
          } catch (...) {
            moved = false;
          }
        }
        if (!moved) {
          try {
            moved = movement->setRoadDestination(to);
          } catch (...) {
            moved = false;
          }
        }
      }
      if (!moved) {
        try {
          actor->setDestination(to, false);
          moved = true;
        } catch (...) {
          moved = false;
        }
      }
      if (!moved) {
        try {
          actor->setDestination(to, true);
          moved = true;
        } catch (...) {
          moved = false;
        }
      }

      if (moved) {
        DWORD &lastLogTick = s_travelLogTick[actorSerial];
        if (nowTick - lastLogTick >= 2000) {
          lastLogTick = nowTick;
          bool movingNow = false;
          bool pathOk = false;
          bool pathFailed = false;
          float currentSpeed = 0.0f;
          try {
            if (movement && (uintptr_t)movement > 0x1000) {
              movingNow = movement->isCurrentlyMoving();
              pathOk = movement->pathOk();
              pathFailed = movement->pathFailed();
              currentSpeed = movement->getCurrentSpeed();
            }
          } catch (...) {
          }
          Log("TRAVEL_TICK: actor=" + actor->getName() + " destination='" +
              target.label + "' dist=" + ToString(dist) +
              " moving=" + (movingNow ? "1" : "0") +
              " path_ok=" + (pathOk ? "1" : "0") +
              " path_failed=" + (pathFailed ? "1" : "0") +
              " speed=" + ToString(currentSpeed));
        }
      } else {
        Log("TRAVEL_TICK: move order failed actor_serial=" +
            ToString(actorSerial) + " destination='" + target.label + "'");
      }
    } catch (...) {
      Log("TRAVEL_TICK: exception actor_serial=" + ToString(actorSerial) +
          " destination='" + target.label + "'");
      ClearTravelTarget(actorSerial);
      s_travelOrderScrubTick.erase(actorSerial);
      s_travelPermajobScrubTick.erase(actorSerial);
      s_travelGoalRefreshTick.erase(actorSerial);
      s_travelLogTick.erase(actorSerial);
    }
  }
}

void ProcessMessageQueue(GameWorld *thisptr) {
  if (TryEnterCriticalSection(&g_msgMutex)) {
    while (!g_messageQueue.empty()) {
      std::string msg = g_messageQueue.front();
      g_messageQueue.pop_front();
      Log("HOOK_MSG_PROC: Processing: " + msg);

      bool isNPCAction = (msg.find("NPC_ACTION: ") == 0);
      bool isPlayerTts = (msg.find("PLAYER_TTS: ") == 0);
      bool isPlayerSay = (msg.find("PLAYER_SAY: ") == 0);
      bool isNPCSay = (msg.find("NPC_SAY: ") == 0);
      bool isNotify = (msg.find("NOTIFY:") == 0);
      bool isCmd = (msg.find("CMD:") == 0);
      bool isRename = (msg.find("NPC_RENAME: ") == 0);

      hand targetHand = g_talkTargetHand;
      // Do not fall back to selection yet; handles inside the branches

      if (isCmd) {
        size_t firstColon = msg.find(":", 4); // skip "CMD: "
        if (firstColon != std::string::npos) {
          std::string command = msg.substr(4, firstColon - 4);
          std::string data = msg.substr(firstColon + 1);

          // Trim command and data
          auto trim = [](std::string &s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
          };
          trim(command);
          // Do not trim data, it may contain multiline blocks we want to keep
          // exactly

          if (command == "TRIGGER_BORED") {
            g_triggerBoredEvent = true;
          } else if (command == "POPULATE_WELCOME") {
            PopulateSettingsUI(data);
          } else if (command == "POPULATE_AINPCINFO") {
            PopulateAiNpcInfoUI(data);
          } else if (command == "SET_AINPCINFO_TEXT") {
            SetAiNpcInfoText(data);
          } else if (command == "POPULATE_AIDIARIES") {
            PopulateAiDiaryUI(data);
          } else if (command == "SET_AIDIARY_TEXT") {
            SetAiDiaryText(data);
          } else if (command == "SET_CONFIG") {
            size_t colon = data.find(":");
            if (colon != std::string::npos) {
              std::string var = data.substr(0, colon);
              std::string val = data.substr(colon + 1);

              // Trim var and val
              auto trimInternal = [](std::string &s) {
                s.erase(0, s.find_first_not_of(" \t\r\n"));
                s.erase(s.find_last_not_of(" \t\r\n") + 1);
              };
              trimInternal(var);
              trimInternal(val);

              if (var == "g_enableBoredEvents") {
                g_enableBoredEvents = (val == "1");
                g_lastBoredEventTick = GetTickCount(); // Reset timer on toggle
              } else if (var == "g_boredEventIntervalSeconds") {
                g_boredEventIntervalSeconds = atoi(val.c_str());
                g_lastBoredEventTick =
                    GetTickCount(); // Reset timer on frequency change
              } else if (var == "g_proximityRadius")
                g_proximityRadius = (float)atof(val.c_str());
              else if (var == "g_boredEventRange")
                g_boredEventRange = (float)atof(val.c_str());
              else if (var == "g_shoutRadius")
                g_shoutRadius = (float)atof(val.c_str());
              else if (var == "g_minFactionRelation")
                g_minFactionRelation = (float)atof(val.c_str());
              else if (var == "g_maxFactionRelation")
                g_maxFactionRelation = (float)atof(val.c_str());
              else if (var == "g_dialogueSpeedSeconds") {
                g_dialogueSpeedSeconds = atoi(val.c_str());
                g_lastDialogueTick =
                    GetTickCount(); // Reset timer on speed change
              } else if (var == "g_speechBubbleLife") {
                g_speechBubbleLife = (float)atof(val.c_str());
              }
            }
          } else if (command == "POPULATE_SETTINGS") {
            PopulateSettingsUI(data);
          }
        }
      } else if (isRename) {
        // Format: "NPC_RENAME: <serial>|<newName>"
        std::string payload = msg.substr(12); // skip "NPC_RENAME: "
        size_t sep = payload.find('|');
        if (sep != std::string::npos) {
          unsigned int serial =
              (unsigned int)strtoul(payload.substr(0, sep).c_str(), NULL, 10);
          std::string newName = payload.substr(sep + 1);
          if (serial > 0 && !newName.empty() && thisptr) {
            const ogre_unordered_set<Character *>::type &chars =
                thisptr->getCharacterUpdateList();
            for (auto it = chars.begin(); it != chars.end(); ++it) {
              if (*it && (uintptr_t)*it > 0x1000 &&
                  (*it)->getHandle().serial == serial) {
                std::string oldName = (*it)->getName();
                (*it)->setName(newName);
                Log("NAME_ASSIGN: Renamed '" + oldName + "' -> '" + newName +
                    "' (serial " + ToString(serial) + ")");
                break;
              }
            }
          }
        }
      } else if (isNotify) {
        std::string text = msg.substr(7);
        EnterCriticalSection(&g_uiMutex);
        QueuedAction act;
        act.type = ACT_NOTIFY;
        act.actor = hand();
        act.target = hand();
        act.message = text;
        act.taskValue = 0;
        g_uiActionQueue.push_back(act);
        LeaveCriticalSection(&g_uiMutex);
      } else if (isPlayerTts) {
        if (!g_ttsEnabled) {
          Log("TIMING_META: PLAYER_TTS ignored because TTS is disabled.");
          continue;
        }
        std::string payload = TrimCopy(msg.substr(12));
        size_t pipePos = payload.find('|');
        std::string hash =
            TrimCopy(pipePos == std::string::npos ? payload : payload.substr(0, pipePos));
        std::string durationStr =
            TrimCopy(pipePos == std::string::npos ? "" : payload.substr(pipePos + 1));

        bool validHash = (hash.size() == 32);
        if (validHash) {
          for (size_t i = 0; i < hash.size(); ++i) {
            const unsigned char ch = static_cast<unsigned char>(hash[i]);
            bool isHex =
                (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                (ch >= 'A' && ch <= 'F');
            if (!isHex) {
              validHash = false;
              break;
            }
          }
        }

        int durationMs = 0;
        if (!durationStr.empty()) {
          bool validDuration = true;
          for (size_t i = 0; i < durationStr.size(); ++i) {
            const unsigned char ch = static_cast<unsigned char>(durationStr[i]);
            if (ch < '0' || ch > '9') {
              validDuration = false;
              break;
            }
          }
          if (validDuration) {
            durationMs = atoi(durationStr.c_str());
          }
        }
        if (durationMs < 0) {
          durationMs = 0;
        } else if (durationMs > 600000) {
          durationMs = 600000;
        }

        Log("TIMING_META: PLAYER_TTS parsed hash_valid=" +
            std::string(validHash ? "1" : "0") +
            " dur_ms=" + ToString(durationMs));

        if (validHash && thisptr->player && thisptr->player->playerCharacters.size() > 0) {
          Character *playerSpeaker = ResolvePlayerSpeakerForCurrentTalk(thisptr);
          if (!playerSpeaker || (uintptr_t)playerSpeaker < 0x1000) {
            playerSpeaker = thisptr->player->playerCharacters[0];
          }
          hand playerHand = playerSpeaker ? playerSpeaker->getHandle() : hand();
          if (playerHand.isValid()) {
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_PLAY_TTS;
            act.actor = playerHand;
            act.target = playerHand;
            act.message = "";
            act.ttsHash = hash;
            act.taskValue = durationMs;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("TIMING_META: PLAYER_TTS queued ACT_PLAY_TTS hash=" +
                hash.substr(0, 8) + " dur_ms=" + ToString(durationMs));
            if (playerSpeaker && (uintptr_t)playerSpeaker > 0x1000) {
              Log("CHAT_SPEAKER: PLAYER_TTS speaker=" + playerSpeaker->getName() +
                  " serial=" +
                  ToString(playerSpeaker->getHandle().serial));
            }
          }
        }
      } else if (isPlayerSay || isNPCAction || isNPCSay) {
        g_lastBoredEventTick = GetTickCount();

        // ???? FIX: For PLAYER_SAY, ensure the bubble appears over the player,
        // not the target NPC.
        if (isPlayerSay) {
          Character *playerSpeaker = ResolvePlayerSpeakerForCurrentTalk(thisptr);
          if (!playerSpeaker && thisptr->player &&
              thisptr->player->playerCharacters.size() > 0) {
            playerSpeaker = thisptr->player->playerCharacters[0];
          }
          if (playerSpeaker && (uintptr_t)playerSpeaker > 0x1000) {
            targetHand = playerSpeaker->getHandle();
            Log("CHAT_SPEAKER: PLAYER_SAY speaker=" + playerSpeaker->getName() +
                " serial=" +
                ToString(playerSpeaker->getHandle().serial));
          }
        } else if (!targetHand.isValid()) {
          targetHand = g_lastSelectionHand;
        }

        std::string content = "";
        bool found = false;
        bool header_processed = false;

        if (isNPCSay || isNPCAction) {
          // AI responses should try to resolve the specific speaker if
          // possible, but if no header is found, we fall back to the current
          // talk target.
          hand fallbackHand = targetHand;
          // targetHand = hand(); // ???? BUG: Resetting here kills bubbles for
          // single-target talk.

          size_t startPos = isNPCSay ? 9 : 12;
          std::string remainder = msg.substr(startPos);
          size_t colon = remainder.find(':');

          std::string name = "";
          unsigned int tSerial = 0;

          if (colon != std::string::npos && colon < 64 && remainder[0] != '[') {
            header_processed = true;
            std::string header = remainder.substr(0, colon);
            name = header;
            size_t piper = header.find("|");
            if (piper != std::string::npos) {
              name = header.substr(0, piper);
              std::string sStr = header.substr(piper + 1);
              size_t endS = sStr.find_first_not_of("0123456789");
              if (endS != std::string::npos)
                sStr = sStr.substr(0, endS);
              tSerial = (unsigned int)strtoul(sStr.c_str(), NULL, 10);
            }

            std::string nLow = name;
            std::transform(nLow.begin(), nLow.end(), nLow.begin(), ::tolower);

            Character *bestMatch = nullptr;
            int bestScore = 0;

            const ogre_unordered_set<Character *>::type &chars =
                thisptr->getCharacterUpdateList();
            for (auto it = chars.begin(); it != chars.end(); ++it) {
              Character *c = *it;
              if (!c || (uintptr_t)c < 0x1000)
                continue;

              int score = 0;
              if (tSerial > 0 && c->getHandle().serial == tSerial)
                score = 1000;
              else {
                std::string cName = c->getName();
                if (cName == name)
                  score = 500;
                else {
                  std::string cLow = cName;
                  std::transform(cLow.begin(), cLow.end(), cLow.begin(),
                                 ::tolower);
                  if (cLow == nLow)
                    score = 400;
                  else if (cLow.find(nLow) == 0)
                    score =
                        200; // Prefix match (e.g. "Mu" -> "Mu the Wanderer")
                  else if (cLow.find(nLow) != std::string::npos)
                    score = 100; // Substring match (e.g. "Mu" -> "Murphy")
                }
              }

              if (score > bestScore) {
                bestScore = score;
                bestMatch = c;
                if (score == 1000)
                  break; // Serial match is absolute
              }
            }

            if (bestMatch && bestScore > 0) {
              targetHand = bestMatch->getHandle();
              found = true;
            }

            // Fallback sphere check if not found in update list or Score too
            // low
            if (!found && thisptr->player &&
                thisptr->player->playerCharacters.size() > 0) {
              Character *p = thisptr->player->playerCharacters[0];
              lektor<RootObject *> results;
              thisptr->getCharactersWithinSphere(
                  results, p->getPosition(), 2500.0f, 0.0f, 0.0f, 0x10, 0, p);
              for (uint32_t i = 0; i < results.size(); ++i) {
                Character *c = (Character *)results.stuff[i];
                if (!c || (uintptr_t)c < 0x1000)
                  continue;

                int score = 0;
                if (tSerial > 0 && c->getHandle().serial == tSerial)
                  score = 1000;
                else {
                  std::string cName = c->getName();
                  if (cName == name)
                    score = 500;
                  else {
                    std::string cLow = cName;
                    std::transform(cLow.begin(), cLow.end(), cLow.begin(),
                                   ::tolower);
                    if (cLow == nLow)
                      score = 400;
                    else if (cLow.find(nLow) == 0)
                      score = 200;
                    else if (cLow.find(nLow) != std::string::npos)
                      score = 100;
                  }
                }

                if (score > bestScore) {
                  bestScore = score;
                  bestMatch = c;
                  if (score == 1000)
                    break;
                }
              }
              if (bestMatch && bestScore > 0) {
                targetHand = bestMatch->getHandle();
                found = true;
              }
            }

            // Final fallback: If name matches current talk target or is empty,
            // use it
            if (!found && fallbackHand.isValid()) {
              Character *fc = ResolveCharacterFromHandSafe(thisptr, fallbackHand);
              if (fc && (uintptr_t)fc > 0x1000) {
                std::string fcName = fc->getName();
                std::transform(fcName.begin(), fcName.end(), fcName.begin(),
                               ::tolower);
                std::string nLow = name;
                std::transform(nLow.begin(), nLow.end(), nLow.begin(),
                               ::tolower);

                if (name.empty() || fcName == nLow ||
                    fcName.find(nLow) != std::string::npos) {
                  targetHand = fallbackHand;
                  found = true;
                }
              }
            }
          }

          // Strip header if we found the NPC (or if we have a fallback and it's
          // 1-on-1 talk)
          if (found || (header_processed && !found && fallbackHand.isValid())) {
            msg = (isNPCSay ? "NPC_SAY: " : "NPC_ACTION: ") +
                  remainder.substr(colon + 1);
            if (msg.length() > startPos && msg[startPos] == ' ')
              msg.erase(startPos, 1);

            // If we didn't find specific NPC but stripped header, use fallback
            if (!found && fallbackHand.isValid()) {
              targetHand = fallbackHand;
            }
          } else {
            Log("HOOK_MSG_PROC: WARNING: speaker not found: " + name);
          }
        }
      }

      if ((isNPCSay || isNPCAction) && targetHand.isValid()) {
        Character *speaker = ResolveCharacterFromHandSafe(thisptr, targetHand);
        if (speaker && (uintptr_t)speaker > 0x1000) {
          QueueIdentityRenameCandidate(speaker,
                                       isNPCSay ? "npc_say_actor"
                                                : "npc_action_actor");
        }
      }

      if (isNPCAction) {
        std::string actStr = TrimCopy(msg.substr(12));
        if (!actStr.empty()) {
          auto parseActionToken = [](const std::string &rawAction,
                                     std::string &commandOut,
                                     std::string &argumentOut) -> bool {
            std::string token = TrimCopy(rawAction);
            if (token.empty()) {
              return false;
            }

            size_t atPos = token.find('@');
            if (atPos == std::string::npos) {
              return false;
            }

            std::string command = token.substr(0, atPos);
            std::string argument = token.substr(atPos + 1);
            command = TrimCopy(command);
            argument = TrimCopy(argument);

            std::transform(command.begin(), command.end(), command.begin(),
                           ::toupper);
            if (command.empty()) {
              return false;
            }

            commandOut = command;
            argumentOut = argument;
            return true;
          };

          std::string actionCommand = "";
          std::string actionArgument = "";
          if (!parseActionToken(actStr, actionCommand, actionArgument)) {
            continue;
          }
          if (actionCommand == "RELEASE_PLAYER" ||
              actionCommand == "RELEASE_PRISONER" ||
              actionCommand == "RELEASEPLAYER" ||
              actionCommand == "STOPCARRYING") {
            actionCommand = "STOP_CARRYING";
          } else if (actionCommand == "DRINKITEM" ||
                     actionCommand == "DRINK-ITEM") {
            actionCommand = "DRINK_ITEM";
          } else if (actionCommand == "USEDRUGS" ||
                     actionCommand == "USE-DRUGS") {
            actionCommand = "USE_DRUGS";
          } else if (actionCommand == "REMOVELIMB") {
            actionCommand = "REMOVE_LIMB";
          } else if (actionCommand == "KILLTARGET" ||
                     actionCommand == "EXECUTE" ||
                     actionCommand == "MURDER") {
            actionCommand = "KILL";
          } else if (actionCommand == "TRAVELLOCATION" ||
                     actionCommand == "TRAVEL-LOCATION") {
            actionCommand = "TRAVEL_LOCATION";
          } else if (actionCommand == "USEOBJECT" ||
                     actionCommand == "USE-OBJECT") {
            actionCommand = "USE_OBJECT";
          } else if (actionCommand == "ROLEPLAYACTION" ||
                     actionCommand == "ROLEPLAY-ACTION" ||
                     actionCommand == "NOTIFY") {
            actionCommand = "ROLEPLAY_ACTION";
          }
          if (ShouldDropDuplicateNpcAction(
                  targetHand.isValid() ? targetHand.serial : 0, actionCommand,
                  actionArgument)) {
            Log("HOOK_MSG_PROC: Dropped duplicate NPC action command=" +
                actionCommand + " arg=" + actionArgument + " actor_serial=" +
                ToString((unsigned int)targetHand.serial));
            continue;
          }
          Log("HOOK_MSG_PROC: Parsed NPC action command=" + actionCommand +
              " arg=" + actionArgument + " actor_serial=" +
              ToString((unsigned int)targetHand.serial));
          Character *actionActor =
              ResolveCharacterFromHandSafe(thisptr, targetHand);
          std::string actionActorName = ResolveCharacterNameSafe(actionActor);
          std::string actionActorFaction = SafeFaction(actionActor);
          bool actionActorResolved =
              actionActor && (uintptr_t)actionActor > 0x1000;
          unsigned int actionActorSerial = 0;
          if (actionActorResolved) {
            try {
              actionActorSerial = actionActor->getHandle().serial;
            } catch (...) {
              actionActorSerial = 0;
            }
          }
          bool actionActorIsPlayerFaction = false;
          try {
            Faction *actorFaction =
                (actionActor && (uintptr_t)actionActor > 0x1000)
                    ? actionActor->getFaction()
                    : nullptr;
            actionActorIsPlayerFaction =
                (actorFaction && actorFaction->isThePlayer());
          } catch (...) {
            actionActorIsPlayerFaction = false;
          }
          std::string actionEventMessage = "action command received: " + actionCommand + "@";
          if (!actionArgument.empty()) {
            actionEventMessage += actionArgument;
          }
          LogGameEvent("infoaction", actionActorName, actionActorFaction, "None",
                       "None", actionEventMessage, targetHand.serial, 0);
          auto shouldSkipSpeakerBoundAction =
              [&](const std::string &commandName) -> bool {
            if (!targetHand.isValid() || targetHand.serial == 0 ||
                !actionActorResolved || actionActorSerial == 0 ||
                actionActorSerial != targetHand.serial) {
              Log("HOOK_MSG_PROC: " + commandName +
                  " skipped; unresolved actor reference queued_serial=" +
                  ToString((unsigned int)targetHand.serial) +
                  " resolved_serial=" + ToString(actionActorSerial));
              return true;
            }
            bool actorUnavailable = false;
            try {
              actorUnavailable =
                  actionActor->isDead() || actionActor->isUnconcious();
            } catch (...) {
              actorUnavailable = true;
            }
            if (actorUnavailable) {
              Log("HOOK_MSG_PROC: " + commandName +
                  " skipped; actor unavailable serial=" +
                  ToString(actionActorSerial));
              return true;
            }
            return false;
          };

          auto parseToggleArg = [](const std::string &raw,
                                   bool &enabledOut) -> bool {
            std::string token = TrimCopy(raw);
            std::transform(token.begin(), token.end(), token.begin(), ::toupper);
            if (token == "ON" || token == "TRUE" || token == "1" ||
                token == "ENABLE" || token == "ENABLED") {
              enabledOut = true;
              return true;
            }
            if (token == "OFF" || token == "FALSE" || token == "0" ||
                token == "DISABLE" || token == "DISABLED") {
              enabledOut = false;
              return true;
            }
            return false;
          };
          auto parseRemoveLimbPayload =
              [](const std::string &rawPayload, std::string &targetOut,
                 int &limbOut) -> bool {
            std::string payload = TrimCopy(rawPayload);
            if (payload.empty()) {
              return false;
            }

            size_t splitPos = payload.rfind('@');
            if (splitPos == std::string::npos) {
              return false;
            }

            std::string targetToken = TrimCopy(payload.substr(0, splitPos));
            std::string limbToken = TrimCopy(payload.substr(splitPos + 1));
            if (targetToken.empty() || limbToken.empty()) {
              return false;
            }

            std::transform(limbToken.begin(), limbToken.end(), limbToken.begin(),
                           ::toupper);

            int parsedLimb = -1;
            if (limbToken == "LEFT_ARM" || limbToken == "LEFTARM" ||
                limbToken == "L_ARM" || limbToken == "LARM") {
              parsedLimb = (int)RobotLimbs::LEFT_ARM;
            } else if (limbToken == "RIGHT_ARM" || limbToken == "RIGHTARM" ||
                       limbToken == "R_ARM" || limbToken == "RARM") {
              parsedLimb = (int)RobotLimbs::RIGHT_ARM;
            } else if (limbToken == "LEFT_LEG" || limbToken == "LEFTLEG" ||
                       limbToken == "L_LEG" || limbToken == "LLEG") {
              parsedLimb = (int)RobotLimbs::LEFT_LEG;
            } else if (limbToken == "RIGHT_LEG" || limbToken == "RIGHTLEG" ||
                       limbToken == "R_LEG" || limbToken == "RLEG") {
              parsedLimb = (int)RobotLimbs::RIGHT_LEG;
            } else {
              return false;
            }

            targetOut = targetToken;
            limbOut = parsedLimb;
            return true;
          };
          auto parseCatsPayload =
              [](const std::string &rawPayload, std::string &targetOut,
                 int &amountOut) -> bool {
            targetOut.clear();
            amountOut = 0;
            std::string payload = TrimCopy(rawPayload);
            if (payload.empty()) {
              return false;
            }

            auto parsePositiveAmount = [](const std::string &raw,
                                          int &outAmount) -> bool {
              std::string token = TrimCopy(raw);
              if (token.empty()) {
                return false;
              }
              for (size_t i = 0; i < token.size(); ++i) {
                unsigned char ch = (unsigned char)token[i];
                if (ch < '0' || ch > '9') {
                  return false;
                }
              }
              int parsed = atoi(token.c_str());
              if (parsed <= 0) {
                return false;
              }
              outAmount = parsed;
              return true;
            };

            size_t splitPos = payload.rfind('@');
            if (splitPos == std::string::npos) {
              return parsePositiveAmount(payload, amountOut);
            }

            std::string left = TrimCopy(payload.substr(0, splitPos));
            std::string right = TrimCopy(payload.substr(splitPos + 1));
            int parsedAmount = 0;
            if (parsePositiveAmount(right, parsedAmount)) {
              targetOut = left;
              amountOut = parsedAmount;
              return true;
            }
            if (parsePositiveAmount(left, parsedAmount)) {
              targetOut = right;
              amountOut = parsedAmount;
              return true;
            }

            return false;
          };
          auto parseGiveItemPayload =
              [](const std::string &rawPayload, std::string &targetOut,
                 std::string &itemOut) -> bool {
            targetOut.clear();
            itemOut.clear();
            std::string payload = TrimCopy(rawPayload);
            if (payload.empty()) {
              return false;
            }

            size_t splitPos = payload.find('@');
            if (splitPos == std::string::npos) {
              itemOut = payload;
              return !itemOut.empty();
            }

            std::string left = TrimCopy(payload.substr(0, splitPos));
            std::string right = TrimCopy(payload.substr(splitPos + 1));
            if (left.empty() || right.empty()) {
              return false;
            }
            targetOut = left;
            itemOut = right;
            return true;
          };
          auto parseTravelLocationPayload =
              [](const std::string &rawPayload, float &xOut, float &yOut,
                 float &zOut, std::string &labelOut) -> bool {
            std::string payload = TrimCopy(rawPayload);
            if (payload.empty()) {
              return false;
            }

            char separator = '|';
            if (payload.find('|') == std::string::npos &&
                payload.find(';') != std::string::npos) {
              separator = ';';
            }

            size_t sep1 = payload.find(separator);
            size_t sep2 =
                (sep1 == std::string::npos) ? std::string::npos
                                            : payload.find(separator, sep1 + 1);
            if (sep1 == std::string::npos || sep2 == std::string::npos) {
              return false;
            }
            size_t sep3 = payload.find(separator, sep2 + 1);

            std::string xToken = TrimCopy(payload.substr(0, sep1));
            std::string yToken = TrimCopy(payload.substr(sep1 + 1, sep2 - sep1 - 1));
            std::string zToken =
                (sep3 == std::string::npos)
                    ? TrimCopy(payload.substr(sep2 + 1))
                    : TrimCopy(payload.substr(sep2 + 1, sep3 - sep2 - 1));

            if (xToken.empty() || yToken.empty() || zToken.empty()) {
              return false;
            }

            auto parseFloatToken = [](const std::string &token,
                                      float &valueOut) -> bool {
              char *endPtr = NULL;
              float parsed = (float)strtod(token.c_str(), &endPtr);
              if (endPtr == token.c_str()) {
                return false;
              }
              while (endPtr && *endPtr != '\0') {
                if (!isspace((unsigned char)*endPtr)) {
                  return false;
                }
                ++endPtr;
              }
              if (parsed < -10000000.0f || parsed > 10000000.0f) {
                return false;
              }
              valueOut = parsed;
              return true;
            };

            if (!parseFloatToken(xToken, xOut) || !parseFloatToken(yToken, yOut) ||
                !parseFloatToken(zToken, zOut)) {
              return false;
            }

            labelOut =
                (sep3 == std::string::npos) ? "" : TrimCopy(payload.substr(sep3 + 1));
            if (!labelOut.empty()) {
              std::replace(labelOut.begin(), labelOut.end(), '@', ' ');
              std::replace(labelOut.begin(), labelOut.end(), '|', ' ');
              std::replace(labelOut.begin(), labelOut.end(), ';', ' ');
              labelOut = TrimCopy(labelOut);
            }
            return true;
          };
          auto resolveActionTargetHand = [&](const std::string &rawTarget,
                                             const hand &actorHandle) -> hand {
            std::string token = TrimCopy(rawTarget);
            if (token.empty()) {
              return hand();
            }

            // Trim leading/trailing punctuation that often appears in LLM
            // action targets (e.g. "player.", "the Whistler,").
            auto trimPunctuation = [](std::string value) -> std::string {
              while (!value.empty()) {
                char c = value.front();
                if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '(' ||
                    c == '[' || c == '{') {
                  value.erase(0, 1);
                  continue;
                }
                break;
              }
              while (!value.empty()) {
                char c = value.back();
                if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == ')' ||
                    c == ']' || c == '}' || c == '.' || c == ',' || c == '!' ||
                    c == '?' || c == ':') {
                  value.erase(value.size() - 1, 1);
                  continue;
                }
                break;
              }
              return value;
            };
            token = trimPunctuation(token);

            if (token.size() >= 2 &&
                ((token.front() == '"' && token.back() == '"') ||
                 (token.front() == '\'' && token.back() == '\''))) {
              token = TrimCopy(token.substr(1, token.size() - 2));
            }

            unsigned int wantedSerial = 0;
            bool hasSerial = false;
            size_t pipePos = token.find('|');
            if (pipePos != std::string::npos) {
              std::string serialPart = TrimCopy(token.substr(pipePos + 1));
              token = TrimCopy(token.substr(0, pipePos));
              if (!serialPart.empty()) {
                bool allDigits = true;
                for (size_t i = 0; i < serialPart.size(); ++i) {
                  unsigned char ch = (unsigned char)serialPart[i];
                  if (ch < '0' || ch > '9') {
                    allDigits = false;
                    break;
                  }
                }
                if (allDigits) {
                  wantedSerial =
                      (unsigned int)strtoul(serialPart.c_str(), NULL, 10);
                  hasSerial = (wantedSerial > 0);
                }
              }
            }
            if (!hasSerial && !token.empty()) {
              bool allDigits = true;
              for (size_t i = 0; i < token.size(); ++i) {
                unsigned char ch = (unsigned char)token[i];
                if (ch < '0' || ch > '9') {
                  allDigits = false;
                  break;
                }
              }
              if (allDigits) {
                wantedSerial = (unsigned int)strtoul(token.c_str(), NULL, 10);
                hasSerial = (wantedSerial > 0);
                token = "";
              }
            }

            std::string tokenLow = token;
            std::transform(tokenLow.begin(), tokenLow.end(), tokenLow.begin(),
                           ::tolower);
            if (tokenLow == "the player") {
              tokenLow = "player";
            } else if (tokenLow.find("the ") == 0) {
              tokenLow = TrimCopy(tokenLow.substr(4));
            } else if (tokenLow.find("a ") == 0) {
              tokenLow = TrimCopy(tokenLow.substr(2));
            } else if (tokenLow.find("an ") == 0) {
              tokenLow = TrimCopy(tokenLow.substr(3));
            }

            auto resolvePlayerHandle = [&]() -> hand {
              if (!thisptr || !thisptr->player) {
                return hand();
              }
              try {
                hand selected = thisptr->player->selectedCharacter;
                if (selected.isValid()) {
                  Character *selectedCharacter = selected.getCharacter();
                  if (selectedCharacter && (uintptr_t)selectedCharacter > 0x1000 &&
                      selectedCharacter->isPlayerCharacter()) {
                    return selected;
                  }
                }
              } catch (...) {
              }
              if (thisptr->player->playerCharacters.size() > 0 &&
                  thisptr->player->playerCharacters[0]) {
                return thisptr->player->playerCharacters[0]->getHandle();
              }
              return hand();
            };
            if (!hasSerial &&
                (tokenLow == "speaker" || tokenLow == "self")) {
              if (actorHandle.isValid()) {
                return actorHandle;
              }
              return resolvePlayerHandle();
            }
            if (!hasSerial &&
                (tokenLow == "player" || tokenLow == "me" || tokenLow == "you")) {
              return resolvePlayerHandle();
            }

            Character *bestMatch = nullptr;
            int bestScore = 0;
            if (thisptr) {
              const ogre_unordered_set<Character *>::type &chars =
                  thisptr->getCharacterUpdateList();
              for (auto it = chars.begin(); it != chars.end(); ++it) {
                Character *candidate = *it;
                if (!candidate || (uintptr_t)candidate < 0x1000) {
                  continue;
                }
                if (actorHandle.isValid() &&
                    candidate->getHandle().serial == actorHandle.serial) {
                  continue;
                }

                int score = 0;
                if (hasSerial && candidate->getHandle().serial == wantedSerial) {
                  score = 1000;
                } else if (!tokenLow.empty()) {
                  std::string candidateName = candidate->getName();
                  std::string candidateLow = candidateName;
                  std::transform(candidateLow.begin(), candidateLow.end(),
                                 candidateLow.begin(), ::tolower);
                  if (candidateLow == tokenLow) {
                    score = 500;
                  } else if (candidateLow.find(tokenLow) == 0) {
                    score = 320;
                  } else if (candidateLow.find(tokenLow) != std::string::npos) {
                    score = 180;
                  } else if (!candidate->displayName.empty()) {
                    std::string displayLow = candidate->displayName;
                    std::transform(displayLow.begin(), displayLow.end(),
                                   displayLow.begin(), ::tolower);
                    if (displayLow == tokenLow) {
                      score = 260;
                    } else if (displayLow.find(tokenLow) != std::string::npos) {
                      score = 140;
                    }
                  }
                }

                if (score > bestScore) {
                  bestScore = score;
                  bestMatch = candidate;
                  if (score == 1000) {
                    break;
                  }
                }
              }
            }

            if (!bestMatch && !hasSerial &&
                (tokenLow == "lead" || tokenLow == "leader")) {
              hand playerHandle = resolvePlayerHandle();
              if (playerHandle.isValid()) {
                return playerHandle;
              }
            }
            if (bestMatch && bestScore > 0) {
              return bestMatch->getHandle();
            }
            return hand();
          };

          if (actionCommand == "JOIN_PARTY") {
            if (!targetHand.isValid()) {
              Log("HOOK_MSG_PROC: JOIN_PARTY ignored; invalid actor handle");
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_JOIN_PARTY;
            act.actor = targetHand;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: JOIN_PARTY queued actor_serial=" +
                ToString((int)targetHand.serial));
          } else if (actionCommand == "ATTACK") {
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_ATTACK;
            act.actor = targetHand;
            hand resolvedTarget = resolveActionTargetHand(actionArgument, targetHand);
            if (!resolvedTarget.isValid() && thisptr->player &&
                thisptr->player->playerCharacters.size() > 0) {
              resolvedTarget = thisptr->player->playerCharacters[0]->getHandle();
            }
            act.target = resolvedTarget;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: ATTACK target resolved arg='" + actionArgument +
                "' target_serial=" + ToString((int)act.target.serial));
          } else if (actionCommand == "SUICIDE") {
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_SUICIDE;
            act.actor = targetHand;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: SUICIDE queued actor_serial=" +
                ToString((int)targetHand.serial));
          } else if (actionCommand == "GIVE_ITEM") {
            std::string giveItemTargetToken = "";
            std::string iName = "";
            if (!parseGiveItemPayload(actionArgument, giveItemTargetToken, iName)) {
              Log("HOOK_MSG_PROC: GIVE_ITEM ignored; invalid payload '" +
                  actionArgument + "'");
              continue;
            }
            hand giveItemTarget = hand();
            if (!giveItemTargetToken.empty()) {
              giveItemTarget =
                  resolveActionTargetHand(giveItemTargetToken, targetHand);
              if (!giveItemTarget.isValid()) {
                Log("HOOK_MSG_PROC: GIVE_ITEM ignored; target unresolved '" +
                    giveItemTargetToken + "'");
                continue;
              }
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_GIVE_ITEM;
            act.actor = targetHand;
            act.target = giveItemTarget;
            act.message = iName;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: GIVE_ITEM queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " item='" + iName +
                "' target='" + giveItemTargetToken + "' target_serial=" +
                ToString((unsigned int)giveItemTarget.serial));
          } else if (actionCommand == "TAKE_ITEM") {
            std::string iName = actionArgument;
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_TAKE_ITEM;
            act.actor = targetHand;
            act.message = iName;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
          } else if (actionCommand == "DROP_ITEM") {
            std::string iName = actionArgument;
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_DROP_ITEM;
            act.actor = targetHand;
            act.message = iName;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
          } else if (actionCommand == "DRINK_ITEM") {
            if (shouldSkipSpeakerBoundAction(actionCommand)) {
              continue;
            }
            std::string drinkItemName = TrimCopy(actionArgument);
            if (drinkItemName.empty()) {
              Log("HOOK_MSG_PROC: DRINK_ITEM ignored; empty item payload");
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_DRINK_ITEM;
            act.actor = targetHand;
            act.message = drinkItemName;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: DRINK_ITEM queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " item='" +
                drinkItemName + "'");
          } else if (actionCommand == "USE_DRUGS") {
            if (shouldSkipSpeakerBoundAction(actionCommand)) {
              continue;
            }
            std::string drugItemName = TrimCopy(actionArgument);
            if (drugItemName.empty()) {
              Log("HOOK_MSG_PROC: USE_DRUGS ignored; empty item payload");
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_USE_DRUGS;
            act.actor = targetHand;
            act.message = drugItemName;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: USE_DRUGS queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " item='" +
                drugItemName + "'");
          } else if (actionCommand == "ROLEPLAY_ACTION") {
            std::string notice = TrimCopy(actionArgument);
            if (notice.empty()) {
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_NOTIFY;
            act.actor = targetHand;
            act.target = hand();
            act.message = notice;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
          } else if (actionCommand == "TAKE_CATS") {
            std::string catsTargetToken = "";
            int catsAmount = 0;
            if (!parseCatsPayload(actionArgument, catsTargetToken, catsAmount)) {
              Log("HOOK_MSG_PROC: TAKE_CATS ignored; invalid payload '" +
                  actionArgument + "'");
              continue;
            }
            hand catsTarget = hand();
            if (!catsTargetToken.empty()) {
              catsTarget = resolveActionTargetHand(catsTargetToken, targetHand);
              if (!catsTarget.isValid()) {
                Log("HOOK_MSG_PROC: TAKE_CATS ignored; target unresolved '" +
                    catsTargetToken + "'");
                continue;
              }
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_TAKE_CATS;
            act.actor = targetHand;
            act.target = catsTarget;
            act.message = catsTargetToken;
            act.taskValue = catsAmount;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: TAKE_CATS queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " amount=" +
                ToString(catsAmount) + " target='" + catsTargetToken +
                "' target_serial=" + ToString((unsigned int)catsTarget.serial));
          } else if (actionCommand == "GIVE_CATS") {
            if (!actionActorIsPlayerFaction) {
              Log("HOOK_MSG_PROC: GIVE_CATS ignored; non-player-faction actor '" +
                  actionActorName + "' serial=" + ToString(actionActorSerial));
              continue;
            }
            std::string catsTargetToken = "";
            int catsAmount = 0;
            if (!parseCatsPayload(actionArgument, catsTargetToken, catsAmount)) {
              Log("HOOK_MSG_PROC: GIVE_CATS ignored; invalid payload '" +
                  actionArgument + "'");
              continue;
            }
            hand catsTarget = hand();
            if (!catsTargetToken.empty()) {
              catsTarget = resolveActionTargetHand(catsTargetToken, targetHand);
              if (!catsTarget.isValid()) {
                Log("HOOK_MSG_PROC: GIVE_CATS ignored; target unresolved '" +
                    catsTargetToken + "'");
                continue;
              }
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_GIVE_CATS;
            act.actor = targetHand;
            act.target = catsTarget;
            act.message = catsTargetToken;
            act.taskValue = catsAmount;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: GIVE_CATS queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " amount=" +
                ToString(catsAmount) + " target='" + catsTargetToken +
                "' target_serial=" + ToString((unsigned int)catsTarget.serial));
          } else if (actionCommand == "LEAVE") {
            if (!targetHand.isValid()) {
              Log("HOOK_MSG_PROC: LEAVE ignored; invalid actor handle");
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_LEAVE;
            act.actor = targetHand;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: LEAVE queued actor_serial=" +
                ToString((int)targetHand.serial));
          } else if (actionCommand == "FACTION_RELATIONS") {
            std::string payload = actionArgument;
            size_t delimiterPos = payload.find('@');
            if (delimiterPos != std::string::npos) {
              std::string targetName = TrimCopy(payload.substr(0, delimiterPos));
              std::string deltaToken =
                  TrimCopy(payload.substr(delimiterPos + 1));
              int amountRaw = atoi(deltaToken.c_str());
              if (targetName.empty() || amountRaw == 0) {
                Log("HOOK_MSG_PROC: FACTION_RELATIONS ignored; invalid payload '" +
                    payload + "'");
                continue;
              }
              int amount = (amountRaw < 0) ? -100 : 100;
              hand relationTarget =
                  resolveActionTargetHand(targetName, targetHand);
              EnterCriticalSection(&g_uiMutex);
              QueuedAction act;
              act.type = ACT_FACTION_RELATIONS;
              act.actor = targetHand;
              act.target = relationTarget;
              // Keep the raw target token as a compatibility fallback
              // when hand resolution fails.
              act.message = targetName;
              act.taskValue = amount;
              g_uiActionQueue.push_back(act);
              LeaveCriticalSection(&g_uiMutex);
              if (!relationTarget.isValid()) {
                Log("HOOK_MSG_PROC: FACTION_RELATIONS fallback to name token '" +
                    targetName + "' (no target hand match)");
              } else {
                Log("HOOK_MSG_PROC: FACTION_RELATIONS target resolved name='" +
                    targetName + "' serial=" +
                    ToString((unsigned int)relationTarget.serial) +
                    " delta=" + ToString(amount));
              }
            }
          } else if (actionCommand == "REMOVE_LIMB") {
            if (shouldSkipSpeakerBoundAction(actionCommand)) {
              continue;
            }
            std::string targetToken = "";
            int limbCode = -1;
            if (!parseRemoveLimbPayload(actionArgument, targetToken, limbCode)) {
              Log("HOOK_MSG_PROC: REMOVE_LIMB ignored; invalid payload '" +
                  actionArgument + "'");
              continue;
            }
            hand limbTarget = resolveActionTargetHand(targetToken, targetHand);
            if (!limbTarget.isValid()) {
              Log("HOOK_MSG_PROC: REMOVE_LIMB ignored; target unresolved '" +
                  targetToken + "'");
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_REMOVE_LIMB;
            act.actor = targetHand;
            act.target = limbTarget;
            act.message = targetToken;
            act.taskValue = limbCode;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: REMOVE_LIMB queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " target='" +
                targetToken + "' target_serial=" +
                ToString((unsigned int)limbTarget.serial) + " limb_code=" +
                ToString(limbCode));
          } else if (actionCommand == "KILL") {
            if (shouldSkipSpeakerBoundAction(actionCommand)) {
              continue;
            }
            std::string targetToken = TrimCopy(actionArgument);
            if (targetToken.empty()) {
              Log("HOOK_MSG_PROC: KILL ignored; empty target payload");
              continue;
            }
            hand killTarget = resolveActionTargetHand(targetToken, targetHand);
            if (!killTarget.isValid()) {
              Log("HOOK_MSG_PROC: KILL ignored; target unresolved '" +
                  targetToken + "'");
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_KILL;
            act.actor = targetHand;
            act.target = killTarget;
            act.message = targetToken;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: KILL queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " target='" +
                targetToken + "' target_serial=" +
                ToString((unsigned int)killTarget.serial));
          } else if (actionCommand == "SPAWN_ITEM") {
            Log("HOOK_MSG_PROC: SPAWN_ITEM ignored; action currently disabled. arg='" +
                actionArgument + "'");
            thisptr->showPlayerAMessage_withLog(
                "Spawn item action is currently disabled.", true);
            continue;
          } else if (actionCommand == "TRAVEL_LOCATION") {
            if (!targetHand.isValid()) {
              Log("HOOK_MSG_PROC: TRAVEL_LOCATION ignored; invalid actor handle");
              continue;
            }

            float travelX = 0.0f;
            float travelY = 0.0f;
            float travelZ = 0.0f;
            std::string travelLabel = "";
            if (!parseTravelLocationPayload(actionArgument, travelX, travelY,
                                            travelZ, travelLabel)) {
              std::string destinationLabel = TrimCopy(actionArgument);
              if (destinationLabel.empty()) {
                destinationLabel = "that location";
              }
              thisptr->showPlayerAMessage_withLog(
                  "Can not travel to " + destinationLabel +
                      " as you have not visited it yet",
                  true);
              Log("HOOK_MSG_PROC: TRAVEL_LOCATION invalid payload '" +
                  actionArgument + "'");
              continue;
            }

            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_TRAVEL_LOCATION;
            act.actor = targetHand;
            act.message = actionArgument;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: TRAVEL_LOCATION queued actor_serial=" +
                ToString((int)targetHand.serial) + " payload='" + actionArgument +
                "'");
          } else if (actionCommand == "USE_OBJECT") {
            if (shouldSkipSpeakerBoundAction(actionCommand)) {
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_USE_OBJECT;
            act.actor = targetHand;
            act.message = actionArgument;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: USE_OBJECT queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " target='" +
                actionArgument + "'");
          } else if (actionCommand == "FOLLOW") {
            if (actionActorIsPlayerFaction) {
              Log("HOOK_MSG_PROC: FOLLOW blocked for player-faction NPC actor=" +
                  actionActorName + " serial=" +
                  ToString((int)targetHand.serial));
              thisptr->showPlayerAMessage_withLog(
                  actionActorName +
                      " cannot use follow actions while in your faction.",
                  true);
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_START_FOLLOW;
            act.actor = targetHand;
            hand followTarget = resolveActionTargetHand(actionArgument, targetHand);
            if (!followTarget.isValid()) {
              LeaveCriticalSection(&g_uiMutex);
              Log("HOOK_MSG_PROC: FOLLOW ignored; could not resolve target from '" +
                  actionArgument + "'");
              continue;
            }
            act.target = followTarget;
            act.taskValue = 0;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: FOLLOW target resolved arg='" + actionArgument +
                "' target_serial=" + ToString((unsigned int)act.target.serial));
          } else if (actionCommand == "STOP_FOLLOW") {
            if (actionActorIsPlayerFaction) {
              Log("HOOK_MSG_PROC: STOP_FOLLOW blocked for player-faction NPC actor=" +
                  actionActorName + " serial=" +
                  ToString((int)targetHand.serial));
              thisptr->showPlayerAMessage_withLog(
                  actionActorName +
                      " cannot use follow actions while in your faction.",
                  true);
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_STOP_FOLLOW;
            act.actor = targetHand;
            act.taskValue = 0;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
          } else if (actionCommand == "IDLE") {
            if (shouldSkipSpeakerBoundAction(actionCommand)) {
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_SET_TASK;
            act.actor = targetHand;
            act.taskValue = 14; // IDLE
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
          } else if (actionCommand == "SET_BLOCK" || actionCommand == "SET_HOLD" ||
                     actionCommand == "SET_PASSIVE" || actionCommand == "SET_JOBS" ||
                     actionCommand == "SET_RANGED" || actionCommand == "SET_TAUNT" ||
                     actionCommand == "SET_SNEAK" || actionCommand == "SET_RESOURCE" ||
                     actionCommand == "SET_MEDIC") {
            bool enabled = false;
            if (!parseToggleArg(actionArgument, enabled)) {
              Log("HOOK_MSG_PROC: Ignoring invalid toggle action argument for " +
                  actionCommand + ": " + actionArgument);
              continue;
            }
            if (shouldSkipSpeakerBoundAction(actionCommand)) {
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_SET_NPC_TOGGLE;
            act.actor = targetHand;
            act.message = actionCommand;
            act.taskValue = enabled ? 1 : 0;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
          } else if (actionCommand == "STOP_CARRYING") {
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_RELEASE;
            act.actor = targetHand;
            act.message = actionArgument;
            hand releaseTarget = resolveActionTargetHand(actionArgument, targetHand);
            if (!releaseTarget.isValid() && actionActor &&
                (uintptr_t)actionActor > 0x1000) {
              try {
                if (actionActor->isCarryingSomething &&
                    actionActor->carryingObject.isValid()) {
                  releaseTarget = actionActor->carryingObject;
                }
              } catch (...) {
              }
            }
            act.target = releaseTarget;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: STOP_CARRYING target resolved arg='" +
                actionArgument + "' target_serial=" +
                ToString((int)act.target.serial));
          } else if (actionCommand == "TASK") {
            std::string tName = actionArgument;
            std::transform(tName.begin(), tName.end(), tName.begin(), ::toupper);
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_SET_TASK;
            act.actor = targetHand;

            // Set default target to player for player-given orders
            if (thisptr->player && thisptr->player->playerCharacters.size() > 0)
              act.target = thisptr->player->playerCharacters[0]->getHandle();

            // Correct mapping for TaskType (NULL_TASK = 0)
            act.taskValue = 24; // Default to WANDERER
            if (tName == "IDLE")
              act.taskValue = 14;
            else if (tName == "RUN_AWAY")
              act.taskValue = 35;
            else if (tName == "CHASE")
              act.taskValue = 46;
            else if (tName == "MOVE_ON_FREE_WILL")
              act.taskValue = 1;
            else if (tName == "MELEE_ATTACK") {
              act.taskValue = 4;
            } else if (tName == "RELEASE_PRISONER") {
              act.taskValue = 110;
            }

            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
          }
        }
      } else {
        // Normal dialogue bubble
        std::string bubbleContent =
            isPlayerSay ? msg.substr(12) : (isNPCSay ? msg.substr(9) : "");
        std::string structuredMessage =
            ExtractDialogueMessageFromStructuredText(bubbleContent);
        if (!structuredMessage.empty()) {
          bubbleContent = structuredMessage;
        }
        int ttsDurationMs = ExtractTrailingTtsDurationMs(bubbleContent);
        std::string ttsHash = ExtractTrailingTtsHash(bubbleContent);
        const bool hadStructuredMessage = !structuredMessage.empty();
        const bool hadTtsMetadata = !ttsHash.empty() || ttsDurationMs > 0;
        if (!g_ttsEnabled) {
          ttsDurationMs = 0;
          ttsHash.clear();
        }
        if (isPlayerSay || hadTtsMetadata) {
          Log("TIMING_META: bubble parse kind=" +
              std::string(isPlayerSay ? "player" : "npc") +
              " text_len=" + ToString((int)bubbleContent.length()) +
              " tts_hash=" + (ttsHash.empty() ? "" : ttsHash.substr(0, 8)) +
              " tts_dur_ms=" + ToString(ttsDurationMs));
        }

        // Suppress bubbles for test commands
        if (!bubbleContent.empty()) {
          if (isPlayerSay && bubbleContent[0] == '/')
            bubbleContent = "";
          else if (isNPCSay && bubbleContent.find("[DEBUG]") == 0)
            bubbleContent = "";
        }

        if (!bubbleContent.empty()) {
          Character *tc = ResolveCharacterFromHandSafe(thisptr, targetHand);
          if (!tc && !isPlayerSay) {
            tc = ResolveCharacterFromHandSafe(thisptr, g_talkTargetHand);
          }
          if (!tc && !isPlayerSay) {
            tc = ResolveCharacterFromHandSafe(thisptr, g_lastSelectionHand);
          }
          if (!tc && isPlayerSay) {
            tc = ResolvePlayerSpeakerForCurrentTalk(thisptr);
            if (!tc && thisptr->player && thisptr->player->playerCharacters.size() > 0) {
              tc = thisptr->player->playerCharacters[0];
            }
          }

          Log("HOOK_MSG_PROC: Queuing SAY for " +
              (tc ? tc->getName() : "Unknown") + ": " + bubbleContent);

          if (tc && (uintptr_t)tc > 0x1000) {
            SyncInventoryForCharacter(tc, true,
                                      isNPCSay ? "dialogue_npc" : "dialogue_player");
          }

          bool trackAsNonAiDialogue = !hadStructuredMessage && !hadTtsMetadata;
          std::string nonAiDialogueLine = bubbleContent;
          if (trackAsNonAiDialogue) {
            nonAiDialogueLine = SanitizeCapturedDialogueLine(nonAiDialogueLine);
            if (nonAiDialogueLine.empty()) {
              trackAsNonAiDialogue = false;
            }
          }
          unsigned int speakerSerial = 0;
          if (tc && (uintptr_t)tc > 0x1000) {
            try {
              speakerSerial = tc->getHandle().serial;
            } catch (...) {
              speakerSerial = 0;
            }
          }
          const std::string &dedupeLine =
              trackAsNonAiDialogue ? nonAiDialogueLine : bubbleContent;
          bool duplicateDialogueLine =
              ShouldDropDuplicateNonAiDialogue(speakerSerial, isPlayerSay,
                                               dedupeLine);

          if (trackAsNonAiDialogue && !duplicateDialogueLine) {
            // Non-AI/ambient dialogue should be stored as base speech with no target.
            std::string listenerName = "None";
            std::string listenerFaction = "None";
            unsigned int listenerSerial = 0;

            std::string speakerName = (tc && (uintptr_t)tc > 0x1000)
                                          ? ResolveCharacterNameSafe(tc)
                                          : (isPlayerSay ? "Player" : "Nearby NPC");
            std::string speakerFaction =
                (tc && (uintptr_t)tc > 0x1000) ? SafeFaction(tc) : "None";

            LogGameEvent("chat", speakerName, speakerFaction, listenerName,
                         listenerFaction, nonAiDialogueLine, speakerSerial,
                         listenerSerial);
          }

          if (targetHand.isValid()) {
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_SAY;
            act.actor = targetHand;
            act.target = targetHand;
            act.message = bubbleContent;
            act.ttsHash = ttsHash;
            act.taskValue = ttsDurationMs;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("TIMING_META: queued ACT_SAY target=" +
                (tc ? tc->getName() : "Unknown") +
                " tts_hash=" + (ttsHash.empty() ? "" : ttsHash.substr(0, 8)) +
                " tts_dur_ms=" + ToString(ttsDurationMs));
          } else {
            Log("HOOK_MSG_PROC: SAY fallback logged without target hand");
          }
        }
      }
    }
    LeaveCriticalSection(&g_msgMutex);
  }
}

void attackingYou_hook(Character *npc, Character *attacker, bool so,
                       bool doAwarenessCheck) {
  if (attacker && npc) {
    LogGameEvent("combat", attacker->getName(), SafeFaction(attacker),
                 npc->getName(), SafeFaction(npc), "Initiated attack",
                 ResolveCharacterSerialForEvent(attacker),
                 ResolveCharacterSerialForEvent(npc));
  }
  if (attackingYou_orig)
    attackingYou_orig(npc, attacker, so, doAwarenessCheck);
}

void applyDamage_hook(MedicalSystem::HealthPartStatus *part,
                      const Damages &damage) {
  if (part && part->me && damage.total() > 15.0f) {
    LogGameEvent("combat", "Unknown", "None", part->me->getName(),
                 SafeFaction(part->me),
                 "Took substantial damage: " + ToString((int)damage.total()), 0,
                 ResolveCharacterSerialForEvent(part->me));
  }
  if (applyDamage_orig)
    applyDamage_orig(part, damage);
}

bool applyFirstAid_hook(MedicalSystem *med, float skill, Item *equipment,
                        float frameTIME, Character *who) {
  bool res = false;
  if (applyFirstAid_orig)
    res = applyFirstAid_orig(med, skill, equipment, frameTIME, who);
  if (res && med && med->me && who) {
    LogGameEvent("healing", who->getName(), SafeFaction(who),
                 med->me->getName(), SafeFaction(med->me),
                 "Applying first aid", ResolveCharacterSerialForEvent(who),
                 ResolveCharacterSerialForEvent(med->me));
  }
  return res;
}

Item *buyItem_hook(Inventory *inv, Item *itemToBuy, RootObject *sendingTo) {
  if (inv && itemToBuy && sendingTo) {
    LogGameEvent("trade", ((Character *)sendingTo)->getName(),
                 SafeFaction((Character *)sendingTo), inv->owner->getName(),
                 SafeFaction(inv->owner), "Bought " + itemToBuy->getName(),
                 ResolveCharacterSerialForEvent((Character *)sendingTo),
                 0);
  }
  if (buyItem_orig)
    return buyItem_orig(inv, itemToBuy, sendingTo);
  return nullptr;
}

static bool HasActiveSpeechBubbleSafe(Character *npc) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }
  try {
    if (!npc->dialogue || (uintptr_t)npc->dialogue <= 0x1000) {
      return false;
    }
    return npc->dialogue->speechTextTimer > 0.05f ||
           npc->dialogue->speechTextTimer_forced > 0.05f;
  } catch (...) {
    return false;
  }
}

static std::string ReadNpcSpeechLineSafe(Character *npc) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return "";
  }
  try {
    if (!npc->dialogue || (uintptr_t)npc->dialogue <= 0x1000) {
      return "";
    }
    return TrimCopy(npc->dialogue->npcReplyText);
  } catch (...) {
    return "";
  }
}

static void ClearSpeechBubbleImmediateSafe(Character *npc) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return;
  }
  try {
    if (!npc->dialogue || (uintptr_t)npc->dialogue <= 0x1000) {
      return;
    }
    npc->dialogue->speechTextTimer = 0.0f;
    npc->dialogue->speechTextTimer_forced = 0.0f;
  } catch (...) {
  }
}

static bool IsNpcInSpeechFlowBySerial(unsigned int serial) {
  if (serial == 0) {
    return false;
  }

  bool inFlow = false;
  if (TryEnterCriticalSection(&g_stateMutex)) {
    try {
      inFlow = g_talkTargetHand.isValid() && g_talkTargetHand.serial == serial;
    } catch (...) {
      inFlow = false;
    }
    LeaveCriticalSection(&g_stateMutex);
  }
  if (inFlow) {
    return true;
  }

  if (!TryEnterCriticalSection(&g_uiMutex)) {
    return false;
  }
  for (std::deque<QueuedAction>::const_iterator it = g_uiActionQueue.begin();
       it != g_uiActionQueue.end(); ++it) {
    const QueuedAction &act = *it;
    if (act.type != ACT_SAY && act.type != ACT_PLAY_TTS) {
      continue;
    }
    if ((act.actor.isValid() && act.actor.serial == serial) ||
        (act.target.isValid() && act.target.serial == serial)) {
      inFlow = true;
      break;
    }
  }
  LeaveCriticalSection(&g_uiMutex);
  return inFlow;
}

static void InterruptConversationForUnavailableNpc(Character *npc,
                                                   const std::string &reason) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return;
  }
  unsigned int serial = ResolveCharacterSerialForEvent(npc);
  if (serial == 0) {
    return;
  }

  const bool inSpeechFlow = IsNpcInSpeechFlowBySerial(serial);
  const bool hadBubble = HasActiveSpeechBubbleSafe(npc);
  if (!inSpeechFlow && !hadBubble) {
    return;
  }

  ClearSpeechBubbleImmediateSafe(npc);
  LONG generation = BeginChatInterruptGeneration();
  Log("CHAT_INTERRUPT: npc unavailable reason=" + reason +
      " npc=" + ResolveCharacterNameSafe(npc) + " serial=" +
      ToString(serial) + " gen=" + ToString((int)generation) +
      " in_flow=" + (inSpeechFlow ? "1" : "0") +
      " had_bubble=" + (hadBubble ? "1" : "0"));
}

static std::string ResolveDialogueReplyTextByIndexSafe(Dialogue *dialogue,
                                                       int replyIndex) {
  if (!dialogue || (uintptr_t)dialogue <= 0x1000 || replyIndex < 0) {
    return "";
  }
  try {
    size_t idx = static_cast<size_t>(replyIndex);
    if (idx < dialogue->responses.size()) {
      return TrimCopy(dialogue->responses[idx]);
    }
  } catch (...) {
  }
  return "";
}

static std::string ResolveDialogueReplyTextByTokenSafe(Dialogue *dialogue,
                                                       const std::string &token) {
  if (!dialogue || (uintptr_t)dialogue <= 0x1000) {
    return "";
  }

  std::string trimmedToken = TrimCopy(token);
  if (trimmedToken.empty()) {
    return "";
  }

  bool numeric = true;
  for (size_t i = 0; i < trimmedToken.length(); ++i) {
    unsigned char ch = static_cast<unsigned char>(trimmedToken[i]);
    if (ch < '0' || ch > '9') {
      numeric = false;
      break;
    }
  }
  if (numeric) {
    int idx = atoi(trimmedToken.c_str());
    return ResolveDialogueReplyTextByIndexSafe(dialogue, idx);
  }

  std::string wanted = ToLowerAsciiCopy(trimmedToken);
  try {
    for (size_t i = 0; i < dialogue->replyIds.size(); ++i) {
      std::string candidate = TrimCopy(dialogue->replyIds[i]);
      if (candidate.empty()) {
        continue;
      }
      if (ToLowerAsciiCopy(candidate) != wanted) {
        continue;
      }
      if (i < dialogue->responses.size()) {
        return TrimCopy(dialogue->responses[i]);
      }
      break;
    }
  } catch (...) {
  }

  return "";
}

static void CapturePlayerDialogueReplyFromUi(Dialogue *dialogue,
                                             const std::string &rawReplyText,
                                             const char *sourceTag) {
  std::string line = TrimCopy(rawReplyText);
  if (line.empty() || line.find("[DEBUG]") == 0) {
    return;
  }

  std::string structured = ExtractDialogueMessageFromStructuredText(line);
  if (!structured.empty()) {
    line = structured;
  }
  line = SanitizeCapturedDialogueLine(line);
  if (line.empty()) {
    return;
  }

  GameWorld *world = GetWorldSafe();
  if (!world) {
    return;
  }

  Character *dialogueOwner = nullptr;
  if (dialogue && (uintptr_t)dialogue > 0x1000) {
    try {
      dialogueOwner = dialogue->getCharacter();
    } catch (...) {
      dialogueOwner = nullptr;
    }
  }

  Character *speaker = ResolveNearestPlayerSpeakerForTarget(world, dialogueOwner);
  if (!speaker && world->player && world->player->playerCharacters.size() > 0) {
    speaker = world->player->playerCharacters[0];
  }
  if (!speaker || (uintptr_t)speaker <= 0x1000) {
    return;
  }

  unsigned int speakerSerial = ResolveCharacterSerialForEvent(speaker);
  if (ShouldDropDuplicateNonAiDialogue(speakerSerial, true, line)) {
    return;
  }
  if (speakerSerial != 0 && IsNpcInSpeechFlowBySerial(speakerSerial)) {
    return;
  }

  Character *listener = nullptr;
  std::string listenerSource = "";
  if (dialogueOwner && (uintptr_t)dialogueOwner > 0x1000 &&
      dialogueOwner != speaker) {
    listener = dialogueOwner;
    listenerSource = "dialogue_owner";
  }
  if (!listener) {
    listener = ResolveDialogueListenerForSpeech(speaker, dialogue, listenerSource);
  }

  std::string listenerName = "None";
  std::string listenerFaction = "None";
  unsigned int listenerSerial = 0;
  if (listener && (uintptr_t)listener > 0x1000) {
    listenerName = ResolveCharacterNameSafe(listener);
    listenerFaction = SafeFaction(listener);
    listenerSerial = ResolveCharacterSerialForEvent(listener);
  }

  bool listenerIsPlayerCharacter = false;
  if (listener && (uintptr_t)listener > 0x1000) {
    try {
      listenerIsPlayerCharacter = listener->isPlayerCharacter();
    } catch (...) {
      listenerIsPlayerCharacter = false;
    }
  }

  bool listenerIsTrader = false;
  if (listener && (uintptr_t)listener > 0x1000 && !listenerIsPlayerCharacter) {
    try {
      listenerIsTrader = listener->isATrader();
    } catch (...) {
      listenerIsTrader = false;
    }
  }

  bool likelyTradeReply = IsLikelyTradeDialogueReply(line);
  if (listener && (uintptr_t)listener > 0x1000 && !listenerIsPlayerCharacter &&
      (likelyTradeReply || listenerIsTrader)) {
    bool capturedTraderInventory =
        CaptureTraderInventorySnapshot(listener, "dialogue_reply_trade");
    Log("TRADER_INVENTORY_CAPTURE: reply-triggered listener=" +
        ResolveCharacterNameSafe(listener) + " serial=" +
        ToString(listenerSerial) +
        " success=" + std::string(capturedTraderInventory ? "1" : "0") +
        " reason=" +
        std::string(likelyTradeReply ? "trade_reply" : "listener_is_trader"));
    if (capturedTraderInventory) {
      PushImmediateContextSnapshot(listener, "dialogue_reply_trade_capture",
                                   true);
    }
  }

  LogGameEvent("chat", ResolveCharacterNameSafe(speaker), SafeFaction(speaker),
               listenerName, listenerFaction, line, speakerSerial, listenerSerial);
  Log("SPEECH_HOOK: player reply captured source=" + std::string(sourceTag) +
      " speaker=" + ResolveCharacterNameSafe(speaker) +
      " speaker_serial=" + ToString(speakerSerial) + " listener=" + listenerName +
      " listener_serial=" + ToString(listenerSerial) +
      " listener_src=" + listenerSource);
}

static void TryCaptureAmbientSpeechFromNative(Character *speaker,
                                              const std::string &rawLine,
                                              bool force,
                                              const char *sourceTag,
                                              Dialogue *dialogueHint) {
  if (!speaker || (uintptr_t)speaker <= 0x1000) {
    return;
  }
  if (speaker->isPlayerCharacter()) {
    return;
  }

  std::string line = TrimCopy(rawLine);
  if (line.empty() || line.find("[DEBUG]") == 0) {
    return;
  }

  std::string structured = ExtractDialogueMessageFromStructuredText(line);
  if (!structured.empty()) {
    line = structured;
  }

  int ttsDurationMs = ExtractTrailingTtsDurationMs(line);
  std::string ttsHash = ExtractTrailingTtsHash(line);
  if (!ttsHash.empty() || ttsDurationMs > 0) {
    return;
  }
  if (LooksLikeDialogueTemplateToken(line)) {
    std::string resolvedLine = ReadNpcSpeechLineSafe(speaker);
    if (!resolvedLine.empty()) {
      std::string structuredResolved =
          ExtractDialogueMessageFromStructuredText(resolvedLine);
      if (!structuredResolved.empty()) {
        resolvedLine = structuredResolved;
      }
      int resolvedTtsDurationMs = ExtractTrailingTtsDurationMs(resolvedLine);
      std::string resolvedTtsHash = ExtractTrailingTtsHash(resolvedLine);
      resolvedLine = TrimCopy(resolvedLine);
      if (resolvedTtsHash.empty() && resolvedTtsDurationMs <= 0 &&
          !resolvedLine.empty() &&
          !LooksLikeDialogueTemplateToken(resolvedLine)) {
        line = resolvedLine;
      } else {
        Log("SPEECH_HOOK: dropped unresolved template token source=" +
            std::string(sourceTag) + " speaker=" +
            ResolveCharacterNameSafe(speaker));
        return;
      }
    } else {
      Log("SPEECH_HOOK: dropped template token with empty resolved line source=" +
          std::string(sourceTag) + " speaker=" +
          ResolveCharacterNameSafe(speaker));
      return;
    }
  }
  line = SanitizeCapturedDialogueLine(line);
  if (line.empty()) {
    return;
  }

  unsigned int speakerSerial = ResolveCharacterSerialForEvent(speaker);
  if (ShouldDropDuplicateNonAiDialogue(speakerSerial, false, line)) {
    return;
  }
  if (speakerSerial != 0 && IsNpcInSpeechFlowBySerial(speakerSerial)) {
    return;
  }

  GameWorld *world = GetWorldSafe();
  if (!world) {
    return;
  }

  // Native ambient speech capture stores base dialogue with no explicit target.
  std::string listenerName = "None";
  std::string listenerFaction = "None";
  unsigned int listenerSerial = 0;
  std::string listenerSource = "";
  Character *listener =
      ResolveDialogueListenerForSpeech(speaker, dialogueHint, listenerSource);
  if (listener && (uintptr_t)listener > 0x1000) {
    listenerName = ResolveCharacterNameSafe(listener);
    listenerFaction = SafeFaction(listener);
    listenerSerial = ResolveCharacterSerialForEvent(listener);
  }

  LogGameEvent("chat", ResolveCharacterNameSafe(speaker), SafeFaction(speaker),
               listenerName, listenerFaction, line, speakerSerial, listenerSerial);
  Log("SPEECH_HOOK: captured source=" + std::string(sourceTag) +
      " force=" + std::string(force ? "1" : "0") +
      " speaker=" + ResolveCharacterNameSafe(speaker) +
      " serial=" + ToString(speakerSerial) + " listener=" + listenerName +
      " listener_serial=" + ToString(listenerSerial) +
      " listener_src=" + listenerSource);
}

void sayALine_hook(Character *speaker, const std::string &line, bool force) {
  if (sayALine_orig) {
    sayALine_orig(speaker, line, force);
  }
  TryCaptureAmbientSpeechFromNative(speaker, line, force, "sayALine", nullptr);
}

void characterSay_hook(Character *speaker, const std::string &line) {
  if (characterSay_orig) {
    characterSay_orig(speaker, line);
  }
  TryCaptureAmbientSpeechFromNative(speaker, line, false, "say", nullptr);
}

bool dialogueSayLine_hook(Dialogue *dialogue, DialogLineData *lineData) {
  bool result = false;
  if (dialogueSayLine_orig) {
    result = dialogueSayLine_orig(dialogue, lineData);
  }

  Character *speaker = nullptr;
  std::string line = "";
  if (dialogue && (uintptr_t)dialogue > 0x1000) {
    try {
      speaker = dialogue->getCharacter();
    } catch (...) {
      speaker = nullptr;
    }
    try {
      line = TrimCopy(dialogue->npcReplyText);
    } catch (...) {
      line = "";
    }
  }
  TryCaptureAmbientSpeechFromNative(speaker, line, false, "dialogue_sayline",
                                    dialogue);
  return result;
}

void dialogueSayText_hook(Dialogue *dialogue, const std::string &line,
                          DialogLineData *lineData) {
  if (dialogueSayText_orig) {
    dialogueSayText_orig(dialogue, line, lineData);
  }

  Character *speaker = nullptr;
  if (dialogue && (uintptr_t)dialogue > 0x1000) {
    try {
      speaker = dialogue->getCharacter();
    } catch (...) {
      speaker = nullptr;
    }
  }
  TryCaptureAmbientSpeechFromNative(speaker, line, false, "dialogue_saytext",
                                    dialogue);
}

void dialogueReplyClickedInt_hook(Dialogue *dialogue, int index) {
  std::string replyText = ResolveDialogueReplyTextByIndexSafe(dialogue, index);
  if (dialogueReplyClickedInt_orig) {
    dialogueReplyClickedInt_orig(dialogue, index);
  }
  if (!replyText.empty()) {
    CapturePlayerDialogueReplyFromUi(dialogue, replyText, "replyClicked_int");
  }
}

void dialogueReplyClickedString_hook(Dialogue *dialogue,
                                     const std::string &indexToken) {
  std::string replyText = ResolveDialogueReplyTextByTokenSafe(dialogue, indexToken);
  if (dialogueReplyClickedString_orig) {
    dialogueReplyClickedString_orig(dialogue, indexToken);
  }
  if (!replyText.empty()) {
    CapturePlayerDialogueReplyFromUi(dialogue, replyText, "replyClicked_string");
  }
}

void declareDead_hook(Character *npc) {
  if (npc) {
    LogGameEvent("death", npc->getName(), SafeFaction(npc), "None", "None",
                 "Has perished", ResolveCharacterSerialForEvent(npc), 0);
    InterruptConversationForUnavailableNpc(npc, "death");
  }
  if (declareDead_orig)
    declareDead_orig(npc);
}

void setPrisonMode_hook(Character *npc, bool on, UseableStuff *h) {
  if (npc) {
    std::string msg = on ? "Was imprisoned" : "Was released from prison";
    LogGameEvent("imprisonment", npc->getName(), SafeFaction(npc), "None",
                 "None", msg, ResolveCharacterSerialForEvent(npc), 0);
  }
  if (setPrisonMode_orig)
    setPrisonMode_orig(npc, on, h);
}

void setProneState_hook(Character *npc, ProneState p) {
  if (npc && p == PS_KO) {
    LogGameEvent("knockout", "Unknown", "None", npc->getName(),
                 SafeFaction(npc), "Was knocked unconscious", 0,
                 ResolveCharacterSerialForEvent(npc));
    InterruptConversationForUnavailableNpc(npc, "knockout");
  }
  if (setProneState_orig)
    setProneState_orig(npc, p);
}

bool isItOkForMeToLoot_hook(Character *npc, RootObject *victim, Item *item) {
  if (npc && victim && item) {
    LogGameEvent("looting", npc->getName(), SafeFaction(npc), victim->getName(),
                 SafeFaction(victim), "Looted " + item->getName(),
                 ResolveCharacterSerialForEvent(npc), 0);
  }
  if (isItOkForMeToLoot_orig)
    return isItOkForMeToLoot_orig(npc, victim, item);
  return false;
}

void setChainedMode_hook(Character *npc, bool on, const hand &owner) {
  if (npc) {
    std::string msg = on ? "Was forced into slavery" : "Was freed from slavery";
    LogGameEvent("slavery", npc->getName(), SafeFaction(npc), "None", "None",
                 msg, ResolveCharacterSerialForEvent(npc), 0);
  }
  if (setChainedMode_orig)
    setChainedMode_orig(npc, on, owner);
}

static bool IsWorldStableForUI(GameWorld *world) {
  if (!world || reinterpret_cast<uintptr_t>(world) < 0x10000) {
    return false;
  }

  __try {
    if (!world->player || reinterpret_cast<uintptr_t>(world->player) < 0x10000) {
      return false;
    }
    if (world->player->playerCharacters.size() == 0) {
      return false;
    }
    Character *player = world->player->playerCharacters[0];
    if (!player || reinterpret_cast<uintptr_t>(player) < 0x1000) {
      return false;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }

  return true;
}

static bool IsSpeechSystemBusyForMOTD() {
  if (IsTtsPlaybackActive()) {
    return true;
  }

  if (!TryEnterCriticalSection(&g_uiMutex)) {
    return true;
  }

  bool busy = false;
  DWORD now = GetTickCount();
  if (!g_uiActionQueue.empty()) {
    busy = true;
  } else if (g_nextSpeechActionTick != 0 && now < g_nextSpeechActionTick) {
    busy = true;
  }

  LeaveCriticalSection(&g_uiMutex);
  return busy;
}


void Hook_PlayerUpdateTick(PlayerInterface *thisptr) {
  if (!thisptr || reinterpret_cast<uintptr_t>(thisptr) < 0x10000) {
    static bool loggedInvalidThis = false;
    if (!loggedInvalidThis) {
      Log("HOOK_WARN: Hook_PlayerUpdateTick received invalid this pointer.");
      loggedInvalidThis = true;
    }
    return;
  }

  if (playerUpdate_orig)
    playerUpdate_orig(thisptr);

  GameWorld *worldUi = GetWorldSafe();
  static bool worldWasStable = false;
  static DWORD worldBecameStableTick = 0;
  static bool motdAutoOpenQueued = false;
  static DWORD motdAutoOpenTick = 0;
  static DWORD motdAutoOpenDeadlineTick = 0;
  bool worldStable = IsWorldStableForUI(worldUi);

  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) {
    // During loads MyGUI can be torn down; clear stale pointers and do nothing.
    g_settingsWindow = nullptr;
    g_startingWindow = nullptr;
    g_welcomeWindow = nullptr;
    g_aiNpcInfoWindow = nullptr;
    g_aiDiaryWindow = nullptr;
    return;
  }

  if (!worldStable) {
    if (worldWasStable) {
      worldWasStable = false;
      Log("HOOK: world transition detected; pausing UI hook logic.");
      EnterCriticalSection(&g_stateMutex);
      g_inventorySyncStateBySerial.clear();
      g_activeInventoryJson = "[]";
      g_playerInventoryJson = "[]";
      g_lastInventoryHand = hand();
      g_playerHand = hand();
      g_activatedAnimalSerials.clear();
      LeaveCriticalSection(&g_stateMutex);
      g_npcWorldEventStateBySerial.clear();
      g_lastNpcWorldEventSweepTick = 0;
      g_lastInventorySweepTick = 0;
      g_lastInfoNpcTelemetryCheckTick = 0;
      g_lastInfoNpcTelemetrySentTick = 0;
      g_lastInfoNpcTelemetryDigest = "";
      g_lastInfoLocTelemetryCheckTick = 0;
      g_lastInfoLocTelemetrySentTick = 0;
      g_lastInfoLocTelemetryDigest = "";
      ResetPlayerCatsSyncState();
      ResetDynamicProfileIntervalSyncState();
      ResetPlayerSquadsSyncState();
      motdAutoOpenQueued = false;
      motdAutoOpenTick = 0;
      motdAutoOpenDeadlineTick = 0;
      Log("INV_SYNC: state reset on world transition.");
    }
    if (g_settingsWindow)
      CloseSettingsUI();
    if (g_startingWindow)
      CloseStartingUI();
    if (g_welcomeWindow)
      CloseWelcomeUI();
    if (g_aiNpcInfoWindow)
      CloseAiNpcInfoUI();
    if (g_aiDiaryWindow)
      CloseAiDiaryUI();
    return;
  }

  if (!worldWasStable) {
    worldWasStable = true;
    worldBecameStableTick = GetTickCount();
    motdAutoOpenQueued = false;
    motdAutoOpenTick = 0;
    motdAutoOpenDeadlineTick = 0;
    Log("HOOK: world stable; delaying UI hook logic.");
    return;
  }

  // Save loads can expose unstable pointers briefly right after world appears.
  if (GetTickCount() - worldBecameStableTick < 3000) {
    return;
  }

  static DWORD lastUiHeartbeatTick = 0;
  DWORD heartbeatNow = GetTickCount();
  if (heartbeatNow - lastUiHeartbeatTick > 5000) {
    lastUiHeartbeatTick = heartbeatNow;
    Log("HOOK_UI: active, world stable, waiting for UI input.");
  }

  if (!g_enableWelcome) {
    motdAutoOpenQueued = false;
    motdAutoOpenTick = 0;
    motdAutoOpenDeadlineTick = 0;
  } else if (!g_welcomeShown) {
    if (!motdAutoOpenQueued) {
      motdAutoOpenQueued = true;
      motdAutoOpenTick = worldBecameStableTick + 5500;
      motdAutoOpenDeadlineTick = worldBecameStableTick + 20000;
      Log("UI: MOTD auto-open queued (startup quiet-window mode).");
    } else {
      DWORD now = GetTickCount();
      if ((LONG)(now - motdAutoOpenDeadlineTick) >= 0 &&
          motdAutoOpenDeadlineTick != 0) {
  // Do not open MOTD late during gameplay; leave manual open via = menu.
        g_welcomeShown = true;
        motdAutoOpenQueued = false;
        motdAutoOpenTick = 0;
        motdAutoOpenDeadlineTick = 0;
        Log("UI: MOTD auto-open skipped (startup quiet window expired).");
      } else if ((LONG)(now - motdAutoOpenTick) >= 0 &&
                 !IsSpeechSystemBusyForMOTD() && !g_chatWindow &&
                 !g_startingWindow && !g_settingsWindow) {
        try {
          CreateWelcomeUI();
          g_welcomeShown = true;
          motdAutoOpenQueued = false;
          motdAutoOpenTick = 0;
          motdAutoOpenDeadlineTick = 0;
          Log("UI: MOTD auto-opened.");
        } catch (...) {
          motdAutoOpenQueued = false;
          motdAutoOpenTick = 0;
          motdAutoOpenDeadlineTick = 0;
          Log("UI_WARN: MOTD auto-open failed; skipping this session.");
        }
      }
    }
  }

  // Left menu hotkey (= key) - only once world/UI are stable.
  if ((GetAsyncKeyState(VK_OEM_PLUS) & 0x8000)) {
    static DWORD lastSettingsTick = 0;
    DWORD now = GetTickCount();
    if (now - lastSettingsTick > 500) {
      lastSettingsTick = now;
      Log("UI: = pressed.");
      if (g_startingWindow) {
        CloseStartingUI();
        Log("UI: CloseStartingUI done.");
      } else {
        CreateStartingUI();
        Log("UI: CreateStartingUI done.");
      }
    }
  }

  // 1. Core Selection Tracking
  static DWORD lastFrameLogTick = 0;
  bool logThisFrame = false;
  {
    DWORD ft = GetTickCount();
    if (ft - lastFrameLogTick > 1000) {
      lastFrameLogTick = ft;
      logThisFrame = true;
    }
  }
  if (logThisFrame) Log("HOOK_FRAME: selection tracking.");
  Character *sel = nullptr;
  try {
    sel = thisptr->selectedObject.getCharacter();
    if (!sel)
      sel = thisptr->selectedCharacter.getCharacter();
  } catch (...) {
  }
  if (logThisFrame) Log("HOOK_FRAME: selection done.");

  // Detect Selection Change
  EnterCriticalSection(&g_stateMutex);
  bool selectionChanged = false;
  if (sel && (uintptr_t)sel > 0x1000) {
    if (sel->getHandle() != g_lastSelectionHand) {
      g_activeCharName = sel->getName();
      g_lastSelectionHand = sel->getHandle();
      selectionChanged = true;
    }
  } else if (g_lastSelectionHand.isValid()) {
    g_activeCharName = "";
    g_lastSelectionHand = hand();
    selectionChanged = true;
  }
  LeaveCriticalSection(&g_stateMutex);
  if (logThisFrame) Log("HOOK_FRAME: selection change check done.");

  if (selectionChanged && sel && (uintptr_t)sel > 0x1000) {
    if (ShouldProcessAnimalCharacter(sel)) {
      SyncInventoryForCharacter(sel, true, "selection_change");
      static DWORD lastSelectionContextPushTick = 0;
      DWORD nowSel = GetTickCount();
      if (nowSel - lastSelectionContextPushTick > 1000) {
        std::string contextType = "npc";
        std::string selectedName = "Unknown";
        try {
          if (sel->isPlayerCharacter()) {
            contextType = "player";
          }
          selectedName = sel->getName();
        } catch (...) {
        }
        std::string selectedContext = BuildNpcContextEnvelope(sel, contextType);
        AsyncPostToStobe(L"/context", selectedContext);
        std::string selectedGameData =
            "{\"type\":\"" + contextType + "\",\"name\":\"" +
            EscapeJSON(selectedName) + "\",\"data\":" + selectedContext + "}";
        AsyncPostToStobe(L"/gamedata", selectedGameData);
        lastSelectionContextPushTick = nowSel;
        Log("CONTEXT_PUSH: sent selected character snapshot.");
      }
    } else {
      Log("ANIMAL_TALKS: ignoring selection context sync for inactive animal.");
    }
  }

  // 2. Message queue + queued actions
  GameWorld *world = GetWorldSafe();
  if (world) {
    ProcessMessageQueue(world);
    static int invTimer = 0;
    ExecuteQueuedActions(world, invTimer);
    ApplyFollowTargets(world);
    ApplyTravelTargets(world);
    RunInventorySyncSweep(world, sel);
    RunNpcWorldEventSweep(world, sel);
    RunInfoTelemetrySweep(world, sel);
  }

  if (world && g_enableBoredEvents) {
    bool forceTrigger = false;
    EnterCriticalSection(&g_stateMutex);
    if (g_triggerBoredEvent) {
      forceTrigger = true;
      g_triggerBoredEvent = false;
    }
    LeaveCriticalSection(&g_stateMutex);

    DWORD now = GetTickCount();
    static DWORD lastBoredLoopTick = 0;
    if (lastBoredLoopTick == 0) {
      lastBoredLoopTick = now;
    }
    DWORD loopDelta = now - lastBoredLoopTick;
    lastBoredLoopTick = now;
    bool worldPaused = world->isPaused();
    if (!forceTrigger && worldPaused && g_lastBoredEventTick != 0) {
      DWORD shiftedTick = g_lastBoredEventTick + loopDelta;
      if (shiftedTick < g_lastBoredEventTick) {
        shiftedTick = now;
      }
      g_lastBoredEventTick = shiftedTick;
    }

    int intervalSeconds = g_boredEventIntervalSeconds;
    if (intervalSeconds < 5) {
      intervalSeconds = 5;
    } else if (intervalSeconds > 3600) {
      intervalSeconds = 3600;
    }
    const DWORD extraBoredDelayMs = 10000;
    DWORD intervalMs = static_cast<DWORD>(intervalSeconds) * 1000 + extraBoredDelayMs;
    if (!forceTrigger && g_lastBoredEventTick == 0) {
      g_lastBoredEventTick = now;
      Log("BORED_EVENT: startup cooldown armed for " +
          ToString(intervalSeconds + 10) + "s");
    }
    bool periodicDue =
        !forceTrigger && !worldPaused && ((now - g_lastBoredEventTick) >= intervalMs);

    if (forceTrigger || periodicDue) {
      bool speechBusy = IsTtsPlaybackActive();
      if (speechBusy) {
        if (forceTrigger) {
          EnterCriticalSection(&g_stateMutex);
          g_triggerBoredEvent = true;
          LeaveCriticalSection(&g_stateMutex);
          Log("BORED_EVENT: delayed (active TTS playback)");
        } else {
          g_lastBoredEventTick = now;
        }
      } else {
        g_lastBoredEventTick = now;
        bool dispatched = TriggerBoredEvent(world, forceTrigger);
        if (!dispatched && forceTrigger) {
          Log("BORED_EVENT: manual trigger skipped (no eligible NPC)");
        }
      }
    }
  }

  // Rename checks are now queued only for dialogue-tagged NPCs.

  if (logThisFrame) Log("HOOK_FRAME: input check.");
  // 4. Input Handling ??? Chat window hotkey
  if ((GetAsyncKeyState(g_chatHotkey) & 0x8000) && !g_chatWindow &&
      !g_aiNpcInfoWindow) {
    static DWORD lastTalkTick = 0;
    if (GetTickCount() - lastTalkTick > 500) {
      lastTalkTick = GetTickCount();
      if (sel && (uintptr_t)sel > 0x1000) {
        Character *chatTarget = sel;
        if (sel->isPlayerCharacter()) {
          Character *nearestSquadmate =
              ResolveNearestSquadmateTargetForSelection(world, sel);
          if (nearestSquadmate && (uintptr_t)nearestSquadmate > 0x1000) {
            chatTarget = nearestSquadmate;
            Log("CHAT_OPEN: selected squadmate '" + sel->getName() +
                "' retargeted to nearest squadmate '" + chatTarget->getName() +
                "'");
          } else {
            // No alternate squadmate to target from selected player actor.
            EnterCriticalSection(&g_msgMutex);
            g_messageQueue.push_back(
                "NOTIFY:No nearby squadmate target available.");
            LeaveCriticalSection(&g_msgMutex);
            Log("CHAT_OPEN: selected squadmate '" + sel->getName() +
                "' has no alternate squadmate target; chat open blocked");
            return;
          }
        }

        bool selectedIsAnimal = IsAnimalCharacterSafe(chatTarget);
        if (selectedIsAnimal) {
          unsigned int animalSerial = chatTarget->getHandle().serial;
          if (!g_enableAnimalTalks) {
            EnterCriticalSection(&g_msgMutex);
            g_messageQueue.push_back(
                "NOTIFY:Animal Talks is OFF in plugin settings.");
            LeaveCriticalSection(&g_msgMutex);
            Log("ANIMAL_TALKS: blocked chat open; toggle disabled serial=" +
                ToString(animalSerial));
            return;
          }
          MarkAnimalActivated(animalSerial);
          Log("ANIMAL_TALKS: activated animal serial=" +
              ToString(animalSerial) + " name=" + chatTarget->getName());
        }

        g_talkTargetHand = chatTarget->getHandle();
        QueueIdentityRenameCandidate(chatTarget, "chat_open_target");
        SyncInventoryForCharacter(chatTarget, true, "chat_open");

        // Suppress vanilla dialogue state to prevent "double dialogue"
        if (chatTarget->dialogue && (uintptr_t)chatTarget->dialogue > 0x1000) {
          try {
            chatTarget->dialogue->endDialogue(true);
            chatTarget->dialogue->setInDialog(false);
          } catch (...) {
          }
        }

        std::string pName = (thisptr->playerCharacters.size() > 0)
                                ? thisptr->playerCharacters[0]->getName()
                                : "Drifter";
        CreateChatUI(chatTarget->getName(), pName,
                     ToString(chatTarget->getHandle().serial));
      }
    }
  }

}

// Redundant hooks removed since playerUpdate handles real-time needs now.

DWORD WINAPI RenameWorker(LPVOID lpParam) {
  // Wait for server to be fully ready
  Sleep(8000);
  Log("NAME_ASSIGN: Background name-assignment thread started.");

  while (true) {
    std::vector<NameCheckItem> batch;
    {
      EnterCriticalSection(&g_nameCheckMutex);
      while (!g_nameCheckQueue.empty() && batch.size() < 100) {
        batch.push_back(g_nameCheckQueue.front());
        g_nameCheckQueue.pop_front();
      }
      LeaveCriticalSection(&g_nameCheckMutex);
    }

    if (batch.empty()) {
      Sleep(1000);
      continue;
    }

    // Build batch request JSON
    std::string reqJson = "[";
    for (size_t i = 0; i < batch.size(); ++i) {
      reqJson += "{\"serial\": " + ToString(batch[i].serial) +
                 ", \"name\": \"" + EscapeJSON(batch[i].name) +
                 "\", \"gender\": \"" + EscapeJSON(batch[i].gender) +
                 "\", \"race\": \"" + EscapeJSON(batch[i].race) +
                 "\", \"faction\": \"" + EscapeJSON(batch[i].faction) + "\"";
      if (!batch[i].contextJson.empty() && batch[i].contextJson[0] == '{') {
        reqJson += ", \"context\": " + batch[i].contextJson;
      }
      reqJson += "}";
      if (i < batch.size() - 1)
        reqJson += ",";
    }
    reqJson += "]";

    std::string resp =
        PostToStobeWithResponse(L"/get_batch_identities", reqJson);
    if (resp.empty()) {
      continue;
    }

    std::string parsedResp = TrimCopy(resp);
    if (parsedResp.size() >= 3 &&
        (unsigned char)parsedResp[0] == 0xEF &&
        (unsigned char)parsedResp[1] == 0xBB &&
        (unsigned char)parsedResp[2] == 0xBF) {
      parsedResp = TrimCopy(parsedResp.substr(3));
    }

    if (parsedResp.empty() || parsedResp == "[]") {
      continue;
    }
    if (parsedResp[0] != '[') {
      std::string snippet = parsedResp.substr(0, std::min<size_t>(120, parsedResp.size()));
      Log("NAME_ASSIGN: Skipping malformed batch identity response: " + snippet);
      continue;
    }

    int assignedCount = 0;
    size_t pos = 0;
    while ((pos = parsedResp.find("{", pos)) != std::string::npos) {
      size_t endPos = parsedResp.find("}", pos);
      if (endPos == std::string::npos)
        break;
      std::string obj = parsedResp.substr(pos, endPos - pos + 1);
      pos = endPos + 1;

      std::string sSerial = JsonReadField(obj, "serial");
      std::string status = JsonReadField(obj, "status");
      unsigned int serial = (unsigned int)strtoul(sSerial.c_str(), NULL, 10);

      bool markDone = false;
      if (status == "rename") {
        std::string newName = JsonReadField(obj, "new_name");
        if (!newName.empty()) {
          std::string renameMsg = "NPC_RENAME: " + sSerial + "|" + newName;
          EnterCriticalSection(&g_msgMutex);
          g_messageQueue.push_back(renameMsg);
          LeaveCriticalSection(&g_msgMutex);
          Log("NAME_ASSIGN: queued runtime rename serial=" + sSerial +
              " new_name=" + newName);
          assignedCount++;
          markDone = true;
        }
      }

      // Only mark complete when a rename has actually been assigned so
      // transient server-side conflicts can retry later.
      if (markDone) {
        EnterCriticalSection(&g_nameCheckMutex);
        g_renamedSerials.insert(serial);
        LeaveCriticalSection(&g_nameCheckMutex);
      }
    }

    /*
    if (assignedCount > 0) {
      EnterCriticalSection(&g_msgMutex);
      g_messageQueue.push_back("NOTIFY:Assigned " + ToString(assignedCount) +
                               " unique names to local NPCs.");
      LeaveCriticalSection(&g_msgMutex);
    }
    */
  }
  return 0;
}

DWORD WINAPI MainThread(LPVOID lpParam) {
  HMODULE hLib = GetModuleHandleA("KenshiLib.dll");
  while (!hLib) {
    Sleep(500);
    hLib = GetModuleHandleA("KenshiLib.dll");
  }
  ppWorld = (GameWorld **)GetProcAddress(hLib, "?ou@@3PEAVGameWorld@@EA");
  if (!ppWorld)
    return 1;
  LoadStobeRuntimeConfig();
  bool nameThreadStarted = false;
  while (true) {
    if (!nameThreadStarted) {
      GameWorld *worldReady = GetWorldSafe();
      if (worldReady) {
        CreateThread(NULL, 0, RenameWorker, NULL, 0, NULL);
        nameThreadStarted = true;
      }
    }

    DWORD now = GetTickCount();
    GameWorld *world = GetWorldSafe();
    if (world && world->player && world->player->playerCharacters.size() > 0) {
      Character *player = world->player->playerCharacters[0];
      if (player && (uintptr_t)player > 0x1000) {
        SyncPlayerCatsValue(player, false, "main_loop");
        SyncPlayerSquadsToConfOpts(world, false, "main_loop");
      }
    }
    SyncDynamicProfileIntervalToConfOpts(false, "main_loop");
    SyncPluginVersionToConfOpts(false, "main_loop");

    if (now - g_lastContextPushTick > 5000) {
      if (world && world->player && world->player->playerCharacters.size() > 0) {
        Character *player = world->player->playerCharacters[0];
        if (player && (uintptr_t)player > 0x1000) {
          std::string playerContext = BuildNpcContextEnvelope(player, "player");
          AsyncPostToStobe(L"/context", playerContext);
          std::string playerName = player->getName();
          std::string playerGameData =
              "{\"type\":\"player\",\"name\":\"" + EscapeJSON(playerName) +
              "\",\"data\":" + playerContext + "}";
          AsyncPostToStobe(L"/gamedata", playerGameData);
          g_lastContextPushTick = now;
        }
      }
    }
    Sleep(2000);
  }
  return 0;
}

__declspec(dllexport) void startPlugin() {
  InitializeCriticalSection(&g_LogMutex);
  InitializeCriticalSection(&g_msgMutex);
  InitializeCriticalSection(&g_uiMutex);
  InitializeCriticalSection(&g_stateMutex);
  InitializeCriticalSection(&g_eventMutex);
  InitializeCriticalSection(&g_nameCheckMutex);
  g_mainThreadId = GetCurrentThreadId();

  ResetRuntimeLogsForSession();
  LoadStobeRuntimeConfig();
  Log("STARTUP: initializing UI-only hook mode.");
  Log(std::string("STARTUP: Stobe plugin version ") + kStobePluginVersion);
  Log("UI: MOTD auto-popup enabled when EnableMOTD is ON.");

  HMODULE hLib = GetModuleHandleA("KenshiLib.dll");
  if (!hLib) {
    Log("ERROR: KenshiLib.dll not loaded; skipping PlayerInterface::update hook.");
    return;
  }

  ppWorld = (GameWorld **)GetProcAddress(hLib, "?ou@@3PEAVGameWorld@@EA");
  if (!ppWorld) {
    Log("HOOK_WARN: failed to resolve GameWorld export (ou).");
  } else {
    Log("STARTUP: resolved GameWorld export (ou).");
  }

  void *thunkPlayer =
      (void *)GetProcAddress(hLib, "?update@PlayerInterface@@QEAAXXZ");
  if (!thunkPlayer) {
    Log("HOOK_WARN: PlayerInterface::update symbol not found.");
    return;
  }
  Log("HOOK_DIAG: thunkPlayer=" + ToString((unsigned int)(uintptr_t)thunkPlayer));

  __int64 realAddr = KenshiLib::GetRealAddress(thunkPlayer);
  Log("HOOK_DIAG: GetRealAddress=" + ToString((unsigned int)(uintptr_t)realAddr));
  if (!realAddr) {
    Log("HOOK_WARN: GetRealAddress returned null, hook not installed.");
    return;
  }

  KenshiLib::HookStatus status = KenshiLib::AddHook(
      (void *)realAddr, (void *)Hook_PlayerUpdateTick, (void **)&playerUpdate_orig);
  Log("HOOK_DIAG: AddHook status=" + ToString((int)status) +
      " orig=" + ToString((unsigned int)(uintptr_t)playerUpdate_orig));
  Log("HOOK: PlayerInterface::update installed (UI-only mode).");

  void *thunkSayALine = (void *)GetProcAddress(
      hLib, "?sayALine@Character@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z");
  if (!thunkSayALine) {
    Log("HOOK_WARN: Character::sayALine symbol not found.");
  } else {
    __int64 realSayALine = KenshiLib::GetRealAddress(thunkSayALine);
    if (!realSayALine) {
      Log("HOOK_WARN: GetRealAddress failed for Character::sayALine.");
    } else {
      KenshiLib::HookStatus sayStatus =
          KenshiLib::AddHook((void *)realSayALine, (void *)sayALine_hook,
                             (void **)&sayALine_orig);
      Log("HOOK_DIAG: Character::sayALine AddHook status=" +
          ToString((int)sayStatus) + " orig=" +
          ToString((unsigned int)(uintptr_t)sayALine_orig));
    }
  }

  void *thunkCharacterSay = (void *)GetProcAddress(
      hLib, "?say@Character@@UEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z");
  if (!thunkCharacterSay) {
    Log("HOOK_WARN: Character::say symbol not found.");
  } else {
    __int64 realCharacterSay = KenshiLib::GetRealAddress(thunkCharacterSay);
    if (!realCharacterSay) {
      Log("HOOK_WARN: GetRealAddress failed for Character::say.");
    } else {
      KenshiLib::HookStatus sayFallbackStatus =
          KenshiLib::AddHook((void *)realCharacterSay, (void *)characterSay_hook,
                             (void **)&characterSay_orig);
      Log("HOOK_DIAG: Character::say AddHook status=" +
          ToString((int)sayFallbackStatus) + " orig=" +
          ToString((unsigned int)(uintptr_t)characterSay_orig));
    }
  }

  void *thunkDialogueSayLine =
      (void *)GetProcAddress(hLib, "?sayLine@Dialogue@@QEAA_NPEAVDialogLineData@@@Z");
  if (!thunkDialogueSayLine) {
    Log("HOOK_WARN: Dialogue::sayLine symbol not found.");
  } else {
    __int64 realDialogueSayLine = KenshiLib::GetRealAddress(thunkDialogueSayLine);
    if (!realDialogueSayLine) {
      Log("HOOK_WARN: GetRealAddress failed for Dialogue::sayLine.");
    } else {
      KenshiLib::HookStatus dialogueSayLineStatus =
          KenshiLib::AddHook((void *)realDialogueSayLine,
                             (void *)dialogueSayLine_hook,
                             (void **)&dialogueSayLine_orig);
      Log("HOOK_DIAG: Dialogue::sayLine AddHook status=" +
          ToString((int)dialogueSayLineStatus) + " orig=" +
          ToString((unsigned int)(uintptr_t)dialogueSayLine_orig));
    }
  }

  void *thunkDialogueSayText = (void *)GetProcAddress(
      hLib,
      "?say@Dialogue@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@PEAVDialogLineData@@@Z");
  if (!thunkDialogueSayText) {
    Log("HOOK_WARN: Dialogue::say(text, line) symbol not found.");
  } else {
    __int64 realDialogueSayText = KenshiLib::GetRealAddress(thunkDialogueSayText);
    if (!realDialogueSayText) {
      Log("HOOK_WARN: GetRealAddress failed for Dialogue::say(text, line).");
    } else {
      KenshiLib::HookStatus dialogueSayTextStatus =
          KenshiLib::AddHook((void *)realDialogueSayText,
                             (void *)dialogueSayText_hook,
                             (void **)&dialogueSayText_orig);
      Log("HOOK_DIAG: Dialogue::say(text, line) AddHook status=" +
          ToString((int)dialogueSayTextStatus) + " orig=" +
          ToString((unsigned int)(uintptr_t)dialogueSayText_orig));
    }
  }

  void *thunkDialogueReplyClickedInt =
      (void *)GetProcAddress(hLib, "?replyClicked@Dialogue@@QEAAXH@Z");
  if (!thunkDialogueReplyClickedInt) {
    Log("HOOK_WARN: Dialogue::replyClicked(int) symbol not found.");
  } else {
    __int64 realDialogueReplyClickedInt =
        KenshiLib::GetRealAddress(thunkDialogueReplyClickedInt);
    if (!realDialogueReplyClickedInt) {
      Log("HOOK_WARN: GetRealAddress failed for Dialogue::replyClicked(int).");
    } else {
      KenshiLib::HookStatus dialogueReplyClickedIntStatus =
          KenshiLib::AddHook((void *)realDialogueReplyClickedInt,
                             (void *)dialogueReplyClickedInt_hook,
                             (void **)&dialogueReplyClickedInt_orig);
      Log("HOOK_DIAG: Dialogue::replyClicked(int) AddHook status=" +
          ToString((int)dialogueReplyClickedIntStatus) + " orig=" +
          ToString((unsigned int)(uintptr_t)dialogueReplyClickedInt_orig));
    }
  }

  void *thunkDialogueReplyClickedString = (void *)GetProcAddress(
      hLib,
      "?replyClicked@Dialogue@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z");
  if (!thunkDialogueReplyClickedString) {
    Log("HOOK_WARN: Dialogue::replyClicked(string) symbol not found.");
  } else {
    __int64 realDialogueReplyClickedString =
        KenshiLib::GetRealAddress(thunkDialogueReplyClickedString);
    if (!realDialogueReplyClickedString) {
      Log("HOOK_WARN: GetRealAddress failed for Dialogue::replyClicked(string).");
    } else {
      KenshiLib::HookStatus dialogueReplyClickedStringStatus =
          KenshiLib::AddHook((void *)realDialogueReplyClickedString,
                             (void *)dialogueReplyClickedString_hook,
                             (void **)&dialogueReplyClickedString_orig);
      Log("HOOK_DIAG: Dialogue::replyClicked(string) AddHook status=" +
          ToString((int)dialogueReplyClickedStringStatus) + " orig=" +
          ToString((unsigned int)(uintptr_t)dialogueReplyClickedString_orig));
    }
  }

  HANDLE hMainThread = CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
  if (!hMainThread) {
    Log("STARTUP_WARN: failed to start MainThread.");
  } else {
    CloseHandle(hMainThread);
    Log("STARTUP: MainThread started.");
  }

  HANDLE hCsvImportThread =
      CreateThread(NULL, 0, CsvImportStartupThread, NULL, 0, NULL);
  if (!hCsvImportThread) {
    Log("STARTUP_WARN: failed to start CSV import startup thread.");
  } else {
    CloseHandle(hCsvImportThread);
    Log("STARTUP: CSV import startup thread started.");
  }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  return TRUE;
}


