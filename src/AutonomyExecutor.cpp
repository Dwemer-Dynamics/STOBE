#include "AutonomyExecutor.h"

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

namespace Stobe {
namespace Autonomy {
namespace {

bool IsCharacterValid(Character *character) {
  return character && reinterpret_cast<uintptr_t>(character) > 0x1000;
}

OrderFingerprint CaptureFirstOrder(OrdersReceiver *orders, TaskType expectedTask,
                                   const Ogre::Vector3 &expectedLocation) {
  OrderFingerprint result;
  if (!orders || reinterpret_cast<uintptr_t>(orders) <= 0x1000) {
    return result;
  }
  try {
    result.orderCount = static_cast<int>(orders->orders.list.size());
    void *first = orders->getFirstOrder();
    if (!first || reinterpret_cast<uintptr_t>(first) <= 0x1000) {
      return result;
    }
    result.runtimePointer =
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(first));
    result.taskType = static_cast<int>(expectedTask);
    result.subjectSerial = 0;
    result.x = expectedLocation.x;
    result.y = expectedLocation.y;
    result.z = expectedLocation.z;
  } catch (...) {
    return OrderFingerprint();
  }
  return result;
}

} // namespace

DispatchResult::DispatchResult()
    : success(false), jobsPreserved(false) {}

DispatchResult DispatchDecision(GameWorld *world,
                                const DecisionEnvelope &decision) {
  DispatchResult result;
  Character *character =
      Stobe::KenshiAi::ResolveCharacter(world, decision.runtimeSerial);
  if (!IsCharacterValid(character)) {
    result.reason = "selected_npc_not_loaded";
    return result;
  }
  try {
    if (character->getHandle().serial != decision.runtimeSerial) {
      result.reason = "runtime_serial_mismatch";
      return result;
    }
    if (!character->isPlayerCharacter()) {
      result.reason = "selected_npc_not_player_faction";
      return result;
    }
    if (character->isDead()) {
      result.reason = "selected_npc_dead";
      return result;
    }
    if (character->isUnconcious()) {
      result.reason = "selected_npc_unconscious";
      return result;
    }
    if (!character->canTakePlayerOrdersAtThisTime()) {
      result.reason = "selected_npc_cannot_take_orders";
      return result;
    }
  } catch (...) {
    result.reason = "character_validation_failed";
    return result;
  }

  const Stobe::KenshiAi::CharacterSnapshot live =
      Stobe::KenshiAi::CaptureCharacter(world, decision.runtimeSerial);
  if (decision.command == DECISION_COMMAND_FLEE) {
    bool hostileObserved = false;
    for (size_t i = 0; i < live.nearbyActors.size(); ++i) {
      if (live.nearbyActors[i].hostile && !live.nearbyActors[i].dead &&
          live.nearbyActors[i].distance <= decision.safeRadius) {
        hostileObserved = true;
        break;
      }
    }
    if (!hostileObserved) {
      result.reason = "flee_threat_no_longer_present";
      return result;
    }
  }
  if (decision.command == DECISION_COMMAND_REST && live.fullyRested) {
    result.reason = "already_fully_rested";
    return result;
  }
  if (decision.command == DECISION_COMMAND_REST) {
    for (size_t i = 0; i < live.nearbyActors.size(); ++i) {
      if (live.nearbyActors[i].hostile && !live.nearbyActors[i].dead &&
          live.nearbyActors[i].distance <= 70.0) {
        result.reason = "rest_hostile_nearby";
        return result;
      }
    }
  }
  Building *restBed = NULL;
  if (decision.command == DECISION_COMMAND_REST) {
    restBed = Stobe::KenshiAi::ResolveNearestRestBed(world, character, 250.0);
    if (!restBed || reinterpret_cast<uintptr_t>(restBed) <= 0x1000) {
      result.reason = "rest_bed_not_found";
      return result;
    }
  }

  OrdersReceiver *orders = NULL;
  try {
    orders = character->getOrdersReciever();
  } catch (...) {
  }
  if (!orders || reinterpret_cast<uintptr_t>(orders) <= 0x1000) {
    result.reason = "orders_receiver_unavailable";
    return result;
  }

  bool jobsEnabledBefore = false;
  int permajobCountBefore = 0;
  try {
    if (orders->hasPlayerOrders() || !orders->orders.list.empty()) {
      result.reason = "existing_player_order";
      return result;
    }
    jobsEnabledBefore = orders->isJobsEnabled();
    permajobCountBefore = orders->getPermajobCount();
  } catch (...) {
    result.reason = "order_precondition_failed";
    return result;
  }

  TaskType task = MOVE_CUS_ORDERED;
  Ogre::Vector3 location;
  hand subject;
  try {
    location = character->getPosition();
    if (decision.command == DECISION_COMMAND_IDLE) {
      task = IDLE;
    } else if (decision.command == DECISION_COMMAND_REST) {
      task = USE_BED_ORDER;
      subject = restBed->getHandle();
      location = restBed->getPosition();
    } else if (decision.command == DECISION_COMMAND_FIRST_AID) {
      Character *target = Stobe::KenshiAi::ResolveCharacter(
          world, decision.targetRuntimeSerial);
      if (!IsCharacterValid(target) || !target->isPlayerCharacter() ||
          target->isDead()) {
        result.reason = "first_aid_target_invalid";
        return result;
      }
      const Ogre::Vector3 targetPosition = target->getPosition();
      const Ogre::Vector3 delta = targetPosition - character->getPosition();
      if (std::sqrt(static_cast<double>(delta.x * delta.x + delta.y * delta.y +
                                        delta.z * delta.z)) > 50.0) {
        result.reason = "first_aid_target_out_of_range";
        return result;
      }
      MedicalSystem *medical = target->getMedical();
      if (!medical || reinterpret_cast<uintptr_t>(medical) <= 0x1000) {
        result.reason = "first_aid_target_medical_unavailable";
        return result;
      }
      const double fleshNeed = std::max(
          0.0, static_cast<double>(medical->scoreFirstAidNeed(false)));
      const double robotNeed = std::max(
          0.0, static_cast<double>(medical->scoreFirstAidNeed(true)));
      if (fleshNeed <= 0.05 && robotNeed <= 0.05) {
        result.reason = "first_aid_no_longer_needed";
        return result;
      }
      task = robotNeed > fleshNeed ? FIRST_AID_ROBOT : FIRST_AID_ORDER;
      subject = target->getHandle();
      location = targetPosition;
    } else {
      location = Ogre::Vector3(static_cast<float>(decision.x),
                               static_cast<float>(decision.y),
                               static_cast<float>(decision.z));
    }
    if (decision.command == DECISION_COMMAND_FLEE) {
      // A raw move order does not reliably displace Kenshi's active combat goal.
      // Use the same disengage/move path as a player-issued ground command.
      character->endCombatMode();
      character->playerMoveOrderDefault(NULL, NULL, location);
    } else {
      orders->addOrder(task, subject, location, false, false);
    }
    if (decision.command == DECISION_COMMAND_TRAVEL_LOCATION ||
        decision.command == DECISION_COMMAND_MOVE_NEARBY ||
        decision.command == DECISION_COMMAND_FLEE) {
      CharMovement *movement = character->getMovement();
      if (movement && reinterpret_cast<uintptr_t>(movement) > 0x1000) {
        movement->setDesiredSpeedOrders(RUN);
        movement->setDesiredSpeed(RUN);
      }
    }
    character->reThinkCurrentAIAction();
  } catch (...) {
    result.reason = "order_dispatch_exception";
    return result;
  }

  result.ownedOrder = CaptureFirstOrder(orders, task, location);
  try {
    result.jobsPreserved = jobsEnabledBefore == orders->isJobsEnabled() &&
                           permajobCountBefore == orders->getPermajobCount();
  } catch (...) {
    result.jobsPreserved = false;
  }
  if (!OrderFingerprintMatches(result.ownedOrder, result.ownedOrder) ||
      result.ownedOrder.taskType != static_cast<int>(task)) {
    result.reason = "order_not_observable_after_dispatch";
    return result;
  }
  if (!result.jobsPreserved) {
    result.reason = "jobs_changed_during_dispatch";
    return result;
  }
  result.success = true;
  result.reason = "owned_order_accepted";
  return result;
}

bool TryCancelOwnedOrder(GameWorld *world, unsigned int runtimeSerial,
                         const OrderFingerprint &ownedOrder,
                         std::string &reasonOut) {
  Character *character =
      Stobe::KenshiAi::ResolveCharacter(world, runtimeSerial);
  if (!IsCharacterValid(character)) {
    reasonOut = "selected_npc_not_loaded";
    return false;
  }
  OrdersReceiver *orders = NULL;
  try {
    orders = character->getOrdersReciever();
  } catch (...) {
  }
  if (!orders || reinterpret_cast<uintptr_t>(orders) <= 0x1000) {
    reasonOut = "orders_receiver_unavailable";
    return false;
  }
  OrderFingerprint current;
  try {
    current.orderCount = static_cast<int>(orders->orders.list.size());
    void *first = orders->getFirstOrder();
    if (first && reinterpret_cast<uintptr_t>(first) > 0x1000) {
      current.runtimePointer = static_cast<unsigned long long>(
          reinterpret_cast<uintptr_t>(first));
      current.taskType = ownedOrder.taskType;
      current.subjectSerial = ownedOrder.subjectSerial;
      current.x = ownedOrder.x;
      current.y = ownedOrder.y;
      current.z = ownedOrder.z;
    }
  } catch (...) {
    reasonOut = "owned_order_capture_failed";
    return false;
  }
  if (current.orderCount == 0) {
    reasonOut = "owned_order_already_complete";
    return true;
  }
  if (!OrderFingerprintMatches(ownedOrder, current)) {
    reasonOut = "owned_order_not_safe_to_cancel";
    return false;
  }
  try {
    orders->clearOrders();
    character->reThinkCurrentAIAction();
    reasonOut = "owned_order_cancelled";
    return true;
  } catch (...) {
    reasonOut = "owned_order_cancel_failed";
    return false;
  }
}

} // namespace Autonomy
} // namespace Stobe
