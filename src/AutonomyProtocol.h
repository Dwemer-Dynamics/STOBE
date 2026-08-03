#pragma once

#include <string>

namespace Stobe {
namespace Autonomy {

struct ControlSnapshot {
  bool valid;
  bool enabled;
  std::string desiredState;
  long long controlRevision;
  int npcId;
  std::string npcStorageId;
  std::string npcName;
  std::string stopMode;

  ControlSnapshot();
};

struct RuntimeFacts {
  bool serverAvailable;
  bool requiresExplicitResume;
  bool manualPauseLatched;
  bool found;
  bool identityMatches;
  bool playerCharacter;
  bool dead;
  bool unconscious;
  bool hasOrdersReceiver;
  bool canTakeOrders;
  bool hasPlayerOrders;

  RuntimeFacts();
};

struct StateDecision {
  std::string state;
  std::string reason;

  StateDecision(const std::string &stateValue, const std::string &reasonValue);
};

enum DecisionCommand {
  DECISION_COMMAND_NONE = 0,
  DECISION_COMMAND_IDLE,
  DECISION_COMMAND_TRAVEL_LOCATION,
  DECISION_COMMAND_MOVE_NEARBY,
  DECISION_COMMAND_FLEE,
  DECISION_COMMAND_FIRST_AID,
  DECISION_COMMAND_REST,
  DECISION_COMMAND_ATTACK,
  DECISION_COMMAND_TAKE_ITEM,
  DECISION_COMMAND_EQUIP_ITEM,
  DECISION_COMMAND_KNOCKOUT,
  DECISION_COMMAND_KILL,
  DECISION_COMMAND_REMOVE_LIMB,
  DECISION_COMMAND_CUT_HORNS,
  DECISION_COMMAND_BUY_ITEM,
  DECISION_COMMAND_SELL_ITEM,
  DECISION_COMMAND_WORK_RESOURCE,
  DECISION_COMMAND_PROSPECT,
  DECISION_COMMAND_CATALOG_ACTION
};

struct DecisionEnvelope {
  bool valid;
  std::string decisionId;
  long long controlRevision;
  int npcId;
  std::string npcStorageId;
  unsigned int runtimeSerial;
  DecisionCommand command;
  std::string commandName;
  std::string actionArgument;
  std::string targetName;
  std::string itemName;
  std::string limbName;
  std::string contextHash;
  long long contextGameTs;
  long long dispatchDeadlineTs;
  long long actionDeadlineTs;
  int idleDurationMs;
  int itemAmount;
  int maxTotalPrice;
  int minTotalPrice;
  int limbCode;
  long long locationZoneId;
  unsigned int targetRuntimeSerial;
  unsigned int resourceRuntimeSerial;
  std::string locationLabel;
  double x;
  double y;
  double z;
  double arrivalRadius;
  double safeRadius;

  DecisionEnvelope();
};

enum DecisionValidation {
  DECISION_VALID = 0,
  DECISION_INVALID,
  DECISION_STALE_REVISION,
  DECISION_IDENTITY_MISMATCH,
  DECISION_RUNTIME_SERIAL_MISMATCH,
  DECISION_EXPIRED
};

bool ParseControlResponse(const std::string &response, ControlSnapshot &out);
unsigned int ParseStorageSerial(const std::string &storageId);
StateDecision EvaluatePhase1State(const ControlSnapshot &control,
                                  const RuntimeFacts &facts);
bool ParseDecisionResponse(const std::string &response, DecisionEnvelope &out,
                           bool &hasDecision);
DecisionValidation ValidateDecisionEnvelope(const DecisionEnvelope &decision,
                                             const ControlSnapshot &control,
                                             unsigned int runtimeSerial,
                                             long long nowEpochSeconds);
const char *DecisionValidationName(DecisionValidation validation);

} // namespace Autonomy
} // namespace Stobe
