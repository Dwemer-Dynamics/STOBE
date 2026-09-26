#include "SupportReportLauncher.h"
#include "Utils.h"
#include <windows.h>

namespace Stobe {
namespace SupportReportLauncher {
namespace {
HANDLE g_worker = NULL;
std::string g_result;
bool g_startFailed = false;

// Only the worker writes the result until its thread handle becomes signaled.
DWORD WINAPI GenerateReport(LPVOID) {
  const wchar_t *launcher = L"C:\\DwemerDistro\\DwemerDistro.exe";
  wchar_t command[] = L"\"C:\\DwemerDistro\\DwemerDistro.exe\" --generate-diagnostics --open-output-folder";
  if (GetFileAttributesW(launcher) == INVALID_FILE_ATTRIBUTES) {
    g_result = "Install or update DwemerDistro to generate logs.";
    return 0;
  }
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(launcher, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                      NULL, NULL, &startup, &process)) {
    Log("SUPPORT_REPORT: launcher start failed error=" + ToString((unsigned int)GetLastError()));
    g_result = "Couldn't start log generation. Check stobe.log.";
    return 0;
  }
  CloseHandle(process.hThread);
  DWORD wait = WaitForSingleObject(process.hProcess, 10 * 60 * 1000);
  DWORD code = 1;
  bool exited = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  if (!exited) {
    g_result = "Log generation has not finished. Check the DwemerDistro launcher log before retrying.";
  } else if (code == 0) {
    g_result = "Logs saved to your Desktop.";
  } else if (code == 3) {
    g_result = "Logs are already being generated.";
  } else {
    g_result = "Couldn't generate logs. Check the DwemerDistro launcher log.";
  }
  Log("SUPPORT_REPORT: wait=" + ToString((unsigned int)wait) + " exit=" + ToString((unsigned int)code));
  return 0;
}
}

bool IsRunning() { return g_worker != NULL || g_startFailed; }

void Start() {
  if (IsRunning()) return;
  g_result.clear();
  g_worker = CreateThread(NULL, 0, GenerateReport, NULL, 0, NULL);
  if (!g_worker) {
    g_result = "Couldn't start log generation. Check stobe.log.";
    g_startFailed = true;
    Log("SUPPORT_REPORT: worker start failed error=" + ToString((unsigned int)GetLastError()));
  }
}

bool TakeResult(std::string &message) {
  if (!g_startFailed) {
    if (!g_worker || WaitForSingleObject(g_worker, 0) != WAIT_OBJECT_0) return false;
    CloseHandle(g_worker);
    g_worker = NULL;
  }
  g_startFailed = false;
  message = g_result;
  return true;
}
}
}
