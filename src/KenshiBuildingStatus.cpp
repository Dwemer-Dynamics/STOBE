#include "KenshiBuildingStatus.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstring>
#include <kenshi/gui/InventoryGUI.h>
#include <kenshi/Building/Building.h>
#include <kenshi/Building/FarmBuilding.h>
#include <kenshi/Building/ProductionBuilding.h>
#include <kenshi/Building/StorageBuilding.h>
#include <kenshi/Building/UseableStuff.h>

namespace {
float Clamp01(float value) {
  if (value < 0.0f) {
    return 0.0f;
  }
  return value > 1.0f ? 1.0f : value;
}

void CopyName(const std::string &value, char *output, size_t capacity) {
  if (!output || capacity == 0) {
    return;
  }
  const size_t length =
      value.size() < capacity - 1 ? value.size() : capacity - 1;
  if (length > 0) {
    std::memcpy(output, value.data(), length);
  }
  output[length] = '\0';
}
} // namespace

bool ReadBuildingRuntimeStatus(void *buildingPointer, char *name,
                               size_t nameCapacity,
                               BuildingRuntimeStatus *status) {
  if (!buildingPointer || !status) {
    return false;
  }

  std::memset(status, 0, sizeof(*status));
  Building *building = static_cast<Building *>(buildingPointer);
  CopyName(building->getName(), name, nameCapacity);

  Building::ConstructionState *construction = building->getBuildState();
  if (construction) {
    status->constructionAvailable = true;
    status->constructionComplete = construction->isComplete;
    status->constructionPaused = construction->isPaused;
    status->constructionDismantled = construction->isDismantled;
    status->constructionMissingMaterials = construction->needMats();
    status->constructionProgress =
        Clamp01(construction->getHealthBarProgress());
  }

  UseableStuff *useable = building->getUseableStuff();
  if (useable) {
    status->useableAvailable = true;
    status->powerOn = useable->isPowerOn();
    status->needsPower = useable->howMuchPowerDoYouWantMax() > 0.0f;
    status->outOfPower = useable->isOutOfPower() > 0.0f;
    status->occupied = useable->getOccupant().isValid();
    status->powerOutput = useable->getPowerOutput();
  }

  StorageBuilding *storage = building->getFunctionStuff();
  if (storage) {
    status->storageAvailable = true;
    status->storageEmpty = storage->isProductionEmpty();
    status->storageFull = storage->isAnyInputsFull();
  }
  return true;
}

bool ReadProductionRuntimeStatus(void *buildingPointer,
                                 ProductionRuntimeStatus *status) {
  if (!buildingPointer || !status) {
    return false;
  }

  std::memset(status, 0, sizeof(*status));
  Building *building = static_cast<Building *>(buildingPointer);
  const BuildingClassType classType = building->getBuildingClass();
  if (classType != BCTYPE_PRODUCTION && classType != BCTYPE_CRAFTING &&
      classType != BCTYPE_ITEM_FURNACE && classType != BCTYPE_FARM) {
    return false;
  }

  ProductionBuilding *production =
      static_cast<ProductionBuilding *>(building);
  status->available = true;
  status->farm = classType == BCTYPE_FARM;
  status->inputBlocked = production->isAnyInputsEmpty();
  status->outputBlocked = production->isProductionFull();
  status->idle = production->dontNeedWorkRightNow();
  const float efficiency = production->getProductionMultForGUI();
  status->efficiency = efficiency > 0.0f ? efficiency : 0.0f;

  UseableStuff *useable = production->getUseableStuff();
  if (useable) {
    status->unpowered = useable->isOutOfPower() > 0.0f;
    status->staffed = useable->getOccupant().isValid();
  }

  if (status->farm) {
    FarmBuilding *farm = static_cast<FarmBuilding *>(building);
    status->inputBlocked = farm->isAnyInputsEmpty();
    status->outputBlocked = farm->isProductionFull();
    status->idle = farm->dontNeedWorkRightNow();
    status->yield = Clamp01(farm->getYieldChancePerCrop(1.0f));
    status->hydroponic = farm->isHydroponic;
  }
  return true;
}
