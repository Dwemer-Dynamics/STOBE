#include "KenshiAiCompat.h"

#include <cstdint>

#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/Character.h>
#include <kenshi/GameWorld.h>

namespace Stobe {
namespace KenshiAi {
namespace {

bool IsValidCharacter(Character *character) {
  return character && reinterpret_cast<uintptr_t>(character) > 0x1000;
}

Character *ResolveCharacter(GameWorld *world, unsigned int serial) {
  if (!world || serial == 0) {
    return NULL;
  }
  try {
    const auto &characters = world->getCharacterUpdateList();
    for (auto it = characters.begin(); it != characters.end(); ++it) {
      Character *candidate = *it;
      if (!IsValidCharacter(candidate)) {
        continue;
      }
      try {
        if (candidate->getHandle().serial == serial) {
          return candidate;
        }
      } catch (...) {
      }
    }
  } catch (...) {
  }
  return NULL;
}

} // namespace

CharacterSnapshot::CharacterSnapshot()
    : found(false), identityMatches(false), playerCharacter(false), dead(false),
      unconscious(false), hasOrdersReceiver(false), canTakeOrders(false),
      hasPlayerOrders(false), runtimeSerial(0) {}

CharacterSnapshot CaptureCharacter(GameWorld *world,
                                   unsigned int expectedSerial) {
  CharacterSnapshot result;
  Character *character = ResolveCharacter(world, expectedSerial);
  if (!IsValidCharacter(character)) {
    return result;
  }
  result.found = true;
  try {
    result.runtimeSerial = character->getHandle().serial;
    result.identityMatches = result.runtimeSerial == expectedSerial;
  } catch (...) {
    return result;
  }
  try {
    result.name = character->getName();
  } catch (...) {
    result.name = "Unknown";
  }
  try {
    result.playerCharacter = character->isPlayerCharacter();
  } catch (...) {
  }
  try {
    result.dead = character->isDead();
  } catch (...) {
  }
  try {
    result.unconscious = character->isUnconcious();
  } catch (...) {
  }
  try {
    result.canTakeOrders = character->canTakePlayerOrdersAtThisTime();
  } catch (...) {
  }
  try {
    OrdersReceiver *orders = character->getOrdersReciever();
    if (orders && reinterpret_cast<uintptr_t>(orders) > 0x1000) {
      result.hasOrdersReceiver = true;
      result.hasPlayerOrders = orders->hasPlayerOrders();
    }
  } catch (...) {
  }
  return result;
}

} // namespace KenshiAi
} // namespace Stobe
