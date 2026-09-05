#include "Context.h"
#include "Functions.h"
#include "Globals.h"
#include "KenshiBuildingCompat.h"
#include "PlayerBaseState.h"
#include "KenshiWeatherCompat.h"
#include "Utils.h"
#include <kenshi/CharStats.h>
#include <kenshi/CharMovement.h>
#include <kenshi/Character.h>
#include <kenshi/Appearance.h>
#include <kenshi/Faction.h>
#include <kenshi/GameData.h>
#include <kenshi/GameWorld.h>
#include <kenshi/InstanceID.h>
#include <kenshi/Inventory.h>
#include <kenshi/Item.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/Platoon.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RaceData.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/Enums.h>
#include <kenshi/util/hand.h>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

static std::string GetVisibleEquipment(Character *npc);

std::string SlotToString(AttachSlot slot) {
  switch (slot) {
  case ATTACH_WEAPON:
    return "weapon";
  case ATTACH_BACK:
    return "back";
  case ATTACH_HAIR:
    return "hair";
  case ATTACH_HAT:
    return "hat";
  case ATTACH_EYES:
    return "eyes";
  case ATTACH_BODY:
    return "body";
  case ATTACH_LEGS:
    return "legs";
  case ATTACH_SHIRT:
    return "shirt";
  case ATTACH_BOOTS:
    return "boots";
  case ATTACH_GLOVES:
    return "gloves";
  case ATTACH_NECK:
    return "neck";
  case ATTACH_BACKPACK:
    return "backpack";
  case ATTACH_BEARD:
    return "beard";
  case ATTACH_BELT:
    return "belt";
  case ATTACH_LEFT_ARM:
    return "left_arm";
  case ATTACH_RIGHT_ARM:
    return "right_arm";
  case ATTACH_LEFT_LEG:
    return "left_leg";
  case ATTACH_RIGHT_LEG:
    return "right_leg";
  default:
    return "none";
  }
}

namespace {
const char *WeatherEffectTypeName(EffectType::Enum type) {
  switch (type) {
  case EffectType::CAMERA:
    return "camera";
  case EffectType::POINT:
    return "point";
  case EffectType::WANDERING:
    return "wandering";
  case EffectType::GLOBAL:
    return "global";
  case EffectType::CAMERA_RAIN:
    return "rain";
  case EffectType::CAMERA_ACID_RAIN:
    return "acid_rain";
  case EffectType::POINT_LIGHTING:
    return "lighting";
  case EffectType::WANDERING_STORM:
    return "storm";
  case EffectType::WANDERING_GAS:
    return "gas";
  case EffectType::GLOBAL_POINT:
    return "global_point";
  default:
    return "none";
  }
}

std::string BuildNamedWeatherContext(Character *npc,
                                     const Ogre::Vector3 &position) {
  std::string weatherName;
  float weatherStrength = 0.0f;
  float windSpeed = 0.0f;
  Ogre::Vector3 windDirection = Ogre::Vector3::ZERO;
  float wetness = 0.0f;
  std::string effectsJson = "[";
  size_t effectCount = 0;

  try {
    WeatherSystem *weatherSystem = WeatherSystem::getInstance();
    if (weatherSystem && (uintptr_t)weatherSystem > 0x1000) {
      WeatherRegion *region = weatherSystem->ActiveRegionWeather;
      WeatherInstance *instance =
          region && (uintptr_t)region > 0x1000
              ? region->getWeatherInstance()
              : nullptr;
      if (instance && (uintptr_t)instance > 0x1000) {
        Weather *weather = instance->getWeather();
        if (weather && (uintptr_t)weather > 0x1000) {
          weatherName = weather->getName();
        }
        weatherStrength = instance->getWeatherStrength();
        windSpeed = instance->getWindSpeed();
        windDirection = instance->getWindDirection();
        wetness = instance->getWetness();
      }

      const auto &effects = weatherSystem->getPositionGlobalEffects(position);
      for (size_t i = 0; i < effects.size() && effectCount < 16; ++i) {
        if (effects[i].second <= 0.0f) {
          continue;
        }
        if (effectCount > 0) {
          effectsJson += ",";
        }
        effectsJson += "{\"type\":\"";
        effectsJson += WeatherEffectTypeName(effects[i].first);
        effectsJson += "\",\"type_id\":" +
                       ToString(static_cast<int>(effects[i].first)) + ",";
        effectsJson += "\"strength\":" + ToString(effects[i].second) + "}";
        ++effectCount;
      }
    }
  } catch (...) {
    weatherName.clear();
    weatherStrength = 0.0f;
    windSpeed = 0.0f;
    windDirection = Ogre::Vector3::ZERO;
    wetness = 0.0f;
    effectsJson = "[";
  }
  effectsJson += "]";

  float affectStrength = 0.0f;
  try {
    affectStrength = npc ? npc->getCurrentWeatherAffectStrength() : 0.0f;
  } catch (...) {
    affectStrength = 0.0f;
  }

  std::string fields;
  fields += "\"weather_name\":\"" + EscapeJSON(weatherName) + "\",";
  fields += "\"weather_strength\":" + ToString(weatherStrength) + ",";
  fields += "\"weather_affect_strength\":" + ToString(affectStrength) + ",";
  fields += "\"wind_speed\":" + ToString(windSpeed) + ",";
  fields += "\"wind_direction\":{";
  fields += "\"x\":" + ToString(windDirection.x) + ",";
  fields += "\"y\":" + ToString(windDirection.y) + ",";
  fields += "\"z\":" + ToString(windDirection.z) + "},";
  fields += "\"wetness\":" + ToString(wetness) + ",";
  fields += "\"active_environmental_effects\":" + effectsJson;
  return fields;
}

bool IsIndoorsHandleValid(const hand &indoorsHandle) {
  return indoorsHandle.isValid() && !indoorsHandle.isNull();
}

std::string GetIndoorBuildingName(const hand &indoorsHandle) {
  if (!IsIndoorsHandleValid(indoorsHandle)) {
    return "";
  }
  RootObjectBase *indoorsObject = indoorsHandle.getRootObjectBase();
  if (!indoorsObject || (uintptr_t)indoorsObject <= 0x1000) {
    return "";
  }
  return indoorsObject->getName();
}

std::string TrimCopy(const std::string &value) {
  size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string ResolveCharacterDisplayNameSafe(Character *character) {
  if (!character || (uintptr_t)character <= 0x1000) {
    return "";
  }
  std::string name = "";
  try {
    name = character->getName();
  } catch (...) {
    name.clear();
  }
  name = TrimCopy(name);
  if (name.empty()) {
    try {
      if (!character->displayName.empty()) {
        name = character->displayName;
      }
    } catch (...) {
      name.clear();
    }
  }
  name = TrimCopy(name);
  if (name.empty()) {
    try {
      if (character->data && !character->data->name.empty()) {
        name = character->data->name;
      }
    } catch (...) {
      name.clear();
    }
  }
  return TrimCopy(name);
}

std::string ResolveCarryTargetNameSafe(const hand &targetHandle) {
  if (!targetHandle.isValid()) {
    return "";
  }
  try {
    Character *targetCharacter = targetHandle.getCharacter();
    if (targetCharacter && (uintptr_t)targetCharacter > 0x1000) {
      std::string name = ResolveCharacterDisplayNameSafe(targetCharacter);
      if (!name.empty()) {
        return name;
      }
    }
  } catch (...) {
  }
  try {
    RootObjectBase *targetBase = targetHandle.getRootObjectBase();
    if (targetBase && (uintptr_t)targetBase > 0x1000) {
      std::string name = TrimCopy(targetBase->getName());
      if (!name.empty()) {
        return name;
      }
    }
  } catch (...) {
  }
  return "";
}

void BuildCarriedByNameLookup(
    GameWorld *world,
    std::map<unsigned int, std::string> &carriedByNameOut) {
  carriedByNameOut.clear();
  if (!world || (uintptr_t)world <= 0x1000) {
    return;
  }
  try {
    const ogre_unordered_set<Character *>::type &chars =
        world->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it) {
      Character *carrier = *it;
      if (!carrier || (uintptr_t)carrier <= 0x1000) {
        continue;
      }
      bool isCarrying = false;
      hand carriedHandle;
      try {
        isCarrying = carrier->isCarryingSomething && carrier->carryingObject.isValid();
        if (isCarrying) {
          carriedHandle = carrier->carryingObject;
        }
      } catch (...) {
        isCarrying = false;
      }
      if (!isCarrying || !carriedHandle.isValid() || carriedHandle.serial == 0) {
        continue;
      }
      if (carriedByNameOut.count(carriedHandle.serial) > 0) {
        continue;
      }
      std::string carrierName = ResolveCharacterDisplayNameSafe(carrier);
      if (carrierName.empty()) {
        carrierName = "someone";
      }
      carriedByNameOut[carriedHandle.serial] = carrierName;
    }
  } catch (...) {
  }
}

int ClampIntRange(int value, int minValue, int maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

std::string ResolveArmourQualityLabelFromLevel(int level) {
  if (level >= 95) {
    return "Masterwork";
  }
  if (level >= 80) {
    return "Specialist";
  }
  if (level >= 60) {
    return "High";
  }
  if (level >= 40) {
    return "Standard";
  }
  if (level >= 20) {
    return "Shoddy";
  }
  return "Prototype";
}

bool TryExtractArmourQuality(Item *item, int &levelOut, std::string &labelOut) {
  levelOut = -1;
  labelOut.clear();
  if (!item || (uintptr_t)item <= 0x1000) {
    return false;
  }

  bool isArmour = false;
  try {
    isArmour = (item->isArmour() != NULL) || (item->isLockedArmour() != NULL);
  } catch (...) {
    isArmour = false;
  }
  if (!isArmour) {
    return false;
  }

  int level = -1;
  try {
    level = item->getLevel();
  } catch (...) {
    level = -1;
  }
  if (level < 0) {
    return false;
  }

  level = ClampIntRange(level, 0, 100);
  levelOut = level;
  labelOut = ResolveArmourQualityLabelFromLevel(level);
  return !labelOut.empty();
}

bool IsUnknownToken(const std::string &value) {
  std::string normalized = TrimCopy(value);
  if (normalized.empty()) {
    return true;
  }
  for (size_t i = 0; i < normalized.size(); ++i) {
    normalized[i] = (char)std::tolower((unsigned char)normalized[i]);
  }
  return normalized == "unknown" || normalized == "none" ||
         normalized == "null" || normalized == "n/a";
}

struct TraderInventoryCacheEntry {
  std::string inventoryJson;
  int itemCount;
  DWORD capturedTick;
};

struct SimpleTraderInventoryItem {
  std::string name;
  std::string itemId;
  std::string quality;
  int qualityLevel;
  std::string weaponModel;
  int count;
  int valueEach;
};

static std::map<unsigned int, TraderInventoryCacheEntry>
    s_traderInventoryCacheBySerial;

std::string UseObjectClassLabel(BuildingClassType classType) {
  switch (classType) {
  case BCTYPE_USABLE:
    return "usable";
  case BCTYPE_TURRET:
    return "turret";
  case BCTYPE_PRODUCTION:
    return "production";
  case BCTYPE_CRAFTING:
    return "crafting";
  case BCTYPE_RESEARCH:
    return "research";
  case BCTYPE_STORAGE:
    return "storage";
  case BCTYPE_FARM:
    return "farm";
  case BCTYPE_LIGHT:
    return "light";
  default:
    return "other";
  }
}

std::string UseObjectFunctionLabel(BuildingFunction functionType) {
  switch (functionType) {
  case BF_CHAIR:
    return "chair";
  case BF_TURRET:
    return "turret";
  case BF_BED:
    return "bed";
  case BF_SKELETON_BED:
    return "skeleton_bed";
  case BF_THRONE:
    return "throne";
  case BF_CRAFTING:
    return "crafting";
  case BF_RESEARCH:
    return "research";
  case BF_MINE:
    return "mine";
  case BF_MINE_NATURAL:
    return "mine_natural";
  case BF_REFINERY:
    return "refinery";
  case BF_GENERATOR:
    return "generator";
  case BF_ENGINE:
    return "engine";
  case BF_STEERING:
    return "steering";
  case BF_ITEM_FURNACE:
    return "item_furnace";
  case BF_CAGE:
    return "cage";
  case BF_TABLE:
    return "table";
  default:
    return "other";
  }
}

std::string UseObjectTaskLabel(TaskType taskType) {
  switch (taskType) {
  case USE_TURRET:
    return "USE_TURRET";
  case MAN_A_TURRET:
    return "MAN_A_TURRET";
  case MAN_A_TURRET_ON_BUILDING:
    return "MAN_A_TURRET_ON_BUILDING";
  case MAN_A_TURRET_PLAYER_JOB:
    return "MAN_A_TURRET_PLAYER_JOB";
  case USE_BED:
    return "USE_BED";
  case USE_BED_ORDER:
    return "USE_BED_ORDER";
  case OPERATE_MACHINERY:
    return "OPERATE_MACHINERY";
  case OPERATE_AUTOMATIC_MACHINERY:
    return "OPERATE_AUTOMATIC_MACHINERY";
  case USE_CAGE:
    return "USE_CAGE";
  case SIT_ON_THRONE:
    return "SIT_ON_THRONE";
  case SIT_AROUND:
    return "SIT_AROUND";
  case USE_TRAINING_DUMMY:
    return "USE_TRAINING_DUMMY";
  case REST:
    return "REST";
  case JOB_KEEP_EVERYTHING_RUNNING:
    return "JOB_KEEP_EVERYTHING_RUNNING";
  default:
    break;
  }
  return "TASK_" + ToString((int)taskType);
}

bool IsUseObjectClassCandidate(BuildingClassType classType) {
  return classType == BCTYPE_USABLE || classType == BCTYPE_TURRET ||
         classType == BCTYPE_PRODUCTION || classType == BCTYPE_CRAFTING ||
         classType == BCTYPE_RESEARCH || classType == BCTYPE_FARM;
}

bool IsUseObjectFunctionCandidate(BuildingFunction functionType) {
  switch (functionType) {
  case BF_CHAIR:
  case BF_TURRET:
  case BF_BED:
  case BF_SKELETON_BED:
  case BF_THRONE:
  case BF_CRAFTING:
  case BF_RESEARCH:
  case BF_MINE:
  case BF_MINE_NATURAL:
  case BF_REFINERY:
  case BF_GENERATOR:
  case BF_ENGINE:
  case BF_STEERING:
  case BF_ITEM_FURNACE:
  case BF_CAGE:
  case BF_TABLE:
    return true;
  default:
    return false;
  }
}

bool IsUseObjectTaskCandidate(TaskType taskType) {
  switch (taskType) {
  case USE_TURRET:
  case MAN_A_TURRET:
  case MAN_A_TURRET_ON_BUILDING:
  case MAN_A_TURRET_PLAYER_JOB:
  case USE_BED:
  case USE_BED_ORDER:
  case OPERATE_MACHINERY:
  case OPERATE_AUTOMATIC_MACHINERY:
  case USE_CAGE:
  case SIT_ON_THRONE:
  case SIT_AROUND:
  case USE_TRAINING_DUMMY:
  case REST:
  case JOB_KEEP_EVERYTHING_RUNNING:
    return true;
  default:
    return false;
  }
}

TaskType ResolveUseObjectTask(TaskType defaultTask,
                              BuildingFunction functionType) {
  if (defaultTask != NULL_TASK && IsUseObjectTaskCandidate(defaultTask)) {
    return defaultTask;
  }
  switch (functionType) {
  case BF_TURRET:
    return MAN_A_TURRET_ON_BUILDING;
  case BF_BED:
  case BF_SKELETON_BED:
    return USE_BED;
  case BF_CAGE:
    return USE_CAGE;
  case BF_THRONE:
    return SIT_ON_THRONE;
  case BF_CHAIR:
  case BF_TABLE:
    return SIT_AROUND;
  case BF_CRAFTING:
  case BF_RESEARCH:
  case BF_REFINERY:
  case BF_GENERATOR:
  case BF_ENGINE:
  case BF_STEERING:
  case BF_ITEM_FURNACE:
  case BF_MINE:
  case BF_MINE_NATURAL:
    return OPERATE_MACHINERY;
  default:
    return NULL_TASK;
  }
}

bool LooksLikeDataStringId(const std::string &value) {
  std::string normalized = TrimCopy(value);
  if (normalized.empty()) {
    return false;
  }
  std::string lower = normalized;
  for (size_t i = 0; i < lower.size(); ++i) {
    lower[i] = (char)std::tolower((unsigned char)lower[i]);
  }
  if (lower.find(".mod") != std::string::npos) {
    return true;
  }
  if (lower.find("gamedata.base") != std::string::npos) {
    return true;
  }
  // Typical FCS/rebirth IDs like "52306-rebirth.mod" or "14519-gamedata.base".
  size_t dash = lower.find('-');
  if (dash != std::string::npos && dash > 0) {
    bool numericPrefix = true;
    for (size_t i = 0; i < dash; ++i) {
      if (!std::isdigit((unsigned char)lower[i])) {
        numericPrefix = false;
        break;
      }
    }
    if (numericPrefix) {
      return true;
    }
  }
  return false;
}

std::string ResolveGameDataString(GameData *data, const char *key) {
  if (!data || !key) {
    return "";
  }
  std::string keyName = key;
  auto it = data->sdata.find(keyName);
  if (it == data->sdata.end()) {
    return "";
  }
  std::string value = TrimCopy(it->second);
  if (IsUnknownToken(value)) {
    return "";
  }
  return value;
}

bool TryResolveGameDataNumber(GameData *data, const char *key, float &valueOut) {
  if (!data || !key) {
    return false;
  }
  std::string keyName = key;

  auto fit = data->fdata.find(keyName);
  if (fit != data->fdata.end()) {
    valueOut = fit->second;
    return true;
  }

  auto iit = data->idata.find(keyName);
  if (iit != data->idata.end()) {
    valueOut = (float)iit->second;
    return true;
  }

  auto sit = data->sdata.find(keyName);
  if (sit != data->sdata.end()) {
    const std::string raw = TrimCopy(sit->second);
    if (!raw.empty()) {
      valueOut = (float)atof(raw.c_str());
      return true;
    }
  }

  return false;
}

struct HornSliderSnapshot {
  bool hasAny;
  bool hasBody;
  bool hasUpper;
  bool hasLower;
  unsigned int liveDataPtr;
  unsigned int sourceDataPtr;
  float body;
  float upper;
  float lower;
  std::string bodyKey;
  std::string upperKey;
  std::string lowerKey;

  HornSliderSnapshot()
      : hasAny(false), hasBody(false), hasUpper(false), hasLower(false),
        liveDataPtr(0), sourceDataPtr(0), body(0.0f), upper(0.0f),
        lower(0.0f) {}
};

std::string CanonicalizeAppearanceSliderKey(const std::string &value) {
  std::string canonical;
  canonical.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    unsigned char ch = static_cast<unsigned char>(value[i]);
    if (std::isalnum(ch)) {
      canonical.push_back((char)std::tolower(ch));
    }
  }
  return canonical;
}

bool TryParseGameDataNumberString(const std::string &rawValue, float &valueOut) {
  const std::string value = TrimCopy(rawValue);
  if (value.empty()) {
    return false;
  }

  char *endPtr = nullptr;
  valueOut = (float)strtod(value.c_str(), &endPtr);
  if (endPtr == value.c_str()) {
    return false;
  }
  while (endPtr && *endPtr != '\0') {
    if (!std::isspace((unsigned char)*endPtr)) {
      return false;
    }
    ++endPtr;
  }
  return true;
}

bool TryResolveGameDataNumberByAliases(GameData *data, const char *const *aliases,
                                       size_t aliasCount, float &valueOut,
                                       std::string &matchedKeyOut) {
  matchedKeyOut.clear();
  if (!data || !aliases || aliasCount == 0) {
    return false;
  }

  for (size_t i = 0; i < aliasCount; ++i) {
    if (!aliases[i]) {
      continue;
    }
    if (TryResolveGameDataNumber(data, aliases[i], valueOut)) {
      matchedKeyOut = aliases[i];
      return true;
    }
  }

  std::vector<std::string> canonicalAliases;
  canonicalAliases.reserve(aliasCount);
  for (size_t i = 0; i < aliasCount; ++i) {
    if (aliases[i]) {
      canonicalAliases.push_back(CanonicalizeAppearanceSliderKey(aliases[i]));
    }
  }

  auto matchesExact = [&](const std::string &candidate) -> bool {
    for (size_t i = 0; i < canonicalAliases.size(); ++i) {
      if (candidate == canonicalAliases[i]) {
        return true;
      }
    }
    return false;
  };

  auto matchesLoose = [&](const std::string &candidate) -> bool {
    for (size_t i = 0; i < canonicalAliases.size(); ++i) {
      if (candidate.find(canonicalAliases[i]) != std::string::npos ||
          canonicalAliases[i].find(candidate) != std::string::npos) {
        return true;
      }
    }
    return false;
  };

  auto tryMatchCanonical = [&](bool allowLoose) -> bool {
    for (auto it = data->fdata.begin(); it != data->fdata.end(); ++it) {
      const std::string candidate =
          CanonicalizeAppearanceSliderKey(it->first);
      const bool matches =
          allowLoose ? matchesLoose(candidate) : matchesExact(candidate);
      if (matches) {
        valueOut = it->second;
        matchedKeyOut = it->first;
        return true;
      }
    }

    for (auto it = data->idata.begin(); it != data->idata.end(); ++it) {
      const std::string candidate =
          CanonicalizeAppearanceSliderKey(it->first);
      const bool matches =
          allowLoose ? matchesLoose(candidate) : matchesExact(candidate);
      if (matches) {
        valueOut = (float)it->second;
        matchedKeyOut = it->first;
        return true;
      }
    }

    for (auto it = data->sdata.begin(); it != data->sdata.end(); ++it) {
      const std::string candidate =
          CanonicalizeAppearanceSliderKey(it->first);
      const bool matches =
          allowLoose ? matchesLoose(candidate) : matchesExact(candidate);
      if (!matches) {
        continue;
      }
      float parsedValue = 0.0f;
      if (TryParseGameDataNumberString(it->second, parsedValue)) {
        valueOut = parsedValue;
        matchedKeyOut = it->first;
        return true;
      }
    }

    return false;
  };

  return tryMatchCanonical(false) || tryMatchCanonical(true);
}

float NormalizeHornLargeValue(float sliderValue) {
  if (sliderValue >= 0.0f && sliderValue <= 1.0f) {
    sliderValue *= 100.0f;
  }
  float normalized = (100.0f - sliderValue) / 100.0f;
  if (normalized < 0.0f) {
    return 0.0f;
  }
  if (normalized > 1.0f) {
    return 1.0f;
  }
  return normalized;
}

float NormalizeHornSliderForOutput(float sliderValue) {
  if (sliderValue >= 0.0f && sliderValue <= 1.0f) {
    return sliderValue * 100.0f;
  }
  return sliderValue;
}

std::string BuildHornSliderPayload(const HornSliderSnapshot &snapshot) {
  if (!snapshot.hasAny) {
    return "{}";
  }

  int sliderCount = 0;
  float sliderSum = 0.0f;
  float largeSum = 0.0f;
  std::string json = "{";
  std::string matchedKeysJson = "{";

  auto appendSlider = [&](const char *name, float sliderValue,
                          const std::string &matchedKey) {
    const float outputValue = NormalizeHornSliderForOutput(sliderValue);
    const float largeNorm = NormalizeHornLargeValue(sliderValue);
    json += "\"" + std::string(name) + "\":" + ToString(outputValue) + ",";
    json += "\"" + std::string(name) + "_large_norm\":" + ToString(largeNorm) +
            ",";
    matchedKeysJson += "\"" + std::string(name) + "\":\"" +
                       EscapeJSON(matchedKey) + "\",";
    sliderSum += outputValue;
    largeSum += largeNorm;
    ++sliderCount;
  };

  if (snapshot.hasBody) {
    appendSlider("body", snapshot.body, snapshot.bodyKey);
  }
  if (snapshot.hasUpper) {
    appendSlider("upper", snapshot.upper, snapshot.upperKey);
  }
  if (snapshot.hasLower) {
    appendSlider("lower", snapshot.lower, snapshot.lowerKey);
  }

  if (sliderCount > 0) {
    json += "\"average\":" + ToString(sliderSum / (float)sliderCount) + ",";
    json += "\"average_large_norm\":" +
            ToString(largeSum / (float)sliderCount) + ",";
  }
  if (matchedKeysJson.back() == ',') {
    matchedKeysJson.pop_back();
  }
  matchedKeysJson += "}";
  json += "\"matched_keys\":" + matchedKeysJson;
  if (json.back() == ',') {
    json.pop_back();
  }
  json += "}";
  return json;
}

void CaptureHornSliderSnapshot(Character *character, HornSliderSnapshot &out) {
  out = HornSliderSnapshot();
  if (!character || (uintptr_t)character <= 0x1000) {
    return;
  }

  AppearanceBase *appearance = nullptr;
  try {
    appearance = character->getAppearance();
  } catch (...) {
    appearance = nullptr;
  }
  if (!appearance || (uintptr_t)appearance <= 0x1000) {
    return;
  }

  GameData *appearanceData = nullptr;
  try {
    appearanceData = appearance->getAppearanceData();
  } catch (...) {
    appearanceData = nullptr;
  }
  if (!appearanceData || (uintptr_t)appearanceData <= 0x1000) {
    return;
  }
  out.liveDataPtr = (unsigned int)(uintptr_t)appearanceData;

  try {
    GameDataCopyStandalone *sourceAppearanceData = character->getAppearanceData();
    if (sourceAppearanceData && (uintptr_t)sourceAppearanceData > 0x1000) {
      out.sourceDataPtr = (unsigned int)(uintptr_t)sourceAppearanceData;
    }
  } catch (...) {
    out.sourceDataPtr = 0;
  }

  static const char *kHornBodyAliases[] = {
      "horn body",           "body horn",           "horn_body",
      "body_horn",           "hornbody",            "bodyhorn",
      "bone_horns_body_short"};
  static const char *kHornUpperAliases[] = {
      "horn upper",          "upper horn",          "horn_upper",
      "upper_horn",          "hornupper",           "upperhorn",
      "bone_horns_top_short"};
  static const char *kHornLowerAliases[] = {
      "horn lower",          "lower horn",          "horn_lower",
      "lower_horn",          "hornlower",           "lowerhorn",
      "bone_horns_bottom_short"};

  out.hasBody = TryResolveGameDataNumberByAliases(
      appearanceData, kHornBodyAliases,
      sizeof(kHornBodyAliases) / sizeof(kHornBodyAliases[0]), out.body,
      out.bodyKey);
  out.hasUpper = TryResolveGameDataNumberByAliases(
      appearanceData, kHornUpperAliases,
      sizeof(kHornUpperAliases) / sizeof(kHornUpperAliases[0]), out.upper,
      out.upperKey);
  out.hasLower = TryResolveGameDataNumberByAliases(
      appearanceData, kHornLowerAliases,
      sizeof(kHornLowerAliases) / sizeof(kHornLowerAliases[0]), out.lower,
      out.lowerKey);
  out.hasAny = out.hasBody || out.hasUpper || out.hasLower;
}

std::string ResolveGameDataRefName(GameData *data, const char *key) {
  if (!data || !key) {
    return "";
  }
  const Ogre::vector<GameDataReference>::type *refs =
      data->getReferenceListIfExists(key);
  if (!refs || refs->empty()) {
    return "";
  }
  for (size_t i = 0; i < refs->size(); ++i) {
    const GameDataReference &ref = refs->at(i);
    // Runtime reference pointers can be stale for nearby streamed world objects.
    // The persisted string ID remains valid without dereferencing that object.
    std::string value = TrimCopy(ref.sid);
    if (!IsUnknownToken(value)) {
      return value;
    }
  }
  return "";
}

std::string ResolveGameDataRefSid(GameData *data, const char *key) {
  if (!data || !key) {
    return "";
  }
  const Ogre::vector<GameDataReference>::type *refs =
      data->getReferenceListIfExists(key);
  if (!refs || refs->empty()) {
    return "";
  }
  for (size_t i = 0; i < refs->size(); ++i) {
    const GameDataReference &ref = refs->at(i);
    std::string sid = TrimCopy(ref.sid);
    if (!IsUnknownToken(sid)) {
      return sid;
    }
  }
  return "";
}

std::string ResolveGameDataIdentity(GameData *data) {
  if (!data || (uintptr_t)data <= 0x1000) {
    return "";
  }
  std::string name = TrimCopy(data->name);
  if (!IsUnknownToken(name)) {
    return name;
  }
  std::string sid = TrimCopy(data->stringID);
  if (!IsUnknownToken(sid)) {
    return sid;
  }
  return "";
}

std::string NormalizeWeaponModelLabel(const std::string &value) {
  std::string normalized = TrimCopy(value);
  if (normalized.empty()) {
    return "";
  }
  std::string collapsed;
  collapsed.reserve(normalized.size());
  bool inWhitespace = false;
  for (size_t i = 0; i < normalized.size(); ++i) {
    unsigned char ch = (unsigned char)normalized[i];
    if (std::isspace(ch)) {
      if (!inWhitespace && !collapsed.empty()) {
        collapsed.push_back(' ');
      }
      inWhitespace = true;
    } else {
      collapsed.push_back((char)ch);
      inWhitespace = false;
    }
  }
  normalized = TrimCopy(collapsed);
  if (normalized.length() >= 2 && normalized.front() == '[' &&
      normalized.back() == ']') {
    normalized = TrimCopy(normalized.substr(1, normalized.length() - 2));
  }
  if (normalized.empty() || IsUnknownToken(normalized) ||
      LooksLikeDataStringId(normalized)) {
    return "";
  }
  return normalized;
}

bool TryExtractWeaponModel(Item *item, std::string &modelOut) {
  modelOut.clear();
  if (!item || (uintptr_t)item <= 0x1000) {
    return false;
  }

  bool isWeaponLike = false;
  try {
    isWeaponLike = (item->isWeapon() != NULL) || (item->isCrossbow() != NULL);
  } catch (...) {
    isWeaponLike = false;
  }
  if (!isWeaponLike) {
    return false;
  }

  auto assignModel = [&](const std::string &candidate) -> bool {
    std::string normalized = NormalizeWeaponModelLabel(candidate);
    if (normalized.empty()) {
      return false;
    }
    modelOut = normalized;
    return true;
  };

  try {
    if (item->manufacturerData && (uintptr_t)item->manufacturerData > 0x1000) {
      if (assignModel(ResolveGameDataIdentity(item->manufacturerData))) {
        return true;
      }
      if (assignModel(TrimCopy(item->manufacturerData->stringID))) {
        return true;
      }
    }
  } catch (...) {
  }

  if (item->data && (uintptr_t)item->data > 0x1000) {
    const char *refKeys[] = {"manufacturer", "weapon_manufacturer",
                             "manufacturerData", "model", "weapon_model"};
    for (size_t i = 0; i < sizeof(refKeys) / sizeof(refKeys[0]); ++i) {
      if (assignModel(ResolveGameDataRefName(item->data, refKeys[i]))) {
        return true;
      }
      if (assignModel(ResolveGameDataRefSid(item->data, refKeys[i]))) {
        return true;
      }
    }

    const char *stringKeys[] = {"weapon_model",      "weaponModel",
                                "manufacturer_name", "manufacturer",
                                "model_name",        "model"};
    for (size_t i = 0; i < sizeof(stringKeys) / sizeof(stringKeys[0]); ++i) {
      if (assignModel(ResolveGameDataString(item->data, stringKeys[i]))) {
        return true;
      }
    }
  }

  return false;
}

std::string CollapseWhitespace(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  bool inWhitespace = false;
  for (size_t i = 0; i < value.size(); ++i) {
    unsigned char ch = (unsigned char)value[i];
    if (std::isspace(ch)) {
      if (!inWhitespace && !out.empty()) {
        out.push_back(' ');
      }
      inWhitespace = true;
    } else {
      out.push_back((char)ch);
      inWhitespace = false;
    }
  }
  return TrimCopy(out);
}

std::string ResolveItemDescription(GameData *data) {
  if (!data || (uintptr_t)data <= 0x1000) {
    return "";
  }
  const char *keys[] = {"description", "item_description", "item_desc", "desc",
                        "tooltip",     "tooltip_text",     "lore",      "flavor",
                        "flavour"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    std::string value = ResolveGameDataString(data, keys[i]);
    if (value.empty()) {
      continue;
    }
    value = CollapseWhitespace(value);
    if (value.empty() || IsUnknownToken(value)) {
      continue;
    }
    return value;
  }
  return "";
}

std::string ResolveTownZoneName(TownBase *town) {
  if (!town || (uintptr_t)town <= 0x1000) {
    return "";
  }
  RootObjectBase *townBase = (RootObjectBase *)town;
  if (!townBase || (uintptr_t)townBase <= 0x1000) {
    return "";
  }
  GameData *townData = townBase->getGameData();
  if (!townData || (uintptr_t)townData <= 0x1000) {
    return "";
  }

  const char *stringKeys[] = {"zone_name",  "region_name", "zone",     "region",
                              "biome_name", "biome",       "territory", "district",
                              "province",   "area_name",   "area"};
  for (size_t i = 0; i < sizeof(stringKeys) / sizeof(stringKeys[0]); ++i) {
    std::string value = ResolveGameDataString(townData, stringKeys[i]);
    if (!value.empty()) {
      return value;
    }
  }

  const char *refKeys[] = {"zone",      "region",     "biome",
                           "zone_name", "region_name","biome_group",
                           "territory", "district",   "province",
                           "area"};
  for (size_t i = 0; i < sizeof(refKeys) / sizeof(refKeys[0]); ++i) {
    std::string value = ResolveGameDataRefName(townData, refKeys[i]);
    if (!value.empty()) {
      return value;
    }
  }

  const char *listKeys[] = {"zone", "region", "biome", "territory", "district",
                            "province", "area"};
  for (size_t i = 0; i < sizeof(listKeys) / sizeof(listKeys[0]); ++i) {
    const std::string key = listKeys[i];
    if (!townData->listExistsAndNotEmpty(key)) {
      continue;
    }
    std::string value = TrimCopy(townData->getFromList(key, 0));
    if (!IsUnknownToken(value)) {
      return value;
    }
  }

  return "";
}

std::string ResolveTownRegionName(TownBase *town) {
  if (!town || (uintptr_t)town <= 0x1000) {
    return "";
  }
  RootObjectBase *townBase = (RootObjectBase *)town;
  if (!townBase || (uintptr_t)townBase <= 0x1000) {
    return "";
  }
  GameData *townData = townBase->getGameData();
  if (!townData || (uintptr_t)townData <= 0x1000) {
    return "";
  }

  const char *stringKeys[] = {"region_name", "region",      "biome_name",
                              "biome",       "territory",   "district",
                              "province",    "area_name",   "area",
                              "zone_name",   "zone"};
  for (size_t i = 0; i < sizeof(stringKeys) / sizeof(stringKeys[0]); ++i) {
    std::string value = ResolveGameDataString(townData, stringKeys[i]);
    if (!value.empty()) {
      return value;
    }
  }

  const char *refKeys[] = {"region",   "biome",      "region_name",
                           "biome_group", "territory", "district",
                           "province", "area",       "zone",
                           "zone_name"};
  for (size_t i = 0; i < sizeof(refKeys) / sizeof(refKeys[0]); ++i) {
    std::string value = ResolveGameDataRefName(townData, refKeys[i]);
    if (!value.empty()) {
      return value;
    }
  }

  const char *listKeys[] = {"region", "biome", "territory", "district",
                            "province", "area", "zone"};
  for (size_t i = 0; i < sizeof(listKeys) / sizeof(listKeys[0]); ++i) {
    const std::string key = listKeys[i];
    if (!townData->listExistsAndNotEmpty(key)) {
      continue;
    }
    std::string value = TrimCopy(townData->getFromList(key, 0));
    if (!IsUnknownToken(value)) {
      return value;
    }
  }

  return "";
}

float ResolveTownBoundaryRadius(TownBase *town) {
  if (!town || (uintptr_t)town <= 0x1000) {
    return -1.0f;
  }
  RootObjectBase *townBase = (RootObjectBase *)town;
  if (!townBase || (uintptr_t)townBase <= 0x1000) {
    return -1.0f;
  }
  GameData *townData = townBase->getGameData();
  if (!townData || (uintptr_t)townData <= 0x1000) {
    return -1.0f;
  }

  const char *radiusKeys[] = {"radius",         "town_radius",
                              "town_size",      "border_radius",
                              "border_range",   "borders_range",
                              "discovery_range"};
  for (size_t i = 0; i < sizeof(radiusKeys) / sizeof(radiusKeys[0]); ++i) {
    float value = -1.0f;
    if (TryResolveGameDataNumber(townData, radiusKeys[i], value) && value > 0.0f) {
      return value;
    }
  }
  return -1.0f;
}

bool IsNpcInsideTownContext(Character *npc, TownBase *town) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }

  bool insideWalls = false;
  try {
    insideWalls = (npc->amInsideTownWalls() != 0);
  } catch (...) {
    insideWalls = false;
  }

  if (!town || (uintptr_t)town <= 0x1000) {
    return insideWalls;
  }

  RootObjectBase *townBase = (RootObjectBase *)town;
  if (!townBase || (uintptr_t)townBase <= 0x1000) {
    return insideWalls;
  }

  Ogre::Vector3 npcPos;
  Ogre::Vector3 townPos;
  try {
    npcPos = npc->getPosition();
    townPos = townBase->getPosition();
  } catch (...) {
    return insideWalls;
  }

  const float dx = npcPos.x - townPos.x;
  const float dy = npcPos.y - townPos.y;
  const float dz = npcPos.z - townPos.z;
  const float distanceToTownCenter = std::sqrt(dx * dx + dy * dy + dz * dz);

  const float townRadius = ResolveTownBoundaryRadius(town);
  if (townRadius > 0.0f) {
    // If engine wall flag lingers while we're clearly outside radius, override it.
    if (distanceToTownCenter > (townRadius * 1.20f)) {
      return false;
    }
    if (distanceToTownCenter <= (townRadius * 1.05f)) {
      return true;
    }
  } else {
    // Some settlements do not expose reliable radius metadata.
    // Use center-distance heuristics to avoid stale town labels while still
    // allowing un-walled settlements to register as "in town" when nearby.
    if (distanceToTownCenter <= 650.0f) {
      return true;
    }
    if (distanceToTownCenter >= 1200.0f) {
      return false;
    }
  }

  return insideWalls;
}

bool IsLikelyWorldZoneName(const std::string &rawValue) {
  std::string value = TrimCopy(rawValue);
  if (value.empty() || IsUnknownToken(value)) {
    return false;
  }
  std::string lowered = value;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  if (lowered.rfind("the ", 0) == 0) {
    lowered = lowered.substr(4);
  }

  static const char *kKnownZones[] = {
      "arach",           "bast",          "black desert",   "bonefields",
      "border zone",     "burning forest","cannibal plains","darkfinger",
      "deadlands",       "dreg",          "fishman island", "floodlands",
      "fog islands",     "forbidden isle","great desert",   "great plateau",
      "greenbeach",      "gut",           "heng",           "hidden forest",
      "high bonefields", "howler maze",   "iron valleys",   "leviathan coast",
      "narrow valley",   "okrans gulf",   "okrans pride",   "outlands",
      "purple sands",    "raptor island", "royal valley",   "shem",
      "shun",            "sinkuun",       "skinners roam",  "sonorous dark",
      "stobes gamble",   "stenn desert",  "stormgap coast", "swamp",
      "the crater",      "the grid",      "the hook",       "the pits",
      "the pits east",   "unwanted zone", "watchers rim",   "wend"};

  for (size_t i = 0; i < sizeof(kKnownZones) / sizeof(kKnownZones[0]); ++i) {
    std::string known = kKnownZones[i];
    if (lowered == known) {
      return true;
    }
    if (known.rfind("the ", 0) == 0 && lowered == known.substr(4)) {
      return true;
    }
  }
  return false;
}

std::string ResolveMappedRegionFromTownName(const std::string &rawTownName) {
  std::string key = TrimCopy(rawTownName);
  if (key.empty() || IsUnknownToken(key)) {
    return "";
  }
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  key = CollapseWhitespace(key);
  if (key.empty()) {
    return "";
  }

  static std::map<std::string, std::string> kTownToRegion;
  if (kTownToRegion.empty()) {
    kTownToRegion["the hub"] = "Border Zone";
    kTownToRegion["squin"] = "Stenn Desert";
    kTownToRegion["admag"] = "Stenn Desert";
    kTownToRegion["stack"] = "Okran's Pride";
    kTownToRegion["bad teeth"] = "Okran's Pride";
    kTownToRegion["blister hill"] = "Okran's Pride";
    kTownToRegion["world's end"] = "Hidden Forest";
    kTownToRegion["mongrel"] = "Fog Islands";
    kTownToRegion["shark"] = "Swamp";
    kTownToRegion["mud town"] = "Swamp";
    kTownToRegion["rot"] = "Swamp";
    kTownToRegion["black desert city"] = "Deadlands";
    kTownToRegion["flats lagoon"] = "The Grid";
    kTownToRegion["clownsteady"] = "The Hook";
    kTownToRegion["drifter's last"] = "The Hook";
    kTownToRegion["bark"] = "The Hook";
    kTownToRegion["heft"] = "Great Desert";
    kTownToRegion["sho-battai"] = "Great Desert";
    kTownToRegion["stoat"] = "Great Desert";
    kTownToRegion["heng"] = "Heng";
    kTownToRegion["catun"] = "The Pits East";
    kTownToRegion["mourn"] = "Bonefields";
    kTownToRegion["black scratch"] = "Stormgap Coast";
    kTownToRegion["spring"] = "Stobe's Gamble";
    kTownToRegion["tower of abuse"] = "Venge";
  }

  auto it = kTownToRegion.find(key);
  if (it != kTownToRegion.end()) {
    return it->second;
  }
  return "";
}

std::string ResolveRootObjectZoneName(RootObjectBase *objectBase);
std::string ResolveRootObjectRegionName(RootObjectBase *objectBase);

std::string ResolveNearestTownMappedRegion(GameWorld *world, Character *npc) {
  if (!world || !npc || (uintptr_t)npc <= 0x1000) {
    return "";
  }

  float searchRadius = g_visionRange;
  if (searchRadius < 30000.0f) {
    searchRadius = 30000.0f;
  } else if (searchRadius > 80000.0f) {
    searchRadius = 80000.0f;
  }

  float bestDistance = 3.4e38f;
  std::string bestRegion = "";

  auto considerObjects = [&](itemType objectType, int maxCount) {
    lektor<RootObject *> nearby;
    world->getObjectsWithinSphere(nearby, npc->getPosition(), searchRadius,
                                  objectType, maxCount, (RootObject *)npc);
    for (uint32_t i = 0; i < nearby.size(); ++i) {
      RootObjectBase *poi = (RootObjectBase *)nearby.stuff[i];
      if (!poi || (uintptr_t)poi <= 0x1000) {
        continue;
      }

      std::string poiName = TrimCopy(poi->getName());
      std::string poiZone = ResolveRootObjectZoneName(poi);
      std::string poiRegion = ResolveRootObjectRegionName(poi);

      std::string mappedRegion = ResolveMappedRegionFromTownName(poiName);
      if (mappedRegion.empty() && IsLikelyWorldZoneName(poiRegion)) {
        mappedRegion = poiRegion;
      }
      if (mappedRegion.empty() && IsLikelyWorldZoneName(poiZone)) {
        mappedRegion = poiZone;
      }
      if (mappedRegion.empty()) {
        continue;
      }

      float distance = npc->getPosition().distance(poi->getPosition());
      if (distance >= bestDistance) {
        continue;
      }
      bestDistance = distance;
      bestRegion = mappedRegion;
    }
  };

  considerObjects(TOWN, 64);
  considerObjects(LOCATION, 96);
  return bestRegion;
}

std::string ResolveNearestWorldZoneName(GameWorld *world, Character *npc) {
  if (!world || !npc || (uintptr_t)npc <= 0x1000) {
    return "";
  }

  float searchRadius = g_visionRange;
  if (searchRadius < 1500.0f) {
    searchRadius = 1500.0f;
  } else if (searchRadius > 12000.0f) {
    searchRadius = 12000.0f;
  }

  auto resolveFromType = [&](itemType objectType, int maxCount,
                             std::string &zoneOut, float &distanceOut) {
    zoneOut.clear();
    distanceOut = 3.4e38f;

    lektor<RootObject *> nearbyPois;
    world->getObjectsWithinSphere(nearbyPois, npc->getPosition(), searchRadius,
                                  objectType, maxCount, (RootObject *)npc);

    for (uint32_t i = 0; i < nearbyPois.size(); ++i) {
      RootObjectBase *poi = (RootObjectBase *)nearbyPois.stuff[i];
      if (!poi || (uintptr_t)poi <= 0x1000) {
        continue;
      }

      std::string poiRegion = ResolveRootObjectRegionName(poi);
      std::string poiZone = ResolveRootObjectZoneName(poi);
      std::string poiName = TrimCopy(poi->getName());
      std::string candidate = "";
      if (IsLikelyWorldZoneName(poiRegion)) {
        candidate = poiRegion;
      } else if (IsLikelyWorldZoneName(poiZone)) {
        candidate = poiZone;
      } else if (IsLikelyWorldZoneName(poiName)) {
        candidate = poiName;
      }
      if (candidate.empty()) {
        continue;
      }

      float distance = npc->getPosition().distance(poi->getPosition());
      if (distance >= distanceOut) {
        continue;
      }
      distanceOut = distance;
      zoneOut = candidate;
    }
  };

  std::string locationZone = "";
  float locationDistance = 3.4e38f;
  resolveFromType(LOCATION, 48, locationZone, locationDistance);

  std::string townZone = "";
  float townDistance = 3.4e38f;
  resolveFromType(TOWN, 24, townZone, townDistance);

  if (!locationZone.empty() && !townZone.empty()) {
    return (locationDistance <= townDistance) ? locationZone : townZone;
  }
  if (!locationZone.empty()) {
    return locationZone;
  }
  return townZone;
}

std::string ResolveRootObjectZoneName(RootObjectBase *objectBase) {
  if (!objectBase || (uintptr_t)objectBase <= 0x1000) {
    return "";
  }
  GameData *objectData = objectBase->getGameData();
  if (!objectData || (uintptr_t)objectData <= 0x1000) {
    return "";
  }

  const char *stringKeys[] = {"zone_name",  "region_name", "zone",     "region",
                              "biome_name", "biome",       "territory", "district",
                              "province",   "area_name",   "area"};
  for (size_t i = 0; i < sizeof(stringKeys) / sizeof(stringKeys[0]); ++i) {
    std::string value = ResolveGameDataString(objectData, stringKeys[i]);
    if (!value.empty()) {
      return value;
    }
  }

  const char *refKeys[] = {"zone",      "region",     "biome",
                           "zone_name", "region_name","biome_group",
                           "territory", "district",   "province",
                           "area"};
  for (size_t i = 0; i < sizeof(refKeys) / sizeof(refKeys[0]); ++i) {
    std::string value = ResolveGameDataRefName(objectData, refKeys[i]);
    if (!value.empty()) {
      return value;
    }
  }

  const char *listKeys[] = {"zone", "region", "biome", "territory", "district",
                            "province", "area"};
  for (size_t i = 0; i < sizeof(listKeys) / sizeof(listKeys[0]); ++i) {
    const std::string key = listKeys[i];
    if (!objectData->listExistsAndNotEmpty(key)) {
      continue;
    }
    std::string value = TrimCopy(objectData->getFromList(key, 0));
    if (!IsUnknownToken(value)) {
      return value;
    }
  }

  return "";
}

std::string ResolveRootObjectRegionName(RootObjectBase *objectBase) {
  if (!objectBase || (uintptr_t)objectBase <= 0x1000) {
    return "";
  }
  GameData *objectData = objectBase->getGameData();
  if (!objectData || (uintptr_t)objectData <= 0x1000) {
    return "";
  }

  const char *stringKeys[] = {"region_name", "region",      "biome_name",
                              "biome",       "territory",   "district",
                              "province",    "area_name",   "area",
                              "zone_name",   "zone"};
  for (size_t i = 0; i < sizeof(stringKeys) / sizeof(stringKeys[0]); ++i) {
    std::string value = ResolveGameDataString(objectData, stringKeys[i]);
    if (!value.empty()) {
      return value;
    }
  }

  const char *refKeys[] = {"region",      "biome",      "region_name",
                           "biome_group", "territory",  "district",
                           "province",    "area",       "zone",
                           "zone_name"};
  for (size_t i = 0; i < sizeof(refKeys) / sizeof(refKeys[0]); ++i) {
    std::string value = ResolveGameDataRefName(objectData, refKeys[i]);
    if (!value.empty()) {
      return value;
    }
  }

  const char *listKeys[] = {"region", "biome", "territory", "district",
                            "province", "area", "zone"};
  for (size_t i = 0; i < sizeof(listKeys) / sizeof(listKeys[0]); ++i) {
    const std::string key = listKeys[i];
    if (!objectData->listExistsAndNotEmpty(key)) {
      continue;
    }
    std::string value = TrimCopy(objectData->getFromList(key, 0));
    if (!IsUnknownToken(value)) {
      return value;
    }
  }

  return "";
}

void ResolveNearbyTownAndZoneFallback(GameWorld *world, Character *npc,
                                      TownBase *currentTown,
                                      std::string &townNameInOut,
                                      std::string &zoneNameInOut) {
  if (!world || !npc || (uintptr_t)npc <= 0x1000) {
    return;
  }
  bool inTownContext = IsNpcInsideTownContext(npc, currentTown);
  if (!inTownContext) {
    // Avoid stale labels when actor has moved outside town walls.
    // Re-resolve zone from nearby world context below.
    townNameInOut.clear();
    zoneNameInOut.clear();
  }

  if (!townNameInOut.empty() && !zoneNameInOut.empty()) {
    return;
  }

  float searchRadius = g_visionRange;
  if (searchRadius < 1500.0f) {
    searchRadius = 1500.0f;
  } else if (searchRadius > 12000.0f) {
    searchRadius = 12000.0f;
  }

  auto resolveNearestPoi = [&](itemType objectType, int maxCount, std::string &nameOut,
                               std::string &zoneOut, float *distanceOut) {
    nameOut.clear();
    zoneOut.clear();
    if (distanceOut) {
      *distanceOut = 3.4e38f;
    }

    lektor<RootObject *> nearbyPois;
    world->getObjectsWithinSphere(nearbyPois, npc->getPosition(), searchRadius,
                                  objectType, maxCount, (RootObject *)npc);

    float bestDistance = 3.4e38f;
    for (uint32_t i = 0; i < nearbyPois.size(); ++i) {
      RootObjectBase *poi = (RootObjectBase *)nearbyPois.stuff[i];
      if (!poi || (uintptr_t)poi <= 0x1000) {
        continue;
      }

      std::string poiName = TrimCopy(poi->getName());
      if (IsUnknownToken(poiName)) {
        poiName.clear();
      }
      std::string poiZone = ResolveRootObjectZoneName(poi);
      if (poiName.empty() && poiZone.empty()) {
        continue;
      }

      float distance = npc->getPosition().distance(poi->getPosition());
      if (distance >= bestDistance) {
        continue;
      }
      bestDistance = distance;
      nameOut = poiName;
      zoneOut = poiZone;
    }
    if (distanceOut) {
      *distanceOut = bestDistance;
    }
  };

  if (zoneNameInOut.empty()) {
    std::string nearestLocationName;
    std::string nearestLocationZone;
    resolveNearestPoi(LOCATION, 48, nearestLocationName, nearestLocationZone, nullptr);
    if (!nearestLocationZone.empty() &&
        (inTownContext || IsLikelyWorldZoneName(nearestLocationZone))) {
      zoneNameInOut = nearestLocationZone;
    } else if (IsLikelyWorldZoneName(nearestLocationName)) {
      zoneNameInOut = nearestLocationName;
    }
  }

  // Town backfill is only valid while actually in a town context.
  if (inTownContext) {
    std::string nearestTownName;
    std::string nearestTownZone;
    float nearestTownDistance = 3.4e38f;
    resolveNearestPoi(TOWN, 24, nearestTownName, nearestTownZone, &nearestTownDistance);

    float currentTownDistance = 3.4e38f;
    if (currentTown && (uintptr_t)currentTown > 0x1000) {
      RootObjectBase *townBase = (RootObjectBase *)currentTown;
      if (townBase && (uintptr_t)townBase > 0x1000) {
        try {
          currentTownDistance = npc->getPosition().distance(townBase->getPosition());
        } catch (...) {
          currentTownDistance = 3.4e38f;
        }
      }
    }

    bool shouldAdoptNearestTown = false;
    if (!nearestTownName.empty()) {
      std::string nearestTownMappedRegion =
          ResolveMappedRegionFromTownName(nearestTownName);
      bool nearestTownLooksValid =
          !nearestTownZone.empty() || !nearestTownMappedRegion.empty();
      if (townNameInOut.empty()) {
        // Only trust nearest-town labels that can map to a known region/zone.
        shouldAdoptNearestTown = nearestTownLooksValid;
      } else if (!IsUnknownToken(townNameInOut)) {
        std::string currentTownLower = townNameInOut;
        std::string nearestTownLower = nearestTownName;
        std::transform(currentTownLower.begin(), currentTownLower.end(),
                       currentTownLower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        std::transform(nearestTownLower.begin(), nearestTownLower.end(),
                       nearestTownLower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        const bool namesDiffer = (currentTownLower != nearestTownLower);
        if (namesDiffer && nearestTownLooksValid && nearestTownDistance < 3.4e38f &&
            currentTownDistance < 3.4e38f) {
          // If current-town pointer is farther than nearest town by a meaningful
          // margin, treat current pointer as stale and snap to nearest town.
          if ((currentTownDistance - nearestTownDistance) > 180.0f) {
            shouldAdoptNearestTown = true;
          }
        }
      }
    }

    if (shouldAdoptNearestTown) {
      townNameInOut = nearestTownName;
      if (!nearestTownZone.empty()) {
        zoneNameInOut = nearestTownZone;
      }
    } else if (zoneNameInOut.empty() && !nearestTownZone.empty()) {
      zoneNameInOut = nearestTownZone;
    }
  }
}

float Clamp01(float value) {
  if (value != value) { // NaN guard
    return 0.5f;
  }
  if (value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

std::string CrimeEnumToLabel(CrimeEnum crime) {
  switch (crime) {
  case CRIME_ENSLAVING:
    return "enslaving";
  case CRIME_LOCKPICKING:
    return "lockpicking";
  case CRIME_STEALING:
    return "stealing";
  case CRIME_MURDER:
    return "murder";
  case CRIME_ASSAULT:
    return "assault";
  case CRIME_ASSAULT_VIP:
    return "assault_vip";
  case CRIME_SLAVE_FREEING:
    return "slave_freeing";
  case CRIME_SMUGGLING:
    return "smuggling";
  case CRIME_TERRORISM:
    return "terrorism";
  case CRIME_LOOTING:
    return "looting";
  case CRIME_TRESSPASSING:
    return "trespassing";
  case CRIME_ESCAPE_PRISON:
    return "escape_prison";
  case CRIME_FENCING:
    return "fencing";
  case CRIME_FARM_EATING:
    return "farm_eating";
  case CRIME_KIDNAPPING:
    return "kidnapping";
  case CRIME_UNIFORM_THEFT:
    return "uniform_theft";
  default:
    return "";
  }
}

std::string ResolveFactionName(Faction *faction) {
  if (!faction || (uintptr_t)faction <= 0x1000) {
    return "";
  }
  std::string name = "";
  try {
    name = faction->getName();
  } catch (...) {
    name = "";
  }
  if (!name.empty() && name != "Unknown") {
    return name;
  }
  if (faction->data && (uintptr_t)faction->data > 0x1000 &&
      !faction->data->name.empty()) {
    return faction->data->name;
  }
  if (faction->data && (uintptr_t)faction->data > 0x1000 &&
      !faction->data->stringID.empty()) {
    return faction->data->stringID;
  }
  return "";
}

std::string ResolveFactionId(Faction *faction, const std::string &fallbackName) {
  if (faction && (uintptr_t)faction > 0x1000 && faction->data &&
      (uintptr_t)faction->data > 0x1000 && !faction->data->stringID.empty()) {
    return faction->data->stringID;
  }
  return fallbackName;
}

std::string BuildBountyPayload(Character *character, int &totalBountyOut) {
  totalBountyOut = 0;
  if (!character || (uintptr_t)character <= 0x1000) {
    return "{}";
  }

  BountyManager *bountyManager = nullptr;
  try {
    bountyManager = &character->crimes;
  } catch (...) {
    bountyManager = nullptr;
  }
  if (!bountyManager || (uintptr_t)bountyManager <= 0x1000) {
    return "{}";
  }

  try {
    totalBountyOut = bountyManager->getTotalBounty();
  } catch (...) {
    totalBountyOut = 0;
  }
  if (totalBountyOut < 0) {
    totalBountyOut = 0;
  }

  int runningTotal = 0;
  int highestAmount = 0;
  std::string primaryFaction = "";
  std::string primaryFactionId = "";
  std::string factionsJson = "";
  int factionCount = 0;

  for (auto it = bountyManager->bounties.begin(); it != bountyManager->bounties.end();
       ++it) {
    Faction *faction = it->first;
    const Bounty &bounty = it->second;

    int amount = bounty.amount;
    if (amount < 0) {
      amount = 0;
    }
    unsigned int crimeMask = bounty.crimes;
    if (amount <= 0 && crimeMask == 0) {
      continue;
    }

    std::string factionName = ResolveFactionName(faction);
    std::string factionId = ResolveFactionId(faction, factionName);
    runningTotal += amount;
    if (amount > highestAmount) {
      highestAmount = amount;
      primaryFaction = factionName;
      primaryFactionId = factionId;
    }

    std::string crimesJson = "";
    int crimesCount = 0;
    for (int ci = (int)CRIME_ENSLAVING; ci < (int)CRIME_END; ++ci) {
      bool hasCrime = false;
      try {
        hasCrime = bounty.hasCrime((CrimeEnum)ci);
      } catch (...) {
        hasCrime = ((crimeMask & (1u << (unsigned int)ci)) != 0u);
      }
      if (!hasCrime) {
        continue;
      }
      std::string crimeLabel = CrimeEnumToLabel((CrimeEnum)ci);
      if (crimeLabel.empty()) {
        continue;
      }
      if (crimesCount > 0) {
        crimesJson += ",";
      }
      crimesJson += "\"" + EscapeJSON(crimeLabel) + "\"";
      crimesCount++;
    }

    int assignedTs = 0;
    try {
      assignedTs = (int)bounty.bountyAssignmentStartedTime.getTotalSeconds();
    } catch (...) {
      assignedTs = 0;
    }
    if (assignedTs < 0) {
      assignedTs = 0;
    }

    if (factionCount > 0) {
      factionsJson += ",";
    }
    factionsJson += "{";
    factionsJson += "\"faction\":\"" + EscapeJSON(factionName) + "\",";
    factionsJson += "\"faction_id\":\"" + EscapeJSON(factionId) + "\",";
    factionsJson += "\"amount\":" + ToString(amount) + ",";
    factionsJson += "\"crime_mask\":" + ToString((int)crimeMask) + ",";
    factionsJson += "\"claimed_once\":" +
                    std::string(bounty.bountyHasBeenClaimedOnce ? "true" : "false");
    if (assignedTs > 0) {
      factionsJson += ",\"assigned_game_ts\":" + ToString(assignedTs);
    }
    if (crimesCount > 0) {
      factionsJson += ",\"what_for\":[" + crimesJson + "]";
    }
    factionsJson += "}";
    factionCount++;
  }

  if (totalBountyOut <= 0 && runningTotal > 0) {
    totalBountyOut = runningTotal;
  }
  if (totalBountyOut < 0) {
    totalBountyOut = 0;
  }

  if (totalBountyOut <= 0 && factionCount == 0) {
    return "{}";
  }

  std::string json = "{";
  json += "\"total\":" + ToString(totalBountyOut) + ",";
  json += "\"factions\":[" + factionsJson + "]";
  if (!primaryFaction.empty()) {
    json += ",\"primary_faction\":\"" + EscapeJSON(primaryFaction) + "\"";
  }
  if (!primaryFactionId.empty()) {
    json += ",\"primary_faction_id\":\"" + EscapeJSON(primaryFactionId) + "\"";
  }
  json += "}";
  return json;
}

void CaptureAppearanceSnapshot(Character *character, bool &hasBeard,
                               bool &isShaved, bool &isFlayed,
                               float &heightNorm, float &ageNorm) {
  hasBeard = false;
  isShaved = false;
  isFlayed = false;
  heightNorm = 0.5f;
  ageNorm = 0.5f;
  if (!character || (uintptr_t)character <= 0x1000) {
    return;
  }

  try {
    ageNorm = Clamp01(character->getAge0to1());
  } catch (...) {
  }
  try {
    isShaved = character->isHeadShaven();
  } catch (...) {
  }

  AppearanceBase *appearance = nullptr;
  try {
    appearance = character->getAppearance();
  } catch (...) {
    appearance = nullptr;
  }
  if (!appearance || (uintptr_t)appearance <= 0x1000) {
    return;
  }

  try {
    heightNorm = Clamp01(appearance->getNormalisedCharacterHeight());
  } catch (...) {
  }
  try {
    if (appearance->isShaved()) {
      isShaved = true;
    }
  } catch (...) {
  }
  try {
    isFlayed = appearance->isFlayed();
  } catch (...) {
  }
  try {
    hasBeard = appearance->getAttachedEntity("beard") != nullptr;
  } catch (...) {
  }
}

bool PartAlreadyCaptured(
    const std::vector<MedicalSystem::HealthPartStatus *> &capturedParts,
    MedicalSystem::HealthPartStatus *candidate) {
  if (!candidate)
    return true;
  for (size_t i = 0; i < capturedParts.size(); ++i) {
    if (capturedParts[i] == candidate)
      return true;
  }
  return false;
}

std::string BuildMedicalPayload(Character *character) {
  if (!character || (uintptr_t)character < 0x1000)
    return "{}";

  MedicalSystem *med = character->getMedical();
  if (!med || (uintptr_t)med < 0x1000)
    return "{}";

  float bloodValue = med->blood;
  float bloodMaxValue = med->getMaxBlood();
  if (bloodMaxValue <= 0.0f) {
    bloodMaxValue = 100.0f;
  }
  if (bloodValue < 0.0f) {
    bloodValue = 0.0f;
  }
  if (bloodValue > bloodMaxValue) {
    bloodValue = bloodMaxValue;
  }

  std::string json = "{";
  json += "\"blood\": " + ToString((int)bloodValue) + ",";
  json += "\"max_blood\": " + ToString((int)bloodMaxValue) + ",";
  json += "\"blood_rate\": " + ToString(med->currentBleedRate) + ",";

  float hungerVal = (300.0f - med->hunger) + med->fed;
  if (hungerVal < 0)
    hungerVal = 0;
  json += "\"hunger\": " + ToString((int)hungerVal) + ",";
  json += "\"hunger_max\": 300,";
  json += "\"is_unconscious\": " + std::string(med->unconcious ? "true" : "false") +
          ",";

  json += "\"limbs\": {";
  std::vector<MedicalSystem::HealthPartStatus *> capturedParts;
  auto addPart = [&](const std::string &name,
                     MedicalSystem::HealthPartStatus *part) {
    json += "\"" + name + "\": " + ToString(part ? (int)part->flesh : 100) + ",";
    json += "\"" + name + "_max\": " +
            ToString(part ? (int)part->maxHealth() : 100) + ",";
    if (part)
      capturedParts.push_back(part);
  };

  addPart("head", med->getPart((unsigned __int64)0));
  addPart("stomach", med->getPart((unsigned __int64)1));
  addPart("left_arm", med->leftArm);
  addPart("right_arm", med->rightArm);
  addPart("left_leg", med->leftLeg);
  addPart("right_leg", med->rightLeg);

  auto isLimbMissing = [&](RobotLimbs::Limb limb) -> bool {
    int limbState = (int)LIMB_ORIGINAL;
    bool limbPartPresent = true;
    try {
      limbState = (int)med->getLimbState(limb);
    } catch (...) {
      limbState = (int)LIMB_ORIGINAL;
    }
    try {
      MedicalSystem::HealthPartStatus *limbPart = med->getPart(limb);
      limbPartPresent = limbPart && (uintptr_t)limbPart >= 0x1000;
    } catch (...) {
      limbPartPresent = true;
    }
    bool limbLostState =
        (limbState == (int)LIMB_STUMP || limbState == (int)LIMB_CRUSHED);
    return limbLostState || !limbPartPresent;
  };
  json += "\"left_arm_missing\": " +
          std::string(isLimbMissing(RobotLimbs::LEFT_ARM) ? "1" : "0") + ",";
  json += "\"right_arm_missing\": " +
          std::string(isLimbMissing(RobotLimbs::RIGHT_ARM) ? "1" : "0") + ",";
  json += "\"left_leg_missing\": " +
          std::string(isLimbMissing(RobotLimbs::LEFT_LEG) ? "1" : "0") + ",";
  json += "\"right_leg_missing\": " +
          std::string(isLimbMissing(RobotLimbs::RIGHT_LEG) ? "1" : "0") + ",";

  int extraHead = 0;
  int extraTorso = 0;
  int extraArm = 0;
  int extraLeg = 0;
  for (uint32_t i = 0; i < med->anatomy.size(); ++i) {
    MedicalSystem::HealthPartStatus *part = med->anatomy[i];
    if (PartAlreadyCaptured(capturedParts, part))
      continue;

    std::string name = "part_" + ToString((int)i);
    if (part->whatAmI == MedicalSystem::HealthPartStatus::PART_HEAD) {
      extraHead++;
      name = "head_extra_" + ToString(extraHead);
    } else if (part->whatAmI == MedicalSystem::HealthPartStatus::PART_TORSO) {
      extraTorso++;
      name = "torso_extra_" + ToString(extraTorso);
    } else if (part->whatAmI == MedicalSystem::HealthPartStatus::PART_ARM) {
      if (part->side == SIDE_LEFT) {
        extraArm++;
        name = "left_arm_extra_" + ToString(extraArm);
      } else if (part->side == SIDE_RIGHT) {
        extraArm++;
        name = "right_arm_extra_" + ToString(extraArm);
      } else {
        extraArm++;
        name = "arm_extra_" + ToString(extraArm);
      }
    } else if (part->whatAmI == MedicalSystem::HealthPartStatus::PART_LEG) {
      if (part->side == SIDE_LEFT) {
        extraLeg++;
        name = "left_leg_extra_" + ToString(extraLeg);
      } else if (part->side == SIDE_RIGHT) {
        extraLeg++;
        name = "right_leg_extra_" + ToString(extraLeg);
      } else {
        extraLeg++;
        name = "leg_extra_" + ToString(extraLeg);
      }
    }

    addPart(name, part);
  }

  if (json.back() == ',')
    json.pop_back();
  json += "}";
  json += "}";
  return json;
}

std::string BuildRoboticLimbPayload(Character *character, bool &hasRoboticLimbs) {
  hasRoboticLimbs = false;
  if (!character || (uintptr_t)character < 0x1000) {
    return "[]";
  }

  MedicalSystem *med = character->getMedical();
  if (!med || (uintptr_t)med < 0x1000) {
    return "[]";
  }

  std::vector<std::string> roboticParts;
  std::vector<MedicalSystem::HealthPartStatus *> capturedParts;
  auto maybeAddRoboticPart = [&](const std::string &name,
                                 MedicalSystem::HealthPartStatus *part) {
    if (!part || (uintptr_t)part <= 0x1000) {
      return;
    }
    bool isRobotic = false;
    try {
      isRobotic = part->isRobotic();
    } catch (...) {
      isRobotic = (part->robotLimb != NULL);
    }
    if (!isRobotic) {
      return;
    }
    roboticParts.push_back(name);
    capturedParts.push_back(part);
    hasRoboticLimbs = true;
  };

  maybeAddRoboticPart("head", med->getPart((unsigned __int64)0));
  maybeAddRoboticPart("stomach", med->getPart((unsigned __int64)1));
  maybeAddRoboticPart("left_arm", med->leftArm);
  maybeAddRoboticPart("right_arm", med->rightArm);
  maybeAddRoboticPart("left_leg", med->leftLeg);
  maybeAddRoboticPart("right_leg", med->rightLeg);

  int extraHead = 0;
  int extraTorso = 0;
  int extraArm = 0;
  int extraLeg = 0;
  for (uint32_t i = 0; i < med->anatomy.size(); ++i) {
    MedicalSystem::HealthPartStatus *part = med->anatomy[i];
    if (PartAlreadyCaptured(capturedParts, part)) {
      continue;
    }
    if (!part || (uintptr_t)part <= 0x1000) {
      continue;
    }

    std::string name = "part_" + ToString((int)i);
    if (part->whatAmI == MedicalSystem::HealthPartStatus::PART_HEAD) {
      extraHead++;
      name = "head_extra_" + ToString(extraHead);
    } else if (part->whatAmI == MedicalSystem::HealthPartStatus::PART_TORSO) {
      extraTorso++;
      name = "torso_extra_" + ToString(extraTorso);
    } else if (part->whatAmI == MedicalSystem::HealthPartStatus::PART_ARM) {
      if (part->side == SIDE_LEFT) {
        extraArm++;
        name = "left_arm_extra_" + ToString(extraArm);
      } else if (part->side == SIDE_RIGHT) {
        extraArm++;
        name = "right_arm_extra_" + ToString(extraArm);
      } else {
        extraArm++;
        name = "arm_extra_" + ToString(extraArm);
      }
    } else if (part->whatAmI == MedicalSystem::HealthPartStatus::PART_LEG) {
      if (part->side == SIDE_LEFT) {
        extraLeg++;
        name = "left_leg_extra_" + ToString(extraLeg);
      } else if (part->side == SIDE_RIGHT) {
        extraLeg++;
        name = "right_leg_extra_" + ToString(extraLeg);
      } else {
        extraLeg++;
        name = "leg_extra_" + ToString(extraLeg);
      }
    }

    maybeAddRoboticPart(name, part);
  }

  std::string json = "[";
  for (size_t i = 0; i < roboticParts.size(); ++i) {
    if (i > 0) {
      json += ",";
    }
    json += "\"" + EscapeJSON(roboticParts[i]) + "\"";
  }
  json += "]";
  return json;
}

std::string BuildStatsPayload(Character *character) {
  if (!character || (uintptr_t)character < 0x1000) {
    return "{}";
  }

  CharStats *stats = character->getStats();
  if (!stats || (uintptr_t)stats < 0x1000) {
    return "{}";
  }

  std::string json = "{";
  auto addStat = [&](const std::string &key, int value) {
    json += "\"" + key + "\": " + ToString(value) + ",";
  };
  auto addSkill = [&](const std::string &key, int statIndex) {
    addStat(key, (int)stats->getStat((StatsEnumerated)statIndex, false));
  };

  // Core attributes
  addStat("strength", (int)stats->_strength);
  addStat("dexterity", (int)stats->_dexterity);
  addStat("toughness", (int)stats->_toughness);
  addStat("perception", (int)stats->perception);

  // Combat + weapon skills
  addSkill("melee_attack", STAT_MELEE_ATTACK);
  addSkill("melee_defence", STAT_MELEE_DEFENCE);
  addSkill("dodge", STAT_DODGE);
  addSkill("martial_arts", STAT_MARTIALARTS);
  addSkill("katanas", STAT_KATANAS);
  addSkill("sabres", STAT_SABRES);
  addSkill("hackers", STAT_HACKERS);
  addSkill("heavy_weapons", STAT_HEAVYWEAPONS);
  addSkill("blunt", STAT_BLUNT);
  addSkill("polearms", STAT_POLEARMS);
  addSkill("crossbows", STAT_CROSSBOWS);
  addSkill("turrets", STAT_TURRETS);
  addSkill("athletics", STAT_ATHLETICS);
  addSkill("stealth", STAT_STEALTH);
  addSkill("assassination", STAT_ASSASSINATION);
  addSkill("swimming", STAT_SWIMMING);
  addSkill("survival", STAT_SURVIVAL);

  // Utility / trade / crafting skills
  addSkill("labouring", STAT_LABOURING);
  addSkill("thieving", STAT_THIEVING);
  addSkill("lockpicking", STAT_LOCKPICKING);
  addSkill("medic", STAT_MEDIC);
  addSkill("science", STAT_SCIENCE);
  addSkill("engineering", STAT_ENGINEERING);
  addSkill("robotics", STAT_ROBOTICS);
  addSkill("farming", STAT_FARMING);
  addSkill("cooking", STAT_COOKING);
  addSkill("weapon_smithing", STAT_SMITHING_WEAPON);
  addSkill("armour_smithing", STAT_SMITHING_ARMOUR);
  addSkill("bow_smithing", STAT_SMITHING_BOW);
  addSkill("hive_medic", STAT_HIVEMEDIC);
  addSkill("vet", STAT_VET);

  if (json.back() == ',')
    json.pop_back();
  json += "}";
  return json;
}

bool IsResourcePermajobTask(TaskType task) {
  return task == JOB_KEEP_EVERYTHING_RUNNING || task == DELIVER_RESOURCES ||
         task == COLLECT_OUTPUT_RESOURCE || task == FILL_MACHINE ||
         task == OPERATE_MACHINERY || task == OPERATE_AUTOMATIC_MACHINERY ||
         task == GET_RID_OF_RESOURCES_IN_MY_INVENTORY ||
         task == DITCH_ALL_RESOURCES || task == AUTO_LABOURING_MINES;
}

bool IsMedicPermajobTask(TaskType task) {
  return task == JOB_MEDIC || task == FIRST_AID_ORDER ||
         task == FIRST_AID_ROBOT || task == SPLINT_ORDER ||
         task == SPLINT_JOB || task == JOB_REPAIR_ROBOT;
}

struct ActivitySnapshot {
  bool isMoving;
  bool isRunning;
  bool isSneaking;
  bool isInCombat;
  bool hasAttackTarget;
  bool isAttacking;
  float movementSpeed;
  std::string attackTargetName;
  std::string currentAction;

  ActivitySnapshot()
      : isMoving(false), isRunning(false), isSneaking(false), isInCombat(false),
        hasAttackTarget(false), isAttacking(false), movementSpeed(0.0f),
        attackTargetName(""), currentAction("idle") {}
};

void CaptureActivitySnapshot(Character *character, const std::string &characterState,
                             ActivitySnapshot &out) {
  out = ActivitySnapshot();
  if (!character || (uintptr_t)character < 0x1000) {
    if (!characterState.empty()) {
      out.currentAction = characterState;
    }
    return;
  }

  try {
    out.movementSpeed = character->getMovementSpeed();
    if (out.movementSpeed < 0.0f) {
      out.movementSpeed = 0.0f;
    }
  } catch (...) {
    out.movementSpeed = 0.0f;
  }

  try {
    out.isSneaking = character->isStealthMode();
  } catch (...) {
    out.isSneaking = false;
  }

  try {
    out.isInCombat = character->isInCombatMode(true, true);
  } catch (...) {
    out.isInCombat = false;
  }

  try {
    hand attackTarget = character->getAttackTarget();
    if (attackTarget.isValid() && !attackTarget.isNull()) {
      out.hasAttackTarget = true;
      Character *target = attackTarget.getCharacter();
      if (target && (uintptr_t)target > 0x1000) {
        out.attackTargetName = TrimCopy(target->getName());
      }
    }
  } catch (...) {
    out.hasAttackTarget = false;
  }

  bool moveStateResolved = false;
  try {
    CharMovement *movement = character->getMovement();
    if (movement && (uintptr_t)movement > 0x1000) {
      out.isRunning = movement->isRunning();
      bool isIdle = movement->isIdle();
      out.isMoving = out.isRunning || !isIdle || out.movementSpeed > 0.06f;
      moveStateResolved = true;
    }
  } catch (...) {
    moveStateResolved = false;
  }
  if (!moveStateResolved) {
    out.isRunning = false;
    out.isMoving = out.movementSpeed > 0.18f;
  }

  out.isAttacking = out.isInCombat && out.hasAttackTarget;
  if (characterState == "dead") {
    out.currentAction = "dead";
  } else if (characterState == "unconscious") {
    out.currentAction = "unconscious";
  } else if (characterState == "imprisoned") {
    out.currentAction = "imprisoned";
  } else if (characterState == "enslaved" || characterState == "escaped-slave") {
    out.currentAction = characterState;
  } else if (out.isAttacking) {
    out.currentAction = "attacking";
  } else if (out.isInCombat) {
    out.currentAction = "combat";
  } else if (out.isSneaking) {
    out.currentAction = "sneaking";
  } else if (out.isRunning) {
    out.currentAction = "running";
  } else if (out.isMoving) {
    out.currentAction = "moving";
  } else {
    out.currentAction = "idle";
  }
}

std::string BuildStandingOrderPayload(Character *character) {
  if (!character || (uintptr_t)character < 0x1000) {
    return "{}";
  }

  bool block = false;
  bool hold = false;
  bool passive = false;
  bool ranged = false;
  bool taunt = false;
  bool sneak = false;
  bool jobsEnabled = false;
  bool resource = false;
  bool medic = false;
  std::string jobListJson = "[]";

  try {
    block = character->getStandingOrder(MessageForB::M_SET_ORDER_DEF) ||
            character->getStandingOrder(MessageForB::M_SET_ORDER_DEFENSIVE_COMBAT);
    hold = character->getStandingOrder(MessageForB::M_SET_ORDER_HOLD);
    passive = character->getStandingOrder(MessageForB::M_SET_ORDER_PASSIVE);
    ranged = character->getStandingOrder(MessageForB::M_SET_ORDER_RANGED);
    taunt = character->getStandingOrder(MessageForB::M_SET_ORDER_TAUNT);
    sneak = character->isStealthMode();

    int jobCount = character->getPermajobCount();
    jobsEnabled = jobCount > 0;
    if (jobCount > 0) {
      jobListJson = "[";
      for (int i = 0; i < jobCount; ++i) {
        if (i > 0) {
          jobListJson += ",";
        }
        TaskType task = character->getPermajob(i);
        if (IsResourcePermajobTask(task)) {
          resource = true;
        }
        if (IsMedicPermajobTask(task)) {
          medic = true;
        }
        std::string taskName = character->getPermajobName(i);
        if (taskName.empty()) {
          taskName = ToString((int)task);
        }
        jobListJson += "\"" + EscapeJSON(taskName) + "\"";
      }
      jobListJson += "]";
    }
  } catch (...) {
    return "{}";
  }

  std::string json = "{";
  json += "\"block\":" + std::string(block ? "true" : "false") + ",";
  json += "\"hold\":" + std::string(hold ? "true" : "false") + ",";
  json += "\"passive\":" + std::string(passive ? "true" : "false") + ",";
  json += "\"jobs\":" + std::string(jobsEnabled ? "true" : "false") + ",";
  json += "\"job_list\":" + jobListJson + ",";
  json += "\"ranged\":" + std::string(ranged ? "true" : "false") + ",";
  json += "\"taunt\":" + std::string(taunt ? "true" : "false") + ",";
  json += "\"sneak\":" + std::string(sneak ? "true" : "false") + ",";
  json += "\"resource\":" + std::string(resource ? "true" : "false") + ",";
  json += "\"medic\":" + std::string(medic ? "true" : "false");
  json += "}";
  return json;
}
} // namespace

std::string BuildIdentityBootstrapContext(Character *npc) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return "{}";
  }

  std::string npcName = npc->getName();

  RaceData *race = npc->getRace() ? npc->getRace() : npc->myRace;
  std::string raceName = "Unknown";
  bool raceIsRobot = false;
  if (race && (uintptr_t)race > 0x1000) {
    raceIsRobot = race->robot;
    if (race->data && !race->data->name.empty()) {
      raceName = race->data->name;
    } else if (race->data && !race->data->stringID.empty()) {
      raceName = race->data->stringID;
    }
  }

  Faction *faction = npc->getFaction() ? npc->getFaction() : npc->owner;
  std::string factionName = "Neutral";
  std::string factionID = "Neutral";
  if (faction && (uintptr_t)faction > 0x1000) {
    std::string fn = faction->getName();
    if (!fn.empty() && fn != "Unknown") {
      factionName = fn;
    } else if (faction->data && !faction->data->name.empty()) {
      factionName = faction->data->name;
    } else if (faction->data && !faction->data->stringID.empty()) {
      factionName = faction->data->stringID;
    }

    if (faction->data && !faction->data->stringID.empty()) {
      factionID = faction->data->stringID;
    } else {
      factionID = factionName;
    }
  }

  std::string gender = npc->isFemale() ? "female" : "male";
  bool hasBeard = false;
  bool isShaved = false;
  bool isFlayed = false;
  float heightNorm = 0.5f;
  float ageNorm = 0.5f;
  CaptureAppearanceSnapshot(npc, hasBeard, isShaved, isFlayed, heightNorm,
                            ageNorm);
  HornSliderSnapshot hornSnapshot;
  CaptureHornSliderSnapshot(npc, hornSnapshot);
  std::string identityFaction = GetIdentityFaction(npc);
  std::string storageId = GetStorageIDFor(npc, npcName, identityFaction);
  std::string stats = BuildStatsPayload(npc);
  std::string medical = BuildMedicalPayload(npc);
  std::string orders = BuildStandingOrderPayload(npc);
  std::string equipment = GetVisibleEquipment(npc);
  int bountyTotal = 0;
  std::string bountyPayload = BuildBountyPayload(npc, bountyTotal);
  ActivitySnapshot activity;
  CaptureActivitySnapshot(npc, "", activity);

  int gameTs = 0;
  GameWorld *world = GetWorldSafe();
  if (world && (uintptr_t)world > 0x1000) {
    TimeOfDay tod = world->getTimeStamp_inGameHours();
    gameTs = (int)tod.getTotalSeconds();
    if (gameTs < 0) {
      gameTs = 0;
    }
  }

  std::map<unsigned int, std::string> carriedByNameBySerial;
  if (world && (uintptr_t)world > 0x1000) {
    BuildCarriedByNameLookup(world, carriedByNameBySerial);
  }

  unsigned int npcSerial = 0;
  try {
    npcSerial = npc->getHandle().serial;
  } catch (...) {
    npcSerial = 0;
  }

  bool isBeingCarried = false;
  std::string carriedByName = "";
  if (npcSerial != 0) {
    std::map<unsigned int, std::string>::const_iterator carrierIt =
        carriedByNameBySerial.find(npcSerial);
    if (carrierIt != carriedByNameBySerial.end()) {
      isBeingCarried = true;
      carriedByName = TrimCopy(carrierIt->second);
    }
  }

  bool isCarrying = false;
  std::string carryingTargetName = "";
  try {
    isCarrying = npc->isCarryingSomething && npc->carryingObject.isValid();
    if (isCarrying) {
      carryingTargetName = ResolveCarryTargetNameSafe(npc->carryingObject);
    }
  } catch (...) {
    isCarrying = false;
    carryingTargetName = "";
  }

  std::string json = "{";
  if (gameTs > 0) {
    json += "\"game_ts\":" + ToString(gameTs) + ",";
  }
  json += "\"race\":\"" + EscapeJSON(raceName) + "\",";
  json += "\"race_is_robot\":" +
          std::string(raceIsRobot ? "true" : "false") + ",";
  json += "\"faction\":\"" + EscapeJSON(factionName) + "\",";
  json += "\"factionID\":\"" + EscapeJSON(factionID) + "\",";
  json += "\"faction_id\":\"" + EscapeJSON(factionID) + "\",";
  json += "\"gender\":\"" + EscapeJSON(gender) + "\",";
  json += "\"current_action\":\"" + EscapeJSON(activity.currentAction) +
          "\",";
  json += "\"is_in_combat\":" +
          std::string(activity.isInCombat ? "true" : "false") + ",";
  json += "\"is_attacking\":" +
          std::string(activity.isAttacking ? "true" : "false") + ",";
  if (!activity.attackTargetName.empty()) {
    json += "\"attack_target\":\"" + EscapeJSON(activity.attackTargetName) +
            "\",";
  }
  json += "\"has_beard\": " + std::string(hasBeard ? "true" : "false") + ",";
  json += "\"is_shaved\": " + std::string(isShaved ? "true" : "false") + ",";
  json += "\"is_flayed\": " + std::string(isFlayed ? "true" : "false") + ",";
  json += "\"height_norm\": " + ToString(heightNorm) + ",";
  json += "\"age_norm\": " + ToString(ageNorm) + ",";
  if (hornSnapshot.hasAny) {
    json += "\"horn_sliders\":" + BuildHornSliderPayload(hornSnapshot) + ",";
  }
  if (!equipment.empty()) {
    json += "\"equipment\":\"" + EscapeJSON(equipment) + "\",";
  }
  if (bountyTotal > 0) {
    json += "\"bounty\":" + ToString(bountyTotal) + ",";
  }
  if (bountyPayload != "{}") {
    json += "\"bounty_info\":" + bountyPayload + ",";
  }
  if (stats != "{}") {
    json += "\"stats\":" + stats + ",";
  }
  if (medical != "{}") {
    json += "\"medical\":" + medical + ",";
  }
  if (orders != "{}") {
    json += "\"orders\":" + orders + ",";
  }
  json += "\"is_carrying\": " + std::string(isCarrying ? "true" : "false") +
          ",";
  if (!carryingTargetName.empty()) {
    json += "\"carrying_target_name\":\"" + EscapeJSON(carryingTargetName) +
            "\",";
  }
  json += "\"is_being_carried\": " +
          std::string(isBeingCarried ? "true" : "false") + ",";
  if (!carriedByName.empty()) {
    json += "\"carried_by_name\":\"" + EscapeJSON(carriedByName) + "\",";
  }
  if (!storageId.empty()) {
    json += "\"storage_id\":\"" + EscapeJSON(storageId) + "\",";
  }
  if (json.back() == ',') {
    json.pop_back();
  }
  json += "}";
  return json;
}

void GetAllInventoryItemsFromInventory(Inventory *inv,
                                       std::vector<Item *> &outItems) {
  if (!inv || (uintptr_t)inv <= 0x1000)
    return;

  lektor<InventorySection *> &sections = inv->sectionsInSearchOrder;
  for (uint32_t s = 0; s < sections.size(); ++s) {
    InventorySection *sect = sections[s];
    if (sect) {
      const Ogre::vector<InventorySection::SectionItem>::type &items =
          sect->getItems();
      for (uint32_t i = 0; i < items.size(); ++i) {
        if (items[i].item)
          outItems.push_back(items[i].item);
      }
    }
  }
}

void GetAllCharacterItems(Character *npc, std::vector<Item *> &outItems) {
  if (!npc)
    return;
  Inventory *inv = npc->getInventory();
  if (!inv)
    return;

  GetAllInventoryItemsFromInventory(inv, outItems);

  ContainerItem *backpack = npc->hasABackpackOn();
  if (backpack && backpack->inventory) {
    GetAllInventoryItemsFromInventory(backpack->inventory, outItems);
  }
}

bool BuildInventorySnapshotFromInventory(Inventory *inv,
                                         std::string &inventoryJsonOut,
                                         int &itemCountOut) {
  inventoryJsonOut = "[]";
  itemCountOut = 0;
  if (!inv || (uintptr_t)inv <= 0x1000) {
    return false;
  }

  std::vector<Item *> rawItems;
  try {
    GetAllInventoryItemsFromInventory(inv, rawItems);
  } catch (...) {
    return false;
  }
  if (rawItems.size() > 600) {
    rawItems.resize(600);
  }

  struct AggregatedInventoryItem {
    std::string name;
    std::string itemId;
    std::string description;
    std::string quality;
    int qualityLevel;
    std::string weaponModel;
    int count;
    int valueEach;
  };

  std::map<std::string, AggregatedInventoryItem> aggregated;
  for (uint32_t i = 0; i < rawItems.size(); ++i) {
    Item *item = rawItems[i];
    if (!item || (uintptr_t)item <= 0x1000) {
      continue;
    }

    std::string itemName = "";
    std::string itemId = "";
    std::string itemDescription = "";
    std::string itemQuality = "";
    int itemQualityLevel = -1;
    std::string itemWeaponModel = "";
    int itemCount = 1;
    int itemValueEach = 0;
    try {
      itemName = TrimCopy(item->getName());
      if (item->data && (uintptr_t)item->data > 0x1000 &&
          !item->data->stringID.empty()) {
        itemId = TrimCopy(item->data->stringID);
      }
      if (item->data && (uintptr_t)item->data > 0x1000) {
        itemDescription = ResolveItemDescription(item->data);
      }
      TryExtractArmourQuality(item, itemQualityLevel, itemQuality);
      TryExtractWeaponModel(item, itemWeaponModel);
      itemCount = item->quantity;
      itemValueEach = item->getValueSingle(false);
    } catch (...) {
      continue;
    }

    if (itemName.empty()) {
      continue;
    }
    if (itemCount <= 0) {
      itemCount = 1;
    }
    if (itemValueEach < 0) {
      itemValueEach = 0;
    }

    std::string key = itemId.empty() ? itemName : itemId;
    for (size_t ci = 0; ci < key.length(); ++ci) {
      key[ci] = static_cast<char>(tolower((unsigned char)key[ci]));
    }
    std::string qualityKey =
        itemQualityLevel >= 0
            ? ToString(itemQualityLevel)
            : (itemQuality.empty() ? std::string("none") : itemQuality);
    for (size_t qi = 0; qi < qualityKey.length(); ++qi) {
      qualityKey[qi] = static_cast<char>(tolower((unsigned char)qualityKey[qi]));
    }
    key += "|q:" + qualityKey;
    if (!itemWeaponModel.empty()) {
      std::string weaponModelKey = itemWeaponModel;
      for (size_t wi = 0; wi < weaponModelKey.length(); ++wi) {
        weaponModelKey[wi] =
            static_cast<char>(tolower((unsigned char)weaponModelKey[wi]));
      }
      key += "|wm:" + weaponModelKey;
    }

    auto it = aggregated.find(key);
    if (it == aggregated.end()) {
      AggregatedInventoryItem entry;
      entry.name = itemName;
      entry.itemId = itemId;
      entry.description = itemDescription;
      entry.quality = itemQuality;
      entry.qualityLevel = itemQualityLevel;
      entry.weaponModel = itemWeaponModel;
      entry.count = itemCount;
      entry.valueEach = itemValueEach;
      aggregated[key] = entry;
    } else {
      it->second.count += itemCount;
      if (it->second.itemId.empty() && !itemId.empty()) {
        it->second.itemId = itemId;
      }
      if (it->second.description.empty() && !itemDescription.empty()) {
        it->second.description = itemDescription;
      }
      if (it->second.valueEach <= 0 && itemValueEach > 0) {
        it->second.valueEach = itemValueEach;
      }
      if (it->second.quality.empty() && !itemQuality.empty()) {
        it->second.quality = itemQuality;
      }
      if (it->second.qualityLevel < 0 && itemQualityLevel >= 0) {
        it->second.qualityLevel = itemQualityLevel;
      }
      if (it->second.weaponModel.empty() && !itemWeaponModel.empty()) {
        it->second.weaponModel = itemWeaponModel;
      }
    }
  }

  if (aggregated.empty()) {
    return true;
  }

  std::string json = "[";
  int outputCount = 0;
  for (auto it = aggregated.begin(); it != aggregated.end(); ++it) {
    const AggregatedInventoryItem &entry = it->second;
    if (outputCount > 0) {
      json += ",";
    }
    json += "{\"name\":\"" + EscapeJSON(entry.name) + "\",";
    json += "\"count\":" + ToString(entry.count) + ",";
    json += "\"equipped\":false";
    if (!entry.itemId.empty()) {
      json += ",\"item_id\":\"" + EscapeJSON(entry.itemId) + "\"";
    }
    if (!entry.description.empty()) {
      json += ",\"description\":\"" + EscapeJSON(entry.description) + "\"";
    }
    if (entry.valueEach > 0) {
      json += ",\"value_each\":" + ToString(entry.valueEach);
    }
    if (entry.qualityLevel >= 0) {
      json += ",\"quality_level\":" + ToString(entry.qualityLevel);
    }
    if (!entry.quality.empty()) {
      json += ",\"quality\":\"" + EscapeJSON(entry.quality) + "\"";
    }
    if (!entry.weaponModel.empty()) {
      json += ",\"weapon_model\":\"" + EscapeJSON(entry.weaponModel) + "\"";
    }
    json += "}";

    ++outputCount;
    if (outputCount >= 120) {
      break;
    }
  }
  json += "]";

  inventoryJsonOut = json;
  itemCountOut = outputCount;
  return true;
}

bool BuildInventorySnapshot(Character *npc, std::string &inventoryJsonOut,
                            std::string &inventoryHashOut, int &itemCountOut) {
  inventoryJsonOut = "[]";
  inventoryHashOut = "";
  itemCountOut = 0;
  if (!npc || (uintptr_t)npc < 0x1000) {
    return false;
  }

  struct AggregatedInventoryItem {
    std::string name;
    std::string itemId;
    std::string description;
    std::string quality;
    int qualityLevel;
    std::string weaponModel;
    int count;
    bool equipped;
    int valueEach;
  };

  std::vector<Item *> rawItems;
  try {
    GetAllCharacterItems(npc, rawItems);
  } catch (...) {
    return false;
  }

  std::map<std::string, AggregatedInventoryItem> aggregated;
  for (uint32_t i = 0; i < rawItems.size(); ++i) {
    Item *item = rawItems[i];
    if (!item || (uintptr_t)item < 0x1000) {
      continue;
    }

    std::string itemName = "";
    std::string itemId = "";
    std::string itemDescription = "";
    std::string itemQuality = "";
    int itemQualityLevel = -1;
    std::string itemWeaponModel = "";
    int itemCount = 1;
    bool itemEquipped = false;
    int itemValueEach = 0;
    try {
      itemName = TrimCopy(item->getName());
      if (item->data && (uintptr_t)item->data > 0x1000 &&
          !item->data->stringID.empty()) {
        itemId = TrimCopy(item->data->stringID);
      }
      if (item->data && (uintptr_t)item->data > 0x1000) {
        itemDescription = ResolveItemDescription(item->data);
      }
      TryExtractArmourQuality(item, itemQualityLevel, itemQuality);
      TryExtractWeaponModel(item, itemWeaponModel);
      itemCount = item->quantity;
      itemEquipped = item->isEquipped;
      itemValueEach = item->getValueSingle(false);
    } catch (...) {
      continue;
    }

    if (itemName.empty()) {
      continue;
    }
    if (itemCount <= 0) {
      itemCount = 1;
    }
    if (itemValueEach < 0) {
      itemValueEach = 0;
    }

    std::string key = itemId.empty() ? itemName : itemId;
    for (size_t ci = 0; ci < key.length(); ++ci) {
      key[ci] = static_cast<char>(tolower((unsigned char)key[ci]));
    }
    std::string qualityKey =
        itemQualityLevel >= 0
            ? ToString(itemQualityLevel)
            : (itemQuality.empty() ? std::string("none") : itemQuality);
    for (size_t qi = 0; qi < qualityKey.length(); ++qi) {
      qualityKey[qi] = static_cast<char>(tolower((unsigned char)qualityKey[qi]));
    }
    key += "|q:" + qualityKey;
    if (!itemWeaponModel.empty()) {
      std::string weaponModelKey = itemWeaponModel;
      for (size_t wi = 0; wi < weaponModelKey.length(); ++wi) {
        weaponModelKey[wi] =
            static_cast<char>(tolower((unsigned char)weaponModelKey[wi]));
      }
      key += "|wm:" + weaponModelKey;
    }
    key += "|" + std::string(itemEquipped ? "1" : "0");

    auto it = aggregated.find(key);
    if (it == aggregated.end()) {
      AggregatedInventoryItem entry;
      entry.name = itemName;
      entry.itemId = itemId;
      entry.description = itemDescription;
      entry.quality = itemQuality;
      entry.qualityLevel = itemQualityLevel;
      entry.weaponModel = itemWeaponModel;
      entry.count = itemCount;
      entry.equipped = itemEquipped;
      entry.valueEach = itemValueEach;
      aggregated[key] = entry;
    } else {
      it->second.count += itemCount;
      if (it->second.itemId.empty() && !itemId.empty()) {
        it->second.itemId = itemId;
      }
      if (it->second.description.empty() && !itemDescription.empty()) {
        it->second.description = itemDescription;
      }
      if (it->second.valueEach <= 0 && itemValueEach > 0) {
        it->second.valueEach = itemValueEach;
      }
      if (it->second.quality.empty() && !itemQuality.empty()) {
        it->second.quality = itemQuality;
      }
      if (it->second.qualityLevel < 0 && itemQualityLevel >= 0) {
        it->second.qualityLevel = itemQualityLevel;
      }
      if (it->second.weaponModel.empty() && !itemWeaponModel.empty()) {
        it->second.weaponModel = itemWeaponModel;
      }
    }
  }

  if (aggregated.empty()) {
    return true;
  }

  std::string json = "[";
  std::string hash = "";
  int outputCount = 0;
  for (auto it = aggregated.begin(); it != aggregated.end(); ++it) {
    const AggregatedInventoryItem &entry = it->second;
    if (outputCount > 0) {
      json += ",";
      hash += "|";
    }
    json += "{\"name\":\"" + EscapeJSON(entry.name) + "\",";
    json += "\"count\":" + ToString(entry.count) + ",";
    json += "\"equipped\":" + std::string(entry.equipped ? "true" : "false");
    if (!entry.itemId.empty()) {
      json += ",\"item_id\":\"" + EscapeJSON(entry.itemId) + "\"";
    }
    if (!entry.description.empty()) {
      json += ",\"description\":\"" + EscapeJSON(entry.description) + "\"";
    }
    if (entry.valueEach > 0) {
      json += ",\"value_each\":" + ToString(entry.valueEach);
    }
    if (entry.qualityLevel >= 0) {
      json += ",\"quality_level\":" + ToString(entry.qualityLevel);
    }
    if (!entry.quality.empty()) {
      json += ",\"quality\":\"" + EscapeJSON(entry.quality) + "\"";
    }
    if (!entry.weaponModel.empty()) {
      json += ",\"weapon_model\":\"" + EscapeJSON(entry.weaponModel) + "\"";
    }
    json += "}";

    hash += EscapeJSON(entry.itemId.empty() ? entry.name : entry.itemId) + "^" +
            ToString(entry.count) + "^" + std::string(entry.equipped ? "1" : "0") + "^" +
            ToString(entry.valueEach) + "^" + EscapeJSON(entry.description) +
            "^" + ToString(entry.qualityLevel) + "^" + EscapeJSON(entry.quality) +
            "^" + EscapeJSON(entry.weaponModel);
    ++outputCount;
    if (outputCount >= 200) {
      break;
    }
  }
  json += "]";

  inventoryJsonOut = json;
  inventoryHashOut = hash;
  itemCountOut = outputCount;
  return true;
}

bool CaptureTraderInventorySnapshot(Character *npc, const std::string &reason) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return false;
  }

  GameWorld *world = GetWorldSafe();
  if (!world || (uintptr_t)world <= 0x1000) {
    return false;
  }

  bool worldLoadingFromSave = true;
  try {
    worldLoadingFromSave = world->isLoadingFromASaveGame();
  } catch (...) {
    worldLoadingFromSave = true;
  }
  if (worldLoadingFromSave) {
    return false;
  }

  unsigned int npcSerial = 0;
  try {
    npcSerial = npc->getHandle().serial;
  } catch (...) {
    npcSerial = 0;
  }
  if (npcSerial == 0) {
    return false;
  }

  const hand &npcIndoorsHandle = npc->isIndoors();
  bool npcIsIndoors = IsIndoorsHandleValid(npcIndoorsHandle);
  unsigned int npcBuildingSerial = npcIsIndoors ? npcIndoorsHandle.serial : 0;

  std::map<std::string, SimpleTraderInventoryItem> aggregated;

  auto lowerAscii = [](const std::string &value) -> std::string {
    std::string lowered = value;
    for (size_t i = 0; i < lowered.length(); ++i) {
      lowered[i] = static_cast<char>(std::tolower((unsigned char)lowered[i]));
    }
    return lowered;
  };

  auto mergeInventory = [&](Inventory *inventory) {
    if (!inventory || (uintptr_t)inventory <= 0x1000) {
      return;
    }
    std::vector<Item *> rawItems;
    try {
      GetAllInventoryItemsFromInventory(inventory, rawItems);
    } catch (...) {
      rawItems.clear();
    }
    if (rawItems.size() > 600) {
      rawItems.resize(600);
    }
    for (uint32_t i = 0; i < rawItems.size(); ++i) {
      Item *item = rawItems[i];
      if (!item || (uintptr_t)item <= 0x1000) {
        continue;
      }

      std::string itemName = "";
      std::string itemId = "";
      std::string itemQuality = "";
      int itemQualityLevel = -1;
      std::string itemWeaponModel = "";
      int itemCount = 1;
      int itemValueEach = 0;
      try {
        itemName = TrimCopy(item->getName());
        if (item->data && (uintptr_t)item->data > 0x1000 &&
            !item->data->stringID.empty()) {
          itemId = TrimCopy(item->data->stringID);
        }
        TryExtractArmourQuality(item, itemQualityLevel, itemQuality);
        TryExtractWeaponModel(item, itemWeaponModel);
        itemCount = item->quantity;
        itemValueEach = item->getValueSingle(false);
      } catch (...) {
        continue;
      }
      if (itemName.empty()) {
        continue;
      }
      if (itemCount <= 0) {
        itemCount = 1;
      }
      if (itemValueEach < 0) {
        itemValueEach = 0;
      }

      std::string key = itemId.empty() ? itemName : itemId;
      key = lowerAscii(key);
      std::string qualityKey =
          itemQualityLevel >= 0
              ? ToString(itemQualityLevel)
              : (itemQuality.empty() ? std::string("none") : itemQuality);
      key += "|q:" + lowerAscii(qualityKey);
      if (!itemWeaponModel.empty()) {
        key += "|wm:" + lowerAscii(itemWeaponModel);
      }
      auto it = aggregated.find(key);
      if (it == aggregated.end()) {
        SimpleTraderInventoryItem entry;
        entry.name = itemName;
        entry.itemId = itemId;
        entry.quality = itemQuality;
        entry.qualityLevel = itemQualityLevel;
        entry.weaponModel = itemWeaponModel;
        entry.count = itemCount;
        entry.valueEach = itemValueEach;
        aggregated[key] = entry;
      } else {
        it->second.count += itemCount;
        if (it->second.itemId.empty() && !itemId.empty()) {
          it->second.itemId = itemId;
        }
        if (it->second.valueEach <= 0 && itemValueEach > 0) {
          it->second.valueEach = itemValueEach;
        }
        if (it->second.quality.empty() && !itemQuality.empty()) {
          it->second.quality = itemQuality;
        }
        if (it->second.qualityLevel < 0 && itemQualityLevel >= 0) {
          it->second.qualityLevel = itemQualityLevel;
        }
        if (it->second.weaponModel.empty() && !itemWeaponModel.empty()) {
          it->second.weaponModel = itemWeaponModel;
        }
      }
      if (aggregated.size() >= 320) {
        break;
      }
    }
  };

  lektor<RootObject *> nearbyBuildings;
  try {
    world->getObjectsWithinSphere(nearbyBuildings, npc->getPosition(), 120.0f,
                                  BUILDING, 96, (RootObject *)npc);
  } catch (...) {
    nearbyBuildings.clear();
  }

  std::map<unsigned int, bool> seenBuildingSerials;
  for (uint32_t i = 0; i < nearbyBuildings.size(); ++i) {
    Building *building = (Building *)nearbyBuildings.stuff[i];
    if (!building || (uintptr_t)building <= 0x1000) {
      continue;
    }

    unsigned int buildingSerial = 0;
    try {
      buildingSerial = building->getHandle().serial;
    } catch (...) {
      buildingSerial = 0;
    }
    if (buildingSerial == 0) {
      continue;
    }
    if (seenBuildingSerials.count(buildingSerial) > 0) {
      continue;
    }
    seenBuildingSerials[buildingSerial] = true;

    bool sameIndoorShell = true;
    if (npcIsIndoors) {
      sameIndoorShell = (buildingSerial == npcBuildingSerial);
      try {
        const hand &buildingIndoors = building->isIndoors();
        if (!sameIndoorShell && IsIndoorsHandleValid(buildingIndoors) &&
            buildingIndoors.serial == npcBuildingSerial) {
          sameIndoorShell = true;
        }
      } catch (...) {
      }
      if (!sameIndoorShell) {
        continue;
      }
    }

    BuildingFunction functionType = BF_ANY;
    BuildingClassType classType = BCTYPE_FLUFF;
    try {
      functionType = building->_NV_getSpecialFunction();
    } catch (...) {
      functionType = BF_ANY;
    }
    try {
      classType = building->_NV_getBuildingClass();
    } catch (...) {
      classType = BCTYPE_FLUFF;
    }
    std::string buildingName = "";
    try {
      buildingName = TrimCopy(building->getName());
    } catch (...) {
      buildingName = "";
    }
    std::string buildingNameLower = lowerAscii(buildingName);
    bool likelyStorageByName =
        buildingNameLower.find("shop") != std::string::npos ||
        buildingNameLower.find("counter") != std::string::npos ||
        buildingNameLower.find("barrel") != std::string::npos ||
        buildingNameLower.find("chest") != std::string::npos ||
        buildingNameLower.find("storage") != std::string::npos ||
        buildingNameLower.find("cabinet") != std::string::npos ||
        buildingNameLower.find("shelf") != std::string::npos ||
        buildingNameLower.find("basket") != std::string::npos;
    bool likelyStorage =
        functionType == BF_SHOP ||
        functionType == BF_GENERAL_STORAGE ||
        functionType == BF_RESOURCE_STORAGE || classType == BCTYPE_STORAGE ||
        classType == BCTYPE_PRODUCTION || classType == BCTYPE_CRAFTING ||
        classType == BCTYPE_USABLE || likelyStorageByName;
    if (!likelyStorage) {
      continue;
    }

    Inventory *buildingInventory = nullptr;
    try {
      buildingInventory = building->getInventory();
    } catch (...) {
      buildingInventory = nullptr;
    }
    mergeInventory(buildingInventory);
    if (aggregated.size() >= 320) {
      break;
    }
  }

  if (aggregated.empty()) {
    try {
      mergeInventory(npc->getInventory());
      ContainerItem *backpack = npc->hasABackpackOn();
      if (backpack && backpack->inventory) {
        mergeInventory(backpack->inventory);
      }
    } catch (...) {
    }
  }

  std::string inventoryJson = "[";
  int inventoryCount = 0;
  for (auto it = aggregated.begin(); it != aggregated.end(); ++it) {
    const SimpleTraderInventoryItem &entry = it->second;
    if (inventoryCount > 0) {
      inventoryJson += ",";
    }
    inventoryJson += "{\"name\":\"" + EscapeJSON(entry.name) + "\",";
    inventoryJson += "\"count\":" + ToString(entry.count);
    if (!entry.itemId.empty()) {
      inventoryJson += ",\"item_id\":\"" + EscapeJSON(entry.itemId) + "\"";
    }
    if (entry.valueEach > 0) {
      inventoryJson += ",\"value_each\":" + ToString(entry.valueEach);
    }
    if (entry.qualityLevel >= 0) {
      inventoryJson += ",\"quality_level\":" + ToString(entry.qualityLevel);
    }
    if (!entry.quality.empty()) {
      inventoryJson += ",\"quality\":\"" + EscapeJSON(entry.quality) + "\"";
    }
    if (!entry.weaponModel.empty()) {
      inventoryJson += ",\"weapon_model\":\"" + EscapeJSON(entry.weaponModel) +
                       "\"";
    }
    inventoryJson += "}";
    inventoryCount += 1;
    if (inventoryCount >= 260) {
      break;
    }
  }
  inventoryJson += "]";

  EnterCriticalSection(&g_stateMutex);
  TraderInventoryCacheEntry &cache = s_traderInventoryCacheBySerial[npcSerial];
  cache.inventoryJson = inventoryJson;
  cache.itemCount = inventoryCount;
  cache.capturedTick = GetTickCount();
  LeaveCriticalSection(&g_stateMutex);

  Log("TRADER_INVENTORY_CAPTURE: npc=" + npc->getName() +
      " serial=" + ToString((int)npcSerial) +
      " items=" + ToString(inventoryCount) + " reason=" + reason);
  return inventoryCount > 0;
}

bool GetCachedTraderInventorySnapshot(Character *npc, std::string &inventoryJsonOut,
                                      int &itemCountOut, int *ageSecondsOut) {
  inventoryJsonOut = "[]";
  itemCountOut = 0;
  if (ageSecondsOut) {
    *ageSecondsOut = -1;
  }
  if (!npc || (uintptr_t)npc < 0x1000) {
    return false;
  }

  unsigned int npcSerial = 0;
  try {
    npcSerial = npc->getHandle().serial;
  } catch (...) {
    npcSerial = 0;
  }
  if (npcSerial == 0) {
    return false;
  }

  EnterCriticalSection(&g_stateMutex);
  auto it = s_traderInventoryCacheBySerial.find(npcSerial);
  if (it != s_traderInventoryCacheBySerial.end()) {
    inventoryJsonOut = it->second.inventoryJson;
    itemCountOut = it->second.itemCount;
    if (ageSecondsOut) {
      DWORD nowTick = GetTickCount();
      *ageSecondsOut = (int)((nowTick - it->second.capturedTick) / 1000);
    }
    LeaveCriticalSection(&g_stateMutex);
    return itemCountOut > 0;
  }
  LeaveCriticalSection(&g_stateMutex);
  return false;
}

static std::string GetHealthStatus(Character *npc) {
  if (!npc || (uintptr_t)npc < 0x1000)
    return "Unknown";
  MedicalSystem *med = npc->getMedical();
  if (!med || (uintptr_t)med < 0x1000)
    return "Unknown";

  if (med->dead)
    return "Dead";
  if (med->unconcious)
    return "Unconscious";
  if (npc->_currentProneState == PS_PLAYING_DEAD)
    return "Playing Dead";

  bool crippled = false;
  if (med->leftLeg && med->leftLeg->flesh < 0)
    crippled = true;
  if (med->rightLeg && med->rightLeg->flesh < 0)
    crippled = true;

  bool injured = false;
  // Check major parts
  MedicalSystem::HealthPartStatus *parts[6] = {med->getPart(0), med->getPart(1),
                                               med->leftArm,    med->rightArm,
                                               med->leftLeg,    med->rightLeg};
  for (int i = 0; i < 6; ++i) {
    MedicalSystem::HealthPartStatus *p = parts[i];
    if (p && p->flesh < p->maxHealth() * 0.70f)
      injured = true;
  }

  if (crippled)
    return "Crippled";
  if (injured)
    return "Injured";
  return "Healthy";
}

static std::string GetVisibleEquipment(Character *npc) {
  if (!npc || (uintptr_t)npc < 0x1000)
    return "";
  Inventory *inv = npc->getInventory();
  if (!inv || (uintptr_t)inv < 0x1000)
    return "";

  auto appendEquipmentName = [](std::string &buffer, Item *item) {
    if (!item || (uintptr_t)item <= 0x1000) {
      return;
    }
    std::string itemName = "";
    try {
      itemName = TrimCopy(item->getName());
    } catch (...) {
      itemName = "";
    }
    if (itemName.empty()) {
      return;
    }

    int qualityLevel = -1;
    std::string qualityLabel;
    if (TryExtractArmourQuality(item, qualityLevel, qualityLabel) &&
        !qualityLabel.empty()) {
      itemName += " [" + qualityLabel + "]";
    }
    std::string weaponModel;
    if (TryExtractWeaponModel(item, weaponModel) && !weaponModel.empty()) {
      itemName += " [" + weaponModel + "]";
    }

    if (!buffer.empty()) {
      buffer += ", ";
    }
    buffer += itemName;
  };

  std::string eq = "";
  lektor<Item *> armor;
  inv->getEquippedArmour(armor);
  for (uint32_t i = 0; i < armor.size(); ++i) {
    appendEquipmentName(eq, armor.stuff[i]);
  }

  lektor<Item *> weapons;
  inv->getEquippedWeapons(weapons);
  for (uint32_t i = 0; i < weapons.size(); ++i) {
    appendEquipmentName(eq, weapons.stuff[i]);
  }
  return eq;
}

std::string GetStorageIDFor(Character *npc, const std::string &name,
                            const std::string &factionName) {
  if (!npc || (uintptr_t)npc < 0x1000) {
    return name;
  }

  InstanceID *iid = npc->getInstanceID();
  if (iid && !iid->uid.empty()) {
    return iid->uid;
  }

  unsigned int serial = npc->getHandle().serial;
  if (serial != 0) {
    return std::string("hand_") +
           std::to_string(static_cast<unsigned long long>(serial));
  }

  return name;
}

std::string GetIdentityFaction(Character *npc) {
  if (!npc || (uintptr_t)npc < 0x1000)
    return "Neutral";

  Faction *faction = nullptr;
  try {
    faction = npc->getFaction() ? npc->getFaction() : npc->owner;
  } catch (...) {
  }

  std::string factionName = "Neutral";
  if (faction && (uintptr_t)faction > 0x1000) {
    factionName = faction->getName();
    if (factionName.empty() || factionName == "Unknown") {
      if (faction->data && !faction->data->name.empty())
        factionName = faction->data->name;
    }
  }

  std::string identityFaction = factionName;
  unsigned int serial = npc->getHandle().serial;

  std::string cached = "";
  EnterCriticalSection(&g_stateMutex);
  if (g_originFactions.count(serial)) {
    cached = g_originFactions[serial];
  }
  LeaveCriticalSection(&g_stateMutex);

  if (!cached.empty()) {
    return cached;
  }

  // If they are in the player squad, try to find their true origin
  if (faction && faction->isThePlayer()) {
    GameData *characterData = npc->getGameData();
    GameWorld *world = GetWorldSafe();
    if (characterData && world && world->factionMgr) {
      const Ogre::vector<GameDataReference>::type *refs =
          characterData->getReferenceListIfExists("faction");
      if (refs && !refs->empty()) {
        Faction *refFaction =
            world->factionMgr->getFactionByStringID(refs->at(0).sid);
        if (refFaction && !refFaction->isThePlayer()) {
          identityFaction = refFaction->getName();
          g_originFactions[serial] = identityFaction;
          return identityFaction;
        }
      }
    }
    // If we still didn't find an origin, use a generic label to avoid
    // volatile player faction names (which change if the player renames their
    // squad)
    if (identityFaction == factionName || identityFaction == "Nameless") {
      identityFaction = "Drifters";
    }
  } else {
    // For non-player NPCs, their current faction is their stable identity
    if (!factionName.empty() && factionName != "Unknown" &&
        factionName != "Neutral") {
      EnterCriticalSection(&g_stateMutex);
      g_originFactions[serial] = factionName;
      LeaveCriticalSection(&g_stateMutex);
    }
  }
  return identityFaction;
}

std::string BuildNpcContextEnvelope(Character *npc, const std::string &type) {
  GameWorld *world = GetWorldSafe();
  if (!npc || (uintptr_t)npc < 0x1000 || !world)
    return "{}";

  std::string json = "{";
  // Write 'type' first so server routing can distinguish player vs NPC context
  json += "\"type\": \"" + type + "\",";
  Stobe::PlayerBase::Snapshot playerBase;
  Stobe::PlayerBase::Capture(world, npc, playerBase);
  json += "\"player_base\":" +
          Stobe::PlayerBase::BuildJson(playerBase) + ",";
  bool isPlayerCharacter = false;
  bool isAnimal = false;
  try {
    isPlayerCharacter = npc->isPlayerCharacter();
  } catch (...) {
    isPlayerCharacter = (type == "player");
  }
  try {
    isAnimal = (npc->isAnimal() != 0);
  } catch (...) {
    isAnimal = false;
  }
  json += "\"is_player_character\": " +
          std::string(isPlayerCharacter ? "true" : "false") + ",";
  json += "\"is_animal\": " + std::string(isAnimal ? "true" : "false") + ",";
  json += "\"handle\": \"" + ToString((int)npc->getHandle().serial) + "\",";
  if (world) {
    TimeOfDay tod = world->getTimeStamp_inGameHours();
    // Kenshi Time tracking:
    // getTotalDays() usually matches game clock days
    // we use total hours/minutes for the remainder of the clock
    int day = (int)tod.getTotalDays();
    int hour = (int)fmod(tod.getTotalHours(), 24.0);
    int minute = (int)fmod(tod.getTotalMinutes(), 60.0);

    json += "\"day\": " + ToString(day) + ",";
    json += "\"hour\": " + ToString(hour) + ",";
    json += "\"minute\": " + ToString(minute) + ",";
    json += "\"game_ts\": " + ToString((int)tod.getTotalSeconds()) + ",";
    json += "\"gamespeed\": " + ToString(world->getFrameSpeedMultiplier()) + ",";
    json += "\"is_paused\": " +
            std::string(world->isPaused() ? "true" : "false") + ",";

    // AI Timers for Debugger
    int nowGameTs = (int)tod.getTotalSeconds();
    int boredElapsedGamets = 0;
    if (g_lastBoredEventGameTs > 0 && nowGameTs >= g_lastBoredEventGameTs) {
      boredElapsedGamets = nowGameTs - g_lastBoredEventGameTs;
    }
    const int boredEventExtraDelayGamets = 10;
    int boredIntervalGamets = g_boredEventIntervalHours * 3600 + boredEventExtraDelayGamets;
    json += "\"bored_event_timer_ms\": " +
            ToString((int)(boredElapsedGamets * 1000)) + ",";
    json += "\"bored_event_interval_ms\": " +
            ToString((int)(boredIntervalGamets * 1000)) + ",";
    json += "\"bored_event_timer_ingame_seconds\": " +
            ToString((int)boredElapsedGamets) + ",";
    json += "\"bored_event_interval_ingame_seconds\": " +
            ToString((int)boredIntervalGamets) + ",";

    DWORD now = GetTickCount();
    DWORD speech_elapsed = now - g_lastDialogueTick;
    json += "\"speech_delay_ms\": " + ToString((int)speech_elapsed) + ",";
    json += "\"speech_interval_ms\": " +
            ToString((int)(g_dialogueSpeedSeconds * 1000)) + ",";
  }

  // --- Character state first so server-side gating can short-circuit dead/KO ---
  std::string charState = "normal";
  bool isDead = false;
  bool isUnconcious = false;
  bool isSlave = false;
  try {
    isDead = npc->isDead();
    if (isDead) {
      charState = "dead";
    } else {
      isUnconcious = npc->isUnconcious();
      if (isUnconcious) {
        charState = "unconscious";
      } else if (npc->inSomething == IN_PRISON) {
        charState = "imprisoned";
      } else {
        // Enslaved: currently assigned as a slave
        try {
          SlaveStateEnum slaveState = npc->isSlave();
          bool chained = npc->isChainedMode();
          if (slaveState != 0) { // 0 == not a slave
            isSlave = true;
            // Escaped slave: has slave status but no chains/owner
            charState = chained ? "enslaved" : "escaped-slave";
          }
        } catch (...) {
        }
      }
    }
  } catch (...) {
  }
  json += "\"character_state\": \"" + charState + "\",";
  json += "\"is_slave\": " + std::string(isSlave ? "true" : "false") + ",";
  json += "\"is_incapacitated\": " +
          std::string((isDead || isUnconcious) ? "true" : "false") + ",";
  int drunkLevel = 0;
  bool isDrunk = false;
  int drunkSecondsRemaining = 0;
  std::string drunkStatus = "sober";
  GetCharacterDrunkPromptState(npc, drunkLevel, isDrunk, drunkStatus,
                               drunkSecondsRemaining);
  bool isHigh = false;
  int highSecondsRemaining = 0;
  float highHungerRateMultiplier = 1.0f;
  std::string highStatus = "sober";
  GetCharacterDrugPromptState(npc, isHigh, highStatus, highSecondsRemaining,
                              highHungerRateMultiplier);
  json += "\"drunk_level\": " + ToString(drunkLevel) + ",";
  json += "\"is_drunk\": " + std::string(isDrunk ? "true" : "false") + ",";
  json += "\"drunk_status\": \"" + EscapeJSON(drunkStatus) + "\",";
  json += "\"drunk_seconds_remaining\": " + ToString(drunkSecondsRemaining) + ",";
  json += "\"is_high\": " + std::string(isHigh ? "true" : "false") + ",";
  json += "\"high_status\": \"" + EscapeJSON(highStatus) + "\",";
  json += "\"high_seconds_remaining\": " + ToString(highSecondsRemaining) + ",";
  json += "\"high_hunger_rate_multiplier\": " +
          ToString(highHungerRateMultiplier) + ",";

  ActivitySnapshot activity;
  CaptureActivitySnapshot(npc, charState, activity);
  json += "\"current_action\": \"" + EscapeJSON(activity.currentAction) + "\",";
  json += "\"is_moving\": " + std::string(activity.isMoving ? "true" : "false") +
          ",";
  json += "\"is_running\": " +
          std::string(activity.isRunning ? "true" : "false") + ",";
  json += "\"is_sneaking\": " +
          std::string(activity.isSneaking ? "true" : "false") + ",";
  json += "\"is_in_combat\": " +
          std::string(activity.isInCombat ? "true" : "false") + ",";
  json += "\"is_attacking\": " +
          std::string(activity.isAttacking ? "true" : "false") + ",";
  json += "\"movement_speed\": " + ToString(activity.movementSpeed) + ",";
  if (!activity.attackTargetName.empty()) {
    json += "\"attack_target\": \"" + EscapeJSON(activity.attackTargetName) +
            "\",";
  }

  std::map<unsigned int, std::string> carriedByNameBySerial;
  BuildCarriedByNameLookup(world, carriedByNameBySerial);
  unsigned int npcSerial = 0;
  try {
    npcSerial = npc->getHandle().serial;
  } catch (...) {
    npcSerial = 0;
  }
  bool isBeingCarried = false;
  std::string carriedByName = "";
  if (npcSerial != 0) {
    std::map<unsigned int, std::string>::const_iterator carrierIt =
        carriedByNameBySerial.find(npcSerial);
    if (carrierIt != carriedByNameBySerial.end()) {
      isBeingCarried = true;
      carriedByName = TrimCopy(carrierIt->second);
    }
  }

  bool isCarrying = false;
  std::string carryingTargetName = "";
  try {
    isCarrying = npc->isCarryingSomething && npc->carryingObject.isValid();
    if (isCarrying) {
      carryingTargetName = ResolveCarryTargetNameSafe(npc->carryingObject);
    }
  } catch (...) {
    isCarrying = false;
    carryingTargetName = "";
  }
  json += "\"is_carrying\": " + std::string(isCarrying ? "true" : "false") +
          ",";
  if (!carryingTargetName.empty()) {
    json += "\"carrying_target_name\": \"" + EscapeJSON(carryingTargetName) +
            "\",";
  }
  json += "\"is_being_carried\": " +
          std::string(isBeingCarried ? "true" : "false") + ",";
  if (!carriedByName.empty()) {
    json += "\"carried_by_name\": \"" + EscapeJSON(carriedByName) + "\",";
  }

  std::string name = "Unknown";
  try {
    name = npc->getName();
    if (name.empty() || name == "Unknown Entity" || name == "Unknown") {
      if (!npc->displayName.empty())
        name = npc->displayName;
      else if (npc->data && !npc->data->name.empty())
        name = npc->data->name;
    }
  } catch (...) {
  }
  json += "\"name\": \"" + EscapeJSON(name) + "\",";

  InstanceID *iid = npc->getInstanceID();
  if (iid && !iid->uid.empty()) {
    json += "\"id\": \"" + EscapeJSON(iid->uid) + "\",";
  } else {
    json += "\"id\": \"hand_" + ToString((int)npc->getHandle().serial) + "\",";
  }

  // Robust Race Name
  RaceData *race = nullptr;
  try {
    race = npc->getRace() ? npc->getRace() : npc->myRace;
  } catch (...) {
  }

  std::string raceName = "Unknown";
  bool raceIsRobot = false;
  if (race && (uintptr_t)race > 0x1000) {
    raceIsRobot = race->robot;
    if (race->data && !race->data->name.empty())
      raceName = race->data->name;
    else if (race->data && !race->data->stringID.empty())
      raceName = race->data->stringID;
  }
  json += "\"race\": \"" + EscapeJSON(raceName) + "\",";
  json += "\"race_is_robot\": " +
          std::string(raceIsRobot ? "true" : "false") + ",";

  // Robust Gender
  std::string gender = "male";
  try {
    gender = npc->isFemale() ? "female" : "male";
  } catch (...) {
  }
  if (npc->sex == "female" || npc->sex == "male")
    gender = npc->sex;
  json += "\"gender\": \"" + gender + "\",";
  bool hasBeard = false;
  bool isShaved = false;
  bool isFlayed = false;
  float heightNorm = 0.5f;
  float ageNorm = 0.5f;
  CaptureAppearanceSnapshot(npc, hasBeard, isShaved, isFlayed, heightNorm,
                            ageNorm);
  HornSliderSnapshot hornSnapshot;
  CaptureHornSliderSnapshot(npc, hornSnapshot);
  json += "\"has_beard\": " + std::string(hasBeard ? "true" : "false") + ",";
  json += "\"is_shaved\": " + std::string(isShaved ? "true" : "false") + ",";
  json += "\"is_flayed\": " + std::string(isFlayed ? "true" : "false") + ",";
  json += "\"height_norm\": " + ToString(heightNorm) + ",";
  json += "\"age_norm\": " + ToString(ageNorm) + ",";
  if (hornSnapshot.hasAny) {
    json += "\"horn_sliders\":" + BuildHornSliderPayload(hornSnapshot) + ",";
  }

  // Robust Faction Name
  Faction *faction = nullptr;
  try {
    faction = npc->getFaction() ? npc->getFaction() : npc->owner;
  } catch (...) {
  }

  std::string factionName = "Neutral";
  std::string factionID = "Neutral";
  if (faction && (uintptr_t)faction > 0x1000) {
    std::string fn = faction->getName();
    if (!fn.empty() && fn != "Unknown")
      factionName = fn;
    else if (faction->data && !faction->data->name.empty())
      factionName = faction->data->name;

    if (faction->data)
      factionID = faction->data->stringID;
    else
      factionID = factionName;
  }
  json += "\"faction\": \"" + EscapeJSON(factionName) + "\",";
  json += "\"factionID\": \"" + EscapeJSON(factionID) + "\",";
  json += "\"faction_id\": \"" + EscapeJSON(factionID) + "\",";

  // IDENTITY STABILITY
  std::string identityFaction = GetIdentityFaction(npc);
  json += "\"origin_faction\": \"" + EscapeJSON(identityFaction) + "\",";

  // Stable Storage ID: Prioritize InstanceID (UUID) or the stable Identity
  // Faction.
  std::string stableID = GetStorageIDFor(npc, name, identityFaction);
  json += "\"storage_id\": \"" + EscapeJSON(stableID) + "\",";

  // Keep relation neutral here; server can resolve richer faction state.
  json += "\"relation\": 0,";

  bool isTrader = false;
  try {
    isTrader = npc->isATrader();
  } catch (...) {
  }
  json += "\"is_trader\": " + std::string(isTrader ? "true" : "false") + ",";

  bool isLeader = false;
  if (faction && (uintptr_t)faction > 0x1000 && faction->data &&
      (uintptr_t)faction->data > 0x1000) {
    hand lHand;
    if (faction->data->getHandle(lHand, "leader") && lHand.isValid()) {
      if (lHand == npc->getHandle())
        isLeader = true;
    }
  }
  json += "\"is_leader\": " + std::string(isLeader ? "true" : "false") + ",";
  std::string ordersPayload = BuildStandingOrderPayload(npc);
  if (ordersPayload != "{}") {
    json += "\"orders\": " + ordersPayload + ",";
  }

  const hand &npcIndoorsHandle = npc->isIndoors();
  bool npcIsIndoors = IsIndoorsHandleValid(npcIndoorsHandle);
  unsigned int npcBuildingSerial = npcIsIndoors ? npcIndoorsHandle.serial : 0;
  std::string npcBuildingName =
      npcIsIndoors ? GetIndoorBuildingName(npcIndoorsHandle) : "";
  int npcFloor = npc->getFloor();
  TownBase *town = npc->getCurrentTownLocation();
  std::string townName = "";
  std::string zoneName = "";
  std::string townRegionName = "";
  if (town) {
    townName = ((RootObjectBase *)town)->getName();
    zoneName = ResolveTownZoneName(town);
    townRegionName = ResolveTownRegionName(town);
  }
  // LOCATION/TOWN sphere scans can return stale streamed objects after a load.
  // Use the character's live town pointer instead of dereferencing those results.
  bool inTownWalls = IsNpcInsideTownContext(npc, town);
  std::string zonePromptName = "";
  if (inTownWalls) {
    if (!townName.empty()) {
      zonePromptName = townName;
    } else if (!zoneName.empty() && !IsLikelyWorldZoneName(zoneName)) {
      zonePromptName = zoneName;
    }
  }
  std::string regionPromptName = "";
  if (IsLikelyWorldZoneName(townRegionName)) {
    regionPromptName = townRegionName;
  } else if (IsLikelyWorldZoneName(zoneName)) {
    regionPromptName = zoneName;
  } else if (!zonePromptName.empty() && IsLikelyWorldZoneName(zonePromptName)) {
    regionPromptName = zonePromptName;
  }
  if (regionPromptName.empty()) {
    regionPromptName = ResolveMappedRegionFromTownName(townName);
  }
  if (regionPromptName.empty()) {
    regionPromptName = ResolveMappedRegionFromTownName(zonePromptName);
  }
  std::string cachedTraderInventoryJson = "[]";
  int cachedTraderInventoryCount = 0;
  int cachedTraderInventoryAgeSeconds = -1;
  if (GetCachedTraderInventorySnapshot(npc, cachedTraderInventoryJson,
                                       cachedTraderInventoryCount,
                                       &cachedTraderInventoryAgeSeconds) &&
      cachedTraderInventoryCount > 0) {
    json += "\"trader_inventory_items\":" + cachedTraderInventoryJson + ",";
    json += "\"trader_inventory_item_count\":" +
            ToString(cachedTraderInventoryCount) + ",";
    json += "\"trader_inventory_age_seconds\":" +
            ToString(cachedTraderInventoryAgeSeconds) + ",";
  }

  if (world) {
    lektor<RootObject *> results;
    world->getCharactersWithinSphere(results, npc->getPosition(), g_visionRange,
                                     0.0f, 0.0f, 16, 0, npc);
    json += "\"nearby\": [";
    for (uint32_t i = 0; i < results.size(); ++i) {
      Character *other = (Character *)results.stuff[i];
      if (other && (uintptr_t)other > 0x1000) {
        bool otherIsAnimal = false;
        unsigned int otherSerial = 0;
        try {
          otherIsAnimal = (other->isAnimal() != 0);
          otherSerial = other->getHandle().serial;
        } catch (...) {
          otherIsAnimal = false;
          otherSerial = 0;
        }
        if (otherIsAnimal &&
            (!g_enableAnimalTalks || !IsAnimalActivated(otherSerial))) {
          continue;
        }

        // ONLY exclude the primary player character (typically the first char
        // in first squad)
        if (world->player && world->player->playerCharacters.size() > 0) {
          if (other == world->player->playerCharacters[0])
            continue;
        }

        if (json.back() == '}')
          json += ",";

        std::string o_name = other->getName();

        // Robust Race Name
        RaceData *o_race = other->getRace() ? other->getRace() : other->myRace;
        std::string o_rn = "Unknown";
        bool o_raceIsRobot = false;
        if (o_race && (uintptr_t)o_race > 0x1000) {
          o_raceIsRobot = o_race->robot;
          if (o_race->data && !o_race->data->name.empty())
            o_rn = o_race->data->name;
          else if (o_race->data && !o_race->data->stringID.empty())
            o_rn = o_race->data->stringID;
        }

        // Robust Faction Name
        Faction *o_fact =
            other->getFaction() ? other->getFaction() : other->owner;
        std::string o_fn = "Neutral";
        std::string o_fid = "Neutral";
        if (o_fact && (uintptr_t)o_fact > 0x1000) {
          std::string fn = o_fact->getName();
          if (!fn.empty() && fn != "Unknown")
            o_fn = fn;
          else if (o_fact->data && !o_fact->data->name.empty())
            o_fn = o_fact->data->name;
          else if (o_fact->data && !o_fact->data->stringID.empty())
            o_fn = o_fact->data->stringID;

          if (o_fact->data && !o_fact->data->stringID.empty())
            o_fid = o_fact->data->stringID;
          else
            o_fid = o_fn;
        }

        std::string o_gender = other->isFemale() ? "female" : "male";
        bool o_hasBeard = false;
        bool o_isShaved = false;
        bool o_isFlayed = false;
        float o_heightNorm = 0.5f;
        float o_ageNorm = 0.5f;
        CaptureAppearanceSnapshot(other, o_hasBeard, o_isShaved, o_isFlayed,
                                  o_heightNorm, o_ageNorm);
        HornSliderSnapshot o_hornSnapshot;
        CaptureHornSliderSnapshot(other, o_hornSnapshot);
        float dist = npc->getPosition().distance(other->getPosition());
        const hand &otherIndoorsHandle = other->isIndoors();
        bool otherIsIndoors = IsIndoorsHandleValid(otherIndoorsHandle);
        unsigned int otherBuildingSerial =
            otherIsIndoors ? otherIndoorsHandle.serial : 0;
        std::string otherBuildingName =
            otherIsIndoors ? GetIndoorBuildingName(otherIndoorsHandle) : "";
        int otherFloor = other->getFloor();
        bool sameBuildingAsActor =
            npcIsIndoors && otherIsIndoors &&
            npcIndoorsHandle == otherIndoorsHandle;
        bool sameFloorAsActor = sameBuildingAsActor && npcFloor == otherFloor;

        // IDENTITY STABILITY: Use the origin-faction cache for overhearers too!
        std::string o_sid_fact = o_fn;
        unsigned int o_serial = other->getHandle().serial;
        bool o_isCarrying = false;
        std::string o_carryingTargetName = "";
        bool o_isBeingCarried = false;
        std::string o_carriedByName = "";
        try {
          o_isCarrying =
              other->isCarryingSomething && other->carryingObject.isValid();
          if (o_isCarrying) {
            o_carryingTargetName =
                ResolveCarryTargetNameSafe(other->carryingObject);
          }
        } catch (...) {
          o_isCarrying = false;
          o_carryingTargetName.clear();
        }
        if (o_serial != 0) {
          std::map<unsigned int, std::string>::const_iterator oCarrierIt =
              carriedByNameBySerial.find(o_serial);
          if (oCarrierIt != carriedByNameBySerial.end()) {
            o_isBeingCarried = true;
            o_carriedByName = TrimCopy(oCarrierIt->second);
          }
        }

        EnterCriticalSection(&g_stateMutex);
        if (g_originFactions.count(o_serial)) {
          o_sid_fact = g_originFactions[o_serial];
        } else if (o_fact && !o_fact->isThePlayer()) {
          // For non-player characters, cache their current faction as origin
          g_originFactions[o_serial] = o_fn;
          o_sid_fact = o_fn;
        }
        LeaveCriticalSection(&g_stateMutex);

        // Include storage_id for perfect overhearer-to-participant mapping
        std::string o_sid = GetStorageIDFor(other, o_name, o_sid_fact);

        // Sensory details for "looking" around
        std::string o_health = GetHealthStatus(other);
        std::string o_medical = BuildMedicalPayload(other);
        bool o_hasRoboticLimbs = false;
        std::string o_roboticLimbs =
            BuildRoboticLimbPayload(other, o_hasRoboticLimbs);
        std::string o_stats = BuildStatsPayload(other);
        std::string o_orders = BuildStandingOrderPayload(other);
        int o_bountyTotal = 0;
        std::string o_bountyPayload = BuildBountyPayload(other, o_bountyTotal);
        // Nearby sphere results can outlive nested inventory/equipment objects
        // during streaming. Dedicated inventory sync supplies those details.
        std::string o_charState = "normal";
        try {
          if (other->isDead()) {
            o_charState = "dead";
          } else if (other->isUnconcious()) {
            o_charState = "unconscious";
          } else if (other->inSomething == IN_PRISON) {
            o_charState = "imprisoned";
          } else {
            SlaveStateEnum oSlaveState = other->isSlave();
            if (oSlaveState != 0) {
              o_charState = other->isChainedMode() ? "enslaved" : "escaped-slave";
            }
          }
        } catch (...) {
          o_charState = "normal";
        }
        ActivitySnapshot o_activity;
        CaptureActivitySnapshot(other, o_charState, o_activity);
        int oDrunkLevel = 0;
        bool oIsDrunk = false;
        int oDrunkSecondsRemaining = 0;
        std::string oDrunkStatus = "sober";
        GetCharacterDrunkPromptState(other, oDrunkLevel, oIsDrunk, oDrunkStatus,
                                     oDrunkSecondsRemaining);
        bool oIsHigh = false;
        int oHighSecondsRemaining = 0;
        float oHighHungerRateMultiplier = 1.0f;
        std::string oHighStatus = "sober";
        GetCharacterDrugPromptState(other, oIsHigh, oHighStatus,
                                    oHighSecondsRemaining,
                                    oHighHungerRateMultiplier);

        json += "{\"name\":\"" + EscapeJSON(o_name) + "\",";
        json += "\"refid\":\"hand_" + ToString(other->getHandle().serial) + "\",";
        json += "\"race\":\"" + EscapeJSON(o_rn) + "\",";
        json += "\"race_is_robot\":" +
                std::string(o_raceIsRobot ? "true" : "false") + ",";
        json += "\"faction\":\"" + EscapeJSON(o_fn) + "\",";
        json += "\"factionID\":\"" + EscapeJSON(o_fid) + "\",";
        json += "\"faction_id\":\"" + EscapeJSON(o_fid) + "\",";
        json += "\"gender\":\"" + EscapeJSON(o_gender) + "\",";
        json += "\"has_beard\": " +
                std::string(o_hasBeard ? "true" : "false") + ",";
        json += "\"is_shaved\": " +
                std::string(o_isShaved ? "true" : "false") + ",";
        json += "\"is_flayed\": " +
                std::string(o_isFlayed ? "true" : "false") + ",";
        json += "\"height_norm\": " + ToString(o_heightNorm) + ",";
        json += "\"age_norm\": " + ToString(o_ageNorm) + ",";
        if (o_hornSnapshot.hasAny) {
          json += "\"horn_sliders\":" + BuildHornSliderPayload(o_hornSnapshot) +
                  ",";
        }
        json += "\"health\":\"" + EscapeJSON(o_health) + "\",";
        json += "\"medical\":" + o_medical + ",";
        json += "\"has_robotic_limbs\": " +
                std::string(o_hasRoboticLimbs ? "true" : "false") + ",";
        if (o_hasRoboticLimbs) {
          json += "\"robotic_limbs\":" + o_roboticLimbs + ",";
        }
        json += "\"stats\":" + o_stats + ",";
        json += "\"orders\":" + o_orders + ",";
        if (o_bountyTotal > 0) {
          json += "\"bounty\":" + ToString(o_bountyTotal) + ",";
        }
        if (o_bountyPayload != "{}") {
          json += "\"bounty_info\":" + o_bountyPayload + ",";
        }
        json += "\"drunk_level\": " + ToString(oDrunkLevel) + ",";
        json += "\"is_drunk\": " + std::string(oIsDrunk ? "true" : "false") +
                ",";
        json += "\"drunk_status\": \"" + EscapeJSON(oDrunkStatus) + "\",";
        json += "\"drunk_seconds_remaining\": " +
                ToString(oDrunkSecondsRemaining) + ",";
        json += "\"is_high\": " + std::string(oIsHigh ? "true" : "false") + ",";
        json += "\"high_status\": \"" + EscapeJSON(oHighStatus) + "\",";
        json += "\"high_seconds_remaining\": " +
                ToString(oHighSecondsRemaining) + ",";
        json += "\"high_hunger_rate_multiplier\": " +
                ToString(oHighHungerRateMultiplier) + ",";
        json += "\"storage_id\":\"" + EscapeJSON(o_sid) + "\",";
        json += "\"indoors\": " +
                std::string(otherIsIndoors ? "true" : "false") + ",";
        json += "\"outdoors\": " +
                std::string(otherIsIndoors ? "false" : "true") + ",";
        json += "\"building_serial\": " +
                ToString((int)otherBuildingSerial) + ",";
        json += "\"building_name\":\"" + EscapeJSON(otherBuildingName) + "\",";
        json += "\"floor\": " + ToString(otherFloor) + ",";
        json += "\"current_action\":\"" + EscapeJSON(o_activity.currentAction) +
                "\",";
        json += "\"is_moving\": " +
                std::string(o_activity.isMoving ? "true" : "false") + ",";
        json += "\"is_running\": " +
                std::string(o_activity.isRunning ? "true" : "false") + ",";
        json += "\"is_sneaking\": " +
                std::string(o_activity.isSneaking ? "true" : "false") + ",";
        json += "\"is_in_combat\": " +
                std::string(o_activity.isInCombat ? "true" : "false") + ",";
        json += "\"is_attacking\": " +
                std::string(o_activity.isAttacking ? "true" : "false") + ",";
        json += "\"movement_speed\": " + ToString(o_activity.movementSpeed) + ",";
        if (!o_activity.attackTargetName.empty()) {
          json += "\"attack_target\":\"" + EscapeJSON(o_activity.attackTargetName) +
                  "\",";
        }
        json += "\"is_carrying\": " +
                std::string(o_isCarrying ? "true" : "false") + ",";
        if (!o_carryingTargetName.empty()) {
          json += "\"carrying_target_name\":\"" +
                  EscapeJSON(o_carryingTargetName) + "\",";
        }
        json += "\"is_being_carried\": " +
                std::string(o_isBeingCarried ? "true" : "false") + ",";
        if (!o_carriedByName.empty()) {
          json += "\"carried_by_name\":\"" + EscapeJSON(o_carriedByName) +
                  "\",";
        }
        json += "\"same_building_as_actor\": " +
                std::string(sameBuildingAsActor ? "true" : "false") + ",";
        json += "\"same_floor_as_actor\": " +
                std::string(sameFloorAsActor ? "true" : "false") + ",";
        json += "\"dist\":" + ToString(dist) + "}";
      }
    }
    json += "],";

    // Nearby world items (ground objects near the speaker).
    json += "\"nearby_items\": [";
    lektor<RootObject *> nearbyItems;
    float nearbyItemRange = g_visionRange;
    if (nearbyItemRange < 600.0f) {
      nearbyItemRange = 600.0f;
    }
    world->getObjectsWithinSphere(nearbyItems, npc->getPosition(),
                                  nearbyItemRange, ITEM, 64,
                                  (RootObject *)npc);
    std::map<std::string, bool> seenNearbyItems;
    int nearbyItemCount = 0;
    int nearbyItemInvalid = 0;
    int nearbyItemNotGround = 0;
    int nearbyItemNoName = 0;
    int nearbyItemDuped = 0;
    int nearbyItemFallbackAdded = 0;
    for (uint32_t i = 0; i < nearbyItems.size(); ++i) {
      Item *item = (Item *)nearbyItems.stuff[i];
      if (!item || (uintptr_t)item <= 0x1000) {
        nearbyItemInvalid++;
        continue;
      }

      bool onGround = true;
      try {
        onGround = item->onGround();
      } catch (...) {
        onGround = true;
      }
      if (!onGround) {
        nearbyItemNotGround++;
        continue;
      }

      std::string itemName = TrimCopy(item->getName());
      if (itemName.empty()) {
        nearbyItemNoName++;
        continue;
      }

      unsigned int itemSerial = 0;
      try {
        itemSerial = item->getHandle().serial;
      } catch (...) {
        itemSerial = 0;
      }
      std::string dedupeKey =
          itemSerial > 0 ? ("id:" + ToString((int)itemSerial))
                         : ("name:" + itemName);
      for (size_t c = 0; c < dedupeKey.size(); ++c) {
        dedupeKey[c] = (char)std::tolower((unsigned char)dedupeKey[c]);
      }
      if (seenNearbyItems.count(dedupeKey)) {
        nearbyItemDuped++;
        continue;
      }
      seenNearbyItems[dedupeKey] = true;

      float itemDist = npc->getPosition().distance(item->getPosition());
      int quantity = 1;
      try {
        quantity = item->quantity;
      } catch (...) {
        quantity = 1;
      }
      if (quantity < 1) {
        quantity = 1;
      }

      if (nearbyItemCount > 0) {
        json += ",";
      }
      std::string refid =
          itemSerial > 0 ? ("hand_" + ToString((int)itemSerial)) : "";
      json += "{\"name\":\"" + EscapeJSON(itemName) + "\",";
      json += "\"refid\":\"" + EscapeJSON(refid) + "\",";
      json += "\"dist\":" + ToString(itemDist) + ",";
      json += "\"quantity\":" + ToString(quantity) + "}";
      nearbyItemCount++;
      if (nearbyItemCount >= 40) {
        break;
      }
    }

    // Fallback when Kenshi reports no on-ground items in range:
    // include nearest non-ground item handles so LLM still gets item context.
    if (nearbyItemCount == 0 && nearbyItems.size() > 0) {
      for (uint32_t i = 0; i < nearbyItems.size(); ++i) {
        Item *item = (Item *)nearbyItems.stuff[i];
        if (!item || (uintptr_t)item <= 0x1000) {
          continue;
        }

        std::string itemName = TrimCopy(item->getName());
        if (itemName.empty()) {
          continue;
        }

        unsigned int itemSerial = 0;
        try {
          itemSerial = item->getHandle().serial;
        } catch (...) {
          itemSerial = 0;
        }
        std::string dedupeKey =
            itemSerial > 0 ? ("id:" + ToString((int)itemSerial))
                           : ("name:" + itemName);
        for (size_t c = 0; c < dedupeKey.size(); ++c) {
          dedupeKey[c] = (char)std::tolower((unsigned char)dedupeKey[c]);
        }
        if (seenNearbyItems.count(dedupeKey)) {
          continue;
        }
        seenNearbyItems[dedupeKey] = true;

        float itemDist = npc->getPosition().distance(item->getPosition());
        int quantity = 1;
        try {
          quantity = item->quantity;
        } catch (...) {
          quantity = 1;
        }
        if (quantity < 1) {
          quantity = 1;
        }

        if (nearbyItemCount > 0) {
          json += ",";
        }
        std::string refid =
            itemSerial > 0 ? ("hand_" + ToString((int)itemSerial)) : "";
        json += "{\"name\":\"" + EscapeJSON(itemName) + "\",";
        json += "\"refid\":\"" + EscapeJSON(refid) + "\",";
        json += "\"dist\":" + ToString(itemDist) + ",";
        json += "\"quantity\":" + ToString(quantity) + "}";
        nearbyItemCount++;
        nearbyItemFallbackAdded++;
        if (nearbyItemCount >= 20) {
          break;
        }
      }
    }

    static DWORD s_lastNearbyItemDiagTick = 0;
    DWORD nearbyDiagNow = GetTickCount();
    if (nearbyDiagNow - s_lastNearbyItemDiagTick >= 5000) {
      s_lastNearbyItemDiagTick = nearbyDiagNow;
      Log("CONTEXT_NEARBY_ITEMS: actor=" + npc->getName() +
          " range=" + ToString((int)nearbyItemRange) +
          " raw=" + ToString((int)nearbyItems.size()) +
          " kept=" + ToString(nearbyItemCount) +
          " invalid=" + ToString(nearbyItemInvalid) +
          " not_ground=" + ToString(nearbyItemNotGround) +
          " noname=" + ToString(nearbyItemNoName) +
          " deduped=" + ToString(nearbyItemDuped) +
          " fallback_added=" + ToString(nearbyItemFallbackAdded));
    }
    json += "],";

    // Nearby buildings/locations/towns as prompt-friendly points of interest.
    json += "\"points_of_interest\": [";
    std::map<std::string, bool> seenPoi;
    int poiCount = 0;
    auto appendPoiResults = [&](lektor<RootObject *> &results,
                                const std::string &typeLabel) {
      for (uint32_t i = 0; i < results.size(); ++i) {
        RootObjectBase *poi = (RootObjectBase *)results.stuff[i];
        if (!poi || (uintptr_t)poi <= 0x1000) {
          continue;
        }

        std::string poiName = TrimCopy(poi->getName());
        if (poiName.empty() || IsUnknownToken(poiName)) {
          continue;
        }

        unsigned int poiSerial = poi->getHandle().serial;
        std::string dedupeKey = poiName + "|" + typeLabel + "|" + ToString(poiSerial);
        for (size_t c = 0; c < dedupeKey.size(); ++c) {
          dedupeKey[c] = (char)std::tolower((unsigned char)dedupeKey[c]);
        }
        if (seenPoi.count(dedupeKey)) {
          continue;
        }
        seenPoi[dedupeKey] = true;

        float poiDist = npc->getPosition().distance(poi->getPosition());
        if (poiCount > 0) {
          json += ",";
        }
        json += "{\"name\":\"" + EscapeJSON(poiName) + "\",";
        json += "\"refid\":\"hand_" + ToString(poiSerial) + "\",";
        json += "\"type\":\"" + EscapeJSON(typeLabel) + "\",";
        json += "\"dist\":" + ToString(poiDist) + "}";
        poiCount++;
        if (poiCount >= 32) {
          break;
        }
      }
    };

    float poiRange = g_visionRange;
    if (poiRange < 600.0f) {
      poiRange = 600.0f;
    }
    lektor<RootObject *> nearbyBuildings;
    world->getObjectsWithinSphere(nearbyBuildings, npc->getPosition(), poiRange,
                                  BUILDING, 64, (RootObject *)npc);
    appendPoiResults(nearbyBuildings, "building");

    std::string nearbyUseObjectsJson = "\"nearby_use_objects\": [";
    int useObjectCount = 0;
    std::map<unsigned int, bool> seenUseObjectSerials;
    std::map<unsigned int, int> useObjectOccupancy;
    lektor<RootObject *> nearbyCharactersForUseObjects;
    world->getObjectsWithinSphere(nearbyCharactersForUseObjects, npc->getPosition(),
                                  poiRange + 4.0f, CHARACTER, 128,
                                  (RootObject *)npc);
    for (uint32_t i = 0; i < nearbyCharactersForUseObjects.size(); ++i) {
      Character *other = (Character *)nearbyCharactersForUseObjects.stuff[i];
      if (!other || (uintptr_t)other <= 0x1000) {
        continue;
      }
      bool dead = false;
      try {
        dead = other->isDead();
      } catch (...) {
        dead = false;
      }
      if (dead) {
        continue;
      }

      unsigned int inWhatSerial = 0;
      try {
        if (other->inSomething != IN_NOTHING && other->inWhat.isValid() &&
            other->inWhat.serial != 0) {
          inWhatSerial = other->inWhat.serial;
        }
      } catch (...) {
        inWhatSerial = 0;
      }

      unsigned int turretSerial = 0;
      try {
        if (other->isUsingTurret.isValid() && other->isUsingTurret.serial != 0) {
          turretSerial = other->isUsingTurret.serial;
        }
      } catch (...) {
        turretSerial = 0;
      }

      if (inWhatSerial != 0) {
        useObjectOccupancy[inWhatSerial] += 1;
      }
      if (turretSerial != 0 && turretSerial != inWhatSerial) {
        useObjectOccupancy[turretSerial] += 1;
      }
    }

    for (uint32_t i = 0; i < nearbyBuildings.size(); ++i) {
      Building *building = (Building *)nearbyBuildings.stuff[i];
      if (!building || (uintptr_t)building <= 0x1000) {
        continue;
      }

      unsigned int buildingSerial = 0;
      try {
        buildingSerial = building->getHandle().serial;
      } catch (...) {
        buildingSerial = 0;
      }
      if (buildingSerial != 0 && seenUseObjectSerials.count(buildingSerial) > 0) {
        continue;
      }
      if (buildingSerial != 0) {
        seenUseObjectSerials[buildingSerial] = true;
      }

      BuildingClassType classType = BCTYPE_FLUFF;
      BuildingFunction functionType = BF_ANY;
      TaskType defaultTask = NULL_TASK;
      bool destroyed = false;
      bool broken = false;
      bool hasFreeSlot = false;
      try {
        classType = building->_NV_getBuildingClass();
      } catch (...) {
        classType = BCTYPE_FLUFF;
      }
      try {
        functionType = building->_NV_getSpecialFunction();
      } catch (...) {
        functionType = BF_ANY;
      }
      try {
        defaultTask = building->_NV_getDefaultTask();
      } catch (...) {
        defaultTask = NULL_TASK;
      }
      TaskType resolvedTask = ResolveUseObjectTask(defaultTask, functionType);
      try {
        destroyed = building->_NV_isDestroyed();
      } catch (...) {
        destroyed = false;
      }
      try {
        broken = building->_NV_isBroken();
      } catch (...) {
        broken = false;
      }
      try {
        building->forceValidUsageNodesValidation();
      } catch (...) {
      }
      try {
        hasFreeSlot = building->hasAnyGoodPositionMarkersLeft();
      } catch (...) {
        hasFreeSlot = false;
      }

      bool candidate = IsUseObjectClassCandidate(classType) ||
                       IsUseObjectFunctionCandidate(functionType) ||
                       resolvedTask != NULL_TASK;
      if (!candidate) {
        continue;
      }

      std::string buildingName = "";
      try {
        buildingName = TrimCopy(building->getName());
      } catch (...) {
        buildingName = "";
      }
      if (buildingName.empty()) {
        buildingName = "nearby object";
      }
      float buildingDist = npc->getPosition().distance(building->getPosition());
      int occupiedEstimate = 0;
      if (buildingSerial != 0 && useObjectOccupancy.count(buildingSerial) > 0) {
        occupiedEstimate = useObjectOccupancy[buildingSerial];
      }
      bool slotAvailableEstimate = hasFreeSlot || occupiedEstimate <= 0;
      bool usableNow = !destroyed && !broken && slotAvailableEstimate &&
                       resolvedTask != NULL_TASK;

      if (useObjectCount > 0) {
        nearbyUseObjectsJson += ",";
      }
      nearbyUseObjectsJson += "{\"name\":\"" + EscapeJSON(buildingName) + "\",";
      nearbyUseObjectsJson += "\"refid\":\"" +
                              EscapeJSON(buildingSerial > 0
                                             ? ("hand_" + ToString((int)buildingSerial))
                                             : "") +
                              "\",";
      nearbyUseObjectsJson += "\"serial\":" + ToString((int)buildingSerial) + ",";
      nearbyUseObjectsJson += "\"dist\":" + ToString(buildingDist) + ",";
      nearbyUseObjectsJson += "\"building_class\":\"" +
                              EscapeJSON(UseObjectClassLabel(classType)) + "\",";
      nearbyUseObjectsJson += "\"special_function\":\"" +
                              EscapeJSON(UseObjectFunctionLabel(functionType)) +
                              "\",";
      nearbyUseObjectsJson += "\"default_task\":\"" +
                              EscapeJSON(UseObjectTaskLabel(defaultTask)) + "\",";
      nearbyUseObjectsJson += "\"resolved_task\":\"" +
                              EscapeJSON(UseObjectTaskLabel(resolvedTask)) + "\",";
      nearbyUseObjectsJson +=
          "\"is_destroyed\":" + std::string(destroyed ? "true" : "false") + ",";
      nearbyUseObjectsJson +=
          "\"is_broken\":" + std::string(broken ? "true" : "false") + ",";
      nearbyUseObjectsJson +=
          "\"has_free_slot\":" +
          std::string(slotAvailableEstimate ? "true" : "false") + ",";
      nearbyUseObjectsJson +=
          "\"has_free_slot_probe\":" + std::string(hasFreeSlot ? "true" : "false") +
          ",";
      nearbyUseObjectsJson +=
          "\"occupied_estimate\":" + ToString(occupiedEstimate) + ",";
      nearbyUseObjectsJson +=
          "\"usable_now\":" + std::string(usableNow ? "true" : "false") + "}";
      useObjectCount++;
      if (useObjectCount >= 32) {
        break;
      }
    }
    nearbyUseObjectsJson += "],";

    json += "],";
    json += nearbyUseObjectsJson;
  }

  int bountyTotal = 0;
  std::string bountyPayload = BuildBountyPayload(npc, bountyTotal);
  if (bountyTotal > 0) {
    json += "\"bounty\": " + ToString(bountyTotal) + ",";
  }
  if (bountyPayload != "{}") {
    json += "\"bounty_info\": " + bountyPayload + ",";
  }

  int money = npc->getMoney();
  if (money <= 0 && npc->getOwnerships())
    money = npc->getOwnerships()->getMoney();
  json += "\"money\": " + ToString(money) + ",";

  if (type == "player" && world && world->player) {
    json += "\"squad\": [";
    for (uint32_t i = 0; i < world->player->playerCharacters.size(); ++i) {
      if (i > 0)
        json += ",";
      json += "\"" + EscapeJSON(world->player->playerCharacters[i]->getName()) +
              "\"";
    }
    json += "],";
  }

  std::string statsPayload = BuildStatsPayload(npc);
  if (statsPayload != "{}") {
    json += "\"stats\": " + statsPayload + ",";
  }

  std::string medicalPayload = BuildMedicalPayload(npc);
  if (medicalPayload != "{}") {
    json += "\"medical\": " + medicalPayload + ",";
  }
  bool hasRoboticLimbs = false;
  std::string roboticLimbPayload = BuildRoboticLimbPayload(npc, hasRoboticLimbs);
  json += "\"has_robotic_limbs\": " +
          std::string(hasRoboticLimbs ? "true" : "false") + ",";
  if (hasRoboticLimbs) {
    json += "\"robotic_limbs\": " + roboticLimbPayload + ",";
  }

  const auto npcPos = npc->getPosition();

  json += "\"environment\": {";
  json += "\"indoors\": " + std::string(npcIsIndoors ? "true" : "false") + ",";
  json += "\"outdoors\": " + std::string(npcIsIndoors ? "false" : "true") + ",";
  json += "\"building_serial\": " + ToString((int)npcBuildingSerial) + ",";
  json += "\"building_name\": \"" + EscapeJSON(npcBuildingName) + "\",";
  json += "\"floor\": " + ToString(npcFloor) + ",";
  json += "\"in_town\": " +
          std::string(inTownWalls ? "true" : "false") + ",";
  json += "\"town_name\": \"" + EscapeJSON(townName) + "\",";
  json += "\"zone_name\": \"" + EscapeJSON(zonePromptName) + "\",";
  json += "\"region_name\": \"" + EscapeJSON(regionPromptName) + "\",";
  json += "\"region\": \"" + EscapeJSON(regionPromptName) + "\",";
  json += "\"x\": " + ToString(npcPos.x) + ",";
  json += "\"y\": " + ToString(npcPos.y) + ",";
  json += "\"z\": " + ToString(npcPos.z) + ",";
  json += "\"weather\": " + ToString((int)npc->getCurrentWeatherAffectStatus()) + ",";
  json += BuildNamedWeatherContext(npc, npcPos);
  json += "},";
  json += "\"town\": \"" + EscapeJSON(townName) + "\",";
  json += "\"zone\": \"" + EscapeJSON(zonePromptName) + "\",";
  json += "\"region\": \"" + EscapeJSON(regionPromptName) + "\",";

  std::string inventoryJson = "[]";
  // Hotfix: do not walk live inventory during context build.
  // This path is intentionally cache-only until we reintroduce safe sampling.
  EnterCriticalSection(&g_stateMutex);
  if (npc->getHandle() == g_lastInventoryHand && !g_activeInventoryJson.empty()) {
    inventoryJson = g_activeInventoryJson;
  } else if ((isPlayerCharacter || type == "player" ||
              npc->getHandle() == g_playerHand) &&
             !g_playerInventoryJson.empty()) {
    inventoryJson = g_playerInventoryJson;
  }
  LeaveCriticalSection(&g_stateMutex);
  json += "\"inventory\": " + inventoryJson + ",";

  json += "\"events\": [";
  EnterCriticalSection(&g_eventMutex);
  int eventCount = 0;
  for (int i = (int)g_gameEvents.size() - 1; i >= 0 && eventCount < 30;
       --i, ++eventCount) {
    if (eventCount > 0)
      json += ",";
    json += "{\"type\": \"" + EscapeJSON(g_gameEvents[i].type) + "\",";
    json += "\"actor\": \"" + EscapeJSON(g_gameEvents[i].actor) + "\",";
    json += "\"actor_faction\": \"" + EscapeJSON(g_gameEvents[i].actorFaction) +
            "\",";
    json += "\"target\": \"" + EscapeJSON(g_gameEvents[i].target) + "\",";
    json += "\"target_faction\": \"" +
            EscapeJSON(g_gameEvents[i].targetFaction) + "\",";
    json += "\"msg\": \"" + EscapeJSON(g_gameEvents[i].message) + "\",";
    json += "\"age\": " +
            ToString((int)(GetTickCount() - g_gameEvents[i].timestamp) / 1000) +
            "}";
  }
  LeaveCriticalSection(&g_eventMutex);
  json += "],";

  if (world && world->player && world->player->playerCharacters.size() > 0) {
    Character *player = world->player->playerCharacters[0];
    json += "\"memories\": { \"short_term\": [";
    bool first = true;
    for (int i = 1; i < 8; ++i) {
      if (npc->getCharacterMemoryTag(player,
                                     (CharacterPerceptionTags_ShortTerm)i)) {
        if (!first)
          json += ",";
        json += ToString(i);
        first = false;
      }
    }
    json += "], \"long_term\": [";
    first = true;
    for (int i = 1; i < 17; ++i) {
      if (npc->getCharacterMemoryTag(player,
                                     (CharacterPerceptionTags_LongTerm)i)) {
        if (!first)
          json += ",";
        json += ToString(i);
        first = false;
      }
    }
    json += "] }";
  } else {
    json += "\"memories\": {}";
  }

  json += "}";
  return json;
}

std::string BuildWorldEventDigest() {
  // World event synthesis is now server-side in StobeServer.
  return "World events are managed by StobeServer.";
}
