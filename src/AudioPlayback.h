#pragma once

#include <string>

bool QueueTtsPlayback(const std::string &ttsHash, int volumePercentOverride = -1,
                      unsigned int speakerSerial = 0);
bool IsTtsPlaybackActive();
int GetTtsPlaybackRemainingMs();
void InterruptTtsPlayback();
