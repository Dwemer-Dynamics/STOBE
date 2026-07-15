#pragma once

#include "AutonomyMonitor.h"

#include <string>

class Character;
class GameWorld;

namespace Stobe {
namespace KenshiAi {

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
  unsigned int runtimeSerial;
  double x;
  double y;
  double z;
  Stobe::Autonomy::OrderFingerprint order;
  std::string name;

  CharacterSnapshot();
};

Character *ResolveCharacter(GameWorld *world, unsigned int serial);
CharacterSnapshot CaptureCharacter(GameWorld *world,
                                   unsigned int expectedSerial);

} // namespace KenshiAi
} // namespace Stobe
