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

bool ParseControlResponse(const std::string &response, ControlSnapshot &out);
unsigned int ParseStorageSerial(const std::string &storageId);
StateDecision EvaluatePhase1State(const ControlSnapshot &control,
                                  const RuntimeFacts &facts);

} // namespace Autonomy
} // namespace Stobe
