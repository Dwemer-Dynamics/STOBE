#include "AutonomySafetyProbePolicy.h"

#include <algorithm>
#include <cctype>

namespace Stobe {
namespace AutonomySafetyProbe {

MutationPreconditions::MutationPreconditions()
    : enabled(false), hasBoundTarget(false), isPlayerCharacter(false),
      isDead(false), isUnconscious(false), hasOrdersReceiver(false),
      canTakeOrders(false), hasOrders(false), travelDestinationSet(false) {}

CommandType ParseCommand(const std::string &value) {
  std::string normalized = value;
  normalized.erase(normalized.begin(),
                   std::find_if(normalized.begin(), normalized.end(),
                                [](unsigned char ch) { return !std::isspace(ch); }));
  normalized.erase(
      std::find_if(normalized.rbegin(), normalized.rend(),
                   [](unsigned char ch) { return !std::isspace(ch); })
          .base(),
      normalized.end());
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) { return (char)std::toupper(ch); });

  if (normalized == "OBSERVE")
    return COMMAND_OBSERVE;
  if (normalized == "IDLE")
    return COMMAND_IDLE;
  if (normalized == "TRAVEL")
    return COMMAND_TRAVEL;
  if (normalized == "RESET")
    return COMMAND_RESET;
  return COMMAND_NONE;
}

const char *CommandName(CommandType command) {
  switch (command) {
  case COMMAND_OBSERVE:
    return "OBSERVE";
  case COMMAND_IDLE:
    return "IDLE";
  case COMMAND_TRAVEL:
    return "TRAVEL";
  case COMMAND_RESET:
    return "RESET";
  default:
    return "NONE";
  }
}

ValidationResult ValidateMutation(
    CommandType command, const MutationPreconditions &preconditions) {
  if (!preconditions.enabled)
    return VALIDATION_DISABLED;
  if (!preconditions.hasBoundTarget)
    return VALIDATION_NO_BOUND_TARGET;
  if (!preconditions.isPlayerCharacter)
    return VALIDATION_NOT_PLAYER_CHARACTER;
  if (preconditions.isDead)
    return VALIDATION_TARGET_DEAD;
  if (preconditions.isUnconscious)
    return VALIDATION_TARGET_UNCONSCIOUS;
  if (!preconditions.hasOrdersReceiver)
    return VALIDATION_NO_ORDERS_RECEIVER;
  if (!preconditions.canTakeOrders)
    return VALIDATION_CANNOT_TAKE_ORDERS;
  if (preconditions.hasOrders)
    return VALIDATION_EXISTING_ORDER;
  if (command == COMMAND_TRAVEL && !preconditions.travelDestinationSet)
    return VALIDATION_TRAVEL_DESTINATION_NOT_SET;
  return VALIDATION_OK;
}

const char *ValidationResultName(ValidationResult result) {
  switch (result) {
  case VALIDATION_OK:
    return "ok";
  case VALIDATION_DISABLED:
    return "disabled";
  case VALIDATION_NO_BOUND_TARGET:
    return "no_bound_target";
  case VALIDATION_NOT_PLAYER_CHARACTER:
    return "not_player_character";
  case VALIDATION_TARGET_DEAD:
    return "target_dead";
  case VALIDATION_TARGET_UNCONSCIOUS:
    return "target_unconscious";
  case VALIDATION_NO_ORDERS_RECEIVER:
    return "no_orders_receiver";
  case VALIDATION_CANNOT_TAKE_ORDERS:
    return "cannot_take_orders";
  case VALIDATION_EXISTING_ORDER:
    return "existing_order";
  case VALIDATION_TRAVEL_DESTINATION_NOT_SET:
    return "travel_destination_not_set";
  default:
    return "unknown";
  }
}

} // namespace AutonomySafetyProbe
} // namespace Stobe

