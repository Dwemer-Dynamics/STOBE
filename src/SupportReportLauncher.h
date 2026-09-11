#pragma once
#include <string>

namespace Stobe {
namespace SupportReportLauncher {
bool IsRunning();
void Start();
// Called on the game UI thread; returns one completed result without waiting.
bool TakeResult(std::string &message);
}
}
