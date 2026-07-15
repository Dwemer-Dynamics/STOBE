#pragma once

#include <string>

namespace Stobe {
namespace AutonomySafetyProbe {

enum CommandType {
  COMMAND_NONE = 0,
  COMMAND_OBSERVE,
  COMMAND_IDLE,
  COMMAND_TRAVEL,
  COMMAND_RESET
};

enum ValidationResult {
  VALIDATION_OK = 0,
  VALIDATION_DISABLED,
  VALIDATION_NO_BOUND_TARGET,
  VALIDATION_NOT_PLAYER_CHARACTER,
  VALIDATION_TARGET_DEAD,
  VALIDATION_TARGET_UNCONSCIOUS,
  VALIDATION_NO_ORDERS_RECEIVER,
  VALIDATION_CANNOT_TAKE_ORDERS,
  VALIDATION_EXISTING_ORDER,
  VALIDATION_TRAVEL_DESTINATION_NOT_SET
};

struct MutationPreconditions {
  bool enabled;
  bool hasBoundTarget;
  bool isPlayerCharacter;
  bool isDead;
  bool isUnconscious;
  bool hasOrdersReceiver;
  bool canTakeOrders;
  bool hasOrders;
  bool travelDestinationSet;

  MutationPreconditions();
};

CommandType ParseCommand(const std::string &value);
const char *CommandName(CommandType command);
ValidationResult ValidateMutation(CommandType command,
                                  const MutationPreconditions &preconditions);
const char *ValidationResultName(ValidationResult result);

} // namespace AutonomySafetyProbe
} // namespace Stobe

