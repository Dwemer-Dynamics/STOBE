#include "KenshiTownIdentity.h"

#include <cstring>
#include <kenshi/Town.h>

bool ReadTownInstanceIdentity(void *townPointer, char *uid, size_t uidCapacity,
                              int *baseIndex, int *modIndex) {
  if (!townPointer || !uid || uidCapacity == 0 || !baseIndex || !modIndex) {
    return false;
  }

  Town *town = static_cast<Town *>(townPointer);
  const InstanceID &identity = town->instanceID;
  if (identity.uid.empty()) {
    return false;
  }

  strncpy_s(uid, uidCapacity, identity.uid.c_str(), _TRUNCATE);
  *baseIndex = static_cast<int>(identity.baseIndex);
  *modIndex = static_cast<int>(identity.modIndex);
  return uid[0] != '\0';
}

bool ReadTownRuntimeStatus(void *townPointer, int *alarmState, float *radius) {
  if (!townPointer || !alarmState || !radius) {
    return false;
  }

  Town *town = static_cast<Town *>(townPointer);
  *alarmState = static_cast<int>(town->getAlarmState());
  *radius = town->getRadius();
  return true;
}
