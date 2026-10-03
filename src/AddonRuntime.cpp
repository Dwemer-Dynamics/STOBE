#include "AddonRuntime.h"

#include <kenshi/Character.h>
#include <kenshi/Faction.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/util/hand.h>

#include "AddonProtocol.h"
#include "ChatBox.h"
#include "Comm.h"
#include "Globals.h"
#include "Interaction.h"
#include "PlaythroughSession.h"
#include "StobeAddonApi.h"
#include "Utils.h"

#include <deque>
#include <map>
#include <string>

Character *ResolveLiveCharacterBySerial(GameWorld *world, unsigned int serial);

namespace Stobe {
namespace Addon {
namespace {

using Stobe::AddonProtocol::ExtCommand;
using Stobe::AddonProtocol::IsValidToken;
using Stobe::AddonProtocol::LowerAscii;

const size_t kMaxAddons = 16;
const size_t kMaxBridges = 64;
const size_t kMaxQueuedWork = 128;
const size_t kMaxPendingRequests = 128;
const size_t kWorkPerTick = 8;
const DWORD kPendingRequestLifetimeMs = 10 * 60 * 1000;
const DWORD kWorldReadyWindowMs = 2000;
const DWORD kEventWindowMs = 10000;
const int kEventsPerWindow = 20;

enum WorkKind {
  WORK_PLAYER_INPUT,
  WORK_CONTEXT_REQUEST,
  WORK_EVENT,
  WORK_CANCEL,
  WORK_ACTION_RESULT,
  WORK_CALLBACK,
  WORK_EXT_ACTION
};

struct WorkItem {
  WorkKind kind;
  StobeAddonId owner; // 0 for Stobe-originated work.
  unsigned long generation;
  StobeActorRef first;
  StobeActorRef second;
  StobeU32 number;
  int flag;
  std::string text;
  std::string key;
  StobeGameThreadCallback callback;
  void *userData;

  WorkItem()
      : kind(WORK_EVENT), owner(0), generation(0), number(0), flag(0),
        callback(NULL), userData(NULL) {
    first.serial = first.generation = 0;
    second.serial = second.generation = 0;
  }
};

struct AddonEntry {
  std::string name;
  std::string version;
  DWORD eventWindowStart;
  int eventCount;
};

struct BridgeEntry {
  StobeAddonId owner;
  std::string name;
  StobeActionHandler handler;
  void *userData;
};

struct PendingRequest {
  StobeAddonId owner;
  unsigned long generation;
  unsigned int actorSerial;
  std::string actorName;
  std::string command;
  std::string parameter;
  DWORD createdTick;
};

struct State {
  CRITICAL_SECTION lock;     // Registry and queue. Never held across callbacks.
  CRITICAL_SECTION dispatch; // Held by the game thread while calling an addon.
  std::map<StobeAddonId, AddonEntry> addons;
  std::map<std::string, BridgeEntry> bridges; // Key: lower-case bridge name.
  std::map<StobeU32, PendingRequest> pending;
  std::deque<WorkItem> work;
  StobeAddonId nextAddonId;
  StobeU32 nextRequestId;
  StobeAddonApiV1 api;
  State() : nextAddonId(1), nextRequestId(1) {
    InitializeCriticalSection(&lock);
    InitializeCriticalSection(&dispatch);
  }
};

volatile LONG g_gameThreadId = 0;
volatile LONG g_lastTickTime = 0;
volatile LONG g_inGameTick = 0;

BOOL CALLBACK InitState(PINIT_ONCE, PVOID, PVOID *context) {
  *context = new State();
  return TRUE;
}

State &Get() {
  static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
  PVOID context = NULL;
  InitOnceExecuteOnce(&once, InitState, NULL, &context);
  return *static_cast<State *>(context);
}

struct Lock {
  CRITICAL_SECTION *section;
  explicit Lock(CRITICAL_SECTION &value) : section(&value) {
    EnterCriticalSection(section);
  }
  ~Lock() { LeaveCriticalSection(section); }
};

bool IsGameThreadInTick() {
  return InterlockedCompareExchange(&g_inGameTick, 0, 0) != 0 &&
         static_cast<DWORD>(InterlockedCompareExchange(&g_gameThreadId, 0, 0)) ==
             GetCurrentThreadId();
}

bool CopyBoundedText(const char *value, size_t maxBytes, bool allowEmpty,
                     std::string &out) {
  if (!value) {
    if (!allowEmpty) {
      return false;
    }
    out.clear();
    return true;
  }
  size_t length = 0;
  while (length <= maxBytes && value[length] != '\0') {
    ++length;
  }
  if (length > maxBytes || (!allowEmpty && length == 0)) {
    return false;
  }
  out.assign(value, length);
  return true;
}

bool IsRegisteredLocked(State &state, StobeAddonId id) {
  return id != 0 && state.addons.find(id) != state.addons.end();
}

std::string AddonNameLocked(State &state, StobeAddonId id) {
  std::map<StobeAddonId, AddonEntry>::const_iterator it = state.addons.find(id);
  return it == state.addons.end() ? std::string("addon") : it->second.name;
}

int Enqueue(WorkItem &item) {
  State &state = Get();
  Lock lock(state.lock);
  if (item.owner != 0 && !IsRegisteredLocked(state, item.owner)) {
    return STOBE_E_NOT_REGISTERED;
  }
  if (state.work.size() >= kMaxQueuedWork) {
    return STOBE_E_LIMIT;
  }
  state.work.push_back(item);
  return STOBE_QUEUED;
}

// Removes an addon's registrations. Caller holds state.lock.
void RemoveAddonLocked(State &state, StobeAddonId id) {
  state.addons.erase(id);
  for (std::map<std::string, BridgeEntry>::iterator it = state.bridges.begin();
       it != state.bridges.end();) {
    if (it->second.owner == id) {
      state.bridges.erase(it++);
    } else {
      ++it;
    }
  }
  for (std::map<StobeU32, PendingRequest>::iterator it = state.pending.begin();
       it != state.pending.end();) {
    if (it->second.owner == id) {
      state.pending.erase(it++);
    } else {
      ++it;
    }
  }
  for (std::deque<WorkItem>::iterator it = state.work.begin();
       it != state.work.end();) {
    if (it->owner == id) {
      it = state.work.erase(it);
    } else {
      ++it;
    }
  }
}

void DisableFaultingAddon(StobeAddonId id, const std::string &where) {
  State &state = Get();
  std::string name;
  {
    Lock lock(state.lock);
    name = AddonNameLocked(state, id);
    RemoveAddonLocked(state, id);
  }
  Log("ADDON_API: disabled addon '" + name + "' after a fault in " + where);
}

bool IsCharacterUsable(Character *character) {
  return character && reinterpret_cast<uintptr_t>(character) > 0x1000;
}

std::string SafeName(Character *character) {
  if (!IsCharacterUsable(character)) {
    return "";
  }
  try {
    return character->getName();
  } catch (...) {
    return "";
  }
}

std::string SafeFactionName(Character *character) {
  if (!IsCharacterUsable(character)) {
    return "None";
  }
  try {
    Faction *faction = character->getFaction();
    return faction ? faction->getName() : std::string("None");
  } catch (...) {
    return "None";
  }
}

// Resolves a reference on the game thread; rejects earlier load generations.
int ResolveActor(GameWorld *world, const StobeActorRef &ref,
                 Character *&characterOut) {
  characterOut = NULL;
  if (ref.serial == 0) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  if (ref.generation != PlaythroughSession::Generation()) {
    return STOBE_E_STALE;
  }
  Character *character = ResolveLiveCharacterBySerial(world, ref.serial);
  if (!IsCharacterUsable(character)) {
    return STOBE_E_NOT_FOUND;
  }
  characterOut = character;
  return STOBE_OK;
}

Character *ResolvePlayerSpeaker(GameWorld *world) {
  if (!world || !world->player) {
    return NULL;
  }
  try {
    for (uint32_t i = 0; i < world->player->playerCharacters.size(); ++i) {
      Character *candidate = world->player->playerCharacters[i];
      if (IsCharacterUsable(candidate) && !candidate->isDead() &&
          !candidate->isUnconcious()) {
        return candidate;
      }
    }
  } catch (...) {
  }
  return NULL;
}

bool IsSerialInStobeSpeech(unsigned int serial) {
  bool inSpeech = false;
  if (TryEnterCriticalSection(&g_stateMutex)) {
    inSpeech = g_talkTargetHand.isValid() && g_talkTargetHand.serial == serial;
    LeaveCriticalSection(&g_stateMutex);
  }
  if (inSpeech || !TryEnterCriticalSection(&g_uiMutex)) {
    return inSpeech;
  }
  for (std::deque<QueuedAction>::const_iterator it = g_uiActionQueue.begin();
       it != g_uiActionQueue.end(); ++it) {
    if ((it->type == ACT_SAY || it->type == ACT_PLAY_TTS) &&
        ((it->actor.isValid() && it->actor.serial == serial) ||
         (it->target.isValid() && it->target.serial == serial))) {
      inSpeech = true;
      break;
    }
  }
  LeaveCriticalSection(&g_uiMutex);
  return inSpeech;
}

std::string ResultLabel(int code) {
  switch (code) {
  case STOBE_E_STALE:
    return "stale_actor";
  case STOBE_E_NOT_FOUND:
    return "actor_not_loaded";
  case STOBE_E_BUSY:
    return "busy";
  case STOBE_E_DISABLED:
    return "interaction_off";
  case STOBE_E_INVALID_ARGUMENT:
    return "invalid";
  default:
    return ToString(code);
  }
}

std::string WithoutSeparators(std::string value) {
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '@' || value[i] == '\r' || value[i] == '\n') {
      value[i] = ' ';
    }
  }
  return value;
}

int CurrentGameTs(GameWorld *world) {
  try {
    const int seconds =
        static_cast<int>(world->getTimeStamp_inGameHours().getTotalSeconds());
    return seconds > 0 ? seconds : 0;
  } catch (...) {
    return 0;
  }
}

// Sends the CHIM-compatible funcret record "command@<cmd>@<param>@<result>"
// and a readable infoaction line for the NPC's event history.
void ReportExternalActionOutcome(GameWorld *world, unsigned int actorSerial,
                                 const std::string &actorName,
                                 const std::string &command,
                                 const std::string &parameter, bool succeeded,
                                 const std::string &detail) {
  Character *actor = ResolveLiveCharacterBySerial(world, actorSerial);
  std::string name = SafeName(actor);
  if (name.empty()) {
    name = actorName.empty() ? std::string("NPC") : actorName;
  }
  std::string result = succeeded ? "completed" : "failed";
  if (!detail.empty()) {
    result += ": " + detail;
  }
  const std::string data = "command@" + WithoutSeparators(command) + "@" +
                           WithoutSeparators(parameter) + "@" +
                           WithoutSeparators(result);
  AsyncPostToStobeSerial(
      L"/StobeServer/stream.php?DATA=" +
          ToWide(BuildStreamQueryData("funcret", data, CurrentGameTs(world))),
      "");
  const std::string message = "external action " + command + " " + result;
  Log("ADDON_ACTION: actor_serial=" + ToString(actorSerial) + " " + message);
  LogGameEvent("infoaction", name, SafeFactionName(actor), "None", "None",
               message, actorSerial, 0);
}

void RunPlayerInput(GameWorld *world, const WorkItem &item) {
  Character *target = NULL;
  int code = ResolveActor(world, item.first, target);
  Character *speaker = ResolvePlayerSpeaker(world);
  if (code == STOBE_OK && !Stobe::Interaction::Allowed()) {
    code = STOBE_E_DISABLED;
  } else if (code == STOBE_OK && (Stobe::UI::g_chatWindow || !speaker)) {
    code = STOBE_E_BUSY; // Never overwrite the player's open chat state.
  }
  if (code != STOBE_OK) {
    Log("ADDON_API: player input dropped target_serial=" +
        ToString(item.first.serial) + " reason=" + ResultLabel(code));
    return;
  }

  // The voice submission path changes the chat UI's remembered target/mode.
  const std::string savedPlayer = Stobe::UI::g_chatPlayerNameStr;
  const std::string savedTargetName = Stobe::UI::g_chatTargetNameStr;
  const std::string savedTargetHandle = Stobe::UI::g_chatTargetHandleStr;
  const std::string savedMode = g_chatMode;
  const size_t savedModeIndex = Stobe::UI::g_lastChatModeIndex;
  Stobe::UI::SubmitVoiceChatText(item.text, SafeName(speaker),
                                 ToString(speaker->getHandle().serial),
                                 SafeName(target), ToString(item.first.serial),
                                 "chat");
  Stobe::UI::g_chatPlayerNameStr = savedPlayer;
  Stobe::UI::g_chatTargetNameStr = savedTargetName;
  Stobe::UI::g_chatTargetHandleStr = savedTargetHandle;
  g_chatMode = savedMode;
  Stobe::UI::g_lastChatModeIndex = savedModeIndex;
  Log("ADDON_API: player input submitted target_serial=" +
      ToString(item.first.serial) + " text_len=" +
      ToString(static_cast<int>(item.text.size())));
}

void RunContextRequest(GameWorld *world, const WorkItem &item) {
  Character *speaker = NULL;
  Character *listener = NULL;
  int code = ResolveActor(world, item.first, speaker);
  if (code == STOBE_OK && item.second.serial != 0) {
    code = ResolveActor(world, item.second, listener);
  }
  if (code == STOBE_OK && !Stobe::Interaction::Allowed()) {
    code = STOBE_E_DISABLED;
  } else if (code == STOBE_OK && (Stobe::UI::IsAiRequestActive() ||
                                  Stobe::UI::IsDirectorSceneActive())) {
    code = STOBE_E_BUSY; // Addon requests do not interrupt active dialogue.
  }
  bool dispatched = false;
  if (code == STOBE_OK) {
    dispatched = Stobe::UI::TriggerBoredEvent(
        world, true, SafeName(speaker), ToString(item.first.serial), 0,
        listener ? SafeName(listener) : std::string(),
        listener ? ToString(item.second.serial) : std::string(), item.text);
  }
  Log("ADDON_API: contextual request speaker_serial=" +
      ToString(item.first.serial) + " result=" +
      (code != STOBE_OK ? ResultLabel(code)
                        : std::string(dispatched ? "dispatched" : "not_eligible")));
}

void RunEvent(GameWorld *world, const WorkItem &item,
              const std::string &addonName) {
  Character *actor = NULL;
  if (item.first.serial != 0) {
    const int code = ResolveActor(world, item.first, actor);
    if (code != STOBE_OK) {
      Log("ADDON_API: event dropped actor_serial=" + ToString(item.first.serial) +
          " reason=" + ResultLabel(code));
      return;
    }
  }
  if (item.number == STOBE_EVENT_PLUGIN_STATE) {
    LogGameEvent("addon_state", addonName, "None", "None", "None",
                 item.key + "=" + item.text, item.first.serial, 0);
    return;
  }
  const std::string actorName = actor ? SafeName(actor) : addonName;
  LogGameEvent("infoaction", actorName, SafeFactionName(actor), "None", "None",
               item.text, item.first.serial, 0);
}

void RunActionResult(GameWorld *world, const WorkItem &item) {
  PendingRequest request;
  {
    State &state = Get();
    Lock lock(state.lock);
    std::map<StobeU32, PendingRequest>::iterator it =
        state.pending.find(item.number);
    if (it == state.pending.end() || it->second.owner != item.owner) {
      Log("ADDON_ACTION: result ignored for unknown request " +
          ToString(item.number));
      return;
    }
    request = it->second;
    state.pending.erase(it);
  }
  if (request.generation != PlaythroughSession::Generation()) {
    Log("ADDON_ACTION: result dropped for request " + ToString(item.number) +
        " from an earlier load");
    return;
  }
  ReportExternalActionOutcome(world, request.actorSerial, request.actorName,
                              request.command, request.parameter,
                              item.flag != 0, item.text);
}

void InvokeCallback(const WorkItem &item) {
  State &state = Get();
  Lock dispatch(state.dispatch);
  {
    Lock lock(state.lock);
    if (!IsRegisteredLocked(state, item.owner)) {
      return;
    }
  }
  bool faulted = false;
  try {
    item.callback(item.userData);
  } catch (...) {
    faulted = true;
  }
  if (faulted) {
    DisableFaultingAddon(item.owner, "game-thread callback");
  }
}

void RunExternalAction(GameWorld *world, const WorkItem &item) {
  const unsigned int actorSerial = item.first.serial;
  Character *actor = ResolveLiveCharacterBySerial(world, actorSerial);
  const std::string actorName = SafeName(actor);
  ExtCommand parsed;
  if (!Stobe::AddonProtocol::ParseExtCommand(item.key, parsed) ||
      item.flag != 0) {
    ReportExternalActionOutcome(world, actorSerial, actorName, item.key,
                                item.text, false,
                                item.flag != 0 ? "parameter too long"
                                               : "malformed command");
    return;
  }
  if (!IsCharacterUsable(actor)) {
    Log("ADDON_ACTION: " + parsed.command + " dropped; actor_serial=" +
        ToString(actorSerial) + " is no longer loaded");
    return;
  }

  State &state = Get();
  Lock dispatch(state.dispatch);
  BridgeEntry bridge;
  StobeU32 requestId = 0;
  bool bridgeFound = false;
  {
    Lock lock(state.lock);
    std::map<std::string, BridgeEntry>::const_iterator it =
        state.bridges.find(LowerAscii(parsed.bridge));
    bridgeFound = it != state.bridges.end();
    if (bridgeFound && state.pending.size() < kMaxPendingRequests) {
      bridge = it->second;
      requestId = state.nextRequestId++;
      if (state.nextRequestId == 0) {
        state.nextRequestId = 1;
      }
      PendingRequest request;
      request.owner = bridge.owner;
      request.generation = item.generation;
      request.actorSerial = actorSerial;
      request.actorName = actorName;
      request.command = parsed.command;
      request.parameter = item.text;
      request.createdTick = GetTickCount();
      state.pending[requestId] = request;
    }
  }
  if (requestId == 0) {
    ReportExternalActionOutcome(world, actorSerial, actorName, parsed.command,
                                item.text, false,
                                bridgeFound ? std::string("too many pending requests")
                                            : "no registered handler for bridge " +
                                                  parsed.bridge);
    return;
  }

  StobeActionRequest request;
  request.struct_size = sizeof(StobeActionRequest);
  request.request_id = requestId;
  request.actor.serial = actorSerial;
  request.actor.generation = item.generation;
  request.actor_name = actorName.c_str();
  request.command = parsed.command.c_str();
  request.bridge = parsed.bridge.c_str();
  request.action = parsed.action.c_str();
  request.parameter = item.text.c_str();

  int verdict = STOBE_ACTION_REJECTED;
  bool faulted = false;
  try {
    verdict = bridge.handler(bridge.userData, &request);
  } catch (...) {
    faulted = true;
  }
  if (faulted) {
    DisableFaultingAddon(bridge.owner, "action handler " + bridge.name);
  }
  if (verdict == STOBE_ACTION_ACCEPTED && !faulted) {
    Log("ADDON_ACTION: " + parsed.command + " accepted by bridge " + bridge.name +
        " request=" + ToString(requestId) + " actor_serial=" +
        ToString(actorSerial));
    return;
  }
  {
    Lock lock(state.lock);
    state.pending.erase(requestId);
  }
  ReportExternalActionOutcome(world, actorSerial, actorName, parsed.command,
                              item.text, false,
                              faulted ? "handler fault" : "rejected by handler");
}

void ExpirePendingRequests() {
  State &state = Get();
  const DWORD now = GetTickCount();
  const unsigned long generation = PlaythroughSession::Generation();
  std::deque<StobeU32> expired;
  {
    Lock lock(state.lock);
    for (std::map<StobeU32, PendingRequest>::iterator it = state.pending.begin();
         it != state.pending.end();) {
      if (it->second.generation != generation ||
          now - it->second.createdTick > kPendingRequestLifetimeMs) {
        expired.push_back(it->first);
        state.pending.erase(it++);
      } else {
        ++it;
      }
    }
  }
  // Expiry is local bookkeeping only; no outcome is invented for the server.
  for (size_t i = 0; i < expired.size(); ++i) {
    Log("ADDON_ACTION: request " + ToString(expired[i]) +
        " expired without a reported result");
  }
}

// ---- API entry points -----------------------------------------------------

int STOBE_CALL ApiRegisterAddon(const StobeAddonInfo *info, StobeAddonId *outId) {
  if (!info || !outId || info->struct_size < sizeof(StobeAddonInfo) ||
      info->reserved != 0) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  AddonEntry entry;
  if (!CopyBoundedText(info->name, STOBE_MAX_NAME_BYTES, false, entry.name) ||
      !IsValidToken(entry.name) ||
      !CopyBoundedText(info->version, STOBE_MAX_NAME_BYTES, true,
                       entry.version)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  entry.eventWindowStart = GetTickCount();
  entry.eventCount = 0;
  State &state = Get();
  StobeAddonId id = 0;
  {
    Lock lock(state.lock);
    if (state.addons.size() >= kMaxAddons) {
      return STOBE_E_LIMIT;
    }
    for (std::map<StobeAddonId, AddonEntry>::const_iterator it =
             state.addons.begin();
         it != state.addons.end(); ++it) {
      if (LowerAscii(it->second.name) == LowerAscii(entry.name)) {
        return STOBE_E_CONFLICT;
      }
    }
    id = state.nextAddonId++;
    state.addons[id] = entry;
  }
  *outId = id;
  Log("ADDON_API: registered addon '" + entry.name + "' version '" +
      entry.version + "' id=" + ToString(id));
  return STOBE_OK;
}

int STOBE_CALL ApiUnregisterAddon(StobeAddonId id) {
  State &state = Get();
  std::string name;
  {
    Lock lock(state.lock);
    if (!IsRegisteredLocked(state, id)) {
      return STOBE_E_NOT_REGISTERED;
    }
    name = AddonNameLocked(state, id);
    RemoveAddonLocked(state, id);
  }
  // Wait for an in-flight callback on the game thread (re-entrant there).
  { Lock dispatch(state.dispatch); }
  Log("ADDON_API: unregistered addon '" + name + "'");
  return STOBE_OK;
}

int STOBE_CALL ApiMakeActorRef(StobeU32 serial, StobeActorRef *outRef) {
  if (serial == 0 || !outRef) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  outRef->serial = serial;
  outRef->generation = static_cast<StobeU32>(PlaythroughSession::Generation());
  return STOBE_OK;
}

StobeU32 STOBE_CALL ApiGetRuntimeFlags() {
  StobeU32 flags = 0;
  if (Stobe::Interaction::Allowed()) {
    flags |= STOBE_RUNTIME_INTERACTION_ENABLED;
  }
  if (Stobe::UI::IsAiRequestActive()) {
    flags |= STOBE_RUNTIME_AI_REQUEST_ACTIVE;
  }
  if (Stobe::UI::IsDirectorSceneActive()) {
    flags |= STOBE_RUNTIME_DIRECTOR_ACTIVE;
  }
  const DWORD lastTick =
      static_cast<DWORD>(InterlockedCompareExchange(&g_lastTickTime, 0, 0));
  if (lastTick != 0 && GetTickCount() - lastTick <= kWorldReadyWindowMs) {
    flags |= STOBE_RUNTIME_WORLD_READY;
  }
  return flags;
}

int STOBE_CALL ApiGetActorState(StobeActorRef actor, StobeActorState *outState) {
  if (!outState || outState->struct_size < sizeof(StobeActorState)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  if (!IsGameThreadInTick()) {
    return STOBE_E_WRONG_THREAD;
  }
  outState->flags = 0;
  Character *character = NULL;
  const int code = ResolveActor(GetWorldSafe(), actor, character);
  if (code != STOBE_OK) {
    return code;
  }
  StobeU32 flags = STOBE_ACTOR_LOADED;
  try {
    if (!character->isDead()) {
      flags |= STOBE_ACTOR_ALIVE;
      if (!character->isUnconcious()) {
        flags |= STOBE_ACTOR_CONSCIOUS;
      }
    }
    Faction *faction = character->getFaction();
    if (faction && faction->isThePlayer()) {
      flags |= STOBE_ACTOR_PLAYER_SQUAD;
    }
  } catch (...) {
  }
  if (IsSerialInStobeSpeech(actor.serial)) {
    flags |= STOBE_ACTOR_IN_STOBE_SPEECH;
  }
  outState->flags = flags;
  return STOBE_OK;
}

int STOBE_CALL ApiSendPlayerInput(StobeAddonId id, StobeActorRef target,
                                  const char *text) {
  WorkItem item;
  if (target.serial == 0 ||
      !CopyBoundedText(text, STOBE_MAX_TEXT_BYTES, false, item.text)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  item.kind = WORK_PLAYER_INPUT;
  item.owner = id;
  item.generation = target.generation;
  item.first = target;
  return Enqueue(item);
}

int STOBE_CALL ApiRequestContextualResponse(StobeAddonId id,
                                            StobeActorRef speaker,
                                            StobeActorRef listener,
                                            const char *direction) {
  WorkItem item;
  if (speaker.serial == 0 || listener.serial == speaker.serial ||
      !CopyBoundedText(direction, STOBE_MAX_TEXT_BYTES, true, item.text)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  item.kind = WORK_CONTEXT_REQUEST;
  item.owner = id;
  item.generation = speaker.generation;
  item.first = speaker;
  item.second = listener;
  return Enqueue(item);
}

int STOBE_CALL ApiSendEvent(StobeAddonId id, StobeU32 kind, StobeActorRef actor,
                            const char *key, const char *text) {
  WorkItem item;
  item.kind = WORK_EVENT;
  item.owner = id;
  item.number = kind;
  item.first = actor;
  item.generation = actor.serial != 0 ? actor.generation
                                      : PlaythroughSession::Generation();
  if (kind == STOBE_EVENT_INFO) {
    if (key || !CopyBoundedText(text, STOBE_MAX_TEXT_BYTES, false, item.text)) {
      return STOBE_E_INVALID_ARGUMENT;
    }
  } else if (kind == STOBE_EVENT_PLUGIN_STATE) {
    if (!CopyBoundedText(key, STOBE_MAX_NAME_BYTES, false, item.key) ||
        !IsValidToken(item.key) ||
        !CopyBoundedText(text, STOBE_MAX_TEXT_BYTES, true, item.text)) {
      return STOBE_E_INVALID_ARGUMENT;
    }
  } else {
    return STOBE_E_INVALID_ARGUMENT;
  }
  State &state = Get();
  {
    Lock lock(state.lock);
    std::map<StobeAddonId, AddonEntry>::iterator it = state.addons.find(id);
    if (it == state.addons.end()) {
      return STOBE_E_NOT_REGISTERED;
    }
    const DWORD now = GetTickCount();
    if (now - it->second.eventWindowStart > kEventWindowMs) {
      it->second.eventWindowStart = now;
      it->second.eventCount = 0;
    }
    if (it->second.eventCount >= kEventsPerWindow) {
      return STOBE_E_LIMIT;
    }
    ++it->second.eventCount;
  }
  return Enqueue(item);
}

int STOBE_CALL ApiCancelDialogue(StobeAddonId id) {
  WorkItem item;
  item.kind = WORK_CANCEL;
  item.owner = id;
  item.generation = PlaythroughSession::Generation();
  return Enqueue(item);
}

int STOBE_CALL ApiRegisterActionBridge(StobeAddonId id, const char *bridge,
                                       StobeActionHandler handler,
                                       void *userData) {
  std::string name;
  if (!handler ||
      !CopyBoundedText(bridge, STOBE_MAX_NAME_BYTES, false, name) ||
      !Stobe::AddonProtocol::IsValidBridgeName(name)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  State &state = Get();
  std::string addonName;
  {
    Lock lock(state.lock);
    if (!IsRegisteredLocked(state, id)) {
      return STOBE_E_NOT_REGISTERED;
    }
    const std::string key = LowerAscii(name);
    std::map<std::string, BridgeEntry>::iterator it = state.bridges.find(key);
    if (it != state.bridges.end() && it->second.owner != id) {
      return STOBE_E_CONFLICT;
    }
    if (it == state.bridges.end() && state.bridges.size() >= kMaxBridges) {
      return STOBE_E_LIMIT;
    }
    BridgeEntry entry;
    entry.owner = id;
    entry.name = name;
    entry.handler = handler;
    entry.userData = userData;
    state.bridges[key] = entry;
    addonName = AddonNameLocked(state, id);
  }
  Log("ADDON_API: addon '" + addonName + "' registered action bridge " + name);
  return STOBE_OK;
}

int STOBE_CALL ApiUnregisterActionBridge(StobeAddonId id, const char *bridge) {
  std::string name;
  if (!CopyBoundedText(bridge, STOBE_MAX_NAME_BYTES, false, name)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  State &state = Get();
  {
    Lock lock(state.lock);
    if (!IsRegisteredLocked(state, id)) {
      return STOBE_E_NOT_REGISTERED;
    }
    std::map<std::string, BridgeEntry>::iterator it =
        state.bridges.find(LowerAscii(name));
    if (it == state.bridges.end() || it->second.owner != id) {
      return STOBE_E_NOT_FOUND;
    }
    state.bridges.erase(it);
  }
  { Lock dispatch(state.dispatch); }
  return STOBE_OK;
}

int STOBE_CALL ApiReportActionResult(StobeAddonId id, StobeU32 requestId,
                                     int succeeded, const char *message) {
  WorkItem item;
  if (requestId == 0 ||
      !CopyBoundedText(message, STOBE_MAX_TEXT_BYTES, true, item.text)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  State &state = Get();
  {
    Lock lock(state.lock);
    std::map<StobeU32, PendingRequest>::const_iterator it =
        state.pending.find(requestId);
    if (it == state.pending.end() || it->second.owner != id) {
      return STOBE_E_NOT_FOUND;
    }
    if (it->second.generation != PlaythroughSession::Generation()) {
      return STOBE_E_STALE;
    }
  }
  item.kind = WORK_ACTION_RESULT;
  item.owner = id;
  item.number = requestId;
  item.flag = succeeded ? 1 : 0;
  item.generation = PlaythroughSession::Generation();
  return Enqueue(item);
}

int STOBE_CALL ApiQueueGameThreadCallback(StobeAddonId id,
                                          StobeGameThreadCallback fn,
                                          void *userData) {
  if (!fn) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  WorkItem item;
  item.kind = WORK_CALLBACK;
  item.owner = id;
  item.generation = PlaythroughSession::Generation();
  item.callback = fn;
  item.userData = userData;
  return Enqueue(item);
}

BOOL CALLBACK InitApiTable(PINIT_ONCE, PVOID, PVOID *) {
  StobeAddonApiV1 &api = Get().api;
  api.struct_size = sizeof(StobeAddonApiV1);
  api.api_version = STOBE_ADDON_API_VERSION;
  api.stobe_version = GetStobePluginVersion();
  api.RegisterAddon = ApiRegisterAddon;
  api.UnregisterAddon = ApiUnregisterAddon;
  api.MakeActorRef = ApiMakeActorRef;
  api.GetRuntimeFlags = ApiGetRuntimeFlags;
  api.GetActorState = ApiGetActorState;
  api.SendPlayerInput = ApiSendPlayerInput;
  api.RequestContextualResponse = ApiRequestContextualResponse;
  api.SendEvent = ApiSendEvent;
  api.CancelDialogue = ApiCancelDialogue;
  api.RegisterActionBridge = ApiRegisterActionBridge;
  api.UnregisterActionBridge = ApiUnregisterActionBridge;
  api.ReportActionResult = ApiReportActionResult;
  api.QueueGameThreadCallback = ApiQueueGameThreadCallback;
  return TRUE;
}

// The table layout is the ABI; appended fields must not move these.
static_assert(sizeof(StobeActorRef) == 8, "StobeActorRef ABI");
static_assert(sizeof(StobeAddonInfo) == 24, "StobeAddonInfo ABI");
static_assert(sizeof(StobeActorState) == 8, "StobeActorState ABI");
static_assert(sizeof(StobeActionRequest) == 56, "StobeActionRequest ABI");
static_assert(sizeof(StobeAddonApiV1) == 16 + 13 * sizeof(void *),
              "StobeAddonApiV1 ABI");

} // namespace

void GameThreadTick(GameWorld *world) {
  if (!world) {
    return;
  }
  InterlockedExchange(&g_gameThreadId, static_cast<LONG>(GetCurrentThreadId()));
  InterlockedExchange(&g_lastTickTime, static_cast<LONG>(GetTickCount()));
  State &state = Get();
  {
    Lock lock(state.lock);
    if (state.work.empty() && state.pending.empty()) {
      return;
    }
  }
  InterlockedExchange(&g_inGameTick, 1);
  ExpirePendingRequests();
  const unsigned long generation = PlaythroughSession::Generation();
  for (size_t processed = 0; processed < kWorkPerTick; ++processed) {
    WorkItem item;
    std::string addonName;
    {
      Lock lock(state.lock);
      if (state.work.empty()) {
        break;
      }
      item = state.work.front();
      state.work.pop_front();
      if (item.owner != 0 && !IsRegisteredLocked(state, item.owner)) {
        continue;
      }
      addonName = AddonNameLocked(state, item.owner);
    }
    if (item.generation != generation) {
      Log("ADDON_API: dropped queued work from an earlier load kind=" +
          ToString(static_cast<int>(item.kind)));
      continue;
    }
    switch (item.kind) {
    case WORK_PLAYER_INPUT:
      RunPlayerInput(world, item);
      break;
    case WORK_CONTEXT_REQUEST:
      RunContextRequest(world, item);
      break;
    case WORK_EVENT:
      RunEvent(world, item, addonName);
      break;
    case WORK_CANCEL:
      BeginChatInterruptGeneration(true);
      Log("ADDON_API: addon '" + addonName + "' cancelled Stobe dialogue");
      break;
    case WORK_ACTION_RESULT:
      RunActionResult(world, item);
      break;
    case WORK_CALLBACK:
      InvokeCallback(item);
      break;
    case WORK_EXT_ACTION:
      RunExternalAction(world, item);
      break;
    }
  }
  InterlockedExchange(&g_inGameTick, 0);
}

void QueueExternalAction(unsigned int actorSerial, const std::string &rawCommand,
                         const std::string &parameter) {
  WorkItem item;
  item.kind = WORK_EXT_ACTION;
  item.generation = PlaythroughSession::Generation();
  item.first.serial = actorSerial;
  item.first.generation = static_cast<StobeU32>(item.generation);
  item.key = rawCommand;
  if (parameter.size() > STOBE_MAX_TEXT_BYTES) {
    item.flag = 1; // Rejected on dispatch; never passed truncated.
  } else {
    item.text = parameter;
  }
  const int code = Enqueue(item);
  if (code != STOBE_QUEUED) {
    Log("ADDON_ACTION: " + rawCommand + " dropped; addon queue full");
  }
}

} // namespace Addon
} // namespace Stobe

extern "C" __declspec(dllexport) const StobeAddonApiV1 *STOBE_CALL
Stobe_GetApi(StobeU32 version) {
  if (version != STOBE_ADDON_API_VERSION) {
    return NULL;
  }
  static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
  InitOnceExecuteOnce(&once, Stobe::Addon::InitApiTable, NULL, NULL);
  return &Stobe::Addon::Get().api;
}
