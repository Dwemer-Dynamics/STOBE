#pragma once

#include <string>

bool QueueTtsPlayback(const std::string &ttsHash, int volumePercentOverride = -1,
                      unsigned int speakerSerial = 0,
                      float playbackSpeedMultiplier = 1.0f);
bool IsTtsPlaybackActive();
int GetTtsPlaybackRemainingMs();
void InterruptTtsPlayback();
