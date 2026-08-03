#include "WorldStateRuntime.h"

#include "Comm.h"
#include "Globals.h"
#include "Utils.h"
#include "WorldStateCatalog.h"

#include <kenshi/Enums.h>
#include <kenshi/GameData.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/GameWorld.h>
#include <kenshi/WorldEventStateQuery.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

struct EvaluatedQuery {
  std::string id;
  std::string name;
  bool result;
  int gameTs;
};

struct DefinitionRule {
  std::string category;
  std::string targetId;
  std::string targetName;
  std::string targetType;
  int expectedValue;
  std::string conditionText;
  std::string inverseText;
};

struct RuntimeDefinition {
  std::string id;
  std::string name;
  std::string sourceMod;
  bool playerInvolvement;
  std::vector<DefinitionRule> rules;
};

static const DWORD kStartupDelayMs = 30000;
static const DWORD kBatchIntervalMs = 250;
static const DWORD kSweepIntervalMs = 60000;
static const DWORD kDiscoveryRetryMs = 10000;
static const size_t kBatchSize = 4;
static const size_t kMaxDiscoveredQueries = 4096;
static const int kMaxDiscoveryAttempts = 3;

size_t g_queryCursor = 0;
DWORD g_lastBatchTick = 0;
DWORD g_nextSweepTick = 0;
DWORD g_nextDiscoveryTick = 0;
bool g_hasSentSnapshot = false;
bool g_wrongThreadLogged = false;
bool g_discoveryReady = false;
bool g_discoveryTruncated = false;
int g_discoveryAttempts = 0;
DWORD g_lastSehCode = 0;
std::vector<std::string> g_queryIds;
std::vector<EvaluatedQuery> g_sweepResults;
std::vector<RuntimeDefinition> g_sweepDefinitions;
std::map<std::string, bool> g_lastSentResults;

int ResolveGameTs(GameWorld *world) {
  if (!world || reinterpret_cast<uintptr_t>(world) <= 0x1000) {
    return 0;
  }
  try {
    TimeOfDay tod = world->getTimeStamp_inGameHours();
    const int gameTs = static_cast<int>(tod.getTotalSeconds());
    return gameTs > 0 ? gameTs : 0;
  } catch (...) {
    return 0;
  }
}

std::string SourceModFromQueryId(const std::string &queryId) {
  const size_t separator = queryId.find('-');
  if (separator == std::string::npos || separator + 1 >= queryId.size()) {
    return "";
  }
  return queryId.substr(separator + 1);
}

std::string TargetTypeForCategory(const std::string &category) {
  if (category == "NPC is" || category == "NPC is NOT") {
    return "CHARACTER";
  }
  if (category == "player ally" || category == "player enemy") {
    return "FACTION";
  }
  if (category == "town okay") {
    return "TOWN";
  }
  return "UNKNOWN";
}

std::string RuleText(const std::string &category, int value,
                     const std::string &targetName, bool inverse) {
  if (category == "NPC is" || category == "NPC is NOT") {
    bool negated = category == "NPC is NOT";
    if (inverse) {
      negated = !negated;
    }
    const char *state = value == 0 ? "dead" : (value == 1 ? "alive" : "imprisoned");
    return targetName + (negated ? " is not " : " is ") + state;
  }
  if (category == "player ally") {
    bool expected = value == 1;
    if (inverse) {
      expected = !expected;
    }
    return targetName +
           (expected ? " is allied with the player faction"
                     : " is not allied with the player faction");
  }
  if (category == "player enemy") {
    bool expected = value == 1;
    if (inverse) {
      expected = !expected;
    }
    return targetName +
           (expected ? " is an enemy of the player faction"
                     : " is not an enemy of the player faction");
  }
  if (category == "town okay") {
    bool intact = value == 1;
    if (inverse) {
      intact = !intact;
    }
    return targetName + (intact ? " is intact" : " is destroyed");
  }
  return targetName + " has world-state value " + ToString(value);
}

bool DiscoverQueryIdsUnsafe(GameWorld *world, std::vector<std::string> &idsOut,
                            bool &truncatedOut) {
  idsOut.clear();
  truncatedOut = false;
  if (!world || reinterpret_cast<uintptr_t>(world) <= 0x1000) {
    return false;
  }

  const int category = static_cast<int>(WORLD_EVENT_STATE);
  const auto categoryIt = world->gamedata.gamedataCatSID.find(category);
  if (categoryIt == world->gamedata.gamedataCatSID.end()) {
    return false;
  }
  const auto &queries = categoryIt->second;
  for (auto queryIt = queries.begin(); queryIt != queries.end(); ++queryIt) {
    if (!queryIt->first.empty()) {
      idsOut.push_back(queryIt->first);
    }
  }

  std::sort(idsOut.begin(), idsOut.end());
  idsOut.erase(std::unique(idsOut.begin(), idsOut.end()), idsOut.end());
  if (idsOut.size() > kMaxDiscoveredQueries) {
    idsOut.resize(kMaxDiscoveredQueries);
    truncatedOut = true;
  }
  return !idsOut.empty();
}

int WorldStateSehFilter(unsigned int code) {
  g_lastSehCode = code;
  return EXCEPTION_EXECUTE_HANDLER;
}

bool DiscoverQueryIds(GameWorld *world, std::vector<std::string> &idsOut,
                      bool &truncatedOut) {
  __try {
    return DiscoverQueryIdsUnsafe(world, idsOut, truncatedOut);
  } __except (WorldStateSehFilter(GetExceptionCode())) {
    return false;
  }
}

bool ReadQueryUnsafe(GameWorld *world, const std::string &queryId,
                     bool includeDefinition, EvaluatedQuery &resultOut,
                     RuntimeDefinition &definitionOut) {
  if (!world || queryId.empty()) {
    return false;
  }

  GameData *data = world->gamedata.getData(queryId, WORLD_EVENT_STATE);
  if (!data || reinterpret_cast<uintptr_t>(data) <= 0x1000) {
    return false;
  }

  WorldEventStateQuery *query = WorldEventStateQuery::getFromData(data);
  if (!query || reinterpret_cast<uintptr_t>(query) <= 0x1000) {
    return false;
  }

  resultOut.id = queryId;
  resultOut.name = data->name;
  resultOut.result = query->isTrue();
  resultOut.gameTs = ResolveGameTs(world);

  if (!includeDefinition) {
    return true;
  }

  definitionOut.id = queryId;
  definitionOut.name = data->name;
  definitionOut.sourceMod = SourceModFromQueryId(queryId);
  definitionOut.playerInvolvement = query->playerInvolvement;
  definitionOut.rules.clear();

  static const char *const categories[] = {
      "NPC is", "NPC is NOT", "player ally", "player enemy", "town okay"};
  for (size_t categoryIndex = 0;
       categoryIndex < sizeof(categories) / sizeof(categories[0]);
       ++categoryIndex) {
    const std::string category = categories[categoryIndex];
    const Ogre::vector<GameDataReference>::type *references =
        data->getReferenceListIfExists(category);
    if (!references) {
      continue;
    }

    for (size_t referenceIndex = 0; referenceIndex < references->size();
         ++referenceIndex) {
      const GameDataReference &reference = (*references)[referenceIndex];
      if (reference.sid.empty()) {
        continue;
      }
      DefinitionRule rule;
      rule.category = category;
      rule.targetId = reference.sid;
      rule.targetName = reference.sid;
      rule.targetType = TargetTypeForCategory(category);
      rule.expectedValue = reference.values[0];

      GameData *target = world->gamedata.getData(reference.sid);
      if (target && reinterpret_cast<uintptr_t>(target) > 0x1000) {
        rule.targetName = target->name;
      }
      rule.conditionText =
          RuleText(category, rule.expectedValue, rule.targetName, false);
      rule.inverseText =
          RuleText(category, rule.expectedValue, rule.targetName, true);
      definitionOut.rules.push_back(rule);
    }
  }
  return true;
}

bool ReadQuery(GameWorld *world, const std::string &queryId,
               bool includeDefinition, EvaluatedQuery &resultOut,
               RuntimeDefinition &definitionOut) {
  __try {
    return ReadQueryUnsafe(world, queryId, includeDefinition, resultOut,
                           definitionOut);
  } __except (WorldStateSehFilter(GetExceptionCode())) {
    return false;
  }
}

std::string RuntimeCatalogId(const std::vector<std::string> &queryIds) {
  unsigned long long hash = 1469598103934665603ULL;
  for (size_t i = 0; i < queryIds.size(); ++i) {
    for (size_t j = 0; j < queryIds[i].size(); ++j) {
      hash ^= static_cast<unsigned char>(queryIds[i][j]);
      hash *= 1099511628211ULL;
    }
    hash ^= static_cast<unsigned char>('\n');
    hash *= 1099511628211ULL;
  }
  char buffer[32] = {};
  sprintf_s(buffer, sizeof(buffer), "fnv1a64:%016llx", hash);
  return buffer;
}

std::string BuildPayload(const std::vector<EvaluatedQuery> &results,
                         const std::vector<RuntimeDefinition> &definitions,
                         bool fullSnapshot) {
  const int gameTs = results.empty() ? 0 : results.front().gameTs;
  std::string payload = "{";
  payload += "\"source\":\"world_event_state_snapshot\",";
  payload += "\"game_ts\":" + ToString(gameTs) + ",";
  payload += "\"catalog_schema_version\":" +
             ToString(Stobe::WorldStateCatalog::kSchemaVersion) + ",";
  payload += "\"catalog_sha256\":\"" +
             std::string(Stobe::WorldStateCatalog::kCatalogSha256) + "\",";
  payload += "\"runtime_catalog_id\":\"" +
             EscapeJSON(RuntimeCatalogId(g_queryIds)) + "\",";
  payload += "\"full_snapshot\":" +
             std::string(fullSnapshot ? "true" : "false") + ",";
  payload += "\"definitions_full_snapshot\":" +
             std::string(fullSnapshot ? "true" : "false") + ",";
  payload += "\"query_count\":" + ToString(static_cast<int>(g_queryIds.size())) +
             ",";
  payload += "\"definition_count\":" +
             ToString(static_cast<int>(definitions.size())) + ",";
  payload += "\"changed_count\":" + ToString(static_cast<int>(results.size())) +
             ",";
  payload += "\"discovery_truncated\":" +
             std::string(g_discoveryTruncated ? "true" : "false") + ",";
  payload += "\"definitions\":[";
  for (size_t i = 0; i < definitions.size(); ++i) {
    if (i > 0) {
      payload += ",";
    }
    payload += "{";
    payload += "\"query_id\":\"" + EscapeJSON(definitions[i].id) + "\",";
    payload += "\"query_name\":\"" + EscapeJSON(definitions[i].name) + "\",";
    payload +=
        "\"source_mod\":\"" + EscapeJSON(definitions[i].sourceMod) + "\",";
    payload += "\"player_involvement\":" +
               std::string(definitions[i].playerInvolvement ? "true"
                                                            : "false") +
               ",";
    payload += "\"rules\":[";
    for (size_t ruleIndex = 0;
         ruleIndex < definitions[i].rules.size(); ++ruleIndex) {
      if (ruleIndex > 0) {
        payload += ",";
      }
      const DefinitionRule &rule = definitions[i].rules[ruleIndex];
      payload += "{";
      payload += "\"category\":\"" + EscapeJSON(rule.category) + "\",";
      payload += "\"target_id\":\"" + EscapeJSON(rule.targetId) + "\",";
      payload += "\"target_name\":\"" + EscapeJSON(rule.targetName) + "\",";
      payload += "\"target_type\":\"" + EscapeJSON(rule.targetType) + "\",";
      payload += "\"expected_value\":" + ToString(rule.expectedValue) + ",";
      payload +=
          "\"condition_text\":\"" + EscapeJSON(rule.conditionText) + "\",";
      payload += "\"inverse_text\":\"" + EscapeJSON(rule.inverseText) + "\"";
      payload += "}";
    }
    payload += "]}";
  }
  payload += "],\"results\":[";
  for (size_t i = 0; i < results.size(); ++i) {
    if (i > 0) {
      payload += ",";
    }
    payload += "{";
    payload += "\"query_id\":\"" + EscapeJSON(results[i].id) + "\",";
    payload += "\"query_name\":\"" + EscapeJSON(results[i].name) + "\",";
    payload += "\"result\":" +
               std::string(results[i].result ? "true" : "false") + ",";
    payload += "\"game_ts\":" + ToString(results[i].gameTs);
    payload += "}";
  }
  payload += "]}";
  return payload;
}

void CompleteSweep(DWORD nowTick) {
  std::vector<EvaluatedQuery> changed;
  const bool fullSnapshot = !g_hasSentSnapshot;

  for (size_t i = 0; i < g_sweepResults.size(); ++i) {
    const EvaluatedQuery &entry = g_sweepResults[i];
    std::map<std::string, bool>::const_iterator previous =
        g_lastSentResults.find(entry.id);
    if (fullSnapshot || previous == g_lastSentResults.end() ||
        previous->second != entry.result) {
      changed.push_back(entry);
    }
  }

  if (fullSnapshot || !changed.empty()) {
    AsyncPostToStobeSerial(
        L"/world_state",
        BuildPayload(changed, fullSnapshot ? g_sweepDefinitions
                                           : std::vector<RuntimeDefinition>(),
                     fullSnapshot));
    for (size_t i = 0; i < changed.size(); ++i) {
      g_lastSentResults[changed[i].id] = changed[i].result;
    }
    g_hasSentSnapshot = true;
    Log("WORLD_STATE_RUNTIME: dispatched full=" +
        std::string(fullSnapshot ? "1" : "0") +
        " discovered=" + ToString(static_cast<int>(g_queryIds.size())) +
        " evaluated=" + ToString(static_cast<int>(g_sweepResults.size())) +
        " definitions=" +
        ToString(static_cast<int>(g_sweepDefinitions.size())) +
        " changed=" + ToString(static_cast<int>(changed.size())));
  }

  g_sweepResults.clear();
  g_sweepDefinitions.clear();
  g_queryCursor = 0;
  g_nextSweepTick = nowTick + kSweepIntervalMs;
}

void UseCompiledCatalogFallback() {
  g_queryIds.assign(
      Stobe::WorldStateCatalog::kQueryIds,
      Stobe::WorldStateCatalog::kQueryIds +
          Stobe::WorldStateCatalog::kQueryCount);
  g_discoveryReady = true;
  g_discoveryTruncated = false;
  Log("WORLD_STATE_DISCOVERY: using compiled vanilla fallback count=" +
      ToString(static_cast<int>(g_queryIds.size())) +
      " seh_code=" + ToString(static_cast<int>(g_lastSehCode)));
}

} // namespace

namespace Stobe {
namespace WorldStateRuntime {

void Reset() {
  g_queryCursor = 0;
  g_lastBatchTick = 0;
  g_nextSweepTick = 0;
  g_nextDiscoveryTick = 0;
  g_hasSentSnapshot = false;
  g_wrongThreadLogged = false;
  g_discoveryReady = false;
  g_discoveryTruncated = false;
  g_discoveryAttempts = 0;
  g_lastSehCode = 0;
  g_queryIds.clear();
  g_sweepResults.clear();
  g_sweepDefinitions.clear();
  g_lastSentResults.clear();
}

void Update(GameWorld *world, DWORD worldStableSinceTick) {
  if (!world || worldStableSinceTick == 0) {
    return;
  }

  const DWORD nowTick = GetTickCount();
  if ((nowTick - worldStableSinceTick) < kStartupDelayMs) {
    return;
  }
  if (g_mainThreadId != 0 && GetCurrentThreadId() != g_mainThreadId) {
    if (!g_wrongThreadLogged) {
      Log("WORLD_STATE_RUNTIME: skipped non-main-thread evaluation");
      g_wrongThreadLogged = true;
    }
    return;
  }

  if (!g_discoveryReady) {
    if (g_nextDiscoveryTick != 0 &&
        static_cast<LONG>(nowTick - g_nextDiscoveryTick) < 0) {
      return;
    }

    std::vector<std::string> discovered;
    bool truncated = false;
    Log("WORLD_STATE_DISCOVERY: starting category scan attempt=" +
        ToString(g_discoveryAttempts + 1));
    if (DiscoverQueryIds(world, discovered, truncated)) {
      g_queryIds.swap(discovered);
      g_discoveryReady = true;
      g_discoveryTruncated = truncated;
      Log("WORLD_STATE_DISCOVERY: complete count=" +
          ToString(static_cast<int>(g_queryIds.size())) +
          " truncated=" + std::string(truncated ? "1" : "0"));
    } else {
      ++g_discoveryAttempts;
      Log("WORLD_STATE_DISCOVERY: attempt failed attempt=" +
          ToString(g_discoveryAttempts) +
          " seh_code=" + ToString(static_cast<int>(g_lastSehCode)));
      if (g_discoveryAttempts >= kMaxDiscoveryAttempts) {
        UseCompiledCatalogFallback();
      } else {
        g_nextDiscoveryTick = nowTick + kDiscoveryRetryMs;
      }
    }
    return;
  }

  if (g_queryCursor == 0 && g_nextSweepTick != 0 &&
      static_cast<LONG>(nowTick - g_nextSweepTick) < 0) {
    return;
  }
  if (g_lastBatchTick != 0 &&
      (nowTick - g_lastBatchTick) < kBatchIntervalMs) {
    return;
  }
  g_lastBatchTick = nowTick;

  const size_t end =
      (g_queryCursor + kBatchSize < g_queryIds.size())
          ? g_queryCursor + kBatchSize
          : g_queryIds.size();
  const bool includeDefinitions = !g_hasSentSnapshot;
  for (; g_queryCursor < end; ++g_queryCursor) {
    EvaluatedQuery evaluated;
    RuntimeDefinition definition;
    if (ReadQuery(world, g_queryIds[g_queryCursor], includeDefinitions,
                  evaluated, definition)) {
      g_sweepResults.push_back(evaluated);
      if (includeDefinitions) {
        g_sweepDefinitions.push_back(definition);
      }
    } else if (g_lastSehCode != 0) {
      Log("WORLD_STATE_RUNTIME: skipped unsafe query id=" +
          g_queryIds[g_queryCursor] +
          " seh_code=" + ToString(static_cast<int>(g_lastSehCode)));
      g_lastSehCode = 0;
    }
  }

  if (g_queryCursor >= g_queryIds.size()) {
    CompleteSweep(nowTick);
  }
}

} // namespace WorldStateRuntime
} // namespace Stobe
