#pragma once

#include <kenshi/RootObject.h>

class GameData;

// Town.h conflicts with Platoon.h in this SDK revision. Keep this shim limited
// to exported non-virtual calls so it does not depend on TownBase vtable slots.
class TownBase : public RootObject {
public:
  std::string getKnownName();
  bool _NV_isDiscovered() const;
  bool _NV_isExplored() const;
  bool withinBordersRange(const Ogre::Vector3 &position, float multiplier) const;
};

class Town : public TownBase {
public:
  GameData *getOriginalGameData() const;
  float getRequiredPower() const;
  float getTotalPower() const;
  bool hasSparePower() const;
  float getBatteryDrain() const;
  float getBatteryChargeMax() const;
  float getBatteryCharge() const;
  float getBatteryChargingUpAmount() const;
  bool isBatteryMode() const;
  bool _NV_hasGates();
  bool _NV_gatesAllClosed();
};

class TownList {
public:
  lektor<RootObject *> &getAllTowns();
  TownBase *getNearestWithinItsRadius(const Ogre::Vector3 &position,
                                      bool skipPlayerTowns) const;
};
