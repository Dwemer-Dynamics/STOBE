#pragma once
#include <string>
#include <vector>
#include <windows.h>

typedef bool (*StobeStreamLineCallback)(const std::string &line, void *userData);

enum SpeechDeliveryState {
  SPEECH_DELIVERY_UNKNOWN = 0,
  SPEECH_DELIVERY_PENDING,
  SPEECH_DELIVERY_SPOKEN,
  SPEECH_DELIVERY_CANCELLED
};

void AsyncPostToStobe(const std::wstring &endpoint, const std::string &jsonData);
void AsyncPostToStobeSerial(const std::wstring &endpoint,
                            const std::string &jsonData);
void PostToStobe(const std::wstring &endpoint, const std::string &jsonData);
std::string PostToStobeWithResponse(const std::wstring &endpoint,
                                    const std::string &jsonData);
bool PostToStobeWithResponseStream(const std::wstring &endpoint,
                                   const std::string &jsonData,
                                   StobeStreamLineCallback callback,
                                   void *userData);
std::string UploadCsvImportToStobe(const std::string &csvData,
                                   const std::string &filename,
                                   const std::string &importType);
std::string UploadWavToStobe(const std::vector<unsigned char> &wavData);
std::wstring ToWide(const std::string &value);
std::string UrlEncode(const std::string &input);
std::string BuildStreamQueryData(const std::string &eventType,
                                 const std::string &eventData,
                                 int gameTs = 0);
void PostSpeechDeliveryState(const std::string &utteranceId,
                             const std::string &deliveryState);
void PostSpeechDeliveryStates(const std::vector<std::string> &utteranceIds,
                              const std::string &deliveryState);
void TrackSpeechDeliveryState(const std::string &utteranceId);
SpeechDeliveryState GetSpeechDeliveryState(const std::string &utteranceId);
void ForgetSpeechDeliveryStates(const std::vector<std::string> &utteranceIds);
bool IsDwemerDistroConnected();
DWORD GetDwemerDistroLastSuccessAgeMs();
std::string GetStobeServerHomeUrl();

DWORD WINAPI BoredEventPollThread(LPVOID lpParam);
