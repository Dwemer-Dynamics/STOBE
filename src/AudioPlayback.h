#pragma once

#include <string>

bool QueueTtsPlayback(const std::string &ttsHash);
bool IsTtsPlaybackActive();
int GetTtsPlaybackRemainingMs();
void InterruptTtsPlayback();
