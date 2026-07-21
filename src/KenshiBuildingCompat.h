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

class StorageBuilding;
class UseableStuff;

class Building : public RootObject {
public:
  BuildingFunction _NV_getSpecialFunction() const;
  bool _NV_isBroken() const;
  bool _NV_isDestroyed() const;
  BuildingClassType _NV_getBuildingClass() const;
  TaskType _NV_getDefaultTask();
  StorageBuilding *_NV_getFunctionStuff();
  UseableStuff *_NV_getUseableStuff();
  bool _NV_isPowerOn() const;
  void forceValidUsageNodesValidation();
  bool hasAnyGoodPositionMarkersLeft();
};

class UseableStuff : public Building {
public:
  UseableStuff *_NV_getUseableStuff();
  bool _NV_dontNeedWorkRightNow() const;
  float _NV_getPowerOutput() const;
  bool _NV_isPowerOn() const;
  TaskType _NV_getDefaultTask();
};

class StorageBuilding : public UseableStuff {
public:
  StorageBuilding *_NV_getFunctionStuff();
  TaskType _NV_getDefaultTask();
  bool _NV_isAnyInputsEmpty() const;
  bool _NV_isAnyInputsFull();
  bool _NV_isProductionFull();
  bool _NV_isProductionEmpty();
};

class ProductionBuilding : public StorageBuilding {
public:
  TaskType _NV_getDefaultTask();
  float getOutput() const;
};

class CraftingBuilding : public ProductionBuilding {
public:
  bool _NV_hasCraftingQueued() const;
};

class GeneratorBuilding : public ProductionBuilding {
public:
  float _NV_getPowerOutput() const;
};

class ResearchBuilding : public UseableStuff {
public:
  bool _NV_dontNeedWorkRightNow() const;
  TaskType _NV_getDefaultTask();
};

class FarmBuilding : public ProductionBuilding {
public:
  bool _NV_dontNeedWorkRightNow() const;
};

class TurretBuilding : public UseableStuff {
public:
  TaskType _NV_getDefaultTask();
};
