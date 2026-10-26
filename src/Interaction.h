#pragma once
#include <windows.h>
#include <string>

namespace Stobe { namespace Interaction {
bool Allowed();
bool ManualInputAllowed();
LONG Epoch();
bool IsCurrent(LONG epoch);
int Status(); // 0 Off, 1 On, 2 syncing, 3 failed (locally Off).
void Toggle();
void Update();
// Game thread. Requests On/Off and returns the request sequence that decides
// its outcome. The current request is returned unchanged when it already asks
// for the same state and has not failed, so repeats never contact the server.
LONG Request(bool enabled);
// Newest requested sequence, newest settled sequence and the status recorded
// when it settled (0 Off, 1 On, 3 failed). Any thread.
void Progress(LONG &requested, LONG &settled, int &settledStatus);
std::string Query();
std::wstring Headers();
} }
