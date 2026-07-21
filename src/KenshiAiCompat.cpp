#include "KenshiAiCompat.h"
#include "KenshiAiRuntimeCompat.h"
#include "KenshiBuildingCompat.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <map>

#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/CharMovement.h>
#include <kenshi/Character.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Inventory.h>
#include <kenshi/Item.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/RootObject.h>
#include <kenshi/ShopTrader.h>

namespace Stobe {
namespace KenshiAi {
namespace {

struct TraderSnapshotCache {
  std::vector<InventoryItemSnapshot> items;
  int cats;
  time_t capturedAt;

  TraderSnapshotCache() : cats(0), capturedAt(0) {}
};

std::map<unsigned int, TraderSnapshotCache> g_traderSnapshotCache;

bool IsValidCharacter(Character *character) {
  return character && reinterpret_cast<uintptr_t>(character) > 0x1000;
}

void CaptureInventoryItems(Inventory *inventory,
                           std::vector<InventoryItemSnapshot> &out,
                           int &totalCount, size_t maxEntries) {
  out.clear();
  totalCount = 0;
  if (!inventory || reinterpret_cast<uintptr_t>(inventory) <= 0x1000) {
    return;
  }
  try {
    const lektor<Item *> &items = inventory->getAllItems();
    for (uint32_t i = 0; i < items.size(); ++i) {
      Item *item = items.stuff[i];
      if (!item || reinterpret_cast<uintptr_t>(item) <= 0x1000) {
        continue;
      }
      InventoryItemSnapshot captured;
      captured.name = item->getName();
      captured.count = item->quantity > 0 ? item->quantity : 1;
      captured.buyValueEach = std::max(0, item->getValueSingle(true));
      captured.sellValueEach = std::max(0, item->getValueSingle(false));
      totalCount += captured.count;
      if (!captured.name.empty() && out.size() < maxEntries) {
        out.push_back(captured);
      }
    }
  } catch (...) {
    out.clear();
    totalCount = 0;
  }
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

InventoryItemSnapshot::InventoryItemSnapshot()
    : count(0), buyValueEach(0), sellValueEach(0) {}

NearbyActorSnapshot::NearbyActorSnapshot()
    : runtimeSerial(0), distance(0.0), playerCharacter(false), trader(false),
      dead(false), unconscious(false), hostile(false), fullyRested(true),
      probablyDying(false), inBed(false), x(0.0), y(0.0), z(0.0),
      overallHealth(1.0), bleedRate(0.0), firstAidNeed(0.0),
      roboticAidNeed(0.0), cats(0) {}

NearbyResourceSnapshot::NearbyResourceSnapshot()
    : runtimeSerial(0), distance(0.0), natural(false), usable(false),
      taskType(static_cast<int>(NULL_TASK)), x(0.0), y(0.0), z(0.0) {}

NearbyWorkSnapshot::NearbyWorkSnapshot()
    : runtimeSerial(0), distance(0.0), usable(false), needsWork(false),
      inputEmpty(false), inputFull(false), outputEmpty(false),
      outputFull(false), powerOn(false), workQueued(false),
      slotAvailable(false), taskType(static_cast<int>(NULL_TASK)),
      powerOutput(0.0), x(0.0), y(0.0), z(0.0) {}

CharacterSnapshot::CharacterSnapshot()
    : found(false), identityMatches(false), playerCharacter(false), dead(false),
      unconscious(false), hasOrdersReceiver(false), canTakeOrders(false),
      hasPlayerOrders(false), paused(false), moving(false), pathFailed(false),
      carrying(false), fullyRested(true), probablyDying(false), inBed(false),
      restBedAvailable(false), inCombat(false), aiTaskExpired(false),
      aiGoalExpired(false), aiIntendsToAttackTarget(false), carriedSerial(0),
      attackTargetSerial(0), runtimeSerial(0), x(0.0), y(0.0), z(0.0),
      overallHealth(1.0), blood(0.0), maxBlood(0.0), bleedRate(0.0),
      firstAidNeed(0.0), roboticAidNeed(0.0), cats(0),
      inventoryItemCount(0), aiPathFailureCount(0) {}

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
    result.cats = std::max(0, character->getMoney());
    CaptureInventoryItems(character->getInventory(), result.inventoryItems,
                          result.inventoryItemCount, 80);
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
    AI *ai = character->getAI();
    AITaskSytem *taskSystem =
        ai && reinterpret_cast<uintptr_t>(ai) > 0x1000
            ? ai->getTaskSystem()
            : NULL;
    if (taskSystem && reinterpret_cast<uintptr_t>(taskSystem) > 0x1000) {
      result.aiCurrentGoal = taskSystem->getCurrentGoalString();
      result.aiTaskExpired = taskSystem->isTaskExpired();
      result.aiGoalExpired = taskSystem->isGoalExpired();
      result.aiPathFailureCount =
          static_cast<int>(std::min<size_t>(taskSystem->pathFailures.size(),
                                            1000));
      Character *attackTarget =
          ResolveCharacterImpl(world, result.attackTargetSerial);
      if (IsValidCharacter(attackTarget)) {
        result.aiIntendsToAttackTarget =
            taskSystem->intendsToAttackTarget(attackTarget);
      }
    }
  } catch (...) {
    result.aiCurrentGoal.clear();
    result.aiTaskExpired = false;
    result.aiGoalExpired = false;
    result.aiPathFailureCount = 0;
    result.aiIntendsToAttackTarget = false;
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
        } else if (orders->hasPlayerOrder(OPERATE_MACHINERY)) {
          result.order.taskType = static_cast<int>(OPERATE_MACHINERY);
        } else if (orders->hasPlayerOrder(PROSPECTING)) {
          result.order.taskType = static_cast<int>(PROSPECTING);
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
        nearby.trader = candidate->isATrader();
        nearby.cats = std::max(0, candidate->getMoney());
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
        if (nearby.trader && nearby.distance <= 30.0) {
          const time_t now = time(NULL);
          std::map<unsigned int, TraderSnapshotCache>::iterator cached =
              g_traderSnapshotCache.find(nearby.runtimeSerial);
          if (cached != g_traderSnapshotCache.end() &&
              now - cached->second.capturedAt < 5) {
            nearby.traderItems = cached->second.items;
            nearby.cats = cached->second.cats;
          } else {
            try {
              ShopTrader trader(candidate);
              int traderItemCount = 0;
              CaptureInventoryItems(trader.getInventory(), nearby.traderItems,
                                    traderItemCount, 100);
              nearby.cats = std::max(0, trader.getMoney());
              TraderSnapshotCache updated;
              updated.items = nearby.traderItems;
              updated.cats = nearby.cats;
              updated.capturedAt = now;
              g_traderSnapshotCache[nearby.runtimeSerial] = updated;
            } catch (...) {
              nearby.traderItems.clear();
            }
          }
        }
        result.nearbyActors.push_back(nearby);
      } catch (...) {
      }
    }
  } catch (...) {
  }
  try {
    const Ogre::Vector3 origin = character->getPosition();
    lektor<RootObject *> nearbyBuildings;
    world->getObjectsWithinSphere(nearbyBuildings, origin, 250.0f, BUILDING,
                                  128, (RootObject *)character);
    for (uint32_t i = 0; i < nearbyBuildings.size(); ++i) {
      Building *building = (Building *)nearbyBuildings.stuff[i];
      if (!building || reinterpret_cast<uintptr_t>(building) <= 0x1000) {
        continue;
      }
      try {
        const BuildingFunction function = building->_NV_getSpecialFunction();
        const BuildingClassType buildingClass =
            building->_NV_getBuildingClass();
        const Ogre::Vector3 position = building->getPosition();
        const Ogre::Vector3 delta = position - origin;
        const double distance = std::sqrt(static_cast<double>(
            delta.x * delta.x + delta.y * delta.y + delta.z * delta.z));
        const bool intact = !building->_NV_isDestroyed() &&
                            !building->_NV_isBroken();

        if ((function == BF_MINE || function == BF_MINE_NATURAL) &&
            result.nearbyResources.size() < 32) {
          NearbyResourceSnapshot resource;
          resource.runtimeSerial = building->getHandle().serial;
          resource.name = building->getName();
          resource.distance = distance;
          resource.natural = function == BF_MINE_NATURAL;
          resource.taskType = static_cast<int>(OPERATE_MACHINERY);
          resource.x = position.x;
          resource.y = position.y;
          resource.z = position.z;
          resource.usable = intact;
          if (resource.usable) {
            building->forceValidUsageNodesValidation();
            resource.usable = building->hasAnyGoodPositionMarkersLeft();
          }
          if (resource.runtimeSerial != 0 && resource.distance <= 250.0) {
            result.nearbyResources.push_back(resource);
          }
        }

        std::string kind;
        if (buildingClass == BCTYPE_FARM) {
          kind = "farm";
        } else if (buildingClass == BCTYPE_CRAFTING ||
                   function == BF_CRAFTING) {
          kind = "crafting";
        } else if (buildingClass == BCTYPE_RESEARCH ||
                   function == BF_RESEARCH) {
          kind = "research";
        } else if (buildingClass == BCTYPE_STORAGE ||
                   function == BF_RESOURCE_STORAGE ||
                   function == BF_GENERAL_STORAGE ||
                   function == BF_LIQUID_TANK) {
          kind = "storage";
        } else if (function == BF_GENERATOR || function == BF_BATTERY) {
          kind = "power";
        } else if (buildingClass == BCTYPE_TURRET ||
                   function == BF_TURRET) {
          kind = "turret";
        }
        if (kind.empty() || result.nearbyWork.size() >= 64 ||
            distance > 250.0) {
          continue;
        }

        NearbyWorkSnapshot work;
        work.runtimeSerial = building->getHandle().serial;
        work.name = building->getName();
        work.kind = kind;
        work.distance = distance;
        work.x = position.x;
        work.y = position.y;
        work.z = position.z;
        work.usable = intact;
        UseableStuff *useable = static_cast<UseableStuff *>(building);
        if (useable && reinterpret_cast<uintptr_t>(useable) > 0x1000) {
          work.powerOn = useable->_NV_isPowerOn();
          work.powerOutput = useable->_NV_getPowerOutput();
        }

        StorageBuilding *storage = NULL;
        if (buildingClass == BCTYPE_STORAGE ||
            buildingClass == BCTYPE_PRODUCTION ||
            buildingClass == BCTYPE_CRAFTING ||
            buildingClass == BCTYPE_FARM || function == BF_GENERATOR ||
            function == BF_BATTERY) {
          storage = static_cast<StorageBuilding *>(building)
                        ->_NV_getFunctionStuff();
        }
        if (storage && reinterpret_cast<uintptr_t>(storage) > 0x1000) {
          work.inputEmpty = storage->_NV_isAnyInputsEmpty();
          work.inputFull = storage->_NV_isAnyInputsFull();
          work.outputEmpty = storage->_NV_isProductionEmpty();
          work.outputFull = storage->_NV_isProductionFull();
        }
        if (kind == "crafting") {
          work.workQueued =
              static_cast<CraftingBuilding *>(building)
                  ->_NV_hasCraftingQueued();
          work.taskType = static_cast<int>(
              static_cast<ProductionBuilding *>(building)
                  ->_NV_getDefaultTask());
          work.needsWork = work.workQueued && !work.inputEmpty &&
                           !work.outputFull && work.powerOn;
        } else if (kind == "farm") {
          work.taskType = static_cast<int>(
              static_cast<ProductionBuilding *>(building)
                  ->_NV_getDefaultTask());
          work.needsWork =
              !static_cast<FarmBuilding *>(building)
                   ->_NV_dontNeedWorkRightNow() &&
              !work.outputFull && work.powerOn;
        } else if (kind == "research") {
          work.taskType = static_cast<int>(
              static_cast<ResearchBuilding *>(building)
                  ->_NV_getDefaultTask());
          work.needsWork =
              !static_cast<ResearchBuilding *>(building)
                   ->_NV_dontNeedWorkRightNow() &&
              work.powerOn;
        } else if (kind == "power") {
          work.taskType = static_cast<int>(
              static_cast<ProductionBuilding *>(building)
                  ->_NV_getDefaultTask());
          work.powerOutput =
              static_cast<GeneratorBuilding *>(building)
                  ->_NV_getPowerOutput();
          work.needsWork = work.inputEmpty;
        } else if (kind == "turret") {
          work.taskType = static_cast<int>(
              static_cast<TurretBuilding *>(building)
                  ->_NV_getDefaultTask());
          work.slotAvailable = building->hasAnyGoodPositionMarkersLeft();
          work.needsWork = work.slotAvailable && work.powerOn;
        } else if (kind == "storage") {
          work.taskType = static_cast<int>(
              static_cast<StorageBuilding *>(building)
                  ->_NV_getDefaultTask());
        }
        work.usable = work.usable && work.powerOn;
        if (kind == "storage") {
          // Storage is hauling context, not an executable work order yet.
          work.needsWork = false;
        }
        if (work.runtimeSerial != 0) {
          result.nearbyWork.push_back(work);
        }
      } catch (...) {
      }
    }
  } catch (...) {
  }
  return result;
}

} // namespace KenshiAi
} // namespace Stobe
