#pragma once

#include <kenshi/Enums.h>
#include <kenshi/RootObject.h>

enum BuildingFunction {
  BF_ANY,
  BF_MINE,
  BF_RESOURCE_STORAGE,
  BF_RESEARCH,
  BF_REFINERY,
  BF_GENERATOR,
  BF_BED,
  BF_TRAINING,
  BF_CAGE,
  BF_SHOP,
  BF_CRAFTING,
  BF_CORPSE_DISPOSAL,
  BF_TURRET,
  BF_GENERAL_STORAGE,
  BF_ITEM_FURNACE,
  BF_LIGHT,
  BF_TABLE,
  BF_CHAIR,
  BF_FLUFF,
  BF_SHELL_WITH_INTERIOR,
  BF_WALL,
  BF_GATE,
  BF_DOOR,
  BF_BATTERY,
  BF_THRONE,
  BF_SKELETON_BED,
  BF_RAIN_COLLECTOR,
  BF_MINE_NATURAL,
  BF_STEERING,
  BF_ENGINE,
  BF_LIQUID_TANK
};

enum BuildingClassType {
  BCTYPE_FLUFF,
  BCTYPE_DOOR,
  BCTYPE_USABLE,
  BCTYPE_STORAGE,
  BCTYPE_PRODUCTION,
  BCTYPE_RESEARCH,
  BCTYPE_CRAFTING,
  BCTYPE_GATEWAY,
  BCTYPE_TURRET,
  BCTYPE_WALL,
  BCTYPE_ITEM_FURNACE,
  BCTYPE_LIGHT,
  BCTYPE_SHELL_WITH_INTERIOR,
  BCTYPE_FARM
};

class Building : public RootObject {
public:
  BuildingFunction _NV_getSpecialFunction() const;
  bool _NV_isBroken() const;
  bool _NV_isDestroyed() const;
  BuildingClassType _NV_getBuildingClass() const;
  TaskType _NV_getDefaultTask();
  void forceValidUsageNodesValidation();
  bool hasAnyGoodPositionMarkersLeft();
};
