#include "AutonomyController.h"

#include "AutonomyExecutor.h"
#include "AutonomyMonitor.h"
#include "AutonomyProtocol.h"
#include "AutonomyReleaseGate.h"
#include "Comm.h"
#include "Globals.h"
#include "KenshiAiCompat.h"
#include "StobeText.h"
#include "Utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <deque>
#include <sstream>
#include <string>
#include <windows.h>

#include <kenshi/GameWorld.h>

namespace {

const DWORD kPollIntervalMs = 1500;
const DWORD kHeartbeatIntervalMs = 3000;
const DWORD kServerUnavailableMs = 8000;
const DWORD kMonitorIntervalMs = 250;
const DWORD kCooldownMs = 2000;
const size_t kReportQueueMax = 128;
const size_t kReportActionReserve = 4;

struct RuntimeReport {
  bool valid;
  bool action;
  bool terminal;
  long long revision;
  int npcId;
  std::string storageId;
  unsigned int runtimeSerial;
  std::string state;
  std::string observation;
  std::string error;
  std::string eventKey;
  std::string decisionId;
  std::string outcome;
  int activeElapsedMs;
  int gameTs;
  unsigned int retryCount;
  DWORD nextAttemptTick;

  RuntimeReport()
      : valid(false), action(false), terminal(false), revision(0), npcId(0),
        runtimeSerial(0), activeElapsedMs(0), gameTs(0), retryCount(0),
        nextAttemptTick(0) {}
};

struct TickRequest {
  bool valid;
  Stobe::Autonomy::ControlSnapshot control;
  Stobe::KenshiAi::CharacterSnapshot character;
  std::string state;
  unsigned int sequence;
  int gameTs;
  long long localTs;

  TickRequest()
      : valid(false), sequence(0), gameTs(0), localTs(0) {}
};

struct TickResult {
  bool ready;
  bool parsed;
  bool hasDecision;
  Stobe::Autonomy::DecisionEnvelope decision;

  TickResult() : ready(false), parsed(false), hasDecision(false) {}
};

struct ActiveAction {
  bool active;
  Stobe::Autonomy::DecisionEnvelope decision;
  Stobe::Autonomy::OrderFingerprint ownedOrder;
  DWORD lastMonitorTick;
  DWORD lastActiveTick;
  int activeElapsedMs;
  int deadlineMs;
  int stationarySamples;
  int pathFailedSamples;
  int nativeExpiredSamples;
  int noProgressElapsedMs;
  bool progressInitialized;
  double bestDistance;
  double startX;
  double startY;
  double startZ;
  bool awaitingExecutionResult;
  bool executionResultReady;
  bool executionSuccess;
  std::string executionReason;

  ActiveAction()
      : active(false), lastMonitorTick(0), lastActiveTick(0),
        activeElapsedMs(0), deadlineMs(0), stationarySamples(0),
        pathFailedSamples(0), nativeExpiredSamples(0), noProgressElapsedMs(0),
        progressInitialized(false), bestDistance(0.0), startX(0.0),
        startY(0.0), startZ(0.0), awaitingExecutionResult(false),
        executionResultReady(false), executionSuccess(false) {}
};

struct ActiveCatalogAction {
  bool active;
  Stobe::Autonomy::ControlSnapshot control;
  Stobe::Autonomy::DecisionEnvelope decision;
  DWORD startedTick;
  int gameTs;

  ActiveCatalogAction() : active(false), startedTick(0), gameTs(0) {}
};

CRITICAL_SECTION g_controlMutex;
LONG g_initialized = 0;
LONG g_threadStarted = 0;
Stobe::Autonomy::ControlSnapshot g_control;
DWORD g_lastServerSuccessTick = 0;
std::deque<RuntimeReport> g_reportQueue;
TickRequest g_tickRequest;
bool g_tickRequestOutstanding = false;
TickResult g_tickResult;
std::string g_terminalPendingDecisionId;

long long g_appliedRevision = -1;
unsigned int g_boundSerial = 0;
bool g_requiresExplicitResume = false;
std::string g_runtimeState = "DISABLED";
std::string g_runtimeReason = "autonomy_disabled";
unsigned int g_stateSequence = 0;
unsigned int g_tickSequence = 0;
DWORD g_lastQueuedHeartbeatTick = 0;
DWORD g_cooldownStartedTick = 0;
DWORD g_nextTickAllowedTick = 0;
ActiveAction g_active;
ActiveCatalogAction g_catalogActive;

void EnsureInitialized() {
  if (InterlockedCompareExchange(&g_initialized, 1, 0) == 0) {
    InitializeCriticalSection(&g_controlMutex);
    InterlockedExchange(&g_initialized, 2);
    return;
  }
  while (InterlockedCompareExchange(&g_initialized, 2, 2) != 2) {
    Sleep(0);
  }
}

int ResolveGameTs(GameWorld *world) {
  if (!world || reinterpret_cast<uintptr_t>(world) <= 0x1000) {
    return 0;
  }
  try {
    const int value = static_cast<int>(
        world->getTimeStamp_inGameHours().getTotalSeconds());
    return value > 0 ? value : 0;
  } catch (...) {
    return 0;
  }
}

bool JsonResponseOk(const std::string &response) {
  const std::string ok = Stobe::Text::JsonReadField(response, "ok");
  return ok == "true" || ok == "1";
}

bool JsonResponseIsFinalRejection(const std::string &response) {
  const std::string error = Stobe::Text::JsonReadField(response, "error");
  return error == "stale_control_revision" ||
         error == "npc_identity_mismatch" ||
         error == "decision_identity_mismatch" ||
         error == "illegal_decision_transition" ||
         error == "invalid_action_observation";
}

std::string BuildReportJson(const RuntimeReport &report) {
  std::ostringstream json;
  json << "{\"control_revision\":" << report.revision
       << ",\"npc_id\":" << report.npcId
       << ",\"npc_storage_id\":\""
       << Stobe::Text::EscapeJSON(report.storageId)
       << "\",\"runtime_serial\":" << report.runtimeSerial;
  if (report.action) {
    json << ",\"report_type\":\"action\",\"decision_id\":\""
         << Stobe::Text::EscapeJSON(report.decisionId)
         << "\",\"outcome\":\"" << Stobe::Text::EscapeJSON(report.outcome)
         << "\",\"reason\":\""
         << Stobe::Text::EscapeJSON(report.observation)
         << "\",\"active_elapsed_ms\":" << report.activeElapsedMs;
  } else {
    json << ",\"state\":\"" << Stobe::Text::EscapeJSON(report.state)
         << "\",\"observation\":\""
         << Stobe::Text::EscapeJSON(report.observation)
         << "\",\"error\":\"" << Stobe::Text::EscapeJSON(report.error)
         << "\",\"event_type\":\"plugin_state\"";
  }
  json << ",\"event_key\":\"" << Stobe::Text::EscapeJSON(report.eventKey)
       << "\",\"game_ts\":" << report.gameTs << "}";
  return json.str();
}

void HashContextValue(unsigned long long &hash, unsigned long long value) {
  const unsigned long long prime = 1099511628211ull;
  for (int index = 0; index < 8; ++index) {
    hash ^= (value >> (index * 8)) & 0xffull;
    hash *= prime;
  }
}

void HashContextString(unsigned long long &hash, const std::string &value) {
  const unsigned long long prime = 1099511628211ull;
  for (size_t index = 0; index < value.size(); ++index) {
    hash ^= static_cast<unsigned char>(value[index]);
    hash *= prime;
  }
}

const char *TypedDecisionPhase(
    Stobe::Autonomy::DecisionCommand command) {
  switch (command) {
  case Stobe::Autonomy::DECISION_COMMAND_MOVE_NEARBY:
  case Stobe::Autonomy::DECISION_COMMAND_FLEE:
  case Stobe::Autonomy::DECISION_COMMAND_FIRST_AID:
  case Stobe::Autonomy::DECISION_COMMAND_REST:
    return "PHASE4";
  case Stobe::Autonomy::DECISION_COMMAND_ATTACK:
  case Stobe::Autonomy::DECISION_COMMAND_TAKE_ITEM:
  case Stobe::Autonomy::DECISION_COMMAND_EQUIP_ITEM:
  case Stobe::Autonomy::DECISION_COMMAND_KNOCKOUT:
  case Stobe::Autonomy::DECISION_COMMAND_KILL:
  case Stobe::Autonomy::DECISION_COMMAND_REMOVE_LIMB:
  case Stobe::Autonomy::DECISION_COMMAND_CUT_HORNS:
    return "PHASE5";
  case Stobe::Autonomy::DECISION_COMMAND_BUY_ITEM:
  case Stobe::Autonomy::DECISION_COMMAND_SELL_ITEM:
  case Stobe::Autonomy::DECISION_COMMAND_WORK_RESOURCE:
  case Stobe::Autonomy::DECISION_COMMAND_PROSPECT:
    return "PHASE6";
  default:
    return "PHASE2";
  }
}

std::string BuildStableContextHash(const TickRequest &tick) {
  unsigned long long hash = 1469598103934665603ull;
  HashContextValue(hash, static_cast<unsigned long long>(tick.control.controlRevision));
  HashContextValue(hash, tick.character.runtimeSerial);
  HashContextValue(hash, static_cast<unsigned long long>(
                             static_cast<long long>(tick.character.x / 10.0)));
  HashContextValue(hash, static_cast<unsigned long long>(
                             static_cast<long long>(tick.character.y / 10.0)));
  HashContextValue(hash, static_cast<unsigned long long>(
                             static_cast<long long>(tick.character.z / 10.0)));
  HashContextValue(hash, static_cast<unsigned long long>(tick.character.order.orderCount));
  HashContextValue(hash, static_cast<unsigned long long>(tick.character.order.taskType));
  HashContextValue(hash, tick.character.dead ? 1 : 0);
  HashContextValue(hash, tick.character.unconscious ? 1 : 0);
  HashContextValue(hash, tick.character.carrying ? 1 : 0);
  HashContextValue(hash, tick.character.carriedSerial);
  HashContextValue(hash, static_cast<unsigned long long>(tick.character.firstAidNeed * 100.0));
  HashContextValue(hash, static_cast<unsigned long long>(tick.character.roboticAidNeed * 100.0));
  HashContextValue(hash, tick.character.fullyRested ? 1 : 0);
  HashContextValue(hash, tick.character.restBedAvailable ? 1 : 0);
  HashContextValue(hash, static_cast<unsigned long long>(tick.character.cats));
  HashContextValue(
      hash, static_cast<unsigned long long>(tick.character.inventoryItemCount));
  HashContextString(hash, tick.character.aiCurrentGoal);
  HashContextValue(hash, tick.character.aiTaskExpired ? 1 : 0);
  HashContextValue(hash, tick.character.aiGoalExpired ? 1 : 0);
  HashContextValue(hash,
                   static_cast<unsigned long long>(
                       tick.character.aiPathFailureCount));
  HashContextValue(hash, tick.character.aiIntendsToAttackTarget ? 1 : 0);
  for (size_t i = 0; i < tick.character.nearbyActors.size(); ++i) {
    const Stobe::KenshiAi::NearbyActorSnapshot &actor =
        tick.character.nearbyActors[i];
    HashContextValue(hash, actor.runtimeSerial);
    HashContextValue(hash, actor.dead ? 1 : 0);
    HashContextValue(hash, actor.unconscious ? 1 : 0);
    HashContextValue(hash, actor.hostile ? 1 : 0);
    HashContextValue(hash, actor.trader ? 1 : 0);
    HashContextValue(hash, static_cast<unsigned long long>(actor.cats));
    HashContextValue(hash, static_cast<unsigned long long>(actor.firstAidNeed * 100.0));
    HashContextValue(hash, static_cast<unsigned long long>(actor.roboticAidNeed * 100.0));
    for (size_t itemIndex = 0; itemIndex < actor.traderItems.size();
         ++itemIndex) {
      HashContextString(hash, actor.traderItems[itemIndex].name);
      HashContextValue(
          hash,
          static_cast<unsigned long long>(actor.traderItems[itemIndex].count));
      HashContextValue(hash, static_cast<unsigned long long>(
                                 actor.traderItems[itemIndex].buyValueEach));
      HashContextValue(hash, static_cast<unsigned long long>(
                                 actor.traderItems[itemIndex].sellValueEach));
    }
  }
  for (size_t i = 0; i < tick.character.inventoryItems.size(); ++i) {
    HashContextString(hash, tick.character.inventoryItems[i].name);
    HashContextValue(
        hash,
        static_cast<unsigned long long>(tick.character.inventoryItems[i].count));
    HashContextValue(hash, static_cast<unsigned long long>(
                               tick.character.inventoryItems[i].sellValueEach));
  }
  for (size_t i = 0; i < tick.character.nearbyResources.size(); ++i) {
    const Stobe::KenshiAi::NearbyResourceSnapshot &resource =
        tick.character.nearbyResources[i];
    HashContextValue(hash, resource.runtimeSerial);
    HashContextValue(hash, resource.usable ? 1 : 0);
  }
  std::ostringstream result;
  result << "phase6-" << hash;
  return result.str();
}

std::string BuildTickJson(const TickRequest &tick) {
  std::ostringstream json;
  json << "{\"control_revision\":" << tick.control.controlRevision
       << ",\"npc_id\":" << tick.control.npcId
       << ",\"npc_storage_id\":\""
       << Stobe::Text::EscapeJSON(tick.control.npcStorageId)
       << "\",\"runtime_serial\":" << tick.character.runtimeSerial
       << ",\"state\":\"" << Stobe::Text::EscapeJSON(tick.state)
       << "\",\"observation\":\"phase_6_runtime_snapshot\""
       << ",\"event_key\":\"\",\"snapshot_sequence\":" << tick.sequence
       << ",\"snapshot_local_ts\":" << tick.localTs
       << ",\"game_ts\":" << tick.gameTs
       << ",\"context_hash\":\"" << BuildStableContextHash(tick) << "\""
       << ",\"position\":{\"x\":" << tick.character.x
       << ",\"y\":" << tick.character.y << ",\"z\":" << tick.character.z
       << "},\"order\":{\"count\":" << tick.character.order.orderCount
       << ",\"task\":" << tick.character.order.taskType
       << ",\"subject_serial\":" << tick.character.order.subjectSerial
       << "},\"status\":{\"player_character\":"
       << (tick.character.playerCharacter ? "true" : "false")
       << ",\"dead\":" << (tick.character.dead ? "true" : "false")
       << ",\"unconscious\":"
       << (tick.character.unconscious ? "true" : "false")
       << ",\"can_take_orders\":"
       << (tick.character.canTakeOrders ? "true" : "false")
       << ",\"has_player_orders\":"
       << (tick.character.hasPlayerOrders ? "true" : "false")
       << ",\"carrying\":" << (tick.character.carrying ? "true" : "false")
       << ",\"carried_serial\":" << tick.character.carriedSerial
       << ",\"in_bed\":" << (tick.character.inBed ? "true" : "false")
       << ",\"fully_rested\":"
       << (tick.character.fullyRested ? "true" : "false")
       << ",\"probably_dying\":"
       << (tick.character.probablyDying ? "true" : "false")
       << ",\"rest_bed_available\":"
       << (tick.character.restBedAvailable ? "true" : "false")
       << ",\"in_combat\":"
       << (tick.character.inCombat ? "true" : "false")
       << "},\"health\":{\"overall\":" << tick.character.overallHealth
       << ",\"blood\":" << tick.character.blood
       << ",\"max_blood\":" << tick.character.maxBlood
       << ",\"bleed_rate\":" << tick.character.bleedRate
       << ",\"first_aid_need\":" << tick.character.firstAidNeed
       << ",\"robotic_aid_need\":" << tick.character.roboticAidNeed
       << "},\"economy\":{\"cats\":" << tick.character.cats
       << ",\"inventory_item_count\":" << tick.character.inventoryItemCount
       << "},\"movement\":{\"moving\":"
       << (tick.character.moving ? "true" : "false")
       << ",\"path_failed\":"
       << (tick.character.pathFailed ? "true" : "false")
       << "},\"ai\":{\"current_goal\":\""
       << Stobe::Text::EscapeJSON(tick.character.aiCurrentGoal)
       << "\",\"task_expired\":"
       << (tick.character.aiTaskExpired ? "true" : "false")
       << ",\"goal_expired\":"
       << (tick.character.aiGoalExpired ? "true" : "false")
       << ",\"path_failure_count\":"
       << tick.character.aiPathFailureCount
       << ",\"intends_to_attack_target\":"
       << (tick.character.aiIntendsToAttackTarget ? "true" : "false")
       << "}}";
  std::string built = json.str();
  if (!built.empty() && built[built.size() - 1] == '}') {
    built.erase(built.size() - 1);
  }
  built += ",\"nearby_actors\":[";
  for (size_t i = 0; i < tick.character.nearbyActors.size(); ++i) {
    const Stobe::KenshiAi::NearbyActorSnapshot &actor =
        tick.character.nearbyActors[i];
    if (i > 0) {
      built += ",";
    }
    std::ostringstream item;
    item << "{\"name\":\"" << Stobe::Text::EscapeJSON(actor.name)
         << "\",\"runtime_serial\":" << actor.runtimeSerial
         << ",\"distance\":" << actor.distance
         << ",\"x\":" << actor.x << ",\"y\":" << actor.y
         << ",\"z\":" << actor.z
         << ",\"player_character\":"
         << (actor.playerCharacter ? "true" : "false")
         << ",\"trader\":" << (actor.trader ? "true" : "false")
         << ",\"cats\":" << actor.cats
         << ",\"dead\":" << (actor.dead ? "true" : "false")
         << ",\"unconscious\":"
         << (actor.unconscious ? "true" : "false")
         << ",\"hostile\":" << (actor.hostile ? "true" : "false")
         << ",\"overall_health\":" << actor.overallHealth
         << ",\"bleed_rate\":" << actor.bleedRate
         << ",\"first_aid_need\":" << actor.firstAidNeed
         << ",\"robotic_aid_need\":" << actor.roboticAidNeed
         << ",\"trader_items\":[";
    for (size_t itemIndex = 0; itemIndex < actor.traderItems.size();
         ++itemIndex) {
      const Stobe::KenshiAi::InventoryItemSnapshot &tradeItem =
          actor.traderItems[itemIndex];
      if (itemIndex > 0) {
        item << ",";
      }
      item << "{\"name\":\"" << Stobe::Text::EscapeJSON(tradeItem.name)
           << "\",\"count\":" << tradeItem.count
           << ",\"buy_value_each\":" << tradeItem.buyValueEach
           << ",\"sell_value_each\":" << tradeItem.sellValueEach << "}";
    }
    item << "]}";
    built += item.str();
  }
  built += "],\"inventory_items\":[";
  for (size_t i = 0; i < tick.character.inventoryItems.size(); ++i) {
    const Stobe::KenshiAi::InventoryItemSnapshot &inventoryItem =
        tick.character.inventoryItems[i];
    if (i > 0) {
      built += ",";
    }
    std::ostringstream item;
    item << "{\"name\":\""
         << Stobe::Text::EscapeJSON(inventoryItem.name)
         << "\",\"count\":" << inventoryItem.count
         << ",\"buy_value_each\":" << inventoryItem.buyValueEach
         << ",\"sell_value_each\":" << inventoryItem.sellValueEach << "}";
    built += item.str();
  }
  built += "],\"nearby_resources\":[";
  for (size_t i = 0; i < tick.character.nearbyResources.size(); ++i) {
    const Stobe::KenshiAi::NearbyResourceSnapshot &resource =
        tick.character.nearbyResources[i];
    if (i > 0) {
      built += ",";
    }
    std::ostringstream item;
    item << "{\"name\":\"" << Stobe::Text::EscapeJSON(resource.name)
         << "\",\"runtime_serial\":" << resource.runtimeSerial
         << ",\"distance\":" << resource.distance
         << ",\"natural\":" << (resource.natural ? "true" : "false")
         << ",\"usable\":" << (resource.usable ? "true" : "false")
         << ",\"task\":" << resource.taskType << ",\"x\":" << resource.x
         << ",\"y\":" << resource.y << ",\"z\":" << resource.z << "}";
    built += item.str();
  }
  built += "]}";
  return built;
}

bool QueueCatalogAction(const Stobe::Autonomy::ControlSnapshot &control,
                        const Stobe::Autonomy::DecisionEnvelope &decision) {
  if (decision.command !=
          Stobe::Autonomy::DECISION_COMMAND_CATALOG_ACTION ||
      decision.commandName.empty() || decision.actionArgument.size() > 1200 ||
      control.npcName.empty()) {
    return false;
  }
  const std::string prefix = decision.commandName == "TALK"
                                 ? "NPC_SAY: "
                                 : "NPC_ACTION: ";
  std::string message = prefix + control.npcName + "|" +
                        ToString(decision.runtimeSerial) + ": ";
  if (decision.commandName == "TALK") {
    if (decision.actionArgument.empty()) {
      return false;
    }
    message += decision.actionArgument;
  } else {
    const std::string adapterCommand =
        decision.commandName == "DRINK" ? "DRINK_ITEM" : decision.commandName;
    message += adapterCommand + "@" + decision.actionArgument;
  }
  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.push_back(message);
  LeaveCriticalSection(&g_msgMutex);
  RegisterPendingAutonomyCatalogMessage(message, decision.decisionId);
  return true;
}

void QueueReportInternal(const RuntimeReport &report) {
  EnterCriticalSection(&g_controlMutex);
  if (!report.action && report.eventKey.empty()) {
    for (std::deque<RuntimeReport>::iterator it = g_reportQueue.begin();
         it != g_reportQueue.end(); ++it) {
      if (!it->action && it->eventKey.empty()) {
        *it = report;
        LeaveCriticalSection(&g_controlMutex);
        return;
      }
    }
  }
  const size_t limit = report.action
                           ? kReportQueueMax
                           : kReportQueueMax - kReportActionReserve;
  while (g_reportQueue.size() >= limit) {
    std::deque<RuntimeReport>::iterator disposable = g_reportQueue.end();
    for (std::deque<RuntimeReport>::iterator it = g_reportQueue.begin();
         it != g_reportQueue.end(); ++it) {
      if (!it->action) {
        disposable = it;
        break;
      }
    }
    if (disposable == g_reportQueue.end()) {
      if (!report.action) {
        LeaveCriticalSection(&g_controlMutex);
        return;
      }
      break;
    }
    g_reportQueue.erase(disposable);
  }
  g_reportQueue.push_back(report);
  if (report.terminal) {
    g_terminalPendingDecisionId = report.decisionId;
  }
  LeaveCriticalSection(&g_controlMutex);
}

void QueueStateReport(const Stobe::Autonomy::ControlSnapshot &control,
                      unsigned int runtimeSerial, const std::string &state,
                      const std::string &reason, int gameTs,
                      bool stateChanged) {
  RuntimeReport report;
  report.valid = control.valid;
  report.revision = control.controlRevision;
  report.npcId = control.npcId;
  report.storageId = control.npcStorageId;
  report.runtimeSerial = runtimeSerial;
  report.state = state;
  report.observation = reason;
  report.error = state == "ERROR" ? reason : "";
  report.gameTs = gameTs;
  if (stateChanged) {
    ++g_stateSequence;
    report.eventKey = "plugin:" + ToString(static_cast<int>(control.controlRevision)) +
                      ":" + state + ":" + ToString(g_stateSequence);
  }
  QueueReportInternal(report);
}

void QueueActionReport(const Stobe::Autonomy::ControlSnapshot &control,
                       const Stobe::Autonomy::DecisionEnvelope &decision,
                       const std::string &outcome, const std::string &reason,
                       int activeElapsedMs, int gameTs, bool terminal) {
  RuntimeReport report;
  report.valid = true;
  report.action = true;
  report.terminal = terminal;
  report.revision = control.controlRevision;
  report.npcId = control.npcId;
  report.storageId = control.npcStorageId;
  report.runtimeSerial = decision.runtimeSerial;
  report.decisionId = decision.decisionId;
  report.outcome = outcome;
  report.observation = reason;
  report.activeElapsedMs = activeElapsedMs;
  report.gameTs = gameTs;
  report.eventKey = "action:" + decision.decisionId + ":" + outcome;
  QueueReportInternal(report);
}

void PublishRuntimeState(const Stobe::Autonomy::ControlSnapshot &control,
                         unsigned int serial, const std::string &state,
                         const std::string &reason, const std::string &name,
                         int gameTs, bool force) {
  const bool changed = force || state != g_runtimeState || reason != g_runtimeReason;
  g_runtimeState = state;
  g_runtimeReason = reason;
  g_boundSerial = serial;
  const DWORD now = GetTickCount();
  const bool heartbeat = g_lastQueuedHeartbeatTick == 0 ||
                         now - g_lastQueuedHeartbeatTick >= kHeartbeatIntervalMs;
  if (changed || heartbeat) {
    g_lastQueuedHeartbeatTick = now;
    QueueStateReport(control, serial, state, reason, gameTs, changed);
  }
  if (changed) {
    Log("AUTONOMY_PHASE3_STATE: revision=" +
        ToString(static_cast<int>(control.controlRevision)) + " state=" + state +
        " reason=" + reason + " serial=" + ToString(serial) + " name=\"" +
        name + "\"");
  }
}

bool HasTerminalPending() {
  EnterCriticalSection(&g_controlMutex);
  const bool pending = !g_terminalPendingDecisionId.empty();
  LeaveCriticalSection(&g_controlMutex);
  return pending;
}

void QueueTickRequest(const Stobe::Autonomy::ControlSnapshot &control,
                      const Stobe::KenshiAi::CharacterSnapshot &character,
                      int gameTs) {
  EnterCriticalSection(&g_controlMutex);
  if (!g_tickRequestOutstanding && !g_tickResult.ready) {
    g_tickRequest = TickRequest();
    g_tickRequest.valid = true;
    g_tickRequest.control = control;
    g_tickRequest.character = character;
    g_tickRequest.state = "DECIDING";
    g_tickRequest.sequence = ++g_tickSequence;
    g_tickRequest.gameTs = gameTs;
    g_tickRequest.localTs = static_cast<long long>(time(NULL));
    g_tickRequestOutstanding = true;
  }
  LeaveCriticalSection(&g_controlMutex);
}

bool TakeTickResult(TickResult &out) {
  EnterCriticalSection(&g_controlMutex);
  if (!g_tickResult.ready) {
    LeaveCriticalSection(&g_controlMutex);
    return false;
  }
  out = g_tickResult;
  g_tickResult = TickResult();
  LeaveCriticalSection(&g_controlMutex);
  return true;
}

void ClearTickState() {
  EnterCriticalSection(&g_controlMutex);
  g_tickRequest = TickRequest();
  g_tickRequestOutstanding = false;
  g_tickResult = TickResult();
  LeaveCriticalSection(&g_controlMutex);
}

DWORD WINAPI AutonomyPollThread(LPVOID) {
  DWORD lastPollTick = 0;
  while (true) {
    const DWORD now = GetTickCount();
    if (lastPollTick == 0 || now - lastPollTick >= kPollIntervalMs) {
      lastPollTick = now;
      Stobe::Autonomy::ControlSnapshot parsed;
      const std::string response =
          PostToStobeWithResponse(L"/autonomy_state", "");
      if (Stobe::Autonomy::ParseControlResponse(response, parsed)) {
        EnterCriticalSection(&g_controlMutex);
        g_control = parsed;
        g_lastServerSuccessTick = GetTickCount();
        LeaveCriticalSection(&g_controlMutex);
      }
    }

    RuntimeReport report;
    bool hasReport = false;
    EnterCriticalSection(&g_controlMutex);
    if (!g_reportQueue.empty() &&
        (g_reportQueue.front().nextAttemptTick == 0 ||
         static_cast<LONG>(now - g_reportQueue.front().nextAttemptTick) >= 0)) {
      report = g_reportQueue.front();
      hasReport = true;
    }
    LeaveCriticalSection(&g_controlMutex);
    if (hasReport) {
      const std::string response = PostToStobeWithResponse(
          L"/autonomy_observation", BuildReportJson(report));
      if (JsonResponseOk(response) || JsonResponseIsFinalRejection(response)) {
        EnterCriticalSection(&g_controlMutex);
        if (!g_reportQueue.empty() &&
            g_reportQueue.front().eventKey == report.eventKey &&
            g_reportQueue.front().decisionId == report.decisionId) {
          g_reportQueue.pop_front();
        }
        if (report.terminal &&
            g_terminalPendingDecisionId == report.decisionId) {
          g_terminalPendingDecisionId.clear();
        }
        g_lastServerSuccessTick = GetTickCount();
        LeaveCriticalSection(&g_controlMutex);
      } else {
        unsigned int retryCount = 0;
        DWORD retryDelay = 0;
        EnterCriticalSection(&g_controlMutex);
        if (!g_reportQueue.empty() &&
            g_reportQueue.front().eventKey == report.eventKey &&
            g_reportQueue.front().decisionId == report.decisionId) {
          RuntimeReport &pending = g_reportQueue.front();
          pending.retryCount = std::min<unsigned int>(pending.retryCount + 1, 6);
          retryCount = pending.retryCount;
          retryDelay = std::min<DWORD>(10000, 500u << (retryCount - 1));
          pending.nextAttemptTick = GetTickCount() + retryDelay;
        }
        LeaveCriticalSection(&g_controlMutex);
        if (retryCount > 0) {
          Log("AUTONOMY_PHASE2_REPORT_RETRY: event=" + report.eventKey +
              " attempt=" + ToString(static_cast<int>(retryCount)) +
              " delay_ms=" + ToString(static_cast<int>(retryDelay)));
        }
      }
    }

    TickRequest tick;
    bool hasTick = false;
    EnterCriticalSection(&g_controlMutex);
    if (g_tickRequest.valid) {
      tick = g_tickRequest;
      g_tickRequest.valid = false;
      hasTick = true;
    }
    LeaveCriticalSection(&g_controlMutex);
    if (hasTick) {
      const std::string response =
          PostToStobeWithResponse(L"/autonomy_tick", BuildTickJson(tick));
      TickResult result;
      result.ready = true;
      result.parsed = Stobe::Autonomy::ParseDecisionResponse(
          response, result.decision, result.hasDecision);
      EnterCriticalSection(&g_controlMutex);
      g_tickResult = result;
      g_tickRequestOutstanding = false;
      if (result.parsed) {
        g_lastServerSuccessTick = GetTickCount();
      }
      LeaveCriticalSection(&g_controlMutex);
    }
    Sleep(100);
  }
  return 0;
}

void ClearActiveAction(GameWorld *world, bool attemptCancel,
                       const std::string &reason) {
  if (!g_active.active) {
    return;
  }
  if (attemptCancel) {
    std::string cancelReason;
    bool cancelled = false;
    if (g_active.awaitingExecutionResult) {
      CancelPendingAutonomyCatalogDecision(g_active.decision.decisionId);
      cancelled = true;
      cancelReason = "queued_action_cancelled";
    } else if (world) {
      cancelled = Stobe::Autonomy::TryCancelOwnedOrder(
          world, g_active.decision.runtimeSerial, g_active.ownedOrder,
          cancelReason);
    } else {
      cancelReason = "world_unavailable";
    }
    Log(std::string("AUTONOMY_") + TypedDecisionPhase(g_active.decision.command) +
        "_CANCEL: decision=" + g_active.decision.decisionId +
        " requested_reason=" + reason + " result=" +
        (cancelled ? "1" : "0") + " reason=" + cancelReason);
  }
  g_active = ActiveAction();
}

void FinishActiveAction(GameWorld *world,
                        const Stobe::Autonomy::ControlSnapshot &control,
                        Stobe::Autonomy::MonitorOutcome outcome,
                        const std::string &reason, int gameTs) {
  if (!g_active.active) {
    return;
  }
  const Stobe::Autonomy::DecisionEnvelope decision = g_active.decision;
  const int elapsed = g_active.activeElapsedMs;
  if (g_active.awaitingExecutionResult) {
    CancelPendingAutonomyCatalogDecision(decision.decisionId);
  } else if (outcome != Stobe::Autonomy::MONITOR_INTERRUPTED) {
    std::string cancelReason;
    Stobe::Autonomy::TryCancelOwnedOrder(
        world, decision.runtimeSerial, g_active.ownedOrder, cancelReason);
  }
  const std::string outcomeName = Stobe::Autonomy::MonitorOutcomeName(outcome);
  QueueActionReport(control, decision, outcomeName, reason, elapsed, gameTs,
                    true);
  Log(std::string("AUTONOMY_") + TypedDecisionPhase(decision.command) +
      "_TERMINAL: decision=" + decision.decisionId +
      " command=" + decision.commandName + " outcome=" + outcomeName +
      " reason=" + reason + " elapsed_ms=" + ToString(elapsed));
  g_active = ActiveAction();
  g_cooldownStartedTick = GetTickCount();
  if (outcome == Stobe::Autonomy::MONITOR_INTERRUPTED &&
      reason == "manual_player_order_detected") {
    g_requiresExplicitResume = true;
    g_runtimeState = "PAUSED_USER";
    g_runtimeReason = reason;
  } else if (outcome == Stobe::Autonomy::MONITOR_UNSAFE) {
    g_requiresExplicitResume = true;
    g_runtimeState = "PAUSED_UNSAFE";
    g_runtimeReason = reason;
  } else {
    g_runtimeState = "COOLDOWN";
    g_runtimeReason = reason;
  }
}

} // namespace

void StartAutonomyController() {
  if (!Stobe::AutonomyReleaseGate::kEnabled) {
    Log("AUTONOMY: disabled for this beta release");
    return;
  }
  EnsureInitialized();
  if (InterlockedCompareExchange(&g_threadStarted, 1, 0) != 0) {
    return;
  }
  HANDLE thread = CreateThread(NULL, 0, AutonomyPollThread, NULL, 0, NULL);
  if (thread) {
    CloseHandle(thread);
    Log("AUTONOMY_PHASE3: supervised planner control loop started");
  } else {
    InterlockedExchange(&g_threadStarted, 0);
    Log("AUTONOMY_PHASE2_ERROR: failed to start control polling thread");
  }
}

void ResetAutonomyController(const char *reason) {
  if (!Stobe::AutonomyReleaseGate::kEnabled) {
    return;
  }
  EnsureInitialized();
  Stobe::Autonomy::ControlSnapshot control;
  EnterCriticalSection(&g_controlMutex);
  control = g_control;
  LeaveCriticalSection(&g_controlMutex);
  if (!control.valid || !control.enabled) {
    g_boundSerial = 0;
    g_catalogActive = ActiveCatalogAction();
    return;
  }

  const std::string resetReason = reason ? reason : "world_transition";
  if (g_catalogActive.active && control.valid) {
    CancelPendingAutonomyCatalogDecision(
        g_catalogActive.decision.decisionId);
    QueueActionReport(control, g_catalogActive.decision, "INTERRUPTED",
                      resetReason,
                      static_cast<int>(GetTickCount() -
                                       g_catalogActive.startedTick),
                      0, true);
  }
  if (g_active.active && control.valid) {
    if (g_active.awaitingExecutionResult) {
      CancelPendingAutonomyCatalogDecision(g_active.decision.decisionId);
    }
    QueueActionReport(control, g_active.decision, "INTERRUPTED", resetReason,
                      g_active.activeElapsedMs, 0, true);
    g_active = ActiveAction();
  }
  g_catalogActive = ActiveCatalogAction();
  ClearTickState();
  g_boundSerial = 0;
  const bool changed = !g_requiresExplicitResume ||
                       g_runtimeState != "PAUSED_UNSAFE" ||
                       g_runtimeReason != resetReason;
  g_requiresExplicitResume = true;
  g_runtimeState = "PAUSED_UNSAFE";
  g_runtimeReason = resetReason;
  if (changed) {
    QueueStateReport(control, 0, g_runtimeState, g_runtimeReason, 0, true);
    Log("AUTONOMY_PHASE2: paused for world transition reason=" + resetReason);
  }
}

void UpdateAutonomyController(GameWorld *world) {
  if (!Stobe::AutonomyReleaseGate::kEnabled) {
    return;
  }
  EnsureInitialized();
  Stobe::Autonomy::ControlSnapshot control;
  DWORD lastServerSuccess = 0;
  bool tickRequestOutstanding = false;
  EnterCriticalSection(&g_controlMutex);
  control = g_control;
  lastServerSuccess = g_lastServerSuccessTick;
  tickRequestOutstanding = g_tickRequestOutstanding;
  LeaveCriticalSection(&g_controlMutex);
  if (!control.valid) {
    return;
  }

  const bool revisionChanged = control.controlRevision != g_appliedRevision;
  if (revisionChanged) {
    ClearActiveAction(world, true, "control_revision_changed");
    ClearTickState();
    if (g_catalogActive.active) {
      CancelPendingAutonomyCatalogDecision(
          g_catalogActive.decision.decisionId);
    }
    g_appliedRevision = control.controlRevision;
    g_catalogActive = ActiveCatalogAction();
    g_boundSerial = 0;
    g_cooldownStartedTick = 0;
    g_nextTickAllowedTick = 0;
    if (control.enabled && control.desiredState == "ARMING") {
      g_requiresExplicitResume = false;
    }
  }

  const int gameTs = ResolveGameTs(world);
  if (!control.enabled || control.desiredState == "DISABLED") {
    ClearActiveAction(world, true, "autonomy_disabled");
    PublishRuntimeState(control, 0, "DISABLED", "autonomy_disabled", "",
                        gameTs, revisionChanged);
    return;
  }
  if (control.desiredState == "PAUSED_USER") {
    ClearActiveAction(world, true, "paused_by_user");
    g_requiresExplicitResume = true;
    PublishRuntimeState(control, 0, "PAUSED_USER", "paused_by_user", "",
                        gameTs, revisionChanged);
    return;
  }

  const unsigned int expectedSerial =
      Stobe::Autonomy::ParseStorageSerial(control.npcStorageId);
  Stobe::KenshiAi::CharacterSnapshot character =
      Stobe::KenshiAi::CaptureCharacter(world, expectedSerial);

  if (g_catalogActive.active) {
    const DWORD elapsed = GetTickCount() - g_catalogActive.startedTick;
    if (elapsed >= 3000) {
      CancelPendingAutonomyCatalogDecision(
          g_catalogActive.decision.decisionId);
      QueueActionReport(g_catalogActive.control, g_catalogActive.decision,
                        "FAILED", "catalog_adapter_rejected", elapsed, gameTs,
                        true);
      Log("AUTONOMY_PHASE3_TERMINAL: decision=" +
          g_catalogActive.decision.decisionId +
          " outcome=FAILED reason=catalog_adapter_rejected");
      g_catalogActive = ActiveCatalogAction();
      g_cooldownStartedTick = GetTickCount();
      PublishRuntimeState(control, character.runtimeSerial, "COOLDOWN",
                          "catalog_adapter_rejected", character.name, gameTs,
                          true);
    } else {
      PublishRuntimeState(control, character.runtimeSerial, "EXECUTING",
                          "catalog_adapter_executing", character.name, gameTs,
                          false);
    }
    return;
  }

  if (character.paused) {
    PublishRuntimeState(control, character.runtimeSerial, "OBSERVING",
                        "game_paused", character.name, gameTs,
                        revisionChanged);
    return;
  }

  if (g_active.active) {
    const DWORD now = GetTickCount();
    if (g_active.lastMonitorTick != 0 &&
        now - g_active.lastMonitorTick < kMonitorIntervalMs) {
      PublishRuntimeState(control, g_active.decision.runtimeSerial,
                          "EXECUTING", "action_in_progress", character.name,
                          gameTs, false);
      return;
    }
    const DWORD delta = g_active.lastActiveTick == 0
                            ? 0
                            : now - g_active.lastActiveTick;
    g_active.lastActiveTick = now;
    g_active.lastMonitorTick = now;
    if (!character.paused) {
      g_active.activeElapsedMs += static_cast<int>(delta);
      if (character.moving) {
        g_active.stationarySamples = 0;
      } else {
        ++g_active.stationarySamples;
      }
      if (character.pathFailed) {
        ++g_active.pathFailedSamples;
      } else {
        g_active.pathFailedSamples = 0;
      }
      if (character.aiTaskExpired && character.aiGoalExpired) {
        ++g_active.nativeExpiredSamples;
      } else {
        g_active.nativeExpiredSamples = 0;
      }
      if (g_active.decision.command ==
              Stobe::Autonomy::DECISION_COMMAND_TRAVEL_LOCATION ||
          g_active.decision.command ==
              Stobe::Autonomy::DECISION_COMMAND_MOVE_NEARBY ||
          g_active.decision.command ==
              Stobe::Autonomy::DECISION_COMMAND_FLEE) {
        const double dx = character.x - g_active.decision.x;
        const double dy = character.y - g_active.decision.y;
        const double dz = character.z - g_active.decision.z;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!g_active.progressInitialized ||
            distance + 2.0 < g_active.bestDistance) {
          g_active.progressInitialized = true;
          g_active.bestDistance = distance;
          g_active.noProgressElapsedMs = 0;
        } else {
          g_active.noProgressElapsedMs += static_cast<int>(delta);
        }
      } else if (g_active.decision.command ==
                 Stobe::Autonomy::DECISION_COMMAND_FIRST_AID) {
        double need = 0.0;
        bool foundTarget = false;
        if (g_active.decision.targetRuntimeSerial == character.runtimeSerial) {
          need = std::max(character.firstAidNeed, character.roboticAidNeed);
          foundTarget = true;
        } else {
          for (size_t i = 0; i < character.nearbyActors.size(); ++i) {
            if (character.nearbyActors[i].runtimeSerial ==
                g_active.decision.targetRuntimeSerial) {
              need = std::max(character.nearbyActors[i].firstAidNeed,
                              character.nearbyActors[i].roboticAidNeed);
              foundTarget = true;
              break;
            }
          }
        }
        if (foundTarget && (!g_active.progressInitialized ||
                            need + 0.05 < g_active.bestDistance)) {
          g_active.progressInitialized = true;
          g_active.bestDistance = need;
          g_active.noProgressElapsedMs = 0;
        } else {
          g_active.noProgressElapsedMs += static_cast<int>(delta);
        }
      }
    }

    if (g_active.awaitingExecutionResult) {
      if (g_active.executionResultReady) {
        FinishActiveAction(
            world, control,
            g_active.executionSuccess ? Stobe::Autonomy::MONITOR_COMPLETED
                                      : Stobe::Autonomy::MONITOR_FAILED,
            g_active.executionReason.empty()
                ? (g_active.executionSuccess ? "action_postcondition_verified"
                                             : "action_postcondition_failed")
                : g_active.executionReason,
            gameTs);
        PublishRuntimeState(control, 0, g_runtimeState, g_runtimeReason,
                            character.name, gameTs, true);
        return;
      }
      if (g_active.deadlineMs > 0 &&
          g_active.activeElapsedMs >= g_active.deadlineMs) {
        FinishActiveAction(world, control, Stobe::Autonomy::MONITOR_TIMED_OUT,
                           "queued_action_deadline_exceeded", gameTs);
        PublishRuntimeState(control, 0, g_runtimeState, g_runtimeReason,
                            character.name, gameTs, true);
        return;
      }
      PublishRuntimeState(control, g_active.decision.runtimeSerial,
                          "EXECUTING", "awaiting_action_postcondition",
                          character.name, gameTs, false);
      return;
    }

    Stobe::Autonomy::MonitorFacts facts;
    facts.found = character.found;
    facts.identityMatches = character.identityMatches;
    facts.playerCharacter = character.playerCharacter;
    facts.dead = character.dead;
    facts.unconscious = character.unconscious;
    facts.paused = character.paused;
    facts.moving = character.moving;
    facts.pathFailed = character.pathFailed;
    facts.hasPlayerOrders = character.hasPlayerOrders;
    facts.fullyRested = character.fullyRested;
    facts.inBed = character.inBed;
    facts.inCombat = character.inCombat;
    facts.nativeTaskExpired = character.aiTaskExpired;
    facts.nativeGoalExpired = character.aiGoalExpired;
    facts.nativeIntendsToAttackTarget =
        character.aiIntendsToAttackTarget;
    facts.nativeCurrentGoal = character.aiCurrentGoal;
    facts.nativePathFailureCount = character.aiPathFailureCount;
    facts.attackTargetSerial = character.attackTargetSerial;
    facts.firstAidNeed = std::max(character.firstAidNeed,
                                  character.roboticAidNeed);
    facts.bleedRate = character.bleedRate;
    facts.pathFailedSamples = g_active.pathFailedSamples;
    facts.nativeExpiredSamples = g_active.nativeExpiredSamples;
    facts.stationarySamples = g_active.stationarySamples;
    facts.activeElapsedMs = g_active.activeElapsedMs;
    facts.noProgressElapsedMs = g_active.noProgressElapsedMs;
    facts.x = character.x;
    facts.y = character.y;
    facts.z = character.z;
    const double fleeDx = character.x - g_active.startX;
    const double fleeDy = character.y - g_active.startY;
    const double fleeDz = character.z - g_active.startZ;
    facts.fleeDistanceTravelled =
        std::sqrt(fleeDx * fleeDx + fleeDy * fleeDy + fleeDz * fleeDz);
    facts.currentOrder = character.order;
    facts.nearestHostileDistance = 1000000.0;
    for (size_t i = 0; i < character.nearbyActors.size(); ++i) {
      const Stobe::KenshiAi::NearbyActorSnapshot &actor =
          character.nearbyActors[i];
      if (actor.hostile && !actor.dead) {
        facts.hostileObserved = true;
        facts.nearestHostileDistance =
            std::min(facts.nearestHostileDistance, actor.distance);
      }
      if (actor.runtimeSerial == g_active.decision.targetRuntimeSerial) {
        facts.targetFound = true;
        facts.targetDead = actor.dead;
        facts.targetUnconscious = actor.unconscious;
        facts.targetFirstAidNeed =
            std::max(actor.firstAidNeed, actor.roboticAidNeed);
        facts.targetBleedRate = actor.bleedRate;
      }
    }
    if (g_active.decision.targetRuntimeSerial == character.runtimeSerial) {
      facts.targetFound = true;
      facts.targetDead = character.dead;
      facts.targetUnconscious = character.unconscious;
      facts.targetFirstAidNeed =
          std::max(character.firstAidNeed, character.roboticAidNeed);
      facts.targetBleedRate = character.bleedRate;
    }
    const Stobe::Autonomy::MonitorResult monitored =
        Stobe::Autonomy::EvaluateActionMonitor(
            g_active.decision, g_active.ownedOrder, facts,
            g_active.deadlineMs);
    if (monitored.outcome != Stobe::Autonomy::MONITOR_RUNNING) {
      FinishActiveAction(world, control, monitored.outcome, monitored.reason,
                         gameTs);
      PublishRuntimeState(control, 0, g_runtimeState, g_runtimeReason,
                          character.name, gameTs, true);
      return;
    }
    PublishRuntimeState(control, g_active.decision.runtimeSerial, "EXECUTING",
                        monitored.reason, character.name, gameTs, false);
    return;
  }

  Stobe::Autonomy::RuntimeFacts facts;
  facts.serverAvailable = tickRequestOutstanding ||
                          (lastServerSuccess != 0 &&
                           GetTickCount() - lastServerSuccess <=
                               kServerUnavailableMs);
  facts.requiresExplicitResume = g_requiresExplicitResume;
  facts.manualPauseLatched = g_requiresExplicitResume &&
                             g_runtimeState == "PAUSED_USER" &&
                             !revisionChanged;
  facts.found = character.found;
  facts.identityMatches = character.identityMatches;
  facts.playerCharacter = character.playerCharacter;
  facts.dead = character.dead;
  facts.unconscious = character.unconscious;
  facts.hasOrdersReceiver = character.hasOrdersReceiver;
  facts.canTakeOrders = character.canTakeOrders;
  facts.hasPlayerOrders = character.hasPlayerOrders;

  const Stobe::Autonomy::StateDecision safeState =
      Stobe::Autonomy::EvaluatePhase1State(control, facts);
  if (safeState.state != "OBSERVING") {
    if (safeState.state == "PAUSED_USER") {
      g_requiresExplicitResume = true;
    }
    PublishRuntimeState(control, 0, safeState.state, safeState.reason,
                        character.name, gameTs, revisionChanged);
    return;
  }

  if (HasTerminalPending() ||
      (g_cooldownStartedTick != 0 &&
       GetTickCount() - g_cooldownStartedTick < kCooldownMs)) {
    PublishRuntimeState(control, character.runtimeSerial, "COOLDOWN",
                        "awaiting_terminal_ack", character.name, gameTs,
                        revisionChanged);
    return;
  }
  g_cooldownStartedTick = 0;

  TickResult tickResult;
  if (TakeTickResult(tickResult)) {
    g_nextTickAllowedTick = GetTickCount() + kPollIntervalMs;
    if (!tickResult.parsed) {
      PublishRuntimeState(control, character.runtimeSerial, "OBSERVING",
                          "tick_response_invalid", character.name, gameTs,
                          true);
      return;
    }
    if (!tickResult.hasDecision) {
      PublishRuntimeState(control, character.runtimeSerial, "OBSERVING",
                          "pilot_queue_empty", character.name, gameTs, true);
      return;
    }
    if (tickResult.hasDecision) {
      const Stobe::Autonomy::DecisionValidation validation =
          Stobe::Autonomy::ValidateDecisionEnvelope(
              tickResult.decision, control, character.runtimeSerial,
              static_cast<long long>(time(NULL)));
      if (validation != Stobe::Autonomy::DECISION_VALID) {
        const std::string reason =
            Stobe::Autonomy::DecisionValidationName(validation);
        QueueActionReport(control, tickResult.decision,
                          validation == Stobe::Autonomy::DECISION_EXPIRED
                              ? "TIMED_OUT"
                              : "FAILED",
                          reason, 0, gameTs, true);
        g_cooldownStartedTick = GetTickCount();
        PublishRuntimeState(control, 0, "COOLDOWN", reason, character.name,
                            gameTs, true);
        return;
      }

      PublishRuntimeState(control, character.runtimeSerial, "ACTION_QUEUED",
                          "decision_validated", character.name, gameTs, true);
      if (tickResult.decision.command ==
          Stobe::Autonomy::DECISION_COMMAND_CATALOG_ACTION) {
        if (!QueueCatalogAction(control, tickResult.decision)) {
          QueueActionReport(control, tickResult.decision, "FAILED",
                            "catalog_action_rejected", 0, gameTs, true);
          g_cooldownStartedTick = GetTickCount();
          PublishRuntimeState(control, 0, "COOLDOWN",
                              "catalog_action_rejected", character.name,
                              gameTs, true);
          return;
        }
        QueueActionReport(control, tickResult.decision, "DISPATCHED",
                          "validated_catalog_adapter", 0, gameTs, false);
        g_catalogActive = ActiveCatalogAction();
        g_catalogActive.active = true;
        g_catalogActive.control = control;
        g_catalogActive.decision = tickResult.decision;
        g_catalogActive.startedTick = GetTickCount();
        g_catalogActive.gameTs = gameTs;
        Log("AUTONOMY_PHASE3_DISPATCH: decision=" +
            tickResult.decision.decisionId + " command=" +
            tickResult.decision.commandName + " serial=" +
            ToString(tickResult.decision.runtimeSerial));
        PublishRuntimeState(control, character.runtimeSerial, "EXECUTING",
                            "catalog_adapter_executing", character.name,
                            gameTs, true);
        return;
      }
      const Stobe::Autonomy::DispatchResult dispatched =
          Stobe::Autonomy::DispatchDecision(world, tickResult.decision);
      if (!dispatched.success) {
        if (dispatched.ownedOrder.orderCount == 1) {
          std::string cancelReason;
          Stobe::Autonomy::TryCancelOwnedOrder(
              world, tickResult.decision.runtimeSerial,
              dispatched.ownedOrder, cancelReason);
        }
        const std::string outcome = dispatched.reason == "existing_player_order"
                                        ? "INTERRUPTED"
                                        : "FAILED";
        QueueActionReport(control, tickResult.decision, outcome,
                          dispatched.reason, 0, gameTs, true);
        g_cooldownStartedTick = GetTickCount();
        if (outcome == "INTERRUPTED") {
          g_requiresExplicitResume = true;
          PublishRuntimeState(control, 0, "PAUSED_USER",
                              "manual_player_order_detected", character.name,
                              gameTs, true);
        } else {
          PublishRuntimeState(control, 0, "COOLDOWN", dispatched.reason,
                              character.name, gameTs, true);
        }
        return;
      }

      g_active = ActiveAction();
      g_active.active = true;
      g_active.decision = tickResult.decision;
      g_active.ownedOrder = dispatched.ownedOrder;
      g_active.lastActiveTick = GetTickCount();
      g_active.startX = character.x;
      g_active.startY = character.y;
      g_active.startZ = character.z;
      g_active.awaitingExecutionResult =
          dispatched.awaitingExecutionResult;
      const long long remaining =
          tickResult.decision.actionDeadlineTs -
          static_cast<long long>(time(NULL));
      g_active.deadlineMs = static_cast<int>(
          std::max<long long>(1, std::min<long long>(remaining, 3600)) * 1000);
      QueueActionReport(control, tickResult.decision, "DISPATCHED",
                        dispatched.reason, 0, gameTs, false);
      if (dispatched.completedImmediately) {
        QueueActionReport(control, tickResult.decision, "COMPLETED",
                          dispatched.reason, 0, gameTs, true);
        Log("AUTONOMY_PHASE5_TERMINAL: decision=" +
            tickResult.decision.decisionId + " command=" +
            tickResult.decision.commandName +
            " outcome=COMPLETED reason=" + dispatched.reason);
        g_active = ActiveAction();
        g_cooldownStartedTick = GetTickCount();
        g_runtimeState = "COOLDOWN";
        g_runtimeReason = dispatched.reason;
        PublishRuntimeState(control, 0, g_runtimeState, g_runtimeReason,
                            character.name, gameTs, true);
        return;
      }
      Log(std::string("AUTONOMY_") +
          TypedDecisionPhase(tickResult.decision.command) +
          "_DISPATCH: decision=" +
          tickResult.decision.decisionId + " command=" +
          tickResult.decision.commandName + " serial=" +
          ToString(tickResult.decision.runtimeSerial) +
          " jobs_preserved=" + (dispatched.jobsPreserved ? "1" : "0"));
      PublishRuntimeState(control, character.runtimeSerial, "EXECUTING",
                          "owned_order_accepted", character.name, gameTs,
                          true);
      return;
    }
  }

  if (g_nextTickAllowedTick != 0 && GetTickCount() < g_nextTickAllowedTick) {
    PublishRuntimeState(control, character.runtimeSerial, "OBSERVING",
                        "pilot_queue_empty", character.name, gameTs, false);
    return;
  }

  Character *restCandidate =
      Stobe::KenshiAi::ResolveCharacter(world, character.runtimeSerial);
  character.restBedAvailable = character.inBed ||
      Stobe::KenshiAi::ResolveNearestRestBed(world, restCandidate, 250.0) != NULL;
  QueueTickRequest(control, character, gameTs);
  PublishRuntimeState(control, character.runtimeSerial, "DECIDING",
                      "planner_tick_requested", character.name, gameTs,
                      revisionChanged);
}

void ReportAutonomyActionExecutionResult(const std::string &decisionId,
                                         bool success,
                                         const std::string &reason) {
  if (!Stobe::AutonomyReleaseGate::kEnabled) {
    return;
  }
  if (g_active.active && g_active.awaitingExecutionResult &&
      !decisionId.empty() &&
      g_active.decision.decisionId == decisionId) {
    g_active.executionResultReady = true;
    g_active.executionSuccess = success;
    g_active.executionReason = reason;
    return;
  }
  if (!g_catalogActive.active || decisionId.empty() ||
      g_catalogActive.decision.decisionId != decisionId) {
    return;
  }
  const int elapsed = static_cast<int>(GetTickCount() -
                                       g_catalogActive.startedTick);
  QueueActionReport(g_catalogActive.control, g_catalogActive.decision,
                    success ? "COMPLETED" : "FAILED", reason, elapsed,
                    g_catalogActive.gameTs, true);
  Log("AUTONOMY_PHASE3_TERMINAL: decision=" + decisionId + " command=" +
      g_catalogActive.decision.commandName + " outcome=" +
      (success ? "COMPLETED" : "FAILED") + " reason=" + reason);
  g_catalogActive = ActiveCatalogAction();
  g_cooldownStartedTick = GetTickCount();
  g_runtimeState = "COOLDOWN";
  g_runtimeReason = reason;
}
