#pragma once

#include <string>
#include <vector>

class Character;
class GameWorld;

namespace Stobe {
namespace PlayerBase {

struct InfrastructureIssueGroup {
  std::string name;
  int count;
  int damaged;
  int destroyed;
  int broken;
  int unpowered;

  InfrastructureIssueGroup();
};

struct ConstructionGroup {
  std::string name;
  int count;
  int paused;
  int missingMaterials;
  float progressTotal;

  ConstructionGroup();
};

struct ProductionGroup {
  std::string name;
  int total;
  int active;
  int inputBlocked;
  int outputBlocked;
  int unpowered;
  int staffed;
  float efficiencyTotal;

  ProductionGroup();
};

struct FarmGroup {
  std::string name;
  int total;
  int active;
  int needsWater;
  int outputFull;
  int unpowered;
  int staffed;
  int hydroponic;
  float yieldTotal;

  FarmGroup();
};

struct StorageGroup {
  std::string name;
  int total;
  int empty;
  int full;
  int itemUnits;

  StorageGroup();
};

struct BaseDetails {
  bool available;
  bool scanTruncated;
  std::string alarmState;
  int hostilesInside;
  int gatesTotal;
  int damagedDefenses;
  int destroyedDefenses;
  int turretsTotal;
  int turretsManned;
  int turretsUnpowered;

  int damagedBuildings;
  int destroyedBuildings;
  int brokenBuildings;
  int unpoweredBuildings;
  std::vector<InfrastructureIssueGroup> infrastructureIssues;

  int constructionTotal;
  int constructionPaused;
  int constructionMissingMaterials;
  float constructionProgressTotal;
  std::vector<ConstructionGroup> constructionGroups;

  int powerConsumers;
  int powerUnpowered;
  int powerSwitchedOff;
  int generatorsTotal;
  int generatorsActive;

  int food;
  int medicine;
  int buildingMaterials;
  int ironPlates;
  int fuel;
  int water;
  int ammunition;

  int storageTotal;
  int storageEmpty;
  int storageFull;
  int storageItemUnits;
  std::vector<StorageGroup> storageGroups;

  int productionTotal;
  int productionActive;
  int productionInputBlocked;
  int productionOutputBlocked;
  int productionUnpowered;
  int productionStaffed;
  float productionEfficiencyTotal;
  std::vector<ProductionGroup> productionGroups;

  int farmTotal;
  int farmActive;
  int farmNeedsWater;
  int farmOutputFull;
  int farmUnpowered;
  int farmStaffed;
  int farmHydroponic;
  float farmYieldTotal;
  std::vector<FarmGroup> farmGroups;

  BaseDetails();
  void Clear();
};

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
  BaseDetails details;

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
