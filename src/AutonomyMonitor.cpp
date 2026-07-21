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
      hasPlayerOrders(false), fullyRested(true), inBed(false),
      targetFound(false), targetDead(false), targetUnconscious(false),
      inCombat(false), hostileObserved(false), nativeTaskExpired(false),
      nativeGoalExpired(false), nativeIntendsToAttackTarget(false),
      pathFailedSamples(0), nativeExpiredSamples(0),
      nativePathFailureCount(0), stationarySamples(0), activeElapsedMs(0),
      noProgressElapsedMs(0), x(0.0), y(0.0), z(0.0),
      firstAidNeed(0.0), bleedRate(0.0), targetFirstAidNeed(0.0),
      targetBleedRate(0.0), nearestHostileDistance(1000000.0),
      fleeDistanceTravelled(0.0), attackTargetSerial(0) {}

MonitorResult::MonitorResult(MonitorOutcome outcomeValue,
                             const std::string &reasonValue,
                             double distanceValue)
    : outcome(outcomeValue), reason(reasonValue), distance(distanceValue) {}

bool OrderFingerprintMatches(const OrderFingerprint &expected,
                             const OrderFingerprint &current,
                             double locationTolerance) {
  if (expected.orderCount != 1 || current.orderCount != 1 ||
      expected.taskType != current.taskType ||
      (expected.subjectSerial != 0 && current.subjectSerial != 0 &&
       expected.subjectSerial != current.subjectSerial)) {
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

  const bool nativeGoalActive = !facts.nativeCurrentGoal.empty() &&
                                !facts.nativeTaskExpired &&
                                !facts.nativeGoalExpired;
  if (!orderPresent && facts.nativeExpiredSamples >= 3) {
    return MonitorResult(MONITOR_FAILED, "native_goal_expired");
  }

  if (decision.command == DECISION_COMMAND_IDLE) {
    if (!facts.moving && facts.stationarySamples >= 3 &&
        facts.activeElapsedMs >= decision.idleDurationMs) {
      return MonitorResult(MONITOR_COMPLETED, "idle_stable");
    }
    if (!orderPresent && !nativeGoalActive && facts.activeElapsedMs >= 2000) {
      return MonitorResult(MONITOR_FAILED, "owned_order_missing");
    }
    return MonitorResult(MONITOR_RUNNING, "idle_in_progress");
  }

  if (decision.command == DECISION_COMMAND_TRAVEL_LOCATION ||
      decision.command == DECISION_COMMAND_MOVE_NEARBY ||
      decision.command == DECISION_COMMAND_FLEE) {
    const double dx = facts.x - decision.x;
    const double dy = facts.y - decision.y;
    const double dz = facts.z - decision.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (decision.command == DECISION_COMMAND_FLEE &&
        facts.fleeDistanceTravelled >= 20.0 &&
        (!facts.hostileObserved ||
         facts.nearestHostileDistance >= decision.safeRadius)) {
      return MonitorResult(MONITOR_COMPLETED, "safe_distance_reached",
                           facts.nearestHostileDistance);
    }
    if (decision.command == DECISION_COMMAND_FLEE &&
        distance <= decision.arrivalRadius && facts.stationarySamples >= 2) {
      return MonitorResult(MONITOR_FAILED, "flee_destination_unsafe",
                           facts.nearestHostileDistance);
    }
    if (distance <= decision.arrivalRadius && facts.stationarySamples >= 2) {
      return MonitorResult(MONITOR_COMPLETED, "destination_reached",
                           distance);
    }
    if (facts.pathFailedSamples >= 3 ||
        (facts.pathFailedSamples >= 1 && facts.nativePathFailureCount > 0)) {
      return MonitorResult(MONITOR_FAILED, "path_failed", distance);
    }
    if (facts.noProgressElapsedMs >= 30000) {
      return MonitorResult(MONITOR_FAILED, "no_travel_progress", distance);
    }
    if (!orderPresent && !nativeGoalActive && facts.activeElapsedMs >= 2000) {
      return MonitorResult(MONITOR_FAILED, "owned_order_missing", distance);
    }
    return MonitorResult(MONITOR_RUNNING,
                         decision.command == DECISION_COMMAND_FLEE
                             ? "flee_in_progress"
                             : "travel_in_progress",
                         distance);
  }
  if (decision.command == DECISION_COMMAND_FIRST_AID) {
    if (!facts.targetFound) {
      return MonitorResult(MONITOR_FAILED, "first_aid_target_not_loaded");
    }
    if (facts.targetDead) {
      return MonitorResult(MONITOR_FAILED, "first_aid_target_dead");
    }
    if (facts.targetFirstAidNeed <= 0.05 &&
        facts.targetBleedRate <= 0.01) {
      return MonitorResult(MONITOR_COMPLETED, "first_aid_complete");
    }
    if (facts.noProgressElapsedMs >= 45000) {
      return MonitorResult(MONITOR_FAILED, "no_first_aid_progress");
    }
    if (!orderPresent && !nativeGoalActive && facts.activeElapsedMs >= 2000) {
      return MonitorResult(MONITOR_FAILED, "owned_order_missing");
    }
    return MonitorResult(MONITOR_RUNNING, "first_aid_in_progress");
  }
  if (decision.command == DECISION_COMMAND_REST) {
    if (facts.fullyRested) {
      return MonitorResult(MONITOR_COMPLETED, "fully_rested");
    }
    if (!facts.inBed &&
        (facts.pathFailedSamples >= 3 ||
         (facts.pathFailedSamples >= 1 &&
          facts.nativePathFailureCount > 0))) {
      return MonitorResult(MONITOR_FAILED, "rest_path_failed");
    }
    if (!facts.inBed && !orderPresent && !nativeGoalActive &&
        facts.activeElapsedMs >= 2000) {
      return MonitorResult(MONITOR_FAILED, "owned_order_missing");
    }
    return MonitorResult(MONITOR_RUNNING,
                         facts.inBed ? "resting_in_bed" : "seeking_rest");
  }
  if (decision.command == DECISION_COMMAND_ATTACK) {
    if (facts.targetFound &&
        (facts.targetDead || facts.targetUnconscious)) {
      return MonitorResult(MONITOR_COMPLETED, "attack_target_neutralized");
    }
    if (!facts.targetFound && facts.activeElapsedMs >= 2000) {
      return MonitorResult(MONITOR_FAILED, "attack_target_not_loaded");
    }
    if (facts.attackTargetSerial != 0 &&
        facts.attackTargetSerial != decision.targetRuntimeSerial) {
      return MonitorResult(MONITOR_INTERRUPTED,
                           "combat_target_replaced");
    }
    if (!orderPresent && !facts.inCombat &&
        !facts.nativeIntendsToAttackTarget && !nativeGoalActive &&
        facts.activeElapsedMs >= 3000) {
      return MonitorResult(MONITOR_FAILED, "attack_order_not_active");
    }
    return MonitorResult(MONITOR_RUNNING, "attack_in_progress");
  }
  if (decision.command == DECISION_COMMAND_WORK_RESOURCE) {
    if (facts.hostileObserved && facts.nearestHostileDistance <= 70.0) {
      return MonitorResult(MONITOR_UNSAFE, "resource_work_threat_detected");
    }
    if (facts.pathFailedSamples >= 3 ||
        (facts.pathFailedSamples >= 1 && facts.nativePathFailureCount > 0)) {
      return MonitorResult(MONITOR_FAILED, "resource_work_path_failed");
    }
    if (facts.activeElapsedMs >= 30000 && orderPresent) {
      return MonitorResult(MONITOR_COMPLETED, "resource_work_cycle_observed");
    }
    if (!orderPresent && !nativeGoalActive && facts.activeElapsedMs >= 3000) {
      return MonitorResult(MONITOR_FAILED, "resource_work_order_missing");
    }
    return MonitorResult(MONITOR_RUNNING, "resource_work_in_progress");
  }
  if (decision.command == DECISION_COMMAND_PROSPECT) {
    if (facts.hostileObserved && facts.nearestHostileDistance <= 70.0) {
      return MonitorResult(MONITOR_UNSAFE, "prospecting_threat_detected");
    }
    if (facts.pathFailedSamples >= 3 ||
        (facts.pathFailedSamples >= 1 && facts.nativePathFailureCount > 0)) {
      return MonitorResult(MONITOR_FAILED, "prospecting_path_failed");
    }
    if (facts.activeElapsedMs >= 15000) {
      return MonitorResult(MONITOR_COMPLETED, "prospecting_scan_completed");
    }
    if (!orderPresent && !nativeGoalActive && facts.activeElapsedMs >= 3000) {
      return MonitorResult(MONITOR_FAILED, "prospecting_order_missing");
    }
    return MonitorResult(MONITOR_RUNNING, "prospecting_in_progress");
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
