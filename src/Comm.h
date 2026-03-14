#pragma once
#include <string>
#include <windows.h>

typedef bool (*StobeStreamLineCallback)(const std::string &line, void *userData);

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
std::wstring ToWide(const std::string &value);
std::string UrlEncode(const std::string &input);
std::string BuildStreamQueryData(const std::string &eventType,
                                 const std::string &eventData,
                                 int gameTs = 0);
bool IsDwemerDistroConnected();
DWORD GetDwemerDistroLastSuccessAgeMs();
std::string GetStobeServerHomeUrl();

DWORD WINAPI BoredEventPollThread(LPVOID lpParam);
