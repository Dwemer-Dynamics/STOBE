#include "Globals.h"
#include "AudioPlayback.h"
#include <algorithm>

// Global Definitions
GameWorld **ppWorld = nullptr;
CRITICAL_SECTION g_LogMutex;
std::deque<std::string> g_messageQueue;
CRITICAL_SECTION g_msgMutex;
hand g_talkTargetHand;
DWORD g_mainThreadId = 0;
DWORD g_lastBoredEventTick = 0;
DWORD g_lastDialogueTick = 0;
DWORD g_nextSpeechActionTick = 0;
DWORD g_lastRechatDispatchTick = 0;
std::map<unsigned int, std::string> g_originFactions;
LONG g_chatInterruptGeneration = 1;

float g_boredEventRange = 200.0f;
float g_proximityRadius = 80.0f;
float g_shoutRadius = 200.0f;
float g_visionRange = 100.0f;
int g_boredEventIntervalSeconds = 240;
bool g_enableBoredEvents = true;
bool g_triggerBoredEvent = false;
float g_minFactionRelation = -100.0f;
float g_maxFactionRelation = 100.0f;
int g_dialogueSpeedSeconds = 5;
float g_speechBubbleLife = 5.0f;
int g_rechatDispatchCooldownMs = 350;
int g_ttsVolumePercent = 100;
bool g_ttsEnabled = true;
bool g_speedDialogue = true;
int g_dynamicProfileIntervalMinutes = 20;

std::string g_activeInventoryJson = "[]";
hand g_lastInventoryHand;
std::string g_activeCharName = "";
hand g_lastSelectionHand;
std::string g_playerInventoryJson = "[]";
hand g_playerHand;
CRITICAL_SECTION g_stateMutex;

std::deque<GameEvent> g_gameEvents;
CRITICAL_SECTION g_eventMutex;

std::deque<QueuedAction> g_uiActionQueue;
CRITICAL_SECTION g_uiMutex;
std::map<unsigned int, hand> g_followTargets;
std::map<unsigned int, TravelTarget> g_travelTargets;

int g_chatHotkey = VK_OEM_2; // '/' by default
std::string g_chatHotkeyStr = "/";
std::string g_chatMode = "chat";
bool g_autoChatEnabled = false;
bool g_useNearestPlayerSpeaker = true;
bool g_enableAnimalTalks = false;
std::map<std::string, std::string> g_uiTranslation;

std::string T(const std::string &key) {
  auto it = g_uiTranslation.find(key);
  if (it != g_uiTranslation.end())
    return it->second;
  return key;
}

// Background Name Assignment system
std::deque<NameCheckItem> g_nameCheckQueue;
CRITICAL_SECTION g_nameCheckMutex;
std::set<unsigned int> g_renamedSerials;
std::set<unsigned int> g_activatedAnimalSerials;
DWORD g_lastContextPushTick = 0;
DWORD g_lastWorldStatePushTick = 0;

GameWorld *GetWorldSafe() {
  if (!ppWorld) {
    return nullptr;
  }

  if (reinterpret_cast<uintptr_t>(ppWorld) < 0x10000) {
    return nullptr;
  }

  __try {
    return *ppWorld;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

LONG GetChatInterruptGeneration() {
  return InterlockedCompareExchange(&g_chatInterruptGeneration, 0, 0);
}

bool IsChatInterruptGenerationCurrent(LONG generation) {
  return generation == GetChatInterruptGeneration();
}

void MarkAnimalActivated(unsigned int serial) {
  if (serial == 0) {
    return;
  }
  EnterCriticalSection(&g_stateMutex);
  g_activatedAnimalSerials.insert(serial);
  LeaveCriticalSection(&g_stateMutex);
}

bool IsAnimalActivated(unsigned int serial) {
  if (serial == 0) {
    return false;
  }
  EnterCriticalSection(&g_stateMutex);
  bool active = g_activatedAnimalSerials.count(serial) > 0;
  LeaveCriticalSection(&g_stateMutex);
  return active;
}

void SetFollowTarget(unsigned int followerSerial, const hand &target) {
  if (followerSerial == 0 || !target.isValid()) {
    return;
  }
  EnterCriticalSection(&g_stateMutex);
  g_followTargets[followerSerial] = target;
  LeaveCriticalSection(&g_stateMutex);
}

void ClearFollowTarget(unsigned int followerSerial) {
  if (followerSerial == 0) {
    return;
  }
  EnterCriticalSection(&g_stateMutex);
  g_followTargets.erase(followerSerial);
  LeaveCriticalSection(&g_stateMutex);
}

void ClearAllFollowTargets() {
  EnterCriticalSection(&g_stateMutex);
  g_followTargets.clear();
  LeaveCriticalSection(&g_stateMutex);
}

std::map<unsigned int, hand> SnapshotFollowTargets() {
  EnterCriticalSection(&g_stateMutex);
  std::map<unsigned int, hand> copy = g_followTargets;
  LeaveCriticalSection(&g_stateMutex);
  return copy;
}

void SetTravelTarget(unsigned int actorSerial, float x, float y, float z,
                     const std::string &label) {
  if (actorSerial == 0) {
    return;
  }
  EnterCriticalSection(&g_stateMutex);
  TravelTarget target;
  target.x = x;
  target.y = y;
  target.z = z;
  target.label = label;
  g_travelTargets[actorSerial] = target;
  LeaveCriticalSection(&g_stateMutex);
}

void ClearTravelTarget(unsigned int actorSerial) {
  if (actorSerial == 0) {
    return;
  }
  EnterCriticalSection(&g_stateMutex);
  g_travelTargets.erase(actorSerial);
  LeaveCriticalSection(&g_stateMutex);
}

void ClearAllTravelTargets() {
  EnterCriticalSection(&g_stateMutex);
  g_travelTargets.clear();
  LeaveCriticalSection(&g_stateMutex);
}

std::map<unsigned int, TravelTarget> SnapshotTravelTargets() {
  EnterCriticalSection(&g_stateMutex);
  std::map<unsigned int, TravelTarget> copy = g_travelTargets;
  LeaveCriticalSection(&g_stateMutex);
  return copy;
}

LONG BeginChatInterruptGeneration() {
  LONG generation = InterlockedIncrement(&g_chatInterruptGeneration);

  InterruptTtsPlayback();

  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.erase(
      std::remove_if(g_messageQueue.begin(), g_messageQueue.end(),
                     [](const std::string &msg) {
                       return msg.find("NPC_SAY: ") == 0 ||
                              msg.find("NPC_ACTION: ") == 0 ||
                              msg.find("PLAYER_TTS: ") == 0;
                     }),
      g_messageQueue.end());
  LeaveCriticalSection(&g_msgMutex);

  EnterCriticalSection(&g_uiMutex);
  g_uiActionQueue.erase(
      std::remove_if(g_uiActionQueue.begin(), g_uiActionQueue.end(),
                     [](const QueuedAction &act) {
                       return act.type == ACT_SAY || act.type == ACT_PLAY_TTS;
                     }),
      g_uiActionQueue.end());
  LeaveCriticalSection(&g_uiMutex);

  EnterCriticalSection(&g_stateMutex);
  g_nextSpeechActionTick = 0;
  g_lastRechatDispatchTick = 0;
  g_followTargets.clear();
  g_travelTargets.clear();
  LeaveCriticalSection(&g_stateMutex);

  return generation;
}
