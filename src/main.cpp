// ???? AGENT PROTOCOL: Before editing this file, you MUST read PROJECT_CONTEXT.md
// ???? This project has strict threading and memory safety rules.
#include <string>
#include <vector>
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <deque>
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
#include "AutonomyController.h"
#include "AutonomySafetyProbe.h"
#include "Globals.h"
#include "KenshiTownCompat.h"
#include "StobeIdentityRename.h"
#include "Utils.h"

#include <kenshi/CharStats.h>
#include <kenshi/Character.h>
#include <kenshi/CharBody.h>
#include <kenshi/CharMovement.h>
#include <kenshi/Dialogue.h>
#include <kenshi/Faction.h>
#include <kenshi/FactionRelations.h>
#include <kenshi/GameData.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Inventory.h>
#include <kenshi/Item.h>
#include <kenshi/Kenshi.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/Platoon.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Tasker.h>
#include <kenshi/gui/InventoryGUI.h>
#include <kenshi/gui/PortraitManager.h>
#include <kenshi/util/hand.h>
#include <kenshi/Damages.h>
#include <ogre/OgreImage.h>

#include <kenshi/RaceData.h>
#include <kenshi/RootObject.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/SharedKing.h>
#include <kenshi/Globals.h>
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
bool (*characterGettingEaten_orig)(Character *, float, Character *) = nullptr;
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

static std::string TrimCopy(const std::string &value);

static bool IsSpeechSerialBoundaryChar(char ch) {
  const unsigned char uch = static_cast<unsigned char>(ch);
  return std::isspace(uch) || ch == '.' || ch == ',' || ch == ';' ||
         ch == ':' || ch == '!' || ch == '?' || ch == '(' || ch == ')' ||
         ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == '"' ||
         ch == '\'' || ch == '/' || ch == '\\' || ch == '-' || ch == '_';
}

// Some queue lines can leak engine serial fragments into spoken text
// (e.g. "|80120293|"). Strip those before showing speech bubbles.
static std::string StripLeakedSpeechSerialTokens(std::string value) {
  if (value.empty()) {
    return "";
  }

  std::string cleaned;
  cleaned.reserve(value.size());
  bool removedAny = false;

  for (size_t i = 0; i < value.size();) {
    if (value[i] != '|') {
      cleaned.push_back(value[i]);
      ++i;
      continue;
    }

    size_t digitsStart = i + 1;
    size_t cursor = digitsStart;
    while (cursor < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[cursor]))) {
      ++cursor;
    }

    const size_t digitCount = cursor - digitsStart;
    const bool hasLongDigitRun = (digitCount >= 6);
    const bool hasClosingPipe = (cursor < value.size() && value[cursor] == '|');
    const size_t tokenEnd = hasClosingPipe ? (cursor + 1) : cursor;

    bool removeToken = false;
    if (hasLongDigitRun && hasClosingPipe) {
      removeToken = true;
    } else if (hasLongDigitRun &&
               (cursor >= value.size() ||
                IsSpeechSerialBoundaryChar(value[cursor]))) {
      const bool leftBoundary =
          (i == 0) || IsSpeechSerialBoundaryChar(value[i - 1]);
      if (leftBoundary) {
        removeToken = true;
      }
    }

    if (removeToken) {
      removedAny = true;
      i = tokenEnd;
      continue;
    }

    cleaned.push_back(value[i]);
    ++i;
  }

  if (!removedAny) {
    return value;
  }

  std::string trimmed = TrimCopy(cleaned);
  std::string collapsed;
  collapsed.reserve(trimmed.size());
  bool prevSpace = false;
  for (size_t i = 0; i < trimmed.size(); ++i) {
    unsigned char ch = static_cast<unsigned char>(trimmed[i]);
    if (std::isspace(ch)) {
      if (!prevSpace) {
        collapsed.push_back(' ');
        prevSpace = true;
      }
      continue;
    }
    prevSpace = false;
    collapsed.push_back(trimmed[i]);
  }

  return TrimCopy(collapsed);
}

static std::string StripDanglingTrailingClosingBrackets(std::string value) {
  value = TrimCopy(value);
  if (value.empty()) {
    return "";
  }

  size_t openCount = 0;
  size_t closeCount = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '[') {
      ++openCount;
    } else if (value[i] == ']') {
      ++closeCount;
    }
  }

  if (closeCount <= openCount) {
    return value;
  }

  while (!value.empty() && closeCount > openCount) {
    value = TrimCopy(value);
    if (value.empty() || value[value.size() - 1] != ']') {
      break;
    }
    value.erase(value.size() - 1);
    if (closeCount > 0) {
      --closeCount;
    }
  }

  return TrimCopy(value);
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
  if (!Stobe::IdentityRename::IsQueueEligibleName(otherName)) {
    return;
  }

  DWORD nowTick = GetTickCount();
  EnterCriticalSection(&g_nameCheckMutex);
  bool alreadyDone = (g_identityRenameCompletedSerials.count(serial) > 0);
  auto nextAttemptIt = g_identityRenameNextAttemptTick.find(serial);
  DWORD nextAttemptTick =
      (nextAttemptIt == g_identityRenameNextAttemptTick.end())
          ? 0
          : nextAttemptIt->second;
  bool attemptReady =
      Stobe::IdentityRename::IsAttemptReady(nowTick, nextAttemptTick);
  if (alreadyDone) {
    LeaveCriticalSection(&g_nameCheckMutex);
    return;
  }
  if (!attemptReady) {
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
  g_identityRenameNextAttemptTick[serial] =
      Stobe::IdentityRename::ResolveQueuedAttemptDeadline(nowTick);
  LeaveCriticalSection(&g_nameCheckMutex);
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

struct WorldStateSerializeStats {
  int queryCount;
  int npcAreCount;
  int npcAreNotCount;
  int townCount;
  int allyCount;
  int enemyCount;
  bool truncated;

  WorldStateSerializeStats()
      : queryCount(0), npcAreCount(0), npcAreNotCount(0), townCount(0),
        allyCount(0), enemyCount(0), truncated(false) {}
};

static int ResolveCurrentGameTsSafe(GameWorld *world) {
  if (!world || (uintptr_t)world <= 0x1000) {
    return 0;
  }
  try {
    TimeOfDay tod = world->getTimeStamp_inGameHours();
    int gameTs = (int)tod.getTotalSeconds();
    if (gameTs < 0) {
      return 0;
    }
    return gameTs;
  } catch (...) {
    return 0;
  }
}

static const char *WorldStateEnumToText(WorldStateEnum state) {
  switch (state) {
  case DEAD:
    return "dead";
  case ALIVE:
    return "alive";
  case IMPRISONED:
    return "imprisoned";
  default:
    return "unknown";
  }
}

static std::string SafeGameDataName(GameData *data) {
  if (!data || (uintptr_t)data <= 0x1000) {
    return "";
  }
  try {
    return TrimCopy(data->name);
  } catch (...) {
    return "";
  }
}

static std::string SafeGameDataStringId(GameData *data) {
  if (!data || (uintptr_t)data <= 0x1000) {
    return "";
  }
  try {
    return TrimCopy(data->stringID);
  } catch (...) {
    return "";
  }
}

static int SafeGameDataNumericId(GameData *data) {
  if (!data || (uintptr_t)data <= 0x1000) {
    return 0;
  }
  try {
    return (int)data->id;
  } catch (...) {
    return 0;
  }
}

static std::string SafeFactionName(Faction *faction) {
  if (!faction || (uintptr_t)faction <= 0x1000) {
    return "";
  }

  try {
    std::string name = TrimCopy(faction->getName());
    if (!name.empty()) {
      return name;
    }
  } catch (...) {
  }

  try {
    if (faction->data && (uintptr_t)faction->data > 0x1000) {
      return TrimCopy(faction->data->name);
    }
  } catch (...) {
  }
  return "";
}

static std::string SafeFactionStringId(Faction *faction) {
  if (!faction || (uintptr_t)faction <= 0x1000) {
    return "";
  }
  try {
    if (faction->data && (uintptr_t)faction->data > 0x1000) {
      return TrimCopy(faction->data->stringID);
    }
  } catch (...) {
  }
  return "";
}

static int SafeFactionNumericId(Faction *faction) {
  if (!faction || (uintptr_t)faction <= 0x1000) {
    return 0;
  }
  try {
    if (faction->data && (uintptr_t)faction->data > 0x1000) {
      return static_cast<int>(faction->data->id);
    }
  } catch (...) {
  }
  return 0;
}

static std::string BuildFactionRelationStableToken(const std::string &stringId,
                                                   int numericId,
                                                   const std::string &name) {
  std::string sid = TrimCopy(stringId);
  if (!sid.empty()) {
    return "sid:" + ToLowerAsciiCopy(sid);
  }
  if (numericId > 0) {
    return "id:" + ToString(numericId);
  }
  std::string trimmedName = TrimCopy(name);
  if (!trimmedName.empty()) {
    return "name:" + ToLowerAsciiCopy(trimmedName);
  }
  return "";
}

static std::string BuildFactionRelationMergeKey(
    const std::string &sourceStringId, int sourceNumericId,
    const std::string &sourceName, const std::string &targetStringId,
    int targetNumericId, const std::string &targetName) {
  std::string sourceToken = BuildFactionRelationStableToken(
      sourceStringId, sourceNumericId, sourceName);
  std::string targetToken = BuildFactionRelationStableToken(
      targetStringId, targetNumericId, targetName);
  if (sourceToken.empty() || targetToken.empty()) {
    return "";
  }
  return sourceToken + "->" + targetToken;
}

struct FactionRelationSnapshotEntry {
  std::string mergeKey;
  std::string sourceName;
  std::string sourceStringId;
  int sourceNumericId;
  std::string targetName;
  std::string targetStringId;
  int targetNumericId;
  double relation;
  bool alliance;
  bool war;
  bool coexists;

  FactionRelationSnapshotEntry()
      : mergeKey(""), sourceName(""), sourceStringId(""), sourceNumericId(0),
        targetName(""), targetStringId(""), targetNumericId(0), relation(0.0),
        alliance(false), war(false), coexists(false) {}
};

static bool IsFactionRelationEntryDifferent(
    const FactionRelationSnapshotEntry &left,
    const FactionRelationSnapshotEntry &right) {
  const double kRelationEpsilon = 0.001;
  if (std::fabs(left.relation - right.relation) > kRelationEpsilon) {
    return true;
  }
  if (left.alliance != right.alliance || left.war != right.war ||
      left.coexists != right.coexists) {
    return true;
  }
  if (left.sourceName != right.sourceName ||
      left.sourceStringId != right.sourceStringId ||
      left.sourceNumericId != right.sourceNumericId ||
      left.targetName != right.targetName ||
      left.targetStringId != right.targetStringId ||
      left.targetNumericId != right.targetNumericId) {
    return true;
  }
  return false;
}

static std::string BuildWorldStateEntityRulesJson(
    const ogre_unordered_map<GameData *, WorldStateEnum>::type &source,
    int &counter, bool &truncated) {
  static const size_t kRuleHardCap = 8192;
  std::string json = "[";
  bool first = true;
  size_t seen = 0;
  for (auto it = source.begin(); it != source.end(); ++it) {
    if (seen >= kRuleHardCap) {
      truncated = true;
      break;
    }
    ++seen;
    GameData *data = it->first;
    if (!data || (uintptr_t)data <= 0x1000) {
      continue;
    }
    std::string name = SafeGameDataName(data);
    std::string sid = SafeGameDataStringId(data);
    int numericId = SafeGameDataNumericId(data);
    if (name.empty() && sid.empty() && numericId <= 0) {
      continue;
    }
    if (!first) {
      json += ",";
    }
    first = false;
    json += "{";
    json += "\"name\":\"" + EscapeJSON(name) + "\",";
    json += "\"string_id\":\"" + EscapeJSON(sid) + "\",";
    json += "\"numeric_id\":" + ToString(numericId) + ",";
    json += "\"state\":\"" +
            std::string(WorldStateEnumToText((WorldStateEnum)it->second)) + "\"";
    json += "}";
    counter += 1;
  }
  json += "]";
  return json;
}

static std::string BuildWorldStateFactionRulesJson(
    const ogre_unordered_map<Faction *, bool>::type &source,
    const std::string &valueKey, int &counter, bool &truncated) {
  static const size_t kRuleHardCap = 8192;
  std::string json = "[";
  bool first = true;
  size_t seen = 0;
  for (auto it = source.begin(); it != source.end(); ++it) {
    if (seen >= kRuleHardCap) {
      truncated = true;
      break;
    }
    ++seen;
    Faction *faction = it->first;
    if (!faction || (uintptr_t)faction <= 0x1000) {
      continue;
    }
    std::string factionName = SafeFactionName(faction);
    if (factionName.empty()) {
      continue;
    }
    if (!first) {
      json += ",";
    }
    first = false;
    json += "{";
    json += "\"name\":\"" + EscapeJSON(factionName) + "\",";
    json += "\"" + EscapeJSON(valueKey) + "\":" +
            std::string(it->second ? "true" : "false");
    json += "}";
    counter += 1;
  }
  json += "]";
  return json;
}

static std::vector<GameData *> g_worldStateQuerySources;
static DWORD g_lastWorldStateSourceScanTick = 0;

static void RefreshWorldStateQuerySources(GameWorld *world, bool forceRefresh) {
  const DWORD now = GetTickCount();
  const DWORD kScanIntervalMs = 180000;
  if (!forceRefresh && !g_worldStateQuerySources.empty() &&
      now - g_lastWorldStateSourceScanTick < kScanIntervalMs) {
    return;
  }
  g_lastWorldStateSourceScanTick = now;

  g_worldStateQuerySources.clear();
  if (!world || (uintptr_t)world <= 0x1000) {
    return;
  }

  size_t scanned = 0;
  size_t matched = 0;
  bool truncated = false;
  static const size_t kDataScanHardCap = 50000;
  std::set<uintptr_t> seen;
  try {
    const ogre_unordered_map<int, GameData *>::type &allData =
        world->gamedata._getAllData();
    for (auto it = allData.begin(); it != allData.end(); ++it) {
      if (scanned >= kDataScanHardCap) {
        truncated = true;
        break;
      }
      ++scanned;
      GameData *data = it->second;
      if (!data || (uintptr_t)data <= 0x1000) {
        continue;
      }

      WorldEventStateQuery *query = nullptr;
      try {
        query = WorldEventStateQuery::getFromData(data);
      } catch (...) {
        query = nullptr;
      }
      if (!query || (uintptr_t)query <= 0x1000) {
        continue;
      }

      uintptr_t key = (uintptr_t)data;
      if (seen.insert(key).second) {
        g_worldStateQuerySources.push_back(data);
        matched += 1;
      }
    }
  } catch (...) {
    truncated = true;
  }

  Log("WORLD_STATE_SYNC: source_scan scanned=" + ToString((int)scanned) +
      " matched=" + ToString((int)matched) +
      " truncated=" + std::string(truncated ? "1" : "0"));
}

static std::string BuildWorldStateSnapshotJson(GameWorld *world,
                                               WorldStateSerializeStats &stats) {
  stats = WorldStateSerializeStats();
  std::string json = "{";
  json += "\"source\":\"world_event_state_query\",";
  json += "\"game_ts\":" + ToString(ResolveCurrentGameTsSafe(world)) + ",";
  json += "\"queries\":[";

  bool firstQuery = true;
  static const size_t kQueryHardCap = 8192;
  size_t querySeen = 0;

  RefreshWorldStateQuerySources(world, false);
  if (g_worldStateQuerySources.empty()) {
    RefreshWorldStateQuerySources(world, true);
  }

  try {
    for (auto it = g_worldStateQuerySources.begin();
         it != g_worldStateQuerySources.end(); ++it) {
      if (querySeen >= kQueryHardCap) {
        stats.truncated = true;
        break;
      }
      ++querySeen;

      GameData *queryData = *it;
      if (!queryData || (uintptr_t)queryData <= 0x1000) {
        continue;
      }
      WorldEventStateQuery *query = nullptr;
      try {
        query = WorldEventStateQuery::getFromData(queryData);
      } catch (...) {
        query = nullptr;
      }
      if (!query || (uintptr_t)query <= 0x1000) {
        continue;
      }

      std::string queryName = SafeGameDataName(queryData);
      std::string querySid = SafeGameDataStringId(queryData);
      int queryNumericId = SafeGameDataNumericId(queryData);

      std::string uniqueNpcAre = "[]";
      std::string uniqueNpcAreNot = "[]";
      std::string towns = "[]";
      std::string allyOf = "[]";
      std::string enemyOf = "[]";

      try {
        uniqueNpcAre = BuildWorldStateEntityRulesJson(query->uniqueNPCsAre,
                                                      stats.npcAreCount,
                                                      stats.truncated);
      } catch (...) {
        stats.truncated = true;
      }
      try {
        uniqueNpcAreNot = BuildWorldStateEntityRulesJson(query->uniqueNPCsAreNot,
                                                         stats.npcAreNotCount,
                                                         stats.truncated);
      } catch (...) {
        stats.truncated = true;
      }
      try {
        towns = BuildWorldStateEntityRulesJson(query->towns, stats.townCount,
                                               stats.truncated);
      } catch (...) {
        stats.truncated = true;
      }
      try {
        allyOf = BuildWorldStateFactionRulesJson(query->isAllyOf, "is_ally",
                                                 stats.allyCount,
                                                 stats.truncated);
      } catch (...) {
        stats.truncated = true;
      }
      try {
        enemyOf = BuildWorldStateFactionRulesJson(query->isEnemyOf, "is_enemy",
                                                  stats.enemyCount,
                                                  stats.truncated);
      } catch (...) {
        stats.truncated = true;
      }

      if (!firstQuery) {
        json += ",";
      }
      firstQuery = false;

      json += "{";
      json += "\"query_name\":\"" + EscapeJSON(queryName) + "\",";
      json += "\"query_string_id\":\"" + EscapeJSON(querySid) + "\",";
      json += "\"query_numeric_id\":" + ToString(queryNumericId) + ",";
      json += "\"player_involvement\":" +
              std::string(query->playerInvolvement ? "true" : "false") + ",";
      json += "\"unique_npcs_are\":" + uniqueNpcAre + ",";
      json += "\"unique_npcs_are_not\":" + uniqueNpcAreNot + ",";
      json += "\"towns\":" + towns + ",";
      json += "\"is_ally_of\":" + allyOf + ",";
      json += "\"is_enemy_of\":" + enemyOf;
      json += "}";
      stats.queryCount += 1;
    }
  } catch (...) {
    stats.truncated = true;
  }

  json += "],";
  json += "\"query_count\":" + ToString(stats.queryCount) + ",";
  json += "\"npc_are_count\":" + ToString(stats.npcAreCount) + ",";
  json += "\"npc_are_not_count\":" + ToString(stats.npcAreNotCount) + ",";
  json += "\"town_count\":" + ToString(stats.townCount) + ",";
  json += "\"ally_count\":" + ToString(stats.allyCount) + ",";
  json += "\"enemy_count\":" + ToString(stats.enemyCount) + ",";
  json += "\"truncated\":" + std::string(stats.truncated ? "true" : "false");
  json += "}";

  return json;
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

  value = StripLeakedSpeechSerialTokens(value);
  value = StripDanglingTrailingClosingBrackets(value);
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

static bool ShouldEmitRegularDialogueProbeLog(unsigned int actorSerial) {
  static std::map<unsigned int, DWORD> s_lastProbeLogTickBySerial;
  const DWORD nowTick = GetTickCount();
  const DWORD minIntervalMs = 1200;

  std::map<unsigned int, DWORD>::iterator existing =
      s_lastProbeLogTickBySerial.find(actorSerial);
  if (existing != s_lastProbeLogTickBySerial.end() &&
      (nowTick - existing->second) < minIntervalMs) {
    return false;
  }

  s_lastProbeLogTickBySerial[actorSerial] = nowTick;
  return true;
}

static std::string ExtractInlineMetadataToken(std::string &message,
                                              const std::string &marker) {
  std::string trimmed = TrimCopy(message);
  size_t markerPos = trimmed.rfind(marker);
  if (markerPos == std::string::npos) {
    message = trimmed;
    return "";
  }

  size_t valuePos = markerPos + marker.length();
  size_t endPos = std::string::npos;
  for (size_t candidate = trimmed.find(']', valuePos);
       candidate != std::string::npos;
       candidate = trimmed.find(']', candidate + 1)) {
    size_t next = candidate + 1;
    while (next < trimmed.size() &&
           std::isspace((unsigned char)trimmed[next])) {
      ++next;
    }
    if (next >= trimmed.size()) {
      endPos = candidate;
      break;
    }
    unsigned char nextCh = (unsigned char)trimmed[next];
    if (trimmed[next] == '[' || trimmed[next] == '(' || trimmed[next] == ')' ||
        trimmed[next] == '.' || trimmed[next] == ',' || trimmed[next] == ';' ||
        trimmed[next] == ':' || trimmed[next] == '!' || trimmed[next] == '?' ||
        std::isspace(nextCh)) {
      endPos = candidate;
      break;
    }
  }
  if (endPos == std::string::npos) {
    message = trimmed;
    return "";
  }

  std::string token = TrimCopy(trimmed.substr(valuePos, endPos - valuePos));
  if (token.empty()) {
    message = trimmed;
    return "";
  }

  trimmed.erase(markerPos, endPos - markerPos + 1);
  message = TrimCopy(trimmed);
  return token;
}

static std::string ExtractTrailingTalkingToToken(std::string &message) {
  std::string trimmed = TrimCopy(message);
  std::string lowered = ToLowerAsciiCopy(trimmed);
  const std::string marker = "(talking to:";
  size_t markerPos = lowered.rfind(marker);
  if (markerPos == std::string::npos) {
    message = trimmed;
    return "";
  }

  size_t closePos = trimmed.find(')', markerPos);
  if (closePos == std::string::npos) {
    message = trimmed;
    return "";
  }

  std::string tail = TrimCopy(trimmed.substr(closePos + 1));
  if (!tail.empty()) {
    message = trimmed;
    return "";
  }

  size_t valuePos = markerPos + marker.length();
  std::string token = TrimCopy(trimmed.substr(valuePos, closePos - valuePos));
  if (token.empty()) {
    message = trimmed;
    return "";
  }

  trimmed.erase(markerPos);
  message = TrimCopy(trimmed);
  return token;
}

static std::string ExtractTalkTargetToken(std::string &message) {
  std::string token = ExtractInlineMetadataToken(message, "[TALKTARGET:");
  if (!token.empty()) {
    return token;
  }
  return ExtractTrailingTalkingToToken(message);
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

static std::string ExtractTrailingUtteranceId(std::string &message) {
  std::string trimmed = TrimCopy(message);
  size_t markerPos = trimmed.rfind("[UTTERANCEID:");
  if (markerPos == std::string::npos) {
    message = trimmed;
    return "";
  }

  size_t endPos = trimmed.find(']', markerPos);
  if (endPos == std::string::npos || endPos != trimmed.size() - 1) {
    message = trimmed;
    return "";
  }

  size_t valuePos = markerPos + 13;
  std::string utteranceId =
      TrimCopy(trimmed.substr(valuePos, endPos - valuePos));
  if (utteranceId.empty() || utteranceId.length() > 80) {
    message = trimmed;
    return "";
  }

  for (size_t i = 0; i < utteranceId.length(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(utteranceId[i]);
    bool isSafe =
        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    if (!isSafe) {
      message = trimmed;
      return "";
    }
  }

  trimmed.erase(markerPos);
  message = TrimCopy(trimmed);
  return utteranceId;
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

struct PortraitSyncState {
  std::string lastHash;
  DWORD lastSentTick;
  DWORD lastSeenTick;
  DWORD lastAttemptTick;
  bool hasSent;

  PortraitSyncState()
      : lastHash(""),
        lastSentTick(0),
        lastSeenTick(0),
        lastAttemptTick(0),
        hasSent(false) {}
};

struct ItemImageSyncState {
  std::string lastHash;
  DWORD lastSentTick;
  DWORD lastSeenTick;
  DWORD lastAttemptTick;
  bool hasSent;

  ItemImageSyncState()
      : lastHash(""),
        lastSentTick(0),
        lastSeenTick(0),
        lastAttemptTick(0),
        hasSent(false) {}
};

struct PendingItemImageSyncRequest {
  hand npcHand;
  std::string reason;
  DWORD queuedTick;

  PendingItemImageSyncRequest() : npcHand(), reason(""), queuedTick(0) {}
};

struct InventoryEventSnapshot {
  std::map<std::string, int> countsByKey;
  std::map<std::string, int> stolenByKey;
  std::map<std::string, int> foodByKey;
  std::map<std::string, std::string> displayNameByKey;
  int totalCount;

  InventoryEventSnapshot() : totalCount(0) {}
};

struct NpcWorldEventState {
  bool initialized;
  bool dead;
  bool unconscious;
  bool enslaved;
  bool hasMoney;
  int money;
  bool hasHunger;
  float hunger;
  float fed;
  float satiety;
  bool carrying;
  unsigned int carryingTargetSerial;
  std::string carryingTargetName;
  bool speechActive;
  bool leftArmPresent;
  bool rightArmPresent;
  bool leftLegPresent;
  bool rightLegPresent;
  int leftArmState;
  int rightArmState;
  int leftLegState;
  int rightLegState;
  int currentTask;
  unsigned int currentTaskSubjectSerial;
  std::string currentTaskSubjectName;
  int constructionAction;
  unsigned int constructionSubjectSerial;
  std::string constructionSubjectName;
  int lockpickingSkill;
  std::string lastSpeechLine;
  InventoryEventSnapshot inventory;
  DWORD lastSeenTick;

  NpcWorldEventState()
      : initialized(false), dead(false), unconscious(false), enslaved(false),
        hasMoney(false), money(0), hasHunger(false), hunger(0.0f), fed(0.0f),
        satiety(0.0f),
        carrying(false), carryingTargetSerial(0), carryingTargetName(""),
        speechActive(false), leftArmPresent(true), rightArmPresent(true),
        leftLegPresent(true), rightLegPresent(true),
        leftArmState((int)LIMB_ORIGINAL), rightArmState((int)LIMB_ORIGINAL),
        leftLegState((int)LIMB_ORIGINAL), rightLegState((int)LIMB_ORIGINAL),
        currentTask((int)NULL_TASK), currentTaskSubjectSerial(0),
        currentTaskSubjectName(""), constructionAction(0),
        constructionSubjectSerial(0), constructionSubjectName(""),
        lockpickingSkill(0), lastSpeechLine(""), lastSeenTick(0) {}
};

struct PendingMedicalItemUseState {
  std::map<std::string, int> pendingByKey;
  DWORD lastUpdateTick;

  PendingMedicalItemUseState() : lastUpdateTick(0) {}
};

struct HealingEventSessionState {
  DWORD lastUpdateTick;
  std::string targetKey;
  std::string itemKey;

  HealingEventSessionState() : lastUpdateTick(0), targetKey(""), itemKey("") {}
};

struct PredationEventSessionState {
  DWORD lastUpdateTick;
  std::string targetKey;

  PredationEventSessionState() : lastUpdateTick(0), targetKey("") {}
};

static std::map<unsigned int, InventorySyncState> g_inventorySyncStateBySerial;
static std::map<std::string, PortraitSyncState> g_portraitSyncStateByStorageId;
static std::map<unsigned int, DWORD> g_portraitSpeechTriggerBySerial;
static std::map<std::string, ItemImageSyncState> g_itemImageSyncStateByItemId;
static std::deque<PendingItemImageSyncRequest> g_itemImageSyncRequestQueue;
static hand g_pendingSelectionContextHand;
static unsigned int g_pendingSelectionContextSerial = 0;
static DWORD g_pendingSelectionContextQueuedTick = 0;
static unsigned int g_lastSelectionSerial = 0;
static DWORD g_lastSelectionContextPushedTick = 0;
static unsigned int g_lastSelectionContextPushedSerial = 0;
static DWORD g_lastInventorySweepTick = 0;
static const DWORD kInventorySweepIntervalMs = 6000;
static const DWORD kInventoryMinResendMs = 1200;
static const size_t kInventorySweepCandidateLimit = 8;
static const DWORD kInventoryStateRetentionMs = 15 * 60 * 1000;
static const size_t kItemImageBatchLimit = 4;
static const size_t kItemImageConsiderMultiplier = 12;
static const DWORD kItemImageMinResendMs = 10 * 60 * 1000;
static const DWORD kItemImageStateRetentionMs = 60 * 60 * 1000;
static const DWORD kItemImageRunCooldownMs = 3 * 1000;
static const DWORD kItemImageStartupDelayMs = 5 * 1000;
static const DWORD kItemImageAttemptCooldownMs = 60 * 1000;
static const DWORD kItemImageInventoryUiHideGraceMs = 15 * 1000;
static const size_t kItemImageRequestQueueMax = 64;
static DWORD g_itemImageLastRunTick = 0;
static DWORD g_lastInventoryUiVisibleTick = 0;
static DWORD g_worldStableSinceTick = 0;
static volatile LONG g_portraitSehCount = 0;
static DWORD g_portraitLastSehTick = 0;
static DWORD g_portraitLastSehCode = 0;
static bool g_portraitSyncDisabledForSession = false;
static bool g_portraitDisableLogged = false;
static DWORD g_portraitSyncDisabledUntilTick = 0;
static DWORD g_lastPortraitSweepTick = 0;
static const DWORD kPortraitSweepIntervalMs = 15000;
static const DWORD kPortraitMinResendMs = 30 * 60 * 1000;
static const size_t kPortraitSweepCandidateLimit = 48;
static const size_t kPortraitSweepBudgetPerPass = 4;
static const DWORD kPortraitStateRetentionMs = 30 * 60 * 1000;
static const DWORD kPortraitSpeechTriggerCooldownMs = 2 * 60 * 1000;
static const DWORD kPortraitSyncBackoffMs = 2 * 60 * 1000;
static const DWORD kPortraitAttemptCooldownMs = 2 * 60 * 1000;
static const DWORD kRecentServerSuccessGraceMs = 10 * 1000;
static size_t g_portraitSweepCursor = 0;
static std::map<unsigned int, NpcWorldEventState> g_npcWorldEventStateBySerial;
static DWORD g_lastNpcWorldEventSweepTick = 0;
static const DWORD kNpcWorldEventSweepIntervalMs = 3000;
static const size_t kNpcWorldEventCandidateLimit = 32;
static const DWORD kNpcWorldEventStateRetentionMs = 10 * 60 * 1000;
static DWORD g_lastIdentityRenameSweepTick = 0;
static const DWORD kIdentityRenameSweepIntervalMs = 2500;
static const size_t kIdentityRenameSweepCandidateLimit = 24;
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
static const char *kStobePluginVersion = "0.9.3";
static const char *kStobePluginReleaseDate = "2026-07-20";
static bool g_pluginVersionSyncHasValue = false;
static std::string g_pluginVersionSyncLastValue = "";
static DWORD g_pluginVersionSyncLastSentTick = 0;
static const DWORD kHookHeavySyncWarmupMs = 45 * 1000;
static const DWORD kNpcWorldEventWarmupMs = 45 * 1000;
static const DWORD kSelectionContextStartupDelayMs = 60 * 1000;
static const DWORD kPluginVersionResendIntervalMs = 10 * 60 * 1000;
static bool g_dynamicProfileIntervalSyncHasValue = false;
static int g_dynamicProfileIntervalSyncLastValue = 0;
static DWORD g_dynamicProfileIntervalSyncLastSentTick = 0;
static const DWORD kDynamicProfileIntervalResendIntervalMs = 5 * 60 * 1000;
static const DWORD kSelectionContextDebounceMs = 1500;
static const DWORD kSelectionContextMinIntervalMs = 8000;
static std::map<unsigned int, PendingMedicalItemUseState>
    g_pendingMedicalItemUseBySerial;
static const DWORD kPendingMedicalItemUseRetentionMs = 20 * 1000;
static std::map<std::string, HealingEventSessionState>
    g_healingEventSessionByActorKey;
static std::map<std::string, DWORD> g_healingEventBurstByTargetKey;
static const DWORD kHealingEventSessionGapMs = 12000;
static const DWORD kHealingEventTargetBurstCooldownMs = 5000;
static const DWORD kHealingEventSessionRetentionMs = 60 * 1000;
static std::map<unsigned int, PredationEventSessionState>
    g_predationEventSessionByEaterSerial;
static const DWORD kPredationEventSessionGapMs = 4000;
static const DWORD kPredationEventSessionRetentionMs = 60 * 1000;
static bool g_playerSquadsSyncHasValue = false;
static std::string g_playerSquadsSyncLastDigest = "";
static DWORD g_playerSquadsSyncLastSentTick = 0;
static std::set<std::string> g_playerSquadsLastKeys;
static const DWORD kPlayerSquadsResendIntervalMs = 60 * 1000;
static std::map<std::string, FactionRelationSnapshotEntry>
    g_factionRelationStateByKey;
static DWORD g_lastFactionRelationSyncTick = 0;
static const DWORD kFactionRelationSyncIntervalMs = 30 * 1000;
static const size_t kFactionRelationSyncHardCap = 262144;
static DWORD g_lastTownKnowledgeScanTick = 0;
static DWORD g_lastTownKnowledgeSentTick = 0;
static std::string g_lastTownKnowledgeDigest = "";
static const DWORD kTownKnowledgeScanIntervalMs = 30 * 1000;
static const DWORD kTownKnowledgeResendIntervalMs = 5 * 60 * 1000;
static const size_t kTownKnowledgeHardCap = 512;

const char *GetStobePluginVersion() {
  return kStobePluginVersion ? kStobePluginVersion : "";
}

static std::string ShortInventoryHashForLog(const std::string &hash) {
  if (hash.length() <= 12) {
    return hash;
  }
  return hash.substr(0, 12);
}

static uint64_t HashFnv1a64(const unsigned char *data, size_t length) {
  const uint64_t kOffset = 1469598103934665603ULL;
  const uint64_t kPrime = 1099511628211ULL;
  uint64_t hash = kOffset;
  if (!data || length == 0) {
    return hash;
  }
  for (size_t i = 0; i < length; ++i) {
    hash ^= static_cast<uint64_t>(data[i]);
    hash *= kPrime;
  }
  return hash;
}

static std::string HexFromU64(uint64_t value) {
  const char *digits = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[i] = digits[static_cast<int>(value & 0xFULL)];
    value >>= 4;
  }
  return out;
}

static std::string Base64EncodeBinary(const std::string &input) {
  static const char *kChars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(((input.size() + 2) / 3) * 4);

  size_t i = 0;
  while (i + 2 < input.size()) {
    const unsigned int a = static_cast<unsigned char>(input[i++]);
    const unsigned int b = static_cast<unsigned char>(input[i++]);
    const unsigned int c = static_cast<unsigned char>(input[i++]);
    const unsigned int triple = (a << 16) | (b << 8) | c;
    output.push_back(kChars[(triple >> 18) & 0x3F]);
    output.push_back(kChars[(triple >> 12) & 0x3F]);
    output.push_back(kChars[(triple >> 6) & 0x3F]);
    output.push_back(kChars[triple & 0x3F]);
  }

  if (i < input.size()) {
    const size_t remaining = input.size() - i;
    const unsigned int a = static_cast<unsigned char>(input[i++]);
    const unsigned int b =
        (remaining > 1) ? static_cast<unsigned char>(input[i++]) : 0;
    const unsigned int triple = (a << 16) | (b << 8);
    if (remaining == 1) {
      output.push_back(kChars[(triple >> 18) & 0x3F]);
      output.push_back(kChars[(triple >> 12) & 0x3F]);
      output.push_back('=');
      output.push_back('=');
    } else {
      output.push_back(kChars[(triple >> 18) & 0x3F]);
      output.push_back(kChars[(triple >> 12) & 0x3F]);
      output.push_back(kChars[(triple >> 6) & 0x3F]);
      output.push_back('=');
    }
  }

  return output;
}

static void WriteUInt32LE(std::string &buffer, size_t offset, uint32_t value) {
  if (offset + 4 > buffer.size()) {
    return;
  }
  buffer[offset + 0] = static_cast<char>(value & 0xFF);
  buffer[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
  buffer[offset + 2] = static_cast<char>((value >> 16) & 0xFF);
  buffer[offset + 3] = static_cast<char>((value >> 24) & 0xFF);
}

static bool EncodeBmp24(const std::vector<unsigned char> &rgba, int width,
                        int height, std::string &bmpDataOut) {
  bmpDataOut.clear();
  if (width <= 0 || height <= 0) {
    return false;
  }
  const size_t expectedBytes =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4U;
  if (rgba.size() < expectedBytes) {
    return false;
  }

  const uint32_t rowStride = static_cast<uint32_t>(width) * 3U;
  const uint32_t paddedRowStride = (rowStride + 3U) & ~3U;
  const uint32_t imageSize =
      paddedRowStride * static_cast<uint32_t>(height);
  const uint32_t fileSize = 54U + imageSize;

  bmpDataOut.assign(fileSize, '\0');
  bmpDataOut[0] = 'B';
  bmpDataOut[1] = 'M';
  WriteUInt32LE(bmpDataOut, 2, fileSize);
  WriteUInt32LE(bmpDataOut, 10, 54U);
  WriteUInt32LE(bmpDataOut, 14, 40U);
  WriteUInt32LE(bmpDataOut, 18, static_cast<uint32_t>(width));
  WriteUInt32LE(bmpDataOut, 22, static_cast<uint32_t>(height));
  bmpDataOut[26] = 1;   // planes
  bmpDataOut[28] = 24;  // bits per pixel
  WriteUInt32LE(bmpDataOut, 34, imageSize);

  unsigned char *dst = reinterpret_cast<unsigned char *>(&bmpDataOut[0]) + 54;
  for (int y = 0; y < height; ++y) {
    const int srcY = height - 1 - y;
    const unsigned char *srcRow =
        &rgba[static_cast<size_t>(srcY) * static_cast<size_t>(width) * 4U];
    unsigned char *dstRow =
        dst + static_cast<size_t>(y) * static_cast<size_t>(paddedRowStride);
    for (int x = 0; x < width; ++x) {
      const unsigned char *srcPx = srcRow + static_cast<size_t>(x) * 4U;
      unsigned char *dstPx = dstRow + static_cast<size_t>(x) * 3U;
      // BMP stores BGR order.
      dstPx[0] = srcPx[2];
      dstPx[1] = srcPx[1];
      dstPx[2] = srcPx[0];
    }
  }
  return true;
}

static bool ResolvePortraitImageRegion(PortraitManager *portraitManager,
                                       PortraitImage *portraitImage,
                                       int &leftOut, int &topOut,
                                       int &widthOut, int &heightOut) {
  leftOut = 0;
  topOut = 0;
  widthOut = 0;
  heightOut = 0;
  if (!portraitManager || (uintptr_t)portraitManager <= 0x1000 ||
      !portraitImage || (uintptr_t)portraitImage <= 0x1000) {
    return false;
  }

  leftOut = portraitImage->coords.left;
  topOut = portraitImage->coords.top;
  widthOut = portraitImage->coords.width;
  heightOut = portraitImage->coords.height;

  if (widthOut <= 0 || heightOut <= 0) {
    widthOut = portraitManager->texturePortraitSize.x;
    heightOut = portraitManager->texturePortraitSize.y;
  }

  Ogre::Texture *texture = portraitManager->texture.getPointer();
  if (!texture || (uintptr_t)texture <= 0x1000) {
    return false;
  }
  const int texWidth = static_cast<int>(texture->getWidth());
  const int texHeight = static_cast<int>(texture->getHeight());
  if (texWidth <= 0 || texHeight <= 0) {
    return false;
  }

  if (leftOut < 0 || topOut < 0 || leftOut >= texWidth || topOut >= texHeight) {
    leftOut = static_cast<int>(portraitImage->textureRect.left * texWidth);
    topOut = static_cast<int>(portraitImage->textureRect.top * texHeight);
  }
  if (widthOut <= 0 || heightOut <= 0) {
    widthOut = static_cast<int>((portraitImage->textureRect.right -
                                 portraitImage->textureRect.left) *
                                texWidth);
    heightOut = static_cast<int>((portraitImage->textureRect.bottom -
                                  portraitImage->textureRect.top) *
                                 texHeight);
  }

  if (leftOut < 0) {
    leftOut = 0;
  }
  if (topOut < 0) {
    topOut = 0;
  }
  if (widthOut <= 0 || heightOut <= 0) {
    return false;
  }
  if (leftOut + widthOut > texWidth) {
    widthOut = texWidth - leftOut;
  }
  if (topOut + heightOut > texHeight) {
    heightOut = texHeight - topOut;
  }
  if (widthOut <= 0 || heightOut <= 0) {
    return false;
  }
  return true;
}

static int PortraitSehFilter(unsigned int code) {
  g_portraitLastSehCode = code;
  g_portraitLastSehTick = GetTickCount();
  InterlockedIncrement(&g_portraitSehCount);
  return EXCEPTION_EXECUTE_HANDLER;
}

static bool HasRecentDwemerDistroConnection(DWORD maxSuccessAgeMs) {
  if (IsDwemerDistroConnected()) {
    return true;
  }

  DWORD ageMs = GetDwemerDistroLastSuccessAgeMs();
  return ageMs != 0xFFFFFFFF && ageMs <= maxSuccessAgeMs;
}

static bool ShouldDeferVisualSyncWhileInventoryVisible(
    Character *npc, const char *syncTag, const std::string &reason,
    DWORD recentVisibleGraceMs = 0);

static bool ShouldAttemptPortraitCapture(const std::string &storageId,
                                         DWORD nowTick, bool force,
                                         const std::string &reason,
                                         std::string *skipReasonOut = nullptr) {
  (void)reason;
  if (skipReasonOut) {
    skipReasonOut->clear();
  }
  if (storageId.empty()) {
    if (skipReasonOut) {
      *skipReasonOut = "empty_storage";
    }
    return false;
  }
  if (!HasRecentDwemerDistroConnection(kRecentServerSuccessGraceMs)) {
    if (skipReasonOut) {
      *skipReasonOut = "offline";
    }
    return false;
  }

  bool allow = false;
  EnterCriticalSection(&g_stateMutex);
  PortraitSyncState &state = g_portraitSyncStateByStorageId[storageId];
  state.lastSeenTick = nowTick;
  DWORD sinceLastAttempt =
      state.lastAttemptTick == 0 ? 0 : (nowTick - state.lastAttemptTick);
  if (force || state.lastAttemptTick == 0 ||
      sinceLastAttempt >= kPortraitAttemptCooldownMs) {
    state.lastAttemptTick = nowTick;
    allow = true;
  }
  LeaveCriticalSection(&g_stateMutex);

  if (!allow && skipReasonOut) {
    *skipReasonOut = "attempt_cooldown";
  }
  return allow;
}

static void MaybeDisablePortraitSyncAfterSeh() {
  DWORD nowTick = GetTickCount();
  if (g_portraitSyncDisabledForSession && nowTick < g_portraitSyncDisabledUntilTick) {
    return;
  }
  LONG sehCount = InterlockedCompareExchange(&g_portraitSehCount, 0, 0);
  if (sehCount >= 3) {
    g_portraitSyncDisabledForSession = true;
    g_portraitSyncDisabledUntilTick = nowTick + kPortraitSyncBackoffMs;
    InterlockedExchange(&g_portraitSehCount, 0);
    if (!g_portraitDisableLogged) {
      g_portraitDisableLogged = true;
      Log("PORTRAIT_SYNC: temporarily disabled after repeated engine SEH faults code=" +
          ToString((int)g_portraitLastSehCode) + " count=" +
          ToString((int)sehCount) + " last_tick=" +
          ToString((int)g_portraitLastSehTick) + " resume_in_ms=" +
          ToString((int)kPortraitSyncBackoffMs));
    }
  }
}

static void RefreshPortraitSyncBackoffState() {
  if (!g_portraitSyncDisabledForSession) {
    return;
  }

  DWORD nowTick = GetTickCount();
  if (g_portraitSyncDisabledUntilTick != 0 && nowTick >= g_portraitSyncDisabledUntilTick) {
    g_portraitSyncDisabledForSession = false;
    g_portraitSyncDisabledUntilTick = 0;
    g_portraitDisableLogged = false;
    InterlockedExchange(&g_portraitSehCount, 0);
    Log("PORTRAIT_SYNC: re-enabled after backoff.");
  }
}

static bool TryCapturePortraitBmpUnsafe(Character *npc, std::string &bmpDataOut,
                                        int &widthOut, int &heightOut,
                                        std::string &imageHashOut,
                                        std::string *diagReasonOut = nullptr) {
  auto fail = [&](const std::string &reason) -> bool {
    if (diagReasonOut) {
      *diagReasonOut = reason;
    }
    return false;
  };
  bmpDataOut.clear();
  imageHashOut.clear();
  widthOut = 0;
  heightOut = 0;
  if (diagReasonOut) {
    diagReasonOut->clear();
  }

  if (!npc || (uintptr_t)npc < 0x1000) {
    return fail("invalid_npc");
  }

  hand characterHandle;
  try {
    characterHandle = npc->getHandle();
  } catch (...) {
    return fail("get_handle_exception");
  }
  if (!characterHandle.isValid()) {
    return fail("invalid_handle");
  }

  PortraitManager *portraitManager = nullptr;
  try {
    portraitManager = PortraitManager::getInstance();
  } catch (...) {
    portraitManager = nullptr;
  }
  if (!portraitManager || (uintptr_t)portraitManager <= 0x1000) {
    return fail("portrait_manager_unavailable");
  }

  try {
    portraitManager->getPortrait(characterHandle);
  } catch (...) {
  }

  bool updateOk = false;
  try {
    updateOk = portraitManager->updatePortraitImage(characterHandle);
  } catch (...) {
    updateOk = false;
  }
  if (!updateOk) {
    try {
      portraitManager->getPortrait(characterHandle);
      updateOk = portraitManager->updatePortraitImage(characterHandle);
    } catch (...) {
      updateOk = false;
    }
  }
  if (!updateOk) {
    return fail("update_portrait_image_failed");
  }

  PortraitImage *portraitImage = nullptr;
  try {
    auto it = portraitManager->characterPortraits.find(characterHandle);
    if (it == portraitManager->characterPortraits.end()) {
      return fail("portrait_map_missing");
    }
    portraitImage = it->second.second;
  } catch (...) {
    return fail("portrait_map_exception");
  }
  if (!portraitImage || (uintptr_t)portraitImage <= 0x1000) {
    return fail("portrait_image_invalid");
  }

  int srcLeft = 0;
  int srcTop = 0;
  int srcWidth = 0;
  int srcHeight = 0;
  if (!ResolvePortraitImageRegion(portraitManager, portraitImage, srcLeft, srcTop,
                                  srcWidth, srcHeight)) {
    return fail("resolve_region_failed");
  }

  Ogre::Texture *texture = portraitManager->texture.getPointer();
  if (!texture || (uintptr_t)texture <= 0x1000) {
    return fail("texture_unavailable");
  }
  std::vector<unsigned char> rgba;
  try {
    Ogre::Image textureImage;
    texture->convertToImage(textureImage, false);
    Ogre::PixelBox pixelBox = textureImage.getPixelBox(0, 0);
    if (!pixelBox.data) {
      return fail("image_pixel_box_no_data");
    }
    if (!Ogre::PixelUtil::isAccessible(pixelBox.format)) {
      return fail("image_pixel_format_inaccessible");
    }

    const size_t elemBytes = Ogre::PixelUtil::getNumElemBytes(pixelBox.format);
    if (elemBytes == 0) {
      return fail("image_pixel_elem_bytes_zero");
    }

    const int imageWidth = static_cast<int>(pixelBox.getWidth());
    const int imageHeight = static_cast<int>(pixelBox.getHeight());
    if (srcLeft < 0 || srcTop < 0 || srcWidth <= 0 || srcHeight <= 0 ||
        srcLeft + srcWidth > imageWidth || srcTop + srcHeight > imageHeight) {
      return fail("image_crop_out_of_bounds");
    }

    rgba.assign(static_cast<size_t>(srcWidth) * static_cast<size_t>(srcHeight) * 4U,
                0U);
    const unsigned char *base = static_cast<const unsigned char *>(pixelBox.data);
    for (int y = 0; y < srcHeight; ++y) {
      for (int x = 0; x < srcWidth; ++x) {
        const size_t srcOffset =
            (static_cast<size_t>(srcTop + y) * static_cast<size_t>(pixelBox.rowPitch) +
             static_cast<size_t>(srcLeft + x)) *
            elemBytes;
        const unsigned char *srcPixel = base + srcOffset;
        unsigned char *dstPixel =
            &rgba[(static_cast<size_t>(y) * static_cast<size_t>(srcWidth) +
                   static_cast<size_t>(x)) *
                  4U];
        Ogre::PixelUtil::unpackColour(&dstPixel[0], &dstPixel[1], &dstPixel[2],
                                      &dstPixel[3], pixelBox.format, srcPixel);
      }
    }
  } catch (...) {
    return fail("image_readback_exception");
  }

  if (!EncodeBmp24(rgba, srcWidth, srcHeight, bmpDataOut) || bmpDataOut.empty()) {
    return fail("encode_bmp_failed");
  }

  imageHashOut = HexFromU64(HashFnv1a64(
      reinterpret_cast<const unsigned char *>(bmpDataOut.data()),
      bmpDataOut.size()));
  widthOut = srcWidth;
  heightOut = srcHeight;
  return true;
}

static bool TryCapturePortraitBmp(Character *npc, std::string &bmpDataOut,
                                  int &widthOut, int &heightOut,
                                  std::string &imageHashOut,
                                  std::string *diagReasonOut = nullptr) {
  RefreshPortraitSyncBackoffState();
  if (g_portraitSyncDisabledForSession) {
    if (diagReasonOut) {
      *diagReasonOut = "portrait_sync_disabled_for_session";
    }
    return false;
  }

  __try {
    return TryCapturePortraitBmpUnsafe(npc, bmpDataOut, widthOut, heightOut,
                                       imageHashOut, diagReasonOut);
  } __except (PortraitSehFilter(GetExceptionCode())) {
    if (diagReasonOut) {
      *diagReasonOut = "portrait_capture_seh";
    }
    MaybeDisablePortraitSyncAfterSeh();
    return false;
  }
}

static bool ShouldSendPortraitSync(const std::string &storageId,
                                   const std::string &imageHash, DWORD nowTick,
                                   bool force, DWORD &sinceLastSentOut,
                                   bool &changedOut, bool &firstOut) {
  sinceLastSentOut = 0;
  changedOut = false;
  firstOut = false;
  if (storageId.empty() || imageHash.empty()) {
    return false;
  }

  bool shouldSend = false;
  EnterCriticalSection(&g_stateMutex);
  PortraitSyncState &state = g_portraitSyncStateByStorageId[storageId];
  state.lastSeenTick = nowTick;
  changedOut = (state.lastHash != imageHash);
  firstOut = !state.hasSent;
  sinceLastSentOut = state.hasSent ? (nowTick - state.lastSentTick) : 0;
  if (force || firstOut || changedOut || sinceLastSentOut >= kPortraitMinResendMs) {
    shouldSend = true;
    state.lastHash = imageHash;
    state.lastSentTick = nowTick;
    state.hasSent = true;
  }
  LeaveCriticalSection(&g_stateMutex);
  return shouldSend;
}

static bool SyncPortraitForCharacterUnsafe(Character *npc, bool force,
                                           const std::string &reason) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return false;
  }

  unsigned int serial = 0;
  std::string npcName = "Unknown";
  std::string storageId = "";
  try {
    serial = npc->getHandle().serial;
    npcName = npc->getName();
    storageId = GetStorageIDFor(npc, npcName, GetIdentityFaction(npc));
  } catch (...) {
    return false;
  }
  if (serial == 0) {
    return false;
  }
  if (storageId.empty()) {
    storageId = "hand_" + ToString((int)serial);
  }
  if (ShouldDeferVisualSyncWhileInventoryVisible(npc, "PORTRAIT_SYNC",
                                                 reason, 1500)) {
    return false;
  }

  DWORD nowTick = GetTickCount();
  std::string skipReason = "";
  if (!ShouldAttemptPortraitCapture(storageId, nowTick, force, reason,
                                    &skipReason)) {
    return false;
  }

  std::string bmpData = "";
  std::string imageHash = "";
  std::string captureReason = "";
  int width = 0;
  int height = 0;
  if (!TryCapturePortraitBmp(npc, bmpData, width, height, imageHash,
                             &captureReason)) {
    static DWORD lastCaptureFailLogTick = 0;
    DWORD nowTick = GetTickCount();
    if (nowTick - lastCaptureFailLogTick >= 5000) {
      lastCaptureFailLogTick = nowTick;
      Log("PORTRAIT_SYNC: capture failed name=" + npcName +
          " storage=" + storageId + " reason=" + captureReason);
    }
    return false;
  }
  if (bmpData.empty() || imageHash.empty() || width <= 0 || height <= 0) {
    static DWORD lastInvalidSampleLogTick = 0;
    DWORD nowTick = GetTickCount();
    if (nowTick - lastInvalidSampleLogTick >= 5000) {
      lastInvalidSampleLogTick = nowTick;
      Log("PORTRAIT_SYNC: invalid sample name=" + npcName +
          " storage=" + storageId + " width=" + ToString(width) +
          " height=" + ToString(height) + " hash_len=" +
          ToString((int)imageHash.length()) + " bytes=" +
          ToString((int)bmpData.length()));
    }
    return false;
  }

  DWORD sinceLastSent = 0;
  bool changed = false;
  bool firstSync = false;
  if (!ShouldSendPortraitSync(storageId, imageHash, nowTick, force, sinceLastSent,
                              changed, firstSync)) {
    return false;
  }

  std::string base64Image = Base64EncodeBinary(bmpData);
  if (base64Image.empty()) {
    static DWORD lastBase64FailLogTick = 0;
    DWORD nowTick = GetTickCount();
    if (nowTick - lastBase64FailLogTick >= 5000) {
      lastBase64FailLogTick = nowTick;
      Log("PORTRAIT_SYNC: base64 encode failed name=" + npcName +
          " storage=" + storageId + " bytes=" + ToString((int)bmpData.length()));
    }
    return false;
  }

  int gameTs = 0;
  try {
    GameWorld *world = GetWorldSafe();
    if (world) {
      TimeOfDay tod = world->getTimeStamp_inGameHours();
      gameTs = static_cast<int>(tod.getTotalSeconds());
    }
  } catch (...) {
    gameTs = 0;
  }

  std::string payload = "{";
  payload += "\"name\":\"" + EscapeJSON(npcName) + "\",";
  payload += "\"storage_id\":\"" + EscapeJSON(storageId) + "\",";
  payload += "\"source\":\"player_faction_portrait_sync\",";
  payload += "\"sync_reason\":\"" + EscapeJSON(reason) + "\",";
  payload += "\"image_hash\":\"" + EscapeJSON(imageHash) + "\",";
  payload += "\"format\":\"bmp\",";
  payload += "\"width\":" + ToString(width) + ",";
  payload += "\"height\":" + ToString(height) + ",";
  payload += "\"game_ts\":" + ToString(gameTs) + ",";
  payload += "\"image_base64\":\"" + EscapeJSON(base64Image) + "\"";
  payload += "}";

  AsyncPostToStobe(L"/portrait_upload", payload);
  Log("PORTRAIT_SYNC: sent name=" + npcName + " storage=" + storageId +
      " hash=" + imageHash + " size=" + ToString(width) + "x" +
      ToString(height) + " changed=" + std::string(changed ? "1" : "0") +
      " first=" + std::string(firstSync ? "1" : "0") + " reason=" + reason);
  return true;
}

static bool SyncPortraitForCharacter(Character *npc, bool force,
                                     const std::string &reason) {
  RefreshPortraitSyncBackoffState();
  if (g_portraitSyncDisabledForSession) {
    return false;
  }

  __try {
    return SyncPortraitForCharacterUnsafe(npc, force, reason);
  } __except (PortraitSehFilter(GetExceptionCode())) {
    MaybeDisablePortraitSyncAfterSeh();
    return false;
  }
}

static bool ShouldTriggerSpeechPortraitSync(unsigned int serial, DWORD nowTick) {
  if (serial == 0) {
    return false;
  }

  bool shouldTrigger = false;
  EnterCriticalSection(&g_stateMutex);
  std::map<unsigned int, DWORD>::iterator it =
      g_portraitSpeechTriggerBySerial.find(serial);
  if (it == g_portraitSpeechTriggerBySerial.end() || it->second == 0 ||
      nowTick - it->second >= kPortraitSpeechTriggerCooldownMs) {
    g_portraitSpeechTriggerBySerial[serial] = nowTick;
    shouldTrigger = true;
  }
  LeaveCriticalSection(&g_stateMutex);
  return shouldTrigger;
}

static void TrySpeechTriggeredPortraitSync(Character *npc,
                                           const std::string &reason) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return;
  }

  bool isPlayerCharacter = false;
  try {
    isPlayerCharacter = npc->isPlayerCharacter();
  } catch (...) {
    isPlayerCharacter = false;
  }
  if (isPlayerCharacter) {
    return;
  }

  unsigned int serial = 0;
  try {
    serial = npc->getHandle().serial;
  } catch (...) {
    serial = 0;
  }
  if (serial == 0) {
    return;
  }
  DWORD nowTick = GetTickCount();
  if (!ShouldTriggerSpeechPortraitSync(serial, nowTick)) {
    return;
  }

  SyncPortraitForCharacter(npc, true, reason);
}

static void TrySpeechTriggeredPortraitSyncForPair(Character *speaker,
                                                  Character *listener,
                                                  const char *sourceTag) {
  std::string reasonBase = "speech_trigger";
  if (sourceTag && sourceTag[0] != '\0') {
    reasonBase = reasonBase + "_" + std::string(sourceTag);
  }

  TrySpeechTriggeredPortraitSync(speaker, reasonBase + "_speaker");
  if (listener && listener != speaker) {
    TrySpeechTriggeredPortraitSync(listener, reasonBase + "_listener");
  }
}

static void PrunePortraitSyncState() {
  DWORD nowTick = GetTickCount();
  int pruned = 0;
  EnterCriticalSection(&g_stateMutex);
  for (std::map<std::string, PortraitSyncState>::iterator it =
           g_portraitSyncStateByStorageId.begin();
       it != g_portraitSyncStateByStorageId.end();) {
    DWORD age = nowTick - it->second.lastSeenTick;
    if (it->second.lastSeenTick == 0 || age > kPortraitStateRetentionMs) {
      it = g_portraitSyncStateByStorageId.erase(it);
      ++pruned;
    } else {
      ++it;
    }
  }

  for (std::map<unsigned int, DWORD>::iterator it =
           g_portraitSpeechTriggerBySerial.begin();
       it != g_portraitSpeechTriggerBySerial.end();) {
    DWORD age = nowTick - it->second;
    if (it->second == 0 || age > kPortraitStateRetentionMs) {
      it = g_portraitSpeechTriggerBySerial.erase(it);
    } else {
      ++it;
    }
  }
  LeaveCriticalSection(&g_stateMutex);
  if (pruned > 0) {
    Log("PORTRAIT_SYNC: pruned stale state entries=" + ToString(pruned));
  }
}

static void ResetPortraitSyncState() {
  EnterCriticalSection(&g_stateMutex);
  g_portraitSyncStateByStorageId.clear();
  g_portraitSpeechTriggerBySerial.clear();
  LeaveCriticalSection(&g_stateMutex);
  g_portraitSyncDisabledForSession = false;
  g_portraitDisableLogged = false;
  g_portraitSyncDisabledUntilTick = 0;
  g_portraitLastSehTick = 0;
  g_portraitLastSehCode = 0;
  g_lastPortraitSweepTick = 0;
  g_portraitSweepCursor = 0;
  InterlockedExchange(&g_portraitSehCount, 0);
}

static bool IsWorldStableForUI(GameWorld *world);

static void RunPlayerFactionPortraitSweep(GameWorld *world) {
  if (!IsWorldStableForUI(world)) {
    static DWORD lastNoRosterLogTick = 0;
    DWORD nowTick = GetTickCount();
    if (nowTick - lastNoRosterLogTick >= 30000) {
      lastNoRosterLogTick = nowTick;
      Log("PORTRAIT_SYNC: skipped sweep (no player roster)");
    }
    return;
  }

  DWORD nowTick = GetTickCount();
  if (nowTick - g_lastPortraitSweepTick < kPortraitSweepIntervalMs) {
    return;
  }
  g_lastPortraitSweepTick = nowTick;

  const size_t rosterSize = world->player->playerCharacters.size();
  if (rosterSize == 0) {
    g_portraitSweepCursor = 0;
    return;
  }
  if (g_portraitSweepCursor >= rosterSize) {
    g_portraitSweepCursor = 0;
  }
  if (!HasRecentDwemerDistroConnection(kRecentServerSuccessGraceMs)) {
    static DWORD lastOfflineLogTick = 0;
    if (nowTick - lastOfflineLogTick >= 30000) {
      lastOfflineLogTick = nowTick;
      Log("PORTRAIT_SYNC: sweep deferred while server connection is unavailable.");
    }
    return;
  }

  size_t candidates = 0;
  size_t sent = 0;
  size_t inspectedSlots = 0;
  const size_t maxInspect =
      std::min(static_cast<size_t>(kPortraitSweepCandidateLimit), rosterSize);
  const size_t budget =
      std::min(static_cast<size_t>(kPortraitSweepBudgetPerPass), maxInspect);
  for (; inspectedSlots < maxInspect && candidates < budget; ++inspectedSlots) {
    size_t rosterIndex = (g_portraitSweepCursor + inspectedSlots) % rosterSize;
    Character *member = world->player->playerCharacters[rosterIndex];
    if (!member || (uintptr_t)member < 0x1000) {
      continue;
    }
    ++candidates;
    if (SyncPortraitForCharacter(member, false, "player_faction_sweep")) {
      ++sent;
    }
  }
  if (inspectedSlots == 0) {
    inspectedSlots = 1;
  }
  g_portraitSweepCursor = (g_portraitSweepCursor + inspectedSlots) % rosterSize;

  static DWORD lastPruneTick = 0;
  if (nowTick - lastPruneTick >= 60000) {
    lastPruneTick = nowTick;
    PrunePortraitSyncState();
  }

  if (sent > 0) {
    Log("PORTRAIT_SYNC: sweep complete candidates=" + ToString((int)candidates) +
        " sent=" + ToString((int)sent) +
        " inspected=" + ToString((int)inspectedSlots));
  } else {
    static DWORD lastNoSendLogTick = 0;
    if (nowTick - lastNoSendLogTick >= 60000) {
      lastNoSendLogTick = nowTick;
      Log("PORTRAIT_SYNC: sweep no-send candidates=" +
          ToString((int)candidates) +
          " inspected=" + ToString((int)inspectedSlots));
    }
  }
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

static void ResetFactionRelationSyncState() {
  EnterCriticalSection(&g_stateMutex);
  g_factionRelationStateByKey.clear();
  g_lastFactionRelationSyncTick = 0;
  LeaveCriticalSection(&g_stateMutex);
}

static void ResetTownKnowledgeSyncState() {
  EnterCriticalSection(&g_stateMutex);
  g_lastTownKnowledgeScanTick = 0;
  g_lastTownKnowledgeSentTick = 0;
  g_lastTownKnowledgeDigest = "";
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
  int intervalHours = g_dynamicProfileIntervalHours;
  if (intervalHours < 1) {
    intervalHours = 1;
  } else if (intervalHours > 720) {
    intervalHours = 720;
  }

  DWORD nowTick = GetTickCount();
  bool shouldSend = false;
  bool changed = false;
  DWORD sinceLastSent = 0;
  EnterCriticalSection(&g_stateMutex);
  changed = (!g_dynamicProfileIntervalSyncHasValue ||
             intervalHours != g_dynamicProfileIntervalSyncLastValue);
  sinceLastSent = g_dynamicProfileIntervalSyncHasValue
                      ? (nowTick - g_dynamicProfileIntervalSyncLastSentTick)
                      : 0;
  if (force || !g_dynamicProfileIntervalSyncHasValue || changed ||
      sinceLastSent >= kDynamicProfileIntervalResendIntervalMs) {
    shouldSend = true;
    g_dynamicProfileIntervalSyncHasValue = true;
    g_dynamicProfileIntervalSyncLastValue = intervalHours;
    g_dynamicProfileIntervalSyncLastSentTick = nowTick;
  }
  LeaveCriticalSection(&g_stateMutex);

  if (!shouldSend) {
    return false;
  }

  std::string payload =
      "{\"id\":\"DYNAMIC_PROFILE_INTERVAL_HOURS\",\"value\":\"" +
      ToString(intervalHours) + "\",\"only_if_changed\":true}";
  AsyncPostToStobe(L"/conf_opts", payload);
  Log("DYNAMIC_PROFILE_SYNC: sent interval_hours=" +
      ToString(intervalHours) + " changed=" +
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

static volatile LONG g_inventorySyncSehCount = 0;
static DWORD g_inventoryLastSehTick = 0;
static DWORD g_inventoryLastSehCode = 0;

static int InventorySyncSehFilter(unsigned int code) {
  g_inventoryLastSehCode = code;
  g_inventoryLastSehTick = GetTickCount();
  InterlockedIncrement(&g_inventorySyncSehCount);
  return EXCEPTION_EXECUTE_HANDLER;
}

static volatile LONG g_itemImageSehCount = 0;
static DWORD g_itemImageLastSehTick = 0;
static DWORD g_itemImageLastSehCode = 0;
static bool g_itemImageSyncDisabledForSession = false;
static bool g_itemImageDisableLogged = false;

static int ItemImageSehFilter(unsigned int code) {
  g_itemImageLastSehCode = code;
  g_itemImageLastSehTick = GetTickCount();
  InterlockedIncrement(&g_itemImageSehCount);
  return EXCEPTION_EXECUTE_HANDLER;
}

static void MaybeDisableItemImageSyncAfterSeh() {
  if (g_itemImageSyncDisabledForSession) {
    return;
  }
  LONG sehCount = InterlockedCompareExchange(&g_itemImageSehCount, 0, 0);
  if (sehCount >= 3) {
    g_itemImageSyncDisabledForSession = true;
    if (!g_itemImageDisableLogged) {
      g_itemImageDisableLogged = true;
      Log("ITEM_IMAGE_SYNC: disabled for this session after repeated engine SEH faults code=" +
          ToString((int)g_itemImageLastSehCode) + " count=" +
          ToString((int)sehCount) + " last_tick=" +
          ToString((int)g_itemImageLastSehTick));
    }
  }
}

static std::string NormalizeItemImageStateKey(const std::string &itemId) {
  std::string key = TrimCopy(itemId);
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<char>(tolower((unsigned char)key[i]));
  }
  return key;
}

static std::string BuildSyntheticItemStringIdFromName(const std::string &itemName) {
  std::string normalized = ToLowerAsciiCopy(TrimCopy(itemName));
  if (normalized.empty()) {
    return "";
  }

  std::string slug = "";
  slug.reserve(normalized.size());
  bool lastWasUnderscore = false;
  for (size_t i = 0; i < normalized.size(); ++i) {
    unsigned char ch = (unsigned char)normalized[i];
    bool isAlphaNum = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    if (isAlphaNum) {
      slug.push_back((char)ch);
      lastWasUnderscore = false;
      continue;
    }
    if (!slug.empty() && !lastWasUnderscore) {
      slug.push_back('_');
      lastWasUnderscore = true;
    }
  }
  while (!slug.empty() && slug.back() == '_') {
    slug.pop_back();
  }
  if (slug.empty()) {
    return "";
  }
  if (slug.length() > 110) {
    slug = slug.substr(0, 110);
    while (!slug.empty() && slug.back() == '_') {
      slug.pop_back();
    }
  }
  if (slug.empty()) {
    return "";
  }
  return "name_" + slug;
}

static bool IsNpcInventoryWindowVisible(Character *npc) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return false;
  }

  Inventory *inventory = nullptr;
  try {
    inventory = npc->getInventory();
  } catch (...) {
    inventory = nullptr;
  }
  if (!inventory || (uintptr_t)inventory < 0x1000) {
    return false;
  }

  InventoryGUI *inventoryGui = nullptr;
  try {
    inventoryGui = inventory->getInventoryGUI();
  } catch (...) {
    inventoryGui = nullptr;
  }
  if (!inventoryGui || (uintptr_t)inventoryGui < 0x1000) {
    return false;
  }

  try {
    return inventoryGui->isVisible();
  } catch (...) {
    return false;
  }
}

static bool IsAnyPlayerInventoryWindowVisible() {
  GameWorld *world = GetWorldSafe();
  if (!world || !world->player) {
    return false;
  }

  try {
    for (uint32_t i = 0; i < world->player->playerCharacters.size(); ++i) {
      Character *playerChar = world->player->playerCharacters[i];
      if (IsNpcInventoryWindowVisible(playerChar)) {
        return true;
      }
    }
  } catch (...) {
    return false;
  }

  return false;
}

static bool IsAnyInventoryOrTradeUiVisible(Character *npc) {
  return IsNpcInventoryWindowVisible(npc) ||
         IsAnyPlayerInventoryWindowVisible();
}

static bool ShouldDeferVisualSyncWhileInventoryVisible(
    Character *npc, const char *syncTag, const std::string &reason,
    DWORD recentVisibleGraceMs) {
  DWORD nowTick = GetTickCount();
  bool inventoryUiVisibleNow = IsAnyInventoryOrTradeUiVisible(npc);
  DWORD lastVisibleTick = 0;

  if (inventoryUiVisibleNow) {
    EnterCriticalSection(&g_stateMutex);
    g_lastInventoryUiVisibleTick = nowTick;
    lastVisibleTick = g_lastInventoryUiVisibleTick;
    LeaveCriticalSection(&g_stateMutex);
  } else if (recentVisibleGraceMs != 0) {
    EnterCriticalSection(&g_stateMutex);
    lastVisibleTick = g_lastInventoryUiVisibleTick;
    LeaveCriticalSection(&g_stateMutex);
  }

  bool inventoryUiRecentlyVisible =
      !inventoryUiVisibleNow && recentVisibleGraceMs != 0 &&
      lastVisibleTick != 0 &&
      (nowTick - lastVisibleTick) < recentVisibleGraceMs;
  if (!inventoryUiVisibleNow && !inventoryUiRecentlyVisible) {
    return false;
  }

  static DWORD lastInventoryUiDeferLogTick = 0;
  if (syncTag && syncTag[0] != '\0' &&
      nowTick - lastInventoryUiDeferLogTick >= 5000) {
    lastInventoryUiDeferLogTick = nowTick;
    Log(std::string(syncTag) +
        ": deferred while inventory UI " +
        std::string(inventoryUiVisibleNow ? "visible" : "recently visible") +
        " reason=" + reason);
  }
  return true;
}

static bool IsItemImageSyncReasonAllowed(const std::string &reason) {
  std::string key = ToLowerAsciiCopy(TrimCopy(reason));
  if (key.empty()) {
    return false;
  }
  if (key == "selection_change" || key == "chat_open" ||
      key == "dialogue_npc" || key == "dialogue_player" ||
      key == "periodic") {
    return true;
  }
  if (key.find("dialogue") != std::string::npos ||
      key.find("description") != std::string::npos ||
      key.find("chat") != std::string::npos ||
      key.find("inventory") != std::string::npos) {
    return true;
  }
  return false;
}

static bool ShouldQueueItemImageSyncForInventorySync(const std::string &reason,
                                                     bool firstSync,
                                                     bool hashChanged) {
  std::string key = ToLowerAsciiCopy(TrimCopy(reason));
  if (!IsItemImageSyncReasonAllowed(key)) {
    return false;
  }
  if (key == "selection_change") {
    return firstSync || hashChanged;
  }
  return true;
}

static bool ShouldAllowItemImageSyncWork(const std::string &reason,
                                         std::string *blockReasonOut = nullptr) {
  if (blockReasonOut) {
    *blockReasonOut = "";
  }
  if (!g_enableItemImageSync) {
    if (blockReasonOut) {
      *blockReasonOut = "setting_disabled";
    }
    return false;
  }
  if (g_itemImageSyncDisabledForSession) {
    if (blockReasonOut) {
      *blockReasonOut = "session_disabled";
    }
    return false;
  }
  std::string reasonKey = ToLowerAsciiCopy(TrimCopy(reason));
  if (!IsItemImageSyncReasonAllowed(reasonKey)) {
    if (blockReasonOut) {
      *blockReasonOut = "reason_not_allowed";
    }
    return false;
  }
  if (!HasRecentDwemerDistroConnection(kRecentServerSuccessGraceMs)) {
    if (blockReasonOut) {
      *blockReasonOut = "offline";
    }
    return false;
  }
  return true;
}

static bool ShouldRunItemImageSyncNow(const std::string &reason, bool force,
                                      std::string *blockReasonOut = nullptr) {
  (void)force;
  if (!ShouldAllowItemImageSyncWork(reason, blockReasonOut)) {
    return false;
  }

  DWORD nowTick = GetTickCount();
  bool allow = false;
  EnterCriticalSection(&g_stateMutex);
  DWORD stableTick = g_worldStableSinceTick;
  DWORD sinceLastRun =
      g_itemImageLastRunTick == 0 ? 0 : (nowTick - g_itemImageLastRunTick);
  bool startupDelayPassed =
      stableTick != 0 && (nowTick - stableTick) >= kItemImageStartupDelayMs;
  bool cooldownPassed =
      g_itemImageLastRunTick == 0 || sinceLastRun >= kItemImageRunCooldownMs;
  if (startupDelayPassed && cooldownPassed) {
    allow = true;
  }
  LeaveCriticalSection(&g_stateMutex);
  if (!allow && blockReasonOut) {
    *blockReasonOut = startupDelayPassed ? "run_cooldown" : "startup_delay";
  }
  return allow;
}

static bool ShouldAttemptItemImageCapture(const std::string &itemId, DWORD nowTick,
                                          bool force,
                                          std::string *skipReasonOut = nullptr) {
  if (skipReasonOut) {
    *skipReasonOut = "";
  }
  std::string stateKey = NormalizeItemImageStateKey(itemId);
  if (stateKey.empty()) {
    if (skipReasonOut) {
      *skipReasonOut = "invalid_item_id";
    }
    return false;
  }

  bool allow = false;
  EnterCriticalSection(&g_stateMutex);
  ItemImageSyncState &state = g_itemImageSyncStateByItemId[stateKey];
  state.lastSeenTick = nowTick;
  DWORD sinceLastAttempt =
      state.lastAttemptTick == 0 ? 0 : (nowTick - state.lastAttemptTick);
  if (force || state.lastAttemptTick == 0 ||
      sinceLastAttempt >= kItemImageAttemptCooldownMs) {
    state.lastAttemptTick = nowTick;
    allow = true;
  }
  LeaveCriticalSection(&g_stateMutex);

  if (!allow && skipReasonOut) {
    *skipReasonOut = "attempt_cooldown";
  }
  return allow;
}

static bool ShouldSendItemImageSync(const std::string &itemId,
                                    const std::string &imageHash, DWORD nowTick,
                                    bool force, DWORD &sinceLastSentOut,
                                    bool &changedOut, bool &firstOut) {
  sinceLastSentOut = 0;
  changedOut = false;
  firstOut = false;
  if (itemId.empty() || imageHash.empty()) {
    return false;
  }

  std::string stateKey = NormalizeItemImageStateKey(itemId);
  if (stateKey.empty()) {
    return false;
  }

  bool shouldSend = false;
  EnterCriticalSection(&g_stateMutex);
  ItemImageSyncState &state = g_itemImageSyncStateByItemId[stateKey];
  state.lastSeenTick = nowTick;
  changedOut = (state.lastHash != imageHash);
  firstOut = !state.hasSent;
  sinceLastSentOut = state.hasSent ? (nowTick - state.lastSentTick) : 0;
  if (force || firstOut || changedOut || sinceLastSentOut >= kItemImageMinResendMs) {
    shouldSend = true;
    state.lastHash = imageHash;
    state.lastSentTick = nowTick;
    state.hasSent = true;
  }
  LeaveCriticalSection(&g_stateMutex);
  return shouldSend;
}

static void QueueItemImageSyncRequest(Character *npc, const std::string &reason) {
  if (!g_enableItemImageSync) {
    return;
  }
  if (!npc || (uintptr_t)npc < 0x1000) {
    return;
  }

  hand npcHand;
  unsigned int serial = 0;
  try {
    npcHand = npc->getHandle();
    serial = npcHand.serial;
  } catch (...) {
    return;
  }
  if (serial == 0 || !npcHand.isValid()) {
    return;
  }

  DWORD nowTick = GetTickCount();
  EnterCriticalSection(&g_stateMutex);
  for (size_t i = 0; i < g_itemImageSyncRequestQueue.size(); ++i) {
    if (g_itemImageSyncRequestQueue[i].npcHand.serial == serial) {
      g_itemImageSyncRequestQueue[i].reason = reason;
      g_itemImageSyncRequestQueue[i].queuedTick = nowTick;
      LeaveCriticalSection(&g_stateMutex);
      return;
    }
  }

  PendingItemImageSyncRequest req;
  req.npcHand = npcHand;
  req.reason = reason;
  req.queuedTick = nowTick;
  g_itemImageSyncRequestQueue.push_back(req);
  while (g_itemImageSyncRequestQueue.size() > kItemImageRequestQueueMax) {
    g_itemImageSyncRequestQueue.pop_front();
  }
  LeaveCriticalSection(&g_stateMutex);
}

static std::string TruncateItemImageDiagValue(const std::string &value,
                                              size_t maxLen = 72) {
  if (value.length() <= maxLen) {
    return value;
  }
  if (maxLen <= 3) {
    return value.substr(0, maxLen);
  }
  return value.substr(0, maxLen - 3) + "...";
}

static bool HasKnownImageExtension(const std::string &name) {
  return EndsWithAsciiInsensitive(name, ".dds") ||
         EndsWithAsciiInsensitive(name, ".png") ||
         EndsWithAsciiInsensitive(name, ".tga") ||
         EndsWithAsciiInsensitive(name, ".bmp") ||
         EndsWithAsciiInsensitive(name, ".jpg") ||
         EndsWithAsciiInsensitive(name, ".jpeg") ||
         EndsWithAsciiInsensitive(name, ".webp");
}

static void AppendUniqueIconResourceCandidate(std::vector<std::string> &out,
                                              const std::string &candidate) {
  std::string trimmed = TrimCopy(candidate);
  if (trimmed.empty()) {
    return;
  }
  std::string lowered = ToLowerAsciiCopy(trimmed);
  for (size_t i = 0; i < out.size(); ++i) {
    if (ToLowerAsciiCopy(out[i]) == lowered) {
      return;
    }
  }
  out.push_back(trimmed);
}

static std::vector<Ogre::String>
BuildItemIconResourceCandidates(const std::string &iconImageName) {
  std::vector<std::string> nativeCandidates;
  std::string trimmed = TrimCopy(iconImageName);
  AppendUniqueIconResourceCandidate(nativeCandidates, trimmed);

  std::string normalizedSlashes = trimmed;
  for (size_t i = 0; i < normalizedSlashes.size(); ++i) {
    if (normalizedSlashes[i] == '\\') {
      normalizedSlashes[i] = '/';
    }
  }
  AppendUniqueIconResourceCandidate(nativeCandidates, normalizedSlashes);

  if (!HasKnownImageExtension(trimmed)) {
    const char *exts[] = {".dds", ".png", ".tga", ".bmp", ".jpg", ".jpeg"};
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); ++i) {
      AppendUniqueIconResourceCandidate(nativeCandidates, trimmed + exts[i]);
      AppendUniqueIconResourceCandidate(nativeCandidates,
                                        normalizedSlashes + exts[i]);
    }
  }

  std::vector<Ogre::String> ogreCandidates;
  ogreCandidates.reserve(nativeCandidates.size());
  for (size_t i = 0; i < nativeCandidates.size(); ++i) {
    ogreCandidates.push_back(Ogre::String(nativeCandidates[i].c_str()));
  }
  return ogreCandidates;
}

static bool TryLoadItemIconImage(const std::string &iconImageName,
                                 Ogre::Image &iconImage,
                                 std::string &resolvedNameOut,
                                 std::string &resolvedGroupOut,
                                 std::string *diagReasonOut = nullptr) {
  resolvedNameOut.clear();
  resolvedGroupOut.clear();
  if (diagReasonOut) {
    diagReasonOut->clear();
  }

  std::vector<Ogre::String> candidates =
      BuildItemIconResourceCandidates(iconImageName);
  if (candidates.empty()) {
    if (diagReasonOut) {
      *diagReasonOut = "icon_candidates_empty";
    }
    return false;
  }

  const char *groupCandidates[] = {"General", "GUI", "Characters", "Materials",
                                   ""};

  for (size_t c = 0; c < candidates.size(); ++c) {
    const Ogre::String &candidate = candidates[c];
    for (size_t g = 0; g < sizeof(groupCandidates) / sizeof(groupCandidates[0]);
         ++g) {
      try {
        iconImage.load(candidate, Ogre::String(groupCandidates[g]));
        resolvedNameOut = candidate.c_str();
        resolvedGroupOut = groupCandidates[g];
        return true;
      } catch (...) {
      }
    }
  }

  if (diagReasonOut) {
    *diagReasonOut =
        "icon_load_failed:" + TruncateItemImageDiagValue(iconImageName);
  }
  return false;
}

static bool ExtractIconRectToRgba(const Ogre::PixelBox &pixelBox, size_t elemBytes,
                                  int srcLeft, int srcTop, int width, int height,
                                  std::vector<unsigned char> &rgbaOut) {
  rgbaOut.clear();
  if (width <= 0 || height <= 0 || srcLeft < 0 || srcTop < 0) {
    return false;
  }
  const int imageWidth = static_cast<int>(pixelBox.getWidth());
  const int imageHeight = static_cast<int>(pixelBox.getHeight());
  if (srcLeft + width > imageWidth || srcTop + height > imageHeight) {
    return false;
  }
  const unsigned char *base = static_cast<const unsigned char *>(pixelBox.data);
  if (!base) {
    return false;
  }

  try {
    rgbaOut.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4U,
                   0U);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const size_t srcOffset =
            (static_cast<size_t>(srcTop + y) *
                 static_cast<size_t>(pixelBox.rowPitch) +
             static_cast<size_t>(srcLeft + x)) *
            elemBytes;
        const unsigned char *srcPixel = base + srcOffset;
        unsigned char *dstPixel =
            &rgbaOut[(static_cast<size_t>(y) * static_cast<size_t>(width) +
                      static_cast<size_t>(x)) *
                     4U];
        Ogre::PixelUtil::unpackColour(&dstPixel[0], &dstPixel[1], &dstPixel[2],
                                      &dstPixel[3], pixelBox.format, srcPixel);
      }
    }
  } catch (...) {
    rgbaOut.clear();
    return false;
  }

  return !rgbaOut.empty();
}

static bool ComputeOpaqueIconBounds(const Ogre::PixelBox &pixelBox, size_t elemBytes,
                                    int alphaThreshold, int &leftOut, int &topOut,
                                    int &widthOut, int &heightOut) {
  leftOut = 0;
  topOut = 0;
  widthOut = 0;
  heightOut = 0;

  const unsigned char *base = static_cast<const unsigned char *>(pixelBox.data);
  if (!base) {
    return false;
  }

  const int imageWidth = static_cast<int>(pixelBox.getWidth());
  const int imageHeight = static_cast<int>(pixelBox.getHeight());
  if (imageWidth <= 0 || imageHeight <= 0) {
    return false;
  }

  int minX = imageWidth;
  int minY = imageHeight;
  int maxX = -1;
  int maxY = -1;
  try {
    for (int y = 0; y < imageHeight; ++y) {
      for (int x = 0; x < imageWidth; ++x) {
        const size_t srcOffset =
            (static_cast<size_t>(y) * static_cast<size_t>(pixelBox.rowPitch) +
             static_cast<size_t>(x)) *
            elemBytes;
        const unsigned char *srcPixel = base + srcOffset;
        unsigned char r = 0;
        unsigned char g = 0;
        unsigned char b = 0;
        unsigned char a = 0;
        Ogre::PixelUtil::unpackColour(&r, &g, &b, &a, pixelBox.format, srcPixel);
        if (a > alphaThreshold && (r > 6 || g > 6 || b > 6)) {
          if (x < minX) {
            minX = x;
          }
          if (y < minY) {
            minY = y;
          }
          if (x > maxX) {
            maxX = x;
          }
          if (y > maxY) {
            maxY = y;
          }
        }
      }
    }
  } catch (...) {
    return false;
  }

  if (maxX < minX || maxY < minY) {
    return false;
  }

  leftOut = minX;
  topOut = minY;
  widthOut = maxX - minX + 1;
  heightOut = maxY - minY + 1;
  return widthOut > 0 && heightOut > 0;
}

static size_t CountOpaquePixelsRgba(const std::vector<unsigned char> &rgba,
                                    unsigned char alphaThreshold) {
  size_t count = 0;
  for (size_t i = 3; i < rgba.size(); i += 4) {
    if (rgba[i] > alphaThreshold) {
      ++count;
    }
  }
  return count;
}

typedef void (*InventoryIconCreateIconImageExportFn)(Item *, std::string &,
                                                      iVector2 &);

static InventoryIconCreateIconImageExportFn ResolveInventoryIconCreateIconImageExport() {
  static InventoryIconCreateIconImageExportFn fn = nullptr;
  static bool resolved = false;
  if (resolved) {
    return fn;
  }
  resolved = true;

  HMODULE kenshiLib = GetModuleHandleA("KenshiLib.dll");
  if (!kenshiLib) {
    return nullptr;
  }

  FARPROC exported = GetProcAddress(
      kenshiLib,
      "?createIconImage@InventoryIcon@@SAXPEAVItem@@AEAV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEAViVector2@@@Z");
  if (exported) {
    fn = reinterpret_cast<InventoryIconCreateIconImageExportFn>(exported);
  }
  return fn;
}

static bool TryCreateIconImageSehSafe(Item *item, std::string &iconImageNameOut,
                                      iVector2 &iconSizeOut,
                                      std::string *diagReasonOut = nullptr) {
  if (diagReasonOut) {
    diagReasonOut->clear();
  }
  iconImageNameOut.clear();
  iconSizeOut.x = 0;
  iconSizeOut.y = 0;
  if (!item || (uintptr_t)item < 0x1000) {
    if (diagReasonOut) {
      *diagReasonOut = "invalid_item";
    }
    return false;
  }

  InventoryIconCreateIconImageExportFn exportFn =
      ResolveInventoryIconCreateIconImageExport();
  if (exportFn) {
    __try {
      exportFn(item, iconImageNameOut, iconSizeOut);
      return true;
    } __except (ItemImageSehFilter(GetExceptionCode())) {
      if (diagReasonOut) {
        *diagReasonOut = "create_icon_image_export_seh";
      }
      MaybeDisableItemImageSyncAfterSeh();
      return false;
    }
  }

  __try {
    InventoryIcon::createIconImage(item, iconImageNameOut, iconSizeOut);
    return true;
  } __except (ItemImageSehFilter(GetExceptionCode())) {
    if (diagReasonOut) {
      *diagReasonOut = "create_icon_image_seh";
    }
    MaybeDisableItemImageSyncAfterSeh();
    return false;
  }
}

static bool TryCaptureItemIconBmpUnsafe(Item *item, std::string &bmpDataOut,
                                        int &widthOut, int &heightOut,
                                        std::string &imageHashOut,
                                        std::string *diagReasonOut = nullptr) {
  auto fail = [&](const std::string &reason) -> bool {
    if (diagReasonOut) {
      *diagReasonOut = reason;
    }
    return false;
  };

  bmpDataOut.clear();
  imageHashOut.clear();
  widthOut = 0;
  heightOut = 0;
  if (diagReasonOut) {
    diagReasonOut->clear();
  }

  if (!item || (uintptr_t)item < 0x1000) {
    return fail("invalid_item");
  }

  std::string iconImageName = "";
  iVector2 iconSize;
  if (!TryCreateIconImageSehSafe(item, iconImageName, iconSize, diagReasonOut)) {
    if (diagReasonOut && !diagReasonOut->empty()) {
      return false;
    }
    return fail("create_icon_image_failed");
  }
  iconImageName = TrimCopy(iconImageName);
  if (iconImageName.empty()) {
    return fail("icon_name_empty");
  }

  Ogre::Image iconImage;
  std::string resolvedName = "";
  std::string resolvedGroup = "";
  if (!TryLoadItemIconImage(iconImageName, iconImage, resolvedName, resolvedGroup,
                            diagReasonOut)) {
    if (diagReasonOut && !diagReasonOut->empty()) {
      return false;
    }
    return fail("icon_load_failed");
  }

  Ogre::PixelBox pixelBox;
  try {
    pixelBox = iconImage.getPixelBox(0, 0);
  } catch (...) {
    return fail("icon_pixel_box_exception");
  }
  if (!pixelBox.data) {
    return fail("icon_pixel_box_no_data");
  }
  if (!Ogre::PixelUtil::isAccessible(pixelBox.format)) {
    return fail("icon_pixel_format_inaccessible");
  }
  const size_t elemBytes = Ogre::PixelUtil::getNumElemBytes(pixelBox.format);
  if (elemBytes == 0) {
    return fail("icon_elem_bytes_zero");
  }

  const int imageWidth = static_cast<int>(pixelBox.getWidth());
  const int imageHeight = static_cast<int>(pixelBox.getHeight());
  if (imageWidth <= 0 || imageHeight <= 0) {
    return fail("icon_dimensions_invalid");
  }
  if (imageWidth < 8 || imageHeight < 8) {
    return fail("icon_dimensions_too_small");
  }

  bool hasIconSizeHint =
      iconSize.x > 0 && iconSize.y > 0 && iconSize.x <= imageWidth &&
      iconSize.y <= imageHeight;

  int opaqueLeft = 0;
  int opaqueTop = 0;
  int opaqueWidth = 0;
  int opaqueHeight = 0;
  // Avoid expensive per-pixel alpha scans on large source textures to prevent
  // frame hangs on the main thread.
  bool canRunOpaqueScan = imageWidth <= 512 && imageHeight <= 512;
  bool hasOpaqueBounds = false;
  if (canRunOpaqueScan) {
    hasOpaqueBounds = ComputeOpaqueIconBounds(pixelBox, elemBytes, 10, opaqueLeft,
                                              opaqueTop, opaqueWidth,
                                              opaqueHeight);
  }
  if (hasOpaqueBounds) {
    const int kOpaquePad = 1;
    int left = opaqueLeft - kOpaquePad;
    int top = opaqueTop - kOpaquePad;
    int right = opaqueLeft + opaqueWidth - 1 + kOpaquePad;
    int bottom = opaqueTop + opaqueHeight - 1 + kOpaquePad;
    if (left < 0) {
      left = 0;
    }
    if (top < 0) {
      top = 0;
    }
    if (right >= imageWidth) {
      right = imageWidth - 1;
    }
    if (bottom >= imageHeight) {
      bottom = imageHeight - 1;
    }
    if (right >= left && bottom >= top) {
      opaqueLeft = left;
      opaqueTop = top;
      opaqueWidth = right - left + 1;
      opaqueHeight = bottom - top + 1;
    }
  }

  int captureLeft = 0;
  int captureTop = 0;
  int captureWidth = imageWidth;
  int captureHeight = imageHeight;
  if (hasIconSizeHint) {
    captureWidth = iconSize.x;
    captureHeight = iconSize.y;
    captureLeft = (imageWidth - captureWidth) / 2;
    captureTop = (imageHeight - captureHeight) / 2;
  } else if (imageWidth > 512 || imageHeight > 512) {
    // No reliable size hint; take a bounded center crop instead of scanning or
    // processing a full atlas-sized source.
    int bounded = std::min(imageWidth, imageHeight);
    if (bounded > 256) {
      bounded = 256;
    }
    if (bounded < 8) {
      return fail("icon_dimensions_too_small");
    }
    captureWidth = bounded;
    captureHeight = bounded;
    captureLeft = (imageWidth - captureWidth) / 2;
    captureTop = (imageHeight - captureHeight) / 2;
  }
  if (hasOpaqueBounds && opaqueWidth >= 8 && opaqueHeight >= 8) {
    long long opaqueArea =
        static_cast<long long>(opaqueWidth) * static_cast<long long>(opaqueHeight);
    long long fullArea =
        static_cast<long long>(imageWidth) * static_cast<long long>(imageHeight);
    bool opaqueLooksSpecific = fullArea > 0 && opaqueArea * 100LL <= fullArea * 95LL;
    if (!hasIconSizeHint || opaqueLooksSpecific) {
      captureLeft = opaqueLeft;
      captureTop = opaqueTop;
      captureWidth = opaqueWidth;
      captureHeight = opaqueHeight;
    }
  }
  if (captureWidth < 8 || captureHeight < 8) {
    return fail("icon_dimensions_too_small");
  }

  const int kMaxCaptureDimension = 512;
  if (captureWidth > kMaxCaptureDimension || captureHeight > kMaxCaptureDimension) {
    bool reduced = false;
    if (hasIconSizeHint && iconSize.x <= kMaxCaptureDimension &&
        iconSize.y <= kMaxCaptureDimension) {
      captureWidth = iconSize.x;
      captureHeight = iconSize.y;
      captureLeft = (imageWidth - captureWidth) / 2;
      captureTop = (imageHeight - captureHeight) / 2;
      reduced = true;
    }
    if (!reduced && hasOpaqueBounds && opaqueWidth >= 8 && opaqueHeight >= 8 &&
        opaqueWidth <= kMaxCaptureDimension &&
        opaqueHeight <= kMaxCaptureDimension) {
      captureLeft = opaqueLeft;
      captureTop = opaqueTop;
      captureWidth = opaqueWidth;
      captureHeight = opaqueHeight;
      reduced = true;
    }
    if (!reduced) {
      return fail("icon_dimensions_too_large");
    }
  }

  std::vector<unsigned char> rgba;
  if (!ExtractIconRectToRgba(pixelBox, elemBytes, captureLeft, captureTop,
                             captureWidth, captureHeight, rgba)) {
    return fail("icon_readback_exception");
  }

  if (hasIconSizeHint &&
      (captureWidth != imageWidth || captureHeight != imageHeight)) {
    size_t totalPixels =
        static_cast<size_t>(captureWidth) * static_cast<size_t>(captureHeight);
    size_t opaquePixels = CountOpaquePixelsRgba(rgba, 8);
    bool mostlyTransparent =
        totalPixels == 0 || opaquePixels == 0 || (opaquePixels * 200U) < totalPixels;
    if (mostlyTransparent) {
      bool replaced = false;
      if (hasOpaqueBounds && opaqueWidth >= 8 && opaqueHeight >= 8 &&
          (opaqueLeft != captureLeft || opaqueTop != captureTop ||
           opaqueWidth != captureWidth || opaqueHeight != captureHeight) &&
          ExtractIconRectToRgba(pixelBox, elemBytes, opaqueLeft, opaqueTop,
                                opaqueWidth, opaqueHeight, rgba)) {
        captureLeft = opaqueLeft;
        captureTop = opaqueTop;
        captureWidth = opaqueWidth;
        captureHeight = opaqueHeight;
        replaced = true;
      }
      if (!replaced &&
          ExtractIconRectToRgba(pixelBox, elemBytes, 0, 0, imageWidth, imageHeight,
                                rgba)) {
        captureLeft = 0;
        captureTop = 0;
        captureWidth = imageWidth;
        captureHeight = imageHeight;
      }
    }
  }

  if (!EncodeBmp24(rgba, captureWidth, captureHeight, bmpDataOut) ||
      bmpDataOut.empty()) {
    return fail("icon_encode_bmp_failed");
  }

  if (diagReasonOut) {
    *diagReasonOut = "ok:" + TruncateItemImageDiagValue(resolvedName) + "@" +
                     TruncateItemImageDiagValue(resolvedGroup) + ":" +
                     ToString(captureWidth) + "x" + ToString(captureHeight);
  }

  imageHashOut = HexFromU64(HashFnv1a64(
      reinterpret_cast<const unsigned char *>(bmpDataOut.data()),
      bmpDataOut.size()));
  widthOut = captureWidth;
  heightOut = captureHeight;
  return true;
}

static bool TryCaptureItemIconBmp(Item *item, std::string &bmpDataOut,
                                  int &widthOut, int &heightOut,
                                  std::string &imageHashOut,
                                  std::string *diagReasonOut = nullptr) {
  if (g_itemImageSyncDisabledForSession) {
    if (diagReasonOut) {
      *diagReasonOut = "item_image_sync_disabled_for_session";
    }
    return false;
  }

  __try {
    return TryCaptureItemIconBmpUnsafe(item, bmpDataOut, widthOut, heightOut,
                                       imageHashOut, diagReasonOut);
  } __except (ItemImageSehFilter(GetExceptionCode())) {
    if (diagReasonOut) {
      *diagReasonOut = "item_icon_capture_seh";
    }
    MaybeDisableItemImageSyncAfterSeh();
    return false;
  }
}

static void PruneItemImageSyncState() {
  DWORD nowTick = GetTickCount();
  int pruned = 0;
  EnterCriticalSection(&g_stateMutex);
  for (std::map<std::string, ItemImageSyncState>::iterator it =
           g_itemImageSyncStateByItemId.begin();
       it != g_itemImageSyncStateByItemId.end();) {
    DWORD age = nowTick - it->second.lastSeenTick;
    if (it->second.lastSeenTick == 0 || age > kItemImageStateRetentionMs) {
      it = g_itemImageSyncStateByItemId.erase(it);
      ++pruned;
    } else {
      ++it;
    }
  }
  LeaveCriticalSection(&g_stateMutex);
  if (pruned > 0) {
    Log("ITEM_IMAGE_SYNC: pruned stale state entries=" + ToString(pruned));
  }
}

static size_t SyncItemImagesForCharacterUnsafe(Character *npc, bool force,
                                               const std::string &reason) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return 0;
  }
  if (g_itemImageSyncDisabledForSession) {
    return 0;
  }
  if (!ShouldAllowItemImageSyncWork(reason, nullptr)) {
    return 0;
  }
    if (ShouldDeferVisualSyncWhileInventoryVisible(
            npc, "ITEM_IMAGE_SYNC", reason,
            kItemImageInventoryUiHideGraceMs)) {
    return 0;
  }

  DWORD nowTick = GetTickCount();
  EnterCriticalSection(&g_stateMutex);
  g_itemImageLastRunTick = nowTick;
  LeaveCriticalSection(&g_stateMutex);

  std::vector<Item *> rawItems;
  try {
    GetAllCharacterItems(npc, rawItems);
  } catch (...) {
    return 0;
  }
  if (rawItems.empty()) {
    return 0;
  }

  std::map<std::string, Item *> uniqueByIdKey;
  std::map<std::string, std::string> sourceItemIdByKey;
  for (uint32_t i = 0; i < rawItems.size(); ++i) {
    Item *item = rawItems[i];
    if (!item || (uintptr_t)item < 0x1000) {
      continue;
    }
    std::string itemId = "";
    std::string itemName = "";
    try {
      itemName = TrimCopy(item->getName());
      if (item->data && (uintptr_t)item->data > 0x1000) {
        itemId = TrimCopy(item->data->stringID);
      }
    } catch (...) {
      itemId = "";
      itemName = "";
    }
    if (itemId.empty()) {
      itemId = BuildSyntheticItemStringIdFromName(itemName);
    }
    if (itemId.empty()) {
      continue;
    }
    std::string key = NormalizeItemImageStateKey(itemId);
    if (key.empty()) {
      continue;
    }
    if (uniqueByIdKey.find(key) == uniqueByIdKey.end()) {
      uniqueByIdKey[key] = item;
      sourceItemIdByKey[key] = itemId;
    }
  }

  if (uniqueByIdKey.empty()) {
    return 0;
  }

  size_t queued = 0;
  size_t captureFailed = 0;
  size_t considered = 0;
  bool truncatedByBatchOrConsiderLimit = false;
  std::string firstCaptureReason = "";
  std::string entriesJson = "[";
  for (std::map<std::string, Item *>::iterator it = uniqueByIdKey.begin();
       it != uniqueByIdKey.end(); ++it) {
    if (queued >= kItemImageBatchLimit) {
      truncatedByBatchOrConsiderLimit = true;
      break;
    }

    const std::string itemId = sourceItemIdByKey[it->first];
    Item *item = it->second;
    if (!item || (uintptr_t)item < 0x1000 || itemId.empty()) {
      continue;
    }
    std::string attemptSkipReason = "";
    if (!ShouldAttemptItemImageCapture(itemId, nowTick, force,
                                       &attemptSkipReason)) {
      continue;
    }
    if (considered >= kItemImageBatchLimit * kItemImageConsiderMultiplier) {
      truncatedByBatchOrConsiderLimit = true;
      break;
    }
    ++considered;

    std::string bmpData = "";
    std::string imageHash = "";
    std::string captureReason = "";
    int width = 0;
    int height = 0;
    if (!TryCaptureItemIconBmp(item, bmpData, width, height, imageHash,
                               &captureReason)) {
      ++captureFailed;
      if (firstCaptureReason.empty() && !captureReason.empty()) {
        firstCaptureReason = captureReason;
      }
      continue;
    }
    if (bmpData.empty() || imageHash.empty() || width <= 0 || height <= 0) {
      ++captureFailed;
      if (firstCaptureReason.empty()) {
        firstCaptureReason = "invalid_captured_icon";
      }
      continue;
    }

    DWORD sinceLastSent = 0;
    bool changed = false;
    bool firstSync = false;
    if (!ShouldSendItemImageSync(itemId, imageHash, nowTick, force,
                                 sinceLastSent, changed, firstSync)) {
      continue;
    }

    std::string base64Image = Base64EncodeBinary(bmpData);
    if (base64Image.empty()) {
      continue;
    }

    std::string itemName = "";
    try {
      itemName = item->getName();
    } catch (...) {
      itemName = "";
    }

    if (queued > 0) {
      entriesJson += ",";
    }
    entriesJson += "{";
    entriesJson += "\"stringid\":\"" + EscapeJSON(itemId) + "\",";
    entriesJson += "\"name\":\"" + EscapeJSON(itemName) + "\",";
    entriesJson += "\"image_hash\":\"" + EscapeJSON(imageHash) + "\",";
    entriesJson += "\"format\":\"bmp\",";
    entriesJson += "\"width\":" + ToString(width) + ",";
    entriesJson += "\"height\":" + ToString(height) + ",";
    entriesJson += "\"image_base64\":\"" + EscapeJSON(base64Image) + "\"";
    entriesJson += "}";
    ++queued;
  }
  entriesJson += "]";

  if (queued == 0) {
    static DWORD lastNoSendLogTick = 0;
    if (considered > 0 && nowTick - lastNoSendLogTick >= 5000) {
      lastNoSendLogTick = nowTick;
      Log("ITEM_IMAGE_SYNC: no-send considered=" + ToString((int)considered) +
          " capture_failed=" + ToString((int)captureFailed) +
          " reason=" + reason +
          (firstCaptureReason.empty()
               ? std::string("")
               : std::string(" capture_reason=" + firstCaptureReason)));
    }
    return 0;
  }

  int gameTs = 0;
  try {
    GameWorld *world = GetWorldSafe();
    if (world) {
      TimeOfDay tod = world->getTimeStamp_inGameHours();
      gameTs = static_cast<int>(tod.getTotalSeconds());
    }
  } catch (...) {
    gameTs = 0;
  }

  std::string payload = "{";
  payload += "\"source\":\"inventory_live_sync\",";
  payload += "\"sync_reason\":\"" + EscapeJSON(reason) + "\",";
  payload += "\"game_ts\":" + ToString(gameTs) + ",";
  payload += "\"entries\":" + entriesJson;
  payload += "}";
  AsyncPostToStobe(L"/item_image_upload", payload);

  static DWORD lastPruneTick = 0;
  if (nowTick - lastPruneTick >= 60000) {
    lastPruneTick = nowTick;
    PruneItemImageSyncState();
  }

  Log("ITEM_IMAGE_SYNC: sent entries=" + ToString((int)queued) +
      " considered=" + ToString((int)considered) +
      " capture_failed=" + ToString((int)captureFailed) + " reason=" + reason);

  if (truncatedByBatchOrConsiderLimit && queued >= kItemImageBatchLimit) {
    QueueItemImageSyncRequest(npc, "inventory_sync_overflow");
    Log("ITEM_IMAGE_SYNC: queued continuation after batch cap reason=inventory_sync_overflow");
  }

  return queued;
}

static size_t SyncItemImagesForCharacter(Character *npc, bool force,
                                         const std::string &reason) {
  if (g_itemImageSyncDisabledForSession) {
    return 0;
  }

  __try {
    return SyncItemImagesForCharacterUnsafe(npc, force, reason);
  } __except (ItemImageSehFilter(GetExceptionCode())) {
    MaybeDisableItemImageSyncAfterSeh();
    return 0;
  }
}

static Character *ResolveCharacterFromHandSehSafe(const hand &characterHand) {
  Character *npc = nullptr;
  __try {
    npc = characterHand.getCharacter();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    npc = nullptr;
  }
  return npc;
}

static void RunQueuedItemImageSync() {
  if (!g_enableItemImageSync) {
    EnterCriticalSection(&g_stateMutex);
    g_itemImageSyncRequestQueue.clear();
    LeaveCriticalSection(&g_stateMutex);
    return;
  }
  if (g_itemImageSyncDisabledForSession) {
    return;
  }

  PendingItemImageSyncRequest peekReq;
  bool hasPending = false;
  EnterCriticalSection(&g_stateMutex);
  if (!g_itemImageSyncRequestQueue.empty()) {
    peekReq = g_itemImageSyncRequestQueue.front();
    hasPending = true;
  }
  LeaveCriticalSection(&g_stateMutex);
  if (!hasPending) {
    return;
  }

  if (!ShouldRunItemImageSyncNow(peekReq.reason, false, nullptr)) {
    return;
  }
    Character *peekNpc = ResolveCharacterFromHandSehSafe(peekReq.npcHand);
    if (peekNpc && (uintptr_t)peekNpc > 0x1000 &&
        ShouldDeferVisualSyncWhileInventoryVisible(
            peekNpc, "ITEM_IMAGE_SYNC", peekReq.reason,
            kItemImageInventoryUiHideGraceMs)) {
      return;
    }

  PendingItemImageSyncRequest req;
  bool hasRequest = false;
  DWORD nowTick = GetTickCount();
  EnterCriticalSection(&g_stateMutex);
  if (!g_itemImageSyncRequestQueue.empty()) {
    req = g_itemImageSyncRequestQueue.front();
    g_itemImageSyncRequestQueue.pop_front();
    g_itemImageLastRunTick = nowTick;
    hasRequest = true;
  }
  LeaveCriticalSection(&g_stateMutex);

  if (!hasRequest) {
    return;
  }

  Character *npc = ResolveCharacterFromHandSehSafe(req.npcHand);
  if (!npc || (uintptr_t)npc < 0x1000) {
    return;
  }

  std::string reason = TrimCopy(req.reason);
  if (reason.empty()) {
    reason = "queued_inventory_sync";
  }
  SyncItemImagesForCharacter(npc, false, reason);
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

static bool SyncInventoryForCharacterUnsafe(Character *npc, bool force,
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

  if (ShouldQueueItemImageSyncForInventorySync(reason, firstSync, hashChanged)) {
    QueueItemImageSyncRequest(npc, reason);
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

static bool SyncInventoryForCharacter(Character *npc, bool force,
                                      const std::string &reason) {
  __try {
    return SyncInventoryForCharacterUnsafe(npc, force, reason);
  } __except (InventorySyncSehFilter(GetExceptionCode())) {
    return false;
  }
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

static std::string BuildHealingSessionTargetKey(unsigned int targetSerial,
                                                const std::string &targetName) {
  if (targetSerial != 0) {
    return "sid:" + ToString((int)targetSerial);
  }
  std::string normalized = ToLowerAsciiCopy(TrimCopy(targetName));
  if (!normalized.empty()) {
    return "name:" + normalized;
  }
  return "unknown";
}

static std::string BuildHealingSessionActorKey(unsigned int healerSerial,
                                               const std::string &healerName) {
  if (healerSerial != 0) {
    return "sid:" + ToString((int)healerSerial);
  }
  std::string normalized = ToLowerAsciiCopy(TrimCopy(healerName));
  if (!normalized.empty()) {
    return "name:" + normalized;
  }
  return "unknown_actor";
}

static bool ShouldEmitHealingEvent(unsigned int healerSerial,
                                   const std::string &healerName,
                                   unsigned int targetSerial,
                                   const std::string &targetName,
                                   const std::string &itemName,
                                   DWORD nowTick) {
  std::string actorKey = BuildHealingSessionActorKey(healerSerial, healerName);
  std::string targetKey = BuildHealingSessionTargetKey(targetSerial, targetName);
  std::string itemKey = NormalizeInventoryKey(itemName);
  if (itemKey.empty()) {
    itemKey = "medical item";
  }
  std::string burstKey = targetKey + "|" + itemKey;

  HealingEventSessionState &state =
      g_healingEventSessionByActorKey[actorKey];
  bool activeSession =
      state.lastUpdateTick != 0 &&
      (nowTick - state.lastUpdateTick) <= kHealingEventSessionGapMs &&
      state.targetKey == targetKey && state.itemKey == itemKey;
  bool targetBurstActive = false;
  std::map<std::string, DWORD>::const_iterator burstIt =
      g_healingEventBurstByTargetKey.find(burstKey);
  if (burstIt != g_healingEventBurstByTargetKey.end() && burstIt->second != 0) {
    targetBurstActive =
        (nowTick - burstIt->second) <= kHealingEventTargetBurstCooldownMs;
  }

  state.lastUpdateTick = nowTick;
  state.targetKey = targetKey;
  state.itemKey = itemKey;
  if (!activeSession && !targetBurstActive) {
    g_healingEventBurstByTargetKey[burstKey] = nowTick;
  }

  return !activeSession && !targetBurstActive;
}

static void PruneHealingEventSessionState(DWORD nowTick) {
  for (std::map<std::string, HealingEventSessionState>::iterator it =
           g_healingEventSessionByActorKey.begin();
       it != g_healingEventSessionByActorKey.end();) {
    const bool expired =
        (it->second.lastUpdateTick == 0) ||
        ((nowTick - it->second.lastUpdateTick) > kHealingEventSessionRetentionMs);
    if (expired) {
      it = g_healingEventSessionByActorKey.erase(it);
    } else {
      ++it;
    }
  }

  for (std::map<std::string, DWORD>::iterator it =
           g_healingEventBurstByTargetKey.begin();
       it != g_healingEventBurstByTargetKey.end();) {
    const bool expired =
        (it->second == 0) ||
        ((nowTick - it->second) > kHealingEventSessionRetentionMs);
    if (expired) {
      it = g_healingEventBurstByTargetKey.erase(it);
    } else {
      ++it;
    }
  }
}

static bool ShouldEmitPredationEvent(unsigned int eaterSerial,
                                     unsigned int victimSerial,
                                     const std::string &victimName,
                                     DWORD nowTick) {
  unsigned int sessionKeySerial =
      eaterSerial != 0 ? eaterSerial : victimSerial;
  if (sessionKeySerial == 0) {
    return true;
  }

  std::string targetKey =
      BuildHealingSessionTargetKey(victimSerial, victimName);

  PredationEventSessionState &state =
      g_predationEventSessionByEaterSerial[sessionKeySerial];
  bool activeSession =
      state.lastUpdateTick != 0 &&
      (nowTick - state.lastUpdateTick) <= kPredationEventSessionGapMs &&
      state.targetKey == targetKey;

  state.lastUpdateTick = nowTick;
  state.targetKey = targetKey;
  return !activeSession;
}

static void PrunePredationEventSessionState(DWORD nowTick) {
  for (std::map<unsigned int, PredationEventSessionState>::iterator it =
           g_predationEventSessionByEaterSerial.begin();
       it != g_predationEventSessionByEaterSerial.end();) {
    const bool expired =
        (it->second.lastUpdateTick == 0) ||
        ((nowTick - it->second.lastUpdateTick) >
         kPredationEventSessionRetentionMs);
    if (expired) {
      it = g_predationEventSessionByEaterSerial.erase(it);
    } else {
      ++it;
    }
  }
}

static unsigned int ResolveCharacterSerialForEvent(Character *npc);
static std::string ResolveCharacterNameSafe(Character *npc);
static Character *ResolveCharacterFromHandSafe(const hand &h);
static std::string ResolveConstructionSubjectName(const hand &subjectHand);
static bool IsAnyPredationTask(TaskType taskType);
static bool IsCorpsePredationTask(TaskType taskType);

static void EmitPredationEvent(Character *eater, Character *victim,
                               const std::string &victimNameHint,
                               DWORD nowTick, bool corpseContext = false) {
  if (!eater || (uintptr_t)eater < 0x1000) {
    return;
  }

  unsigned int eaterSerial = ResolveCharacterSerialForEvent(eater);
  unsigned int victimSerial = ResolveCharacterSerialForEvent(victim);
  if (eaterSerial != 0 && victimSerial != 0 && eaterSerial == victimSerial) {
    return;
  }

  bool victimAlive = true;
  bool victimDead = false;
  if (victim && (uintptr_t)victim >= 0x1000) {
    try {
      victimAlive = !victim->isDead();
      victimDead = !victimAlive;
    } catch (...) {
      victimAlive = false;
      victimDead = false;
    }
  }
  if (!victimAlive && !corpseContext) {
    return;
  }

  std::string victimName =
      victim ? ResolveCharacterNameSafe(victim) : TrimCopy(victimNameHint);
  if (victimName.empty()) {
    victimName = "someone";
  }

  if (!ShouldEmitPredationEvent(eaterSerial, victimSerial, victimName, nowTick)) {
    return;
  }

  std::string eaterName = ResolveCharacterNameSafe(eater);
  if (eaterName.empty()) {
    eaterName = "Unknown";
  }

  std::string message = "";
  if (victimDead) {
    message = "is eating " + victimName + "'s corpse.";
  } else if (corpseContext) {
    message = "is eating " + victimName + " while they are down.";
  } else {
    message = "is eating " + victimName + " alive!";
  }
  // Keep stream text clean (no talking-to suffix), but still include victim
  // in people-present via target serial.
  LogGameEvent("predation", eaterName, SafeFaction(eater), "", "None", message,
               eaterSerial, victimSerial);
}

static void EmitPredationEventFromTask(Character *eater, const hand &subjectHand,
                                       DWORD nowTick, TaskType taskType) {
  Character *victim = ResolveCharacterFromHandSafe(subjectHand);
  std::string victimNameHint = ResolveConstructionSubjectName(subjectHand);
  EmitPredationEvent(eater, victim, victimNameHint, nowTick,
                     IsCorpsePredationTask(taskType));
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

static Character *ResolveLikelyTraderForActor(GameWorld *world, Character *actor) {
  auto isValidTrader = [&](Character *candidate) -> bool {
    if (!candidate || (uintptr_t)candidate < 0x1000 || candidate == actor) {
      return false;
    }
    bool trader = false;
    try {
      trader = candidate->isATrader();
    } catch (...) {
      trader = false;
    }
    return trader;
  };

  Character *talkTarget = ResolveCharacterFromHandSafe(g_talkTargetHand);
  if (isValidTrader(talkTarget)) {
    return talkTarget;
  }
  Character *selectionTarget = ResolveCharacterFromHandSafe(g_lastSelectionHand);
  if (isValidTrader(selectionTarget)) {
    return selectionTarget;
  }

  if (!world || !actor || (uintptr_t)world < 0x1000 || (uintptr_t)actor < 0x1000) {
    return nullptr;
  }

  Ogre::Vector3 actorPos = Ogre::Vector3::ZERO;
  bool actorPosValid = false;
  try {
    actorPos = actor->getPosition();
    actorPosValid = true;
  } catch (...) {
    actorPosValid = false;
  }
  if (!actorPosValid) {
    return nullptr;
  }

  lektor<RootObject *> nearby;
  world->getCharactersWithinSphere(nearby, actorPos, 30.0f, 0.0f, 0.0f, 16, 0,
                                   actor);

  Character *best = nullptr;
  float bestDistance = 1e9f;
  for (uint32_t i = 0; i < nearby.size(); ++i) {
    Character *candidate = (Character *)nearby.stuff[i];
    if (!isValidTrader(candidate)) {
      continue;
    }
    float distance = 9999.0f;
    try {
      distance = candidate->getPosition().distance(actorPos);
    } catch (...) {
      distance = 9999.0f;
    }
    if (distance < bestDistance) {
      best = candidate;
      bestDistance = distance;
    }
  }

  return best;
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

static bool IsAliveConsciousCharacterForTargeting(Character *npc) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return false;
  }
  try {
    if (npc->isDead() || npc->isUnconcious()) {
      return false;
    }
  } catch (...) {
    return false;
  }
  return true;
}

static Character *ResolveFirstAliveConsciousPlayerCharacter(GameWorld *world) {
  if (!world || !world->player) {
    return nullptr;
  }
  for (uint32_t i = 0; i < world->player->playerCharacters.size(); ++i) {
    Character *candidate = world->player->playerCharacters[i];
    if (IsAliveConsciousCharacterForTargeting(candidate)) {
      return candidate;
    }
  }
  return nullptr;
}

static Character *ResolveNearestPlayerSpeakerForTarget(GameWorld *world,
                                                       Character *target) {
  if (!world || !world->player || world->player->playerCharacters.size() == 0) {
    return nullptr;
  }
  Character *fallback = ResolveFirstAliveConsciousPlayerCharacter(world);
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
    if (!IsAliveConsciousCharacterForTargeting(candidate)) {
      continue;
    }
    // If the target is a squadmate, pick another squadmate as the speaker.
    if (candidate == target) {
      continue;
    }
    float dist = candidate->getPosition().distance(target->getPosition());
    if (!best || dist < bestDist) {
      best = candidate;
      bestDist = dist;
    }
  }
  if (best) {
    return best;
  }
  if (fallback && (uintptr_t)fallback >= 0x1000 && fallback != target) {
    return fallback;
  }

  for (uint32_t i = 0; i < world->player->playerCharacters.size(); ++i) {
    Character *candidate = world->player->playerCharacters[i];
    if (!IsAliveConsciousCharacterForTargeting(candidate) || candidate == target) {
      continue;
    }
    return candidate;
  }
  return fallback;
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
    if (!IsAliveConsciousCharacterForTargeting(candidate) || candidate == selected) {
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

static bool IsIndoorHandleValidForTargeting(const hand &indoorsHandle) {
  return indoorsHandle.isValid() && !indoorsHandle.isNull();
}

static bool TryGetTargetSpatialState(Character *character, bool &hasBuilding,
                                     unsigned int &buildingSerial,
                                     int &floorValue) {
  hasBuilding = false;
  buildingSerial = 0;
  floorValue = 0;
  if (!character || (uintptr_t)character <= 0x1000) {
    return false;
  }
  try {
    const hand &indoorsHandle = character->isIndoors();
    hasBuilding = IsIndoorHandleValidForTargeting(indoorsHandle);
    buildingSerial = hasBuilding ? indoorsHandle.serial : 0;
    floorValue = character->getFloor();
    return true;
  } catch (...) {
    return false;
  }
}

static bool IsTargetAreaCompatibleForSelection(Character *anchor,
                                               Character *candidate) {
  if (!anchor || !candidate || (uintptr_t)anchor <= 0x1000 ||
      (uintptr_t)candidate <= 0x1000) {
    return false;
  }

  bool anchorHasBuilding = false;
  bool candidateHasBuilding = false;
  unsigned int anchorBuildingSerial = 0;
  unsigned int candidateBuildingSerial = 0;
  int anchorFloor = 0;
  int candidateFloor = 0;
  if (!TryGetTargetSpatialState(anchor, anchorHasBuilding, anchorBuildingSerial,
                                anchorFloor)) {
    return false;
  }
  if (!TryGetTargetSpatialState(candidate, candidateHasBuilding,
                                candidateBuildingSerial, candidateFloor)) {
    return false;
  }

  if (anchorHasBuilding) {
    if (!candidateHasBuilding) {
      return false;
    }
    if (anchorBuildingSerial != 0 && candidateBuildingSerial != 0) {
      if (anchorBuildingSerial != candidateBuildingSerial) {
        return false;
      }
    } else {
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

static Character *ResolveNearestNpcTargetForSelection(GameWorld *world,
                                                      Character *selected) {
  if (!world || !selected || (uintptr_t)selected < 0x1000) {
    return nullptr;
  }

  Character *best = nullptr;
  float bestDist = 1e30f;
  try {
    float searchRadius = g_proximityRadius;
    if (searchRadius < 10.0f) {
      searchRadius = 10.0f;
    }
    Ogre::Vector3 selectedPos = selected->getPosition();
    lektor<RootObject *> nearby;
    world->getCharactersWithinSphere(nearby, selectedPos, searchRadius, 0.0f,
                                     0.0f, 0x10, 0, selected);

    for (uint32_t i = 0; i < nearby.size(); ++i) {
      Character *candidate = (Character *)nearby.stuff[i];
      if (!IsAliveConsciousCharacterForTargeting(candidate) ||
          candidate == selected) {
        continue;
      }
      if (!IsTargetAreaCompatibleForSelection(selected, candidate)) {
        continue;
      }
      float dist = candidate->getPosition().distance(selectedPos);
      if (!best || dist < bestDist) {
        best = candidate;
        bestDist = dist;
      }
    }
  } catch (...) {
    return nullptr;
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

static Character *ResolveSelectedPlayerSpeakerForCurrentTalk(GameWorld *world) {
  if (!world || !world->player) {
    return nullptr;
  }

  hand selectedHand;
  try {
    selectedHand = world->player->selectedCharacter;
  } catch (...) {
    return nullptr;
  }
  if (!selectedHand.isValid()) {
    return nullptr;
  }

  Character *selected = ResolveCharacterFromHandSafe(selectedHand);
  if (!selected || (uintptr_t)selected < 0x1000) {
    return nullptr;
  }

  bool isPlayerCharacter = false;
  try {
    isPlayerCharacter = selected->isPlayerCharacter();
  } catch (...) {
    isPlayerCharacter = false;
  }
  if (!isPlayerCharacter || !IsAliveConsciousCharacterForTargeting(selected)) {
    return nullptr;
  }

  return selected;
}

static Character *ResolvePreferredPlayerSpeakerForCurrentTalk(GameWorld *world) {
  Character *selectedSpeaker = ResolveSelectedPlayerSpeakerForCurrentTalk(world);
  if (selectedSpeaker && (uintptr_t)selectedSpeaker >= 0x1000) {
    return selectedSpeaker;
  }
  return ResolvePlayerSpeakerForCurrentTalk(world);
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

static unsigned int ResolveRootObjectSerialForEvent(RootObjectBase *obj) {
  if (!obj || (uintptr_t)obj < 0x1000) {
    return 0;
  }
  unsigned int serial = 0;
  try {
    serial = obj->getHandle().serial;
  } catch (...) {
    serial = 0;
  }
  return serial;
}

static std::string ResolveRootObjectNameSafe(RootObjectBase *obj) {
  if (!obj || (uintptr_t)obj < 0x1000) {
    return "Unknown";
  }
  try {
    std::string name = obj->getName();
    if (!name.empty()) {
      return name;
    }
  } catch (...) {
  }
  return "Unknown";
}

static bool TryResolveRootObjectMoneySafe(RootObject *obj, int &moneyOut) {
  moneyOut = 0;
  if (!obj || (uintptr_t)obj < 0x1000) {
    return false;
  }

  int resolved = 0;
  bool hasValue = false;
  try {
    resolved = obj->getMoney();
    hasValue = true;
  } catch (...) {
    hasValue = false;
  }

  try {
    Ownerships *ownerships = obj->getOwnerships();
    if (ownerships && (uintptr_t)ownerships > 0x1000) {
      int ownershipMoney = ownerships->getMoney();
      if (!hasValue || ownershipMoney > resolved) {
        resolved = ownershipMoney;
        hasValue = true;
      }
    }
  } catch (...) {
  }

  if (!hasValue) {
    return false;
  }
  if (resolved < 0) {
    resolved = 0;
  }
  moneyOut = resolved;
  return true;
}

static bool TryResolveCharacterMoneySafe(Character *npc, int &moneyOut) {
  moneyOut = 0;
  if (!npc || (uintptr_t)npc < 0x1000) {
    return false;
  }

  int resolved = 0;
  bool hasValue = false;
  try {
    resolved = npc->getMoney();
    hasValue = true;
  } catch (...) {
    hasValue = false;
  }

  try {
    Ownerships *ownerships = npc->getOwnerships();
    if (ownerships && (uintptr_t)ownerships > 0x1000) {
      int ownershipMoney = ownerships->getMoney();
      if (!hasValue || ownershipMoney > resolved) {
        resolved = ownershipMoney;
        hasValue = true;
      }
    }
  } catch (...) {
  }

  if (!hasValue) {
    return false;
  }
  if (resolved < 0) {
    resolved = 0;
  }
  moneyOut = resolved;
  return true;
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

static Character *ResolveCharacterFromHandSafe(const hand &h);
static std::string ResolveConstructionSubjectName(const hand &subjectHand);
static unsigned int ResolveCharacterSerialForEvent(Character *npc);
static std::string ResolveCharacterNameSafe(Character *npc);
static TaskType ResolveCurrentNpcTaskSafe(Character *npc, hand &subjectOut);

static bool IsPredationTaskAgainstVictim(Character *attacker,
                                         Character *victim) {
  if (!attacker || !victim || (uintptr_t)attacker < 0x1000 ||
      (uintptr_t)victim < 0x1000 || attacker == victim) {
    return false;
  }
  hand subject;
  TaskType task = ResolveCurrentNpcTaskSafe(attacker, subject);
  if (!IsAnyPredationTask(task)) {
    return false;
  }

  unsigned int victimSerial = ResolveCharacterSerialForEvent(victim);
  if (victimSerial != 0 && subject.isValid() && !subject.isNull() &&
      subject.serial == victimSerial) {
    return true;
  }

  Character *subjectCharacter = ResolveCharacterFromHandSafe(subject);
  return subjectCharacter && subjectCharacter == victim;
}

static Character *ResolvePredationAttackerForVictim(Character *victim) {
  if (!victim || (uintptr_t)victim < 0x1000) {
    return nullptr;
  }
  unsigned int victimSerial = ResolveCharacterSerialForEvent(victim);
  GameWorld *world = GetWorldSafe();
  if (!world || (uintptr_t)world < 0x1000) {
    return nullptr;
  }
  try {
    const auto &chars = world->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it) {
      Character *candidate = *it;
      if (!candidate || (uintptr_t)candidate < 0x1000 || candidate == victim) {
        continue;
      }
      hand subject;
      TaskType task = ResolveCurrentNpcTaskSafe(candidate, subject);
      if (!IsAnyPredationTask(task)) {
        continue;
      }
      if (victimSerial != 0 && subject.isValid() && !subject.isNull() &&
          subject.serial == victimSerial) {
        return candidate;
      }
      Character *subjectCharacter = ResolveCharacterFromHandSafe(subject);
      if (subjectCharacter && subjectCharacter == victim) {
        return candidate;
      }
    }
  } catch (...) {
  }
  return nullptr;
}

static bool IsAnyPredationTask(TaskType taskType) {
  return taskType == EAT_TARGET_ALIVE || taskType == EAT_A_RANDOM_DEAD_BODY ||
         taskType == EAT_A_RANDOM_KO_BODY;
}

static bool IsCorpsePredationTask(TaskType taskType) {
  return taskType == EAT_A_RANDOM_DEAD_BODY || taskType == EAT_A_RANDOM_KO_BODY;
}

static CombatAttribution ResolveCombatAttribution(Character *target) {
  CombatAttribution out;
  if (!target || (uintptr_t)target < 0x1000) {
    return out;
  }

  Character *attacker = nullptr;
  bool predationAttribution = false;
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
    attacker = ResolvePredationAttackerForVictim(target);
    if (attacker) {
      predationAttribution = true;
    }
  }

  if (!attacker) {
    return out;
  }

  if (!predationAttribution && IsPredationTaskAgainstVictim(attacker, target)) {
    predationAttribution = true;
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
  out.weaponName =
      predationAttribution ? "Teeth" : ResolvePrimaryWeaponName(attacker);
  if (!predationAttribution &&
      (out.weaponName.empty() || out.weaponName == "Unknown") &&
      CharacterHasHacksaw(attacker)) {
    out.weaponName = "Hacksaw";
  }
  if (out.weaponName.empty() || out.weaponName == "Unknown") {
    out.weaponName = predationAttribution ? "Teeth" : "Unarmed";
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

static bool ResolveNpcHungerMetrics(Character *npc, float &hungerOut,
                                    float &fedOut, float &satietyOut) {
  hungerOut = 0.0f;
  fedOut = 0.0f;
  satietyOut = 0.0f;
  if (!npc || (uintptr_t)npc < 0x1000) {
    return false;
  }

  MedicalSystem *med = nullptr;
  try {
    med = npc->getMedical();
  } catch (...) {
    med = nullptr;
  }
  if (!med || (uintptr_t)med < 0x1000) {
    return false;
  }

  try {
    hungerOut = med->hunger;
    fedOut = med->fed;
  } catch (...) {
    hungerOut = 0.0f;
    fedOut = 0.0f;
    satietyOut = 0.0f;
    return false;
  }

  if (hungerOut < 0.0f) {
    hungerOut = 0.0f;
  }
  if (fedOut < 0.0f) {
    fedOut = 0.0f;
  }
  satietyOut = (300.0f - hungerOut) + fedOut;
  if (satietyOut < 0.0f) {
    satietyOut = 0.0f;
  }
  return true;
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
    bool isFood = false;
    try {
      itemName = TrimCopy(item->getName());
      count = item->quantity;
      isStolen = item->isStolen(true);
      GameData *itemData = item->getGameData();
      if (itemData && (uintptr_t)itemData > 0x1000) {
        isFood = Item::isFood(itemData);
      }
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
    if (isFood) {
      out.foodByKey[key] += count;
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

enum ConstructionActionEventType {
  CONSTRUCTION_ACTION_NONE = 0,
  CONSTRUCTION_ACTION_BUILD = 1,
  CONSTRUCTION_ACTION_DISMANTLE = 2,
};

static TaskType ResolveCurrentNpcTaskSafe(Character *npc, hand &subjectOut) {
  subjectOut = hand();
  if (!npc || (uintptr_t)npc < 0x1000) {
    return NULL_TASK;
  }

  try {
    CharBody *body = npc->getBody();
    if (!body || (uintptr_t)body < 0x1000) {
      return NULL_TASK;
    }

    Tasker *action = nullptr;
    try {
      action = body->currentAction;
    } catch (...) {
      action = nullptr;
    }
    if (!action || (uintptr_t)action < 0x1000) {
      return NULL_TASK;
    }

    try {
      hand subject = action->subject;
      if (subject.isValid() && !subject.isNull()) {
        subjectOut = subject;
      } else if (body->target.isValid() && !body->target.isNull()) {
        subjectOut = body->target;
      }
    } catch (...) {
      subjectOut = hand();
    }

    TaskType taskKey = NULL_TASK;
    try {
      if (action->taskData && (uintptr_t)action->taskData > 0x1000) {
        taskKey = action->taskData->key;
      }
    } catch (...) {
      taskKey = NULL_TASK;
    }
    return taskKey;
  } catch (...) {
    return NULL_TASK;
  }
}

static ConstructionActionEventType ResolveConstructionActionType(TaskType taskType) {
  switch (taskType) {
  case BUILD:
  case ADD_MATERIALS_TO_BUILDING:
  case FIND_SOME_BUILDING_MATERIALS:
  case JOB_BUILDER:
    return CONSTRUCTION_ACTION_BUILD;
  case DISMANTLE:
    return CONSTRUCTION_ACTION_DISMANTLE;
  default:
    return CONSTRUCTION_ACTION_NONE;
  }
}

static std::string ResolveConstructionSubjectName(const hand &subjectHand) {
  if (!subjectHand.isValid() || subjectHand.isNull()) {
    return "";
  }

  try {
    RootObject *subject = subjectHand.getRootObject();
    if (subject && (uintptr_t)subject > 0x1000) {
      std::string name = TrimCopy(subject->getName());
      if (!name.empty()) {
        return name;
      }
    }
  } catch (...) {
  }

  try {
    Character *subjectCharacter = subjectHand.getCharacter();
    if (subjectCharacter && (uintptr_t)subjectCharacter > 0x1000) {
      std::string name = TrimCopy(subjectCharacter->getName());
      if (!name.empty()) {
        return name;
      }
    }
  } catch (...) {
  }

  return "";
}

static std::string DescribeConstructionSubject(const std::string &subjectName) {
  std::string name = TrimCopy(subjectName);
  if (name.empty()) {
    return "an object";
  }
  return name;
}

static void EmitBuildEvent(Character *npc, const std::string &subjectName) {
  std::string actorName = ResolveCharacterNameSafe(npc);
  std::string actorFaction = SafeFaction(npc);
  std::string message =
      "started building " + DescribeConstructionSubject(subjectName);
  LogGameEvent("build", actorName, actorFaction, "None", "None", message,
               ResolveCharacterSerialForEvent(npc), 0);
}

static void EmitDismantleEvent(Character *npc, const std::string &subjectName) {
  std::string actorName = ResolveCharacterNameSafe(npc);
  std::string actorFaction = SafeFaction(npc);
  std::string message =
      "started dismantling " + DescribeConstructionSubject(subjectName);
  LogGameEvent("dismantle", actorName, actorFaction, "None", "None", message,
               ResolveCharacterSerialForEvent(npc), 0);
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

static std::string EnsureLeadingArticle(const std::string &rawValue) {
  std::string value = TrimCopy(rawValue);
  if (value.empty()) {
    return "an unknown weapon";
  }
  std::string lower = ToLowerAsciiCopy(value);
  if (lower.rfind("a ", 0) == 0 || lower.rfind("an ", 0) == 0 ||
      lower.rfind("the ", 0) == 0) {
    return value;
  }
  char first = (char)std::tolower((unsigned char)value[0]);
  bool vowel =
      first == 'a' || first == 'e' || first == 'i' || first == 'o' || first == 'u';
  return std::string(vowel ? "an " : "a ") + value;
}

static void EmitKnockoutEvent(Character *victim) {
  CombatAttribution attribution = ResolveCombatAttribution(victim);
  std::string victimName = ResolveCharacterNameSafe(victim);
  std::string victimFaction = SafeFaction(victim);
  std::string attackerName = TrimCopy(attribution.actorName);
  std::string weaponName = TrimCopy(attribution.weaponName);
  std::string weaponLower = ToLowerAsciiCopy(weaponName);
  bool unknownWeapon =
      weaponName.empty() || weaponLower == "unknown" ||
      weaponLower == "unknown weapon" || weaponLower == "an unknown weapon" ||
      weaponLower == "none";
  bool knownAttacker =
      !attackerName.empty() && ToLowerAsciiCopy(attackerName) != "unknown" &&
      ToLowerAsciiCopy(attackerName) != ToLowerAsciiCopy(victimName);

  std::string message = "was Knocked Out.";
  if (!unknownWeapon) {
    message = "Knocked out by " + EnsureLeadingArticle(weaponName);
    if (knownAttacker) {
      message += " from " + attackerName;
    }
  }
  LogGameEvent("knockout", victimName, victimFaction, "None", "None", message,
               ResolveCharacterSerialForEvent(victim), 0);
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
  if (enslavedNow && owner) {
    std::string factionLabel = TrimCopy(ownerFaction);
    if (factionLabel.empty() || ToLowerAsciiCopy(factionLabel) == "none") {
      factionLabel = "Unknown Faction";
    }
    message += " by " + ownerName + " [" + factionLabel + "]";
  }
  LogGameEvent("slavery", victimName, victimFaction, "None", "None", message,
               ResolveCharacterSerialForEvent(victim),
               ResolveCharacterSerialForEvent(owner));
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

static std::string ResolveInventoryDisplayNameForKey(
    const std::string &key, const InventoryEventSnapshot &preferredSnapshot,
    const InventoryEventSnapshot &fallbackSnapshot) {
  std::map<std::string, std::string>::const_iterator preferredIt =
      preferredSnapshot.displayNameByKey.find(key);
  if (preferredIt != preferredSnapshot.displayNameByKey.end() &&
      !preferredIt->second.empty()) {
    return preferredIt->second;
  }
  std::map<std::string, std::string>::const_iterator fallbackIt =
      fallbackSnapshot.displayNameByKey.find(key);
  if (fallbackIt != fallbackSnapshot.displayNameByKey.end() &&
      !fallbackIt->second.empty()) {
    return fallbackIt->second;
  }
  return "Unknown Item";
}

struct InventoryDeltaDisplayEntry {
  std::string name;
  int count;

  InventoryDeltaDisplayEntry() : name(""), count(0) {}
  InventoryDeltaDisplayEntry(const std::string &entryName, int entryCount)
      : name(entryName), count(entryCount) {}
};

static std::string BuildInventoryDeltaItemsText(
    const std::map<std::string, int> &deltaByKey,
    const InventoryEventSnapshot &preferredSnapshot,
    const InventoryEventSnapshot &fallbackSnapshot) {
  std::vector<InventoryDeltaDisplayEntry> items;
  for (std::map<std::string, int>::const_iterator it = deltaByKey.begin();
       it != deltaByKey.end(); ++it) {
    if (it->second <= 0) {
      continue;
    }
    std::string itemName =
        ResolveInventoryDisplayNameForKey(it->first, preferredSnapshot,
                                          fallbackSnapshot);
    items.push_back(InventoryDeltaDisplayEntry(itemName, it->second));
  }
  if (items.empty()) {
    return "";
  }

  std::stable_sort(
      items.begin(), items.end(),
      [](const InventoryDeltaDisplayEntry &left,
         const InventoryDeltaDisplayEntry &right) -> bool {
        if (left.count != right.count) {
          return left.count > right.count;
        }
        return left.name < right.name;
      });

  std::string itemsText = "";
  for (size_t i = 0; i < items.size(); ++i) {
    if (items[i].count <= 0) {
      continue;
    }
    if (!itemsText.empty()) {
      itemsText += ", ";
    }
    std::string itemName = items[i].name.empty() ? "Unknown Item" : items[i].name;
    itemsText += ToString(items[i].count) + "x " + itemName;
  }
  return itemsText;
}

static void SubtractInventoryDeltaByKey(
    std::map<std::string, int> &deltaByKey,
    const std::map<std::string, int> &subtractByKey) {
  if (deltaByKey.empty() || subtractByKey.empty()) {
    return;
  }
  for (std::map<std::string, int>::const_iterator it = subtractByKey.begin();
       it != subtractByKey.end(); ++it) {
    if (it->second <= 0) {
      continue;
    }
    std::map<std::string, int>::iterator deltaIt = deltaByKey.find(it->first);
    if (deltaIt == deltaByKey.end()) {
      continue;
    }
    deltaIt->second -= it->second;
    if (deltaIt->second <= 0) {
      deltaByKey.erase(deltaIt);
    }
  }
}

static void RegisterPendingMedicalItemUse(unsigned int actorSerial,
                                          const std::string &itemName,
                                          int quantity) {
  if (actorSerial == 0 || quantity <= 0) {
    return;
  }
  std::string key = NormalizeInventoryKey(itemName);
  if (key.empty()) {
    return;
  }

  PendingMedicalItemUseState &state =
      g_pendingMedicalItemUseBySerial[actorSerial];
  state.pendingByKey[key] += quantity;
  state.lastUpdateTick = GetTickCount();
}

static std::map<std::string, int> ConsumePendingMedicalItemLossByKey(
    unsigned int actorSerial, const std::map<std::string, int> &lossByKey,
    DWORD nowTick) {
  std::map<std::string, int> consumedByKey;
  if (actorSerial == 0 || lossByKey.empty()) {
    return consumedByKey;
  }

  std::map<unsigned int, PendingMedicalItemUseState>::iterator stateIt =
      g_pendingMedicalItemUseBySerial.find(actorSerial);
  if (stateIt == g_pendingMedicalItemUseBySerial.end()) {
    return consumedByKey;
  }

  PendingMedicalItemUseState &state = stateIt->second;
  if (state.lastUpdateTick == 0 ||
      (nowTick - state.lastUpdateTick) > kPendingMedicalItemUseRetentionMs) {
    g_pendingMedicalItemUseBySerial.erase(stateIt);
    return consumedByKey;
  }

  for (std::map<std::string, int>::const_iterator it = lossByKey.begin();
       it != lossByKey.end(); ++it) {
    const std::string &key = it->first;
    int lossQty = it->second;
    if (key.empty() || lossQty <= 0) {
      continue;
    }
    std::map<std::string, int>::iterator pendingIt = state.pendingByKey.find(key);
    if (pendingIt == state.pendingByKey.end() || pendingIt->second <= 0) {
      continue;
    }
    int matchedQty = lossQty < pendingIt->second ? lossQty : pendingIt->second;
    if (matchedQty <= 0) {
      continue;
    }
    consumedByKey[key] += matchedQty;
    pendingIt->second -= matchedQty;
    if (pendingIt->second <= 0) {
      state.pendingByKey.erase(pendingIt);
    }
  }

  if (state.pendingByKey.empty()) {
    g_pendingMedicalItemUseBySerial.erase(stateIt);
  } else {
    state.lastUpdateTick = nowTick;
  }
  return consumedByKey;
}

static void PrunePendingMedicalItemUseState(DWORD nowTick) {
  for (std::map<unsigned int, PendingMedicalItemUseState>::iterator it =
           g_pendingMedicalItemUseBySerial.begin();
       it != g_pendingMedicalItemUseBySerial.end();) {
    const bool expired =
        (it->second.lastUpdateTick == 0) ||
        ((nowTick - it->second.lastUpdateTick) > kPendingMedicalItemUseRetentionMs);
    if (expired || it->second.pendingByKey.empty()) {
      it = g_pendingMedicalItemUseBySerial.erase(it);
    } else {
      ++it;
    }
  }
}

static std::map<std::string, int> DetectFoodConsumptionLossByKey(
    const std::map<std::string, int> &lossByKey,
    const InventoryEventSnapshot &beforeSnapshot, bool hadHungerBefore,
    float hungerBefore, float fedBefore, float satietyBefore, bool hasHungerAfter,
    float hungerAfter, float fedAfter, float satietyAfter) {
  std::map<std::string, int> consumedByKey;
  if (lossByKey.empty() || !hadHungerBefore || !hasHungerAfter) {
    return consumedByKey;
  }

  const float hungerDrop = hungerBefore - hungerAfter;
  const float fedRise = fedAfter - fedBefore;
  const float satietyRise = satietyAfter - satietyBefore;
  const bool hasConsumptionSignal =
      hungerDrop >= 0.10f || fedRise >= 0.10f || satietyRise >= 0.50f;
  if (!hasConsumptionSignal) {
    return consumedByKey;
  }

  for (std::map<std::string, int>::const_iterator it = lossByKey.begin();
       it != lossByKey.end(); ++it) {
    const std::string &key = it->first;
    int lossQty = it->second;
    if (key.empty() || lossQty <= 0) {
      continue;
    }
    std::map<std::string, int>::const_iterator foodIt =
        beforeSnapshot.foodByKey.find(key);
    if (foodIt == beforeSnapshot.foodByKey.end() || foodIt->second <= 0) {
      continue;
    }
    int consumedQty = lossQty < foodIt->second ? lossQty : foodIt->second;
    if (consumedQty > 0) {
      consumedByKey[key] = consumedQty;
    }
  }
  return consumedByKey;
}

static void EmitEatEvent(Character *npc,
                         const InventoryEventSnapshot &beforeSnapshot,
                         const InventoryEventSnapshot &afterSnapshot,
                         const std::map<std::string, int> &consumedByKey) {
  if (consumedByKey.empty()) {
    return;
  }

  std::string itemsText =
      BuildInventoryDeltaItemsText(consumedByKey, beforeSnapshot, afterSnapshot);
  if (itemsText.empty()) {
    return;
  }

  std::string actorName = ResolveCharacterNameSafe(npc);
  std::string actorFaction = SafeFaction(npc);
  std::string message = "ate " + itemsText;
  LogGameEvent("eat", actorName, actorFaction, "None", "None", message,
               ResolveCharacterSerialForEvent(npc), 0);
}

static void ComputeInventoryDeltaByKey(
    const InventoryEventSnapshot &beforeSnapshot,
    const InventoryEventSnapshot &afterSnapshot,
    std::map<std::string, int> &gainByKeyOut,
    std::map<std::string, int> &lossByKeyOut) {
  gainByKeyOut.clear();
  lossByKeyOut.clear();

  for (std::map<std::string, int>::const_iterator it =
           afterSnapshot.countsByKey.begin();
       it != afterSnapshot.countsByKey.end(); ++it) {
    const std::string &key = it->first;
    int afterCount = it->second;
    int beforeCount = 0;
    std::map<std::string, int>::const_iterator beforeIt =
        beforeSnapshot.countsByKey.find(key);
    if (beforeIt != beforeSnapshot.countsByKey.end()) {
      beforeCount = beforeIt->second;
    }
    if (afterCount > beforeCount) {
      gainByKeyOut[key] = afterCount - beforeCount;
    }
  }

  for (std::map<std::string, int>::const_iterator it =
           beforeSnapshot.countsByKey.begin();
       it != beforeSnapshot.countsByKey.end(); ++it) {
    const std::string &key = it->first;
    int beforeCount = it->second;
    int afterCount = 0;
    std::map<std::string, int>::const_iterator afterIt =
        afterSnapshot.countsByKey.find(key);
    if (afterIt != afterSnapshot.countsByKey.end()) {
      afterCount = afterIt->second;
    }
    if (beforeCount > afterCount) {
      lossByKeyOut[key] = beforeCount - afterCount;
    }
  }
}

static void EmitPickupEvent(
    Character *npc, const NpcWorldEventState &state,
    const InventoryEventSnapshot &currentSnapshot,
    const std::map<std::string, int> *matchedTransferGainByKey,
    bool hasMoneyAfter, int moneyAfter) {
  struct PickupDeltaEntry {
    std::string name;
    int count;

    PickupDeltaEntry() : name(""), count(0) {}
    PickupDeltaEntry(const std::string &entryName, int entryCount)
        : name(entryName), count(entryCount) {}
  };

  int addedCount = 0;
  int addedStolenCount = 0;
  std::vector<PickupDeltaEntry> addedItems;

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
    if (matchedTransferGainByKey) {
      std::map<std::string, int>::const_iterator matchedIt =
          matchedTransferGainByKey->find(key);
      if (matchedIt != matchedTransferGainByKey->end()) {
        int matchedQty = matchedIt->second;
        if (matchedQty > 0) {
          int consumed = matchedQty > delta ? delta : matchedQty;
          delta -= consumed;
        }
      }
    }
    if (delta <= 0) {
      continue;
    }
    addedCount += delta;
    std::string displayName =
        ResolveInventoryDisplayNameForKey(key, currentSnapshot, state.inventory);
    addedItems.push_back(PickupDeltaEntry(displayName, delta));

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
    if (matchedTransferGainByKey) {
      std::map<std::string, int>::const_iterator matchedIt =
          matchedTransferGainByKey->find(key);
      if (matchedIt != matchedTransferGainByKey->end()) {
        int matchedQty = matchedIt->second;
        if (matchedQty > 0 && stolenDelta > 0) {
          stolenDelta -= matchedQty;
          if (stolenDelta < 0) {
            stolenDelta = 0;
          }
        }
      }
    }
    if (stolenDelta > 0) {
      addedStolenCount += stolenDelta;
    }
  }

  if (addedCount <= 0) {
    return;
  }

  std::string actorName = ResolveCharacterNameSafe(npc);
  std::string actorFaction = SafeFaction(npc);

  bool isPlayerActor = false;
  try {
    isPlayerActor = npc && (uintptr_t)npc > 0x1000 && npc->isPlayerCharacter();
  } catch (...) {
    isPlayerActor = false;
  }
  int catsSpent = 0;
  if (state.hasMoney && hasMoneyAfter && state.money > moneyAfter) {
    catsSpent = state.money - moneyAfter;
  }

  if (isPlayerActor && catsSpent > 0) {
    GameWorld *world = GetWorldSafe();
    Character *seller = ResolveLikelyTraderForActor(world, npc);
    bool sellerValid = seller && (uintptr_t)seller > 0x1000 && seller != npc;
    bool sellerIsTrader = false;
    if (sellerValid) {
      try {
        sellerIsTrader = seller->isATrader();
      } catch (...) {
        sellerIsTrader = false;
      }
    }
    if (sellerValid && sellerIsTrader) {
      std::string sellerName = ResolveCharacterNameSafe(seller);
      std::string sellerFaction = SafeFaction(seller);
      std::map<std::string, int> buyDeltaByKey;
      for (std::map<std::string, int>::const_iterator it =
               currentSnapshot.countsByKey.begin();
           it != currentSnapshot.countsByKey.end(); ++it) {
        int beforeCount = 0;
        std::map<std::string, int>::const_iterator beforeIt =
            state.inventory.countsByKey.find(it->first);
        if (beforeIt != state.inventory.countsByKey.end()) {
          beforeCount = beforeIt->second;
        }
        if (it->second > beforeCount) {
          buyDeltaByKey[it->first] = it->second - beforeCount;
        }
      }
      std::string itemsText =
          BuildInventoryDeltaItemsText(buyDeltaByKey, currentSnapshot, state.inventory);
      if (itemsText.empty()) {
        itemsText = ToString(addedCount) + "x Unknown Item";
      }
      std::string message =
          "bought " + itemsText + " from " + sellerName + " for " +
          ToString(catsSpent) + " cats";
      LogGameEvent("trade", actorName, actorFaction, sellerName, sellerFaction,
                   message, ResolveCharacterSerialForEvent(npc),
                   ResolveCharacterSerialForEvent(seller));
      return;
    }
  }

  std::string mode = "normal";
  if (addedStolenCount > 0 && addedStolenCount >= addedCount) {
    mode = "theft";
  } else if (addedStolenCount > 0) {
    mode = "mixed";
  }

  std::string message = "";
  if (addedItems.size() <= 1) {
    std::string itemName = !addedItems.empty() && !addedItems[0].name.empty()
                               ? addedItems[0].name
                               : "Unknown Item";
    message =
        "picked up " + ToString(addedCount) + "x " + itemName + " (" + mode + ")";
  } else {
    std::stable_sort(
        addedItems.begin(), addedItems.end(),
        [](const PickupDeltaEntry &left,
           const PickupDeltaEntry &right) -> bool {
          if (left.count != right.count) {
            return left.count > right.count;
          }
          return left.name < right.name;
        });

    std::string breakdown = "";
    for (size_t i = 0; i < addedItems.size(); ++i) {
      if (i > 0) {
        breakdown += ", ";
      }
      breakdown += ToString(addedItems[i].count) + "x " + addedItems[i].name;
    }

    message = "picked up " + ToString(addedCount) + " total: " + breakdown +
              " (" + mode + ")";
  }
  LogGameEvent("item_pickup", actorName, actorFaction, "None", "None",
               message, ResolveCharacterSerialForEvent(npc), 0);
}

struct PendingInventoryTransferDelta {
  unsigned int serial;
  Character *npc;
  std::string actorName;
  std::string actorFaction;
  InventoryEventSnapshot beforeSnapshot;
  InventoryEventSnapshot afterSnapshot;
  std::map<std::string, int> gainByKey;
  std::map<std::string, int> lossByKey;

  PendingInventoryTransferDelta()
      : serial(0), npc(nullptr), actorName(""), actorFaction("") {}
};

struct PendingInventoryPickupEvent {
  unsigned int serial;
  Character *npc;
  NpcWorldEventState priorState;
  InventoryEventSnapshot currentSnapshot;
  bool hasMoneyAfter;
  int moneyAfter;

  PendingInventoryPickupEvent()
      : serial(0), npc(nullptr), priorState(), hasMoneyAfter(false),
        moneyAfter(0) {}
};

struct TransferEventAggregation {
  unsigned int fromSerial;
  unsigned int toSerial;
  std::string fromName;
  std::string fromFaction;
  std::string toName;
  std::string toFaction;
  std::map<std::string, int> qtyByKey;
  std::map<std::string, std::string> displayNameByKey;
  std::vector<std::string> itemOrder;

  TransferEventAggregation()
      : fromSerial(0), toSerial(0), fromName(""), fromFaction(""),
        toName(""), toFaction("") {}
};

struct PendingTransferLoss {
  unsigned int fromSerial;
  std::string fromName;
  std::string fromFaction;
  std::string itemKey;
  std::string itemName;
  int qty;
  DWORD observedTick;

  PendingTransferLoss()
      : fromSerial(0), fromName(""), fromFaction(""), itemKey(""),
        itemName(""), qty(0), observedTick(0) {}
};

static std::string BuildTransferPairKey(unsigned int fromSerial,
                                        unsigned int toSerial) {
  return ToString(fromSerial) + "->" + ToString(toSerial);
}

static Character *ResolveCharacterBySerialForInventoryEvent(unsigned int serial) {
  if (serial == 0) {
    return nullptr;
  }
  GameWorld *world = GetWorldSafe();
  if (!world || (uintptr_t)world < 0x1000) {
    return nullptr;
  }
  try {
    const auto &chars = world->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it) {
      Character *candidate = *it;
      if (!candidate || (uintptr_t)candidate < 0x1000) {
        continue;
      }
      unsigned int candidateSerial = 0;
      try {
        candidateSerial = candidate->getHandle().serial;
      } catch (...) {
        candidateSerial = 0;
      }
      if (candidateSerial == serial) {
        return candidate;
      }
    }
  } catch (...) {
  }
  return nullptr;
}

static Character *ResolveLikelyInventoryTransferCounterparty(Character *actor) {
  if (!actor || (uintptr_t)actor < 0x1000) {
    return nullptr;
  }

  GameWorld *world = GetWorldSafe();
  if (!world || (uintptr_t)world < 0x1000) {
    return nullptr;
  }

  Character *talkTarget = ResolveCharacterFromHandSafe(g_talkTargetHand);
  if (talkTarget && (uintptr_t)talkTarget > 0x1000) {
    if (talkTarget == actor) {
      Character *playerSpeaker = ResolveNearestPlayerSpeakerForTarget(world, actor);
      if (playerSpeaker && (uintptr_t)playerSpeaker > 0x1000 &&
          playerSpeaker != actor) {
        return playerSpeaker;
      }
    } else {
      bool actorIsPlayer = false;
      try {
        actorIsPlayer = actor->isPlayerCharacter();
      } catch (...) {
        actorIsPlayer = false;
      }
      if (actorIsPlayer) {
        return talkTarget;
      }
    }
  }

  std::string listenerSource = "";
  Character *listener =
      ResolveDialogueListenerForSpeech(actor, nullptr, listenerSource);
  if (listener && (uintptr_t)listener > 0x1000 && listener != actor) {
    return listener;
  }

  Character *selectionTarget = ResolveCharacterFromHandSafe(g_lastSelectionHand);
  if (selectionTarget && (uintptr_t)selectionTarget > 0x1000 &&
      selectionTarget == actor) {
    Character *playerSpeaker = ResolveNearestPlayerSpeakerForTarget(world, actor);
    if (playerSpeaker && (uintptr_t)playerSpeaker > 0x1000 &&
        playerSpeaker != actor) {
      return playerSpeaker;
    }
  }

  return nullptr;
}

static void EmitInventoryTransferEventsFromDeltas(
    const std::vector<PendingInventoryTransferDelta> &deltas,
    std::map<unsigned int, std::map<std::string, int> > &matchedGainBySerialOut) {
  matchedGainBySerialOut.clear();

  static std::deque<PendingTransferLoss> pendingLosses;
  DWORD nowTick = GetTickCount();
  const DWORD kPendingLossMaxAgeMs = 6000;
  const size_t kPendingLossMaxEntries = 512;

  std::map<std::string, TransferEventAggregation> aggregatedByPair;
  std::map<std::string, TransferEventAggregation> agedOutByPair;

  auto appendAggregation = [&](std::map<std::string, TransferEventAggregation> &dest,
                               unsigned int fromSerial,
                               const std::string &fromName,
                               const std::string &fromFaction,
                               unsigned int toSerial, const std::string &toName,
                               const std::string &toFaction,
                               const std::string &itemKey,
                               const std::string &itemName, int matchedQty) {
    if (fromSerial == 0 || itemKey.empty() || matchedQty <= 0) {
      return;
    }
    std::string pairKey = BuildTransferPairKey(fromSerial, toSerial);
    TransferEventAggregation &agg = dest[pairKey];
    agg.fromSerial = fromSerial;
    agg.toSerial = toSerial;
    if (agg.fromName.empty()) {
      agg.fromName = fromName;
    }
    if (agg.fromFaction.empty()) {
      agg.fromFaction = fromFaction;
    }
    if (agg.toName.empty()) {
      agg.toName = toName;
    }
    if (agg.toFaction.empty()) {
      agg.toFaction = toFaction;
    }
    if (agg.qtyByKey.count(itemKey) == 0) {
      agg.itemOrder.push_back(itemKey);
    }
    agg.qtyByKey[itemKey] += matchedQty;
    if (agg.displayNameByKey.count(itemKey) == 0) {
      agg.displayNameByKey[itemKey] = itemName;
    }
  };

  for (std::deque<PendingTransferLoss>::iterator it = pendingLosses.begin();
       it != pendingLosses.end();) {
    bool expired = (it->observedTick == 0) ||
                   (nowTick - it->observedTick > kPendingLossMaxAgeMs);
    bool empty = it->qty <= 0 || it->itemKey.empty() || it->fromSerial == 0;
    if (expired || empty) {
      if (!empty && expired) {
        Character *fromActor =
            ResolveCharacterBySerialForInventoryEvent(it->fromSerial);
        Character *counterparty =
            ResolveLikelyInventoryTransferCounterparty(fromActor);
        if (counterparty && (uintptr_t)counterparty > 0x1000) {
          appendAggregation(
              agedOutByPair, it->fromSerial, it->fromName, it->fromFaction,
              ResolveCharacterSerialForEvent(counterparty),
              ResolveCharacterNameSafe(counterparty), SafeFaction(counterparty),
              it->itemKey, it->itemName, it->qty);
        } else {
          appendAggregation(agedOutByPair, it->fromSerial, it->fromName,
                            it->fromFaction, 0, "Ground", "None", it->itemKey,
                            it->itemName, it->qty);
        }
      }
      it = pendingLosses.erase(it);
    } else {
      ++it;
    }
  }

  if (!deltas.empty()) {
    std::vector<PendingInventoryTransferDelta> working = deltas;
    auto appendMatch = [&](unsigned int fromSerial, const std::string &fromName,
                           const std::string &fromFaction, unsigned int toSerial,
                           const std::string &toName,
                           const std::string &toFaction,
                           const std::string &itemKey,
                           const std::string &itemName, int matchedQty) {
      appendAggregation(aggregatedByPair, fromSerial, fromName, fromFaction,
                        toSerial, toName, toFaction, itemKey, itemName,
                        matchedQty);
    };

    for (size_t toIndex = 0; toIndex < working.size(); ++toIndex) {
      PendingInventoryTransferDelta &toDelta = working[toIndex];
      if (toDelta.serial == 0) {
        continue;
      }

      for (std::map<std::string, int>::iterator gainIt = toDelta.gainByKey.begin();
           gainIt != toDelta.gainByKey.end(); ++gainIt) {
        const std::string &itemKey = gainIt->first;
        int gainRemaining = gainIt->second;
        if (gainRemaining <= 0) {
          continue;
        }

        for (size_t fromIndex = 0;
             fromIndex < working.size() && gainRemaining > 0; ++fromIndex) {
          if (fromIndex == toIndex) {
            continue;
          }
          PendingInventoryTransferDelta &fromDelta = working[fromIndex];
          if (fromDelta.serial == 0 || fromDelta.serial == toDelta.serial) {
            continue;
          }
          std::map<std::string, int>::iterator lossIt =
              fromDelta.lossByKey.find(itemKey);
          if (lossIt == fromDelta.lossByKey.end() || lossIt->second <= 0) {
            continue;
          }

          int matchedQty =
              gainRemaining < lossIt->second ? gainRemaining : lossIt->second;
          if (matchedQty <= 0) {
            continue;
          }

          gainRemaining -= matchedQty;
          gainIt->second -= matchedQty;
          lossIt->second -= matchedQty;
          matchedGainBySerialOut[toDelta.serial][itemKey] += matchedQty;
          appendMatch(fromDelta.serial, fromDelta.actorName, fromDelta.actorFaction,
                      toDelta.serial, toDelta.actorName, toDelta.actorFaction,
                      itemKey,
                      ResolveInventoryDisplayNameForKey(
                          itemKey, fromDelta.beforeSnapshot, toDelta.afterSnapshot),
                      matchedQty);
        }

        if (gainRemaining > 0) {
          for (std::deque<PendingTransferLoss>::iterator pendingIt =
                   pendingLosses.begin();
               pendingIt != pendingLosses.end() && gainRemaining > 0;
               ++pendingIt) {
            if (pendingIt->qty <= 0 || pendingIt->itemKey != itemKey) {
              continue;
            }
            if (pendingIt->fromSerial == 0 ||
                pendingIt->fromSerial == toDelta.serial) {
              continue;
            }
            int matchedQty =
                gainRemaining < pendingIt->qty ? gainRemaining : pendingIt->qty;
            if (matchedQty <= 0) {
              continue;
            }
            gainRemaining -= matchedQty;
            gainIt->second -= matchedQty;
            pendingIt->qty -= matchedQty;
            matchedGainBySerialOut[toDelta.serial][itemKey] += matchedQty;
            appendMatch(
                pendingIt->fromSerial, pendingIt->fromName, pendingIt->fromFaction,
                toDelta.serial, toDelta.actorName, toDelta.actorFaction, itemKey,
                pendingIt->itemName.empty()
                    ? ResolveInventoryDisplayNameForKey(
                          itemKey, toDelta.afterSnapshot, toDelta.beforeSnapshot)
                    : pendingIt->itemName,
                matchedQty);
          }
        }
      }
    }

    for (std::deque<PendingTransferLoss>::iterator it = pendingLosses.begin();
         it != pendingLosses.end();) {
      if (it->qty <= 0) {
        it = pendingLosses.erase(it);
      } else {
        ++it;
      }
    }

    for (size_t fromIndex = 0; fromIndex < working.size(); ++fromIndex) {
      PendingInventoryTransferDelta &fromDelta = working[fromIndex];
      if (fromDelta.serial == 0) {
        continue;
      }
      for (std::map<std::string, int>::const_iterator lossIt =
               fromDelta.lossByKey.begin();
           lossIt != fromDelta.lossByKey.end(); ++lossIt) {
        const std::string &itemKey = lossIt->first;
        int lossQty = lossIt->second;
        if (itemKey.empty() || lossQty <= 0) {
          continue;
        }
        PendingTransferLoss pending;
        pending.fromSerial = fromDelta.serial;
        pending.fromName = fromDelta.actorName;
        pending.fromFaction = fromDelta.actorFaction;
        pending.itemKey = itemKey;
        pending.itemName = ResolveInventoryDisplayNameForKey(
            itemKey, fromDelta.beforeSnapshot, fromDelta.afterSnapshot);
        pending.qty = lossQty;
        pending.observedTick = nowTick;
        pendingLosses.push_back(pending);
      }
    }
  }

  while (pendingLosses.size() > kPendingLossMaxEntries) {
    const PendingTransferLoss &overflow = pendingLosses.front();
    if (overflow.qty > 0 && overflow.fromSerial != 0 && !overflow.itemKey.empty()) {
      Character *fromActor =
          ResolveCharacterBySerialForInventoryEvent(overflow.fromSerial);
      Character *counterparty =
          ResolveLikelyInventoryTransferCounterparty(fromActor);
      if (counterparty && (uintptr_t)counterparty > 0x1000) {
        appendAggregation(
            agedOutByPair, overflow.fromSerial, overflow.fromName,
            overflow.fromFaction, ResolveCharacterSerialForEvent(counterparty),
            ResolveCharacterNameSafe(counterparty), SafeFaction(counterparty),
            overflow.itemKey, overflow.itemName, overflow.qty);
      } else {
        appendAggregation(agedOutByPair, overflow.fromSerial, overflow.fromName,
                          overflow.fromFaction, 0, "Ground", "None",
                          overflow.itemKey, overflow.itemName, overflow.qty);
      }
    }
    pendingLosses.pop_front();
  }

  auto emitAggregations =
      [&](std::map<std::string, TransferEventAggregation> &byPair) {
        for (std::map<std::string, TransferEventAggregation>::iterator it =
                 byPair.begin();
             it != byPair.end(); ++it) {
          TransferEventAggregation &agg = it->second;
          if (agg.qtyByKey.empty()) {
            continue;
          }

          std::string itemsText = "";
          for (size_t i = 0; i < agg.itemOrder.size(); ++i) {
            const std::string &itemKey = agg.itemOrder[i];
            std::map<std::string, int>::const_iterator qtyIt =
                agg.qtyByKey.find(itemKey);
            if (qtyIt == agg.qtyByKey.end() || qtyIt->second <= 0) {
              continue;
            }
            std::string itemName = "Unknown Item";
            std::map<std::string, std::string>::const_iterator nameIt =
                agg.displayNameByKey.find(itemKey);
            if (nameIt != agg.displayNameByKey.end() && !nameIt->second.empty()) {
              itemName = nameIt->second;
            }
            if (!itemsText.empty()) {
              itemsText += ", ";
            }
            itemsText += ToString(qtyIt->second) + "x " + itemName;
          }
          if (itemsText.empty()) {
            continue;
          }

          std::string fromName = agg.fromName.empty() ? "Unknown" : agg.fromName;
          std::string toName = agg.toName.empty() ? "Unknown" : agg.toName;
          std::string fromFaction =
              agg.fromFaction.empty() ? "None" : agg.fromFaction;
          std::string toFaction = agg.toFaction.empty() ? "None" : agg.toFaction;

          std::string toNameLower = ToLowerAsciiCopy(TrimCopy(toName));
          bool isGroundDrop = (agg.toSerial == 0) || (toNameLower == "ground");
          std::string message = isGroundDrop
                                    ? ("dropped " + itemsText +
                                       " on the ground")
                                    : ("transferred " + itemsText + " to " +
                                       toName);
          LogGameEvent("trade", fromName, fromFaction, toName, toFaction,
                       message, agg.fromSerial, agg.toSerial);
        }
      };
  emitAggregations(aggregatedByPair);
  emitAggregations(agedOutByPair);
}

static Character *ResolveCharacterBySerialForCarryEvent(unsigned int serial) {
  return ResolveCharacterBySerialForInventoryEvent(serial);
}

static void EmitCarryPickupEvent(Character *carrier, unsigned int targetSerial,
                                 const std::string &targetNameHint) {
  std::string actorName = ResolveCharacterNameSafe(carrier);
  std::string actorFaction = SafeFaction(carrier);
  Character *target = ResolveCharacterBySerialForCarryEvent(targetSerial);
  std::string targetName = target ? ResolveCharacterNameSafe(target)
                                  : TrimCopy(targetNameHint);
  if (targetName.empty()) {
    targetName = "someone";
  }
  std::string targetFaction = target ? SafeFaction(target) : "None";
  std::string message = "picked up " + targetName;
  LogGameEvent("carry", actorName, actorFaction, targetName, targetFaction,
               message, ResolveCharacterSerialForEvent(carrier),
               targetSerial != 0 ? targetSerial
                                 : ResolveCharacterSerialForEvent(target));
}

static void EmitCarryDropEvent(Character *carrier, unsigned int targetSerial,
                               const std::string &targetNameHint) {
  std::string actorName = ResolveCharacterNameSafe(carrier);
  std::string actorFaction = SafeFaction(carrier);
  Character *target = ResolveCharacterBySerialForCarryEvent(targetSerial);
  std::string targetName = target ? ResolveCharacterNameSafe(target)
                                  : TrimCopy(targetNameHint);
  if (targetName.empty()) {
    targetName = "their carried target";
  }
  std::string targetFaction = target ? SafeFaction(target) : "None";
  std::string message = "put down " + targetName;
  LogGameEvent("carry", actorName, actorFaction, targetName, targetFaction,
               message, ResolveCharacterSerialForEvent(carrier),
               targetSerial != 0 ? targetSerial
                                 : ResolveCharacterSerialForEvent(target));
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
  PrunePendingMedicalItemUseState(nowTick);
  PruneHealingEventSessionState(nowTick);
  PrunePredationEventSessionState(nowTick);
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

static bool TryParseInfoTelemetryBool(const std::string &rawValue,
                                      bool &valueOut) {
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

static bool IsInfoTelemetryIndoorsHandleValid(const hand &indoorsHandle) {
  return indoorsHandle.isValid() && !indoorsHandle.isNull();
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

  std::string rawTown = JsonReadField(contextJson, "town");
  std::string rawZone = JsonReadField(contextJson, "zone");
  std::string rawRegion = JsonReadField(contextJson, "region");
  std::string town = NormalizeInfoTelemetryToken(rawTown);
  std::string zone = NormalizeInfoTelemetryToken(rawZone);
  std::string region = NormalizeInfoTelemetryToken(rawRegion);
  std::string environmentJson = JsonReadField(contextJson, "environment");
  std::string rawBuilding = JsonReadField(environmentJson, "building_name");
  std::string rawEnvZone = JsonReadField(environmentJson, "zone_name");
  std::string rawEnvRegionName = JsonReadField(environmentJson, "region_name");
  std::string rawEnvRegion = JsonReadField(environmentJson, "region");
  std::string building = NormalizeInfoTelemetryToken(rawBuilding);
  if (zone.empty()) {
    zone = NormalizeInfoTelemetryToken(rawEnvZone);
  }
  if (region.empty()) {
    region = NormalizeInfoTelemetryToken(rawEnvRegionName);
  }
  if (region.empty()) {
    region = NormalizeInfoTelemetryToken(rawEnvRegion);
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
  bool indoorsKnown = TryParseInfoTelemetryBool(
      JsonReadField(environmentJson, "indoors"), indoorsValue);
  bool outdoorsKnown = TryParseInfoTelemetryBool(
      JsonReadField(environmentJson, "outdoors"), outdoorsValue);
  bool inTownKnown = TryParseInfoTelemetryBool(
      JsonReadField(environmentJson, "in_town"), inTownValue);
  bool useBuilding =
      IsInfoTelemetryIndoorsHandleValid(player->isIndoors());
  if (indoorsKnown && indoorsValue) {
    useBuilding = true;
  }
  if ((outdoorsKnown && outdoorsValue) || (indoorsKnown && !indoorsValue)) {
    useBuilding = false;
  }
  if (!useBuilding) {
    building.clear();
  }
  std::string playerName = "Unknown";
  try {
    playerName = player->getName();
  } catch (...) {
    playerName = "Unknown";
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
  if (location.empty()) {
    Log("GEO_DEBUG_INFOLOC: skipped empty location actor=" + playerName +
        " raw_town=" + safeGeoToken(rawTown) + " raw_zone=" + safeGeoToken(rawZone) +
        " raw_region=" + safeGeoToken(rawRegion) +
        " raw_building=" + safeGeoToken(rawBuilding) +
        " raw_env_zone=" + safeGeoToken(rawEnvZone) +
        " raw_env_region_name=" + safeGeoToken(rawEnvRegionName) +
        " raw_env_region=" + safeGeoToken(rawEnvRegion) +
        " building=" + safeGeoToken(building) + " town=" + safeGeoToken(town) +
        " zone=" + safeGeoToken(zone) + " region=" + safeGeoToken(region) +
        " indoors=" + std::string(useBuilding ? "1" : "0") +
        " in_town=" +
        std::string((inTownKnown && inTownValue) ? "1" : (inTownKnown ? "0" : "?")));
    return false;
  }

  std::string locationWithRegion = location;
  if (!region.empty() &&
      ToLowerAsciiCopy(locationWithRegion).find(ToLowerAsciiCopy(region)) ==
          std::string::npos) {
    locationWithRegion += ", " + region;
  }

  messageOut = "location update: " + locationWithRegion;
  digestOut = ToLowerAsciiCopy(location + "|" + zone + "|" + region);
  Log("GEO_DEBUG_INFOLOC: actor=" + playerName +
      " message=" + safeGeoToken(messageOut) +
      " digest=" + safeGeoToken(digestOut) +
      " raw_town=" + safeGeoToken(rawTown) + " raw_zone=" + safeGeoToken(rawZone) +
      " raw_region=" + safeGeoToken(rawRegion) +
      " raw_building=" + safeGeoToken(rawBuilding) +
      " raw_env_zone=" + safeGeoToken(rawEnvZone) +
      " raw_env_region_name=" + safeGeoToken(rawEnvRegionName) +
      " raw_env_region=" + safeGeoToken(rawEnvRegion) +
      " building=" + safeGeoToken(building) + " town=" + safeGeoToken(town) +
      " zone=" + safeGeoToken(zone) + " region=" + safeGeoToken(region) +
      " indoors=" + std::string(useBuilding ? "1" : "0") +
      " in_town=" +
      std::string((inTownKnown && inTownValue) ? "1" : (inTownKnown ? "0" : "?")));
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
  QueueIdentityRenameCandidate(candidate, "infonpc_scan");
}

static void PrimeNearbyIdentityRenames(GameWorld *world, Character *player,
                                       Character *selection) {
  if (!world || !player || (uintptr_t)player < 0x1000) {
    return;
  }

  float scanRange = g_boredEventRange;
  if (scanRange < 80.0f) {
    scanRange = 80.0f;
  } else if (scanRange > 600.0f) {
    scanRange = 600.0f;
  }

  std::vector<InfoNearbyNpcCandidate> candidates;
  std::set<unsigned int> seen;
  candidates.reserve(kIdentityRenameSweepCandidateLimit);

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
    if (candidates.size() >= kIdentityRenameSweepCandidateLimit) {
      break;
    }
  }
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
  if (nowTick - g_lastIdentityRenameSweepTick >=
      kIdentityRenameSweepIntervalMs) {
    g_lastIdentityRenameSweepTick = nowTick;
    PrimeNearbyIdentityRenames(world, player, selection);
  }
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

static void RunFactionRelationSync(GameWorld *world) {
  if (!world || (uintptr_t)world < 0x1000 || !world->factionMgr ||
      (uintptr_t)world->factionMgr < 0x1000) {
    return;
  }

  DWORD nowTick = GetTickCount();
  std::map<std::string, FactionRelationSnapshotEntry> previousState;
  EnterCriticalSection(&g_stateMutex);
  if (g_lastFactionRelationSyncTick != 0 &&
      (nowTick - g_lastFactionRelationSyncTick) < kFactionRelationSyncIntervalMs) {
    LeaveCriticalSection(&g_stateMutex);
    return;
  }
  if (!HasRecentDwemerDistroConnection(kRecentServerSuccessGraceMs)) {
    g_lastFactionRelationSyncTick = nowTick;
    LeaveCriticalSection(&g_stateMutex);
    static DWORD lastOfflineLogTick = 0;
    if (nowTick - lastOfflineLogTick >= 30000) {
      lastOfflineLogTick = nowTick;
      Log("FACTION_REL_SYNC: deferred while server connection is unavailable.");
    }
    return;
  }
  previousState = g_factionRelationStateByKey;
  LeaveCriticalSection(&g_stateMutex);

  const lektor<Faction *> *allFactions = nullptr;
  try {
    allFactions = world->factionMgr->getAllFactions();
  } catch (...) {
    allFactions = nullptr;
  }
  if (!allFactions || allFactions->count == 0) {
    EnterCriticalSection(&g_stateMutex);
    g_lastFactionRelationSyncTick = nowTick;
    g_factionRelationStateByKey.clear();
    LeaveCriticalSection(&g_stateMutex);
    return;
  }

  std::map<std::string, FactionRelationSnapshotEntry> currentState;
  size_t scannedPairs = 0;
  bool truncated = false;

  bool hardStop = false;
  for (uint32_t i = 0; i < allFactions->count && !hardStop; ++i) {
    Faction *sourceFaction = allFactions->stuff[i];
    if (!sourceFaction || (uintptr_t)sourceFaction <= 0x1000 ||
        !sourceFaction->relations ||
        (uintptr_t)sourceFaction->relations <= 0x1000) {
      continue;
    }

    std::string sourceName = SafeFactionName(sourceFaction);
    std::string sourceStringId = SafeFactionStringId(sourceFaction);
    int sourceNumericId = SafeFactionNumericId(sourceFaction);
    if (sourceName.empty() && sourceStringId.empty() && sourceNumericId <= 0) {
      continue;
    }

    for (uint32_t j = 0; j < allFactions->count; ++j) {
      if (scannedPairs >= kFactionRelationSyncHardCap) {
        truncated = true;
        hardStop = true;
        break;
      }

      Faction *targetFaction = allFactions->stuff[j];
      if (!targetFaction || (uintptr_t)targetFaction <= 0x1000 ||
          targetFaction == sourceFaction) {
        continue;
      }

      std::string targetName = SafeFactionName(targetFaction);
      std::string targetStringId = SafeFactionStringId(targetFaction);
      int targetNumericId = SafeFactionNumericId(targetFaction);
      if (targetName.empty() && targetStringId.empty() && targetNumericId <= 0) {
        continue;
      }

      std::string mergeKey =
          BuildFactionRelationMergeKey(sourceStringId, sourceNumericId,
                                       sourceName, targetStringId, targetNumericId,
                                       targetName);
      if (mergeKey.empty()) {
        continue;
      }

      double relationValue = 0.0;
      bool relationResolved = false;
      try {
        relationValue =
            static_cast<double>(sourceFaction->relations->getFactionRelation(
                targetFaction));
        relationResolved = true;
      } catch (...) {
        relationResolved = false;
      }
      if (!relationResolved) {
        continue;
      }

      bool alliance = false;
      bool war = false;
      bool coexists = false;
      try {
        FactionRelations::RelationData *relationData =
            sourceFaction->relations->getRelationData(targetFaction);
        if (relationData && (uintptr_t)relationData > 0x1000) {
          alliance = relationData->alliance;
          war = relationData->war;
          coexists = relationData->coexists;
        }
      } catch (...) {
      }

      FactionRelationSnapshotEntry entry;
      entry.mergeKey = mergeKey;
      entry.sourceName = sourceName;
      entry.sourceStringId = sourceStringId;
      entry.sourceNumericId = sourceNumericId;
      entry.targetName = targetName;
      entry.targetStringId = targetStringId;
      entry.targetNumericId = targetNumericId;
      entry.relation = relationValue;
      entry.alliance = alliance;
      entry.war = war;
      entry.coexists = coexists;
      currentState[mergeKey] = entry;
      ++scannedPairs;
    }
  }

  std::vector<FactionRelationSnapshotEntry> changedEntries;
  changedEntries.reserve(currentState.size());
  for (std::map<std::string, FactionRelationSnapshotEntry>::const_iterator it =
           currentState.begin();
       it != currentState.end(); ++it) {
    std::map<std::string, FactionRelationSnapshotEntry>::const_iterator previousIt =
        previousState.find(it->first);
    if (previousIt == previousState.end() ||
        IsFactionRelationEntryDifferent(it->second, previousIt->second)) {
      changedEntries.push_back(it->second);
    }
  }

  std::vector<std::string> removedKeys;
  for (std::map<std::string, FactionRelationSnapshotEntry>::const_iterator it =
           previousState.begin();
       it != previousState.end(); ++it) {
    if (currentState.find(it->first) == currentState.end()) {
      removedKeys.push_back(it->first);
    }
  }

  EnterCriticalSection(&g_stateMutex);
  g_lastFactionRelationSyncTick = nowTick;
  g_factionRelationStateByKey = currentState;
  LeaveCriticalSection(&g_stateMutex);

  if (changedEntries.empty() && removedKeys.empty()) {
    if (truncated) {
      Log("FACTION_REL_SYNC: no delta but scan truncated at " +
          ToString((int)kFactionRelationSyncHardCap) + " pairs.");
    }
    return;
  }

  bool isFullSnapshot = previousState.empty();
  int gameTs = ResolveCurrentGameTsSafe(world);
  std::string payload = "{";
  payload += "\"source\":\"faction_relations_snapshot\",";
  payload += "\"game_ts\":" + ToString(gameTs) + ",";
  payload +=
      "\"full_snapshot\":" + std::string(isFullSnapshot ? "true" : "false") + ",";
  payload += "\"truncated\":" + std::string(truncated ? "true" : "false") + ",";
  payload += "\"scanned_pairs\":" + ToString((int)scannedPairs) + ",";
  payload += "\"changed_count\":" + ToString((int)changedEntries.size()) + ",";
  payload += "\"removed_count\":" + ToString((int)removedKeys.size()) + ",";

  payload += "\"relations\":[";
  for (size_t i = 0; i < changedEntries.size(); ++i) {
    const FactionRelationSnapshotEntry &entry = changedEntries[i];
    if (i > 0) {
      payload += ",";
    }
    payload += "{";
    payload += "\"merge_key\":\"" + EscapeJSON(entry.mergeKey) + "\",";
    payload += "\"source_name\":\"" + EscapeJSON(entry.sourceName) + "\",";
    payload += "\"source_string_id\":\"" + EscapeJSON(entry.sourceStringId) +
               "\",";
    payload += "\"source_numeric_id\":" + ToString(entry.sourceNumericId) + ",";
    payload += "\"target_name\":\"" + EscapeJSON(entry.targetName) + "\",";
    payload += "\"target_string_id\":\"" + EscapeJSON(entry.targetStringId) +
               "\",";
    payload += "\"target_numeric_id\":" + ToString(entry.targetNumericId) + ",";
    payload += "\"relation\":" + ToString(static_cast<float>(entry.relation)) +
               ",";
    payload += "\"alliance\":" + std::string(entry.alliance ? "true" : "false") +
               ",";
    payload += "\"war\":" + std::string(entry.war ? "true" : "false") + ",";
    payload += "\"coexists\":" + std::string(entry.coexists ? "true" : "false");
    payload += "}";
  }
  payload += "],";

  payload += "\"removed\":[";
  for (size_t i = 0; i < removedKeys.size(); ++i) {
    if (i > 0) {
      payload += ",";
    }
    payload += "{\"merge_key\":\"" + EscapeJSON(removedKeys[i]) + "\"}";
  }
  payload += "]";
  payload += "}";

  AsyncPostToStobe(L"/faction_relations", payload);
  Log("FACTION_REL_SYNC: sent changed=" + ToString((int)changedEntries.size()) +
      " removed=" + ToString((int)removedKeys.size()) +
      " scanned_pairs=" + ToString((int)scannedPairs) +
      " full_snapshot=" + std::string(isFullSnapshot ? "1" : "0") +
      " truncated=" + std::string(truncated ? "1" : "0"));
}

static void RunTownKnowledgeSync(GameWorld *world) {
  if (!world || (uintptr_t)world < 0x1000 || !shou ||
      (uintptr_t)shou < 0x1000 || !shou->townList ||
      (uintptr_t)shou->townList < 0x1000) {
    return;
  }

  const DWORD nowTick = GetTickCount();
  if (g_lastTownKnowledgeScanTick != 0 &&
      (nowTick - g_lastTownKnowledgeScanTick) <
          kTownKnowledgeScanIntervalMs) {
    return;
  }
  g_lastTownKnowledgeScanTick = nowTick;

  if (!HasRecentDwemerDistroConnection(kRecentServerSuccessGraceMs)) {
    return;
  }

  lektor<RootObject *> *allTowns = nullptr;
  try {
    allTowns = &shou->townList->getAllTowns();
  } catch (...) {
    allTowns = nullptr;
  }
  if (!allTowns) {
    return;
  }

  std::string townsJson = "[";
  size_t emitted = 0;
  const uint32_t count = allTowns->count;
  for (uint32_t i = 0; i < count && emitted < kTownKnowledgeHardCap; ++i) {
    RootObject *object = allTowns->stuff[i];
    if (!object || (uintptr_t)object < 0x1000) {
      continue;
    }

    TownBase *townBase = nullptr;
    bool discovered = false;
    bool explored = false;
    std::string name;
    Ogre::Vector3 position = Ogre::Vector3::ZERO;
    try {
      if (object->getDataType() != TOWN) {
        continue;
      }
      townBase = static_cast<TownBase *>(object);
      discovered = townBase->_NV_isDiscovered();
      explored = townBase->_NV_isExplored();
      if (!discovered) {
        continue;
      }
      name = TrimCopy(townBase->getKnownName());
      if (name.empty()) {
        continue;
      }
      position = object->getPosition();
    } catch (...) {
      continue;
    }

    if (emitted > 0) {
      townsJson += ",";
    }
    townsJson += "{\"name\":\"" + EscapeJSON(name) + "\",";
    townsJson += "\"x\":" + ToString(position.x) + ",";
    townsJson += "\"y\":" + ToString(position.y) + ",";
    townsJson += "\"z\":" + ToString(position.z) + ",";
    townsJson += "\"discovered\":true,";
    townsJson +=
        "\"explored\":" + std::string(explored ? "true" : "false") + "}";
    ++emitted;
  }
  townsJson += "]";

  const bool changed = townsJson != g_lastTownKnowledgeDigest;
  const bool resendDue = g_lastTownKnowledgeSentTick == 0 ||
                         (nowTick - g_lastTownKnowledgeSentTick) >=
                             kTownKnowledgeResendIntervalMs;
  if (!changed && !resendDue) {
    return;
  }

  const int gameTs = ResolveCurrentGameTsSafe(world);
  std::string payload = "{\"source\":\"kenshi_town_list\",";
  payload += "\"game_ts\":" + ToString(gameTs) + ",";
  payload += "\"towns\":" + townsJson + "}";
  AsyncPostToStobe(L"/town_knowledge", payload);
  g_lastTownKnowledgeDigest = townsJson;
  g_lastTownKnowledgeSentTick = nowTick;
  Log("TOWN_KNOWLEDGE_SYNC: sent discovered=" + ToString((int)emitted) +
      " scanned=" + ToString((int)count) +
      " changed=" + std::string(changed ? "1" : "0"));
}

static void RunNpcWorldEventSweepUnsafe(GameWorld *world, Character *selection) {
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

  AddInventorySyncCandidate(player, candidates, seen);
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

  std::vector<PendingInventoryTransferDelta> pendingTransferDeltas;
  std::vector<PendingInventoryPickupEvent> pendingPickupEvents;
  pendingTransferDeltas.reserve(candidates.size());
  pendingPickupEvents.reserve(candidates.size());

  for (size_t i = 0; i < candidates.size(); ++i) {
    Character *npc = candidates[i];
    if (!npc || (uintptr_t)npc < 0x1000) {
      continue;
    }
    bool isPlayerActor = (npc == player);

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
    bool hasMoneyNow = false;
    int moneyNow = 0;
    bool hasHungerNow = false;
    float hungerNow = 0.0f;
    float fedNow = 0.0f;
    float satietyNow = 0.0f;
    int lockpickingNow = 0;
    try {
      deadNow = npc->isDead();
      unconsciousNow = npc->isUnconcious();
      SlaveStateEnum slaveState = npc->isSlave();
      enslavedNow = (slaveState != 0) || npc->isChainedMode();
    } catch (...) {
      continue;
    }
    hasMoneyNow = TryResolveCharacterMoneySafe(npc, moneyNow);
    hasHungerNow = ResolveNpcHungerMetrics(npc, hungerNow, fedNow, satietyNow);

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
    std::string rawSpeechLine = ReadNpcSpeechLineSafe(npc);
    std::string speechLine = rawSpeechLine;
    bool speechHadTtsMetadata = false;
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
      speechLine = SanitizeCapturedDialogueLine(speechLine);
    }
    std::string normalizedSpeechLine = ToLowerAsciiCopy(TrimCopy(speechLine));
    if (normalizedSpeechLine.length() > 240) {
      normalizedSpeechLine = normalizedSpeechLine.substr(0, 240);
    }
    bool dialogueConversationActiveNow = false;
    if (npc->dialogue && (uintptr_t)npc->dialogue > 0x1000) {
      try {
        dialogueConversationActiveNow =
            !npc->dialogue->conversationHasEndedPrettyMuch();
      } catch (...) {
        dialogueConversationActiveNow = false;
      }
    }
    hand currentTaskSubject;
    TaskType currentTaskNow = ResolveCurrentNpcTaskSafe(npc, currentTaskSubject);
    unsigned int currentTaskSubjectSerialNow = 0;
    std::string currentTaskSubjectNameNow = "";
    if (currentTaskSubject.isValid() && !currentTaskSubject.isNull()) {
      currentTaskSubjectSerialNow = currentTaskSubject.serial;
      currentTaskSubjectNameNow = ResolveConstructionSubjectName(currentTaskSubject);
    }
    ConstructionActionEventType constructionActionNow =
        ResolveConstructionActionType(currentTaskNow);
    unsigned int constructionSubjectSerialNow = 0;
    std::string constructionSubjectNameNow = "";
    if (constructionActionNow != CONSTRUCTION_ACTION_NONE) {
      constructionSubjectSerialNow = currentTaskSubject.serial;
      constructionSubjectNameNow = ResolveConstructionSubjectName(currentTaskSubject);
    }
    bool carryingNow = false;
    unsigned int carryingTargetSerialNow = 0;
    std::string carryingTargetNameNow = "";
    try {
      carryingNow = npc->isCarryingSomething && npc->carryingObject.isValid();
      if (carryingNow) {
        carryingTargetSerialNow = npc->carryingObject.serial;
        carryingTargetNameNow = ResolveConstructionSubjectName(npc->carryingObject);
      }
    } catch (...) {
      carryingNow = false;
      carryingTargetSerialNow = 0;
      carryingTargetNameNow.clear();
    }
    if (carryingNow && carryingTargetNameNow.empty()) {
      carryingTargetNameNow = "someone";
    }

    NpcWorldEventState &state = g_npcWorldEventStateBySerial[serial];
    if (!state.initialized) {
      state.initialized = true;
      state.dead = deadNow;
      state.unconscious = unconsciousNow;
      state.enslaved = enslavedNow;
      state.hasMoney = hasMoneyNow;
      state.money = moneyNow;
      state.hasHunger = hasHungerNow;
      state.hunger = hungerNow;
      state.fed = fedNow;
      state.satiety = satietyNow;
      state.carrying = carryingNow;
      state.carryingTargetSerial = carryingTargetSerialNow;
      state.carryingTargetName = carryingTargetNameNow;
      state.speechActive = speechActiveNow;
      state.leftArmPresent = leftArmPresent;
      state.rightArmPresent = rightArmPresent;
      state.leftLegPresent = leftLegPresent;
      state.rightLegPresent = rightLegPresent;
      state.leftArmState = leftArmState;
      state.rightArmState = rightArmState;
      state.leftLegState = leftLegState;
      state.rightLegState = rightLegState;
      state.currentTask = (int)currentTaskNow;
      state.currentTaskSubjectSerial = currentTaskSubjectSerialNow;
      state.currentTaskSubjectName = currentTaskSubjectNameNow;
      state.constructionAction = (int)constructionActionNow;
      state.constructionSubjectSerial = constructionSubjectSerialNow;
      state.constructionSubjectName = constructionSubjectNameNow;
      state.lockpickingSkill = lockpickingNow;
      state.lastSpeechLine = normalizedSpeechLine;
      state.inventory = inventorySnapshot;
      state.lastSeenTick = nowTick;
      if (!isPlayerActor && IsAnyPredationTask(currentTaskNow)) {
        EmitPredationEventFromTask(npc, currentTaskSubject, nowTick,
                                   currentTaskNow);
      }
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

    NpcWorldEventState previousState = state;
    std::map<std::string, int> gainByKey;
    std::map<std::string, int> lossByKey;
    bool suppressPickupEvent = false;
    ComputeInventoryDeltaByKey(previousState.inventory, inventorySnapshot,
                               gainByKey, lossByKey);
    if (!gainByKey.empty() || !lossByKey.empty()) {
      bool hasMoneyDelta = previousState.hasMoney && hasMoneyNow;
      int moneyDelta = hasMoneyDelta ? (moneyNow - previousState.money) : 0;
      Character *tradeCounterparty = nullptr;
      if (hasMoneyDelta && moneyDelta != 0) {
        tradeCounterparty = ResolveLikelyInventoryTransferCounterparty(npc);
        if ((!tradeCounterparty || (uintptr_t)tradeCounterparty <= 0x1000) &&
            isPlayerActor) {
          tradeCounterparty = ResolveLikelyTraderForActor(world, npc);
        }
      }
      if (tradeCounterparty && (uintptr_t)tradeCounterparty > 0x1000) {
        std::string actorName = ResolveCharacterNameSafe(npc);
        std::string actorFaction = SafeFaction(npc);
        std::string traderName = ResolveCharacterNameSafe(tradeCounterparty);
        std::string traderFaction = SafeFaction(tradeCounterparty);
        unsigned int traderSerial =
            ResolveCharacterSerialForEvent(tradeCounterparty);

        if (moneyDelta < 0 && !gainByKey.empty()) {
          std::string itemsText = BuildInventoryDeltaItemsText(
              gainByKey, inventorySnapshot, previousState.inventory);
          if (itemsText.empty()) {
            itemsText = "items";
          }
          std::string message =
              "bought " + itemsText + " from " + traderName + " for " +
              ToString(-moneyDelta) + " cats";
          LogGameEvent("trade", actorName, actorFaction, traderName, traderFaction,
                       message, serial, traderSerial);
          gainByKey.clear();
          suppressPickupEvent = true;
        } else if (moneyDelta > 0 && !lossByKey.empty()) {
          std::string itemsText = BuildInventoryDeltaItemsText(
              lossByKey, previousState.inventory, inventorySnapshot);
          if (itemsText.empty()) {
            itemsText = "items";
          }
          std::string message =
              "sold " + itemsText + " to " + traderName + " for " +
              ToString(moneyDelta) + " cats";
          LogGameEvent("trade", actorName, actorFaction, traderName, traderFaction,
                       message, serial, traderSerial);
          lossByKey.clear();
        }
      }
    }
    if (!isPlayerActor && !lossByKey.empty()) {
      std::map<std::string, int> consumedFoodByKey =
          DetectFoodConsumptionLossByKey(
              lossByKey, previousState.inventory, previousState.hasHunger,
              previousState.hunger, previousState.fed, previousState.satiety,
              hasHungerNow, hungerNow, fedNow, satietyNow);
      if (!consumedFoodByKey.empty()) {
        SubtractInventoryDeltaByKey(lossByKey, consumedFoodByKey);
        EmitEatEvent(npc, previousState.inventory, inventorySnapshot,
                     consumedFoodByKey);
      }
    }
    if (!lossByKey.empty()) {
      std::map<std::string, int> consumedMedicalByKey =
          ConsumePendingMedicalItemLossByKey(serial, lossByKey, nowTick);
      if (!consumedMedicalByKey.empty()) {
        SubtractInventoryDeltaByKey(lossByKey, consumedMedicalByKey);
      }
    }
    if (!gainByKey.empty() || !lossByKey.empty()) {
      PendingInventoryTransferDelta transferDelta;
      transferDelta.serial = serial;
      transferDelta.npc = npc;
      transferDelta.actorName = ResolveCharacterNameSafe(npc);
      transferDelta.actorFaction = SafeFaction(npc);
      transferDelta.beforeSnapshot = previousState.inventory;
      transferDelta.afterSnapshot = inventorySnapshot;
      transferDelta.gainByKey = gainByKey;
      transferDelta.lossByKey = lossByKey;
      pendingTransferDeltas.push_back(transferDelta);
    }
    if (!suppressPickupEvent &&
        inventorySnapshot.totalCount > previousState.inventory.totalCount) {
      PendingInventoryPickupEvent pickupEvent;
      pickupEvent.serial = serial;
      pickupEvent.npc = npc;
      pickupEvent.priorState = previousState;
      pickupEvent.currentSnapshot = inventorySnapshot;
      pickupEvent.hasMoneyAfter = hasMoneyNow;
      pickupEvent.moneyAfter = moneyNow;
      pendingPickupEvents.push_back(pickupEvent);
    }

    if (!isPlayerActor) {
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
      if (!state.carrying && carryingNow) {
        EmitCarryPickupEvent(npc, carryingTargetSerialNow, carryingTargetNameNow);
      } else if (state.carrying && !carryingNow) {
        EmitCarryDropEvent(npc, state.carryingTargetSerial,
                           state.carryingTargetName);
      } else if (state.carrying && carryingNow &&
                 state.carryingTargetSerial != carryingTargetSerialNow) {
        EmitCarryDropEvent(npc, state.carryingTargetSerial,
                           state.carryingTargetName);
        EmitCarryPickupEvent(npc, carryingTargetSerialNow, carryingTargetNameNow);
      }

      bool predationTaskNow = IsAnyPredationTask(currentTaskNow);
      bool predationTaskPrevious = IsAnyPredationTask((TaskType)state.currentTask);
      bool predationTargetChanged = false;
      if (predationTaskNow && predationTaskPrevious) {
        if (currentTaskSubjectSerialNow != 0 && state.currentTaskSubjectSerial != 0) {
          predationTargetChanged =
              (currentTaskSubjectSerialNow != state.currentTaskSubjectSerial);
        } else {
          predationTargetChanged =
              ToLowerAsciiCopy(currentTaskSubjectNameNow) !=
              ToLowerAsciiCopy(state.currentTaskSubjectName);
        }
      }
      if (predationTaskNow && (!predationTaskPrevious || predationTargetChanged)) {
        EmitPredationEventFromTask(npc, currentTaskSubject, nowTick,
                                   currentTaskNow);
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

      if (lockpickingNow > state.lockpickingSkill) {
        EmitLockpickedEvent(npc, state.lockpickingSkill, lockpickingNow);
      }
      bool constructionChanged =
          state.constructionAction != (int)constructionActionNow;
      bool constructionTargetChanged =
          !constructionChanged &&
          constructionActionNow != CONSTRUCTION_ACTION_NONE &&
          ((constructionSubjectSerialNow != 0 &&
            constructionSubjectSerialNow != state.constructionSubjectSerial) ||
           ((constructionSubjectSerialNow == 0 ||
             state.constructionSubjectSerial == 0) &&
            !constructionSubjectNameNow.empty() &&
            constructionSubjectNameNow != state.constructionSubjectName));
      if (constructionActionNow == CONSTRUCTION_ACTION_BUILD &&
          (constructionChanged || constructionTargetChanged)) {
        EmitBuildEvent(npc, constructionSubjectNameNow);
      } else if (constructionActionNow == CONSTRUCTION_ACTION_DISMANTLE &&
                 (constructionChanged || constructionTargetChanged)) {
        EmitDismantleEvent(npc, constructionSubjectNameNow);
      }

      bool hasAmbientSpeech = !normalizedSpeechLine.empty();
      bool speechChanged =
          hasAmbientSpeech && normalizedSpeechLine != state.lastSpeechLine;
      bool inSpeechFlow = false;
      bool duplicateDialogue = false;
      if (g_enableRegularDialogueCapture &&
          (hasAmbientSpeech || speechActiveNow || dialogueConversationActiveNow)) {
        inSpeechFlow = IsNpcInSpeechFlowBySerial(serial);
        if (hasAmbientSpeech && speechChanged && !speechHadTtsMetadata &&
            !inSpeechFlow) {
          duplicateDialogue =
              ShouldDropDuplicateNonAiDialogue(serial, false, speechLine);
        }
      }
      if (g_enableRegularDialogueCapture && speechChanged &&
          !speechHadTtsMetadata && !inSpeechFlow && !duplicateDialogue) {
        unsigned int listenerSerial = 0;
        std::string listenerName = "None";
        std::string listenerFaction = "None";
        Character *listener = nullptr;
        std::string listenerSource = "";
        try {
          listener =
              ResolveDialogueListenerForSpeech(npc, npc->dialogue, listenerSource);
        } catch (...) {
          listener = nullptr;
        }
        if (listener && (uintptr_t)listener > 0x1000 && listener != npc) {
          listenerName = ResolveCharacterNameSafe(listener);
          listenerFaction = SafeFaction(listener);
          listenerSerial = ResolveCharacterSerialForEvent(listener);
        }
        LogGameEvent("chat", ResolveCharacterNameSafe(npc), SafeFaction(npc),
                     listenerName, listenerFaction, speechLine, serial,
                     listenerSerial);
      }
    }

    state.dead = deadNow;
    state.unconscious = unconsciousNow;
    state.enslaved = enslavedNow;
    state.hasMoney = hasMoneyNow;
    state.money = moneyNow;
    state.hasHunger = hasHungerNow;
    state.hunger = hungerNow;
    state.fed = fedNow;
    state.satiety = satietyNow;
    state.carrying = carryingNow;
    state.carryingTargetSerial = carryingTargetSerialNow;
    state.carryingTargetName = carryingTargetNameNow;
    state.speechActive = speechActiveNow;
    state.leftArmPresent = leftArmPresent;
    state.rightArmPresent = rightArmPresent;
    state.leftLegPresent = leftLegPresent;
    state.rightLegPresent = rightLegPresent;
    state.leftArmState = leftArmState;
    state.rightArmState = rightArmState;
    state.leftLegState = leftLegState;
    state.rightLegState = rightLegState;
    state.currentTask = (int)currentTaskNow;
    state.currentTaskSubjectSerial = currentTaskSubjectSerialNow;
    state.currentTaskSubjectName = currentTaskSubjectNameNow;
    state.constructionAction = (int)constructionActionNow;
    state.constructionSubjectSerial = constructionSubjectSerialNow;
    state.constructionSubjectName = constructionSubjectNameNow;
    state.lockpickingSkill = lockpickingNow;
    state.lastSpeechLine = normalizedSpeechLine;
    state.inventory = inventorySnapshot;
    state.lastSeenTick = nowTick;
  }

  std::map<unsigned int, std::map<std::string, int> > matchedTransferGainBySerial;
  EmitInventoryTransferEventsFromDeltas(pendingTransferDeltas,
                                        matchedTransferGainBySerial);
  for (size_t i = 0; i < pendingPickupEvents.size(); ++i) {
    const PendingInventoryPickupEvent &pickupEvent = pendingPickupEvents[i];
    const std::map<std::string, int> *matchedGain = nullptr;
    std::map<unsigned int, std::map<std::string, int> >::const_iterator matchedIt =
        matchedTransferGainBySerial.find(pickupEvent.serial);
    if (matchedIt != matchedTransferGainBySerial.end()) {
      matchedGain = &matchedIt->second;
    }
    EmitPickupEvent(pickupEvent.npc, pickupEvent.priorState,
                    pickupEvent.currentSnapshot, matchedGain,
                    pickupEvent.hasMoneyAfter, pickupEvent.moneyAfter);
  }

  static DWORD lastPruneTick = 0;
  if (nowTick - lastPruneTick >= 60000) {
    lastPruneTick = nowTick;
    PruneNpcWorldEventState();
  }
}

static volatile LONG g_npcWorldEventSweepSehCount = 0;
static DWORD g_npcWorldEventSweepLastSehCode = 0;

static int NpcWorldEventSweepSehFilter(unsigned int code) {
  g_npcWorldEventSweepLastSehCode = code;
  InterlockedIncrement(&g_npcWorldEventSweepSehCount);
  return EXCEPTION_EXECUTE_HANDLER;
}

static void RunNpcWorldEventSweep(GameWorld *world, Character *selection) {
  __try {
    RunNpcWorldEventSweepUnsafe(world, selection);
  } __except (NpcWorldEventSweepSehFilter(GetExceptionCode())) {
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
      std::string autonomyDecisionId;
      const bool autonomyCatalogMessage =
          ClaimPendingAutonomyCatalogMessageLocked(msg, autonomyDecisionId);
      size_t autonomyQueueSizeBefore = 0;
      if (autonomyCatalogMessage) {
        EnterCriticalSection(&g_uiMutex);
        autonomyQueueSizeBefore = g_uiActionQueue.size();
        LeaveCriticalSection(&g_uiMutex);
      }
      Log("HOOK_MSG_PROC: Processing: " + msg);

      bool isNPCAction = (msg.find("NPC_ACTION: ") == 0);
      bool isPlayerTts = (msg.find("PLAYER_TTS: ") == 0);
      bool isPlayerSay = (msg.find("PLAYER_SAY: ") == 0);
      bool isNPCSay = (msg.find("NPC_SAY: ") == 0);
      bool isNotify = (msg.find("NOTIFY:") == 0);
      bool isCmd = (msg.find("CMD:") == 0);
      bool isRename = (msg.find("NPC_RENAME: ") == 0);
      bool speakerResolvedFromHeader = false;

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
          } else if (command == "POPULATE_AIDIARY_ENTRIES") {
            PopulateAiDiaryEntries(data);
          } else if (command == "SET_AIDIARY_TEXT") {
            SetAiDiaryText(data);
          } else if (command == "SET_STOBE_HISTORY") {
            SetRecentHistoryText(data);
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
                g_lastBoredEventTick = GetTickCount();
                g_lastBoredEventGameTs = 0; // Re-arm interval on toggle.
              } else if (var == "g_boredEventIntervalHours") {
                g_boredEventIntervalHours = atoi(val.c_str());
                if (g_boredEventIntervalHours < 1) {
                  g_boredEventIntervalHours = 1;
                } else if (g_boredEventIntervalHours > 720) {
                  g_boredEventIntervalHours = 720;
                }
                g_lastBoredEventTick = GetTickCount();
                g_lastBoredEventGameTs = 0; // Re-arm interval on frequency change.
              } else if (var == "g_boredEventIntervalSeconds") {
                int legacySeconds = atoi(val.c_str());
                if (legacySeconds < 1) {
                  legacySeconds = 3600;
                }
                if (legacySeconds <= 300) {
                  g_boredEventIntervalHours = 3;
                } else {
                  g_boredEventIntervalHours = (legacySeconds + 3599) / 3600;
                }
                if (g_boredEventIntervalHours < 1) {
                  g_boredEventIntervalHours = 1;
                } else if (g_boredEventIntervalHours > 720) {
                  g_boredEventIntervalHours = 720;
                }
                g_lastBoredEventTick = GetTickCount();
                g_lastBoredEventGameTs = 0; // Re-arm interval on legacy frequency change.
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
        int ttsDurationMs = ExtractTrailingTtsDurationMs(text);
        std::string ttsHash = ExtractTrailingTtsHash(text);
        if (!g_ttsEnabled) {
          ttsDurationMs = 0;
          ttsHash.clear();
        }
        EnterCriticalSection(&g_uiMutex);
        QueuedAction act;
        act.type = ACT_NOTIFY;
        act.actor = hand();
        act.target = hand();
        act.message = text;
        act.ttsHash = ttsHash;
        act.taskValue = ttsDurationMs;
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
          Character *playerSpeaker = ResolvePreferredPlayerSpeakerForCurrentTalk(thisptr);
          if (!playerSpeaker || (uintptr_t)playerSpeaker < 0x1000) {
            playerSpeaker = ResolveFirstAliveConsciousPlayerCharacter(thisptr);
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
        g_lastBoredEventGameTs = ResolveCurrentGameTsSafe(GetWorldSafe());

        // ???? FIX: For PLAYER_SAY, ensure the bubble appears over the player,
        // not the target NPC.
        if (isPlayerSay) {
          Character *playerSpeaker = ResolvePreferredPlayerSpeakerForCurrentTalk(thisptr);
          if (!playerSpeaker && thisptr->player &&
              thisptr->player->playerCharacters.size() > 0) {
            playerSpeaker = ResolveFirstAliveConsciousPlayerCharacter(thisptr);
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
        speakerResolvedFromHeader = false;

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
              speakerResolvedFromHeader = true;
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
                speakerResolvedFromHeader = true;
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
              actionCommand == "STOPCARRYING" ||
              actionCommand == "DROPNPC" ||
              actionCommand == "DROP_NPC" ||
              actionCommand == "DROP-NPC" ||
              actionCommand == "PUTDOWNNPC" ||
              actionCommand == "PUT_DOWN_NPC" ||
              actionCommand == "PUT-DOWN-NPC" ||
              actionCommand == "RELEASENPC" ||
              actionCommand == "RELEASE_NPC" ||
              actionCommand == "RELEASE-NPC") {
            actionCommand = "STOP_CARRYING";
          } else if (actionCommand == "PICKUPNPC" ||
                     actionCommand == "PICKUP-NPC" ||
                     actionCommand == "KIDNAP") {
            actionCommand = "PICKUP_NPC";
          } else if (actionCommand == "DRINKITEM" ||
                     actionCommand == "DRINK-ITEM") {
            actionCommand = "DRINK_ITEM";
          } else if (actionCommand == "USEDRUGS" ||
                     actionCommand == "USE-DRUGS") {
            actionCommand = "USE_DRUGS";
          } else if (actionCommand == "FORCEDRINK" ||
                     actionCommand == "FORCE-DRINK") {
            actionCommand = "FORCE_DRINK";
          } else if (actionCommand == "REMOVELIMB") {
            actionCommand = "REMOVE_LIMB";
          } else if (actionCommand == "CUTHORNS" ||
                     actionCommand == "CUT-HORNS") {
            actionCommand = "CUT_HORNS";
          } else if (actionCommand == "KO" ||
                     actionCommand == "KNOCK_OUT" ||
                     actionCommand == "KNOCK-OUT") {
            actionCommand = "KNOCKOUT";
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
          bool logAsInfoAction =
              (actionCommand != "REMOVE_LIMB" && actionCommand != "CUT_HORNS");
          if (logAsInfoAction) {
            std::string actionEventMessage =
                "action command received: " + actionCommand + "@";
            if (!actionArgument.empty()) {
              actionEventMessage += actionArgument;
            }
            LogGameEvent("infoaction", actionActorName, actionActorFaction,
                         "None", "None", actionEventMessage, targetHand.serial,
                         0);
          }
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
          auto knockoutTargetMatchesActorName =
              [&](const std::string &rawTargetToken) -> bool {
            if (!targetHand.isValid() || targetHand.serial == 0 ||
                !actionActorResolved || actionActorSerial == 0 ||
                actionActorSerial != targetHand.serial || !actionActor ||
                (uintptr_t)actionActor <= 0x1000) {
              return false;
            }

            std::string tokenLow = ToLowerAsciiCopy(TrimCopy(rawTargetToken));
            if (tokenLow.empty()) {
              return false;
            }

            std::string actorNameLow = ToLowerAsciiCopy(TrimCopy(actionActorName));
            if (!actorNameLow.empty() &&
                (actorNameLow == tokenLow ||
                 actorNameLow.find(tokenLow) == 0)) {
              return true;
            }

            std::string displayNameLow =
                ToLowerAsciiCopy(TrimCopy(actionActor->displayName));
            if (!displayNameLow.empty() &&
                (displayNameLow == tokenLow ||
                 displayNameLow.find(tokenLow) == 0)) {
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
          auto parseSingleTargetPayload =
              [](const std::string &rawPayload,
                 std::string &targetOut) -> bool {
            targetOut = TrimCopy(rawPayload);
            return !targetOut.empty();
          };
          auto parseForceDrinkPayload =
              [](const std::string &rawPayload, std::string &targetOut,
                 std::string &drinkOut) -> bool {
            targetOut.clear();
            drinkOut.clear();
            std::string payload = TrimCopy(rawPayload);
            if (payload.empty()) {
              return false;
            }
            size_t splitPos = payload.find('@');
            if (splitPos == std::string::npos) {
              targetOut = TrimCopy(payload);
              drinkOut = "Cactus Rum";
              return !targetOut.empty();
            }

            targetOut = TrimCopy(payload.substr(0, splitPos));
            drinkOut = TrimCopy(payload.substr(splitPos + 1));
            if (targetOut.empty()) {
              return false;
            }
            if (drinkOut.empty()) {
              drinkOut = "Cactus Rum";
            }
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
                 std::string &itemOut, int &amountOut) -> bool {
            targetOut.clear();
            itemOut.clear();
            amountOut = 1;
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
            auto parseItemSpec = [&](const std::string &rawSpec,
                                     std::string &itemNameOut,
                                     int &itemAmountOut) -> bool {
              auto stripTrailingNoise = [](std::string value) -> std::string {
                value = TrimCopy(value);
                if (value.empty()) {
                  return value;
                }

                size_t fencePos = value.find("```");
                if (fencePos != std::string::npos) {
                  value = TrimCopy(value.substr(0, fencePos));
                }

                std::string valueLower = ToLowerAsciiCopy(value);
                size_t sourcePos = valueLower.find("[source:");
                if (sourcePos != std::string::npos) {
                  value = TrimCopy(value.substr(0, sourcePos));
                  valueLower = ToLowerAsciiCopy(value);
                }
                size_t talkingPos = valueLower.find("(talking to:");
                if (talkingPos != std::string::npos) {
                  value = TrimCopy(value.substr(0, talkingPos));
                }

                while (!value.empty()) {
                  char c = value.back();
                  if (c == '`' || c == '.' || c == ',' || c == ';' || c == ':' ||
                      c == '!' || c == '?' || c == ')' || c == ']' || c == '}') {
                    value.erase(value.size() - 1, 1);
                    continue;
                  }
                  break;
                }
                return TrimCopy(value);
              };

              itemNameOut = stripTrailingNoise(rawSpec);
              itemAmountOut = 1;
              if (itemNameOut.empty()) {
                return false;
              }

              std::string leadingLower = ToLowerAsciiCopy(itemNameOut);
              if (leadingLower.find("and ") == 0) {
                itemNameOut = TrimCopy(itemNameOut.substr(4));
              } else if (leadingLower.find("then ") == 0) {
                itemNameOut = TrimCopy(itemNameOut.substr(5));
              }
              itemNameOut = stripTrailingNoise(itemNameOut);
              if (itemNameOut.empty()) {
                return false;
              }

              if (itemNameOut.size() >= 2 &&
                  ((itemNameOut.front() == '"' && itemNameOut.back() == '"') ||
                   (itemNameOut.front() == '\'' && itemNameOut.back() == '\''))) {
                itemNameOut =
                    TrimCopy(itemNameOut.substr(1, itemNameOut.size() - 2));
                itemNameOut = stripTrailingNoise(itemNameOut);
              }
              if (itemNameOut.empty()) {
                return false;
              }

              // "2x Item Name" / "2 Item Name"
              size_t leadingDigitsEnd = 0;
              while (leadingDigitsEnd < itemNameOut.size() &&
                     std::isdigit((unsigned char)itemNameOut[leadingDigitsEnd])) {
                ++leadingDigitsEnd;
              }
              if (leadingDigitsEnd > 0 && leadingDigitsEnd < itemNameOut.size()) {
                size_t markerPos = leadingDigitsEnd;
                if (markerPos < itemNameOut.size() &&
                    (itemNameOut[markerPos] == 'x' ||
                     itemNameOut[markerPos] == 'X' ||
                     itemNameOut[markerPos] == '*')) {
                  ++markerPos;
                }
                if (markerPos < itemNameOut.size() &&
                    std::isspace((unsigned char)itemNameOut[markerPos])) {
                  std::string amountToken = itemNameOut.substr(0, leadingDigitsEnd);
                  int parsedLeadingAmount = 0;
                  if (parsePositiveAmount(amountToken, parsedLeadingAmount)) {
                    std::string remainder =
                        TrimCopy(itemNameOut.substr(markerPos + 1));
                    if (!remainder.empty()) {
                      itemNameOut = remainder;
                      itemAmountOut = parsedLeadingAmount;
                      return true;
                    }
                  }
                }
              }

              // "Item Name x2" / "Item Name 2" / compact "ItemName2"
              size_t digitStart = itemNameOut.size();
              while (digitStart > 0 &&
                     std::isdigit((unsigned char)itemNameOut[digitStart - 1])) {
                --digitStart;
              }
              if (digitStart < itemNameOut.size() && digitStart > 0) {
                std::string amountToken = itemNameOut.substr(digitStart);
                int parsedTrailingAmount = 0;
                if (parsePositiveAmount(amountToken, parsedTrailingAmount)) {
                  const char prev = itemNameOut[digitStart - 1];
                  const bool explicitSeparator =
                      std::isspace((unsigned char)prev) || prev == 'x' ||
                      prev == 'X' || prev == '*';
                  const bool compactSuffix =
                      std::isalpha((unsigned char)prev) &&
                      parsedTrailingAmount <= 20;
                  if (explicitSeparator || compactSuffix) {
                    size_t baseEnd = digitStart;
                    if ((prev == 'x' || prev == 'X' || prev == '*') &&
                        baseEnd > 0) {
                      --baseEnd;
                    }
                    std::string baseName = TrimCopy(itemNameOut.substr(0, baseEnd));
                    if (!baseName.empty()) {
                      itemNameOut = baseName;
                      itemAmountOut = parsedTrailingAmount;
                      return true;
                    }
                  }
                }
              }

              return !itemNameOut.empty();
            };

            size_t firstSplitPos = payload.find('@');
            if (firstSplitPos == std::string::npos) {
              return parseItemSpec(payload, itemOut, amountOut);
            }

            size_t lastSplitPos = payload.find_last_of('@');
            if (firstSplitPos == lastSplitPos) {
              std::string left = TrimCopy(payload.substr(0, firstSplitPos));
              std::string right = TrimCopy(payload.substr(firstSplitPos + 1));
              if (left.empty() || right.empty()) {
                return false;
              }
              targetOut = left;
              return parseItemSpec(right, itemOut, amountOut);
            }

            std::string left = TrimCopy(payload.substr(0, firstSplitPos));
            std::string middle =
                TrimCopy(payload.substr(firstSplitPos + 1,
                                        lastSplitPos - firstSplitPos - 1));
            std::string right = TrimCopy(payload.substr(lastSplitPos + 1));
            if (left.empty() || middle.empty() || right.empty()) {
              return false;
            }

            int explicitAmount = 0;
            if (!parsePositiveAmount(right, explicitAmount)) {
              targetOut = left;
              std::string mergedItem =
                  TrimCopy(payload.substr(firstSplitPos + 1));
              return parseItemSpec(mergedItem, itemOut, amountOut);
            }

            targetOut = left;
            int parsedFromSpec = 1;
            if (!parseItemSpec(middle, itemOut, parsedFromSpec)) {
              return false;
            }
            amountOut = explicitAmount;
            return amountOut > 0;
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
            Character *actorCharacter = nullptr;
            Ogre::Vector3 actorPosition = Ogre::Vector3::ZERO;
            bool actorPositionValid = false;
            hand actorIndoorsHandle;
            bool actorIsIndoors = false;
            int actorFloor = 0;
            if (thisptr) {
              const ogre_unordered_set<Character *>::type &chars =
                  thisptr->getCharacterUpdateList();
              if (actorHandle.isValid() && actorHandle.serial != 0) {
                for (auto it = chars.begin(); it != chars.end(); ++it) {
                  Character *candidate = *it;
                  if (!candidate || (uintptr_t)candidate < 0x1000) {
                    continue;
                  }
                  unsigned int candidateSerial = 0;
                  try {
                    candidateSerial = candidate->getHandle().serial;
                  } catch (...) {
                    candidateSerial = 0;
                  }
                  if (candidateSerial == actorHandle.serial) {
                    actorCharacter = candidate;
                    break;
                  }
                }
              }
              if (!actorCharacter && actorHandle.isValid()) {
                try {
                  Character *directActor = actorHandle.getCharacter();
                  if (directActor && (uintptr_t)directActor > 0x1000) {
                    actorCharacter = directActor;
                  }
                } catch (...) {
                  actorCharacter = nullptr;
                }
              }
              if (actorCharacter && (uintptr_t)actorCharacter > 0x1000) {
                try {
                  actorPosition = actorCharacter->getPosition();
                  actorPositionValid = true;
                } catch (...) {
                  actorPositionValid = false;
                }
                try {
                  actorIndoorsHandle = actorCharacter->isIndoors();
                  actorIsIndoors =
                      actorIndoorsHandle.isValid() && !actorIndoorsHandle.isNull();
                } catch (...) {
                  actorIsIndoors = false;
                }
                try {
                  actorFloor = actorCharacter->getFloor();
                } catch (...) {
                  actorFloor = 0;
                }
              }
              for (auto it = chars.begin(); it != chars.end(); ++it) {
                Character *candidate = *it;
                if (!candidate || (uintptr_t)candidate < 0x1000) {
                  continue;
                }
                hand candidateHandle = hand();
                unsigned int candidateSerial = 0;
                try {
                  candidateHandle = candidate->getHandle();
                  candidateSerial = candidateHandle.serial;
                } catch (...) {
                  candidateHandle = hand();
                  candidateSerial = 0;
                }
                if (!candidateHandle.isValid() || candidateSerial == 0) {
                  continue;
                }
                if (actorHandle.isValid() && candidateSerial == actorHandle.serial) {
                  continue;
                }

                int score = 0;
                if (hasSerial && candidateSerial == wantedSerial) {
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
                if (score > 0 && actorCharacter &&
                    (uintptr_t)actorCharacter > 0x1000 && actorPositionValid) {
                  float distance = -1.0f;
                  try {
                    distance = candidate->getPosition().distance(actorPosition);
                  } catch (...) {
                    distance = -1.0f;
                  }
                  if (distance >= 0.0f) {
                    if (distance <= 8.0f) {
                      score += 260;
                    } else if (distance <= 25.0f) {
                      score += 180;
                    } else if (distance <= 60.0f) {
                      score += 110;
                    } else if (distance <= 120.0f) {
                      score += 40;
                    } else if (!hasSerial && distance >= 300.0f) {
                      score -= 100;
                    }
                  }

                  bool candidateIsIndoors = false;
                  hand candidateIndoorsHandle;
                  try {
                    candidateIndoorsHandle = candidate->isIndoors();
                    candidateIsIndoors = candidateIndoorsHandle.isValid() &&
                                         !candidateIndoorsHandle.isNull();
                  } catch (...) {
                    candidateIsIndoors = false;
                  }
                  if (candidateIsIndoors == actorIsIndoors) {
                    score += 24;
                  }
                  if (candidateIsIndoors && actorIsIndoors &&
                      candidateIndoorsHandle.serial == actorIndoorsHandle.serial) {
                    score += 90;
                  } else if (candidateIsIndoors != actorIsIndoors) {
                    score -= 30;
                  }

                  int candidateFloor = 0;
                  bool candidateFloorValid = true;
                  try {
                    candidateFloor = candidate->getFloor();
                  } catch (...) {
                    candidateFloorValid = false;
                  }
                  if (candidateFloorValid) {
                    if (candidateFloor == actorFloor) {
                      score += 20;
                    } else {
                      score -= 6;
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

            // Fallback: nearby sphere query catches dead/KO actors that may not
            // be present in the regular update set.
            if (thisptr && (!bestMatch || bestScore < 1000)) {
              Character *anchorCharacter = actorCharacter;
              if ((!anchorCharacter || (uintptr_t)anchorCharacter <= 0x1000) &&
                  thisptr->player &&
                  thisptr->player->playerCharacters.size() > 0 &&
                  thisptr->player->playerCharacters[0]) {
                anchorCharacter = thisptr->player->playerCharacters[0];
              }

              if (anchorCharacter && (uintptr_t)anchorCharacter > 0x1000) {
                Ogre::Vector3 anchorPos = Ogre::Vector3::ZERO;
                bool anchorPosValid = false;
                try {
                  anchorPos = anchorCharacter->getPosition();
                  anchorPosValid = true;
                } catch (...) {
                  anchorPosValid = false;
                }

                if (anchorPosValid) {
                  lektor<RootObject *> nearby;
                  try {
                    thisptr->getCharactersWithinSphere(nearby, anchorPos, 600.0f,
                                                       0.0f, 0.0f, 16, 0,
                                                       anchorCharacter);
                  } catch (...) {
                    nearby.clear();
                  }

                  for (uint32_t i = 0; i < nearby.size(); ++i) {
                    Character *candidate = (Character *)nearby.stuff[i];
                    if (!candidate || (uintptr_t)candidate < 0x1000) {
                      continue;
                    }

                    hand candidateHandle;
                    unsigned int candidateSerial = 0;
                    try {
                      candidateHandle = candidate->getHandle();
                      candidateSerial = candidateHandle.serial;
                    } catch (...) {
                      candidateHandle = hand();
                      candidateSerial = 0;
                    }
                    if (!candidateHandle.isValid() || candidateSerial == 0) {
                      continue;
                    }
                    if (actorHandle.isValid() &&
                        candidateSerial == actorHandle.serial) {
                      continue;
                    }

                    int score = 0;
                    if (hasSerial && candidateSerial == wantedSerial) {
                      score = 1200;
                    } else if (!tokenLow.empty()) {
                      std::string candidateName = candidate->getName();
                      std::string candidateLow = candidateName;
                      std::transform(candidateLow.begin(), candidateLow.end(),
                                     candidateLow.begin(), ::tolower);
                      if (candidateLow == tokenLow) {
                        score = 520;
                      } else if (candidateLow.find(tokenLow) == 0) {
                        score = 340;
                      } else if (candidateLow.find(tokenLow) != std::string::npos) {
                        score = 200;
                      } else if (!candidate->displayName.empty()) {
                        std::string displayLow = candidate->displayName;
                        std::transform(displayLow.begin(), displayLow.end(),
                                       displayLow.begin(), ::tolower);
                        if (displayLow == tokenLow) {
                          score = 280;
                        } else if (displayLow.find(tokenLow) !=
                                   std::string::npos) {
                          score = 160;
                        }
                      }
                    }
                    if (score <= 0) {
                      continue;
                    }

                    try {
                      float distance = candidate->getPosition().distance(anchorPos);
                      if (distance <= 8.0f) {
                        score += 320;
                      } else if (distance <= 25.0f) {
                        score += 220;
                      } else if (distance <= 60.0f) {
                        score += 130;
                      } else if (distance <= 120.0f) {
                        score += 55;
                      } else if (!hasSerial && distance >= 300.0f) {
                        score -= 120;
                      }
                    } catch (...) {
                    }

                    if (score > bestScore) {
                      bestScore = score;
                      bestMatch = candidate;
                      if (score >= 1200) {
                        break;
                      }
                    }
                  }
                }
              }
            }

            if (!bestMatch && !hasSerial &&
                (tokenLow == "lead" || tokenLow == "leader")) {
              hand playerHandle = resolvePlayerHandle();
              if (playerHandle.isValid() && playerHandle.serial != 0) {
                return playerHandle;
              }
            }
            if (bestMatch && bestScore > 0) {
              hand bestHandle = hand();
              try {
                bestHandle = bestMatch->getHandle();
              } catch (...) {
                bestHandle = hand();
              }
              if (bestHandle.isValid() && bestHandle.serial != 0) {
                return bestHandle;
              }
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
            std::vector<std::pair<std::string, int>> giveItemRequests;

            std::string trimmedGiveItemArg = TrimCopy(actionArgument);
            bool listSyntaxRequested =
                trimmedGiveItemArg.find(',') != std::string::npos ||
                trimmedGiveItemArg.find(';') != std::string::npos;

            if (listSyntaxRequested) {
              std::string listPayload = trimmedGiveItemArg;
              size_t firstAt = listPayload.find('@');
              if (firstAt != std::string::npos) {
                std::string targetToken = TrimCopy(listPayload.substr(0, firstAt));
                std::string itemsToken = TrimCopy(listPayload.substr(firstAt + 1));
                if (!targetToken.empty() && !itemsToken.empty()) {
                  giveItemTargetToken = targetToken;
                  listPayload = itemsToken;
                }
              }

              std::vector<std::string> parts;
              std::string current = "";
              std::string listPayloadLower = ToLowerAsciiCopy(listPayload);
              for (size_t i = 0; i < listPayload.size(); ++i) {
                char ch = listPayload[i];
                bool shouldSplit = (ch == ',' || ch == ';');
                size_t splitAdvance = 0;
                if (!shouldSplit && i + 5 <= listPayloadLower.size() &&
                    listPayloadLower.substr(i, 5) == " and ") {
                  shouldSplit = true;
                  splitAdvance = 4;
                }
                if (shouldSplit) {
                  std::string part = TrimCopy(current);
                  if (!part.empty()) {
                    parts.push_back(part);
                  }
                  current.clear();
                  i += splitAdvance;
                  continue;
                }
                current.push_back(ch);
              }
              std::string tail = TrimCopy(current);
              if (!tail.empty()) {
                parts.push_back(tail);
              }

              for (size_t i = 0; i < parts.size(); ++i) {
                std::string parsedTargetToken = "";
                std::string parsedItemName = "";
                int parsedAmount = 1;
                if (!parseGiveItemPayload(parts[i], parsedTargetToken, parsedItemName,
                                          parsedAmount)) {
                  Log("HOOK_MSG_PROC: GIVE_ITEM list part parse failed part='" +
                      parts[i] + "'");
                  continue;
                }
                if (!parsedTargetToken.empty() && giveItemTargetToken.empty()) {
                  giveItemTargetToken = parsedTargetToken;
                }
                if (!parsedItemName.empty() && parsedAmount > 0) {
                  giveItemRequests.push_back(
                      std::make_pair(parsedItemName, parsedAmount));
                }
              }
            }

            if (giveItemRequests.empty()) {
              std::string iName = "";
              int giveItemAmount = 1;
              if (!parseGiveItemPayload(actionArgument, giveItemTargetToken, iName,
                                        giveItemAmount)) {
                Log("HOOK_MSG_PROC: GIVE_ITEM ignored; invalid payload '" +
                    actionArgument + "'");
                continue;
              }
              giveItemRequests.push_back(std::make_pair(iName, giveItemAmount));
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
            } else if (actionActorResolved && actionActor) {
              std::string inferredRecipientSource = "";
              Dialogue *dialogueHint = nullptr;
              try {
                dialogueHint = actionActor->dialogue;
              } catch (...) {
                dialogueHint = nullptr;
              }
              Character *inferredRecipient = ResolveDialogueListenerForSpeech(
                  actionActor, dialogueHint, inferredRecipientSource);
              if (inferredRecipient && (uintptr_t)inferredRecipient > 0x1000 &&
                  inferredRecipient != actionActor) {
                try {
                  giveItemTarget = inferredRecipient->getHandle();
                } catch (...) {
                  giveItemTarget = hand();
                }
                if (giveItemTarget.isValid()) {
                  giveItemTargetToken = inferredRecipient->getName();
                  Log("HOOK_MSG_PROC: GIVE_ITEM inferred recipient '" +
                      giveItemTargetToken + "' source=" +
                      inferredRecipientSource + " serial=" +
                      ToString((unsigned int)giveItemTarget.serial));
                }
              }
            }
            EnterCriticalSection(&g_uiMutex);
            for (size_t i = 0; i < giveItemRequests.size(); ++i) {
              QueuedAction act;
              act.type = ACT_GIVE_ITEM;
              act.actor = targetHand;
              act.target = giveItemTarget;
              act.message = giveItemRequests[i].first;
              act.taskValue = giveItemRequests[i].second;
              g_uiActionQueue.push_back(act);
            }
            LeaveCriticalSection(&g_uiMutex);
            std::string giveItemSummary = "";
            for (size_t i = 0; i < giveItemRequests.size(); ++i) {
              if (i > 0) {
                giveItemSummary += ", ";
              }
              giveItemSummary += giveItemRequests[i].first + " x" +
                                 ToString(giveItemRequests[i].second);
            }
            Log("HOOK_MSG_PROC: GIVE_ITEM queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " count=" +
                ToString((int)giveItemRequests.size()) + " items=[" +
                giveItemSummary + "] target='" + giveItemTargetToken +
                "' target_serial=" + ToString((unsigned int)giveItemTarget.serial));
          } else if (actionCommand == "TAKE_ITEM") {
            std::string takeItemTargetToken = "";
            std::vector<std::pair<std::string, int>> takeItemRequests;

            auto parseLegacyTakeItemPayload =
                [&](const std::string &rawPayload, std::string &targetOut,
                    std::string &itemOut, int &amountOut) -> bool {
              targetOut.clear();
              itemOut.clear();
              amountOut = 1;
              std::string payload = TrimCopy(rawPayload);
              if (payload.empty()) {
                return false;
              }

              auto canonicalLootQueryFromSuffix =
                  [&](const std::string &rawSuffix) -> std::string {
                std::string suffix = ToLowerAsciiCopy(TrimCopy(rawSuffix));
                if (suffix == "equip" || suffix == "equipment" ||
                    suffix == "gear" || suffix == "armor" ||
                    suffix == "armour") {
                  return "equipment";
                }
                if (suffix == "all" || suffix == "everything" ||
                    suffix == "inventory" || suffix == "items" ||
                    suffix == "loot") {
                  return "all";
                }
                if (suffix == "weapon" || suffix == "weapons") {
                  return "weapon";
                }
                return "";
              };

              // Handle malformed payloads such as "Blue Eyesequipment"
              // where target and loot keyword are concatenated.
              {
                auto trimTargetPrefixToken = [](std::string value) -> std::string {
                  while (!value.empty()) {
                    char tail = value.back();
                    if (tail == '-' || tail == '_' || tail == ':' || tail == '.' ||
                        tail == ',' || tail == ';' || tail == '/' || tail == '\\' ||
                        tail == '|' || std::isspace((unsigned char)tail)) {
                      value.erase(value.size() - 1, 1);
                      continue;
                    }
                    break;
                  }
                  while (!value.empty()) {
                    char head = value.front();
                    if (head == '-' || head == '_' || head == ':' || head == '.' ||
                        head == ',' || head == ';' || head == '/' || head == '\\' ||
                        head == '|' || std::isspace((unsigned char)head)) {
                      value.erase(0, 1);
                      continue;
                    }
                    break;
                  }
                  return TrimCopy(value);
                };
                auto extractPrefixByLootSuffix =
                    [&](const std::string &rawValue, const std::string &suffix,
                        std::string &prefixOut) -> bool {
                  prefixOut.clear();
                  std::string loweredValue = ToLowerAsciiCopy(rawValue);
                  std::string suffixLower = ToLowerAsciiCopy(suffix);
                  if (loweredValue.length() > suffixLower.length() &&
                      loweredValue.compare(loweredValue.length() - suffixLower.length(),
                                           suffixLower.length(), suffixLower) == 0) {
                    prefixOut =
                        rawValue.substr(0, rawValue.length() - suffixLower.length());
                    return true;
                  }
                  const char *joiners[] = {"/", "\\", "-", "_", ":", "|"};
                  for (size_t joinIdx = 0; joinIdx < sizeof(joiners) / sizeof(joiners[0]);
                       ++joinIdx) {
                    std::string joined = std::string(joiners[joinIdx]) + suffixLower;
                    if (loweredValue.length() <= joined.length()) {
                      continue;
                    }
                    if (loweredValue.compare(loweredValue.length() - joined.length(),
                                             joined.length(), joined) != 0) {
                      continue;
                    }
                    prefixOut = rawValue.substr(0, rawValue.length() - joined.length());
                    return true;
                  }
                  return false;
                };
                auto extractSuffixByLootPrefix =
                    [&](const std::string &rawValue, const std::string &prefix,
                        std::string &suffixOut) -> bool {
                  suffixOut.clear();
                  std::string loweredValue = ToLowerAsciiCopy(rawValue);
                  std::string prefixLower = ToLowerAsciiCopy(prefix);
                  if (loweredValue.length() > prefixLower.length() &&
                      loweredValue.compare(0, prefixLower.length(), prefixLower) ==
                          0) {
                    suffixOut = rawValue.substr(prefixLower.length());
                    return true;
                  }
                  const char *joiners[] = {"/", "\\", "-", "_", ":", "|"};
                  for (size_t joinIdx = 0; joinIdx < sizeof(joiners) / sizeof(joiners[0]);
                       ++joinIdx) {
                    std::string joined = prefixLower + std::string(joiners[joinIdx]);
                    if (loweredValue.length() <= joined.length()) {
                      continue;
                    }
                    if (loweredValue.compare(0, joined.length(), joined) != 0) {
                      continue;
                    }
                    suffixOut = rawValue.substr(joined.length());
                    return true;
                  }
                  return false;
                };

                const std::string loweredPayload = ToLowerAsciiCopy(payload);
                const char *suffixes[] = {"equipment", "equip",  "gear",
                                          "armor",     "armour", "all",
                                          "everything","inventory","items",
                                          "loot",      "weapon", "weapons"};
                for (size_t suffixIdx = 0; suffixIdx < sizeof(suffixes) / sizeof(suffixes[0]);
                     ++suffixIdx) {
                  const std::string suffix = suffixes[suffixIdx];
                  std::string prefix = "";
                  if (!extractPrefixByLootSuffix(payload, suffix, prefix)) {
                    continue;
                  }
                  prefix = trimTargetPrefixToken(prefix);
                  if (prefix.empty()) {
                    continue;
                  }
                  std::string canonicalQuery = canonicalLootQueryFromSuffix(suffix);
                  if (canonicalQuery.empty()) {
                    continue;
                  }

                  hand candidateTarget = resolveActionTargetHand(prefix, targetHand);
                  if (!candidateTarget.isValid() && canonicalQuery == "all") {
                    // Support malformed variants like "Blue Eyesequipment/all".
                    const char *nestedSuffixes[] = {"equipment", "equip", "gear",
                                                    "armor", "armour", "weapon",
                                                    "weapons"};
                    for (size_t nestedIdx = 0;
                         nestedIdx < sizeof(nestedSuffixes) / sizeof(nestedSuffixes[0]);
                         ++nestedIdx) {
                      std::string nestedPrefix = "";
                      if (!extractPrefixByLootSuffix(prefix, nestedSuffixes[nestedIdx],
                                                     nestedPrefix)) {
                        continue;
                      }
                      nestedPrefix = trimTargetPrefixToken(nestedPrefix);
                      if (nestedPrefix.empty()) {
                        continue;
                      }
                      hand nestedTarget =
                          resolveActionTargetHand(nestedPrefix, targetHand);
                      if (!nestedTarget.isValid()) {
                        continue;
                      }
                      targetOut = nestedPrefix;
                      itemOut = canonicalQuery;
                      amountOut = 1;
                      return true;
                    }
                  }

                  if (candidateTarget.isValid()) {
                    targetOut = prefix;
                    itemOut = canonicalQuery;
                    amountOut = 1;
                    return true;
                  }

                  // If this looks like a named target intent, don't downgrade to
                  // taking items from player inventory.
                  if (prefix.find(' ') != std::string::npos) {
                    return false;
                  }
                }

                // Support malformed variants like "equipmentBlue Eyes",
                // where loot keyword is concatenated before target.
                for (size_t suffixIdx = 0; suffixIdx < sizeof(suffixes) / sizeof(suffixes[0]);
                     ++suffixIdx) {
                  const std::string suffix = suffixes[suffixIdx];
                  std::string trailingTarget = "";
                  if (!extractSuffixByLootPrefix(payload, suffix, trailingTarget)) {
                    continue;
                  }
                  trailingTarget = trimTargetPrefixToken(trailingTarget);
                  if (trailingTarget.empty()) {
                    continue;
                  }

                  std::string canonicalQuery = canonicalLootQueryFromSuffix(suffix);
                  if (canonicalQuery.empty()) {
                    continue;
                  }

                  hand candidateTarget =
                      resolveActionTargetHand(trailingTarget, targetHand);
                  if (candidateTarget.isValid()) {
                    targetOut = trailingTarget;
                    itemOut = canonicalQuery;
                    amountOut = 1;
                    return true;
                  }

                  // If this looks like a named target intent, don't downgrade to
                  // taking items from player inventory.
                  if (trailingTarget.find(' ') != std::string::npos) {
                    return false;
                  }
                }
              }

              size_t firstAtPos = payload.find('@');
              if (firstAtPos != std::string::npos &&
                  firstAtPos == payload.find_last_of('@')) {
                std::string leftAtToken = TrimCopy(payload.substr(0, firstAtPos));
                std::string rightAtToken =
                    TrimCopy(payload.substr(firstAtPos + 1));
                if (!leftAtToken.empty() && !rightAtToken.empty()) {
                  bool rightIsDigits = true;
                  for (size_t i = 0; i < rightAtToken.size(); ++i) {
                    unsigned char ch = (unsigned char)rightAtToken[i];
                    if (ch < '0' || ch > '9') {
                      rightIsDigits = false;
                      break;
                    }
                  }
                  if (rightIsDigits) {
                    hand leftAsTarget =
                        resolveActionTargetHand(leftAtToken, targetHand);
                    hand rightAsTarget =
                        resolveActionTargetHand(rightAtToken, targetHand);
                    if (!leftAsTarget.isValid() && !rightAsTarget.isValid()) {
                      int parsedAmount = atoi(rightAtToken.c_str());
                      if (parsedAmount > 0) {
                        targetOut.clear();
                        itemOut = leftAtToken;
                        amountOut = parsedAmount;
                        return true;
                      }
                    }
                  }
                }
              }

              // Primary parser handles target@item and quantity formats.
              if (parseGiveItemPayload(payload, targetOut, itemOut, amountOut)) {
                if (!targetOut.empty()) {
                  hand parsedTarget = resolveActionTargetHand(targetOut, targetHand);
                  if (!parsedTarget.isValid()) {
                    targetOut.clear();
                    itemOut.clear();
                    amountOut = 1;
                  }
                }
              }
              if (!itemOut.empty() || !targetOut.empty()) {
                if (itemOut.empty() && !targetOut.empty()) {
                  itemOut = "equipment";
                  amountOut = 1;
                }
                return !itemOut.empty() && amountOut > 0;
              }

              // Target-only shorthand means "loot target equipment".
              hand shorthandTarget = resolveActionTargetHand(payload, targetHand);
              if (shorthandTarget.isValid()) {
                targetOut = payload;
                itemOut = "equipment";
                amountOut = 1;
                return true;
              }

              // Legacy fallback supports item@target and target@item.
              size_t splitPos = payload.find('@');
              if (splitPos == std::string::npos) {
                return false;
              }
              std::string leftToken = TrimCopy(payload.substr(0, splitPos));
              std::string rightToken = TrimCopy(payload.substr(splitPos + 1));
              if (leftToken.empty() || rightToken.empty()) {
                return false;
              }
              hand leftTarget = resolveActionTargetHand(leftToken, targetHand);
              hand rightTarget = resolveActionTargetHand(rightToken, targetHand);
              std::string itemSpec = "";
              if (leftTarget.isValid() &&
                  (!rightTarget.isValid() || rightToken.empty())) {
                targetOut = leftToken;
                itemSpec = rightToken;
              } else if (rightTarget.isValid() &&
                         (!leftTarget.isValid() || leftToken.empty())) {
                targetOut = rightToken;
                itemSpec = leftToken;
              } else if (leftTarget.isValid() && rightTarget.isValid()) {
                targetOut = leftToken;
                itemSpec = rightToken;
              } else {
                return false;
              }

              if (itemSpec.empty()) {
                itemOut = "equipment";
                amountOut = 1;
                return true;
              }

              std::string ignoredTargetToken = "";
              if (!parseGiveItemPayload(itemSpec, ignoredTargetToken, itemOut,
                                        amountOut)) {
                itemOut = TrimCopy(itemSpec);
                amountOut = 1;
              }
              if (itemOut.empty()) {
                itemOut = "equipment";
                amountOut = 1;
              }
              return amountOut > 0;
            };

            std::string trimmedTakeItemArg = TrimCopy(actionArgument);
            bool listSyntaxRequested =
                trimmedTakeItemArg.find(',') != std::string::npos ||
                trimmedTakeItemArg.find(';') != std::string::npos;

            if (listSyntaxRequested) {
              std::string listPayload = trimmedTakeItemArg;
              size_t firstAt = listPayload.find('@');
              if (firstAt != std::string::npos) {
                std::string targetToken = TrimCopy(listPayload.substr(0, firstAt));
                std::string itemsToken = TrimCopy(listPayload.substr(firstAt + 1));
                hand resolvedPrefixTarget =
                    resolveActionTargetHand(targetToken, targetHand);
                if (!targetToken.empty() && !itemsToken.empty() &&
                    resolvedPrefixTarget.isValid()) {
                  takeItemTargetToken = targetToken;
                  listPayload = itemsToken;
                }
              }

              std::vector<std::string> parts;
              std::string current = "";
              std::string listPayloadLower = ToLowerAsciiCopy(listPayload);
              for (size_t i = 0; i < listPayload.size(); ++i) {
                char ch = listPayload[i];
                bool shouldSplit = (ch == ',' || ch == ';');
                size_t splitAdvance = 0;
                if (!shouldSplit && i + 5 <= listPayloadLower.size() &&
                    listPayloadLower.substr(i, 5) == " and ") {
                  shouldSplit = true;
                  splitAdvance = 4;
                }
                if (shouldSplit) {
                  std::string part = TrimCopy(current);
                  if (!part.empty()) {
                    parts.push_back(part);
                  }
                  current.clear();
                  i += splitAdvance;
                  continue;
                }
                current.push_back(ch);
              }
              std::string tail = TrimCopy(current);
              if (!tail.empty()) {
                parts.push_back(tail);
              }

              for (size_t i = 0; i < parts.size(); ++i) {
                std::string parsedTargetToken = "";
                std::string parsedItemName = "";
                int parsedAmount = 1;
                if (!parseLegacyTakeItemPayload(parts[i], parsedTargetToken,
                                                parsedItemName, parsedAmount)) {
                  Log("HOOK_MSG_PROC: TAKE_ITEM list part parse failed part='" +
                      parts[i] + "'");
                  continue;
                }
                if (!parsedTargetToken.empty() && takeItemTargetToken.empty()) {
                  takeItemTargetToken = parsedTargetToken;
                }
                if (!parsedItemName.empty() && parsedAmount > 0) {
                  takeItemRequests.push_back(
                      std::make_pair(parsedItemName, parsedAmount));
                }
              }
            }

            if (takeItemRequests.empty()) {
              std::string iName = "";
              int takeItemAmount = 1;
              std::string parsedTargetToken = "";
              if (!parseLegacyTakeItemPayload(actionArgument, parsedTargetToken,
                                              iName, takeItemAmount)) {
                Log("HOOK_MSG_PROC: TAKE_ITEM ignored; invalid payload '" +
                    actionArgument + "'");
                continue;
              }
              if (!parsedTargetToken.empty()) {
                takeItemTargetToken = parsedTargetToken;
              }
              takeItemRequests.push_back(std::make_pair(iName, takeItemAmount));
            }

            hand takeItemTarget = hand();
            if (!takeItemTargetToken.empty()) {
              takeItemTarget =
                  resolveActionTargetHand(takeItemTargetToken, targetHand);
              if (!takeItemTarget.isValid() || takeItemTarget.serial == 0) {
                Log("HOOK_MSG_PROC: TAKE_ITEM target unresolved at parse-time '" +
                    takeItemTargetToken +
                    "'; deferring to runtime token resolution");
                takeItemTarget = hand();
              }
            }

            EnterCriticalSection(&g_uiMutex);
            for (size_t i = 0; i < takeItemRequests.size(); ++i) {
              QueuedAction act;
              act.type = ACT_TAKE_ITEM;
              act.actor = targetHand;
              act.target = takeItemTarget;
              act.message = takeItemRequests[i].first;
              act.targetToken = takeItemTargetToken;
              act.taskValue = takeItemRequests[i].second;
              g_uiActionQueue.push_back(act);
            }
            LeaveCriticalSection(&g_uiMutex);

            std::string takeItemSummary = "";
            for (size_t i = 0; i < takeItemRequests.size(); ++i) {
              if (i > 0) {
                takeItemSummary += ", ";
              }
              takeItemSummary += takeItemRequests[i].first + " x" +
                                 ToString(takeItemRequests[i].second);
            }
            Log("HOOK_MSG_PROC: TAKE_ITEM queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " count=" +
                ToString((int)takeItemRequests.size()) + " items=[" +
                takeItemSummary + "] target='" + takeItemTargetToken +
                "' target_serial=" + ToString((unsigned int)takeItemTarget.serial));
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
          } else if (actionCommand == "FORCE_DRINK") {
            if (shouldSkipSpeakerBoundAction(actionCommand)) {
              continue;
            }
            std::string targetToken = "";
            std::string drinkItemName = "";
            if (!parseForceDrinkPayload(actionArgument, targetToken,
                                        drinkItemName)) {
              Log("HOOK_MSG_PROC: FORCE_DRINK ignored; invalid payload '" +
                  actionArgument + "'");
              continue;
            }
            hand forceDrinkTarget =
                resolveActionTargetHand(targetToken, targetHand);
            if (!forceDrinkTarget.isValid()) {
              Log("HOOK_MSG_PROC: FORCE_DRINK ignored; target unresolved '" +
                  targetToken + "'");
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_FORCE_DRINK;
            act.actor = targetHand;
            act.target = forceDrinkTarget;
            act.message = drinkItemName;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: FORCE_DRINK queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " target='" +
                targetToken + "' target_serial=" +
                ToString((unsigned int)forceDrinkTarget.serial) + " item='" +
                drinkItemName + "'");
          } else if (actionCommand == "PICKUP_NPC") {
            if (shouldSkipSpeakerBoundAction(actionCommand)) {
              continue;
            }
            std::string targetToken = TrimCopy(actionArgument);
            if (targetToken.empty()) {
              Log("HOOK_MSG_PROC: PICKUP_NPC ignored; empty target payload");
              continue;
            }
            hand pickupTarget = resolveActionTargetHand(targetToken, targetHand);
            if (!pickupTarget.isValid()) {
              Log("HOOK_MSG_PROC: PICKUP_NPC ignored; target unresolved '" +
                  targetToken + "'");
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_PICKUP_NPC;
            act.actor = targetHand;
            act.target = pickupTarget;
            act.message = targetToken;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: PICKUP_NPC queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " target='" +
                targetToken + "' target_serial=" +
                ToString((unsigned int)pickupTarget.serial));
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
              Log("HOOK_MSG_PROC: REMOVE_LIMB target unresolved at parse-time '" +
                  targetToken +
                  "'; deferring to runtime token resolution");
              limbTarget = hand();
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_REMOVE_LIMB;
            act.actor = targetHand;
            act.target = limbTarget;
            act.message = targetToken;
            act.targetToken = targetToken;
            act.taskValue = limbCode;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: REMOVE_LIMB queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " target='" +
                targetToken + "' target_serial=" +
                ToString((unsigned int)limbTarget.serial) + " limb_code=" +
                ToString(limbCode));
          } else if (actionCommand == "CUT_HORNS") {
            if (shouldSkipSpeakerBoundAction(actionCommand)) {
              continue;
            }
            std::string targetToken = "";
            if (!parseSingleTargetPayload(actionArgument, targetToken)) {
              Log("HOOK_MSG_PROC: CUT_HORNS ignored; invalid payload '" +
                  actionArgument + "'");
              continue;
            }
            hand hornTarget = resolveActionTargetHand(targetToken, targetHand);
            if (!hornTarget.isValid()) {
              Log("HOOK_MSG_PROC: CUT_HORNS target unresolved at parse-time '" +
                  targetToken +
                  "'; deferring to runtime token resolution");
              hornTarget = hand();
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_CUT_HORNS;
            act.actor = targetHand;
            act.target = hornTarget;
            act.message = targetToken;
            act.targetToken = targetToken;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: CUT_HORNS queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " target='" +
                targetToken + "' target_serial=" +
                ToString((unsigned int)hornTarget.serial));
          } else if (actionCommand == "KNOCKOUT") {
            if (shouldSkipSpeakerBoundAction(actionCommand)) {
              continue;
            }
            std::string targetToken = TrimCopy(actionArgument);
            if (targetToken.empty()) {
              Log("HOOK_MSG_PROC: KNOCKOUT ignored; empty target payload");
              continue;
            }
            hand knockoutTarget = hand();
            if (knockoutTargetMatchesActorName(targetToken)) {
              knockoutTarget = targetHand;
              Log("HOOK_MSG_PROC: KNOCKOUT self-target resolved from actor name '" +
                  targetToken + "' actor_serial=" +
                  ToString((unsigned int)targetHand.serial));
            } else {
              knockoutTarget = resolveActionTargetHand(targetToken, targetHand);
            }
            if (!knockoutTarget.isValid()) {
              Log("HOOK_MSG_PROC: KNOCKOUT ignored; target unresolved '" +
                  targetToken + "'");
              continue;
            }
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_KNOCKOUT;
            act.actor = targetHand;
            act.target = knockoutTarget;
            act.message = targetToken;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("HOOK_MSG_PROC: KNOCKOUT queued actor_serial=" +
                ToString((unsigned int)targetHand.serial) + " target='" +
                targetToken + "' target_serial=" +
                ToString((unsigned int)knockoutTarget.serial));
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
        std::string utteranceId = ExtractTrailingUtteranceId(bubbleContent);
        int ttsDurationMs = ExtractTrailingTtsDurationMs(bubbleContent);
        std::string ttsHash = ExtractTrailingTtsHash(bubbleContent);
        std::string talkTargetToken = ExtractTalkTargetToken(bubbleContent);
        bubbleContent = StripLeakedSpeechSerialTokens(bubbleContent);
        bubbleContent = StripDanglingTrailingClosingBrackets(bubbleContent);
        const bool hadStructuredMessage = !structuredMessage.empty();
        const bool hadTtsMetadata = !ttsHash.empty() || ttsDurationMs > 0;
        const bool hadAiDeliveryMetadata = !utteranceId.empty();
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
        if (bubbleContent.empty() && !utteranceId.empty()) {
          PostSpeechDeliveryState(utteranceId, "cancelled");
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
            tc = ResolvePreferredPlayerSpeakerForCurrentTalk(thisptr);
            if (!tc && thisptr->player && thisptr->player->playerCharacters.size() > 0) {
              tc = ResolveFirstAliveConsciousPlayerCharacter(thisptr);
            }
          }
          if (isNPCSay && tc && (uintptr_t)tc > 0x1000 && tc->isPlayerCharacter()) {
            // Keep explicit stream speakers (autochat/rechat) on their own actor.
            // Only retarget legacy fallback cases where no speaker was resolved.
            if (!speakerResolvedFromHeader) {
              Character *talkTarget =
                  ResolveCharacterFromHandSafe(thisptr, g_talkTargetHand);
              if (talkTarget && (uintptr_t)talkTarget > 0x1000 &&
                  !talkTarget->isPlayerCharacter()) {
                Log("CHAT_SPEAKER: NPC_SAY retargeted from player " +
                    tc->getName() + " to talk target " + talkTarget->getName());
                tc = talkTarget;
                targetHand = talkTarget->getHandle();
              }
            } else {
              Log("CHAT_SPEAKER: NPC_SAY keeping explicit player speaker " +
                  tc->getName());
            }
          }

          Log("HOOK_MSG_PROC: Queuing SAY for " +
              (tc ? tc->getName() : "Unknown") + ": " + bubbleContent);

          if (tc && (uintptr_t)tc > 0x1000) {
            SyncInventoryForCharacter(tc, true,
                                      isNPCSay ? "dialogue_npc" : "dialogue_player");
          }

          bool trackAsNonAiDialogue =
              !hadStructuredMessage && !hadTtsMetadata && !hadAiDeliveryMetadata;
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

          if (g_enableRegularDialogueCapture && trackAsNonAiDialogue &&
              !duplicateDialogueLine) {
            std::string listenerName = "None";
            std::string listenerFaction = "None";
            unsigned int listenerSerial = 0;
            Character *listener = nullptr;
            if (tc && (uintptr_t)tc > 0x1000) {
              if (isPlayerSay) {
                listener = ResolveCharacterFromHandSafe(thisptr, g_talkTargetHand);
                if ((!listener || (uintptr_t)listener <= 0x1000 || listener == tc) &&
                    targetHand.isValid()) {
                  listener = ResolveCharacterFromHandSafe(thisptr, targetHand);
                }
              } else {
                std::string listenerSource = "";
                listener = ResolveDialogueListenerForSpeech(tc, nullptr, listenerSource);
                if ((!listener || (uintptr_t)listener <= 0x1000 || listener == tc) &&
                    targetHand.isValid()) {
                  listener = ResolveCharacterFromHandSafe(thisptr, targetHand);
                }
              }
            }
            if (listener && (uintptr_t)listener > 0x1000 && listener != tc) {
              listenerName = ResolveCharacterNameSafe(listener);
              listenerFaction = SafeFaction(listener);
              listenerSerial = ResolveCharacterSerialForEvent(listener);
            }

            std::string speakerName = (tc && (uintptr_t)tc > 0x1000)
                                          ? ResolveCharacterNameSafe(tc)
                                          : (isPlayerSay ? "Player" : "Nearby NPC");
            std::string speakerFaction =
                (tc && (uintptr_t)tc > 0x1000) ? SafeFaction(tc) : "None";

            LogGameEvent("chat", speakerName, speakerFaction, listenerName,
                         listenerFaction, nonAiDialogueLine, speakerSerial,
                         listenerSerial);
            Log("REGULAR_DIALOGUE: bubble captured speaker=" + speakerName +
                " listener=" + listenerName + " player_line=" +
                std::string(isPlayerSay ? "1" : "0"));
          }

          hand sayTargetHand = targetHand;
          if (tc && (uintptr_t)tc > 0x1000) {
            sayTargetHand = tc->getHandle();
            targetHand = sayTargetHand;
          }

          if (sayTargetHand.isValid()) {
            EnterCriticalSection(&g_uiMutex);
            QueuedAction act;
            act.type = ACT_SAY;
            act.actor = sayTargetHand;
            act.target = sayTargetHand;
            act.message = bubbleContent;
            act.targetToken = talkTargetToken;
            act.ttsHash = ttsHash;
            act.utteranceId = utteranceId;
            int speechTimingMs = ttsDurationMs;
            if (isPlayerSay && g_ttsEnabled && ttsHash.empty() &&
                ttsDurationMs <= 0) {
              // PLAYER_TTS metadata arrives asynchronously after PLAYER_SAY.
              // Use a sentinel so only local player speech gets the short wait.
              speechTimingMs = -1;
            }
            act.taskValue = speechTimingMs;
            g_uiActionQueue.push_back(act);
            LeaveCriticalSection(&g_uiMutex);
            Log("TIMING_META: queued ACT_SAY target=" +
                (tc ? tc->getName() : "Unknown") +
                " utterance_id=" + utteranceId +
                " tts_hash=" + (ttsHash.empty() ? "" : ttsHash.substr(0, 8)) +
                " tts_dur_ms=" + ToString(ttsDurationMs) +
                " player_tts_wait_hint=" +
                std::string(speechTimingMs < 0 ? "1" : "0"));
          } else {
            if (!utteranceId.empty()) {
              PostSpeechDeliveryState(utteranceId, "cancelled");
            }
            Log("HOOK_MSG_PROC: SAY fallback logged without target hand actor=" +
                std::string(tc ? tc->getName() : "Unknown"));
          }
        }
      }
      if (autonomyCatalogMessage) {
        size_t tagged = 0;
        EnterCriticalSection(&g_uiMutex);
        if (g_uiActionQueue.size() > autonomyQueueSizeBefore) {
          for (size_t index = autonomyQueueSizeBefore;
               index < g_uiActionQueue.size(); ++index) {
            g_uiActionQueue[index].autonomyDecisionId = autonomyDecisionId;
            ++tagged;
          }
        }
        LeaveCriticalSection(&g_uiMutex);
        if (tagged > 0) {
          Log("AUTONOMY_PHASE3_ADAPTER: decision=" + autonomyDecisionId +
              " queued_actions=" + ToString(static_cast<int>(tagged)));
        } else {
          ReportAutonomyActionExecutionResult(
              autonomyDecisionId, false, "catalog_adapter_no_queued_action");
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
    std::string itemName = "Medical item";
    unsigned int itemOwnerSerial = 0;
    Character *itemOwner = nullptr;
    if (equipment) {
      std::string resolvedItem = TrimCopy(equipment->getName());
      if (!resolvedItem.empty()) {
        itemName = resolvedItem;
      }
      try {
        hand ownerHand = equipment->getInventoryWeAreIn();
        itemOwner = ResolveCharacterFromHandSafe(ownerHand);
      } catch (...) {
        itemOwner = nullptr;
      }
      itemOwnerSerial = ResolveCharacterSerialForEvent(itemOwner);
    }

    unsigned int whoSerial = ResolveCharacterSerialForEvent(who);
    unsigned int medSerial = ResolveCharacterSerialForEvent(med->me);
    unsigned int healerSerial = itemOwnerSerial != 0 ? itemOwnerSerial : whoSerial;

    auto registerPendingFor = [&](unsigned int serial) {
      if (serial != 0) {
        RegisterPendingMedicalItemUse(serial, itemName, 1);
      }
    };
    if (itemOwnerSerial != 0) {
      registerPendingFor(itemOwnerSerial);
    } else {
      registerPendingFor(whoSerial);
      if (medSerial != 0 && medSerial != whoSerial) {
        registerPendingFor(medSerial);
      }
    }

    std::string actorName = itemOwner ? ResolveCharacterNameSafe(itemOwner)
                                      : ResolveCharacterNameSafe(who);
    std::string actorFaction = itemOwner ? SafeFaction(itemOwner) : SafeFaction(who);
    std::string targetName = TrimCopy(med->me->getName());
    if (targetName.empty()) {
      targetName = "Unknown";
    }
    bool selfHeal =
        (healerSerial != 0 && medSerial != 0 && healerSerial == medSerial);
    if (!selfHeal) {
      std::string normalizedActor = ToLowerAsciiCopy(TrimCopy(actorName));
      std::string normalizedTarget = ToLowerAsciiCopy(TrimCopy(targetName));
      selfHeal =
          !normalizedActor.empty() && normalizedActor == normalizedTarget;
    }
    std::string healTargetText = selfHeal ? "themself" : targetName;
    std::string healMsg =
        "is using (" + itemName + ") to heal " + healTargetText;

    DWORD nowTick = GetTickCount();
    if (ShouldEmitHealingEvent(healerSerial, actorName, medSerial, targetName,
                               itemName, nowTick)) {
      LogGameEvent("healing", actorName, actorFaction,
                   med->me->getName(), SafeFaction(med->me),
                   healMsg, healerSerial, medSerial);
    }
  }
  return res;
}

bool characterGettingEaten_hook(Character *victim, float amount,
                                Character *eater) {
  bool res = false;
  if (characterGettingEaten_orig) {
    res = characterGettingEaten_orig(victim, amount, eater);
  }

  if (!victim || !eater) {
    return res;
  }

  EmitPredationEvent(eater, victim, "", GetTickCount());
  return res;
}

Item *buyItem_hook(Inventory *inv, Item *itemToBuy, RootObject *sendingTo) {
  RootObject *buyerObj = nullptr;
  RootObject *sellerObj = nullptr;
  if (sendingTo && (uintptr_t)sendingTo > 0x1000) {
    buyerObj = sendingTo;
  }
  if (inv && (uintptr_t)inv > 0x1000) {
    try {
      sellerObj = inv->owner;
    } catch (...) {
      sellerObj = nullptr;
    }
  }

  int buyerMoneyBefore = 0;
  int sellerMoneyBefore = 0;
  bool hasBuyerMoneyBefore =
      TryResolveRootObjectMoneySafe(buyerObj, buyerMoneyBefore);
  bool hasSellerMoneyBefore =
      TryResolveRootObjectMoneySafe(sellerObj, sellerMoneyBefore);

  Item *result = nullptr;
  if (buyItem_orig) {
    result = buyItem_orig(inv, itemToBuy, sendingTo);
  }

  if (buyerObj && result) {
    int buyerMoneyAfter = 0;
    int sellerMoneyAfter = 0;
    bool hasBuyerMoneyAfter =
        TryResolveRootObjectMoneySafe(buyerObj, buyerMoneyAfter);
    bool hasSellerMoneyAfter =
        TryResolveRootObjectMoneySafe(sellerObj, sellerMoneyAfter);

    int buyerSpent = 0;
    if (hasBuyerMoneyBefore && hasBuyerMoneyAfter &&
        buyerMoneyBefore > buyerMoneyAfter) {
      buyerSpent = buyerMoneyBefore - buyerMoneyAfter;
    }
    int sellerGained = 0;
    if (hasSellerMoneyBefore && hasSellerMoneyAfter &&
        sellerMoneyAfter > sellerMoneyBefore) {
      sellerGained = sellerMoneyAfter - sellerMoneyBefore;
    }
    int catsCost = buyerSpent > 0 ? buyerSpent : sellerGained;

    std::string itemName = "Unknown Item";
    int itemQty = 1;
    if (itemToBuy && (uintptr_t)itemToBuy > 0x1000) {
      try {
        itemName = itemToBuy->getName();
        itemQty = itemToBuy->quantity;
      } catch (...) {
        itemName = "Unknown Item";
        itemQty = 1;
      }
    }
    if (itemName == "Unknown Item" && result && (uintptr_t)result > 0x1000) {
      try {
        itemName = result->getName();
        if (itemQty <= 0) {
          itemQty = result->quantity;
        }
      } catch (...) {
      }
    }
    if (itemQty <= 0) {
      itemQty = 1;
    }

    std::string buyerName = ResolveRootObjectNameSafe(buyerObj);
    std::string buyerFaction = SafeFaction(buyerObj);
    std::string sellerName = ResolveRootObjectNameSafe(sellerObj);
    std::string sellerFaction = SafeFaction(sellerObj);

    if (!sellerObj || sellerName == buyerName) {
      sellerName = "None";
      sellerFaction = "None";
    }

    std::string message =
        "bought " + ToString(itemQty) + "x " + itemName;
    if (sellerName != "None") {
      message += " from " + sellerName;
    }
    if (catsCost > 0) {
      message += " for " + ToString(catsCost) + " cats";
    }

    LogGameEvent("trade", buyerName, buyerFaction, sellerName, sellerFaction,
                 message, ResolveRootObjectSerialForEvent(buyerObj),
                 ResolveRootObjectSerialForEvent(sellerObj));
  }
  return result;
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
    std::string line = TrimCopy(npc->dialogue->npcReplyText);
    if (!line.empty()) {
      return line;
    }
    if (npc->dialogue->speechBubblePanel &&
        (uintptr_t)npc->dialogue->speechBubblePanel > 0x1000 &&
        npc->dialogue->speechBubblePanel->textBox &&
        (uintptr_t)npc->dialogue->speechBubblePanel->textBox > 0x1000) {
      try {
        line = TrimCopy(npc->dialogue->speechBubblePanel->textBox->getOnlyText());
      } catch (...) {
        line.clear();
      }
      if (!line.empty()) {
        return line;
      }
      try {
        line = TrimCopy(npc->dialogue->speechBubblePanel->textBox->getCaption());
      } catch (...) {
        line.clear();
      }
      if (!line.empty()) {
        return line;
      }
    }
    try {
      line = TrimCopy(npc->dialogue->sayMsg);
    } catch (...) {
      line.clear();
    }
    if (!line.empty()) {
      return line;
    }
    if (npc->dialogue->currentLine &&
        (uintptr_t)npc->dialogue->currentLine > 0x1000) {
      try {
        line = TrimCopy(npc->dialogue->currentLine->getText(false));
      } catch (...) {
        line.clear();
      }
      if (!line.empty()) {
        return line;
      }
      try {
        std::string currentLineText;
        npc->dialogue->currentLine->getText(currentLineText, false);
        line = TrimCopy(currentLineText);
      } catch (...) {
        line.clear();
      }
    }
    return TrimCopy(line);
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
  if (!g_enableRegularDialogueCapture) {
    Log("SPEECH_HOOK: skipped player reply capture; regular dialogue disabled.");
    return;
  }

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
    speaker = ResolveFirstAliveConsciousPlayerCharacter(world);
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

  TrySpeechTriggeredPortraitSyncForPair(speaker, listener, sourceTag);

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
}

static void TryCaptureAmbientSpeechFromNative(Character *speaker,
                                              const std::string &rawLine,
                                              bool force,
                                              const char *sourceTag,
                                              Dialogue *dialogueHint) {
  if (!g_enableRegularDialogueCapture) {
    return;
  }
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

  TrySpeechTriggeredPortraitSyncForPair(speaker, listener, sourceTag);

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

static bool TryGetPrimaryPlayerCharacterSafe(GameWorld *world,
                                             Character *&playerOut) {
  playerOut = nullptr;
  if (!IsWorldStableForUI(world)) {
    return false;
  }

  __try {
    if (!world || !world->player ||
        world->player->playerCharacters.size() == 0) {
      return false;
    }
    Character *player = world->player->playerCharacters[0];
    if (!player || reinterpret_cast<uintptr_t>(player) < 0x1000) {
      return false;
    }
    playerOut = player;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    playerOut = nullptr;
    return false;
  }
}

static bool TryGetCharacterHandleSafe(Character *character, hand &handleOut) {
  handleOut = hand();
  if (!character || reinterpret_cast<uintptr_t>(character) < 0x1000) {
    return false;
  }
  __try {
    handleOut = character->getHandle();
    return handleOut.isValid() && handleOut.serial != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    handleOut = hand();
    return false;
  }
}

static Character *ResolveSelectedCharacterSehSafe(PlayerInterface *thisptr) {
  if (!thisptr || reinterpret_cast<uintptr_t>(thisptr) < 0x10000) {
    return nullptr;
  }

  Character *selected = nullptr;
  __try {
    selected = thisptr->selectedObject.getCharacter();
    if (!selected || reinterpret_cast<uintptr_t>(selected) < 0x1000) {
      selected = thisptr->selectedCharacter.getCharacter();
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    selected = nullptr;
  }

  if (selected && reinterpret_cast<uintptr_t>(selected) >= 0x1000) {
    return selected;
  }
  return nullptr;
}

static void ClearPendingSelectionContextPush() {
  EnterCriticalSection(&g_stateMutex);
  g_pendingSelectionContextHand = hand();
  g_pendingSelectionContextSerial = 0;
  g_pendingSelectionContextQueuedTick = 0;
  LeaveCriticalSection(&g_stateMutex);
}

static void QueueSelectionContextPush(const hand &selectionHand) {
  if (!selectionHand.isValid() || selectionHand.serial == 0) {
    ClearPendingSelectionContextPush();
    return;
  }
  DWORD nowTick = GetTickCount();
  EnterCriticalSection(&g_stateMutex);
  if (g_pendingSelectionContextSerial == selectionHand.serial &&
      g_pendingSelectionContextQueuedTick != 0) {
    LeaveCriticalSection(&g_stateMutex);
    return;
  }
  if (g_lastSelectionContextPushedSerial == selectionHand.serial &&
      g_lastSelectionContextPushedTick != 0 &&
      (nowTick - g_lastSelectionContextPushedTick) <
          kSelectionContextMinIntervalMs) {
    LeaveCriticalSection(&g_stateMutex);
    return;
  }
  g_pendingSelectionContextHand = selectionHand;
  g_pendingSelectionContextSerial = selectionHand.serial;
  g_pendingSelectionContextQueuedTick = nowTick;
  LeaveCriticalSection(&g_stateMutex);
}

static void RunPendingSelectionContextPush(Character *sel,
                                           DWORD worldBecameStableTick) {
  if (!sel || reinterpret_cast<uintptr_t>(sel) < 0x1000) {
    ClearPendingSelectionContextPush();
    return;
  }

  hand selectionHand;
  if (!TryGetCharacterHandleSafe(sel, selectionHand)) {
    ClearPendingSelectionContextPush();
    return;
  }

  DWORD nowTick = GetTickCount();
  if ((nowTick - worldBecameStableTick) < kSelectionContextStartupDelayMs) {
    return;
  }

  hand pendingHand;
  unsigned int pendingSerial = 0;
  DWORD queuedTick = 0;
  DWORD lastPushedTick = 0;
  EnterCriticalSection(&g_stateMutex);
  pendingHand = g_pendingSelectionContextHand;
  pendingSerial = g_pendingSelectionContextSerial;
  queuedTick = g_pendingSelectionContextQueuedTick;
  lastPushedTick = g_lastSelectionContextPushedTick;
  LeaveCriticalSection(&g_stateMutex);

  if (!pendingHand.isValid() || pendingSerial == 0 ||
      pendingSerial != selectionHand.serial || queuedTick == 0) {
    return;
  }
  if (nowTick - queuedTick < kSelectionContextDebounceMs) {
    return;
  }
  if (lastPushedTick != 0 &&
      (nowTick - lastPushedTick) < kSelectionContextMinIntervalMs) {
    return;
  }

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

  EnterCriticalSection(&g_stateMutex);
  if (g_pendingSelectionContextSerial == selectionHand.serial) {
    g_pendingSelectionContextHand = hand();
    g_pendingSelectionContextSerial = 0;
    g_pendingSelectionContextQueuedTick = 0;
    g_lastSelectionContextPushedTick = nowTick;
    g_lastSelectionContextPushedSerial = selectionHand.serial;
  }
  LeaveCriticalSection(&g_stateMutex);
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
  static LONG postLoadHookProbe = 0;
  if (!thisptr || reinterpret_cast<uintptr_t>(thisptr) < 0x10000) {
    static bool loggedInvalidThis = false;
    if (!loggedInvalidThis) {
      Log("HOOK_WARN: Hook_PlayerUpdateTick received invalid this pointer.");
      loggedInvalidThis = true;
    }
    return;
  }

  const bool tracePostLoadHook =
      InterlockedCompareExchange(&postLoadHookProbe, 2, 1) == 1;
  if (tracePostLoadHook) {
    Log("HOOK_LOAD_PROBE: entering first post-stable PlayerInterface::update");
  }
  if (playerUpdate_orig)
    playerUpdate_orig(thisptr);
  if (tracePostLoadHook) {
    Log("HOOK_LOAD_PROBE: original PlayerInterface::update returned");
  }

  GameWorld *worldUi = GetWorldSafe();
  if (tracePostLoadHook) {
    Log("HOOK_LOAD_PROBE: GameWorld lookup returned");
  }
  static bool worldWasStable = false;
  static DWORD worldBecameStableTick = 0;
  static bool heavySweepPrimed = false;
  static bool motdAutoOpenQueued = false;
  static DWORD motdAutoOpenTick = 0;
  static DWORD motdAutoOpenDeadlineTick = 0;
  static bool loadInitEventDispatched = false;
  static bool postLoadPipelineProbed = false;
  static bool heavySyncPipelineProbed = false;
  bool worldStable = IsWorldStableForUI(worldUi);
  if (tracePostLoadHook) {
    Log(std::string("HOOK_LOAD_PROBE: stability check returned stable=") +
        (worldStable ? "1" : "0"));
  }

  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (tracePostLoadHook) {
    Log(std::string("HOOK_LOAD_PROBE: GUI lookup returned available=") +
        (gui ? "1" : "0"));
  }
  if (!gui) {
    // During loads MyGUI can be torn down; clear stale pointers and do nothing.
    ResetAutonomyController("gui_unavailable");
    ResetAutonomySafetyProbe("gui_unavailable");
    CloseChatUI();
    g_settingsWindow = nullptr;
    g_startingWindow = nullptr;
    g_welcomeWindow = nullptr;
    g_aiNpcInfoWindow = nullptr;
    g_aiDiaryWindow = nullptr;
    g_recentHistoryWindow = nullptr;
    g_statusHudWindow = nullptr;
    return;
  }

  if (!worldStable) {
    ResetAutonomyController("world_unstable");
    ResetAutonomySafetyProbe("world_unstable");
    if (worldWasStable) {
      worldWasStable = false;
      Log("HOOK: world transition detected; pausing UI hook logic.");
      EnterCriticalSection(&g_stateMutex);
        g_inventorySyncStateBySerial.clear();
        g_itemImageSyncStateByItemId.clear();
        g_itemImageSyncRequestQueue.clear();
        g_itemImageLastRunTick = 0;
        g_lastInventoryUiVisibleTick = 0;
        g_worldStableSinceTick = 0;
      g_pendingSelectionContextHand = hand();
      g_pendingSelectionContextSerial = 0;
      g_pendingSelectionContextQueuedTick = 0;
      g_lastSelectionSerial = 0;
      g_lastSelectionContextPushedTick = 0;
      g_lastSelectionContextPushedSerial = 0;
      g_activeInventoryJson = "[]";
      g_playerInventoryJson = "[]";
      g_lastInventoryHand = hand();
      g_playerHand = hand();
      g_activatedAnimalSerials.clear();
      g_activatedAiActorNames.clear();
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
      ResetPortraitSyncState();
      ResetPlayerCatsSyncState();
      ResetDynamicProfileIntervalSyncState();
      ResetPlayerSquadsSyncState();
      ResetFactionRelationSyncState();
      ResetTownKnowledgeSyncState();
      heavySweepPrimed = false;
      motdAutoOpenQueued = false;
      motdAutoOpenTick = 0;
      motdAutoOpenDeadlineTick = 0;
      loadInitEventDispatched = false;
      postLoadPipelineProbed = false;
      heavySyncPipelineProbed = false;
      Log("INV_SYNC: state reset on world transition.");
      Log("PORTRAIT_SYNC: state reset on world transition.");
    }
    if (IsAnyStobeMenuUIOpen())
      CloseAllStobeMenuUI();
    if (g_statusHudWindow)
      CloseStatusHud();
    if (g_chatWindow)
      CloseChatUI();
    return;
  }

  if (!worldWasStable) {
    worldWasStable = true;
      worldBecameStableTick = GetTickCount();
      EnterCriticalSection(&g_stateMutex);
      g_lastInventoryUiVisibleTick = 0;
      g_worldStableSinceTick = worldBecameStableTick;
    g_pendingSelectionContextHand = hand();
    g_pendingSelectionContextSerial = 0;
    g_pendingSelectionContextQueuedTick = 0;
    g_lastSelectionSerial = 0;
    g_lastSelectionContextPushedTick = 0;
    g_lastSelectionContextPushedSerial = 0;
    LeaveCriticalSection(&g_stateMutex);
    heavySweepPrimed = false;
    motdAutoOpenQueued = false;
    motdAutoOpenTick = 0;
    motdAutoOpenDeadlineTick = 0;
    loadInitEventDispatched = false;
    postLoadPipelineProbed = false;
    heavySyncPipelineProbed = false;
    InterlockedExchange(&postLoadHookProbe, 1);
    Log("HOOK: world stable; delaying UI hook logic.");
    return;
  }

  // Save loads can expose unstable pointers for several seconds after world appears.
  if (GetTickCount() - worldBecameStableTick < 10000) {
    return;
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
  // Do not open MOTD late during gameplay; leave manual open via general menu hotkey.
        g_welcomeShown = true;
        motdAutoOpenQueued = false;
        motdAutoOpenTick = 0;
        motdAutoOpenDeadlineTick = 0;
        Log("UI: MOTD auto-open skipped (startup quiet window expired).");
      } else if ((LONG)(now - motdAutoOpenTick) >= 0 &&
                  !IsSpeechSystemBusyForMOTD() && !g_chatWindow &&
                  !IsAnyStobeMenuUIOpen()) {
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

  // Left menu hotkey (configurable) - only once world/UI are stable.
  if ((GetAsyncKeyState(g_generalHotkey) & 0x8000)) {
    static DWORD lastSettingsTick = 0;
    DWORD now = GetTickCount();
    if (now - lastSettingsTick > 500) {
      lastSettingsTick = now;
      Log("UI: general hotkey pressed [" + g_generalHotkeyStr + "].");
      if (IsAnyStobeMenuUIOpen()) {
        CloseAllStobeMenuUI();
        Log("UI: CloseAllStobeMenuUI done.");
      } else {
        CreateStartingUI();
        Log("UI: CreateStartingUI done.");
      }
    }
  }

  UpdateStatusHud(worldUi);

  bool probePostLoadPipeline = !postLoadPipelineProbed;
  if (probePostLoadPipeline) {
    Log("HOOK_LOAD_PROBE: entering warmed post-load pipeline");
  }

  // 1. Core Selection Tracking
  Character *sel = ResolveSelectedCharacterSehSafe(thisptr);

  // Detect Selection Change
  EnterCriticalSection(&g_stateMutex);
  bool selectionChanged = false;
  hand currentSelectionHand;
  bool hasSelectionHandle = TryGetCharacterHandleSafe(sel, currentSelectionHand);
  if (hasSelectionHandle && currentSelectionHand.serial != 0) {
    if (currentSelectionHand.serial != g_lastSelectionSerial) {
      std::string selectedName = "";
      try {
        selectedName = sel->getName();
      } catch (...) {
        selectedName = "";
      }
      g_activeCharName = selectedName;
      g_lastSelectionSerial = currentSelectionHand.serial;
      g_lastSelectionHand = currentSelectionHand;
      selectionChanged = true;
    } else {
      g_lastSelectionHand = currentSelectionHand;
    }
  } else if (g_lastSelectionSerial != 0 || g_lastSelectionHand.isValid()) {
    g_activeCharName = "";
    g_lastSelectionSerial = 0;
    g_lastSelectionHand = hand();
    selectionChanged = true;
  }
  LeaveCriticalSection(&g_stateMutex);

  if (selectionChanged && hasSelectionHandle && sel &&
      (uintptr_t)sel > 0x1000) {
    if (ShouldProcessAnimalCharacter(sel)) {
      if (GetTickCount() - worldBecameStableTick >= kHookHeavySyncWarmupMs) {
        SyncInventoryForCharacter(sel, true, "selection_change");
        SyncPortraitForCharacter(sel, false, "selection_change");
      } else {
        Log("HOOK_LOAD_PROBE: deferred initial selection inventory and portrait sync");
      }
      // Full context snapshots can traverse stale nearby objects immediately
      // after a save load. Dialogue and action paths still build them on demand.
      ClearPendingSelectionContextPush();
    } else {
      ClearPendingSelectionContextPush();
      Log("ANIMAL_TALKS: ignoring selection context sync for inactive animal.");
    }
  } else if (selectionChanged) {
    ClearPendingSelectionContextPush();
  }
  if (probePostLoadPipeline) {
    Log("HOOK_LOAD_PROBE: selection stage complete");
  }

  // 2. Message queue + queued actions
  GameWorld *world = GetWorldSafe();
  bool worldFrameStable = IsWorldStableForUI(world);
  if (world && worldFrameStable) {
    UpdateAutonomyController(world);
    UpdateAutonomySafetyProbe(world, sel);
    if (!loadInitEventDispatched) {
      if (probePostLoadPipeline) {
        Log("HOOK_LOAD_PROBE: narrator init stage starting");
      }
      bool dispatchedViaStream =
          TriggerNarratorWelcomeOnLoad(world, ResolvePlayerSpeakerForCurrentTalk(world));
      if (!dispatchedViaStream) {
        Character *initSpeaker = ResolvePlayerSpeakerForCurrentTalk(world);
        if ((!initSpeaker || (uintptr_t)initSpeaker < 0x1000) && world->player &&
            world->player->playerCharacters.size() > 0) {
          initSpeaker = ResolveFirstAliveConsciousPlayerCharacter(world);
        }

        std::string initActor = ResolveCharacterNameSafe(initSpeaker);
        if (initActor.empty() || initActor == "Unknown") {
          initActor = "Player";
        }
        std::string initFaction = SafeFaction(initSpeaker);
        if (initFaction.empty()) {
          initFaction = "None";
        }
        unsigned int initActorSerial = ResolveCharacterSerialForEvent(initSpeaker);
        std::string initMessage = "game load detected";

        LogGameEvent("init", initActor, initFaction, "", "None", initMessage,
                     initActorSerial, 0);
        Log("LOAD_SYNC: narrator welcome stream unavailable; queued init fallback actor=" +
            initActor + " serial=" + ToString((int)initActorSerial));
      }
      loadInitEventDispatched = true;
      if (probePostLoadPipeline) {
        Log("HOOK_LOAD_PROBE: narrator init stage complete");
      }
    }

    if (probePostLoadPipeline) {
      Log("HOOK_LOAD_PROBE: message and action stage starting");
    }
    ProcessMessageQueue(world);
    static int invTimer = 0;
    ExecuteQueuedActions(world, invTimer);
    ApplyFollowTargets(world);
    ApplyTravelTargets(world);
    RunQueuedItemImageSync();
    RunPendingSelectionContextPush(sel, worldBecameStableTick);
    if (probePostLoadPipeline) {
      Log("HOOK_LOAD_PROBE: message and action stage complete");
    }

    DWORD nowTick = GetTickCount();
    if (!heavySweepPrimed) {
      g_lastInventorySweepTick = nowTick;
      g_lastPortraitSweepTick = nowTick;
      g_lastNpcWorldEventSweepTick = nowTick;
      g_lastInfoNpcTelemetryCheckTick = nowTick;
      g_lastInfoLocTelemetryCheckTick = nowTick;
      heavySweepPrimed = true;
    }

    bool heavySyncReady = (nowTick - worldBecameStableTick) >= kHookHeavySyncWarmupMs;
    bool npcEventSyncReady =
        (nowTick - worldBecameStableTick) >= kNpcWorldEventWarmupMs;
    bool probeHeavySyncPipeline = heavySyncReady && !heavySyncPipelineProbed;
    if (npcEventSyncReady) {
      if (probeHeavySyncPipeline) {
        Log("HOOK_LOAD_PROBE: NPC world event sweep starting");
      }
      LONG sweepSehBefore =
          InterlockedCompareExchange(&g_npcWorldEventSweepSehCount, 0, 0);
      RunNpcWorldEventSweep(world, sel);
      LONG sweepSehAfter =
          InterlockedCompareExchange(&g_npcWorldEventSweepSehCount, 0, 0);
      if (sweepSehAfter != sweepSehBefore) {
        Log("NPC_WORLD_EVENT: skipped unsafe sweep after engine fault code=" +
            ToString((int)g_npcWorldEventSweepLastSehCode));
      }
      if (probeHeavySyncPipeline) {
        Log("HOOK_LOAD_PROBE: NPC world event sweep complete");
      }
    }
    if (heavySyncReady) {
      if (probeHeavySyncPipeline) {
        Log("HOOK_LOAD_PROBE: inventory sweep starting");
      }
      RunInventorySyncSweep(world, sel);
      if (probeHeavySyncPipeline) {
        Log("HOOK_LOAD_PROBE: inventory sweep complete");
        Log("HOOK_LOAD_PROBE: portrait sweep starting");
      }
      RunPlayerFactionPortraitSweep(world);
      if (probeHeavySyncPipeline) {
        Log("HOOK_LOAD_PROBE: portrait sweep complete");
        Log("HOOK_LOAD_PROBE: info telemetry sweep starting");
      }
      RunInfoTelemetrySweep(world, sel);
      if (probeHeavySyncPipeline) {
        Log("HOOK_LOAD_PROBE: info telemetry sweep complete");
        Log("HOOK_LOAD_PROBE: faction relation sync starting");
      }
      RunFactionRelationSync(world);
      if (probeHeavySyncPipeline) {
        Log("HOOK_LOAD_PROBE: faction relation sync complete");
        Log("HOOK_LOAD_PROBE: town knowledge sync starting");
      }
      RunTownKnowledgeSync(world);
      if (probeHeavySyncPipeline) {
        heavySyncPipelineProbed = true;
        Log("HOOK_LOAD_PROBE: town knowledge sync complete");
        Log("HOOK_LOAD_PROBE: heavy sync pipeline complete");
      }
    }
  }
  if (probePostLoadPipeline) {
    postLoadPipelineProbed = true;
    Log("HOOK_LOAD_PROBE: warmed post-load pipeline complete");
  }

  if (world && worldFrameStable && g_enableBoredEvents) {
    bool forceTrigger = false;
    EnterCriticalSection(&g_stateMutex);
    if (g_triggerBoredEvent) {
      forceTrigger = true;
      g_triggerBoredEvent = false;
    }
    LeaveCriticalSection(&g_stateMutex);

    DWORD now = GetTickCount();
    bool worldPaused = world->isPaused();
    int nowGameTs = 0;
    try {
      TimeOfDay nowTod = world->getTimeStamp_inGameHours();
      nowGameTs = (int)nowTod.getTotalSeconds();
    } catch (...) {
      nowGameTs = 0;
    }
    if (nowGameTs <= 0) {
      nowGameTs = g_lastBoredEventGameTs;
    }

    int intervalHours = g_boredEventIntervalHours;
    if (intervalHours < 1) {
      intervalHours = 1;
    } else if (intervalHours > 720) {
      intervalHours = 720;
    }

    const int extraBoredDelayGamets = 10;
    const int intervalGamets = intervalHours * 3600 + extraBoredDelayGamets;

    if (!forceTrigger && g_lastBoredEventGameTs > 0 &&
        nowGameTs > 0 && nowGameTs + 5 < g_lastBoredEventGameTs) {
      // Game load/rewind: rebase so bored events do not immediately spam.
      g_lastBoredEventGameTs = nowGameTs;
      g_lastBoredEventTick = now;
    }

    if (!forceTrigger && g_lastBoredEventGameTs <= 0) {
      g_lastBoredEventGameTs = nowGameTs;
      g_lastBoredEventTick = now;
      Log("BORED_EVENT: startup cooldown armed for " + ToString(intervalHours) +
          "h (+10s ingame grace)");
    }

    bool periodicDue =
        !forceTrigger && !worldPaused && nowGameTs > 0 &&
        g_lastBoredEventGameTs > 0 &&
        ((nowGameTs - g_lastBoredEventGameTs) >= intervalGamets);

    if (forceTrigger || periodicDue) {
      bool speechBusy = IsTtsPlaybackActive();
      if (speechBusy) {
        if (forceTrigger) {
          EnterCriticalSection(&g_stateMutex);
          g_triggerBoredEvent = true;
          LeaveCriticalSection(&g_stateMutex);
          Log("BORED_EVENT: delayed (active TTS playback)");
        } else {
          g_lastBoredEventGameTs = nowGameTs;
          g_lastBoredEventTick = now;
        }
      } else {
        g_lastBoredEventGameTs = nowGameTs;
        g_lastBoredEventTick = now;
        bool dispatched = TriggerBoredEvent(world, forceTrigger);
        if (!dispatched && forceTrigger) {
          Log("BORED_EVENT: manual trigger skipped (no eligible NPC)");
        }
      }
    }
  }

  // Rename checks are now queued only for dialogue-tagged NPCs.
  // 4. Input Handling ??? Chat window hotkey
  if ((GetAsyncKeyState(g_chatHotkey) & 0x8000) && !g_chatWindow) {
    static DWORD lastTalkTick = 0;
    if (GetTickCount() - lastTalkTick > 500) {
      lastTalkTick = GetTickCount();
      Log("UI: chat hotkey pressed [" + g_chatHotkeyStr + "].");
      if (IsAnyStobeMenuUIOpen()) {
        CloseAllStobeMenuUI();
        Log("UI: closed STOBE menu before opening chat.");
      }
      if (sel && (uintptr_t)sel > 0x1000) {
        Character *chatTarget = sel;
        if (sel->isPlayerCharacter()) {
          Character *resolvedTarget = nullptr;
          if (g_useNearestPlayerSpeaker) {
            resolvedTarget = ResolveNearestNpcTargetForSelection(world, sel);
            if (resolvedTarget && (uintptr_t)resolvedTarget > 0x1000) {
              chatTarget = resolvedTarget;
              Log("CHAT_OPEN: selected player speaker '" + sel->getName() +
                  "' targeted nearest NPC '" + chatTarget->getName() + "'");
            } else {
              EnterCriticalSection(&g_msgMutex);
              g_messageQueue.push_back("NOTIFY:No nearby NPC target available.");
              LeaveCriticalSection(&g_msgMutex);
              Log("CHAT_OPEN: selected player speaker '" + sel->getName() +
                  "' has no nearby NPC target; chat open blocked");
              return;
            }
          } else {
            resolvedTarget = ResolveNearestSquadmateTargetForSelection(world, sel);
            if (resolvedTarget && (uintptr_t)resolvedTarget > 0x1000) {
              chatTarget = resolvedTarget;
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
        }

        if (!IsAliveConsciousCharacterForTargeting(chatTarget)) {
          Log("CHAT_OPEN: allowing dead_or_unconscious target='" +
              chatTarget->getName() + "'");
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
        Log("UI: CreateChatUI done target=" + chatTarget->getName());
      } else {
        EnterCriticalSection(&g_msgMutex);
        g_messageQueue.push_back("NOTIFY:Select an NPC before opening chat.");
        LeaveCriticalSection(&g_msgMutex);
        Log("CHAT_OPEN: no selected character; chat open blocked");
      }
    }
  }

}

// Redundant hooks removed since playerUpdate handles real-time needs now.

DWORD WINAPI RenameWorker(LPVOID lpParam) {
  // Wait for server to be fully ready
  Sleep(8000);

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
      DWORD retryTick =
          Stobe::IdentityRename::ResolveRetryAttemptDeadline(GetTickCount());
      EnterCriticalSection(&g_nameCheckMutex);
      for (size_t i = 0; i < batch.size(); ++i) {
        if (g_identityRenameCompletedSerials.count(batch[i].serial) == 0) {
          g_identityRenameNextAttemptTick[batch[i].serial] = retryTick;
        }
      }
      LeaveCriticalSection(&g_nameCheckMutex);
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
      DWORD retryTick =
          Stobe::IdentityRename::ResolveRetryAttemptDeadline(GetTickCount());
      EnterCriticalSection(&g_nameCheckMutex);
      for (size_t i = 0; i < batch.size(); ++i) {
        if (g_identityRenameCompletedSerials.count(batch[i].serial) == 0) {
          g_identityRenameNextAttemptTick[batch[i].serial] = retryTick;
        }
      }
      LeaveCriticalSection(&g_nameCheckMutex);
      continue;
    }
    if (parsedResp[0] != '[') {
      std::string snippet = parsedResp.substr(0, std::min<size_t>(120, parsedResp.size()));
      Log("NAME_ASSIGN: Skipping malformed batch identity response: " + snippet);
      DWORD retryTick =
          Stobe::IdentityRename::ResolveRetryAttemptDeadline(GetTickCount());
      EnterCriticalSection(&g_nameCheckMutex);
      for (size_t i = 0; i < batch.size(); ++i) {
        if (g_identityRenameCompletedSerials.count(batch[i].serial) == 0) {
          g_identityRenameNextAttemptTick[batch[i].serial] = retryTick;
        }
      }
      LeaveCriticalSection(&g_nameCheckMutex);
      continue;
    }

    int assignedCount = 0;
    std::set<unsigned int> responseSerials;
    DWORD retryTick =
        Stobe::IdentityRename::ResolveRetryAttemptDeadline(GetTickCount());
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
      if (serial == 0) {
        continue;
      }
      responseSerials.insert(serial);

      Stobe::IdentityRename::BatchStatus batchStatus =
          Stobe::IdentityRename::ParseBatchStatus(status);
      if (batchStatus == Stobe::IdentityRename::BATCH_STATUS_RENAME) {
        std::string newName = JsonReadField(obj, "new_name");
        if (!newName.empty()) {
          std::string renameMsg = "NPC_RENAME: " + sSerial + "|" + newName;
          EnterCriticalSection(&g_msgMutex);
          g_messageQueue.push_back(renameMsg);
          LeaveCriticalSection(&g_msgMutex);
          assignedCount++;
        } else {
          batchStatus = Stobe::IdentityRename::BATCH_STATUS_RETRY;
        }
      }

      EnterCriticalSection(&g_nameCheckMutex);
      if (batchStatus == Stobe::IdentityRename::BATCH_STATUS_RENAME) {
        g_renamedSerials.insert(serial);
        g_identityRenameCompletedSerials.insert(serial);
        g_identityRenameNextAttemptTick.erase(serial);
      } else if (batchStatus ==
                 Stobe::IdentityRename::BATCH_STATUS_COMPLETE) {
        g_identityRenameCompletedSerials.insert(serial);
        g_identityRenameNextAttemptTick.erase(serial);
      } else {
        g_identityRenameNextAttemptTick[serial] = retryTick;
      }
      LeaveCriticalSection(&g_nameCheckMutex);

    }

    EnterCriticalSection(&g_nameCheckMutex);
    for (size_t i = 0; i < batch.size(); ++i) {
      unsigned int batchSerial = batch[i].serial;
      if (batchSerial == 0 ||
          responseSerials.count(batchSerial) > 0 ||
          g_identityRenameCompletedSerials.count(batchSerial) > 0) {
        continue;
      }
      g_identityRenameNextAttemptTick[batchSerial] = retryTick;
    }
    LeaveCriticalSection(&g_nameCheckMutex);

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
      CreateThread(NULL, 0, RenameWorker, NULL, 0, NULL);
      nameThreadStarted = true;
    }

    // Keep main thread lightweight and avoid direct world/player reads.
    // Runtime game-state sync now runs from the hooked PlayerInterface update path.
    SyncDynamicProfileIntervalToConfOpts(false, "main_loop");
    SyncPluginVersionToConfOpts(false, "main_loop");
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
  Log(std::string("STARTUP: Stobe plugin release date ") + kStobePluginReleaseDate);
  Log("UI: MOTD auto-popup enabled when EnableMOTD is ON.");
  StartAutonomyController();

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

  void *thunkInventoryBuyItem = (void *)GetProcAddress(
      hLib, "?buyItem@Inventory@@QEAAPEAVItem@@PEAV2@PEAVRootObject@@@Z");
  if (!thunkInventoryBuyItem) {
    Log("HOOK_WARN: Inventory::buyItem symbol not found.");
  } else {
    __int64 realInventoryBuyItem = KenshiLib::GetRealAddress(thunkInventoryBuyItem);
    if (!realInventoryBuyItem) {
      Log("HOOK_WARN: GetRealAddress failed for Inventory::buyItem.");
    } else {
      KenshiLib::HookStatus buyItemStatus =
          KenshiLib::AddHook((void *)realInventoryBuyItem, (void *)buyItem_hook,
                             (void **)&buyItem_orig);
      Log("HOOK_DIAG: Inventory::buyItem AddHook status=" +
          ToString((int)buyItemStatus) + " orig=" +
          ToString((unsigned int)(uintptr_t)buyItem_orig));
    }
  }

  void *thunkMedicalApplyFirstAid = (void *)GetProcAddress(
      hLib, "?applyFirstAid@MedicalSystem@@QEAA_NMPEAVItem@@MPEAVCharacter@@@Z");
  if (!thunkMedicalApplyFirstAid) {
    Log("HOOK_WARN: MedicalSystem::applyFirstAid symbol not found.");
  } else {
    __int64 realMedicalApplyFirstAid =
        KenshiLib::GetRealAddress(thunkMedicalApplyFirstAid);
    if (!realMedicalApplyFirstAid) {
      Log("HOOK_WARN: GetRealAddress failed for MedicalSystem::applyFirstAid.");
    } else {
      KenshiLib::HookStatus medicalFirstAidStatus = KenshiLib::AddHook(
          (void *)realMedicalApplyFirstAid, (void *)applyFirstAid_hook,
          (void **)&applyFirstAid_orig);
      Log("HOOK_DIAG: MedicalSystem::applyFirstAid AddHook status=" +
          ToString((int)medicalFirstAidStatus) + " orig=" +
          ToString((unsigned int)(uintptr_t)applyFirstAid_orig));
    }
  }

  void *thunkCharacterGettingEaten = (void *)GetProcAddress(
      hLib, "?gettingEaten@Character@@UEAA_NMPEAV1@@Z");
  if (!thunkCharacterGettingEaten) {
    thunkCharacterGettingEaten = (void *)GetProcAddress(
        hLib, "?gettingEaten@Character@@QEAA_NMPEAV1@@Z");
  }
  if (!thunkCharacterGettingEaten) {
    Log("HOOK_WARN: Character::gettingEaten symbol not found.");
  } else {
    __int64 realCharacterGettingEaten =
        KenshiLib::GetRealAddress(thunkCharacterGettingEaten);
    if (!realCharacterGettingEaten) {
      Log("HOOK_WARN: GetRealAddress failed for Character::gettingEaten.");
    } else {
      KenshiLib::HookStatus characterGettingEatenStatus = KenshiLib::AddHook(
          (void *)realCharacterGettingEaten, (void *)characterGettingEaten_hook,
          (void **)&characterGettingEaten_orig);
      Log("HOOK_DIAG: Character::gettingEaten AddHook status=" +
          ToString((int)characterGettingEatenStatus) + " orig=" +
          ToString((unsigned int)(uintptr_t)characterGettingEaten_orig));
    }
  }

  const bool kEnableSpeechCaptureHooks = false;
  const bool kEnableDialogueReplyHooks = true;
  if (!kEnableSpeechCaptureHooks) {
    Log("HOOK: speech/dialogue capture hooks disabled for stability.");
  } else {
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
  }

  if (!kEnableDialogueReplyHooks) {
    Log("HOOK: dialogue reply hooks disabled.");
  } else {
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


