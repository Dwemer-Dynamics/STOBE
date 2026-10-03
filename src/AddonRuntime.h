#pragma once

#include <string>

class GameWorld;

// Public native addon API runtime (include/StobeAddonApi.h). Addon calls only
// enqueue work; game work runs from GameThreadTick on Kenshi's game thread.
namespace Stobe {
namespace Addon {

// Called once per stable-world PlayerInterface update.
void GameThreadTick(GameWorld *world);

// Queues an ExtCmd<Bridge>_<Action> action for dispatch to its registered
// bridge. Unregistered or malformed commands are rejected and reported as a
// failed action. rawCommand keeps its original case.
void QueueExternalAction(unsigned int actorSerial, const std::string &rawCommand,
                         const std::string &parameter);

} // namespace Addon
} // namespace Stobe
