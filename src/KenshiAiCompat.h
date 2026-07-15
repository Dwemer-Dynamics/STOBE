#pragma once

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
  unsigned int runtimeSerial;
  std::string name;

  CharacterSnapshot();
};

CharacterSnapshot CaptureCharacter(GameWorld *world,
                                   unsigned int expectedSerial);

} // namespace KenshiAi
} // namespace Stobe
