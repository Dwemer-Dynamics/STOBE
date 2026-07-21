#pragma once

#include "AutonomyMonitor.h"

#include <string>
#include <vector>

class Character;
class GameWorld;
class Building;

namespace Stobe {
namespace KenshiAi {

struct InventoryItemSnapshot {
  std::string name;
  int count;
  int buyValueEach;
  int sellValueEach;

  InventoryItemSnapshot();
};

struct NearbyActorSnapshot {
  unsigned int runtimeSerial;
  double distance;
  bool playerCharacter;
  bool trader;
  bool dead;
  bool unconscious;
  bool hostile;
  bool fullyRested;
  bool probablyDying;
  bool inBed;
  double x;
  double y;
  double z;
  double overallHealth;
  double bleedRate;
  double firstAidNeed;
  double roboticAidNeed;
  int cats;
  std::string name;
  std::vector<InventoryItemSnapshot> traderItems;

  NearbyActorSnapshot();
};

struct NearbyResourceSnapshot {
  unsigned int runtimeSerial;
  double distance;
  bool natural;
  bool usable;
  int taskType;
  std::string name;
  double x;
  double y;
  double z;

  NearbyResourceSnapshot();
};

struct NearbyWorkSnapshot {
  unsigned int runtimeSerial;
  double distance;
  bool usable;
  bool needsWork;
  bool inputEmpty;
  bool inputFull;
  bool outputEmpty;
  bool outputFull;
  bool powerOn;
  bool workQueued;
  bool slotAvailable;
  int taskType;
  double powerOutput;
  double x;
  double y;
  double z;
  std::string name;
  std::string kind;

  NearbyWorkSnapshot();
};

struct CharacterSnapshot {
  bool found;
  bool identityMatches;
  bool playerCharacter;
  bool dead;
  bool unconscious;
  bool hasOrdersReceiver;
  bool canTakeOrders;
  bool hasPlayerOrders;
  bool paused;
  bool moving;
  bool pathFailed;
  bool carrying;
  bool fullyRested;
  bool probablyDying;
  bool inBed;
  bool restBedAvailable;
  bool inCombat;
  bool aiTaskExpired;
  bool aiGoalExpired;
  bool aiIntendsToAttackTarget;
  unsigned int carriedSerial;
  unsigned int attackTargetSerial;
  unsigned int runtimeSerial;
  double x;
  double y;
  double z;
  double overallHealth;
  double blood;
  double maxBlood;
  double bleedRate;
  double firstAidNeed;
  double roboticAidNeed;
  int cats;
  int inventoryItemCount;
  int aiPathFailureCount;
  Stobe::Autonomy::OrderFingerprint order;
  std::string name;
  std::string aiCurrentGoal;
  std::vector<InventoryItemSnapshot> inventoryItems;
  std::vector<NearbyActorSnapshot> nearbyActors;
  std::vector<NearbyResourceSnapshot> nearbyResources;
  std::vector<NearbyWorkSnapshot> nearbyWork;

  CharacterSnapshot();
};

Character *ResolveCharacter(GameWorld *world, unsigned int serial);
Building *ResolveNearestRestBed(GameWorld *world, Character *character,
                                double maxDistance = 250.0);
CharacterSnapshot CaptureCharacter(GameWorld *world,
                                   unsigned int expectedSerial);

} // namespace KenshiAi
} // namespace Stobe
