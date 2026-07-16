#pragma once

#include "AutonomyProtocol.h"

#include <string>

namespace Stobe {
namespace Autonomy {

enum MonitorOutcome {
  MONITOR_RUNNING = 0,
  MONITOR_COMPLETED,
  MONITOR_FAILED,
  MONITOR_INTERRUPTED,
  MONITOR_TIMED_OUT,
  MONITOR_UNSAFE
};

struct OrderFingerprint {
  int taskType;
  unsigned int subjectSerial;
  double x;
  double y;
  double z;
  unsigned long long runtimePointer;
  int orderCount;

  OrderFingerprint();
};

struct MonitorFacts {
  bool found;
  bool identityMatches;
  bool playerCharacter;
  bool dead;
  bool unconscious;
  bool paused;
  bool moving;
  bool pathFailed;
  bool hasPlayerOrders;
  bool fullyRested;
  bool inBed;
  bool targetFound;
  bool targetDead;
  bool targetUnconscious;
  bool inCombat;
  bool hostileObserved;
  int pathFailedSamples;
  int stationarySamples;
  int activeElapsedMs;
  int noProgressElapsedMs;
  double x;
  double y;
  double z;
  double firstAidNeed;
  double bleedRate;
  double targetFirstAidNeed;
  double targetBleedRate;
  double nearestHostileDistance;
  double fleeDistanceTravelled;
  unsigned int attackTargetSerial;
  OrderFingerprint currentOrder;

  MonitorFacts();
};

struct MonitorResult {
  MonitorOutcome outcome;
  std::string reason;
  double distance;

  MonitorResult(MonitorOutcome outcomeValue, const std::string &reasonValue,
                double distanceValue = 0.0);
};

bool OrderFingerprintMatches(const OrderFingerprint &expected,
                             const OrderFingerprint &current,
                             double locationTolerance = 0.5);
MonitorResult EvaluateActionMonitor(const DecisionEnvelope &decision,
                                    const OrderFingerprint &ownedOrder,
                                    const MonitorFacts &facts,
                                    int actionDeadlineMs);
const char *MonitorOutcomeName(MonitorOutcome outcome);

} // namespace Autonomy
} // namespace Stobe
