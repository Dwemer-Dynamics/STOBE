#pragma once

#include <string>

enum TtsPlaybackOwner {
  TTS_PLAYBACK_OWNER_DEFAULT = 0,
  TTS_PLAYBACK_OWNER_DIALOGUE_MENU = 1
};

bool QueueTtsPlayback(const std::string &ttsHash, int volumePercentOverride = -1,
                      unsigned int speakerSerial = 0,
                      float playbackSpeedMultiplier = 1.0f,
                      int owner = TTS_PLAYBACK_OWNER_DEFAULT);
bool IsTtsPlaybackActive();
int GetTtsPlaybackRemainingMs();
void InterruptTtsPlayback();
bool InterruptTtsPlaybackIfOwner(int owner);
