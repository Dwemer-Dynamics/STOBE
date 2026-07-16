#include "KenshiAiCompat.h"
#include "KenshiBuildingCompat.h"

#include <algorithm>
#include <cstdint>
#include <cmath>

#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/CharMovement.h>
#include <kenshi/Character.h>
#include <kenshi/GameWorld.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/RootObject.h>

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

Building *ResolveNearestRestBedImpl(GameWorld *world, Character *character,
                                    double maxDistance) {
  if (!world || !IsValidCharacter(character) || maxDistance <= 0.0) {
    return NULL;
  }
  try {
    if (character->inSomething == IN_BED && character->inWhat.isValid() &&
        !character->inWhat.isNull()) {
      Building *currentBed = character->inWhat.getBuilding();
      if (currentBed && reinterpret_cast<uintptr_t>(currentBed) > 0x1000) {
        return currentBed;
      }
    }
  } catch (...) {
  }

  Building *best = NULL;
  double bestDistance = maxDistance + 1.0;
  try {
    const Ogre::Vector3 origin = character->getPosition();
    lektor<RootObject *> nearby;
    world->getObjectsWithinSphere(nearby, origin,
                                  static_cast<float>(maxDistance), BUILDING,
                                  96, (RootObject *)character);
    for (uint32_t i = 0; i < nearby.size(); ++i) {
      Building *candidate = (Building *)nearby.stuff[i];
      if (!candidate || reinterpret_cast<uintptr_t>(candidate) <= 0x1000) {
        continue;
      }
      try {
        const BuildingFunction function = candidate->_NV_getSpecialFunction();
        const TaskType defaultTask = candidate->_NV_getDefaultTask();
        const bool bed = function == BF_BED || function == BF_SKELETON_BED ||
                         defaultTask == USE_BED ||
                         defaultTask == USE_BED_ORDER;
        if (!bed || candidate->_NV_isDestroyed() ||
            candidate->_NV_isBroken()) {
          continue;
        }
        candidate->forceValidUsageNodesValidation();
        if (!candidate->hasAnyGoodPositionMarkersLeft()) {
          continue;
        }
        const Ogre::Vector3 delta = candidate->getPosition() - origin;
        const double distance = std::sqrt(static_cast<double>(
            delta.x * delta.x + delta.y * delta.y + delta.z * delta.z));
        if (distance <= maxDistance && distance < bestDistance) {
          best = candidate;
          bestDistance = distance;
        }
      } catch (...) {
      }
    }
  } catch (...) {
  }
  return best;
}

} // namespace

NearbyActorSnapshot::NearbyActorSnapshot()
    : runtimeSerial(0), distance(0.0), playerCharacter(false), dead(false),
      unconscious(false), hostile(false), fullyRested(true),
      probablyDying(false), inBed(false), x(0.0), y(0.0), z(0.0),
      overallHealth(1.0), bleedRate(0.0), firstAidNeed(0.0),
      roboticAidNeed(0.0) {}

CharacterSnapshot::CharacterSnapshot()
    : found(false), identityMatches(false), playerCharacter(false), dead(false),
      unconscious(false), hasOrdersReceiver(false), canTakeOrders(false),
      hasPlayerOrders(false), paused(false), moving(false), pathFailed(false),
      carrying(false), fullyRested(true), probablyDying(false), inBed(false),
      restBedAvailable(false), inCombat(false), carriedSerial(0),
      attackTargetSerial(0), runtimeSerial(0), x(0.0), y(0.0), z(0.0),
      overallHealth(1.0), blood(0.0), maxBlood(0.0), bleedRate(0.0),
      firstAidNeed(0.0), roboticAidNeed(0.0) {}

Character *ResolveCharacter(GameWorld *world, unsigned int serial) {
  return ResolveCharacterImpl(world, serial);
}

Building *ResolveNearestRestBed(GameWorld *world, Character *character,
                                double maxDistance) {
  return ResolveNearestRestBedImpl(world, character, maxDistance);
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
    MedicalSystem *medical = character->getMedical();
    if (medical && reinterpret_cast<uintptr_t>(medical) > 0x1000) {
      result.fullyRested = medical->isFullyRested();
      result.probablyDying = medical->isProbablyDying();
      result.overallHealth = medical->getOverallHealthRating();
      result.blood = medical->blood;
      result.maxBlood = medical->getMaxBlood();
      result.bleedRate = medical->currentBleedRate;
      result.firstAidNeed = std::max(0.0, static_cast<double>(medical->scoreFirstAidNeed(false)));
      result.roboticAidNeed = std::max(0.0, static_cast<double>(medical->scoreFirstAidNeed(true)));
    }
    result.inBed = character->inSomething == IN_BED;
  } catch (...) {
  }
  try {
    result.carrying = character->isCarryingSomething;
    if (result.carrying) {
      result.carriedSerial = character->getCarryingObject().serial;
    }
  } catch (...) {
    result.carrying = false;
    result.carriedSerial = 0;
  }
  try {
    result.inCombat = character->isInCombatMode(true, true);
    const hand attackTarget = character->getAttackTarget();
    if (attackTarget.isValid() && !attackTarget.isNull()) {
      result.attackTargetSerial = attackTarget.serial;
    }
  } catch (...) {
    result.inCombat = false;
    result.attackTargetSerial = 0;
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
        } else if (orders->hasPlayerOrder(FIRST_AID_ORDER)) {
          result.order.taskType = static_cast<int>(FIRST_AID_ORDER);
        } else if (orders->hasPlayerOrder(FIRST_AID_ROBOT)) {
          result.order.taskType = static_cast<int>(FIRST_AID_ROBOT);
        } else if (orders->hasPlayerOrder(USE_BED_ORDER)) {
          result.order.taskType = static_cast<int>(USE_BED_ORDER);
        } else if (orders->hasPlayerOrder(REST)) {
          result.order.taskType = static_cast<int>(REST);
        } else if (orders->hasPlayerOrder(
                       UNPROVOKED_FOCUSED_MELEE_ATTACK)) {
          result.order.taskType =
              static_cast<int>(UNPROVOKED_FOCUSED_MELEE_ATTACK);
        } else if (orders->hasPlayerOrder(FOCUSED_MELEE_ATTACK)) {
          result.order.taskType = static_cast<int>(FOCUSED_MELEE_ATTACK);
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
  try {
    const Ogre::Vector3 actorPosition = character->getPosition();
    const auto &characters = world->getCharacterUpdateList();
    for (auto it = characters.begin();
         it != characters.end() && result.nearbyActors.size() < 32; ++it) {
      Character *candidate = *it;
      if (!IsValidCharacter(candidate) || candidate == character) {
        continue;
      }
      NearbyActorSnapshot nearby;
      try {
        nearby.runtimeSerial = candidate->getHandle().serial;
        nearby.name = candidate->getName();
        const Ogre::Vector3 candidatePosition = candidate->getPosition();
        const Ogre::Vector3 delta = candidatePosition - actorPosition;
        nearby.x = candidatePosition.x;
        nearby.y = candidatePosition.y;
        nearby.z = candidatePosition.z;
        nearby.distance = std::sqrt(static_cast<double>(delta.x * delta.x +
                                                        delta.y * delta.y +
                                                        delta.z * delta.z));
        if (nearby.runtimeSerial == 0 || nearby.name.empty() ||
            nearby.distance > 250.0) {
          continue;
        }
        nearby.playerCharacter = candidate->isPlayerCharacter();
        nearby.dead = candidate->isDead();
        nearby.unconscious = candidate->isUnconcious();
        nearby.hostile = character->isEnemy(candidate, true);
        MedicalSystem *medical = candidate->getMedical();
        if (medical && reinterpret_cast<uintptr_t>(medical) > 0x1000) {
          nearby.fullyRested = medical->isFullyRested();
          nearby.probablyDying = medical->isProbablyDying();
          nearby.overallHealth = medical->getOverallHealthRating();
          nearby.bleedRate = medical->currentBleedRate;
          nearby.firstAidNeed = std::max(0.0, static_cast<double>(medical->scoreFirstAidNeed(false)));
          nearby.roboticAidNeed = std::max(0.0, static_cast<double>(medical->scoreFirstAidNeed(true)));
        }
        nearby.inBed = candidate->inSomething == IN_BED;
        result.nearbyActors.push_back(nearby);
      } catch (...) {
      }
    }
  } catch (...) {
  }
  return result;
}

} // namespace KenshiAi
} // namespace Stobe
