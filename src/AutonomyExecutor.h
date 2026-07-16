#pragma once

#include "AutonomyMonitor.h"
#include "AutonomyProtocol.h"

#include <string>

class GameWorld;

namespace Stobe {
namespace Autonomy {

struct DispatchResult {
  bool success;
  bool completedImmediately;
  bool awaitingExecutionResult;
  std::string reason;
  OrderFingerprint ownedOrder;
  bool jobsPreserved;

  DispatchResult();
};

DispatchResult DispatchDecision(GameWorld *world,
                                const DecisionEnvelope &decision);
bool TryCancelOwnedOrder(GameWorld *world, unsigned int runtimeSerial,
                         const OrderFingerprint &ownedOrder,
                         std::string &reasonOut);

} // namespace Autonomy
} // namespace Stobe
