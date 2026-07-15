#include "AutonomyMonitor.h"

#include <cmath>

namespace Stobe {
namespace Autonomy {

OrderFingerprint::OrderFingerprint()
    : taskType(0), subjectSerial(0), x(0.0), y(0.0), z(0.0),
      runtimePointer(0), orderCount(0) {}

MonitorFacts::MonitorFacts()
    : found(false), identityMatches(false), playerCharacter(false), dead(false),
      unconscious(false), paused(false), moving(false), pathFailed(false),
      hasPlayerOrders(false), pathFailedSamples(0), stationarySamples(0),
      activeElapsedMs(0), noProgressElapsedMs(0), x(0.0), y(0.0), z(0.0) {}

MonitorResult::MonitorResult(MonitorOutcome outcomeValue,
                             const std::string &reasonValue,
                             double distanceValue)
    : outcome(outcomeValue), reason(reasonValue), distance(distanceValue) {}

bool OrderFingerprintMatches(const OrderFingerprint &expected,
                             const OrderFingerprint &current,
                             double locationTolerance) {
  if (expected.orderCount != 1 || current.orderCount != 1 ||
      expected.taskType != current.taskType ||
      expected.subjectSerial != current.subjectSerial) {
    return false;
  }
  if (expected.runtimePointer != 0 && current.runtimePointer != 0 &&
      expected.runtimePointer != current.runtimePointer) {
    return false;
  }
  if (expected.runtimePointer != 0 && current.runtimePointer != 0) {
    return true;
  }
  const double dx = expected.x - current.x;
  const double dy = expected.y - current.y;
  const double dz = expected.z - current.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz) <= locationTolerance;
}

MonitorResult EvaluateActionMonitor(const DecisionEnvelope &decision,
                                    const OrderFingerprint &ownedOrder,
                                    const MonitorFacts &facts,
                                    int actionDeadlineMs) {
  if (!facts.found) {
    return MonitorResult(MONITOR_UNSAFE, "selected_npc_not_loaded");
  }
  if (!facts.identityMatches) {
    return MonitorResult(MONITOR_FAILED, "npc_identity_mismatch");
  }
  if (!facts.playerCharacter) {
    return MonitorResult(MONITOR_FAILED, "selected_npc_not_player_faction");
  }
  if (facts.dead) {
    return MonitorResult(MONITOR_UNSAFE, "selected_npc_dead");
  }
  if (facts.unconscious) {
    return MonitorResult(MONITOR_UNSAFE, "selected_npc_unconscious");
  }
  if (!facts.paused && actionDeadlineMs > 0 &&
      facts.activeElapsedMs >= actionDeadlineMs) {
    return MonitorResult(MONITOR_TIMED_OUT, "action_deadline_exceeded");
  }

  const bool orderPresent = facts.currentOrder.orderCount > 0;
  const bool ownedOrderMatches =
      orderPresent && OrderFingerprintMatches(ownedOrder, facts.currentOrder);
  if (orderPresent && !ownedOrderMatches) {
    return MonitorResult(MONITOR_INTERRUPTED,
                         "manual_player_order_detected");
  }

  if (decision.command == DECISION_COMMAND_IDLE) {
    if (!facts.moving && facts.stationarySamples >= 3 &&
        facts.activeElapsedMs >= decision.idleDurationMs) {
      return MonitorResult(MONITOR_COMPLETED, "idle_stable");
    }
    if (!orderPresent && facts.activeElapsedMs >= 2000) {
      return MonitorResult(MONITOR_FAILED, "owned_order_missing");
    }
    return MonitorResult(MONITOR_RUNNING, "idle_in_progress");
  }

  if (decision.command == DECISION_COMMAND_TRAVEL_LOCATION) {
    const double dx = facts.x - decision.x;
    const double dy = facts.y - decision.y;
    const double dz = facts.z - decision.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance <= decision.arrivalRadius && facts.stationarySamples >= 2) {
      return MonitorResult(MONITOR_COMPLETED, "destination_reached",
                           distance);
    }
    if (facts.pathFailedSamples >= 3) {
      return MonitorResult(MONITOR_FAILED, "path_failed", distance);
    }
    if (facts.noProgressElapsedMs >= 30000) {
      return MonitorResult(MONITOR_FAILED, "no_travel_progress", distance);
    }
    if (!orderPresent && facts.activeElapsedMs >= 2000) {
      return MonitorResult(MONITOR_FAILED, "owned_order_missing", distance);
    }
    return MonitorResult(MONITOR_RUNNING, "travel_in_progress", distance);
  }
  return MonitorResult(MONITOR_FAILED, "unsupported_command");
}

const char *MonitorOutcomeName(MonitorOutcome outcome) {
  switch (outcome) {
  case MONITOR_RUNNING:
    return "RUNNING";
  case MONITOR_COMPLETED:
    return "COMPLETED";
  case MONITOR_FAILED:
    return "FAILED";
  case MONITOR_INTERRUPTED:
    return "INTERRUPTED";
  case MONITOR_TIMED_OUT:
    return "TIMED_OUT";
  case MONITOR_UNSAFE:
    return "INTERRUPTED";
  default:
    return "FAILED";
  }
}

} // namespace Autonomy
} // namespace Stobe
