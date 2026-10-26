// ParityProbe: minimal Stobe native addon used to test the public addon API.
// It registers the ParityProbe bridge, answers ExtCmdParityProbe_Ping with a
// result report and never changes game state. ExtCmdParityProbe_Report is the
// read-only fixture for the server's opt-in follow-up (stobe.addon_followup.v1):
// it reports "completed: green ..." so the server can voice one reply line. With API version 2 it also
// exercises control calls whose net effect is nothing: it re-requests the
// interaction state only when it is already On, and releases a lock and a
// busy flag in the same callback that set them. With API version 3 it only
// reads agents (list, closest, registration); it never registers, unregisters
// or refreshes anyone. With API version 4 and the environment variable
// STOBE_PARITY_PROBE_V4_DEMO=1 set before Kenshi starts, Ping@<op> (normal,
// whisper, shout, context, reactexplicit, reacteligible or state) runs that one
// operation 10 seconds later and logs its code and ticket; it may write NPC
// context or start dialogue, so it is off by default. Report never does.
// Without Stobe it stays inert.
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "StobeAddonApi.h"

namespace {

const StobeAddonApiV1 *g_api = NULL;
const StobeAddonApiV2 *g_v2 = NULL; // NULL with a version 1 Stobe.
const StobeAddonApiV3 *g_v3 = NULL; // NULL before version 3.
const StobeAddonApiV4 *g_v4 = NULL; // NULL before version 4.
bool g_v4Demo = false;              // STOBE_PARITY_PROBE_V4_DEMO=1
StobeAddonId g_addon = 0;
char g_logPath[MAX_PATH] = {0};

void LogLine(const char *text) {
  if (g_logPath[0] == '\0') {
    return;
  }
  FILE *file = NULL;
  if (fopen_s(&file, g_logPath, "a") == 0 && file) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(file, "%02u:%02u:%02u %s\n", now.wHour, now.wMinute, now.wSecond,
            text);
    fclose(file);
  }
}

// Writes ParityProbe.log beside this DLL.
void InitLogPath() {
  HMODULE self = NULL;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&InitLogPath), &self) ||
      !GetModuleFileNameA(self, g_logPath, MAX_PATH)) {
    g_logPath[0] = '\0';
    return;
  }
  char *slash = strrchr(g_logPath, '\\');
  if (!slash || static_cast<size_t>(slash - g_logPath) + 16 >= MAX_PATH) {
    g_logPath[0] = '\0';
    return;
  }
  strcpy_s(slash + 1, MAX_PATH - (slash + 1 - g_logPath), "ParityProbe.log");
}

// Game thread, once per ticket. Only logs; never blocks.
void STOBE_CALL OnControlStatus(void *, const StobeControlStatus *status) {
  char line[120];
  sprintf_s(line, sizeof(line), "control ticket=%u kind=%u state=%u reason=%d",
            status->ticket, status->kind, status->state, status->reason);
  LogLine(line);
}

// Version 2 demonstration with no lasting effect. Game thread.
void ExerciseControl(StobeActorRef actor) {
  if (!g_v2) {
    return;
  }
  const StobeU32 interaction = g_v2->GetInteractionState();
  StobeU32 ticket = 0;
  int requestCode = 0;
  // Asking for the confirmed current state completes without a server call.
  // Never request Off: the interaction toggle belongs to the player.
  if ((g_v2->capabilities & STOBE_CAP_INTERACTION_CONTROL) &&
      interaction == STOBE_INTERACTION_ON) {
    requestCode = g_v2->SetInteractionEnabled(g_addon, 1, &ticket);
  }
  int lockCode = 0;
  StobeAddonId owner = 0;
  int unlockCode = 0;
  if (g_v2->capabilities & STOBE_CAP_ACTOR_LOCKS) {
    lockCode = g_v2->SetActorLock(g_addon, actor, 1);
    g_v2->GetActorLockOwner(actor, &owner);
    unlockCode = lockCode == STOBE_OK ? g_v2->SetActorLock(g_addon, actor, 0) : 0;
  }
  // Older version 2 builds lack the busy calls; the capability bit says so.
  int busyCode = 0;
  StobeAddonId busyOwner = 0;
  int clearCode = 0;
  if (g_v2->capabilities & STOBE_CAP_ACTOR_BUSY) {
    busyCode = g_v2->SetActorBusy(g_addon, actor, 1);
    g_v2->GetActorBusyOwner(actor, &busyOwner);
    clearCode = busyCode == STOBE_OK ? g_v2->SetActorBusy(g_addon, actor, 0) : 0;
  }
  char line[240];
  sprintf_s(line, sizeof(line),
            "v2 interaction=%u request=%d ticket=%u lock=%d owner_is_self=%d "
            "unlock=%d busy=%d busy_owner_is_self=%d clear=%d",
            interaction, requestCode, ticket, lockCode, owner == g_addon,
            unlockCode, busyCode, busyOwner == g_addon, clearCode);
  LogLine(line);
}

// Version 3 agent queries, read-only. Game thread.
void ExerciseAgents(StobeActorRef actor) {
  if (!g_v3 || !(g_v3->v2.capabilities & STOBE_CAP_AGENTS)) {
    return;
  }
  StobeAgentInfo agents[8];
  agents[0].struct_size = sizeof(StobeAgentInfo);
  StobeU32 count = 0;
  const int listCode = g_v3->ListAgents(g_addon, agents, 8, &count);
  StobeActorRef closest = {0, 0};
  const StobeActorRef player = {0, 0};
  const int closestCode = g_v3->FindClosestAgent(g_addon, player, &closest);
  StobeU32 mode = 0;
  StobeAddonId owner = 0;
  const int modeCode = g_v3->GetAgentRegistration(actor, &mode, &owner);
  char line[200];
  sprintf_s(line, sizeof(line),
            "v3 agents=%d count=%u first=%.32s closest=%d serial=%u "
            "registration=%d mode=%u owner=%u",
            listCode, count, count > 0 ? agents[0].name : "", closestCode,
            closest.serial, modeCode, mode, owner);
  LogLine(line);
}

// Opt-in version 4 demo: Ping@<op> runs one operation 10 seconds later, when
// the Ping's own reply has usually settled. At most one is pending.
const char *const kDemoOps[] = {"normal",       "whisper",      "shout",
                                "context",      "reactexplicit", "reacteligible",
                                "state"};
const int kDemoOpCount = sizeof(kDemoOps) / sizeof(kDemoOps[0]);
volatile LONG g_demoToken = 0; // nonzero while a demo is pending
LONG g_demoNext = 0;
int g_demoOp = 0;              // written before g_demoToken is published
StobeActorRef g_demoActor = {0, 0};

// Game thread. Claims the token, so a dropped or timed-out run never fires.
void STOBE_CALL RunDemo(void *token) {
  const LONG mine = static_cast<LONG>(reinterpret_cast<LONG_PTR>(token));
  if (InterlockedCompareExchange(&g_demoToken, 0, mine) != mine) {
    return;
  }
  const StobeU32 caps = g_v4->v3.v2.capabilities;
  const StobeActorRef anyone = {0, 0};
  StobeU32 ticket = 0;
  int code = STOBE_E_BUSY; // world not ready: log the refusal, never retry
  if (!(g_api->GetRuntimeFlags() & STOBE_RUNTIME_WORLD_READY)) {
    code = STOBE_E_BUSY;
  } else if (g_demoOp <= 3) { // STOBE_MESSAGE_NORMAL..CONTEXT
    code = (caps & STOBE_CAP_ADDON_MESSAGE)
               ? g_v4->SendAddonMessage(g_addon, g_demoActor,
                                        static_cast<StobeU32>(g_demoOp),
                                        "ParityProbe checked in with you.",
                                        &ticket)
               : STOBE_E_INVALID_ARGUMENT;
  } else if (g_demoOp <= 5) {
    code = (caps & STOBE_CAP_ADDON_REACTION)
               ? g_v4->RequestAddonReaction(
                     g_addon, g_demoActor, anyone,
                     g_demoOp == 4 ? STOBE_REACTION_EXPLICIT
                                   : STOBE_REACTION_ELIGIBLE,
                     "React briefly to the probe.", &ticket)
               : STOBE_E_INVALID_ARGUMENT;
  } else {
    // Structured state rides the existing addon_state route; no model call.
    code = g_api->SendEvent(
        g_addon, STOBE_EVENT_PLUGIN_STATE, g_demoActor, "probe.status",
        "{\"v\":1,\"type\":\"status\",\"name\":\"probe\",\"text\":\"ok\"}");
  }
  char line[160];
  sprintf_s(line, sizeof(line), "v4 demo %s serial=%u code=%d ticket=%u",
            kDemoOps[g_demoOp], g_demoActor.serial, code, ticket);
  LogLine(line);
}

// Background thread: sleeps, then only queues RunDemo. No engine access.
DWORD WINAPI DemoDelayThread(LPVOID token) {
  Sleep(10000);
  const int queued = g_api->QueueGameThreadCallback(g_addon, RunDemo, token);
  const LONG mine = static_cast<LONG>(reinterpret_cast<LONG_PTR>(token));
  char line[64];
  sprintf_s(line, sizeof(line), "v4 demo queue=%d", queued);
  LogLine(line);
  // Stobe drops the callback on a load change; release the slot after 60 s.
  Sleep(queued >= 0 ? 60000 : 0);
  if (InterlockedCompareExchange(&g_demoToken, 0, mine) == mine) {
    LogLine("v4 demo did not run (queue failed or load changed)");
  }
  return 0;
}

// Game thread, from Ping. Unknown parameters leave Ping read-only.
void ScheduleDemo(const StobeActionRequest *request) {
  if (!g_v4Demo || !g_v4) {
    return;
  }
  int op = 0;
  while (op < kDemoOpCount && _stricmp(request->parameter, kDemoOps[op]) != 0) {
    ++op;
  }
  if (op == kDemoOpCount) {
    LogLine("v4 demo: parameter is not a demo operation; read-only Ping");
    return;
  }
  LONG token = InterlockedIncrement(&g_demoNext);
  if (token == 0) {
    token = InterlockedIncrement(&g_demoNext);
  }
  if (g_demoToken != 0) {
    LogLine("v4 demo refused: one is already pending");
    return;
  }
  g_demoOp = op;
  g_demoActor = request->actor; // serial + generation; Stobe rejects stale refs
  if (InterlockedCompareExchange(&g_demoToken, token, 0) != 0) {
    LogLine("v4 demo refused: one is already pending");
    return;
  }
  HANDLE thread = CreateThread(NULL, 0, DemoDelayThread,
                               reinterpret_cast<LPVOID>(LONG_PTR(token)), 0, NULL);
  if (!thread) {
    g_demoToken = 0;
    LogLine("v4 demo refused: no delay thread");
    return;
  }
  CloseHandle(thread);
  char line[96];
  sprintf_s(line, sizeof(line), "v4 demo %s scheduled in 10 s serial=%u",
            kDemoOps[op], request->actor.serial);
  LogLine(line);
}

// Runs on Kenshi's game thread. Reads state only, then reports the result.
int STOBE_CALL OnParityAction(void *, const StobeActionRequest *request) {
  if (!request || request->struct_size < sizeof(StobeActionRequest) ||
      (_stricmp(request->action, "Ping") != 0 &&
       _stricmp(request->action, "Report") != 0)) {
    LogLine("rejected unsupported ParityProbe action");
    return STOBE_ACTION_REJECTED;
  }

  StobeActorState state;
  state.struct_size = sizeof(state);
  state.flags = 0;
  const int stateCode = g_api->GetActorState(request->actor, &state);

  const bool report = _stricmp(request->action, "Report") == 0;
  char result[200];
  if (report) {
    sprintf_s(result, sizeof(result), "green serial=%u state=%d",
              request->actor.serial, stateCode);
  } else {
    sprintf_s(result, sizeof(result),
              "pong serial=%u state=%d flags=0x%02x target=%.64s",
              request->actor.serial, stateCode, state.flags,
              request->parameter);
  }
  const int reported =
      g_api->ReportActionResult(g_addon, request->request_id, 1, result);

  char line[320];
  sprintf_s(line, sizeof(line), "%s request=%u actor=%u report=%d: %s",
            request->command, request->request_id, request->actor.serial,
            reported, result);
  LogLine(line);
  if (!report) {
    ExerciseControl(request->actor);
    ExerciseAgents(request->actor);
    ScheduleDemo(request);
  }
  return reported >= 0 ? STOBE_ACTION_ACCEPTED : STOBE_ACTION_REJECTED;
}

// Prefers version 4, then 3 and 2; an older Stobe returns NULL for newer
// versions.
const StobeAddonApiV1 *GetStobeApi(StobeGetApiFn getApi) {
  const StobeAddonApiV1 *api = getApi(STOBE_ADDON_API_VERSION_4);
  if (api && api->api_version >= STOBE_ADDON_API_VERSION_4 &&
      api->struct_size >= sizeof(StobeAddonApiV4)) {
    g_v4 = reinterpret_cast<const StobeAddonApiV4 *>(api);
    g_v3 = &g_v4->v3;
    g_v2 = &g_v3->v2;
    return api;
  }
  api = getApi(STOBE_ADDON_API_VERSION_3);
  if (api && api->api_version >= STOBE_ADDON_API_VERSION_3 &&
      api->struct_size >= sizeof(StobeAddonApiV3)) {
    g_v3 = reinterpret_cast<const StobeAddonApiV3 *>(api);
    g_v2 = &g_v3->v2;
    return api;
  }
  api = getApi(STOBE_ADDON_API_VERSION_2);
  if (api && api->api_version >= STOBE_ADDON_API_VERSION_2 &&
      api->struct_size >= sizeof(StobeAddonApiV2)) {
    g_v2 = reinterpret_cast<const StobeAddonApiV2 *>(api);
    return api;
  }
  api = getApi(STOBE_ADDON_API_VERSION);
  return api && api->struct_size >= sizeof(StobeAddonApiV1) ? api : NULL;
}

// Waits briefly for Stobe; RE_Kenshi does not guarantee plugin load order.
DWORD WINAPI ConnectThread(LPVOID) {
  for (int attempt = 0; attempt < 240 && !g_api; ++attempt) {
    HMODULE stobe = GetModuleHandleA("Stobe.dll");
    StobeGetApiFn getApi =
        stobe ? reinterpret_cast<StobeGetApiFn>(
                    GetProcAddress(stobe, STOBE_GET_API_EXPORT))
              : NULL;
    const StobeAddonApiV1 *api = getApi ? GetStobeApi(getApi) : NULL;
    if (api) {
      g_api = api;
      break;
    }
    if (stobe && !getApi) {
      LogLine("Stobe.dll has no addon API; staying inert");
      return 0;
    }
    Sleep(500);
  }
  if (!g_api) {
    LogLine("Stobe addon API unavailable; staying inert");
    return 0;
  }

  StobeAddonInfo info;
  info.struct_size = sizeof(info);
  info.reserved = 0;
  info.name = "ParityProbe";
  info.version = "1.0.0";
  int code = g_api->RegisterAddon(&info, &g_addon);
  if (code == STOBE_OK) {
    code = g_api->RegisterActionBridge(g_addon, "ParityProbe", OnParityAction,
                                       NULL);
  }
  if (code == STOBE_OK && g_v2 &&
      (g_v2->capabilities & STOBE_CAP_CONTROL_STATUS)) {
    code = g_v2->SetControlCallback(g_addon, OnControlStatus, NULL);
  }
  char line[160];
  sprintf_s(line, sizeof(line), "connected to Stobe %s api=%u register=%d",
            g_api->stobe_version ? g_api->stobe_version : "?",
            g_api->api_version, code);
  LogLine(line);
  return 0;
}

} // namespace

// RE_Kenshi plugin entry point (same C++ signature as Stobe's).
__declspec(dllexport) void startPlugin() {
  InitLogPath();
  char demo[4] = {0};
  g_v4Demo = GetEnvironmentVariableA("STOBE_PARITY_PROBE_V4_DEMO", demo,
                                     sizeof(demo)) == 1 &&
             demo[0] == '1';
  HANDLE thread = CreateThread(NULL, 0, ConnectThread, NULL, 0, NULL);
  if (thread) {
    CloseHandle(thread);
  }
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
