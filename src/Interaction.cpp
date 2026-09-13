#include "Interaction.h"
#include "Comm.h"
#include "Globals.h"
#include "Utils.h"
#include "VoiceCapture.h"
#include <kenshi/GameWorld.h>
#include <cstdlib>

namespace Stobe { namespace Interaction {
namespace {
volatile LONG status = 2, desired = -1, generation = 0, epoch = 1, busy = 0;
DWORD nextAttempt = 0;

// Run on the existing event-driven update path; never touch game/UI objects here.
DWORD WINAPI Synchronize(LPVOID) {
  std::string response = PostToStobeWithResponse(L"/StobeServer/interaction.php", "{}");
  std::string enabled = JsonReadField(response, "enabled");
  std::string version = JsonReadField(response, "generation");
  const LONG requested = InterlockedCompareExchange(&desired, 0, 0);
  bool valid = (enabled == "true" || enabled == "false") && !version.empty()
      && version.find_first_not_of("0123456789") == std::string::npos;
  if (valid && requested >= 0 && (enabled == "true") != (requested == 1)) {
    response = PostToStobeWithResponse(L"/StobeServer/interaction.php",
        std::string("{\"enabled\":") + (requested ? "true" : "false") +
        ",\"generation\":" + version + "}");
    enabled = JsonReadField(response, "enabled");
    version = JsonReadField(response, "generation");
    valid = (enabled == "true" || enabled == "false") && !version.empty()
        && version.find_first_not_of("0123456789") == std::string::npos
        && (enabled == "true") == (requested == 1);
  }
  if (valid) {
    InterlockedExchange(&generation, std::strtol(version.c_str(), NULL, 10));
    InterlockedExchange(&desired, enabled == "true" ? 1 : 0);
    InterlockedExchange(&status, enabled == "true" ? 1 : 0);
  } else {
    InterlockedExchange(&status, 3);
  }
  InterlockedExchange(&busy, 0);
  return 0;
}
}

bool Allowed() { return Status() == 1; }
int Status() { return InterlockedCompareExchange(&status, 0, 0); }
LONG Epoch() { return InterlockedCompareExchange(&epoch, 0, 0); }
bool IsCurrent(LONG value) { return Allowed() && value == Epoch(); }

bool ManualInputAllowed() {
  if (Allowed()) return true;
  GameWorld *world = GetWorldSafe();
  if (world) world->showPlayerAMessage_withLog("Stobe is off.", true);
  return false;
}

void Toggle() {
  if (InterlockedCompareExchange(&busy, 0, 0) || Status() == 2) return;
  if (Status() != 3) InterlockedExchange(&desired, Allowed() ? 0 : 1);
  InterlockedExchange(&status, 2);
  InterlockedIncrement(&epoch);
  Stobe::Voice::Cancel();
  // Invalidate requests and pending actions, preserving the line already playing.
  BeginChatInterruptGeneration(false);
  nextAttempt = 0;
  Update();
}

void Update() {
  if (Status() != 2 && Status() != 3) return;
  DWORD now = GetTickCount();
  if (nextAttempt && static_cast<LONG>(now - nextAttempt) < 0) return;
  if (InterlockedCompareExchange(&busy, 1, 0)) return;
  nextAttempt = now + 5000;
  HANDLE thread = CreateThread(NULL, 0, Synchronize, NULL, 0, NULL);
  if (thread) CloseHandle(thread);
  else { InterlockedExchange(&status, 3); InterlockedExchange(&busy, 0); }
}

std::string Query() {
  return "&interaction_generation=" + ToString((int)InterlockedCompareExchange(&generation, 0, 0))
      + "&interaction_passive=" + (Allowed() ? "0" : "1");
}

std::wstring Headers() {
  return L"X-Stobe-Generation: " + ToWide(ToString((int)InterlockedCompareExchange(&generation, 0, 0)))
      + L"\r\nX-Stobe-Passive: " + (Allowed() ? L"0" : L"1") + L"\r\n";
}
} }
