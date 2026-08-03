#include "AudioPlayback.h"

#include "Comm.h"
#include "Globals.h"
#include "Utils.h"

#include <windows.h>
#include <mmsystem.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

namespace {
struct TtsPlaybackTask {
  std::string hash;
  LONG generation;
  int volumePercentOverride;
  unsigned int speakerSerial;
  float playbackSpeedMultiplier;
  int owner;
};

LONG g_ttsPlaybackBusy = 0;
LONG g_ttsPlaybackGeneration = 1;
LONG g_ttsPlaybackOwner = TTS_PLAYBACK_OWNER_DEFAULT;
DWORD g_ttsPlaybackEndTick = 0;

int RoundToInt(float value) {
  return static_cast<int>(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

long long RoundToLongLong(double value) {
  return static_cast<long long>(value >= 0.0 ? value + 0.5 : value - 0.5);
}

LONG CurrentTtsPlaybackGeneration() {
  return InterlockedCompareExchange(&g_ttsPlaybackGeneration, 0, 0);
}

void ReleasePlaybackSlot(LONG generation) {
  if (generation != CurrentTtsPlaybackGeneration()) {
    return;
  }
  g_ttsPlaybackEndTick = 0;
  InterlockedExchange(&g_ttsPlaybackOwner, TTS_PLAYBACK_OWNER_DEFAULT);
  InterlockedExchange(&g_ttsPlaybackBusy, 0);
}

unsigned int ReadLe32(const unsigned char *bytes) {
  if (!bytes) {
    return 0;
  }
  return static_cast<unsigned int>(bytes[0]) |
         (static_cast<unsigned int>(bytes[1]) << 8) |
         (static_cast<unsigned int>(bytes[2]) << 16) |
         (static_cast<unsigned int>(bytes[3]) << 24);
}

void WriteLe32(unsigned char *bytes, unsigned int value) {
  if (!bytes) {
    return;
  }
  bytes[0] = static_cast<unsigned char>(value & 0xFF);
  bytes[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
  bytes[2] = static_cast<unsigned char>((value >> 16) & 0xFF);
  bytes[3] = static_cast<unsigned char>((value >> 24) & 0xFF);
}

std::string ShortHashForLog(const std::string &hash) {
  if (hash.length() <= 8) {
    return hash;
  }
  return hash.substr(0, 8);
}

int ClampVolumePercent(int volumePercent) {
  if (volumePercent < 0) {
    return 0;
  }
  if (volumePercent > 100) {
    return 100;
  }
  return volumePercent;
}

int EstimateWavDurationMs(const std::string &wavData) {
  if (wavData.size() < 12 || wavData.compare(0, 4, "RIFF") != 0 ||
      wavData.compare(8, 4, "WAVE") != 0) {
    return 0;
  }

  const unsigned char *bytes =
      reinterpret_cast<const unsigned char *>(wavData.data());
  unsigned short channels = 0;
  unsigned int sampleRate = 0;
  unsigned short bitsPerSample = 0;
  unsigned int dataSize = 0;

  size_t chunkPos = 12;
  while (chunkPos + 8 <= wavData.size()) {
    unsigned int chunkSize = static_cast<unsigned int>(
        bytes[chunkPos + 4] | (bytes[chunkPos + 5] << 8) |
        (bytes[chunkPos + 6] << 16) | (bytes[chunkPos + 7] << 24));
    size_t chunkDataPos = chunkPos + 8;
    size_t paddedChunkSize =
        static_cast<size_t>(chunkSize) + static_cast<size_t>(chunkSize & 1U);
    if (chunkDataPos + paddedChunkSize > wavData.size()) {
      break;
    }

    bool isFmt = bytes[chunkPos] == 'f' && bytes[chunkPos + 1] == 'm' &&
                 bytes[chunkPos + 2] == 't' && bytes[chunkPos + 3] == ' ';
    bool isData = bytes[chunkPos] == 'd' && bytes[chunkPos + 1] == 'a' &&
                  bytes[chunkPos + 2] == 't' && bytes[chunkPos + 3] == 'a';

    if (isFmt && chunkSize >= 16) {
      channels = static_cast<unsigned short>(bytes[chunkDataPos + 2] |
                                             (bytes[chunkDataPos + 3] << 8));
      sampleRate = static_cast<unsigned int>(
          bytes[chunkDataPos + 4] | (bytes[chunkDataPos + 5] << 8) |
          (bytes[chunkDataPos + 6] << 16) | (bytes[chunkDataPos + 7] << 24));
      bitsPerSample = static_cast<unsigned short>(
          bytes[chunkDataPos + 14] | (bytes[chunkDataPos + 15] << 8));
    } else if (isData) {
      dataSize = chunkSize;
    }

    chunkPos = chunkDataPos + paddedChunkSize;
  }

  if (channels == 0 || sampleRate == 0 || bitsPerSample == 0 || dataSize == 0) {
    return 0;
  }

  double bytesPerSecond =
      static_cast<double>(channels) * static_cast<double>(sampleRate) *
      (static_cast<double>(bitsPerSample) / 8.0);
  if (bytesPerSecond <= 0.0) {
    return 0;
  }

  int durationMs = static_cast<int>((static_cast<double>(dataSize) / bytesPerSecond) * 1000.0);
  if (durationMs < 0) {
    durationMs = 0;
  } else if (durationMs > 600000) {
    durationMs = 600000;
  }
  return durationMs;
}

void ApplyWavSpeedMultiplierInPlace(std::string &wavData,
                                    float playbackSpeedMultiplier) {
  if (wavData.size() < 44 || wavData.compare(0, 4, "RIFF") != 0 ||
      wavData.compare(8, 4, "WAVE") != 0) {
    return;
  }
  if (!(playbackSpeedMultiplier > 1.0f)) {
    return;
  }
  if (playbackSpeedMultiplier > 3.0f) {
    playbackSpeedMultiplier = 3.0f;
  }

  unsigned char *bytes = reinterpret_cast<unsigned char *>(&wavData[0]);
  size_t chunkPos = 12;
  while (chunkPos + 8 <= wavData.size()) {
    unsigned int chunkSize = ReadLe32(bytes + chunkPos + 4);
    size_t chunkDataPos = chunkPos + 8;
    size_t paddedChunkSize =
        static_cast<size_t>(chunkSize) + static_cast<size_t>(chunkSize & 1U);
    if (chunkDataPos + paddedChunkSize > wavData.size()) {
      break;
    }

    bool isFmt = bytes[chunkPos] == 'f' && bytes[chunkPos + 1] == 'm' &&
                 bytes[chunkPos + 2] == 't' && bytes[chunkPos + 3] == ' ';
    if (isFmt && chunkSize >= 16) {
      unsigned int sampleRate = ReadLe32(bytes + chunkDataPos + 4);
      unsigned int byteRate = ReadLe32(bytes + chunkDataPos + 8);
      if (sampleRate == 0 || byteRate == 0) {
        return;
      }

      double scaledSampleRate =
          static_cast<double>(sampleRate) * playbackSpeedMultiplier;
      if (scaledSampleRate < 1.0) {
        scaledSampleRate = 1.0;
      } else if (scaledSampleRate > 384000.0) {
        scaledSampleRate = 384000.0;
      }
      double scaledByteRate =
          static_cast<double>(byteRate) * playbackSpeedMultiplier;
      if (scaledByteRate < 1.0) {
        scaledByteRate = 1.0;
      } else if (scaledByteRate > 4294967295.0) {
        scaledByteRate = 4294967295.0;
      }

      WriteLe32(bytes + chunkDataPos + 4,
                static_cast<unsigned int>(scaledSampleRate + 0.5));
      WriteLe32(bytes + chunkDataPos + 8,
                static_cast<unsigned int>(scaledByteRate + 0.5));
      return;
    }
    chunkPos = chunkDataPos + paddedChunkSize;
  }
}

void ApplyWavVolumeInPlace(std::string &wavData, int volumePercent) {
  if (wavData.size() < 44 || wavData.compare(0, 4, "RIFF") != 0 ||
      wavData.compare(8, 4, "WAVE") != 0) {
    return;
  }
  if (volumePercent >= 100) {
    return;
  }
  if (volumePercent < 0) {
    volumePercent = 0;
  }

  unsigned char *bytes = reinterpret_cast<unsigned char *>(&wavData[0]);
  unsigned short audioFormat = 0;
  unsigned short bitsPerSample = 0;
  size_t dataPos = 0;
  unsigned int dataSize = 0;

  size_t chunkPos = 12;
  while (chunkPos + 8 <= wavData.size()) {
    unsigned int chunkSize = static_cast<unsigned int>(
        bytes[chunkPos + 4] | (bytes[chunkPos + 5] << 8) |
        (bytes[chunkPos + 6] << 16) | (bytes[chunkPos + 7] << 24));
    size_t chunkDataPos = chunkPos + 8;
    size_t paddedChunkSize =
        static_cast<size_t>(chunkSize) + static_cast<size_t>(chunkSize & 1U);
    if (chunkDataPos + paddedChunkSize > wavData.size()) {
      break;
    }

    bool isFmt = bytes[chunkPos] == 'f' && bytes[chunkPos + 1] == 'm' &&
                 bytes[chunkPos + 2] == 't' && bytes[chunkPos + 3] == ' ';
    bool isData = bytes[chunkPos] == 'd' && bytes[chunkPos + 1] == 'a' &&
                  bytes[chunkPos + 2] == 't' && bytes[chunkPos + 3] == 'a';

    if (isFmt && chunkSize >= 16) {
      audioFormat = static_cast<unsigned short>(
          bytes[chunkDataPos] | (bytes[chunkDataPos + 1] << 8));
      bitsPerSample = static_cast<unsigned short>(
          bytes[chunkDataPos + 14] | (bytes[chunkDataPos + 15] << 8));
    } else if (isData) {
      dataPos = chunkDataPos;
      dataSize = chunkSize;
    }

    chunkPos = chunkDataPos + paddedChunkSize;
  }

  if (bitsPerSample == 0 || dataPos == 0 || dataSize == 0 ||
      dataPos + dataSize > wavData.size()) {
    return;
  }

  // IEEE float PCM.
  if (audioFormat == 3) {
    if (bitsPerSample == 32) {
      if (volumePercent == 0) {
        memset(bytes + dataPos, 0, dataSize);
        return;
      }
      const float gain = static_cast<float>(volumePercent) / 100.0f;
      for (size_t i = dataPos; i + 3 < dataPos + dataSize; i += 4) {
        float sample;
        memcpy(&sample, bytes + i, sizeof(float));
        if (sample != sample) {
          sample = 0.0f;
        }
        float out = sample * gain;
        if (out > 1.0f) {
          out = 1.0f;
        } else if (out < -1.0f) {
          out = -1.0f;
        }
        memcpy(bytes + i, &out, sizeof(float));
      }
      return;
    }
    if (bitsPerSample == 64) {
      if (volumePercent == 0) {
        memset(bytes + dataPos, 0, dataSize);
        return;
      }
      const double gain = static_cast<double>(volumePercent) / 100.0;
      for (size_t i = dataPos; i + 7 < dataPos + dataSize; i += 8) {
        double sample;
        memcpy(&sample, bytes + i, sizeof(double));
        if (sample != sample) {
          sample = 0.0;
        }
        double out = sample * gain;
        if (out > 1.0) {
          out = 1.0;
        } else if (out < -1.0) {
          out = -1.0;
        }
        memcpy(bytes + i, &out, sizeof(double));
      }
      return;
    }
    return;
  }

  // Integer PCM only below.
  if (audioFormat != 1) {
    return;
  }

  if (volumePercent == 0) {
    if (bitsPerSample == 8) {
      memset(bytes + dataPos, 128, dataSize);
    } else {
      memset(bytes + dataPos, 0, dataSize);
    }
    return;
  }

  const float gain = static_cast<float>(volumePercent) / 100.0f;
  if (bitsPerSample == 8) {
    for (size_t i = dataPos; i < dataPos + dataSize; ++i) {
      int centered = static_cast<int>(bytes[i]) - 128;
      int scaled = RoundToInt(static_cast<float>(centered) * gain);
      int out = scaled + 128;
      if (out < 0) {
        out = 0;
      } else if (out > 255) {
        out = 255;
      }
      bytes[i] = static_cast<unsigned char>(out);
    }
    return;
  }

  if (bitsPerSample == 16) {
    for (size_t i = dataPos; i + 1 < dataPos + dataSize; i += 2) {
      int sample = static_cast<int>(static_cast<int16_t>(
          bytes[i] | (static_cast<int>(bytes[i + 1]) << 8)));
      int scaled = RoundToInt(static_cast<float>(sample) * gain);
      if (scaled > 32767) {
        scaled = 32767;
      } else if (scaled < -32768) {
        scaled = -32768;
      }
      bytes[i] = static_cast<unsigned char>(scaled & 0xFF);
      bytes[i + 1] = static_cast<unsigned char>((scaled >> 8) & 0xFF);
    }
    return;
  }

  if (bitsPerSample == 24) {
    for (size_t i = dataPos; i + 2 < dataPos + dataSize; i += 3) {
      int sample = (bytes[i] | (bytes[i + 1] << 8) | (bytes[i + 2] << 16));
      if (sample & 0x800000) {
        sample |= ~0xFFFFFF;
      }
      int scaled = RoundToInt(static_cast<float>(sample) * gain);
      if (scaled > 8388607) {
        scaled = 8388607;
      } else if (scaled < -8388608) {
        scaled = -8388608;
      }
      bytes[i] = static_cast<unsigned char>(scaled & 0xFF);
      bytes[i + 1] = static_cast<unsigned char>((scaled >> 8) & 0xFF);
      bytes[i + 2] = static_cast<unsigned char>((scaled >> 16) & 0xFF);
    }
    return;
  }

  if (bitsPerSample == 32) {
    for (size_t i = dataPos; i + 3 < dataPos + dataSize; i += 4) {
      int sample = static_cast<int>(
          bytes[i] | (bytes[i + 1] << 8) | (bytes[i + 2] << 16) |
          (bytes[i + 3] << 24));
      long long scaled = RoundToLongLong(static_cast<double>(sample) *
                                         static_cast<double>(gain));
      if (scaled > 2147483647LL) {
        scaled = 2147483647LL;
      } else if (scaled < -2147483648LL) {
        scaled = -2147483648LL;
      }
      int out = static_cast<int>(scaled);
      bytes[i] = static_cast<unsigned char>(out & 0xFF);
      bytes[i + 1] = static_cast<unsigned char>((out >> 8) & 0xFF);
      bytes[i + 2] = static_cast<unsigned char>((out >> 16) & 0xFF);
      bytes[i + 3] = static_cast<unsigned char>((out >> 24) & 0xFF);
    }
  }
}

bool IsHexHash(const std::string &value) {
  if (value.length() != 32) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(value[i]);
    bool isHex =
        (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    if (!isHex) {
      return false;
    }
  }
  return true;
}

bool EnsureDirectoryRecursive(const std::string &path) {
  if (path.empty()) {
    return false;
  }

  std::string current;
  current.reserve(path.size());
  for (size_t i = 0; i < path.size(); ++i) {
    const char ch = path[i];
    current.push_back(ch);
    if (ch != '\\' && ch != '/') {
      continue;
    }
    if (current.length() <= 3) {
      continue;
    }
    CreateDirectoryA(current.c_str(), NULL);
  }

  if (CreateDirectoryA(path.c_str(), NULL)) {
    return true;
  }
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string ResolveTtsCacheFilePath(const std::string &hash) {
  char tempPath[MAX_PATH] = {0};
  DWORD len = GetTempPathA(MAX_PATH, tempPath);
  std::string base = (len > 0 && len < MAX_PATH) ? std::string(tempPath, len) : ".\\";

  std::string dir = base + "Stobe\\tts_cache";
  if (!EnsureDirectoryRecursive(dir)) {
    return "";
  }

  return dir + "\\" + hash + ".wav";
}

DWORD WINAPI PlaybackThreadProc(LPVOID lpParam) {
  TtsPlaybackTask *task = static_cast<TtsPlaybackTask *>(lpParam);
  if (!task) {
    return 0;
  }

  std::string hash = task->hash;
  LONG generation = task->generation;
  int volumePercentOverride = task->volumePercentOverride;
  unsigned int speakerSerial = task->speakerSerial;
  float playbackSpeedMultiplier = task->playbackSpeedMultiplier;
  int owner = task->owner;
  delete task;
  DWORD threadStartTick = GetTickCount();

  if (!IsHexHash(hash)) {
    ReleasePlaybackSlot(generation);
    return 0;
  }
  if (generation != CurrentTtsPlaybackGeneration()) {
    return 0;
  }

  std::wstring endpoint = L"/StobeServer/soundcache/" + ToWide(UrlEncode(hash + ".wav"));
  std::string wavData = PostToStobeWithResponse(endpoint, "");
  DWORD downloadMs = GetTickCount() - threadStartTick;
  if (generation != CurrentTtsPlaybackGeneration()) {
    return 0;
  }
  if (wavData.size() < 44 || wavData.compare(0, 4, "RIFF") != 0) {
    Log("TTS_PLAYBACK: invalid WAV payload hash=" + ShortHashForLog(hash) +
        " bytes=" + ToString((int)wavData.size()) +
        " download_ms=" + ToString((int)downloadMs));
    ReleasePlaybackSlot(generation);
    return 0;
  }

  std::string filePath = ResolveTtsCacheFilePath(hash);
  if (filePath.empty()) {
    Log("TTS_PLAYBACK: Could not resolve cache path for hash " + hash);
    ReleasePlaybackSlot(generation);
    return 0;
  }

  if (generation != CurrentTtsPlaybackGeneration()) {
    return 0;
  }
  if (!(playbackSpeedMultiplier > 0.0f)) {
    playbackSpeedMultiplier = 1.0f;
  }
  if (playbackSpeedMultiplier < 1.0f) {
    playbackSpeedMultiplier = 1.0f;
  } else if (playbackSpeedMultiplier > 3.0f) {
    playbackSpeedMultiplier = 3.0f;
  }
  if (playbackSpeedMultiplier > 1.001f) {
    ApplyWavSpeedMultiplierInPlace(wavData, playbackSpeedMultiplier);
  }

  int durationMs = EstimateWavDurationMs(wavData);
  if (durationMs <= 0) {
    durationMs = 5000;
  }
  int volumePercent = (volumePercentOverride >= 0)
                          ? ClampVolumePercent(volumePercentOverride)
                          : ClampVolumePercent(g_ttsVolumePercent);
  if (volumePercent < 100) {
    ApplyWavVolumeInPlace(wavData, volumePercent);
  }

  std::ofstream out(filePath.c_str(), std::ios::binary | std::ios::trunc);
  if (!out) {
    Log("TTS_PLAYBACK: Failed to open cache file " + filePath);
    ReleasePlaybackSlot(generation);
    return 0;
  }
  out.write(wavData.data(), static_cast<std::streamsize>(wavData.size()));
  out.close();

  std::wstring wideFilePath = ToWide(filePath);

  Log("TTS_PLAYBACK: clip prepared hash=" + ShortHashForLog(hash) +
      " bytes=" + ToString((int)wavData.size()) +
      " est_ms=" + ToString(durationMs) +
      " volume_pct=" + ToString(volumePercent) +
      " volume_override=" + std::string(volumePercentOverride >= 0 ? "1" : "0") +
      " speed=" + ToString(playbackSpeedMultiplier) +
      " owner=" + ToString(owner) +
      " speaker_serial=" + ToString((int)speakerSerial) +
      " download_ms=" + ToString((int)downloadMs));

  DWORD playFlags = SND_FILENAME | SND_ASYNC | SND_NODEFAULT;
#ifdef SND_SYSTEM
  playFlags |= SND_SYSTEM;
#endif
  if (!PlaySoundW(wideFilePath.c_str(), NULL, playFlags)) {
    Log("TTS_PLAYBACK: PlaySound failed for hash " + hash);
    ReleasePlaybackSlot(generation);
    return 0;
  }

  Log("TTS_PLAYBACK: playing hash=" + ShortHashForLog(hash) +
      " est_ms=" + ToString(durationMs));

  DWORD startTick = GetTickCount();
  g_ttsPlaybackEndTick = startTick + static_cast<DWORD>(durationMs + 100);
  bool interrupted = false;
  while (true) {
    if (generation != CurrentTtsPlaybackGeneration()) {
      interrupted = true;
      PlaySoundW(NULL, NULL, SND_ASYNC);
      break;
    }
    DWORD elapsed = GetTickCount() - startTick;
    if (elapsed >= static_cast<DWORD>(durationMs + 100)) {
      break;
    }
    Sleep(25);
  }

  DWORD playbackElapsedMs = GetTickCount() - startTick;
  Log("TTS_PLAYBACK: finished hash=" + ShortHashForLog(hash) +
      " elapsed_ms=" + ToString((int)playbackElapsedMs) +
      " interrupted=" + std::string(interrupted ? "1" : "0"));

  ReleasePlaybackSlot(generation);
  return 0;
}
} // namespace

bool QueueTtsPlayback(const std::string &ttsHash, int volumePercentOverride,
                      unsigned int speakerSerial,
                      float playbackSpeedMultiplier, int owner) {
  if (!IsHexHash(ttsHash)) {
    Log("TTS_PLAYBACK: rejected invalid hash");
    return false;
  }

  if (InterlockedCompareExchange(&g_ttsPlaybackBusy, 1, 0) != 0) {
    Log("TTS_PLAYBACK: skip enqueue hash=" + ShortHashForLog(ttsHash) +
        " reason=busy");
    return false;
  }

  g_ttsPlaybackEndTick = 0;

  TtsPlaybackTask *task = new TtsPlaybackTask();
  task->hash = ttsHash;
  task->generation = CurrentTtsPlaybackGeneration();
  task->volumePercentOverride = volumePercentOverride;
  task->speakerSerial = speakerSerial;
  task->playbackSpeedMultiplier = playbackSpeedMultiplier;
  task->owner = owner;
  InterlockedExchange(&g_ttsPlaybackOwner, owner);
  LONG taskGeneration = task->generation;

  HANDLE threadHandle = CreateThread(NULL, 0, PlaybackThreadProc, task, 0, NULL);
  if (threadHandle) {
    CloseHandle(threadHandle);
    Log("TTS_PLAYBACK: enqueued hash=" + ShortHashForLog(ttsHash) +
        " generation=" + ToString((int)taskGeneration) +
        " volume_override=" + std::string(volumePercentOverride >= 0 ? "1" : "0") +
        " speed=" + ToString(playbackSpeedMultiplier) +
        " speaker_serial=" + ToString((int)speakerSerial) +
        " owner=" + ToString(owner));
    return true;
  }

  delete task;
  ReleasePlaybackSlot(taskGeneration);
  Log("TTS_PLAYBACK: failed to create playback thread");
  return false;
}

bool IsTtsPlaybackActive() {
  return InterlockedCompareExchange(&g_ttsPlaybackBusy, 0, 0) != 0;
}

int GetTtsPlaybackRemainingMs() {
  if (!IsTtsPlaybackActive()) {
    return 0;
  }
  DWORD endTick = g_ttsPlaybackEndTick;
  if (endTick == 0) {
    return 0;
  }
  DWORD nowTick = GetTickCount();
  if (nowTick >= endTick) {
    return 0;
  }
  DWORD remaining = endTick - nowTick;
  return static_cast<int>(remaining > 600000 ? 600000 : remaining);
}

void InterruptTtsPlayback() {
  InterlockedIncrement(&g_ttsPlaybackGeneration);
  g_ttsPlaybackEndTick = 0;
  PlaySoundW(NULL, NULL, SND_ASYNC);
  InterlockedExchange(&g_ttsPlaybackOwner, TTS_PLAYBACK_OWNER_DEFAULT);
  InterlockedExchange(&g_ttsPlaybackBusy, 0);
  Log("TTS_PLAYBACK: interrupted current playback");
}

bool InterruptTtsPlaybackIfOwner(int owner) {
  if (InterlockedCompareExchange(&g_ttsPlaybackOwner, 0, 0) != owner) {
    return false;
  }
  InterruptTtsPlayback();
  return true;
}
