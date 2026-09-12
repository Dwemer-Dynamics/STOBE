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
std::string Query();
std::wstring Headers();
} }
