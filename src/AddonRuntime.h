#pragma once

#include <string>

#include "StobeAddonApi.h"

class GameWorld;

// Public native addon API runtime (include/StobeAddonApi.h). Addon calls only
// enqueue work; game work runs from GameThreadTick on Kenshi's game thread.
namespace Stobe {
namespace Addon {

// Called once per stable-world PlayerInterface update.
void GameThreadTick(GameWorld *world);

// Queues an ExtCmd<Bridge>_<Action> action for dispatch to its registered
// bridge. Unregistered or malformed commands, and actors that are unloaded,
// dead or unconscious at dispatch, are reported as failed actions. actorSerial
// 0 means the speaker was not resolved exactly and is reported as failed
// without calling a bridge. rawCommand keeps its original case.
void QueueExternalAction(unsigned int actorSerial, const std::string &rawCommand,
                         const std::string &parameter);

// True when an addon holds a dialogue lock on the serial in the current load.
// Stobe's automatic selection, rechat, autonomy, request starts and spoken
// lines skip locked actors; physical actions are not gated. Any thread; no
// lock taken when none exist.
bool IsActorLocked(unsigned int serial);
// True when an addon holds an animation-busy flag on the serial in the
// current load. Stobe's built-in actions, follow/travel/move orders and
// autonomy skip busy actors; ExtCmd dispatch checks the owner itself.
bool IsActorBusy(unsigned int serial);
// STOBE_OK when Stobe's AI dialogue may use the serial, else STOBE_E_LOCKED or
// STOBE_E_ACTOR_BUSY: a lock or busy flag blocks selection, rechat, request
// starts and spoken lines. Any thread.
int DialogueGate(unsigned int serial);

} // namespace Addon
} // namespace Stobe
