#include "PlayerBaseState.h"

#include "Comm.h"
#include "KenshiBuildingCompat.h"
#include "KenshiTownIdentity.h"
#include "KenshiTownCompat.h"
#include "Utils.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <kenshi/Character.h>
#include <kenshi/Enums.h>
#include <kenshi/Faction.h>
#include <kenshi/GameData.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/Inventory.h>
#include <kenshi/Item.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/SharedKing.h>
#include <kenshi/util/TimeOfDay.h>
#include <map>
#include <sstream>
#include <windows.h>

namespace Stobe {
namespace PlayerBase {

namespace {
Snapshot g_selectedSnapshot;
bool g_hasSelectedSnapshot = false;
DWORD g_lastCaptureTick = 0;
DWORD g_lastPostTick = 0;
std::string g_lastDigest;
std::string g_sessionId;
bool g_captureFaulted = false;
bool g_captureFaultLogged = false;
BaseDetails g_cachedDetails;
std::string g_cachedDetailsBaseId;
DWORD g_lastDetailsTick = 0;
bool g_detailsFaulted = false;
bool g_detailsFaultLogged = false;

const DWORD kDetailsRefreshMs = 5000;
const int kMaxBaseBuildings = 512;
const int kMaxBaseCharacters = 256;
const int kMaxSupplyItems = 2048;

bool IsUsablePointer(const void *value) {
  return value && reinterpret_cast<uintptr_t>(value) > 0x1000;
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool ContainsToken(const std::string &value, const char *token) {
  return value.find(token) != std::string::npos;
}

int AddClamped(int current, int amount) {
  if (amount <= 0 || current >= 1000000000) {
    return current;
  }
  return amount > 1000000000 - current ? 1000000000 : current + amount;
}

float SafeMetric(float value) {
  return _finite(value) && value > 0.0f ? value : 0.0f;
}

int ResolveGameTs(GameWorld *world) {
  if (!IsUsablePointer(world)) {
    return 0;
  }
  const int value =
      static_cast<int>(world->getTimeStamp_inGameHours().getTotalSeconds());
  return value > 0 ? value : 0;
}

bool IsPlayerOwnedTownAt(TownBase *candidate, const Ogre::Vector3 &position) {
  if (!IsUsablePointer(candidate) || candidate->getDataType() != TOWN ||
      !candidate->withinBordersRange(position, 1.0f)) {
    return false;
  }
  Faction *owner = candidate->getFaction();
  return IsUsablePointer(owner) && owner->isThePlayer();
}

TownBase *ResolvePlayerOwnedTown(Character *actor,
                                 const Ogre::Vector3 &position) {
  TownBase *candidate = nullptr;
  if (IsUsablePointer(shou) && IsUsablePointer(shou->townList)) {
    candidate =
        shou->townList->getNearestWithinItsRadius(position, false);
  }
  if (IsPlayerOwnedTownAt(candidate, position)) {
    return candidate;
  }

  candidate = actor->getCurrentTownLocation();
  return IsPlayerOwnedTownAt(candidate, position) ? candidate : nullptr;
}

std::string BuildStableBaseId(Town *town, const Ogre::Vector3 &townPosition) {
  char instanceUid[256] = {};
  int baseIndex = 0;
  int modIndex = 0;
  if (ReadTownInstanceIdentity(town, instanceUid, sizeof(instanceUid),
                               &baseIndex, &modIndex)) {
    return std::string(instanceUid) + ":" + ToString(baseIndex) + ":" +
           ToString(modIndex);
  }

  std::string sourceId = "player_town";
  GameData *townData = town->getOriginalGameData();
  if (IsUsablePointer(townData) && !townData->stringID.empty()) {
    sourceId = townData->stringID;
  }

  const int x = static_cast<int>(
      townPosition.x >= 0.0f ? townPosition.x + 0.5f : townPosition.x - 0.5f);
  const int z = static_cast<int>(
      townPosition.z >= 0.0f ? townPosition.z + 0.5f : townPosition.z - 0.5f);
  return sourceId + ":" + ToString(x) + ":" + ToString(z);
}

int CountPlayerMembersInside(GameWorld *world, TownBase *base) {
  if (!IsUsablePointer(world) || !IsUsablePointer(world->player) ||
      !IsUsablePointer(base)) {
    return 0;
  }

  int count = 0;
  const uint32_t total = world->player->playerCharacters.size();
  for (uint32_t i = 0; i < total; ++i) {
    Character *member = world->player->playerCharacters[i];
    if (!IsUsablePointer(member)) {
      continue;
    }
    const Ogre::Vector3 memberPosition = member->getPosition();
    if (base->withinBordersRange(memberPosition, 1.0f)) {
      ++count;
    }
  }
  return count;
}

const char *AlarmStateName(int state) {
  switch (state) {
  case 1:
    return "intruder";
  case 2:
    return "escape";
  case 3:
    return "attack";
  default:
    return "none";
  }
}

bool IsInfrastructure(BuildingClassType classType,
                      BuildingFunction functionType) {
  if (classType != BCTYPE_FLUFF && classType != BCTYPE_DOOR &&
      classType != BCTYPE_LIGHT) {
    return true;
  }
  return functionType != BF_ANY && functionType != BF_DOOR &&
         functionType != BF_LIGHT && functionType != BF_TABLE &&
         functionType != BF_CHAIR && functionType != BF_FLUFF;
}

bool HasSupplyInventory(BuildingClassType classType,
                        BuildingFunction functionType) {
  return classType == BCTYPE_STORAGE || classType == BCTYPE_PRODUCTION ||
         classType == BCTYPE_CRAFTING || classType == BCTYPE_ITEM_FURNACE ||
         classType == BCTYPE_FARM || functionType == BF_RESOURCE_STORAGE ||
         functionType == BF_GENERAL_STORAGE ||
         functionType == BF_RAIN_COLLECTOR ||
         functionType == BF_LIQUID_TANK || functionType == BF_GENERATOR;
}

void CaptureSupplyItem(Item *item, BaseDetails &details) {
  if (!IsUsablePointer(item)) {
    return;
  }

  const int quantity = item->quantity > 0 ? item->quantity : 1;
  const ItemFunction functionType = item->itemFunction;
  std::string identity = LowerAscii(item->getName());
  if (IsUsablePointer(item->data)) {
    identity += "|" + LowerAscii(item->data->stringID);
  }

  if (functionType == ITEM_FOOD || functionType == ITEM_FOOD_RESTRICTED) {
    details.food = AddClamped(details.food, quantity);
  }
  if (functionType == ITEM_FIRSTAID || functionType == ITEM_MEDRIGGING ||
      functionType == ITEM_ROBOTREPAIR) {
    details.medicine = AddClamped(details.medicine, quantity);
  }
  if (functionType == ITEM_AMMO) {
    details.ammunition = AddClamped(details.ammunition, quantity);
  }
  if (ContainsToken(identity, "building material")) {
    details.buildingMaterials =
        AddClamped(details.buildingMaterials, quantity);
  }
  if (ContainsToken(identity, "iron plate")) {
    details.ironPlates = AddClamped(details.ironPlates, quantity);
  }
  if (ContainsToken(identity, "fuel") || ContainsToken(identity, "biofuel")) {
    details.fuel = AddClamped(details.fuel, quantity);
  }
  if (ContainsToken(identity, "water")) {
    details.water = AddClamped(details.water, quantity);
  }
}

bool IsBuildingInsideBase(Building *building, TownBase *base) {
  if (!IsUsablePointer(building) || !IsUsablePointer(base)) {
    return false;
  }
  Faction *owner = building->getFaction();
  return IsUsablePointer(owner) && owner->isThePlayer() &&
         base->withinBordersRange(building->getPosition(), 1.05f);
}

void CaptureProductionStatus(Building *building, BuildingClassType classType,
                             bool destroyed, bool broken,
                             BaseDetails &details) {
  ProductionBuilding *production = building->_NV_getProductionBuilding();
  if (!IsUsablePointer(production)) {
    return;
  }

  UseableStuff *usable = production->_NV_getUseableStuff();
  const bool inputBlocked = production->_NV_isAnyInputsEmpty();
  const bool outputBlocked = production->_NV_isProductionFull();
  const bool unpowered =
      IsUsablePointer(usable) && usable->_NV_isOutOfPower() > 0.0f;
  const bool staffed =
      IsUsablePointer(usable) && usable->getOccupant().isValid();
  const bool idle =
      IsUsablePointer(usable) && usable->_NV_dontNeedWorkRightNow();
  const bool active = !destroyed && !broken && !inputBlocked &&
                      !outputBlocked && !unpowered && !idle;

  if (classType == BCTYPE_FARM) {
    ++details.farmTotal;
    details.farmActive += active ? 1 : 0;
    details.farmNeedsWater += inputBlocked ? 1 : 0;
    details.farmOutputFull += outputBlocked ? 1 : 0;
    details.farmUnpowered += unpowered ? 1 : 0;
    details.farmStaffed += staffed ? 1 : 0;
    return;
  }

  ++details.productionTotal;
  details.productionActive += active ? 1 : 0;
  details.productionInputBlocked += inputBlocked ? 1 : 0;
  details.productionOutputBlocked += outputBlocked ? 1 : 0;
  details.productionUnpowered += unpowered ? 1 : 0;
  details.productionStaffed += staffed ? 1 : 0;
}

// Performs the expensive loaded-base scan behind a separate SEH boundary.
bool CaptureDetailsUnsafe(GameWorld *world, Character *actor, Town *town,
                          BaseDetails &details) {
  details.Clear();
  if (!IsUsablePointer(world) || !IsUsablePointer(actor) ||
      !IsUsablePointer(town) || !IsUsablePointer(world->player)) {
    return false;
  }

  int alarmState = 0;
  float radius = 0.0f;
  ReadTownRuntimeStatus(town, &alarmState, &radius);
  details.alarmState = AlarmStateName(alarmState);
  radius = SafeMetric(radius);
  if (radius < 250.0f) {
    radius = 650.0f;
  } else if (radius > 3000.0f) {
    radius = 3000.0f;
  }
  const Ogre::Vector3 center = town->getPosition();

  lektor<RootObject *> characters;
  world->getObjectsWithinSphere(characters, center, radius * 1.05f, CHARACTER,
                                kMaxBaseCharacters, (RootObject *)actor);
  details.scanTruncated =
      characters.size() >= static_cast<uint32_t>(kMaxBaseCharacters);
  for (uint32_t i = 0; i < characters.size(); ++i) {
    Character *candidate = static_cast<Character *>(characters[i]);
    if (!IsUsablePointer(candidate) ||
        !town->withinBordersRange(candidate->getPosition(), 1.05f) ||
        candidate->isDead()) {
      continue;
    }
    if (world->player->isEnemy(candidate)) {
      ++details.hostilesInside;
    }
  }

  lektor<RootObject *> buildings;
  world->getObjectsWithinSphere(buildings, center, radius * 1.05f, BUILDING,
                                kMaxBaseBuildings, (RootObject *)actor);
  if (buildings.size() >= static_cast<uint32_t>(kMaxBaseBuildings)) {
    details.scanTruncated = true;
  }

  std::map<unsigned int, bool> seen;
  int supplyItemsScanned = 0;
  for (uint32_t i = 0; i < buildings.size(); ++i) {
    Building *building = static_cast<Building *>(buildings[i]);
    if (!IsBuildingInsideBase(building, town)) {
      continue;
    }

    const unsigned int serial = building->getHandle().serial;
    if (serial != 0 && seen.count(serial) > 0) {
      continue;
    }
    if (serial != 0) {
      seen[serial] = true;
    }

    const BuildingClassType classType = building->_NV_getBuildingClass();
    const BuildingFunction functionType = building->_NV_getSpecialFunction();
    const bool destroyed = building->_NV_isDestroyed();
    const bool broken = building->_NV_isBroken();
    const bool damaged = building->_NV_isDamaged();
    UseableStuff *usable = building->_NV_getUseableStuff();
    const bool unpowered =
        IsUsablePointer(usable) && usable->_NV_isOutOfPower() > 0.0f;

    const bool infrastructure = IsInfrastructure(classType, functionType);
    if (infrastructure) {
      ++details.infrastructureTotal;
      details.damagedBuildings += damaged ? 1 : 0;
      details.destroyedBuildings += destroyed ? 1 : 0;
      details.brokenBuildings += broken ? 1 : 0;
      details.unpoweredBuildings += unpowered ? 1 : 0;
    }
    details.storageBuildings +=
        (classType == BCTYPE_STORAGE ||
         functionType == BF_RESOURCE_STORAGE ||
         functionType == BF_GENERAL_STORAGE)
            ? 1
            : 0;
    details.productionBuildings +=
        (classType == BCTYPE_PRODUCTION || classType == BCTYPE_CRAFTING ||
         classType == BCTYPE_ITEM_FURNACE)
            ? 1
            : 0;
    details.farms += classType == BCTYPE_FARM ? 1 : 0;
    details.researchBenches += classType == BCTYPE_RESEARCH ? 1 : 0;
    details.generators += functionType == BF_GENERATOR ? 1 : 0;
    details.batteries += functionType == BF_BATTERY ? 1 : 0;
    details.beds +=
        (functionType == BF_BED || functionType == BF_SKELETON_BED) ? 1 : 0;
    details.cages += functionType == BF_CAGE ? 1 : 0;

    const bool isGate =
        classType == BCTYPE_GATEWAY || functionType == BF_GATE;
    const bool isWall = classType == BCTYPE_WALL || functionType == BF_WALL;
    if (isGate) {
      ++details.gatesTotal;
    }
    if ((isGate || isWall) && damaged) {
      ++details.damagedDefenses;
    }
    if ((isGate || isWall) && destroyed) {
      ++details.destroyedDefenses;
    }

    if (classType == BCTYPE_TURRET || functionType == BF_TURRET) {
      ++details.turretsTotal;
      details.turretsUnpowered += unpowered ? 1 : 0;
      details.turretsManned +=
          IsUsablePointer(usable) && usable->getOccupant().isValid() ? 1 : 0;
    }

    CaptureProductionStatus(building, classType, destroyed, broken, details);

    if (!HasSupplyInventory(classType, functionType) ||
        supplyItemsScanned >= kMaxSupplyItems) {
      continue;
    }
    Inventory *inventory = building->getInventory();
    if (!IsUsablePointer(inventory)) {
      continue;
    }
    const lektor<Item *> &items = inventory->getAllItems();
    for (uint32_t itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
      if (supplyItemsScanned >= kMaxSupplyItems) {
        details.scanTruncated = true;
        break;
      }
      CaptureSupplyItem(items[itemIndex], details);
      ++supplyItemsScanned;
    }
  }

  details.available = true;
  return true;
}

bool CaptureDetailsCandidateSeh(GameWorld *world, Character *actor, Town *town,
                                BaseDetails *candidate) {
  bool sampled = false;
  __try {
    sampled = CaptureDetailsUnsafe(world, actor, town, *candidate);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    sampled = false;
    g_detailsFaulted = true;
  }
  return sampled;
}

void CaptureDetailsCached(GameWorld *world, Character *actor, Town *town,
                          const std::string &baseId, BaseDetails &out) {
  out.Clear();
  if (g_detailsFaulted) {
    if (!g_detailsFaultLogged) {
      g_detailsFaultLogged = true;
      Log("PLAYER_BASE: detail scan faulted; detailed metrics disabled until "
          "the next world load.");
    }
    return;
  }

  const DWORD now = GetTickCount();
  if (baseId == g_cachedDetailsBaseId && g_lastDetailsTick != 0 &&
      now - g_lastDetailsTick < kDetailsRefreshMs) {
    out = g_cachedDetails;
    return;
  }

  BaseDetails *candidate = new BaseDetails();
  if (CaptureDetailsCandidateSeh(world, actor, town, candidate)) {
    g_cachedDetails = *candidate;
    g_cachedDetailsBaseId = baseId;
    g_lastDetailsTick = now;
    out = *candidate;
    delete candidate;
  } else if (!g_detailsFaulted) {
    delete candidate;
  }
  // A candidate interrupted by an access violation is intentionally abandoned.
}

bool CaptureUnsafe(GameWorld *world, Character *actor, Snapshot &out) {
  out.Clear();
  if (!IsUsablePointer(world) || !IsUsablePointer(actor)) {
    return true;
  }

  out.gameTs = ResolveGameTs(world);
  out.observerSerial = actor->getHandle().serial;
  out.observerName = actor->getName();

  const Ogre::Vector3 actorPosition = actor->getPosition();
  TownBase *base = ResolvePlayerOwnedTown(actor, actorPosition);
  if (!base) {
    return true;
  }

  Town *town = static_cast<Town *>(base);
  const Ogre::Vector3 townPosition = town->getPosition();
  out.inside = true;
  out.baseId = BuildStableBaseId(town, townPosition);
  out.name = town->getKnownName();
  if (out.name.empty()) {
    out.name = town->getName();
  }
  if (out.name.empty()) {
    out.name = "Player Base";
  }
  out.powerGenerated = SafeMetric(town->getTotalPower());
  out.powerRequired = SafeMetric(town->getRequiredPower());
  out.batteryCharge = SafeMetric(town->getBatteryCharge());
  out.batteryCapacity = SafeMetric(town->getBatteryChargeMax());
  out.batteryDrain = SafeMetric(town->getBatteryDrain());
  out.batteryCharging = SafeMetric(town->getBatteryChargingUpAmount());
  out.batteryMode = town->isBatteryMode();
  out.hasSparePower = town->hasSparePower();
  out.membersInside = CountPlayerMembersInside(world, base);
  out.hasGates = town->_NV_hasGates();
  out.gatesClosed = out.hasGates && town->_NV_gatesAllClosed();
  CaptureDetailsCached(world, actor, town, out.baseId, out.details);
  return true;
}

bool CaptureCandidateSeh(GameWorld *world, Character *actor,
                         Snapshot *candidate) {
  bool sampled = false;
  __try {
    sampled = CaptureUnsafe(world, actor, *candidate);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    sampled = false;
    g_captureFaulted = true;
  }
  return sampled;
}

bool CaptureSeh(GameWorld *world, Character *actor, Snapshot *out) {
  if (g_captureFaulted) {
    return false;
  }

  Snapshot *candidate = new Snapshot();
  const bool sampled = CaptureCandidateSeh(world, actor, candidate);
  if (sampled) {
    *out = *candidate;
    delete candidate;
  }
  // A candidate interrupted by an access violation is intentionally abandoned.
  return sampled;
}

Character *ResolveObservedPlayer(GameWorld *world, Character *selected) {
  if (IsUsablePointer(selected)) {
    Faction *faction = selected->getFaction();
    if (IsUsablePointer(faction) && faction->isThePlayer()) {
      return selected;
    }
  }
  if (!IsUsablePointer(world) || !IsUsablePointer(world->player) ||
      world->player->playerCharacters.size() == 0) {
    return nullptr;
  }
  Character *primary = world->player->playerCharacters[0];
  return IsUsablePointer(primary) ? primary : nullptr;
}

Character *ResolveObservedPlayerSeh(GameWorld *world, Character *selected) {
  Character *observer = nullptr;
  __try {
    observer = ResolveObservedPlayer(world, selected);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    observer = nullptr;
  }
  return observer;
}

std::string EnsureSessionId() {
  if (g_sessionId.empty()) {
    g_sessionId = ToString(static_cast<int>(GetCurrentProcessId())) + "-" +
                  ToString(static_cast<int>(GetTickCount()));
  }
  return g_sessionId;
}

std::string BuildDigest(const Snapshot &snapshot) {
  std::ostringstream stream;
  stream << (snapshot.inside ? "1" : "0") << "|" << snapshot.baseId << "|"
         << snapshot.observerSerial << "|" << snapshot.powerGenerated << "|"
         << snapshot.powerRequired << "|" << snapshot.batteryCharge << "|"
         << snapshot.batteryCapacity << "|" << snapshot.batteryDrain << "|"
         << snapshot.batteryCharging << "|" << (snapshot.batteryMode ? 1 : 0)
         << "|" << (snapshot.hasSparePower ? 1 : 0) << "|"
         << snapshot.membersInside << "|" << (snapshot.hasGates ? 1 : 0)
         << "|" << (snapshot.gatesClosed ? 1 : 0) << "|"
         << (snapshot.details.available ? 1 : 0) << "|"
         << snapshot.details.alarmState << "|"
         << snapshot.details.hostilesInside << "|"
         << snapshot.details.infrastructureTotal << "|"
         << snapshot.details.damagedBuildings << "|"
         << snapshot.details.food << "|" << snapshot.details.medicine << "|"
         << snapshot.details.productionActive << "|"
         << snapshot.details.productionInputBlocked << "|"
         << snapshot.details.productionOutputBlocked << "|"
         << snapshot.details.farmActive << "|"
         << snapshot.details.farmNeedsWater << "|"
         << (snapshot.details.scanTruncated ? 1 : 0);
  return stream.str();
}

std::string BuildPresenceJson(const Snapshot &snapshot) {
  return "{\"session_id\":\"" + EscapeJSON(EnsureSessionId()) +
         "\",\"observer_serial\":" +
         ToString(static_cast<int>(snapshot.observerSerial)) +
         ",\"observer_name\":\"" + EscapeJSON(snapshot.observerName) +
         "\",\"game_ts\":" + ToString(snapshot.gameTs) +
         ",\"player_base\":" + BuildJson(snapshot) + "}";
}
} // namespace

BaseDetails::BaseDetails() { Clear(); }

void BaseDetails::Clear() {
  available = false;
  scanTruncated = false;
  alarmState = "none";
  hostilesInside = 0;
  gatesTotal = 0;
  damagedDefenses = 0;
  destroyedDefenses = 0;
  turretsTotal = 0;
  turretsManned = 0;
  turretsUnpowered = 0;
  infrastructureTotal = 0;
  storageBuildings = 0;
  productionBuildings = 0;
  farms = 0;
  researchBenches = 0;
  generators = 0;
  batteries = 0;
  beds = 0;
  cages = 0;
  damagedBuildings = 0;
  destroyedBuildings = 0;
  brokenBuildings = 0;
  unpoweredBuildings = 0;
  food = 0;
  medicine = 0;
  buildingMaterials = 0;
  ironPlates = 0;
  fuel = 0;
  water = 0;
  ammunition = 0;
  productionTotal = 0;
  productionActive = 0;
  productionInputBlocked = 0;
  productionOutputBlocked = 0;
  productionUnpowered = 0;
  productionStaffed = 0;
  farmTotal = 0;
  farmActive = 0;
  farmNeedsWater = 0;
  farmOutputFull = 0;
  farmUnpowered = 0;
  farmStaffed = 0;
}

Snapshot::Snapshot() { Clear(); }

void Snapshot::Clear() {
  inside = false;
  baseId.clear();
  name.clear();
  observerSerial = 0;
  observerName.clear();
  powerGenerated = 0.0f;
  powerRequired = 0.0f;
  batteryCharge = 0.0f;
  batteryCapacity = 0.0f;
  batteryDrain = 0.0f;
  batteryCharging = 0.0f;
  batteryMode = false;
  hasSparePower = false;
  membersInside = 0;
  hasGates = false;
  gatesClosed = false;
  gameTs = 0;
  details.Clear();
}

bool Capture(GameWorld *world, Character *actor, Snapshot &out) {
  out.Clear();
  const bool sampled = CaptureSeh(world, actor, &out);
  if (!sampled && g_captureFaulted && !g_captureFaultLogged) {
    g_captureFaultLogged = true;
    Log("PLAYER_BASE: capture faulted; disabled until the next world load.");
  }
  return sampled && out.inside;
}

std::string BuildJson(const Snapshot &snapshot) {
  if (!snapshot.inside) {
    return "{\"inside\":false,\"observed_game_ts\":" +
           ToString(snapshot.gameTs) + "}";
  }

  std::ostringstream json;
  json << "{\"inside\":true,\"base_id\":\""
       << EscapeJSON(snapshot.baseId) << "\",\"name\":\""
       << EscapeJSON(snapshot.name) << "\",\"power_generated\":"
       << snapshot.powerGenerated << ",\"power_required\":"
       << snapshot.powerRequired << ",\"battery_charge\":"
       << snapshot.batteryCharge << ",\"battery_capacity\":"
       << snapshot.batteryCapacity << ",\"battery_drain\":"
       << snapshot.batteryDrain << ",\"battery_charging\":"
       << snapshot.batteryCharging << ",\"battery_mode\":"
       << (snapshot.batteryMode ? "true" : "false")
       << ",\"has_spare_power\":"
       << (snapshot.hasSparePower ? "true" : "false")
       << ",\"members_inside\":" << snapshot.membersInside
       << ",\"has_gates\":" << (snapshot.hasGates ? "true" : "false")
       << ",\"gates_closed\":"
       << (snapshot.gatesClosed ? "true" : "false")
       << ",\"details\":{\"available\":"
       << (snapshot.details.available ? "true" : "false")
       << ",\"scan_truncated\":"
       << (snapshot.details.scanTruncated ? "true" : "false")
       << ",\"security\":{\"alarm_state\":\""
       << EscapeJSON(snapshot.details.alarmState)
       << "\",\"hostiles_inside\":" << snapshot.details.hostilesInside
       << ",\"gates_total\":" << snapshot.details.gatesTotal
       << ",\"damaged_defenses\":" << snapshot.details.damagedDefenses
       << ",\"destroyed_defenses\":" << snapshot.details.destroyedDefenses
       << ",\"turrets_total\":" << snapshot.details.turretsTotal
       << ",\"turrets_manned\":" << snapshot.details.turretsManned
       << ",\"turrets_unpowered\":" << snapshot.details.turretsUnpowered
       << "},\"infrastructure\":{\"total\":"
       << snapshot.details.infrastructureTotal
       << ",\"storage\":" << snapshot.details.storageBuildings
       << ",\"production\":" << snapshot.details.productionBuildings
       << ",\"farms\":" << snapshot.details.farms
       << ",\"research\":" << snapshot.details.researchBenches
       << ",\"generators\":" << snapshot.details.generators
       << ",\"batteries\":" << snapshot.details.batteries
       << ",\"beds\":" << snapshot.details.beds
       << ",\"cages\":" << snapshot.details.cages
       << ",\"damaged\":" << snapshot.details.damagedBuildings
       << ",\"destroyed\":" << snapshot.details.destroyedBuildings
       << ",\"broken\":" << snapshot.details.brokenBuildings
       << ",\"unpowered\":" << snapshot.details.unpoweredBuildings
       << "},\"supplies\":{\"food\":" << snapshot.details.food
       << ",\"medicine\":" << snapshot.details.medicine
       << ",\"building_materials\":" << snapshot.details.buildingMaterials
       << ",\"iron_plates\":" << snapshot.details.ironPlates
       << ",\"fuel\":" << snapshot.details.fuel
       << ",\"water\":" << snapshot.details.water
       << ",\"ammunition\":" << snapshot.details.ammunition
       << "},\"production\":{\"total\":" << snapshot.details.productionTotal
       << ",\"active\":" << snapshot.details.productionActive
       << ",\"input_blocked\":" << snapshot.details.productionInputBlocked
       << ",\"output_blocked\":" << snapshot.details.productionOutputBlocked
       << ",\"unpowered\":" << snapshot.details.productionUnpowered
       << ",\"staffed\":" << snapshot.details.productionStaffed
       << "},\"farms\":{\"total\":" << snapshot.details.farmTotal
       << ",\"active\":" << snapshot.details.farmActive
       << ",\"needs_water\":" << snapshot.details.farmNeedsWater
       << ",\"output_full\":" << snapshot.details.farmOutputFull
       << ",\"unpowered\":" << snapshot.details.farmUnpowered
       << ",\"staffed\":" << snapshot.details.farmStaffed
       << "}},\"observed_game_ts\":" << snapshot.gameTs << "}";
  return json.str();
}

void Update(GameWorld *world, Character *selected) {
  const DWORD now = GetTickCount();
  if (g_lastCaptureTick != 0 && now - g_lastCaptureTick < 1000) {
    return;
  }
  g_lastCaptureTick = now;

  Snapshot current;
  Character *observer = ResolveObservedPlayerSeh(world, selected);
  Capture(world, observer, current);
  g_selectedSnapshot = current;
  g_hasSelectedSnapshot = true;

  const std::string digest = BuildDigest(current);
  const bool changed = digest != g_lastDigest;
  if (!changed && g_lastPostTick != 0 && now - g_lastPostTick < 10000) {
    return;
  }

  AsyncPostToStobeSerial(L"/player_base_state", BuildPresenceJson(current));
  g_lastDigest = digest;
  g_lastPostTick = now;
}

bool GetSelectedSnapshot(Snapshot &out) {
  if (!g_hasSelectedSnapshot) {
    return false;
  }
  out = g_selectedSnapshot;
  return true;
}

void Reset() {
  g_selectedSnapshot.Clear();
  g_hasSelectedSnapshot = false;
  g_lastCaptureTick = 0;
  g_lastPostTick = 0;
  g_lastDigest.clear();
  g_sessionId.clear();
  g_captureFaulted = false;
  g_captureFaultLogged = false;
  g_cachedDetails.Clear();
  g_cachedDetailsBaseId.clear();
  g_lastDetailsTick = 0;
  g_detailsFaulted = false;
  g_detailsFaultLogged = false;
}

} // namespace PlayerBase
} // namespace Stobe
