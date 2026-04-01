#pragma once
#include <string>
#include <vector>
#include <windows.h>

class GameWorld;

void Log(const std::string &msg);
std::string ToString(int val);
std::string ToString(unsigned int val);
std::string ToString(float val);
std::string ToString(float val);
std::string EscapeJSON(const std::string &s);
std::string UnescapeJSON(const std::string &s);
std::wstring WideFromUtf8(const std::string &str);
std::string JsonReadField(const std::string &json, const std::string &key);
void LoadStobeRuntimeConfig();
void SaveStobeRuntimeConfig();
void SetHotkeyFromString(const std::string &keyStr);
float ResolveDialogueGameSpeedMultiplier(GameWorld *world);
void SleepIfPaused(DWORD ms);
void ResetRuntimeLogsForSession();
std::string SanitizeDialogueForEventStream(const std::string &value);
