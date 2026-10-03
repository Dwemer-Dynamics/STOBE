// ParityProbe: minimal Stobe native addon used to test the public addon API.
// It registers the ParityProbe bridge, answers ExtCmdParityProbe_Ping with a
// result report and never changes game state. With API version 2 it also
// exercises control calls whose net effect is nothing: it re-requests the
// interaction state only when it is already On, and releases a lock and a
// busy flag in the same callback that set them. Without Stobe it stays inert.
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "StobeAddonApi.h"

namespace {

const StobeAddonApiV1 *g_api = NULL;
const StobeAddonApiV2 *g_v2 = NULL; // NULL with a version 1 Stobe.
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

// Runs on Kenshi's game thread. Reads state only, then reports the result.
int STOBE_CALL OnParityAction(void *, const StobeActionRequest *request) {
  if (!request || request->struct_size < sizeof(StobeActionRequest) ||
      _stricmp(request->action, "Ping") != 0) {
    LogLine("rejected unsupported ParityProbe action");
    return STOBE_ACTION_REJECTED;
  }

  StobeActorState state;
  state.struct_size = sizeof(state);
  state.flags = 0;
  const int stateCode = g_api->GetActorState(request->actor, &state);

  char result[200];
  sprintf_s(result, sizeof(result), "pong serial=%u state=%d flags=0x%02x target=%.64s",
            request->actor.serial, stateCode, state.flags, request->parameter);
  const int reported =
      g_api->ReportActionResult(g_addon, request->request_id, 1, result);

  char line[320];
  sprintf_s(line, sizeof(line), "%s request=%u actor=%u report=%d: %s",
            request->command, request->request_id, request->actor.serial,
            reported, result);
  LogLine(line);
  ExerciseControl(request->actor);
  return reported >= 0 ? STOBE_ACTION_ACCEPTED : STOBE_ACTION_REJECTED;
}

// Prefers version 2; a Stobe without it returns NULL, so fall back to 1.
const StobeAddonApiV1 *GetStobeApi(StobeGetApiFn getApi) {
  const StobeAddonApiV1 *api = getApi(STOBE_ADDON_API_VERSION_2);
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
  HANDLE thread = CreateThread(NULL, 0, ConnectThread, NULL, 0, NULL);
  if (thread) {
    CloseHandle(thread);
  }
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
