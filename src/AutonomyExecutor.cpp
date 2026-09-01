#include "AutonomyExecutor.h"

#include "Functions.h"
#include "Globals.h"
#include "KenshiAiCompat.h"
#include "KenshiBuildingCompat.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <vector>

#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/CharMovement.h>
#include <kenshi/Character.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Inventory.h>
#include <kenshi/Item.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/ShopTrader.h>

namespace Stobe {
namespace Autonomy {
namespace {

bool IsCharacterValid(Character *character) {
  return character && reinterpret_cast<uintptr_t>(character) > 0x1000;
}

std::string NormalizeItemToken(const std::string &value) {
  std::string result;
  bool previousSpace = false;
  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(value[i]);
    if (std::isalnum(ch)) {
      result.push_back(static_cast<char>(std::tolower(ch)));
      previousSpace = false;
    } else if (!result.empty() && !previousSpace) {
      result.push_back(' ');
      previousSpace = true;
    }
  }
  while (!result.empty() && result[result.size() - 1] == ' ') {
    result.erase(result.size() - 1);
  }
  return result;
}

bool ItemMatches(Item *item, const std::string &queryToken) {
  if (!item || reinterpret_cast<uintptr_t>(item) <= 0x1000 ||
      queryToken.empty()) {
    return false;
  }
  try {
    const std::string itemToken = NormalizeItemToken(item->getName());
    return !itemToken.empty() && itemToken == queryToken;
  } catch (...) {
    return false;
  }
}

int CountMatchingItems(Character *character, const std::string &queryToken) {
  if (!IsCharacterValid(character) || queryToken.empty()) {
    return 0;
  }
  Inventory *inventory = NULL;
  try {
    inventory = character->getInventory();
  } catch (...) {
  }
  if (!inventory || reinterpret_cast<uintptr_t>(inventory) <= 0x1000) {
    return 0;
  }
  int count = 0;
  try {
    const lektor<Item *> &items = inventory->getAllItems();
    for (uint32_t i = 0; i < items.size(); ++i) {
      Item *item = items.stuff[i];
      if (!ItemMatches(item, queryToken)) {
        continue;
      }
      int quantity = 1;
      try {
        quantity = item->quantity > 0 ? item->quantity : 1;
      } catch (...) {
      }
      count += quantity;
    }
  } catch (...) {
    return 0;
  }
  return count;
}

Item *FindMatchingItem(Character *character, const std::string &queryToken) {
  if (!IsCharacterValid(character) || queryToken.empty()) {
    return NULL;
  }
  Inventory *inventory = NULL;
  try {
    inventory = character->getInventory();
  } catch (...) {
  }
  if (!inventory || reinterpret_cast<uintptr_t>(inventory) <= 0x1000) {
    return NULL;
  }
  try {
    const lektor<Item *> &items = inventory->getAllItems();
    Item *partialMatch = NULL;
    std::string partialToken;
    for (uint32_t i = 0; i < items.size(); ++i) {
      Item *item = items.stuff[i];
      if (!item || reinterpret_cast<uintptr_t>(item) <= 0x1000) {
        continue;
      }
      const std::string itemToken = NormalizeItemToken(item->getName());
      if (itemToken == queryToken) {
        return item;
      }
      if (itemToken.empty() ||
          (itemToken.find(queryToken) == std::string::npos &&
           queryToken.find(itemToken) == std::string::npos)) {
        continue;
      }
      if (partialMatch && partialToken != itemToken) {
        return NULL;
      }
      partialMatch = item;
      partialToken = itemToken;
    }
    return partialMatch;
  } catch (...) {
  }
  return NULL;
}

Item *FindMatchingItem(Inventory *inventory, const std::string &queryToken) {
  if (!inventory || reinterpret_cast<uintptr_t>(inventory) <= 0x1000 ||
      queryToken.empty()) {
    return NULL;
  }
  try {
    const lektor<Item *> &items = inventory->getAllItems();
    Item *match = NULL;
    for (uint32_t i = 0; i < items.size(); ++i) {
      Item *item = items.stuff[i];
      if (!ItemMatches(item, queryToken)) {
        continue;
      }
      if (match) {
        return NULL;
      }
      match = item;
    }
    return match;
  } catch (...) {
    return NULL;
  }
}

int CountMatchingItems(Inventory *inventory, const std::string &queryToken) {
  if (!inventory || reinterpret_cast<uintptr_t>(inventory) <= 0x1000 ||
      queryToken.empty()) {
    return 0;
  }
  int count = 0;
  try {
    const lektor<Item *> &items = inventory->getAllItems();
    for (uint32_t i = 0; i < items.size(); ++i) {
      Item *item = items.stuff[i];
      if (ItemMatches(item, queryToken)) {
        count += item->quantity > 0 ? item->quantity : 1;
      }
    }
  } catch (...) {
    return 0;
  }
  return count;
}

Building *ResolveResourceBuilding(GameWorld *world, Character *character,
                                  unsigned int serial) {
  if (!world || !IsCharacterValid(character) || serial == 0) {
    return NULL;
  }
  try {
    lektor<RootObject *> nearby;
    world->getObjectsWithinSphere(nearby, character->getPosition(), 300.0f,
                                  BUILDING, 160, (RootObject *)character);
    for (uint32_t i = 0; i < nearby.size(); ++i) {
      Building *building = (Building *)nearby.stuff[i];
      if (building && reinterpret_cast<uintptr_t>(building) > 0x1000 &&
          building->getHandle().serial == serial) {
        return building;
      }
    }
  } catch (...) {
  }
  return NULL;
}

double CharacterDistance(Character *left, Character *right) {
  if (!IsCharacterValid(left) || !IsCharacterValid(right)) {
    return 1000000.0;
  }
  try {
    const Ogre::Vector3 delta = left->getPosition() - right->getPosition();
    return std::sqrt(static_cast<double>(
        delta.x * delta.x + delta.y * delta.y + delta.z * delta.z));
  } catch (...) {
    return 1000000.0;
  }
}

bool IsQueuedMutationCommand(DecisionCommand command) {
  return command == DECISION_COMMAND_KNOCKOUT ||
         command == DECISION_COMMAND_KILL ||
         command == DECISION_COMMAND_REMOVE_LIMB ||
         command == DECISION_COMMAND_CUT_HORNS;
}

ActionType QueuedMutationType(DecisionCommand command) {
  switch (command) {
  case DECISION_COMMAND_KNOCKOUT:
    return ACT_KNOCKOUT;
  case DECISION_COMMAND_KILL:
    return ACT_KILL;
  case DECISION_COMMAND_REMOVE_LIMB:
    return ACT_REMOVE_LIMB;
  case DECISION_COMMAND_CUT_HORNS:
    return ACT_CUT_HORNS;
  default:
    return ACT_NOTIFY;
  }
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
    : success(false), completedImmediately(false),
      awaitingExecutionResult(false), jobsPreserved(false) {}

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

  Character *targetCharacter = NULL;
  if (decision.targetRuntimeSerial != 0) {
    targetCharacter = Stobe::KenshiAi::ResolveCharacter(
        world, decision.targetRuntimeSerial);
  }
  if (decision.command == DECISION_COMMAND_ATTACK ||
      decision.command == DECISION_COMMAND_TAKE_ITEM ||
      decision.command == DECISION_COMMAND_BUY_ITEM ||
      decision.command == DECISION_COMMAND_SELL_ITEM ||
      IsQueuedMutationCommand(decision.command)) {
    if (!IsCharacterValid(targetCharacter)) {
      result.reason = "target_not_loaded";
      return result;
    }
    try {
      if (targetCharacter->getHandle().serial !=
          decision.targetRuntimeSerial) {
        result.reason = "target_runtime_serial_mismatch";
        return result;
      }
    } catch (...) {
      result.reason = "target_validation_failed";
      return result;
    }
  }

  if (decision.command == DECISION_COMMAND_BUY_ITEM ||
      decision.command == DECISION_COMMAND_SELL_ITEM) {
    if (targetCharacter == character) {
      result.reason = "trade_self_target";
      return result;
    }
    try {
      if (!targetCharacter->isATrader()) {
        result.reason = "trade_target_not_trader";
        return result;
      }
    } catch (...) {
      result.reason = "trade_target_validation_failed";
      return result;
    }
    if (CharacterDistance(character, targetCharacter) > 15.0) {
      result.reason = "trade_target_out_of_range";
      return result;
    }
    const std::string queryToken = NormalizeItemToken(decision.itemName);
    if (queryToken.empty()) {
      result.reason = "trade_item_invalid";
      return result;
    }
    try {
      ShopTrader trader(targetCharacter);
      Inventory *shopInventory = trader.getInventory();
      Inventory *actorInventory = character->getInventory();
      if (!shopInventory || !actorInventory) {
        result.reason = "trade_inventory_unavailable";
        return result;
      }
      const bool buying = decision.command == DECISION_COMMAND_BUY_ITEM;
      Inventory *sourceInventory = buying ? shopInventory : actorInventory;
      Item *item = FindMatchingItem(sourceInventory, queryToken);
      if (!item) {
        result.reason = "trade_item_not_found_or_ambiguous";
        return result;
      }
      const int stackCount = item->quantity > 0 ? item->quantity : 1;
      if (stackCount != 1 || decision.itemAmount != 1) {
        result.reason = "trade_requires_single_item_stack";
        return result;
      }
      const int quotedPrice =
          std::max(0, item->getValueSingle(buying));
      if (buying && quotedPrice > decision.maxTotalPrice) {
        result.reason = "buy_price_limit_exceeded";
        return result;
      }
      if (!buying && quotedPrice < decision.minTotalPrice) {
        result.reason = "sell_price_below_minimum";
        return result;
      }
      const int actorCatsBefore = character->getMoney();
      const int actorCountBefore =
          CountMatchingItems(actorInventory, queryToken);
      Item *transferred = sourceInventory->buyItem(
          item, buying ? static_cast<RootObject *>(character)
                       : static_cast<RootObject *>(&trader));
      if (!transferred) {
        result.reason = buying ? "buy_transaction_rejected"
                               : "sell_transaction_rejected";
        return result;
      }
      const int actorCatsAfter = character->getMoney();
      const int actorCountAfter =
          CountMatchingItems(actorInventory, queryToken);
      const int actualPrice =
          buying ? actorCatsBefore - actorCatsAfter
                 : actorCatsAfter - actorCatsBefore;
      const bool inventoryChanged =
          buying ? actorCountAfter > actorCountBefore
                 : actorCountAfter < actorCountBefore;
      const bool moneyChanged = actualPrice > 0;
      if (!inventoryChanged || !moneyChanged ||
          (buying && actualPrice > decision.maxTotalPrice) ||
          (!buying && actualPrice < decision.minTotalPrice)) {
        result.reason = "trade_postcondition_failed";
        return result;
      }
      character->reThinkCurrentAIAction();
      targetCharacter->reThinkCurrentAIAction();
      result.success = true;
      result.completedImmediately = true;
      result.jobsPreserved = true;
      result.reason = buying ? "item_bought" : "item_sold";
      return result;
    } catch (...) {
      result.reason = "trade_transaction_exception";
      return result;
    }
  }

  if (decision.command == DECISION_COMMAND_EQUIP_ITEM) {
    const std::string queryToken = NormalizeItemToken(decision.itemName);
    Item *item = FindMatchingItem(character, queryToken);
    if (!item) {
      result.reason = "equip_item_not_found";
      return result;
    }
    try {
      if (item->isEquipped) {
        result.success = true;
        result.completedImmediately = true;
        result.jobsPreserved = true;
        result.reason = "item_already_equipped";
        return result;
      }
      Inventory *inventory = character->getInventory();
      if (!inventory || reinterpret_cast<uintptr_t>(inventory) <= 0x1000 ||
          !inventory->equipItem(item) || !item->isEquipped) {
        result.reason = "equip_item_failed";
        return result;
      }
      character->reThinkCurrentAIAction();
    } catch (...) {
      result.reason = "equip_item_exception";
      return result;
    }
    result.success = true;
    result.completedImmediately = true;
    result.jobsPreserved = true;
    result.reason = "item_equipped";
    return result;
  }

  if (decision.command == DECISION_COMMAND_TAKE_ITEM) {
    if (targetCharacter == character) {
      result.reason = "loot_self_target";
      return result;
    }
    bool sourceDead = false;
    std::string lootReason;
    if (!IsTakeItemLootTargetValid(world, targetCharacter, lootReason,
                                   sourceDead)) {
      result.reason = lootReason.empty() ? "loot_target_not_helpless"
                                         : "loot_target_" + lootReason;
      return result;
    }
    if (CharacterDistance(character, targetCharacter) > 15.0) {
      result.reason = "loot_target_out_of_range";
      return result;
    }
    const std::string queryToken = NormalizeItemToken(decision.itemName);
    if (queryToken == "equipment" || queryToken == "all" ||
        queryToken == "everything" || queryToken == "inventory" ||
        queryToken == "loot") {
      result.reason = "loot_item_must_be_specific";
      return result;
    }
    Item *item = FindMatchingItem(targetCharacter, queryToken);
    if (!item) {
      result.reason = "loot_item_not_found_or_ambiguous";
      return result;
    }
    std::string resolvedItemToken;
    try {
      resolvedItemToken = NormalizeItemToken(item->getName());
    } catch (...) {
    }
    if (resolvedItemToken.empty()) {
      result.reason = "loot_item_name_unavailable";
      return result;
    }
    const int actorCountBefore =
        CountMatchingItems(character, resolvedItemToken);
    const int sourceCountBefore =
        CountMatchingItems(targetCharacter, resolvedItemToken);
    int transferQuantity = decision.itemAmount;
    try {
      const int available = item->quantity > 0 ? item->quantity : 1;
      transferQuantity = std::min(transferQuantity, available);
      if (item->isEquipped) {
        targetCharacter->unequipItem(item->inventorySection, item);
      }
    } catch (...) {
    }
    Inventory *sourceInventory = NULL;
    try {
      sourceInventory = item->getInventory();
      if (!sourceInventory) {
        sourceInventory = targetCharacter->getInventory();
      }
    } catch (...) {
    }
    if (!sourceInventory ||
        reinterpret_cast<uintptr_t>(sourceInventory) <= 0x1000) {
      result.reason = "loot_source_inventory_unavailable";
      return result;
    }
    Item *detached = NULL;
    try {
      detached = sourceInventory->removeItemDontDestroy_returnsItem(
          item, transferQuantity, false);
    } catch (...) {
    }
    if (!detached || reinterpret_cast<uintptr_t>(detached) <= 0x1000) {
      result.reason = "loot_item_detach_failed";
      return result;
    }
    bool accepted = false;
    try {
      accepted = character->giveItem(detached, false, false);
    } catch (...) {
    }
    if (!accepted) {
      bool restored = false;
      try {
        restored = sourceInventory->addItem(
            detached, transferQuantity, false, false);
      } catch (...) {
      }
      if (!restored) {
        try {
          restored = targetCharacter->giveItem(detached, true, false);
        } catch (...) {
        }
      }
      result.reason = restored ? "loot_recipient_inventory_full"
                               : "loot_recipient_full_rollback_failed";
      return result;
    }
    const int actorCountAfter =
        CountMatchingItems(character, resolvedItemToken);
    const int sourceCountAfter =
        CountMatchingItems(targetCharacter, resolvedItemToken);
    if (actorCountAfter <= actorCountBefore ||
        sourceCountAfter >= sourceCountBefore) {
      result.reason = "loot_postcondition_failed";
      return result;
    }
    try {
      character->reThinkCurrentAIAction();
      targetCharacter->reThinkCurrentAIAction();
    } catch (...) {
    }
    result.success = true;
    result.completedImmediately = true;
    result.jobsPreserved = true;
    result.reason = sourceDead ? "corpse_item_looted" : "item_looted";
    return result;
  }

  if (IsQueuedMutationCommand(decision.command)) {
    if (decision.command != DECISION_COMMAND_KNOCKOUT &&
        targetCharacter == character) {
      result.reason = "self_target_not_allowed";
      return result;
    }
    if ((decision.command == DECISION_COMMAND_REMOVE_LIMB ||
         decision.command == DECISION_COMMAND_CUT_HORNS) &&
        !CharacterHasHacksaw(character)) {
      result.reason = "missing_hacksaw";
      return result;
    }
    if (CharacterDistance(character, targetCharacter) > 15.0) {
      result.reason = "target_out_of_range";
      return result;
    }
    QueuedAction action;
    action.type = QueuedMutationType(decision.command);
    action.actor = character->getHandle();
    action.target = targetCharacter->getHandle();
    action.message = decision.targetName;
    action.targetToken = decision.targetName;
    action.taskValue = decision.limbCode;
    action.autonomyDecisionId = decision.decisionId;
    EnterCriticalSection(&g_uiMutex);
    g_uiActionQueue.push_back(action);
    LeaveCriticalSection(&g_uiMutex);
    result.success = true;
    result.awaitingExecutionResult = true;
    result.jobsPreserved = true;
    result.reason = "validated_action_queued";
    return result;
  }

  TaskType task = MOVE_CUS_ORDERED;
  Ogre::Vector3 location;
  hand subject;
  Building *resourceBuilding = NULL;
  try {
    location = character->getPosition();
    if (decision.command == DECISION_COMMAND_ATTACK) {
      if (targetCharacter == character || targetCharacter->isDead()) {
        result.reason = targetCharacter == character ? "attack_self_target"
                                                     : "attack_target_dead";
        return result;
      }
      task = UNPROVOKED_FOCUSED_MELEE_ATTACK;
      subject = targetCharacter->getHandle();
      location = targetCharacter->getPosition();
    } else if (decision.command == DECISION_COMMAND_IDLE) {
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
    } else if (decision.command == DECISION_COMMAND_WORK_RESOURCE ||
               decision.command == DECISION_COMMAND_PROSPECT) {
      resourceBuilding = ResolveResourceBuilding(
          world, character, decision.resourceRuntimeSerial);
      if (!resourceBuilding) {
        result.reason = "resource_target_not_loaded";
        return result;
      }
      const BuildingFunction function =
          resourceBuilding->_NV_getSpecialFunction();
      if (function != BF_MINE && function != BF_MINE_NATURAL) {
        result.reason = "resource_target_not_mine";
        return result;
      }
      if (resourceBuilding->_NV_isDestroyed() ||
          resourceBuilding->_NV_isBroken()) {
        result.reason = "resource_target_unusable";
        return result;
      }
      resourceBuilding->forceValidUsageNodesValidation();
      if (!resourceBuilding->hasAnyGoodPositionMarkersLeft()) {
        result.reason = "resource_target_occupied";
        return result;
      }
      task = decision.command == DECISION_COMMAND_WORK_RESOURCE
                 ? OPERATE_MACHINERY
                 : PROSPECTING;
      subject = resourceBuilding->getHandle();
      location = resourceBuilding->getPosition();
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
      if (decision.command == DECISION_COMMAND_ATTACK) {
        BreakFactionCeasefireForExplicitAttack(character, targetCharacter,
                                               "autonomy_attack_action");
      }
      orders->addOrder(task, subject, location, false, false);
      if (decision.command == DECISION_COMMAND_ATTACK) {
        character->attackTarget(targetCharacter);
      }
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
  if (resourceBuilding) {
    result.ownedOrder.subjectSerial = decision.resourceRuntimeSerial;
  }
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
    if (ownedOrder.taskType ==
        static_cast<int>(UNPROVOKED_FOCUSED_MELEE_ATTACK)) {
      character->endCombatMode();
    }
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
