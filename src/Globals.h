#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <windows.h>

// Forward declarations for Kenshi types
class GameWorld;
namespace Ogre {
class Vector3;
}
#include <kenshi/util/hand.h>

// Global communication state
extern GameWorld **ppWorld;
extern CRITICAL_SECTION g_LogMutex;
extern std::deque<std::string> g_messageQueue;
extern CRITICAL_SECTION g_msgMutex;
extern hand g_talkTargetHand;
extern DWORD g_mainThreadId;
extern DWORD g_lastBoredEventTick;
extern int g_lastBoredEventGameTs;
extern DWORD g_lastDialogueTick;
extern DWORD g_nextSpeechActionTick;
extern DWORD g_lastRechatDispatchTick;
extern std::map<unsigned int, std::string> g_originFactions;
extern LONG g_chatInterruptGeneration;

// Configuration variables
extern float g_boredEventRange;
extern float g_proximityRadius;
extern float g_shoutRadius;
extern float g_visionRange;
extern int g_boredEventIntervalHours;
extern bool g_enableBoredEvents;
extern bool g_triggerBoredEvent;
extern float g_minFactionRelation;
extern float g_maxFactionRelation;
extern int g_dialogueSpeedSeconds;
extern float g_speechBubbleLife;
extern int g_rechatDispatchCooldownMs;
extern int g_ttsVolumePercent;
extern bool g_ttsEnabled;
extern bool g_enableDialogueMenuTts;
extern bool g_speedDialogue;
extern bool g_enableRegularDialogueCapture;
extern bool g_enableItemImageSync;
extern bool g_enableStatusHud;
extern bool g_enableNpcRename;
extern int g_dynamicProfileIntervalHours;
extern std::string g_narratorDisplayName;

// State tracking for inventory/debugger
extern std::string g_activeInventoryJson;
extern hand g_lastInventoryHand;
extern std::string g_activeCharName;
extern hand g_lastSelectionHand;
extern std::string g_playerInventoryJson;
extern hand g_playerHand;
extern CRITICAL_SECTION g_stateMutex;
extern int g_chatHotkey;
extern std::string g_chatHotkeyStr;
extern int g_generalHotkey;
extern std::string g_generalHotkeyStr;
extern int g_pushToTalkHotkey;
extern std::string g_pushToTalkHotkeyStr;
extern std::string g_chatMode;
extern bool g_autoChatEnabled;
extern bool g_useNearestPlayerSpeaker;
extern bool g_enableAnimalTalks;
extern std::string g_serverHost;
extern int g_serverPort;
extern std::map<std::string, std::string> g_uiTranslation;
std::string T(const std::string &key);

// UI Task queue (for thread-safe UI access)
enum ActionType {
  ACT_SAY,
  ACT_PLAY_TTS,
  ACT_ATTACK,
  ACT_STOP_ATTACK,
  ACT_SUICIDE,
  ACT_JOIN_PARTY,
  ACT_SET_TASK,
  ACT_START_FOLLOW,
  ACT_STOP_FOLLOW,
  ACT_SET_NPC_TOGGLE,
  ACT_NOTIFY,
  ACT_DROP_ITEM,
  ACT_GIVE_ITEM,
  ACT_LEAVE,
  ACT_GIVE_CATS,
  ACT_TAKE_CATS,
  ACT_FACTION_RELATIONS,
  ACT_SPAWN_ITEM,
  ACT_RELEASE,
  ACT_PICKUP_NPC,
  ACT_TAKE_ITEM,
  ACT_TRAVEL_LOCATION,
  ACT_DRINK_ITEM,
  ACT_FORCE_DRINK,
  ACT_USE_DRUGS,
  ACT_REMOVE_LIMB,
  ACT_CUT_HORNS,
  ACT_USE_OBJECT,
  ACT_KNOCKOUT,
  ACT_KILL,
  ACT_MOVE_TO
};

struct GameEvent {
  std::string type;
  std::string actor;
  std::string actorFaction;
  std::string target;
  std::string targetFaction;
  std::string message;
  DWORD timestamp;
};

extern std::deque<GameEvent> g_gameEvents;
extern CRITICAL_SECTION g_eventMutex;
void LogGameEvent(const std::string &type, const std::string &actor,
                  const std::string &actorFaction, const std::string &target,
                  const std::string &targetFaction, const std::string &message,
                  unsigned int actorSerial = 0,
                  unsigned int targetSerial = 0);

struct QueuedAction {
  ActionType type;
  hand actor;
  hand target;
  std::string message;    // Item name or notification text
  std::string targetToken; // Optional text fallback for explicit target resolution
  std::string ttsHash; // Optional soundcache hash for line-level TTS playback
  std::string utteranceId; // Delivery-tracked AI speech chunk id
  int taskValue;       // For ACT_SET_TASK, money amounts, or Relation Change
  DWORD proximityStartTick; // Non-zero while waiting to enter close-action range.
  bool proximityMoveIssued; // True after at least one approach move order.
  bool narratorNotification; // Queue narrator popups with speech timing.
  bool allowUnavailableSpeech; // Preserve reactions to forced limb removal.
  std::string autonomyDecisionId; // Set only for validated autonomy actions.

  QueuedAction()
      : type(ACT_NOTIFY), taskValue(0), proximityStartTick(0),
        proximityMoveIssued(false), narratorNotification(false),
        allowUnavailableSpeech(false) {}
};

struct PendingAutonomyCatalogMessage {
  std::string message;
  std::string decisionId;
};

extern std::deque<PendingAutonomyCatalogMessage>
    g_pendingAutonomyCatalogMessages;

// Register acquires g_msgMutex. Claim is called only while ProcessMessageQueue
// already owns g_msgMutex.
void RegisterPendingAutonomyCatalogMessage(const std::string &message,
                                           const std::string &decisionId);
bool ClaimPendingAutonomyCatalogMessageLocked(const std::string &message,
                                              std::string &decisionIdOut);
void CancelPendingAutonomyCatalogDecision(const std::string &decisionId);

extern std::deque<QueuedAction> g_uiActionQueue;
extern CRITICAL_SECTION g_uiMutex;
extern std::map<unsigned int, hand> g_followTargets;
struct TravelTarget {
  float x;
  float y;
  float z;
  std::string label;
};
extern std::map<unsigned int, TravelTarget> g_travelTargets;

// Background Name Assignment system
struct NameCheckItem {
  unsigned int serial;
  std::string name;
  std::string gender; // "Male" | "Female"
  std::string race;
  std::string faction;
  std::string contextJson;
};
extern std::deque<NameCheckItem> g_nameCheckQueue;
extern CRITICAL_SECTION g_nameCheckMutex;
extern std::set<unsigned int> g_identityRenameCompletedSerials;
extern std::map<unsigned int, DWORD> g_identityRenameNextAttemptTick;
extern std::set<unsigned int> g_renamedSerials;
extern std::set<unsigned int> g_activatedAnimalSerials;
extern DWORD g_lastContextPushTick;
extern DWORD g_lastWorldStatePushTick;

void MarkAnimalActivated(unsigned int serial);
bool IsAnimalActivated(unsigned int serial);

// Safely resolves the current GameWorld pointer from KenshiLib globals.
// Returns nullptr when the world pointer is not initialized or unreadable.
GameWorld *GetWorldSafe();

// Increments generation, flushes stale chat/TTS queue entries, and interrupts
// active TTS playback. Returns the new generation token.
LONG BeginChatInterruptGeneration();
LONG GetChatInterruptGeneration();
bool IsChatInterruptGenerationCurrent(LONG generation);
void BeginPlayerTtsPlaybackBarrier(LONG generation);
bool IsPlayerTtsPlaybackBarrierPending(LONG generation = 0);
bool ClearPlayerTtsPlaybackBarrier(LONG generation);
void SetNarratorDisplayName(const std::string &name);
std::string GetNarratorDisplayName();

void SetFollowTarget(unsigned int followerSerial, const hand &target);
void ClearFollowTarget(unsigned int followerSerial);
void ClearAllFollowTargets();
std::map<unsigned int, hand> SnapshotFollowTargets();
void SetTravelTarget(unsigned int actorSerial, float x, float y, float z,
                     const std::string &label);
void ClearTravelTarget(unsigned int actorSerial);
void ClearAllTravelTargets();
std::map<unsigned int, TravelTarget> SnapshotTravelTargets();

// Returns the current Stobe plugin version string compiled into the DLL.
const char *GetStobePluginVersion();
