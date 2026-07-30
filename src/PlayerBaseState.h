#pragma once

#include <string>

class Character;
class GameWorld;

namespace Stobe {
namespace PlayerBase {

struct Snapshot {
  bool inside;
  std::string baseId;
  std::string name;
  unsigned int observerSerial;
  std::string observerName;
  float powerGenerated;
  float powerRequired;
  float batteryCharge;
  float batteryCapacity;
  float batteryDrain;
  float batteryCharging;
  bool batteryMode;
  bool hasSparePower;
  int membersInside;
  bool hasGates;
  bool gatesClosed;
  int gameTs;

  Snapshot();
  void Clear();
};

// Captures one actor's current player-owned base without retaining engine pointers.
bool Capture(GameWorld *world, Character *actor, Snapshot &out);
std::string BuildJson(const Snapshot &snapshot);

// Publishes selected-player presence and maintains the status HUD snapshot.
void Update(GameWorld *world, Character *selected);
bool GetSelectedSnapshot(Snapshot &out);
void Reset();

} // namespace PlayerBase
} // namespace Stobe
