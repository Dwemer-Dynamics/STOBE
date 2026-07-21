#pragma once

#include <kenshi/RootObject.h>

// Town.h conflicts with Platoon.h in this SDK revision. Keep this shim limited
// to exported non-virtual calls so it does not depend on TownBase vtable slots.
class TownBase : public RootObject {
public:
  std::string getKnownName();
  bool _NV_isDiscovered() const;
  bool _NV_isExplored() const;
};

class TownList {
public:
  lektor<RootObject *> &getAllTowns();
};
