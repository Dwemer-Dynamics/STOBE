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
  std::string contextHash;
  long long contextGameTs;
  long long dispatchDeadlineTs;
  long long actionDeadlineTs;
  int idleDurationMs;
  long long locationZoneId;
  std::string locationLabel;
  double x;
  double y;
  double z;
  double arrivalRadius;

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
