#include "KenshiAiCompat.h"

#include <cstdint>

#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/CharMovement.h>
#include <kenshi/Character.h>
#include <kenshi/GameWorld.h>

namespace Stobe {
namespace KenshiAi {
namespace {

bool IsValidCharacter(Character *character) {
  return character && reinterpret_cast<uintptr_t>(character) > 0x1000;
}

Character *ResolveCharacterImpl(GameWorld *world, unsigned int serial) {
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
      hasPlayerOrders(false), paused(false), moving(false), pathFailed(false),
      runtimeSerial(0), x(0.0), y(0.0), z(0.0) {}

Character *ResolveCharacter(GameWorld *world, unsigned int serial) {
  return ResolveCharacterImpl(world, serial);
}

CharacterSnapshot CaptureCharacter(GameWorld *world,
                                   unsigned int expectedSerial) {
  CharacterSnapshot result;
  Character *character = ResolveCharacterImpl(world, expectedSerial);
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
    const Ogre::Vector3 position = character->getPosition();
    result.x = position.x;
    result.y = position.y;
    result.z = position.z;
  } catch (...) {
  }
  try {
    result.paused = world->isPaused();
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
      result.order.orderCount = static_cast<int>(orders->orders.list.size());
      void *first = orders->getFirstOrder();
      if (first && reinterpret_cast<uintptr_t>(first) > 0x1000) {
        result.order.runtimePointer =
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(first));
        if (orders->hasPlayerOrder(IDLE)) {
          result.order.taskType = static_cast<int>(IDLE);
        } else if (orders->hasPlayerOrder(MOVE_CUS_ORDERED)) {
          result.order.taskType = static_cast<int>(MOVE_CUS_ORDERED);
        }
      }
    }
  } catch (...) {
  }
  try {
    CharMovement *movement = character->getMovement();
    if (movement && reinterpret_cast<uintptr_t>(movement) > 0x1000) {
      result.moving = movement->isCurrentlyMoving();
      result.pathFailed = movement->pathFailed();
      const Ogre::Vector3 destination = movement->getDestination();
      result.order.x = destination.x;
      result.order.y = destination.y;
      result.order.z = destination.z;
    }
  } catch (...) {
  }
  return result;
}

} // namespace KenshiAi
} // namespace Stobe
