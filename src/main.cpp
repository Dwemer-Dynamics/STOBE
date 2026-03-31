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
#include "Globals.h"
#include "Utils.h"

#include <kenshi/CharStats.h>
#include <kenshi/Character.h>
#include <kenshi/CharBody.h>
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
#include <kenshi/Tasker.h>
#include <kenshi/gui/InventoryGUI.h>
#include <kenshi/gui/PortraitManager.h>
#include <kenshi/util/hand.h>
#include <kenshi/Damages.h>
#include <ogre/OgreImage.h>

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

struct PortraitSyncState {
  std::string lastHash;
  DWORD lastSentTick;
  DWORD lastSeenTick;
  bool hasSent;

  PortraitSyncState()
      : lastHash(""), lastSentTick(0), lastSeenTick(0), hasSent(false) {}
};

struct ItemImageSyncState {
  std::string lastHash;
  DWORD lastSentTick;
  DWORD lastSeenTick;
  bool hasSent;

  ItemImageSyncState()
      : lastHash(""), lastSentTick(0), lastSeenTick(0), hasSent(false) {}
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
  int currentTask;
  int constructionAction;
  unsigned int constructionSubjectSerial;
  std::string constructionSubjectName;
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
        currentTask((int)NULL_TASK), constructionAction(0), constructionSubjectSerial(0),
        constructionSubjectName(""),
        lockpickingSkill(0), lastSpeechLine(""),
        lastSeenTick(0) {}
};

static std::map<unsigned int, InventorySyncState> g_inventorySyncStateBySerial;
static std::map<std::string, PortraitSyncState> g_portraitSyncStateByStorageId;
static std::map<unsigned int, DWORD> g_portraitSpeechTriggerBySerial;
static std::map<std::string, ItemImageSyncState> g_itemImageSyncStateByItemId;
static std::deque<PendingItemImageSyncRequest> g_itemImageSyncRequestQueue;
static DWORD g_lastInventorySweepTick = 0;
static const DWORD kInventorySweepIntervalMs = 6000;
static const DWORD kInventoryMinResendMs = 1200;
static const size_t kInventorySweepCandidateLimit = 8;
static const DWORD kInventoryStateRetentionMs = 15 * 60 * 1000;
static const size_t kItemImageBatchLimit = 5;
static const DWORD kItemImageMinResendMs = 30 * 60 * 1000;
static const DWORD kItemImageStateRetentionMs = 60 * 60 * 1000;
static const DWORD kItemImageRunCooldownMs = 10 * 1000;
static const DWORD kItemImageStartupDelayMs = 20 * 1000;
static const size_t kItemImageRequestQueueMax = 64;
static DWORD g_itemImageLastRunTick = 0;
static DWORD g_worldStableSinceTick = 0;
static volatile LONG g_portraitSehCount = 0;
static DWORD g_portraitLastSehTick = 0;
static DWORD g_portraitLastSehCode = 0;
static bool g_portraitSyncDisabledForSession = false;
static bool g_portraitDisableLogged = false;
static DWORD g_lastPortraitSweepTick = 0;
static const DWORD kPortraitSweepIntervalMs = 15000;
static const DWORD kPortraitMinResendMs = 30 * 60 * 1000;
static const size_t kPortraitSweepCandidateLimit = 48;
static const DWORD kPortraitStateRetentionMs = 30 * 60 * 1000;
static const DWORD kPortraitSpeechTriggerCooldownMs = 2 * 60 * 1000;
static std::map<unsigned int, NpcWorldEventState> g_npcWorldEventStateBySerial;
static DWORD g_lastNpcWorldEventSweepTick = 0;
static const DWORD kNpcWorldEventSweepIntervalMs = 3000;
static const size_t kNpcWorldEventCandidateLimit = 32;
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
static const char *kStobePluginVersion = "0.7.0";
static const char *kStobePluginReleaseDate = "2026-03-28";
static bool g_pluginVersionSyncHasValue = false;
static std::string g_pluginVersionSyncLastValue = "";
static DWORD g_pluginVersionSyncLastSentTick = 0;
static const DWORD kHookHeavySyncWarmupMs = 45 * 1000;
static const DWORD kSelectionContextStartupDelayMs = 60 * 1000;
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

static void MaybeDisablePortraitSyncAfterSeh() {
  if (g_portraitSyncDisabledForSession) {
    return;
  }
  LONG sehCount = InterlockedCompareExchange(&g_portraitSehCount, 0, 0);
  if (sehCount >= 3) {
    g_portraitSyncDisabledForSession = true;
    if (!g_portraitDisableLogged) {
      g_portraitDisableLogged = true;
      Log("PORTRAIT_SYNC: disabled for this session after repeated engine SEH faults code=" +
          ToString((int)g_portraitLastSehCode) + " count=" +
          ToString((int)sehCount) + " last_tick=" +
          ToString((int)g_portraitLastSehTick));
    }
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

  DWORD nowTick = GetTickCount();
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

  size_t candidates = 0;
  size_t sent = 0;
  for (uint32_t i = 0; i < world->player->playerCharacters.size(); ++i) {
    Character *member = world->player->playerCharacters[i];
    if (!member || (uintptr_t)member < 0x1000) {
      continue;
    }
    ++candidates;
    if (SyncPortraitForCharacter(member, false, "player_faction_sweep")) {
      ++sent;
    }
    if (candidates >= kPortraitSweepCandidateLimit) {
      break;
    }
  }

  static DWORD lastPruneTick = 0;
  if (nowTick - lastPruneTick >= 60000) {
    lastPruneTick = nowTick;
    PrunePortraitSyncState();
  }

  if (sent > 0) {
    Log("PORTRAIT_SYNC: sweep complete candidates=" + ToString((int)candidates) +
        " sent=" + ToString((int)sent));
  } else {
    static DWORD lastNoSendLogTick = 0;
    if (nowTick - lastNoSendLogTick >= 60000) {
      lastNoSendLogTick = nowTick;
      Log("PORTRAIT_SYNC: sweep no-send candidates=" +
          ToString((int)candidates));
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

static bool IsItemImageSyncReasonAllowed(const std::string &reason) {
  std::string key = ToLowerAsciiCopy(TrimCopy(reason));
  if (key.empty()) {
    return false;
  }
  if (key == "periodic") {
    return false;
  }
  if (key == "selection_change" || key == "chat_open" ||
      key == "dialogue_npc" || key == "dialogue_player") {
    return true;
  }
  if (key.find("dialogue") != std::string::npos ||
      key.find("description") != std::string::npos ||
      key.find("chat") != std::string::npos) {
    return true;
  }
  return false;
}

static bool ShouldRunItemImageSyncNow(const std::string &reason, bool force) {
  (void)force;
  if (g_itemImageSyncDisabledForSession) {
    return false;
  }
  std::string reasonKey = ToLowerAsciiCopy(TrimCopy(reason));
  if (!IsItemImageSyncReasonAllowed(reasonKey)) {
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
  std::string firstCaptureReason = "";
  std::string entriesJson = "[";
  for (std::map<std::string, Item *>::iterator it = uniqueByIdKey.begin();
       it != uniqueByIdKey.end(); ++it) {
    if (queued >= kItemImageBatchLimit) {
      break;
    }
    if (considered >= kItemImageBatchLimit * 6) {
      break;
    }
    ++considered;

    const std::string itemId = sourceItemIdByKey[it->first];
    Item *item = it->second;
    if (!item || (uintptr_t)item < 0x1000 || itemId.empty()) {
      continue;
    }

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
  if (g_itemImageSyncDisabledForSession) {
    return;
  }

  PendingItemImageSyncRequest req;
  bool hasRequest = false;
  DWORD nowTick = GetTickCount();
  EnterCriticalSection(&g_stateMutex);
  DWORD stableTick = g_worldStableSinceTick;
  DWORD sinceLastRun =
      g_itemImageLastRunTick == 0 ? 0 : (nowTick - g_itemImageLastRunTick);
  bool startupDelayPassed =
      stableTick != 0 && (nowTick - stableTick) >= kItemImageStartupDelayMs;
  bool cooldownPassed =
      g_itemImageLastRunTick == 0 || sinceLastRun >= kItemImageRunCooldownMs;

  if (startupDelayPassed && cooldownPassed && !g_itemImageSyncRequestQueue.empty()) {
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

  if (IsItemImageSyncReasonAllowed(reason)) {
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
      action = body->getCurrentActionOrMessage();
    } catch (...) {
      action = nullptr;
    }
    if (!action || (uintptr_t)action < 0x1000) {
      try {
        action = body->getCurrentAction();
      } catch (...) {
        action = nullptr;
      }
    }
    if (!action || (uintptr_t)action < 0x1000) {
      return NULL_TASK;
    }

    try {
      hand subject = body->getCurrentSubject();
      if (subject.isValid() && !subject.isNull()) {
        subjectOut = subject;
      }
    } catch (...) {
      subjectOut = hand();
    }

    TaskType taskKey = NULL_TASK;
    try {
      taskKey = action->key();
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
  std::string message = "Knocked out by " + EnsureLeadingArticle(attribution.weaponName);
  if (!attackerName.empty() && ToLowerAsciiCopy(attackerName) != "unknown" &&
      ToLowerAsciiCopy(attackerName) != ToLowerAsciiCopy(victimName)) {
    message += " from " + attackerName;
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
  LogGameEvent("item_pickup", actorName, actorFaction, "None", "None",
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
    hand currentTaskSubject;
    TaskType currentTaskNow = ResolveCurrentNpcTaskSafe(npc, currentTaskSubject);
    ConstructionActionEventType constructionActionNow =
        ResolveConstructionActionType(currentTaskNow);
    unsigned int constructionSubjectSerialNow = 0;
    std::string constructionSubjectNameNow = "";
    if (constructionActionNow != CONSTRUCTION_ACTION_NONE) {
      constructionSubjectSerialNow = currentTaskSubject.serial;
      constructionSubjectNameNow = ResolveConstructionSubjectName(currentTaskSubject);
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
      state.currentTask = (int)currentTaskNow;
      state.constructionAction = (int)constructionActionNow;
      state.constructionSubjectSerial = constructionSubjectSerialNow;
      state.constructionSubjectName = constructionSubjectNameNow;
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
    bool constructionChanged =
        state.constructionAction != (int)constructionActionNow;
    bool constructionTargetChanged =
        !constructionChanged &&
        constructionActionNow != CONSTRUCTION_ACTION_NONE &&
        ((constructionSubjectSerialNow != 0 &&
          constructionSubjectSerialNow != state.constructionSubjectSerial) ||
         ((constructionSubjectSerialNow == 0 || state.constructionSubjectSerial == 0) &&
          !constructionSubjectNameNow.empty() &&
          constructionSubjectNameNow != state.constructionSubjectName));
    if (constructionActionNow == CONSTRUCTION_ACTION_BUILD &&
        (constructionChanged || constructionTargetChanged)) {
      EmitBuildEvent(npc, constructionSubjectNameNow);
    } else if (constructionActionNow == CONSTRUCTION_ACTION_DISMANTLE &&
               (constructionChanged || constructionTargetChanged)) {
      EmitDismantleEvent(npc, constructionSubjectNameNow);
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
    state.currentTask = (int)currentTaskNow;
    state.constructionAction = (int)constructionActionNow;
    state.constructionSubjectSerial = constructionSubjectSerialNow;
    state.constructionSubjectName = constructionSubjectNameNow;
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
          Character *playerSpeaker = ResolvePlayerSpeakerForCurrentTalk(thisptr);
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

        // ???? FIX: For PLAYER_SAY, ensure the bubble appears over the player,
        // not the target NPC.
        if (isPlayerSay) {
          Character *playerSpeaker = ResolvePlayerSpeakerForCurrentTalk(thisptr);
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
            act.ttsHash = ttsHash;
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
                " tts_hash=" + (ttsHash.empty() ? "" : ttsHash.substr(0, 8)) +
                " tts_dur_ms=" + ToString(ttsDurationMs) +
                " player_tts_wait_hint=" +
                std::string(speechTimingMs < 0 ? "1" : "0"));
          } else {
            Log("HOOK_MSG_PROC: SAY fallback logged without target hand actor=" +
                std::string(tc ? tc->getName() : "Unknown"));
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
    if (itemToBuy && (uintptr_t)itemToBuy > 0x1000) {
      try {
        itemName = itemToBuy->getName();
      } catch (...) {
        itemName = "Unknown Item";
      }
    }
    if (itemName == "Unknown Item" && result && (uintptr_t)result > 0x1000) {
      try {
        itemName = result->getName();
      } catch (...) {
      }
    }

    std::string buyerName = ResolveRootObjectNameSafe(buyerObj);
    std::string buyerFaction = SafeFaction(buyerObj);
    std::string sellerName = ResolveRootObjectNameSafe(sellerObj);
    std::string sellerFaction = SafeFaction(sellerObj);

    if (!sellerObj || sellerName == buyerName) {
      sellerName = "None";
      sellerFaction = "None";
    }

    std::string message = "bought " + itemName;
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
  static bool heavySweepPrimed = false;
  static DWORD lastHeavySyncGuardLogTick = 0;
  static bool motdAutoOpenQueued = false;
  static DWORD motdAutoOpenTick = 0;
  static DWORD motdAutoOpenDeadlineTick = 0;
  static bool loadInitEventDispatched = false;
  bool worldStable = IsWorldStableForUI(worldUi);

  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) {
    // During loads MyGUI can be torn down; clear stale pointers and do nothing.
    CloseChatUI();
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
      g_itemImageSyncStateByItemId.clear();
      g_itemImageSyncRequestQueue.clear();
      g_itemImageLastRunTick = 0;
      g_worldStableSinceTick = 0;
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
      heavySweepPrimed = false;
      lastHeavySyncGuardLogTick = 0;
      motdAutoOpenQueued = false;
      motdAutoOpenTick = 0;
      motdAutoOpenDeadlineTick = 0;
      loadInitEventDispatched = false;
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
    if (g_chatWindow)
      CloseChatUI();
    return;
  }

  if (!worldWasStable) {
    worldWasStable = true;
    worldBecameStableTick = GetTickCount();
    EnterCriticalSection(&g_stateMutex);
    g_worldStableSinceTick = worldBecameStableTick;
    LeaveCriticalSection(&g_stateMutex);
    heavySweepPrimed = false;
    lastHeavySyncGuardLogTick = 0;
    motdAutoOpenQueued = false;
    motdAutoOpenTick = 0;
    motdAutoOpenDeadlineTick = 0;
    loadInitEventDispatched = false;
    Log("HOOK: world stable; delaying UI hook logic.");
    return;
  }

  // Save loads can expose unstable pointers for several seconds after world appears.
  if (GetTickCount() - worldBecameStableTick < 10000) {
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
  Character *sel = ResolveSelectedCharacterSehSafe(thisptr);
  if (logThisFrame) Log("HOOK_FRAME: selection done.");

  // Detect Selection Change
  EnterCriticalSection(&g_stateMutex);
  bool selectionChanged = false;
  hand currentSelectionHand;
  bool hasSelectionHandle = TryGetCharacterHandleSafe(sel, currentSelectionHand);
  if (hasSelectionHandle) {
    if (currentSelectionHand != g_lastSelectionHand) {
      std::string selectedName = "";
      try {
        selectedName = sel->getName();
      } catch (...) {
        selectedName = "";
      }
      g_activeCharName = selectedName;
      g_lastSelectionHand = currentSelectionHand;
      selectionChanged = true;
    }
  } else if (g_lastSelectionHand.isValid()) {
    g_activeCharName = "";
    g_lastSelectionHand = hand();
    selectionChanged = true;
  }
  LeaveCriticalSection(&g_stateMutex);
  if (logThisFrame) Log("HOOK_FRAME: selection change check done.");

  if (selectionChanged && hasSelectionHandle && sel &&
      (uintptr_t)sel > 0x1000) {
    if (ShouldProcessAnimalCharacter(sel)) {
      SyncInventoryForCharacter(sel, true, "selection_change");
      SyncPortraitForCharacter(sel, true, "selection_change");
      static DWORD lastSelectionContextPushTick = 0;
      static DWORD lastSelectionContextDelayLogTick = 0;
      DWORD nowSel = GetTickCount();
      bool selectionContextReady =
          (nowSel - worldBecameStableTick) >= kSelectionContextStartupDelayMs;
      if (selectionContextReady && nowSel - lastSelectionContextPushTick > 1000) {
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
      } else if (!selectionContextReady &&
                 nowSel - lastSelectionContextDelayLogTick >= 10000) {
        lastSelectionContextDelayLogTick = nowSel;
        Log("CONTEXT_PUSH: delayed selected character snapshot until startup window passes.");
      }
    } else {
      Log("ANIMAL_TALKS: ignoring selection context sync for inactive animal.");
    }
  }

  // 2. Message queue + queued actions
  GameWorld *world = GetWorldSafe();
  bool worldFrameStable = IsWorldStableForUI(world);
  if (world && worldFrameStable) {
    if (!loadInitEventDispatched) {
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
        std::string initMessage = "game load detected (fallback init event)";

        LogGameEvent("init", initActor, initFaction, "", "None", initMessage,
                     initActorSerial, 0);
        Log("LOAD_SYNC: narrator welcome stream unavailable; queued init fallback actor=" +
            initActor + " serial=" + ToString((int)initActorSerial));
      }
      loadInitEventDispatched = true;
    }

    ProcessMessageQueue(world);
    static int invTimer = 0;
    ExecuteQueuedActions(world, invTimer);
    ApplyFollowTargets(world);
    ApplyTravelTargets(world);
    RunQueuedItemImageSync();

    DWORD nowTick = GetTickCount();
    if (!heavySweepPrimed) {
      g_lastInventorySweepTick = nowTick;
      g_lastPortraitSweepTick = nowTick;
      g_lastNpcWorldEventSweepTick = nowTick;
      g_lastInfoNpcTelemetryCheckTick = nowTick;
      g_lastInfoLocTelemetryCheckTick = nowTick;
      heavySweepPrimed = true;
      Log("SYNC_GUARD: primed heavy periodic sweep timers.");
    }

    bool heavySyncReady = (nowTick - worldBecameStableTick) >= kHookHeavySyncWarmupMs;
    if (heavySyncReady) {
      RunInventorySyncSweep(world, sel);
      RunPlayerFactionPortraitSweep(world);
      RunNpcWorldEventSweep(world, sel);
      RunInfoTelemetrySweep(world, sel);
    } else if (nowTick - lastHeavySyncGuardLogTick >= 10000) {
      lastHeavySyncGuardLogTick = nowTick;
      Log("SYNC_GUARD: heavy periodic sweeps delayed until world warmup completes.");
    }
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

  const bool kEnableSpeechCaptureHooks = false;
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


