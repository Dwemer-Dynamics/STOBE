#pragma once

#include <kenshi/Enums.h>
#include <kenshi/RootObject.h>

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
