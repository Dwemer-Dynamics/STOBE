#pragma once

#include <windows.h>

class GameWorld;

namespace Stobe {
namespace WorldStateRuntime {

void Reset();
void Update(GameWorld *world, DWORD worldStableSinceTick);

} // namespace WorldStateRuntime
} // namespace Stobe
