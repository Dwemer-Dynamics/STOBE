// ParityProbe: minimal Stobe native addon used to test the public addon API.
// It registers the ParityProbe bridge, answers ExtCmdParityProbe_Ping with a
// result report and never changes game state. Without Stobe it stays inert.
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "StobeAddonApi.h"

namespace {

const StobeAddonApiV1 *g_api = NULL;
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
  return reported >= 0 ? STOBE_ACTION_ACCEPTED : STOBE_ACTION_REJECTED;
}

// Waits briefly for Stobe; RE_Kenshi does not guarantee plugin load order.
DWORD WINAPI ConnectThread(LPVOID) {
  for (int attempt = 0; attempt < 240 && !g_api; ++attempt) {
    HMODULE stobe = GetModuleHandleA("Stobe.dll");
    StobeGetApiFn getApi =
        stobe ? reinterpret_cast<StobeGetApiFn>(
                    GetProcAddress(stobe, STOBE_GET_API_EXPORT))
              : NULL;
    const StobeAddonApiV1 *api = getApi ? getApi(STOBE_ADDON_API_VERSION) : NULL;
    if (api && api->struct_size >= sizeof(StobeAddonApiV1)) {
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
