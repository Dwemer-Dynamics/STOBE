#include "VoiceCapture.h"

#include "Comm.h"
#include "Globals.h"
#include "Utils.h"

#include <audioclient.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

namespace Stobe {
namespace Voice {
namespace {

const int kSampleRate = 16000;
const int kBufferCount = 8;
const int kBufferBytes = 4000;
const DWORD kMaximumRecordingMs = 60000;
const size_t kMaximumCaptureEndpoints = 12;
const DWORD kAutoConvertPcmFlag = 0x80000000;
const DWORD kSrcDefaultQualityFlag = 0x08000000;

struct Result {
  Context context;
  std::string text;
  std::string error;
};

struct CaptureSession {
  IAudioClient *audioClient;
  IAudioCaptureClient *captureClient;
  std::string name;
  std::vector<short> audio;
  bool started;

  CaptureSession()
      : audioClient(NULL), captureClient(NULL), started(false) {}
};

struct SignalStats {
  int peak;
  double rms;
  double signalPercent;
  bool valid;

  SignalStats() : peak(0), rms(0.0), signalPercent(0.0), valid(false) {}
};

DWORD g_startedAt = 0;
LONG g_recording = 0;
LONG g_cancelRequested = 0;
LONG g_captureWorkerActive = 0;
LONG g_captureWorkerStarted = 0;
LONG g_initialized = 0;
LONG g_nextResultId = 0;
HANDLE g_captureStartEvent = NULL;
HANDLE g_captureReadyEvent = NULL;
CRITICAL_SECTION g_mutex;
std::map<int, Result> g_results;
Context g_pendingContext;

void EnsureInitialized() {
  if (InterlockedCompareExchange(&g_initialized, 1, 0) == 0)
    InitializeCriticalSection(&g_mutex);
}

WAVEFORMATEX CreateRecordingFormat() {
  WAVEFORMATEX format = {};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nChannels = 1;
  format.nSamplesPerSec = kSampleRate;
  format.wBitsPerSample = 16;
  format.nBlockAlign = 2;
  format.nAvgBytesPerSec = kSampleRate * format.nBlockAlign;
  return format;
}

std::string WideToUtf8(const wchar_t *value) {
  if (!value || !value[0]) return "";
  int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL);
  if (required <= 1) return "";
  std::vector<char> converted((size_t)required);
  WideCharToMultiByte(CP_UTF8, 0, value, -1, &converted[0], required, NULL, NULL);
  return std::string(&converted[0]);
}

std::string GetEndpointFriendlyName(IMMDevice *endpoint) {
  if (!endpoint) return "Unknown input";
  IPropertyStore *store = NULL;
  PROPVARIANT value;
  PropVariantInit(&value);
  std::string name = "Unknown input";
  if (SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ, &store)) && store &&
      SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
      value.vt == VT_LPWSTR) {
    std::string converted = WideToUtf8(value.pwszVal);
    if (!converted.empty()) name = converted;
  }
  PropVariantClear(&value);
  if (store) store->Release();
  return name;
}

void WriteLe16(std::vector<unsigned char> &out, unsigned short value) {
  out.push_back((unsigned char)(value & 0xff));
  out.push_back((unsigned char)((value >> 8) & 0xff));
}

void WriteLe32(std::vector<unsigned char> &out, unsigned int value) {
  for (int i = 0; i < 4; ++i)
    out.push_back((unsigned char)((value >> (i * 8)) & 0xff));
}

std::vector<unsigned char> BuildWav(const std::vector<short> &samples) {
  const unsigned int pcmBytes = (unsigned int)(samples.size() * sizeof(short));
  std::vector<unsigned char> wav;
  wav.reserve(44 + pcmBytes);
  const unsigned char riff[] = {'R','I','F','F'};
  const unsigned char waveFmt[] = {'W','A','V','E','f','m','t',' '};
  const unsigned char data[] = {'d','a','t','a'};
  wav.insert(wav.end(), riff, riff + sizeof(riff));
  WriteLe32(wav, 36 + pcmBytes);
  wav.insert(wav.end(), waveFmt, waveFmt + sizeof(waveFmt));
  WriteLe32(wav, 16);
  WriteLe16(wav, 1);
  WriteLe16(wav, 1);
  WriteLe32(wav, kSampleRate);
  WriteLe32(wav, kSampleRate * 2);
  WriteLe16(wav, 2);
  WriteLe16(wav, 16);
  wav.insert(wav.end(), data, data + sizeof(data));
  WriteLe32(wav, pcmBytes);
  const unsigned char *pcm = reinterpret_cast<const unsigned char *>(&samples[0]);
  wav.insert(wav.end(), pcm, pcm + pcmBytes);
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

void CloseCaptureSession(CaptureSession *session) {
  if (!session) return;
  if (session->started && session->audioClient) session->audioClient->Stop();
  if (session->captureClient) session->captureClient->Release();
  if (session->audioClient) session->audioClient->Release();
  session->captureClient = NULL;
  session->audioClient = NULL;
  session->started = false;
}

void CloseCaptureSessions(std::vector<CaptureSession *> &sessions) {
  for (size_t i = 0; i < sessions.size(); ++i) {
    CloseCaptureSession(sessions[i]);
    delete sessions[i];
  }
  sessions.clear();
}

bool OpenWasapiCaptureSessions(std::vector<CaptureSession *> &sessions) {
  IMMDeviceEnumerator *enumerator = NULL;
  IMMDeviceCollection *collection = NULL;
  HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator));
  if (FAILED(result) || !enumerator) return false;

  result = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(result) || !collection) {
    enumerator->Release();
    return false;
  }

  UINT count = 0;
  collection->GetCount(&count);
  WAVEFORMATEX format = CreateRecordingFormat();
  // These Windows 7 flags are missing from Kenshi's legacy SDK headers.
  const DWORD flags = kAutoConvertPcmFlag | kSrcDefaultQualityFlag;
  const REFERENCE_TIME bufferDuration = 10000000;
  for (UINT i = 0; i < count && sessions.size() < kMaximumCaptureEndpoints; ++i) {
    IMMDevice *endpoint = NULL;
    if (FAILED(collection->Item(i, &endpoint)) || !endpoint) continue;

    CaptureSession *session = new CaptureSession();
    session->name = GetEndpointFriendlyName(endpoint);
    result = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL,
                                reinterpret_cast<void **>(&session->audioClient));
    if (SUCCEEDED(result)) {
      result = session->audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                                bufferDuration, 0, &format, NULL);
    }
    if (SUCCEEDED(result)) {
      result = session->audioClient->GetService(
          __uuidof(IAudioCaptureClient),
          reinterpret_cast<void **>(&session->captureClient));
    }
    endpoint->Release();

    if (FAILED(result)) {
      Log("STT_CAPTURE: skipped input=" + session->name +
          " error=" + ToString((int)result));
      CloseCaptureSession(session);
      delete session;
      continue;
    }
    sessions.push_back(session);
  }

  collection->Release();
  enumerator->Release();
  Log("STT_CAPTURE: WASAPI discovery opened inputs=" +
      ToString((int)sessions.size()));
  return !sessions.empty();
}

void ReadWasapiPackets(CaptureSession *session) {
  if (!session || !session->captureClient) return;
  UINT32 packetFrames = 0;
  HRESULT result = session->captureClient->GetNextPacketSize(&packetFrames);
  while (SUCCEEDED(result) && packetFrames > 0) {
    BYTE *packetData = NULL;
    UINT32 frames = 0;
    DWORD flags = 0;
    result = session->captureClient->GetBuffer(&packetData, &frames, &flags,
                                                NULL, NULL);
    if (FAILED(result)) break;
    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !packetData) {
      session->audio.insert(session->audio.end(), frames, 0);
    } else {
      const short *samples = reinterpret_cast<const short *>(packetData);
      session->audio.insert(session->audio.end(), samples, samples + frames);
    }
    session->captureClient->ReleaseBuffer(frames);
    result = session->captureClient->GetNextPacketSize(&packetFrames);
  }
}

bool CaptureWithWasapi(std::vector<CaptureSession *> &sessions) {
  if (sessions.empty() && !OpenWasapiCaptureSessions(sessions)) return false;

  bool anyStarted = false;
  for (size_t i = 0; i < sessions.size(); ++i) {
    sessions[i]->audio.clear();
    HRESULT result = sessions[i]->audioClient->Start();
    sessions[i]->started = SUCCEEDED(result);
    if (sessions[i]->started) {
      anyStarted = true;
    } else {
      Log("STT_CAPTURE: could not start input=" + sessions[i]->name +
          " error=" + ToString((int)result));
    }
  }
  if (!anyStarted) {
    CloseCaptureSessions(sessions);
    return false;
  }

  while (InterlockedCompareExchange(&g_recording, 0, 0) != 0 &&
         GetTickCount() - g_startedAt < kMaximumRecordingMs) {
    for (size_t i = 0; i < sessions.size(); ++i)
      if (sessions[i]->started) ReadWasapiPackets(sessions[i]);
    Sleep(5);
  }
  for (size_t i = 0; i < sessions.size(); ++i) {
    if (!sessions[i]->started) continue;
    ReadWasapiPackets(sessions[i]);
    sessions[i]->audioClient->Stop();
    sessions[i]->started = false;
  }
  return true;
}

std::string GetWaveInputName(HWAVEIN input) {
  UINT deviceId = 0;
  WAVEINCAPSW caps = {};
  if (input && waveInGetID(input, &deviceId) == MMSYSERR_NOERROR &&
      waveInGetDevCapsW(deviceId, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
    std::string name = WideToUtf8(caps.szPname);
    if (!name.empty()) return name;
  }
  return "Windows default";
}

bool CaptureWithWinMM(CaptureSession &session) {
  WAVEFORMATEX format = CreateRecordingFormat();
  HWAVEIN input = NULL;
  MMRESULT result = waveInOpen(&input, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
  if (result != MMSYSERR_NOERROR) {
    result = waveInOpen(&input, WAVE_MAPPER, &format, 0, 0,
                        CALLBACK_NULL | WAVE_FORMAT_DIRECT);
  }
  if (result != MMSYSERR_NOERROR || !input) {
    Log("STT_CAPTURE: WinMM fallback failed error=" + ToString((int)result));
    return false;
  }

  session.name = GetWaveInputName(input);
  char buffers[kBufferCount][kBufferBytes] = {};
  WAVEHDR headers[kBufferCount] = {};
  for (int i = 0; i < kBufferCount; ++i) {
    headers[i].lpData = buffers[i];
    headers[i].dwBufferLength = kBufferBytes;
    if (waveInPrepareHeader(input, &headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
        waveInAddBuffer(input, &headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
      waveInReset(input);
      for (int j = 0; j <= i; ++j)
        if (headers[j].dwFlags & WHDR_PREPARED)
          waveInUnprepareHeader(input, &headers[j], sizeof(WAVEHDR));
      waveInClose(input);
      return false;
    }
  }

  if (waveInStart(input) != MMSYSERR_NOERROR) {
    waveInReset(input);
    for (int i = 0; i < kBufferCount; ++i)
      waveInUnprepareHeader(input, &headers[i], sizeof(WAVEHDR));
    waveInClose(input);
    return false;
  }

  Log("STT_CAPTURE: using WinMM fallback input=" + session.name);
  while (InterlockedCompareExchange(&g_recording, 0, 0) != 0 &&
         GetTickCount() - g_startedAt < kMaximumRecordingMs) {
    for (int i = 0; i < kBufferCount; ++i) {
      if (!(headers[i].dwFlags & WHDR_DONE)) continue;
      const short *samples = reinterpret_cast<const short *>(headers[i].lpData);
      size_t count = headers[i].dwBytesRecorded / sizeof(short);
      session.audio.insert(session.audio.end(), samples, samples + count);
      headers[i].dwBytesRecorded = 0;
      waveInAddBuffer(input, &headers[i], sizeof(WAVEHDR));
    }
    Sleep(5);
  }

  waveInStop(input);
  waveInReset(input);
  for (int i = 0; i < kBufferCount; ++i) {
    if (headers[i].dwBytesRecorded > 0) {
      const short *samples = reinterpret_cast<const short *>(headers[i].lpData);
      size_t count = headers[i].dwBytesRecorded / sizeof(short);
      session.audio.insert(session.audio.end(), samples, samples + count);
    }
    if (headers[i].dwFlags & WHDR_PREPARED)
      waveInUnprepareHeader(input, &headers[i], sizeof(WAVEHDR));
  }
  waveInClose(input);
  return true;
}

SignalStats AnalyzeSignal(const std::vector<short> &audio) {
  SignalStats stats;
  if (audio.empty()) return stats;
  const int signalFloor = 64;
  double sumSquares = 0.0;
  size_t signalSamples = 0;
  for (size_t i = 0; i < audio.size(); ++i) {
    int amplitude = std::abs((int)audio[i]);
    if (amplitude > stats.peak) stats.peak = amplitude;
    sumSquares += (double)audio[i] * (double)audio[i];
    if (amplitude >= signalFloor) ++signalSamples;
  }
  stats.rms = std::sqrt(sumSquares / (double)audio.size());
  stats.signalPercent = 100.0 * (double)signalSamples / (double)audio.size();
  stats.valid = audio.size() >= 8000 && stats.peak >= signalFloor && stats.rms >= 2.0;
  return stats;
}

std::string SignalLog(const CaptureSession &session, const SignalStats &stats) {
  std::ostringstream line;
  line << "STT_CAPTURE: input=" << session.name
       << " samples=" << session.audio.size()
       << " peak=" << stats.peak
       << " rms=" << std::fixed << std::setprecision(1) << stats.rms
       << " signal=" << std::setprecision(2) << stats.signalPercent << "%"
       << " valid=" << (stats.valid ? 1 : 0);
  return line.str();
}

DWORD WINAPI CaptureWorkerThread(LPVOID) {
  HRESULT initResult = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  bool shouldUninitialize = SUCCEEDED(initResult);
  std::vector<CaptureSession *> sessions;
  if (SUCCEEDED(initResult) || initResult == RPC_E_CHANGED_MODE)
    OpenWasapiCaptureSessions(sessions);
  SetEvent(g_captureReadyEvent);

  for (;;) {
    if (WaitForSingleObject(g_captureStartEvent, INFINITE) != WAIT_OBJECT_0)
      break;

    Context context;
    EnterCriticalSection(&g_mutex);
    context = g_pendingContext;
    LeaveCriticalSection(&g_mutex);

    bool usedWasapi = false;
    if (SUCCEEDED(initResult) || initResult == RPC_E_CHANGED_MODE)
      usedWasapi = CaptureWithWasapi(sessions);

    CaptureSession winmmSession;
    if (!usedWasapi) CaptureWithWinMM(winmmSession);
    InterlockedExchange(&g_recording, 0);

    if (InterlockedCompareExchange(&g_cancelRequested, 0, 0) == 0) {
      CaptureSession *winner = NULL;
      double winnerScore = -1.0;
      if (usedWasapi) {
        for (size_t i = 0; i < sessions.size(); ++i) {
          SignalStats stats = AnalyzeSignal(sessions[i]->audio);
          Log(SignalLog(*sessions[i], stats));
          double score = stats.rms + ((double)stats.peak * 0.02);
          if (stats.valid && score > winnerScore) {
            winner = sessions[i];
            winnerScore = score;
          }
        }
      } else {
        SignalStats stats = AnalyzeSignal(winmmSession.audio);
        Log(SignalLog(winmmSession, stats));
        if (stats.valid) winner = &winmmSession;
      }

      DWORD duration = GetTickCount() - g_startedAt;
      if (duration < 500) {
        QueueResult(context, "", "Hold push-to-talk longer before releasing it.");
      } else if (!winner) {
        QueueResult(context, "",
                    "No microphone signal was detected. Check your Windows input device.");
      } else {
        Log("STT_CAPTURE: selected input=" + winner->name +
            " duration_ms=" + ToString((int)duration));
        std::vector<unsigned char> wav = BuildWav(winner->audio);
        std::string response = UploadWavToStobe(wav);
        std::string text = JsonReadField(response, "text");
        std::string error = JsonReadField(response, "error");
        if (text.empty() && error.empty()) error = "Speech transcription failed.";
        QueueResult(context, text, error);
      }
    }

    InterlockedExchange(&g_captureWorkerActive, 0);
  }

  CloseCaptureSessions(sessions);
  if (shouldUninitialize) CoUninitialize();
  return 0;
}

bool EnsureCaptureWorker(bool waitUntilReady) {
  if (InterlockedCompareExchange(&g_captureWorkerStarted, 1, 0) == 0) {
    g_captureStartEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_captureReadyEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_captureStartEvent || !g_captureReadyEvent) {
      Log("STT_CAPTURE: could not create capture worker events");
      if (g_captureStartEvent) CloseHandle(g_captureStartEvent);
      if (g_captureReadyEvent) CloseHandle(g_captureReadyEvent);
      g_captureStartEvent = NULL;
      g_captureReadyEvent = NULL;
      InterlockedExchange(&g_captureWorkerStarted, 0);
      return false;
    }
    HANDLE thread = CreateThread(NULL, 0, CaptureWorkerThread, NULL, 0, NULL);
    if (!thread) {
      Log("STT_CAPTURE: could not start capture worker");
      CloseHandle(g_captureStartEvent);
      CloseHandle(g_captureReadyEvent);
      g_captureStartEvent = NULL;
      g_captureReadyEvent = NULL;
      InterlockedExchange(&g_captureWorkerStarted, 0);
      return false;
    }
    CloseHandle(thread);
  }

  if (!g_captureReadyEvent) return false;
  if (!waitUntilReady) return true;
  return WaitForSingleObject(g_captureReadyEvent, 2000) == WAIT_OBJECT_0;
}

} // namespace

bool Start(const Context &context) {
  EnsureInitialized();
  if (!EnsureCaptureWorker(true)) {
    Log("STT_CAPTURE: microphone warmup did not complete");
    return false;
  }
  if (InterlockedCompareExchange(&g_captureWorkerActive, 1, 0) != 0)
    return false;
  InterlockedExchange(&g_cancelRequested, 0);
  InterlockedExchange(&g_recording, 1);
  g_startedAt = GetTickCount();
  EnterCriticalSection(&g_mutex);
  g_pendingContext = context;
  LeaveCriticalSection(&g_mutex);
  if (!SetEvent(g_captureStartEvent)) {
    InterlockedExchange(&g_recording, 0);
    InterlockedExchange(&g_captureWorkerActive, 0);
    return false;
  }
  Log("STT_CAPTURE: recording started mode=" + context.mode +
      " speaker=" + context.speakerName + " target=" + context.targetName);
  return true;
}

void StopAndTranscribe() {
  if (InterlockedExchange(&g_recording, 0) == 0) return;
  Log("STT_CAPTURE: stop requested; selecting active microphone input");
}

void Cancel() {
  if (InterlockedCompareExchange(&g_captureWorkerActive, 0, 0) == 0) return;
  InterlockedExchange(&g_cancelRequested, 1);
  InterlockedExchange(&g_recording, 0);
  Log("STT_CAPTURE: recording cancelled");
}

void Update() {
  EnsureInitialized();
  EnsureCaptureWorker(false);
  if (IsRecording() && GetTickCount() - g_startedAt >= kMaximumRecordingMs)
    StopAndTranscribe();
}

bool IsRecording() {
  return InterlockedCompareExchange(&g_recording, 0, 0) != 0;
}

bool ConsumeResult(int resultId, Context &contextOut, std::string &textOut,
                   std::string &errorOut) {
  EnsureInitialized();
  EnterCriticalSection(&g_mutex);
  std::map<int, Result>::iterator found = g_results.find(resultId);
  if (found == g_results.end()) {
    LeaveCriticalSection(&g_mutex);
    return false;
  }
  contextOut = found->second.context;
  textOut = found->second.text;
  errorOut = found->second.error;
  g_results.erase(found);
  LeaveCriticalSection(&g_mutex);
  return true;
}

} // namespace Voice
} // namespace Stobe
