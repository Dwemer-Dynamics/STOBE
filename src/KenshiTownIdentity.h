#pragma once

#include <cstddef>

// Reads Town::instanceID in a translation unit isolated from Platoon.h.
bool ReadTownInstanceIdentity(void *town, char *uid, size_t uidCapacity,
                              int *baseIndex, int *modIndex);

// Reads virtual Town methods in the translation unit that owns the full ABI.
bool ReadTownRuntimeStatus(void *town, int *alarmState, float *radius);
