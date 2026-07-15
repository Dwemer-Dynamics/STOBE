#pragma once

#include "AutonomyMonitor.h"

#include <string>
#include <vector>

class Character;
class GameWorld;

namespace Stobe {
namespace KenshiAi {

struct NearbyActorSnapshot {
  unsigned int runtimeSerial;
  double distance;
  bool playerCharacter;
  bool dead;
  bool unconscious;
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
  unsigned int carriedSerial;
  unsigned int runtimeSerial;
  double x;
  double y;
  double z;
  Stobe::Autonomy::OrderFingerprint order;
  std::string name;
  std::vector<NearbyActorSnapshot> nearbyActors;

  CharacterSnapshot();
};

Character *ResolveCharacter(GameWorld *world, unsigned int serial);
CharacterSnapshot CaptureCharacter(GameWorld *world,
                                   unsigned int expectedSerial);

} // namespace KenshiAi
} // namespace Stobe
