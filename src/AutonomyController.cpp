#include "AutonomyController.h"

#include "AutonomyExecutor.h"
#include "AutonomyMonitor.h"
#include "AutonomyProtocol.h"
#include "Comm.h"
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
  int noProgressElapsedMs;
  bool progressInitialized;
  double bestDistance;

  ActiveAction()
      : active(false), lastMonitorTick(0), lastActiveTick(0),
        activeElapsedMs(0), deadlineMs(0), stationarySamples(0),
        pathFailedSamples(0), noProgressElapsedMs(0),
        progressInitialized(false), bestDistance(0.0) {}
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

std::string BuildTickJson(const TickRequest &tick) {
  std::ostringstream json;
  json << "{\"control_revision\":" << tick.control.controlRevision
       << ",\"npc_id\":" << tick.control.npcId
       << ",\"npc_storage_id\":\""
       << Stobe::Text::EscapeJSON(tick.control.npcStorageId)
       << "\",\"runtime_serial\":" << tick.character.runtimeSerial
       << ",\"state\":\"" << Stobe::Text::EscapeJSON(tick.state)
       << "\",\"observation\":\"phase_2_runtime_snapshot\""
       << ",\"event_key\":\"\",\"snapshot_sequence\":" << tick.sequence
       << ",\"snapshot_local_ts\":" << tick.localTs
       << ",\"game_ts\":" << tick.gameTs
       << ",\"context_hash\":\"phase2-" << tick.control.controlRevision << "-"
       << tick.sequence << "-" << tick.character.runtimeSerial << "\""
       << ",\"position\":{\"x\":" << tick.character.x
       << ",\"y\":" << tick.character.y << ",\"z\":" << tick.character.z
       << "},\"order\":{\"count\":" << tick.character.order.orderCount
       << ",\"task\":" << tick.character.order.taskType
       << ",\"subject_serial\":" << tick.character.order.subjectSerial
       << "},\"movement\":{\"moving\":"
       << (tick.character.moving ? "true" : "false")
       << ",\"path_failed\":"
       << (tick.character.pathFailed ? "true" : "false") << "}}";
  return json.str();
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
    Log("AUTONOMY_PHASE2_STATE: revision=" +
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
  if (attemptCancel && world) {
    std::string cancelReason;
    const bool cancelled = Stobe::Autonomy::TryCancelOwnedOrder(
        world, g_active.decision.runtimeSerial, g_active.ownedOrder,
        cancelReason);
    Log("AUTONOMY_PHASE2_CANCEL: decision=" + g_active.decision.decisionId +
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
  if (outcome != Stobe::Autonomy::MONITOR_INTERRUPTED) {
    std::string cancelReason;
    Stobe::Autonomy::TryCancelOwnedOrder(
        world, decision.runtimeSerial, g_active.ownedOrder, cancelReason);
  }
  const std::string outcomeName = Stobe::Autonomy::MonitorOutcomeName(outcome);
  QueueActionReport(control, decision, outcomeName, reason, elapsed, gameTs,
                    true);
  Log("AUTONOMY_PHASE2_TERMINAL: decision=" + decision.decisionId +
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
  EnsureInitialized();
  if (InterlockedCompareExchange(&g_threadStarted, 1, 0) != 0) {
    return;
  }
  HANDLE thread = CreateThread(NULL, 0, AutonomyPollThread, NULL, 0, NULL);
  if (thread) {
    CloseHandle(thread);
    Log("AUTONOMY_PHASE2: deterministic control loop started");
  } else {
    InterlockedExchange(&g_threadStarted, 0);
    Log("AUTONOMY_PHASE2_ERROR: failed to start control polling thread");
  }
}

void ResetAutonomyController(const char *reason) {
  EnsureInitialized();
  Stobe::Autonomy::ControlSnapshot control;
  EnterCriticalSection(&g_controlMutex);
  control = g_control;
  LeaveCriticalSection(&g_controlMutex);
  const std::string resetReason = reason ? reason : "world_transition";
  if (g_active.active && control.valid) {
    QueueActionReport(control, g_active.decision, "INTERRUPTED", resetReason,
                      g_active.activeElapsedMs, 0, true);
    g_active = ActiveAction();
  }
  ClearTickState();
  g_boundSerial = 0;
  if (!control.valid || !control.enabled) {
    return;
  }
  g_requiresExplicitResume = true;
  g_runtimeState = "PAUSED_UNSAFE";
  g_runtimeReason = resetReason;
  QueueStateReport(control, 0, g_runtimeState, g_runtimeReason, 0, true);
  Log("AUTONOMY_PHASE2: paused for world transition reason=" + resetReason);
}

void UpdateAutonomyController(GameWorld *world) {
  EnsureInitialized();
  Stobe::Autonomy::ControlSnapshot control;
  DWORD lastServerSuccess = 0;
  EnterCriticalSection(&g_controlMutex);
  control = g_control;
  lastServerSuccess = g_lastServerSuccessTick;
  LeaveCriticalSection(&g_controlMutex);
  if (!control.valid) {
    return;
  }

  const bool revisionChanged = control.controlRevision != g_appliedRevision;
  if (revisionChanged) {
    ClearActiveAction(world, true, "control_revision_changed");
    ClearTickState();
    g_appliedRevision = control.controlRevision;
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
      if (g_active.decision.command ==
          Stobe::Autonomy::DECISION_COMMAND_TRAVEL_LOCATION) {
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
      }
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
    facts.pathFailedSamples = g_active.pathFailedSamples;
    facts.stationarySamples = g_active.stationarySamples;
    facts.activeElapsedMs = g_active.activeElapsedMs;
    facts.noProgressElapsedMs = g_active.noProgressElapsedMs;
    facts.x = character.x;
    facts.y = character.y;
    facts.z = character.z;
    facts.currentOrder = character.order;
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
  facts.serverAvailable = lastServerSuccess != 0 &&
                          GetTickCount() - lastServerSuccess <=
                              kServerUnavailableMs;
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
      const long long remaining =
          tickResult.decision.actionDeadlineTs -
          static_cast<long long>(time(NULL));
      g_active.deadlineMs = static_cast<int>(
          std::max<long long>(1, std::min<long long>(remaining, 3600)) * 1000);
      QueueActionReport(control, tickResult.decision, "DISPATCHED",
                        dispatched.reason, 0, gameTs, false);
      Log("AUTONOMY_PHASE2_DISPATCH: decision=" +
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

  QueueTickRequest(control, character, gameTs);
  PublishRuntimeState(control, character.runtimeSerial, "DECIDING",
                      "deterministic_tick_requested", character.name, gameTs,
                      revisionChanged);
}
