#include "AutonomyController.h"

#include "AutonomyProtocol.h"
#include "Comm.h"
#include "KenshiAiCompat.h"
#include "StobeText.h"
#include "Utils.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <windows.h>

#include <kenshi/GameWorld.h>

namespace {

const DWORD kPollIntervalMs = 1500;
const DWORD kHeartbeatIntervalMs = 3000;
const DWORD kServerUnavailableMs = 8000;

struct RuntimeReport {
  bool valid;
  long long revision;
  int npcId;
  std::string storageId;
  unsigned int runtimeSerial;
  std::string state;
  std::string observation;
  std::string error;
  std::string eventKey;
  int gameTs;

  RuntimeReport()
      : valid(false), revision(0), npcId(0), runtimeSerial(0), gameTs(0) {}
};

CRITICAL_SECTION g_controlMutex;
LONG g_initialized = 0;
LONG g_threadStarted = 0;
Stobe::Autonomy::ControlSnapshot g_control;
DWORD g_lastServerSuccessTick = 0;
RuntimeReport g_pendingReport;

long long g_appliedRevision = -1;
unsigned int g_boundSerial = 0;
bool g_requiresExplicitResume = false;
std::string g_runtimeState = "DISABLED";
std::string g_runtimeReason = "autonomy_disabled";
unsigned int g_stateSequence = 0;
DWORD g_lastQueuedHeartbeatTick = 0;

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

std::string BuildReportJson(const RuntimeReport &report) {
  std::ostringstream json;
  json << "{\"control_revision\":" << report.revision
       << ",\"npc_id\":" << report.npcId
       << ",\"npc_storage_id\":\""
       << Stobe::Text::EscapeJSON(report.storageId)
       << "\",\"runtime_serial\":" << report.runtimeSerial
       << ",\"state\":\"" << Stobe::Text::EscapeJSON(report.state)
       << "\",\"observation\":\""
       << Stobe::Text::EscapeJSON(report.observation)
       << "\",\"error\":\"" << Stobe::Text::EscapeJSON(report.error)
       << "\",\"event_key\":\""
       << Stobe::Text::EscapeJSON(report.eventKey)
       << "\",\"event_type\":\"plugin_state\",\"game_ts\":"
       << report.gameTs << "}";
  return json.str();
}

void QueueReport(const Stobe::Autonomy::ControlSnapshot &control,
                 unsigned int runtimeSerial, const std::string &state,
                 const std::string &reason, int gameTs, bool stateChanged) {
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

  EnterCriticalSection(&g_controlMutex);
  g_pendingReport = report;
  LeaveCriticalSection(&g_controlMutex);
}

DWORD WINAPI AutonomyPollThread(LPVOID) {
  DWORD lastPollTick = 0;
  DWORD lastReportTick = 0;
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
    EnterCriticalSection(&g_controlMutex);
    if (g_pendingReport.valid &&
        (lastReportTick == 0 || GetTickCount() - lastReportTick >= 100)) {
      report = g_pendingReport;
      g_pendingReport.valid = false;
    }
    LeaveCriticalSection(&g_controlMutex);
    if (report.valid) {
      PostToStobeWithResponse(L"/autonomy_observation",
                              BuildReportJson(report));
      lastReportTick = GetTickCount();
    }
    Sleep(100);
  }
  return 0;
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
    Log("AUTONOMY_PHASE1: control polling started; actions disabled");
  } else {
    InterlockedExchange(&g_threadStarted, 0);
    Log("AUTONOMY_PHASE1_ERROR: failed to start control polling thread");
  }
}

void ResetAutonomyController(const char *reason) {
  EnsureInitialized();
  Stobe::Autonomy::ControlSnapshot control;
  EnterCriticalSection(&g_controlMutex);
  control = g_control;
  LeaveCriticalSection(&g_controlMutex);
  if (!control.valid || !control.enabled) {
    g_boundSerial = 0;
    return;
  }
  const std::string resetReason = reason ? reason : "world_transition";
  const bool changed = !g_requiresExplicitResume ||
                       g_runtimeState != "PAUSED_UNSAFE" ||
                       g_runtimeReason != resetReason;
  g_requiresExplicitResume = true;
  g_boundSerial = 0;
  g_runtimeState = "PAUSED_UNSAFE";
  g_runtimeReason = resetReason;
  if (changed) {
    QueueReport(control, 0, g_runtimeState, g_runtimeReason, 0, true);
    Log("AUTONOMY_PHASE1: paused for world transition reason=" + resetReason);
  }
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
    g_appliedRevision = control.controlRevision;
    g_boundSerial = 0;
    if (control.enabled && control.desiredState == "ARMING") {
      g_requiresExplicitResume = false;
    }
  }

  Stobe::Autonomy::RuntimeFacts facts;
  facts.serverAvailable = lastServerSuccess != 0 &&
                          GetTickCount() - lastServerSuccess <=
                              kServerUnavailableMs;
  facts.requiresExplicitResume = g_requiresExplicitResume;
  facts.manualPauseLatched = g_requiresExplicitResume &&
                             g_runtimeState == "PAUSED_USER" &&
                             !revisionChanged;

  const unsigned int expectedSerial =
      Stobe::Autonomy::ParseStorageSerial(control.npcStorageId);
  Stobe::KenshiAi::CharacterSnapshot character;
  if (world && expectedSerial != 0 && control.enabled &&
      control.desiredState == "ARMING" && facts.serverAvailable &&
      !facts.requiresExplicitResume) {
    character = Stobe::KenshiAi::CaptureCharacter(world, expectedSerial);
    facts.found = character.found;
    facts.identityMatches = character.identityMatches;
    facts.playerCharacter = character.playerCharacter;
    facts.dead = character.dead;
    facts.unconscious = character.unconscious;
    facts.hasOrdersReceiver = character.hasOrdersReceiver;
    facts.canTakeOrders = character.canTakeOrders;
    facts.hasPlayerOrders = character.hasPlayerOrders;
  }

  const Stobe::Autonomy::StateDecision decision =
      Stobe::Autonomy::EvaluatePhase1State(control, facts);
  if (decision.state == "PAUSED_USER" &&
      decision.reason == "manual_player_order_detected") {
    g_requiresExplicitResume = true;
  }
  g_boundSerial = decision.state == "OBSERVING" ? character.runtimeSerial : 0;

  const bool stateChanged = revisionChanged || decision.state != g_runtimeState ||
                            decision.reason != g_runtimeReason;
  g_runtimeState = decision.state;
  g_runtimeReason = decision.reason;
  const DWORD now = GetTickCount();
  const bool heartbeatDue = g_lastQueuedHeartbeatTick == 0 ||
                            now - g_lastQueuedHeartbeatTick >=
                                kHeartbeatIntervalMs;
  if (stateChanged || heartbeatDue) {
    g_lastQueuedHeartbeatTick = now;
    QueueReport(control, g_boundSerial, g_runtimeState, g_runtimeReason,
                ResolveGameTs(world), stateChanged);
  }
  if (stateChanged) {
    Log("AUTONOMY_PHASE1_STATE: revision=" +
        ToString(static_cast<int>(control.controlRevision)) + " state=" +
        g_runtimeState + " reason=" + g_runtimeReason + " serial=" +
        ToString(g_boundSerial) + " name=\"" + character.name + "\"");
  }
}
