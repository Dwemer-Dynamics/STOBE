#pragma once

#include <cstddef>

struct BuildingRuntimeStatus {
  bool constructionAvailable;
  bool constructionComplete;
  bool constructionPaused;
  bool constructionDismantled;
  bool constructionMissingMaterials;
  float constructionProgress;
  bool useableAvailable;
  bool powerOn;
  bool needsPower;
  bool outOfPower;
  bool occupied;
  float powerOutput;
  bool storageAvailable;
  bool storageEmpty;
  bool storageFull;
};

struct ProductionRuntimeStatus {
  bool available;
  bool farm;
  bool inputBlocked;
  bool outputBlocked;
  bool idle;
  bool unpowered;
  bool staffed;
  float efficiency;
  float yield;
  bool hydroponic;
};

// Reads virtual building methods in a translation unit that owns the full ABI.
bool ReadBuildingRuntimeStatus(void *building, char *name, size_t nameCapacity,
                               BuildingRuntimeStatus *status);
bool ReadProductionRuntimeStatus(void *building,
                                 ProductionRuntimeStatus *status);
