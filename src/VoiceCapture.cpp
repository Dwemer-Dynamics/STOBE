#include "VoiceCapture.h"

#include "Comm.h"
#include "Globals.h"
#include "Utils.h"

#include <mmsystem.h>
#include <map>
#include <vector>

namespace Stobe {
namespace Voice {
namespace {

const int kSampleRate = 16000;
const int kBufferCount = 8;
const int kBufferBytes = 3200;
const DWORD kMaximumRecordingMs = 60000;

struct Result {
  Context context;
  std::string text;
  std::string error;
};

struct UploadTask {
  Context context;
  std::vector<unsigned char> wav;
};

HWAVEIN g_waveIn = NULL;
WAVEHDR g_headers[kBufferCount] = {};
char g_buffers[kBufferCount][kBufferBytes] = {};
std::vector<unsigned char> g_pcm;
Context g_context;
DWORD g_startedAt = 0;
LONG g_recording = 0;
LONG g_initialized = 0;
LONG g_nextResultId = 0;
CRITICAL_SECTION g_mutex;
std::map<int, Result> g_results;

void EnsureInitialized() {
  if (InterlockedCompareExchange(&g_initialized, 1, 0) == 0)
    InitializeCriticalSection(&g_mutex);
}

void WriteLe16(std::vector<unsigned char> &out, unsigned short value) {
  out.push_back((unsigned char)(value & 0xff));
  out.push_back((unsigned char)((value >> 8) & 0xff));
}

void WriteLe32(std::vector<unsigned char> &out, unsigned int value) {
  for (int i = 0; i < 4; ++i)
    out.push_back((unsigned char)((value >> (i * 8)) & 0xff));
}

std::vector<unsigned char> BuildWav(const std::vector<unsigned char> &pcm) {
  std::vector<unsigned char> wav;
  wav.reserve(44 + pcm.size());
  const unsigned char riff[] = {'R','I','F','F'};
  const unsigned char waveFmt[] = {'W','A','V','E','f','m','t',' '};
  const unsigned char data[] = {'d','a','t','a'};
  wav.insert(wav.end(), riff, riff + sizeof(riff)); WriteLe32(wav, (unsigned int)(36 + pcm.size()));
  wav.insert(wav.end(), waveFmt, waveFmt + sizeof(waveFmt)); WriteLe32(wav, 16);
  WriteLe16(wav, 1); WriteLe16(wav, 1); WriteLe32(wav, kSampleRate);
  WriteLe32(wav, kSampleRate * 2); WriteLe16(wav, 2); WriteLe16(wav, 16);
  wav.insert(wav.end(), data, data + sizeof(data)); WriteLe32(wav, (unsigned int)pcm.size());
  wav.insert(wav.end(), pcm.begin(), pcm.end());
  return wav;
}

void QueueResult(const Context &context, const std::string &text,
                 const std::string &error) {
  EnsureInitialized();
  int id = (int)InterlockedIncrement(&g_nextResultId);
  EnterCriticalSection(&g_mutex);
  Result result;
  result.context = context;
  result.text = text;
  result.error = error;
  g_results[id] = result;
  LeaveCriticalSection(&g_mutex);
  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.push_back("CMD: STT_TRANSCRIPT:" + ToString(id));
  LeaveCriticalSection(&g_msgMutex);
}

DWORD WINAPI UploadThread(LPVOID parameter) {
  UploadTask *task = static_cast<UploadTask *>(parameter);
  if (!task) return 0;
  std::string response = UploadWavToStobe(task->wav);
  std::string text = JsonReadField(response, "text");
  std::string error = JsonReadField(response, "error");
  if (text.empty() && error.empty()) error = "Speech transcription failed.";
  QueueResult(task->context, text, error);
  delete task;
  return 0;
}

void CALLBACK WaveInCallback(HWAVEIN input, UINT message, DWORD_PTR,
                             DWORD_PTR param1, DWORD_PTR) {
  if (message != WIM_DATA || !param1) return;
  WAVEHDR *header = reinterpret_cast<WAVEHDR *>(param1);
  if (header->dwBytesRecorded > 0) {
    EnsureInitialized();
    EnterCriticalSection(&g_mutex);
    const unsigned char *begin = reinterpret_cast<unsigned char *>(header->lpData);
    g_pcm.insert(g_pcm.end(), begin, begin + header->dwBytesRecorded);
    LeaveCriticalSection(&g_mutex);
  }
  if (InterlockedCompareExchange(&g_recording, 0, 0) != 0) {
    header->dwBytesRecorded = 0;
    waveInAddBuffer(input, header, sizeof(WAVEHDR));
  }
}

} // namespace

bool Start(const Context &context) {
  EnsureInitialized();
  if (InterlockedCompareExchange(&g_recording, 1, 0) != 0) return false;
  WAVEFORMATEX format = {};
  format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 1;
  format.nSamplesPerSec = kSampleRate; format.wBitsPerSample = 16;
  format.nBlockAlign = 2; format.nAvgBytesPerSec = kSampleRate * 2;
  if (waveInOpen(&g_waveIn, WAVE_MAPPER, &format,
                 reinterpret_cast<DWORD_PTR>(WaveInCallback), 0,
                 CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
    InterlockedExchange(&g_recording, 0);
    Log("STT_CAPTURE: waveInOpen failed");
    return false;
  }
  EnterCriticalSection(&g_mutex);
  g_pcm.clear(); g_context = context;
  LeaveCriticalSection(&g_mutex);
  for (int i = 0; i < kBufferCount; ++i) {
    ZeroMemory(&g_headers[i], sizeof(WAVEHDR));
    g_headers[i].lpData = g_buffers[i]; g_headers[i].dwBufferLength = kBufferBytes;
    waveInPrepareHeader(g_waveIn, &g_headers[i], sizeof(WAVEHDR));
    waveInAddBuffer(g_waveIn, &g_headers[i], sizeof(WAVEHDR));
  }
  g_startedAt = GetTickCount();
  if (waveInStart(g_waveIn) != MMSYSERR_NOERROR) {
    Cancel(); return false;
  }
  Log("STT_CAPTURE: recording started mode=" + context.mode +
      " speaker=" + context.speakerName + " target=" + context.targetName);
  return true;
}

void StopAndTranscribe() {
  if (InterlockedExchange(&g_recording, 0) == 0 || !g_waveIn) return;
  waveInStop(g_waveIn); waveInReset(g_waveIn);
  for (int i = 0; i < kBufferCount; ++i)
    waveInUnprepareHeader(g_waveIn, &g_headers[i], sizeof(WAVEHDR));
  waveInClose(g_waveIn); g_waveIn = NULL;
  std::vector<unsigned char> pcm; Context context;
  EnterCriticalSection(&g_mutex); pcm.swap(g_pcm); context = g_context; LeaveCriticalSection(&g_mutex);
  DWORD duration = GetTickCount() - g_startedAt;
  Log("STT_CAPTURE: recording stopped duration_ms=" + ToString((int)duration) +
      " pcm_bytes=" + ToString((int)pcm.size()));
  if (duration < 500 || pcm.size() < 16000) {
    QueueResult(context, "", "Hold push-to-talk longer before releasing it.");
    return;
  }
  UploadTask *task = new UploadTask(); task->context = context; task->wav = BuildWav(pcm);
  HANDLE thread = CreateThread(NULL, 0, UploadThread, task, 0, NULL);
  if (thread) CloseHandle(thread); else { delete task; QueueResult(context, "", "Could not start speech transcription."); }
}

void Cancel() {
  if (InterlockedExchange(&g_recording, 0) == 0 || !g_waveIn) return;
  waveInStop(g_waveIn); waveInReset(g_waveIn);
  for (int i = 0; i < kBufferCount; ++i)
    waveInUnprepareHeader(g_waveIn, &g_headers[i], sizeof(WAVEHDR));
  waveInClose(g_waveIn); g_waveIn = NULL;
  EnterCriticalSection(&g_mutex); g_pcm.clear(); LeaveCriticalSection(&g_mutex);
  Log("STT_CAPTURE: recording cancelled");
}

void Update() {
  if (IsRecording() && GetTickCount() - g_startedAt >= kMaximumRecordingMs)
    StopAndTranscribe();
}

bool IsRecording() { return InterlockedCompareExchange(&g_recording, 0, 0) != 0; }

bool ConsumeResult(int resultId, Context &contextOut, std::string &textOut,
                   std::string &errorOut) {
  EnsureInitialized(); EnterCriticalSection(&g_mutex);
  std::map<int, Result>::iterator found = g_results.find(resultId);
  if (found == g_results.end()) { LeaveCriticalSection(&g_mutex); return false; }
  contextOut = found->second.context; textOut = found->second.text;
  errorOut = found->second.error; g_results.erase(found);
  LeaveCriticalSection(&g_mutex); return true;
}

} // namespace Voice
} // namespace Stobe
