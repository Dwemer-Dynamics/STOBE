#pragma once

#include "AutonomyMonitor.h"

#include <string>
#include <vector>

class Character;
class GameWorld;
class Building;

namespace Stobe {
namespace KenshiAi {

struct NearbyActorSnapshot {
  unsigned int runtimeSerial;
  double distance;
  bool playerCharacter;
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
  std::string name;

  NearbyActorSnapshot();
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
  unsigned int carriedSerial;
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
  Stobe::Autonomy::OrderFingerprint order;
  std::string name;
  std::vector<NearbyActorSnapshot> nearbyActors;

  CharacterSnapshot();
};

Character *ResolveCharacter(GameWorld *world, unsigned int serial);
Building *ResolveNearestRestBed(GameWorld *world, Character *character,
                                double maxDistance = 250.0);
CharacterSnapshot CaptureCharacter(GameWorld *world,
                                   unsigned int expectedSerial);

} // namespace KenshiAi
} // namespace Stobe
