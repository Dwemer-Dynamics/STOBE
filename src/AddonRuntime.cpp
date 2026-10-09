#include "AddonRuntime.h"

#include <kenshi/Character.h>
#include <kenshi/Faction.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/util/hand.h>

#include "AddonProtocol.h"
#include "AudioPlayback.h"
#include "ChatBox.h"
#include "Comm.h"
#include "Globals.h"
#include "Interaction.h"
#include "PlaythroughSession.h"
#include "StobeAddonApi.h"
#include "Utils.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

Character *ResolveLiveCharacterBySerial(GameWorld *world, unsigned int serial);

namespace Stobe {
namespace Addon {
namespace {

using Stobe::AddonProtocol::ActorLockTable;
using Stobe::AddonProtocol::ExtCommand;
using Stobe::AddonProtocol::IsValidToken;
using Stobe::AddonProtocol::LowerAscii;

const size_t kMaxAddons = 16;
const size_t kMaxBridges = 64;
const size_t kMaxQueuedWork = 128;
const size_t kMaxPendingRequests = 128;
const size_t kMaxTickets = 128;
const size_t kMaxActorLocks = 64;
const size_t kMaxAgentRegistrations = 64;
const size_t kMaxRefreshRequests = 32;
const size_t kMaxFollowupReports = 16; // the server's open-row cap
const size_t kMaxAgentScan = 512; // loaded characters examined per query
const size_t kWorkPerTick = 8;
const DWORD kPendingRequestLifetimeMs = 10 * 60 * 1000;
const DWORD kExpiryIntervalMs = 1000;
const DWORD kWorldReadyWindowMs = 2000;
const DWORD kEventWindowMs = 10000;
const int kEventsPerWindow = 20;

enum WorkKind {
  WORK_PLAYER_INPUT,
  WORK_CONTEXT_REQUEST,
  WORK_EVENT,
  WORK_CANCEL,
  WORK_CALLBACK,
  WORK_EXT_ACTION,
  WORK_SET_INTERACTION,
  WORK_ADDON_MESSAGE, // number: STOBE_MESSAGE_*
  WORK_ADDON_REACTION // number: STOBE_REACTION_*
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
  StobeU32 ticket; // 0 when the caller did not ask for one.

  WorkItem()
      : kind(WORK_EVENT), owner(0), generation(0), number(0), flag(0),
        callback(NULL), userData(NULL), ticket(0) {
    first.serial = first.generation = 0;
    second.serial = second.generation = 0;
  }
};

struct AddonEntry {
  std::string name;
  std::string version;
  DWORD eventWindowStart;
  int eventCount;
  StobeControlCallback controlCallback;
  void *controlUserData;
};

// A control request's feedback record. Interaction tickets resolve from the
// Interaction request sequence once dispatched (hasSeq).
struct Ticket {
  StobeAddonId owner;
  StobeU32 kind;
  StobeU32 state;
  int reason;
  bool hasSeq;
  LONG seq;
  bool noticeQueued;
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
  StobeU32 aid; // stobe.addon_followup.v1 id, 0 for legacy actions
};

// The terminal result of an accepted request, reported on the game thread.
struct Outcome {
  StobeU32 requestId;
  PendingRequest request;
  bool succeeded;
  std::string detail;
  Outcome() : requestId(0), succeeded(false) {}
};

struct State {
  CRITICAL_SECTION lock;     // Registry and queue. Never held across callbacks.
  CRITICAL_SECTION dispatch; // Held by the game thread while calling an addon.
  std::map<StobeAddonId, AddonEntry> addons;
  std::map<std::string, BridgeEntry> bridges; // Key: lower-case bridge name.
  // An accepted request lives in exactly one of pending or outcomes; whoever
  // removes it from pending owns its single outcome. Together they hold at
  // most kMaxPendingRequests entries.
  std::map<StobeU32, PendingRequest> pending;
  std::deque<Outcome> outcomes;
  std::deque<WorkItem> work;
  // At most kMaxTickets; each ticket has at most one queued notice.
  std::map<StobeU32, Ticket> tickets;
  std::deque<StobeU32> notices;
  size_t seqTickets; // Pending tickets waiting on an interaction sequence.
  LONG seenRequested, seenSettled; // Progress at the last ticket scan.
  // Leaf lock below state.lock guarding dialogue locks and busy flags;
  // readers on any thread take it shared.
  SRWLOCK locksLock;
  ActorLockTable locks;
  ActorLockTable busy;
  // Agent registration overrides; an actor is in at most one of them.
  ActorLockTable agentIn;
  ActorLockTable agentOut;
  unsigned long lockGeneration; // Game thread only.
  // Pending refresh parts by actor and requesting addon.
  Stobe::AddonProtocol::RefreshTable refresh;
  // Aid outcomes waiting for the follow-up transport; the game thread drains.
  Stobe::AddonProtocol::FollowupQueue followups;
  StobeAddonId nextAddonId;
  StobeU32 nextRequestId;
  StobeU32 nextTicket;
  DWORD lastExpiryTick;
  unsigned long expiryGeneration;
  StobeAddonApiV1 api;
  StobeAddonApiV2 apiV2;
  StobeAddonApiV3 apiV3;
  StobeAddonApiV4 apiV4;
  State()
      : seqTickets(0), seenRequested(-2), seenSettled(-2),
        locks(kMaxActorLocks), busy(kMaxActorLocks),
        agentIn(kMaxAgentRegistrations), agentOut(kMaxAgentRegistrations),
        lockGeneration(0), refresh(kMaxRefreshRequests),
        followups(kMaxFollowupReports),
        nextAddonId(1),
        nextRequestId(1), nextTicket(1), lastExpiryTick(0),
        expiryGeneration(0) {
    InitializeCriticalSection(&lock);
    InitializeCriticalSection(&dispatch);
    InitializeSRWLock(&locksLock);
  }
};

// Let IsActorLocked and IsActorBusy skip the lock while none exist.
volatile LONG g_actorLockCount = 0;
volatile LONG g_actorBusyCount = 0;
volatile LONG g_agentRegistrationCount = 0;
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

// Caller holds state.lock. Returns 0 when every retained ticket is pending
// or awaiting its callback; otherwise the oldest finished one is evicted.
StobeU32 CreateTicketLocked(State &state, StobeAddonId owner, StobeU32 kind) {
  if (state.tickets.size() >= kMaxTickets) {
    std::map<StobeU32, Ticket>::iterator it = state.tickets.begin();
    while (it != state.tickets.end() &&
           (it->second.state == STOBE_CONTROL_PENDING || it->second.noticeQueued)) {
      ++it;
    }
    if (it == state.tickets.end()) {
      return 0;
    }
    state.tickets.erase(it);
  }
  const StobeU32 id = state.nextTicket++;
  if (state.nextTicket == 0) {
    state.nextTicket = 1;
  }
  Ticket ticket;
  ticket.owner = owner;
  ticket.kind = kind;
  ticket.state = STOBE_CONTROL_PENDING;
  ticket.reason = 0;
  ticket.hasSeq = false;
  ticket.seq = 0;
  ticket.noticeQueued = false;
  state.tickets[id] = ticket;
  return id;
}

// Caller holds state.lock. A ticket leaves PENDING once; later calls are
// ignored. The owner's callback, if any, is queued for the game thread.
void FinishTicketLocked(State &state, StobeU32 id, StobeU32 result, int reason) {
  std::map<StobeU32, Ticket>::iterator it = state.tickets.find(id);
  if (id == 0 || it == state.tickets.end() ||
      it->second.state != STOBE_CONTROL_PENDING) {
    return;
  }
  if (it->second.hasSeq) {
    --state.seqTickets;
  }
  it->second.state = result;
  it->second.reason = reason;
  std::map<StobeAddonId, AddonEntry>::const_iterator addon =
      state.addons.find(it->second.owner);
  if (addon != state.addons.end() && addon->second.controlCallback) {
    it->second.noticeQueued = true;
    state.notices.push_back(id);
  }
}

void FinishTicket(StobeU32 id, StobeU32 result, int reason) {
  if (id == 0) {
    return;
  }
  State &state = Get();
  Lock lock(state.lock);
  FinishTicketLocked(state, id, result, reason);
}

// outTicket non-NULL asks for a feedback ticket of ticketKind.
int Enqueue(WorkItem &item, StobeU32 ticketKind = 0,
            StobeU32 *outTicket = NULL) {
  State &state = Get();
  Lock lock(state.lock);
  if (item.owner != 0 && !IsRegisteredLocked(state, item.owner)) {
    return STOBE_E_NOT_REGISTERED;
  }
  if (state.work.size() >= kMaxQueuedWork) {
    return STOBE_E_LIMIT;
  }
  if (outTicket) {
    item.ticket = CreateTicketLocked(state, item.owner, ticketKind);
    if (item.ticket == 0) {
      return STOBE_E_LIMIT;
    }
    *outTicket = item.ticket;
  }
  state.work.push_back(item);
  return STOBE_QUEUED;
}

// Caller holds state.locksLock exclusively.
void PublishLockCountLocked(State &state) {
  InterlockedExchange(&g_actorLockCount,
                      static_cast<LONG>(state.locks.Size()));
  InterlockedExchange(&g_actorBusyCount, static_cast<LONG>(state.busy.Size()));
  InterlockedExchange(&g_agentRegistrationCount,
                      static_cast<LONG>(state.agentIn.Size() +
                                        state.agentOut.Size()));
}

// Owner of the actor's busy flag (busy) or dialogue lock in the given
// generation, or 0. Any thread.
StobeAddonId ActorFlagOwner(bool busy, unsigned int serial,
                            unsigned long generation) {
  if (serial == 0 ||
      InterlockedCompareExchange(busy ? &g_actorBusyCount : &g_actorLockCount,
                                 0, 0) == 0) {
    return 0;
  }
  State &state = Get();
  AcquireSRWLockShared(&state.locksLock);
  const StobeAddonId owner =
      (busy ? state.busy : state.locks).Owner(serial, generation);
  ReleaseSRWLockShared(&state.locksLock);
  return owner;
}

// Closes a pending request. A request from the current load gets one queued
// outcome; one from an earlier load is cancelled locally and never reported.
// Caller holds state.lock.
void ClosePendingLocked(State &state,
                        std::map<StobeU32, PendingRequest>::iterator it,
                        bool succeeded, const std::string &detail) {
  if (it->second.generation == PlaythroughSession::Generation()) {
    Outcome outcome;
    outcome.requestId = it->first;
    outcome.request = it->second;
    outcome.succeeded = succeeded;
    outcome.detail = detail;
    state.outcomes.push_back(outcome);
  }
  state.pending.erase(it);
}

// Removes an addon's registrations; its accepted requests fail with detail.
// Caller holds state.lock.
void RemoveAddonLocked(State &state, StobeAddonId id, const std::string &detail) {
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
      ClosePendingLocked(state, it++, false, detail);
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
  // Its tickets go with it; queued notices for them are skipped.
  for (std::map<StobeU32, Ticket>::iterator it = state.tickets.begin();
       it != state.tickets.end();) {
    if (it->second.owner == id) {
      if (it->second.hasSeq && it->second.state == STOBE_CONTROL_PENDING) {
        --state.seqTickets;
      }
      state.tickets.erase(it++);
    } else {
      ++it;
    }
  }
  AcquireSRWLockExclusive(&state.locksLock);
  state.locks.RemoveOwner(id);
  state.busy.RemoveOwner(id);
  state.agentIn.RemoveOwner(id);
  state.agentOut.RemoveOwner(id);
  PublishLockCountLocked(state);
  ReleaseSRWLockExclusive(&state.locksLock);
  state.refresh.RemoveOwner(id);
  // Its unsent completions start no model turn; failures still close rows.
  state.followups.RemoveCompleted(id);
}

void DisableFaultingAddon(StobeAddonId id, const std::string &where) {
  State &state = Get();
  std::string name;
  {
    Lock lock(state.lock);
    name = AddonNameLocked(state, id);
    RemoveAddonLocked(state, id, "addon disabled after a fault");
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

// True when the ref was issued before the current load. Entry points reject
// these at the call; ResolveActor re-checks at dispatch because a load can
// still happen while the work is queued.
bool IsStaleRef(const StobeActorRef &ref) {
  return ref.generation != PlaythroughSession::Generation();
}

// Resolves a reference on the game thread; rejects earlier load generations.
int ResolveActor(GameWorld *world, const StobeActorRef &ref,
                 Character *&characterOut) {
  characterOut = NULL;
  if (ref.serial == 0) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  if (IsStaleRef(ref)) {
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
  case STOBE_E_LOCKED:
    return "actor_locked";
  case STOBE_E_INELIGIBLE:
    return "not_eligible";
  case STOBE_E_ACTOR_BUSY:
    return "actor_busy";
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
//
// An action that carried a server aid appends the follow-up query and is
// queued for the native stream path (DrainFollowupReports). arid is the bridge
// request id; a failure before any bridge accepted gets a fresh id from the
// same sequence that no handler ever received, so the server can close its row
// without that id naming a handler request. Only accepted requests complete.
void ReportExternalActionOutcome(GameWorld *world, unsigned int actorSerial,
                                 const std::string &actorName,
                                 const std::string &command,
                                 const std::string &parameter, bool succeeded,
                                 const std::string &detail, StobeU32 aid = 0,
                                 StobeU32 arid = 0, StobeAddonId owner = 0) {
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
  const std::string query =
      "/StobeServer/stream.php?DATA=" +
      BuildStreamQueryData("funcret", data, CurrentGameTs(world));
  bool queued = false;
  if (aid != 0 && actorSerial != 0) {
    State &state = Get();
    Lock lock(state.lock);
    // A completion whose addon has since unregistered or faulted starts no
    // model turn (aid kept only for the log); the plain funcret reports it.
    const bool ownerGone =
        succeeded && state.addons.find(owner) == state.addons.end();
    if (!ownerGone && arid == 0) {
      arid = state.nextRequestId++;
      if (state.nextRequestId == 0) {
        state.nextRequestId = 1;
      }
    }
    Stobe::AddonProtocol::FollowupReport report;
    report.aid = aid;
    report.sid = actorSerial;
    report.arid = arid;
    report.generation = PlaythroughSession::Generation();
    report.interactionEpoch = Stobe::Interaction::Epoch();
    report.completed = succeeded;
    report.owner = succeeded ? owner : 0;
    report.actorName = name;
    const std::string people =
        "[\"" +
        Stobe::AddonProtocol::JsonEscape(name + "|" + ToString(actorSerial)) +
        "\"]";
    report.query = query + Stobe::AddonProtocol::BuildFollowupQuery(
                               aid, actorSerial, arid, people, g_ttsEnabled);
    queued = !ownerGone && state.followups.Add(report);
  }
  if (!queued) {
    // Legacy lines and servers. A full queue also lands here, so observers
    // still see the result and the server row expires unclaimed.
    if (aid != 0) {
      Log("ADDON_FOLLOWUP: aid=" + ToString(aid) +
          " reported without follow-up (queue full, actor unresolved or "
          "addon removed)");
    }
    AsyncPostToStobeSerial(ToWide(query), "");
  }
  const std::string message = "external action " + command + " " + result;
  Log("ADDON_ACTION: actor_serial=" + ToString(actorSerial) + " " + message);
  LogGameEvent("infoaction", name, SafeFactionName(actor), "None", "None",
               message, actorSerial, 0);
}

int SubmitPlayerLine(const WorkItem &item, Character *speaker, Character *target,
                     const std::string &selectedMode,
                     const std::string &requestMode);

// Returns STOBE_OK when Stobe started the chat request.
int RunPlayerInput(GameWorld *world, const WorkItem &item) {
  Character *target = NULL;
  int code = ResolveActor(world, item.first, target);
  Character *speaker = ResolvePlayerSpeaker(world);
  if (code == STOBE_OK && !Stobe::Interaction::Allowed()) {
    code = STOBE_E_DISABLED;
  } else if (code == STOBE_OK && (Stobe::UI::g_chatWindow || !speaker)) {
    code = STOBE_E_BUSY; // Never overwrite the player's open chat state.
  } else if (code == STOBE_OK) {
    code = DialogueGate(item.first.serial);
  }
  if (code != STOBE_OK) {
    Log("ADDON_API: player input dropped target_serial=" +
        ToString(item.first.serial) + " reason=" + ResultLabel(code));
    return code;
  }
  return SubmitPlayerLine(item, speaker, target, "chat", "");
}

// Says item.text from speaker to target through the chat submission path.
// requestMode empty lets the selected mode and autochat toggle decide it, as
// for V1 player input. Returns STOBE_OK when a chat request started.
int SubmitPlayerLine(const WorkItem &item, Character *speaker, Character *target,
                     const std::string &selectedMode,
                     const std::string &requestMode) {
  int code = STOBE_OK;
  // The voice submission path changes the chat UI's remembered target/mode.
  const std::string savedPlayer = Stobe::UI::g_chatPlayerNameStr;
  const std::string savedTargetName = Stobe::UI::g_chatTargetNameStr;
  const std::string savedTargetHandle = Stobe::UI::g_chatTargetHandleStr;
  const std::string savedMode = g_chatMode;
  const size_t savedModeIndex = Stobe::UI::g_lastChatModeIndex;
  const LONG startsBefore = Stobe::UI::ChatRequestStartCount();
  Stobe::UI::SubmitVoiceChatText(item.text, SafeName(speaker),
                                 ToString(speaker->getHandle().serial),
                                 SafeName(target), ToString(item.first.serial),
                                 selectedMode, requestMode);
  Stobe::UI::g_chatPlayerNameStr = savedPlayer;
  Stobe::UI::g_chatTargetNameStr = savedTargetName;
  Stobe::UI::g_chatTargetHandleStr = savedTargetHandle;
  g_chatMode = savedMode;
  Stobe::UI::g_lastChatModeIndex = savedModeIndex;
  // The chat path may still refuse (range, area); only a started request counts.
  if (Stobe::UI::ChatRequestStartCount() == startsBefore) {
    code = STOBE_E_INELIGIBLE;
  }
  Log("ADDON_API: player input submitted target_serial=" +
      ToString(item.first.serial) + " mode=" +
      (requestMode.empty() ? std::string("selected") : requestMode) +
      " text_len=" +
      ToString(static_cast<int>(item.text.size())) + " result=" +
      (code == STOBE_OK ? std::string("started") : ResultLabel(code)));
  return code;
}

// Returns STOBE_OK when Stobe started the contextual request.
int RunContextRequest(GameWorld *world, const WorkItem &item) {
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
  } else if (code == STOBE_OK) {
    code = DialogueGate(item.first.serial);
    if (code == STOBE_OK) code = DialogueGate(item.second.serial);
  }
  bool dispatched = false;
  if (code == STOBE_OK) {
    dispatched = Stobe::UI::TriggerBoredEvent(
        world, true, SafeName(speaker), ToString(item.first.serial), 0,
        listener ? SafeName(listener) : std::string(),
        listener ? ToString(item.second.serial) : std::string(), item.text,
        true);
    if (!dispatched) {
      code = STOBE_E_INELIGIBLE;
    }
  }
  Log("ADDON_API: contextual request speaker_serial=" +
      ToString(item.first.serial) + " result=" +
      (code != STOBE_OK ? ResultLabel(code) : std::string("dispatched")));
  return code;
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
                                               : "malformed command",
                                item.number);
    return;
  }
  if (actorSerial == 0) {
    // The response did not name an exact actor; addons never get a guess.
    ReportExternalActionOutcome(world, 0, std::string(), parsed.command,
                                item.text, false, "speaker unresolved");
    return;
  }
  bool actorUnavailable = !IsCharacterUsable(actor);
  if (!actorUnavailable) {
    try {
      actorUnavailable = actor->isDead() || actor->isUnconcious();
    } catch (...) {
      actorUnavailable = true;
    }
  }
  if (actorUnavailable) {
    ReportExternalActionOutcome(world, actorSerial, actorName, parsed.command,
                                item.text, false,
                                IsCharacterUsable(actor) ? "actor unavailable"
                                                         : "actor not loaded",
                                item.number);
    return;
  }
  // The dialogue lock does not gate actions; a busy flag refuses every
  // bridge except its owner's, so the owner can coordinate its animation.
  const StobeAddonId busyOwner =
      ActorFlagOwner(true, actorSerial, item.generation);
  State &state = Get();
  BridgeEntry bridge;
  StobeU32 requestId = 0;
  bool bridgeFound = false;
  int gate = STOBE_OK;
  int verdict = STOBE_ACTION_REJECTED;
  bool faulted = false;
  {
    Lock dispatch(state.dispatch);
    {
      Lock lock(state.lock);
      std::map<std::string, BridgeEntry>::const_iterator it =
          state.bridges.find(LowerAscii(parsed.bridge));
      bridgeFound = it != state.bridges.end();
      if (bridgeFound) {
        gate = Stobe::AddonProtocol::ActorGate(
            Stobe::AddonProtocol::ACTOR_ADDON_ACTION, 0, busyOwner,
            it->second.owner);
      }
      if (bridgeFound && gate == STOBE_OK &&
          state.pending.size() + state.outcomes.size() < kMaxPendingRequests) {
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
        request.aid = item.number;
        state.pending[requestId] = request;
      }
    }
    if (requestId != 0) {
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
      try {
        verdict = bridge.handler(bridge.userData, &request);
      } catch (...) {
        faulted = true;
      }
    }
  }
  if (requestId == 0) {
    ReportExternalActionOutcome(world, actorSerial, actorName, parsed.command,
                                item.text, false,
                                !bridgeFound ? "no registered handler for bridge " +
                                                   parsed.bridge
                                : gate != STOBE_OK ? std::string("actor busy")
                                                   : std::string("too many pending requests"),
                                item.number);
    return;
  }
  if (verdict == STOBE_ACTION_ACCEPTED && !faulted) {
    Log("ADDON_ACTION: " + parsed.command + " accepted by bridge " + bridge.name +
        " request=" + ToString(requestId) + " actor_serial=" +
        ToString(actorSerial));
    return;
  }
  // A handler may already have reported, or been unregistered, before it
  // returned; only a request still pending is failed here.
  bool closed = false;
  {
    Lock lock(state.lock);
    closed = state.pending.erase(requestId) != 0;
  }
  if (faulted) {
    DisableFaultingAddon(bridge.owner, "action handler " + bridge.name);
  }
  if (closed) {
    ReportExternalActionOutcome(world, actorSerial, actorName, parsed.command,
                                item.text, false,
                                faulted ? "handler fault" : "rejected by handler",
                                item.number, requestId);
  }
}

// Times out accepted requests and cancels those from an earlier load. Runs at
// most once per second unless the load generation changed.
void ExpirePendingRequests() {
  State &state = Get();
  const DWORD now = GetTickCount();
  const unsigned long generation = PlaythroughSession::Generation();
  int timedOut = 0;
  int cancelled = 0;
  {
    Lock lock(state.lock);
    if (state.expiryGeneration == generation &&
        now - state.lastExpiryTick < kExpiryIntervalMs) {
      return;
    }
    state.expiryGeneration = generation;
    state.lastExpiryTick = now;
    for (std::map<StobeU32, PendingRequest>::iterator it = state.pending.begin();
         it != state.pending.end();) {
      if (it->second.generation != generation) {
        ++cancelled;
        state.pending.erase(it++);
      } else if (now - it->second.createdTick > kPendingRequestLifetimeMs) {
        ++timedOut;
        ClosePendingLocked(state, it++, false, "timed out");
      } else {
        ++it;
      }
    }
    for (std::deque<Outcome>::iterator it = state.outcomes.begin();
         it != state.outcomes.end();) {
      if (it->request.generation != generation) {
        ++cancelled;
        it = state.outcomes.erase(it);
      } else {
        ++it;
      }
    }
  }
  if (timedOut > 0 || cancelled > 0) {
    Log("ADDON_ACTION: requests timed_out=" + ToString(timedOut) +
        " cancelled_by_load=" + ToString(cancelled));
  }
}

void ReportOutcome(GameWorld *world, const Outcome &outcome) {
  ReportExternalActionOutcome(world, outcome.request.actorSerial,
                              outcome.request.actorName,
                              outcome.request.command, outcome.request.parameter,
                              outcome.succeeded, outcome.detail,
                              outcome.request.aid, outcome.requestId,
                              outcome.request.owner);
}

// Sends at most one queued aid outcome per tick while no stream request or
// Director scene runs. A completed outcome also waits for idle dialogue
// (interaction on, chat closed, no queued speech or playback), so its reply
// never interrupts the player, another stream or speech. A failure only
// closes the server row; its stream applies no line.
void DrainFollowupReports() {
  State &state = Get();
  {
    Lock lock(state.lock);
    state.followups.Prune(PlaythroughSession::Generation(),
                          Stobe::Interaction::Epoch());
    if (state.followups.Size() == 0) {
      return;
    }
  }
  if (Stobe::UI::IsAiRequestActive() || Stobe::UI::IsDirectorSceneActive()) {
    return;
  }
  bool dialogueIdle = Stobe::Interaction::Allowed() &&
                      !Stobe::UI::g_chatWindow && !IsTtsPlaybackActive();
  if (dialogueIdle) {
    if (TryEnterCriticalSection(&g_uiMutex)) {
      for (std::deque<QueuedAction>::const_iterator it =
               g_uiActionQueue.begin();
           it != g_uiActionQueue.end() && dialogueIdle; ++it) {
        dialogueIdle = it->type != ACT_SAY && it->type != ACT_PLAY_TTS;
      }
      LeaveCriticalSection(&g_uiMutex);
    } else {
      dialogueIdle = false;
    }
  }
  Stobe::AddonProtocol::FollowupReport report;
  {
    Lock lock(state.lock);
    if (!state.followups.Take(dialogueIdle, report)) {
      return;
    }
  }
  // A locked or busy actor cannot speak: the stream applies no line.
  const bool speak =
      report.completed && DialogueGate(report.sid) == STOBE_OK;
  const bool started = Stobe::UI::StartAddonFollowupStream(
      ToWide(report.query), speak ? report.actorName : std::string(),
      report.sid, "[]");
  Log("ADDON_FOLLOWUP: aid=" + ToString(report.aid) + " arid=" +
      ToString(report.arid) + " sid=" + ToString(report.sid) +
      (report.completed ? " completed" : " failed") +
      (speak ? "" : " no_speech") + (started ? " sent" : " transport_failed"));
}

// Settles interaction tickets. Scans only while some are pending and the
// Interaction progress changed since the last scan.
void ResolveInteractionTickets() {
  State &state = Get();
  Lock lock(state.lock);
  if (state.seqTickets == 0) {
    return;
  }
  LONG requested = 0;
  LONG settled = 0;
  int settledStatus = 0;
  Stobe::Interaction::Progress(requested, settled, settledStatus);
  if (requested == state.seenRequested && settled == state.seenSettled) {
    return;
  }
  state.seenRequested = requested;
  state.seenSettled = settled;
  for (std::map<StobeU32, Ticket>::iterator it = state.tickets.begin();
       it != state.tickets.end(); ++it) {
    if (it->second.state != STOBE_CONTROL_PENDING || !it->second.hasSeq) {
      continue;
    }
    int reason = 0;
    const StobeU32 result = Stobe::AddonProtocol::ResolveInteractionTicket(
        it->second.seq, requested, settled, settledStatus, reason);
    if (result != STOBE_CONTROL_PENDING) {
      FinishTicketLocked(state, it->first, result, reason);
    }
  }
}

void RunSetInteraction(const WorkItem &item, const std::string &addonName) {
  const LONG seq = Stobe::Interaction::Request(item.flag != 0);
  State &state = Get();
  {
    Lock lock(state.lock);
    std::map<StobeU32, Ticket>::iterator it = state.tickets.find(item.ticket);
    if (it != state.tickets.end() && it->second.state == STOBE_CONTROL_PENDING) {
      it->second.hasSeq = true;
      it->second.seq = seq;
      ++state.seqTickets;
      state.seenRequested = -2; // Force a scan; the request may be settled.
    }
  }
  Log("ADDON_API: addon '" + addonName + "' requested interaction " +
      (item.flag != 0 ? "on" : "off") + " seq=" + ToString(static_cast<int>(seq)));
}

void DeliverNotice(StobeU32 id) {
  State &state = Get();
  Lock dispatch(state.dispatch);
  StobeControlStatus status;
  StobeControlCallback callback = NULL;
  void *userData = NULL;
  StobeAddonId owner = 0;
  {
    Lock lock(state.lock);
    std::map<StobeU32, Ticket>::iterator it = state.tickets.find(id);
    if (it == state.tickets.end()) {
      return; // Owner unregistered.
    }
    it->second.noticeQueued = false;
    std::map<StobeAddonId, AddonEntry>::const_iterator addon =
        state.addons.find(it->second.owner);
    if (addon == state.addons.end() || !addon->second.controlCallback) {
      return;
    }
    owner = it->second.owner;
    callback = addon->second.controlCallback;
    userData = addon->second.controlUserData;
    status.struct_size = sizeof(StobeControlStatus);
    status.ticket = id;
    status.kind = it->second.kind;
    status.state = it->second.state;
    status.reason = it->second.reason;
    status.reserved = 0;
  }
  bool faulted = false;
  try {
    callback(userData, &status);
  } catch (...) {
    faulted = true;
  }
  if (faulted) {
    DisableFaultingAddon(owner, "control callback");
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
  entry.controlCallback = NULL;
  entry.controlUserData = NULL;
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
    RemoveAddonLocked(state, id, "addon unregistered");
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

StobeU32 ActorFlags(Character *character, unsigned int serial) {
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
  if (IsSerialInStobeSpeech(serial)) {
    flags |= STOBE_ACTOR_IN_STOBE_SPEECH;
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
  outState->flags = ActorFlags(character, actor.serial);
  return STOBE_OK;
}

int STOBE_CALL ApiSendPlayerInputTracked(StobeAddonId id,
                                         StobeActorRef target,
                                         const char *text,
                                         StobeU32 *outTicket) {
  WorkItem item;
  if (target.serial == 0 ||
      !CopyBoundedText(text, STOBE_MAX_TEXT_BYTES, false, item.text)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  if (IsStaleRef(target)) {
    return STOBE_E_STALE;
  }
  item.kind = WORK_PLAYER_INPUT;
  item.owner = id;
  item.generation = target.generation;
  item.first = target;
  return Enqueue(item, STOBE_CONTROL_KIND_PLAYER_INPUT, outTicket);
}

int STOBE_CALL ApiSendPlayerInput(StobeAddonId id, StobeActorRef target,
                                  const char *text) {
  return ApiSendPlayerInputTracked(id, target, text, NULL);
}

int STOBE_CALL ApiRequestContextualResponseTracked(StobeAddonId id,
                                                   StobeActorRef speaker,
                                                   StobeActorRef listener,
                                                   const char *direction,
                                                   StobeU32 *outTicket) {
  WorkItem item;
  if (speaker.serial == 0 || listener.serial == speaker.serial ||
      !CopyBoundedText(direction, STOBE_MAX_TEXT_BYTES, true, item.text)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  if (IsStaleRef(speaker) || (listener.serial != 0 && IsStaleRef(listener))) {
    return STOBE_E_STALE;
  }
  item.kind = WORK_CONTEXT_REQUEST;
  item.owner = id;
  item.generation = speaker.generation;
  item.first = speaker;
  item.second = listener;
  return Enqueue(item, STOBE_CONTROL_KIND_CONTEXT_REQUEST, outTicket);
}

int STOBE_CALL ApiRequestContextualResponse(StobeAddonId id,
                                            StobeActorRef speaker,
                                            StobeActorRef listener,
                                            const char *direction) {
  return ApiRequestContextualResponseTracked(id, speaker, listener, direction,
                                             NULL);
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
  if (actor.serial != 0 && IsStaleRef(actor)) {
    return STOBE_E_STALE;
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

// Moves the request straight to its outcome, so a result cannot be lost to a
// full work queue or race the timeout. A second report finds nothing pending.
int STOBE_CALL ApiReportActionResult(StobeAddonId id, StobeU32 requestId,
                                     int succeeded, const char *message) {
  std::string text;
  if (requestId == 0 ||
      !CopyBoundedText(message, STOBE_MAX_TEXT_BYTES, true, text)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  State &state = Get();
  Lock lock(state.lock);
  if (!IsRegisteredLocked(state, id)) {
    return STOBE_E_NOT_REGISTERED;
  }
  std::map<StobeU32, PendingRequest>::iterator it = state.pending.find(requestId);
  if (it == state.pending.end() || it->second.owner != id) {
    return STOBE_E_NOT_FOUND;
  }
  if (it->second.generation != PlaythroughSession::Generation()) {
    return STOBE_E_STALE;
  }
  ClosePendingLocked(state, it, succeeded != 0, text);
  return STOBE_QUEUED;
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

// ---- Version 2 entry points ------------------------------------------------

StobeU32 STOBE_CALL ApiGetInteractionState() {
  return static_cast<StobeU32>(Stobe::Interaction::Status());
}

int STOBE_CALL ApiSetInteractionEnabled(StobeAddonId id, int enabled,
                                        StobeU32 *outTicket) {
  WorkItem item;
  item.kind = WORK_SET_INTERACTION;
  item.owner = id;
  item.generation = PlaythroughSession::Generation();
  item.flag = enabled != 0 ? 1 : 0;
  return Enqueue(item, STOBE_CONTROL_KIND_INTERACTION, outTicket);
}

// Dialogue locks and busy flags share ownership, conflict and cleanup rules.
int SetActorFlag(bool busy, StobeAddonId id, StobeActorRef actor, bool on) {
  if (actor.serial == 0) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  if (IsStaleRef(actor)) {
    return STOBE_E_STALE;
  }
  State &state = Get();
  int code = STOBE_OK;
  bool changed = false;
  std::string name;
  {
    Lock lock(state.lock);
    if (!IsRegisteredLocked(state, id)) {
      return STOBE_E_NOT_REGISTERED;
    }
    name = AddonNameLocked(state, id);
    ActorLockTable &table = busy ? state.busy : state.locks;
    AcquireSRWLockExclusive(&state.locksLock);
    const bool before = table.Owner(actor.serial, actor.generation) != 0;
    code = table.Set(id, actor.serial, actor.generation, on);
    changed = before != (table.Owner(actor.serial, actor.generation) != 0);
    PublishLockCountLocked(state);
    ReleaseSRWLockExclusive(&state.locksLock);
  }
  if (changed) {
    Log("ADDON_API: addon '" + name + "' " +
        (busy ? (on ? "set busy" : "cleared busy") : (on ? "locked" : "unlocked")) +
        " actor_serial=" + ToString(actor.serial));
  }
  return code;
}

int GetActorFlagOwner(bool busy, StobeActorRef actor, StobeAddonId *outOwner) {
  if (actor.serial == 0 || !outOwner) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  *outOwner = 0;
  if (IsStaleRef(actor)) {
    return STOBE_E_STALE;
  }
  *outOwner = ActorFlagOwner(busy, actor.serial, actor.generation);
  return STOBE_OK;
}

int STOBE_CALL ApiSetActorLock(StobeAddonId id, StobeActorRef actor,
                               int locked) {
  return SetActorFlag(false, id, actor, locked != 0);
}

int STOBE_CALL ApiGetActorLockOwner(StobeActorRef actor, StobeAddonId *outOwner) {
  return GetActorFlagOwner(false, actor, outOwner);
}

int STOBE_CALL ApiSetActorBusy(StobeAddonId id, StobeActorRef actor, int busy) {
  return SetActorFlag(true, id, actor, busy != 0);
}

int STOBE_CALL ApiGetActorBusyOwner(StobeActorRef actor, StobeAddonId *outOwner) {
  return GetActorFlagOwner(true, actor, outOwner);
}

int STOBE_CALL ApiSetControlCallback(StobeAddonId id, StobeControlCallback fn,
                                     void *userData) {
  State &state = Get();
  {
    Lock lock(state.lock);
    std::map<StobeAddonId, AddonEntry>::iterator it = state.addons.find(id);
    if (id == 0 || it == state.addons.end()) {
      return STOBE_E_NOT_REGISTERED;
    }
    it->second.controlCallback = fn;
    it->second.controlUserData = userData;
  }
  if (!fn) {
    Lock dispatch(state.dispatch); // Wait for an in-flight callback.
  }
  return STOBE_OK;
}

int STOBE_CALL ApiGetControlStatus(StobeAddonId id, StobeU32 ticket,
                                   StobeControlStatus *outStatus) {
  if (ticket == 0 || !outStatus ||
      outStatus->struct_size < sizeof(StobeControlStatus)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  State &state = Get();
  Lock lock(state.lock);
  if (!IsRegisteredLocked(state, id)) {
    return STOBE_E_NOT_REGISTERED;
  }
  std::map<StobeU32, Ticket>::const_iterator it = state.tickets.find(ticket);
  if (it == state.tickets.end() || it->second.owner != id) {
    return STOBE_E_NOT_FOUND;
  }
  outStatus->struct_size = sizeof(StobeControlStatus);
  outStatus->ticket = ticket;
  outStatus->kind = it->second.kind;
  outStatus->state = it->second.state;
  outStatus->reason = it->second.reason;
  outStatus->reserved = 0;
  return STOBE_OK;
}

// STOBE_AGENT_* override for serial in the given load, with its owner.
StobeU32 AgentMode(unsigned int serial, unsigned long generation,
                   StobeAddonId *outOwner) {
  if (outOwner) {
    *outOwner = 0;
  }
  if (InterlockedCompareExchange(&g_agentRegistrationCount, 0, 0) == 0) {
    return STOBE_AGENT_AUTO;
  }
  State &state = Get();
  AcquireSRWLockShared(&state.locksLock);
  StobeAddonId owner = state.agentIn.Owner(serial, generation);
  StobeU32 mode = owner != 0 ? STOBE_AGENT_REGISTERED : STOBE_AGENT_AUTO;
  if (owner == 0) {
    owner = state.agentOut.Owner(serial, generation);
    mode = owner != 0 ? STOBE_AGENT_UNREGISTERED : STOBE_AGENT_AUTO;
  }
  ReleaseSRWLockShared(&state.locksLock);
  if (outOwner) {
    *outOwner = owner;
  }
  return mode;
}

struct Agent {
  Character *character;
  unsigned int serial;
  float distance;
  StobeU32 mode;
  bool autoAgent;
  std::string name;
};

bool AgentNearer(const Agent &lhs, const Agent &rhs) {
  const bool lhsKnown = lhs.distance >= 0.0f;
  if (lhsKnown != (rhs.distance >= 0.0f)) {
    return lhsKnown;
  }
  if (lhs.distance != rhs.distance) {
    return lhs.distance < rhs.distance;
  }
  return lhs.serial < rhs.serial;
}

// Game thread. Agents among Kenshi's loaded characters, nearest first, using
// the chat target rules. Examines at most kMaxAgentScan characters per call;
// returns false when the scan stopped there with characters left unexamined.
bool CollectAgents(GameWorld *world, std::vector<Agent> &out) {
  out.clear();
  bool complete = true;
  if (!world) {
    return complete;
  }
  const unsigned long generation = PlaythroughSession::Generation();
  try {
    const ogre_unordered_set<Character *>::type &chars =
        world->getCharacterUpdateList();
    size_t scanned = 0;
    ogre_unordered_set<Character *>::type::const_iterator it = chars.begin();
    for (; it != chars.end() && scanned < kMaxAgentScan; ++it, ++scanned) {
      Character *character = *it;
      if (!IsCharacterUsable(character)) {
        continue;
      }
      Agent agent;
      agent.character = character;
      agent.serial = character->getHandle().serial;
      if (agent.serial == 0) {
        continue;
      }
      agent.mode = AgentMode(agent.serial, generation, NULL);
      if (agent.mode == STOBE_AGENT_UNREGISTERED) {
        continue;
      }
      agent.autoAgent =
          Stobe::UI::IsAutoAgentCandidate(world, character, agent.distance);
      if (!agent.autoAgent) {
        if (agent.mode != STOBE_AGENT_REGISTERED || character->isDead() ||
            character->isUnconcious()) {
          continue;
        }
        agent.distance = -1.0f;
      }
      agent.name = SafeName(character);
      out.push_back(agent);
    }
    complete = it == chars.end();
  } catch (...) {
    complete = false;
  }
  std::sort(out.begin(), out.end(), AgentNearer);
  return complete;
}

int CheckAgentQuery(StobeAddonId id) {
  if (!IsGameThreadInTick()) {
    return STOBE_E_WRONG_THREAD;
  }
  State &state = Get();
  Lock lock(state.lock);
  return IsRegisteredLocked(state, id) ? STOBE_OK : STOBE_E_NOT_REGISTERED;
}

StobeActorRef CurrentRef(unsigned int serial) {
  StobeActorRef ref;
  ref.serial = serial;
  ref.generation = static_cast<StobeU32>(PlaythroughSession::Generation());
  return ref;
}

int STOBE_CALL ApiListAgents(StobeAddonId id, StobeAgentInfo *out,
                             StobeU32 capacity, StobeU32 *outCount) {
  if (!out || !outCount || capacity == 0 ||
      out[0].struct_size < sizeof(StobeAgentInfo)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  *outCount = 0;
  const int check = CheckAgentQuery(id);
  if (check != STOBE_OK) {
    return check;
  }
  std::vector<Agent> agents;
  CollectAgents(GetWorldSafe(), agents);
  const size_t limit = std::min<size_t>(
      std::min<size_t>(capacity, STOBE_MAX_AGENTS), agents.size());
  for (size_t i = 0; i < limit; ++i) {
    StobeAgentInfo &info = out[i];
    std::memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(StobeAgentInfo);
    info.actor = CurrentRef(agents[i].serial);
    info.flags = ActorFlags(agents[i].character, agents[i].serial) |
                 (agents[i].autoAgent ? STOBE_ACTOR_AUTO_AGENT : 0u);
    info.distance = agents[i].distance;
    info.registration = agents[i].mode;
    const size_t length =
        std::min<size_t>(agents[i].name.size(), sizeof(info.name) - 1);
    std::memcpy(info.name, agents[i].name.data(), length);
  }
  *outCount = static_cast<StobeU32>(limit);
  return STOBE_OK;
}

int STOBE_CALL ApiFindAgentByName(StobeAddonId id, const char *name,
                                  StobeActorRef *outActor) {
  std::string wanted;
  if (!outActor || !CopyBoundedText(name, STOBE_MAX_NAME_BYTES, false, wanted)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  outActor->serial = outActor->generation = 0;
  const int check = CheckAgentQuery(id);
  if (check != STOBE_OK) {
    return check;
  }
  std::vector<Agent> agents;
  const bool complete = CollectAgents(GetWorldSafe(), agents);
  std::vector<std::pair<std::string, unsigned int> > named;
  for (size_t i = 0; i < agents.size(); ++i) {
    named.push_back(std::make_pair(agents[i].name, agents[i].serial));
  }
  unsigned int serial = 0;
  const int code =
      Stobe::AddonProtocol::MatchAgentName(named, wanted, complete, serial);
  if (code == STOBE_OK) {
    *outActor = CurrentRef(serial);
  }
  return code;
}

int STOBE_CALL ApiFindClosestAgent(StobeAddonId id, StobeActorRef origin,
                                   StobeActorRef *outActor) {
  if (!outActor) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  outActor->serial = outActor->generation = 0;
  const int check = CheckAgentQuery(id);
  if (check != STOBE_OK) {
    return check;
  }
  GameWorld *world = GetWorldSafe();
  Character *anchor = NULL;
  if (origin.serial != 0) {
    const int code = ResolveActor(world, origin, anchor);
    if (code != STOBE_OK) {
      return code;
    }
  }
  std::vector<Agent> agents;
  CollectAgents(world, agents);
  unsigned int best = 0;
  float bestDistance = 0.0f;
  for (size_t i = 0; i < agents.size(); ++i) {
    if (agents[i].serial == origin.serial) {
      continue;
    }
    float distance = agents[i].distance;
    if (anchor) {
      try {
        distance = anchor->getPosition().distance(
            agents[i].character->getPosition());
      } catch (...) {
        continue;
      }
    }
    if (distance < 0.0f) {
      continue;
    }
    if (best == 0 || distance < bestDistance) {
      best = agents[i].serial;
      bestDistance = distance;
    }
  }
  if (best == 0) {
    return STOBE_E_NOT_FOUND;
  }
  *outActor = CurrentRef(best);
  return STOBE_OK;
}

int STOBE_CALL ApiSetAgentRegistration(StobeAddonId id, StobeActorRef actor,
                                       StobeU32 mode) {
  if (actor.serial == 0 || mode > STOBE_AGENT_UNREGISTERED) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  if (IsStaleRef(actor)) {
    return STOBE_E_STALE;
  }
  State &state = Get();
  int code = STOBE_OK;
  std::string name;
  {
    Lock lock(state.lock);
    if (!IsRegisteredLocked(state, id)) {
      return STOBE_E_NOT_REGISTERED;
    }
    name = AddonNameLocked(state, id);
    AcquireSRWLockExclusive(&state.locksLock);
    const unsigned int inOwner =
        state.agentIn.Owner(actor.serial, actor.generation);
    const unsigned int outOwner =
        state.agentOut.Owner(actor.serial, actor.generation);
    if ((inOwner != 0 && inOwner != id) || (outOwner != 0 && outOwner != id)) {
      code = STOBE_E_CONFLICT;
    } else if (mode == STOBE_AGENT_REGISTERED &&
               !state.refresh.CanAdd(actor.serial)) {
      code = STOBE_E_LIMIT; // The profile upload is reserved before commit.
    } else {
      code = Stobe::AddonProtocol::SetAgentMode(state.agentIn, state.agentOut,
                                                id, actor.serial,
                                                actor.generation, mode);
    }
    PublishLockCountLocked(state);
    ReleaseSRWLockExclusive(&state.locksLock);
    if (code == STOBE_OK && mode == STOBE_AGENT_REGISTERED) {
      state.refresh.Add(id, actor.serial, actor.generation,
                        REFRESH_PROFILE | STOBE_REFRESH_CONTEXT);
    } else if (code == STOBE_OK) {
      state.refresh.Cancel(id, actor.serial, REFRESH_PROFILE);
    }
  }
  if (code == STOBE_OK) {
    Log("ADDON_API: addon '" + name + "' set agent mode=" +
        ToString(static_cast<int>(mode)) +
        " actor_serial=" + ToString(actor.serial));
  }
  return code == STOBE_OK && mode == STOBE_AGENT_REGISTERED ? STOBE_QUEUED
                                                            : code;
}

int STOBE_CALL ApiGetAgentRegistration(StobeActorRef actor, StobeU32 *outMode,
                                       StobeAddonId *outOwner) {
  if (actor.serial == 0 || !outMode || !outOwner) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  *outMode = STOBE_AGENT_AUTO;
  *outOwner = 0;
  if (IsStaleRef(actor)) {
    return STOBE_E_STALE;
  }
  *outMode = AgentMode(actor.serial, actor.generation, outOwner);
  return STOBE_OK;
}

int STOBE_CALL ApiRequestContextRefresh(StobeAddonId id, StobeActorRef actor,
                                        StobeU32 parts) {
  const StobeU32 known = STOBE_REFRESH_CONTEXT | STOBE_REFRESH_INVENTORY;
  if (actor.serial == 0 || parts == 0 || (parts & ~known) != 0) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  if (IsStaleRef(actor)) {
    return STOBE_E_STALE;
  }
  State &state = Get();
  Lock lock(state.lock);
  if (!IsRegisteredLocked(state, id)) {
    return STOBE_E_NOT_REGISTERED;
  }
  return state.refresh.Add(id, actor.serial, actor.generation, parts)
             ? STOBE_QUEUED
             : STOBE_E_LIMIT;
}

// ---- Version 4 ----

// True while Stobe dialogue is in flight or the player is in a menu: a
// streamed reply, a director scene, TTS playback or a queued Stobe line, the
// chat box open, or the game paused. A contended queue counts as busy.
bool IsDialogueBusy(GameWorld *world) {
  if (Stobe::UI::IsAiRequestActive() || Stobe::UI::IsDirectorSceneActive() ||
      IsTtsPlaybackActive() || Stobe::UI::g_chatWindow) {
    return true;
  }
  try {
    if (world->isPaused()) {
      return true;
    }
  } catch (...) {
    return true;
  }
  // Streamed lines wait in g_messageQueue before they become UI actions.
  if (!TryEnterCriticalSection(&g_msgMutex)) {
    return true;
  }
  bool queued = false;
  for (std::deque<std::string>::const_iterator it = g_messageQueue.begin();
       it != g_messageQueue.end() && !queued; ++it) {
    queued = it->find("NPC_SAY: ") == 0 || it->find("PLAYER_TTS: ") == 0;
  }
  LeaveCriticalSection(&g_msgMutex);
  if (queued || !TryEnterCriticalSection(&g_uiMutex)) {
    return true;
  }
  for (std::deque<QueuedAction>::const_iterator it = g_uiActionQueue.begin();
       it != g_uiActionQueue.end() && !queued; ++it) {
    queued = it->type == ACT_SAY || it->type == ACT_PLAY_TTS;
  }
  LeaveCriticalSection(&g_uiMutex);
  return queued;
}

// Shared hard gates, checked before anything changes.
int AddonSpeechGate(GameWorld *world) {
  if (!Stobe::Interaction::Allowed()) {
    return STOBE_E_DISABLED;
  }
  return IsDialogueBusy(world) ? STOBE_E_BUSY : STOBE_OK;
}

bool IsAliveAndConscious(Character *character, unsigned int serial) {
  const StobeU32 need = STOBE_ACTOR_ALIVE | STOBE_ACTOR_CONSCIOUS;
  return (ActorFlags(character, serial) & need) == need;
}

int RunAddonMessage(GameWorld *world, const WorkItem &item) {
  std::string selectedMode;
  std::string requestMode;
  Stobe::AddonProtocol::AddonMessageModes(item.number, selectedMode,
                                          requestMode);
  Character *target = NULL;
  Character *speaker = ResolvePlayerSpeaker(world);
  int code = ResolveActor(world, item.first, target);
  if (code == STOBE_OK) {
    code = AddonSpeechGate(world);
  }
  if (code == STOBE_OK && !speaker) {
    code = STOBE_E_BUSY;
  }
  if (code == STOBE_OK) {
    code = DialogueGate(item.first.serial);
  }
  if (code == STOBE_OK && !IsAliveAndConscious(target, item.first.serial)) {
    code = STOBE_E_INELIGIBLE;
  }
  float distance = -1.0f;
  // The chat path skips range and area checks for injection; apply chat's.
  if (code == STOBE_OK && item.number == STOBE_MESSAGE_CONTEXT &&
      !Stobe::UI::IsAutoAgentCandidate(world, target, distance)) {
    code = STOBE_E_INELIGIBLE;
  }
  if (code != STOBE_OK) {
    Log("ADDON_API: addon message dropped target_serial=" +
        ToString(item.first.serial) + " mode=" + requestMode +
        " reason=" + ResultLabel(code));
    return code;
  }
  return SubmitPlayerLine(item, speaker, target, selectedMode, requestMode);
}

Stobe::AddonProtocol::ReactionActor DescribeReactionActor(GameWorld *world,
                                                          Character *character,
                                                          unsigned int serial) {
  Stobe::AddonProtocol::ReactionActor actor;
  actor.named = character != NULL;
  const StobeU32 mode =
      actor.named ? AgentMode(serial, PlaythroughSession::Generation(), NULL)
                  : STOBE_AGENT_AUTO;
  actor.registered = mode == STOBE_AGENT_REGISTERED;
  actor.excluded = mode == STOBE_AGENT_UNREGISTERED;
  float distance = -1.0f;
  actor.autoAgent = actor.named &&
                    Stobe::UI::IsAutoAgentCandidate(world, character, distance);
  return actor;
}

int RunAddonReaction(GameWorld *world, const WorkItem &item) {
  Character *speaker = NULL;
  Character *listener = NULL;
  int code = ResolveActor(world, item.first, speaker);
  if (code == STOBE_OK && item.second.serial != 0) {
    code = ResolveActor(world, item.second, listener);
  }
  if (code == STOBE_OK) {
    code = AddonSpeechGate(world);
  }
  if (code == STOBE_OK) {
    code = DialogueGate(item.first.serial);
    if (code == STOBE_OK) code = DialogueGate(item.second.serial);
  }
  // The exact bored-event path relaxes alive and area checks for its named
  // speaker and searches around it, so check reach from the player here.
  Character *player = ResolvePlayerSpeaker(world);
  if (code == STOBE_OK &&
      (!player || !Stobe::UI::IsInConversationReach(player, speaker) ||
       (listener && !Stobe::UI::IsInConversationReach(speaker, listener)))) {
    code = STOBE_E_INELIGIBLE;
  }
  if (code == STOBE_OK) {
    code = Stobe::AddonProtocol::ReactionEligibility(
        item.number, g_enableBoredEvents,
        DescribeReactionActor(world, speaker, item.first.serial),
        DescribeReactionActor(world, listener, item.second.serial));
  }
  if (code == STOBE_OK &&
      !Stobe::UI::TriggerBoredEvent(
          world, true, SafeName(speaker), ToString(item.first.serial), 0,
          listener ? SafeName(listener) : std::string(),
          listener ? ToString(item.second.serial) : std::string(), item.text,
          true, true)) {
    code = STOBE_E_INELIGIBLE;
  }
  Log("ADDON_API: addon reaction speaker_serial=" +
      ToString(item.first.serial) + " listener_serial=" +
      ToString(item.second.serial) + " eligibility=" +
      (item.number == STOBE_REACTION_EXPLICIT ? "explicit" : "eligible") +
      " result=" +
      (code != STOBE_OK ? ResultLabel(code) : std::string("dispatched")));
  return code;
}

int STOBE_CALL ApiSendAddonMessage(StobeAddonId id, StobeActorRef actor,
                                   StobeU32 mode, const char *text,
                                   StobeU32 *outTicket) {
  WorkItem item;
  std::string selectedMode;
  std::string requestMode;
  if (actor.serial == 0 ||
      !Stobe::AddonProtocol::AddonMessageModes(mode, selectedMode,
                                               requestMode) ||
      !CopyBoundedText(text, STOBE_MAX_TEXT_BYTES, false, item.text)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  if (IsStaleRef(actor)) {
    return STOBE_E_STALE;
  }
  item.kind = WORK_ADDON_MESSAGE;
  item.owner = id;
  item.generation = actor.generation;
  item.first = actor;
  item.number = mode;
  return Enqueue(item, STOBE_CONTROL_KIND_PLAYER_INPUT, outTicket);
}

int STOBE_CALL ApiRequestAddonReaction(StobeAddonId id, StobeActorRef speaker,
                                       StobeActorRef listener,
                                       StobeU32 eligibility,
                                       const char *direction,
                                       StobeU32 *outTicket) {
  WorkItem item;
  if (speaker.serial == 0 || listener.serial == speaker.serial ||
      (eligibility != STOBE_REACTION_EXPLICIT &&
       eligibility != STOBE_REACTION_ELIGIBLE) ||
      !CopyBoundedText(direction, STOBE_MAX_TEXT_BYTES, true, item.text)) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  if (IsStaleRef(speaker) || (listener.serial != 0 && IsStaleRef(listener))) {
    return STOBE_E_STALE;
  }
  item.kind = WORK_ADDON_REACTION;
  item.owner = id;
  item.generation = speaker.generation;
  item.first = speaker;
  item.second = listener;
  item.number = eligibility;
  return Enqueue(item, STOBE_CONTROL_KIND_CONTEXT_REQUEST, outTicket);
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

  StobeAddonApiV2 &v2 = Get().apiV2;
  v2.v1 = api;
  v2.v1.struct_size = sizeof(StobeAddonApiV2);
  v2.v1.api_version = STOBE_ADDON_API_VERSION_2;
  v2.capabilities = STOBE_CAP_INTERACTION_CONTROL | STOBE_CAP_ACTOR_LOCKS |
                    STOBE_CAP_CONTROL_STATUS | STOBE_CAP_ACTOR_BUSY;
  v2.reserved = 0;
  v2.GetInteractionState = ApiGetInteractionState;
  v2.SetInteractionEnabled = ApiSetInteractionEnabled;
  v2.SetActorLock = ApiSetActorLock;
  v2.GetActorLockOwner = ApiGetActorLockOwner;
  v2.SendPlayerInputTracked = ApiSendPlayerInputTracked;
  v2.RequestContextualResponseTracked = ApiRequestContextualResponseTracked;
  v2.SetControlCallback = ApiSetControlCallback;
  v2.GetControlStatus = ApiGetControlStatus;
  v2.SetActorBusy = ApiSetActorBusy;
  v2.GetActorBusyOwner = ApiGetActorBusyOwner;

  StobeAddonApiV3 &v3 = Get().apiV3;
  v3.v2 = v2;
  v3.v2.v1.struct_size = sizeof(StobeAddonApiV3);
  v3.v2.v1.api_version = STOBE_ADDON_API_VERSION_3;
  v3.v2.capabilities |= STOBE_CAP_AGENTS | STOBE_CAP_CONTEXT_REFRESH;
  v3.ListAgents = ApiListAgents;
  v3.FindAgentByName = ApiFindAgentByName;
  v3.FindClosestAgent = ApiFindClosestAgent;
  v3.SetAgentRegistration = ApiSetAgentRegistration;
  v3.GetAgentRegistration = ApiGetAgentRegistration;
  v3.RequestContextRefresh = ApiRequestContextRefresh;

  StobeAddonApiV4 &v4 = Get().apiV4;
  v4.v3 = v3;
  v4.v3.v2.v1.struct_size = sizeof(StobeAddonApiV4);
  v4.v3.v2.v1.api_version = STOBE_ADDON_API_VERSION_4;
  v4.v3.v2.capabilities |= STOBE_CAP_ADDON_MESSAGE | STOBE_CAP_ADDON_REACTION;
  v4.SendAddonMessage = ApiSendAddonMessage;
  v4.RequestAddonReaction = ApiRequestAddonReaction;
  return TRUE;
}

// The table layout is the ABI; appended fields must not move these.
static_assert(sizeof(StobeActorRef) == 8, "StobeActorRef ABI");
static_assert(sizeof(StobeAddonInfo) == 24, "StobeAddonInfo ABI");
static_assert(sizeof(StobeActorState) == 8, "StobeActorState ABI");
static_assert(sizeof(StobeActionRequest) == 56, "StobeActionRequest ABI");
static_assert(sizeof(StobeAddonApiV1) == 16 + 13 * sizeof(void *),
              "StobeAddonApiV1 ABI");
static_assert(sizeof(StobeControlStatus) == 24, "StobeControlStatus ABI");
static_assert(offsetof(StobeAddonApiV2, capabilities) == sizeof(StobeAddonApiV1),
              "StobeAddonApiV2 embeds V1 first");
static_assert(sizeof(StobeAddonApiV2) ==
                  sizeof(StobeAddonApiV1) + 8 + 10 * sizeof(void *),
              "StobeAddonApiV2 ABI");
static_assert(sizeof(StobeAgentInfo) == 72, "StobeAgentInfo ABI");
static_assert(offsetof(StobeAddonApiV3, ListAgents) == sizeof(StobeAddonApiV2),
              "StobeAddonApiV3 embeds V2 first");
static_assert(sizeof(StobeAddonApiV3) ==
                  sizeof(StobeAddonApiV2) + 6 * sizeof(void *),
              "StobeAddonApiV3 ABI");
static_assert(offsetof(StobeAddonApiV4, SendAddonMessage) ==
                  sizeof(StobeAddonApiV3),
              "StobeAddonApiV4 embeds V3 first");
static_assert(sizeof(StobeAddonApiV4) ==
                  sizeof(StobeAddonApiV3) + 2 * sizeof(void *),
              "StobeAddonApiV4 ABI");

} // namespace

void GameThreadTick(GameWorld *world) {
  if (!world) {
    return;
  }
  InterlockedExchange(&g_gameThreadId, static_cast<LONG>(GetCurrentThreadId()));
  InterlockedExchange(&g_lastTickTime, static_cast<LONG>(GetTickCount()));
  State &state = Get();
  const unsigned long generation = PlaythroughSession::Generation();
  if (state.lockGeneration != generation) {
    // A load clears every actor lock and busy flag.
    size_t cleared = 0;
    {
      Lock lock(state.lock);
      AcquireSRWLockExclusive(&state.locksLock);
      cleared = state.locks.RemoveOtherGenerations(generation) +
                state.busy.RemoveOtherGenerations(generation) +
                state.agentIn.RemoveOtherGenerations(generation) +
                state.agentOut.RemoveOtherGenerations(generation);
      PublishLockCountLocked(state);
      ReleaseSRWLockExclusive(&state.locksLock);
      state.refresh.RemoveOtherGenerations(generation);
    }
    state.lockGeneration = generation;
    if (cleared > 0) {
      Log("ADDON_API: load cleared " + ToString(static_cast<int>(cleared)) +
          " actor lock(s)/busy flag(s)/agent registration(s)");
    }
  }
  DrainFollowupReports();
  {
    Lock lock(state.lock);
    if (state.work.empty() && state.pending.empty() &&
        state.outcomes.empty() && state.notices.empty() &&
        state.seqTickets == 0) {
      return;
    }
  }
  InterlockedExchange(&g_inGameTick, 1);
  ExpirePendingRequests();
  ResolveInteractionTickets();
  // Outcomes, notices and work share the per-frame budget in that order.
  for (size_t processed = 0; processed < kWorkPerTick; ++processed) {
    WorkItem item;
    Outcome outcome;
    bool haveOutcome = false;
    StobeU32 notice = 0;
    std::string addonName;
    {
      Lock lock(state.lock);
      if (!state.outcomes.empty()) {
        outcome = state.outcomes.front();
        state.outcomes.pop_front();
        haveOutcome = true;
      } else if (!state.notices.empty()) {
        notice = state.notices.front();
        state.notices.pop_front();
      } else if (state.work.empty()) {
        break;
      } else {
        item = state.work.front();
        state.work.pop_front();
        if (item.owner != 0 && !IsRegisteredLocked(state, item.owner)) {
          continue;
        }
        addonName = AddonNameLocked(state, item.owner);
      }
    }
    if (haveOutcome) {
      // Never send a result into a later load.
      if (outcome.request.generation == generation) {
        ReportOutcome(world, outcome);
      }
      continue;
    }
    if (notice != 0) {
      DeliverNotice(notice);
      continue;
    }
    if (item.generation != generation) {
      FinishTicket(item.ticket, STOBE_CONTROL_CANCELLED, STOBE_E_STALE);
      Log("ADDON_API: dropped queued work from an earlier load kind=" +
          ToString(static_cast<int>(item.kind)));
      continue;
    }
    switch (item.kind) {
    case WORK_PLAYER_INPUT:
    case WORK_CONTEXT_REQUEST:
    case WORK_ADDON_MESSAGE:
    case WORK_ADDON_REACTION: {
      const int code =
          item.kind == WORK_PLAYER_INPUT     ? RunPlayerInput(world, item)
          : item.kind == WORK_CONTEXT_REQUEST ? RunContextRequest(world, item)
          : item.kind == WORK_ADDON_MESSAGE   ? RunAddonMessage(world, item)
                                              : RunAddonReaction(world, item);
      FinishTicket(item.ticket,
                   code == STOBE_OK ? STOBE_CONTROL_ACCEPTED
                                    : STOBE_CONTROL_REJECTED,
                   code);
      break;
    }
    case WORK_SET_INTERACTION:
      RunSetInteraction(item, addonName);
      break;
    case WORK_EVENT:
      RunEvent(world, item, addonName);
      break;
    case WORK_CANCEL:
      BeginChatInterruptGeneration(true);
      Log("ADDON_API: addon '" + addonName + "' cancelled Stobe dialogue");
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

bool IsActorLocked(unsigned int serial) {
  return ActorFlagOwner(false, serial, PlaythroughSession::Generation()) != 0;
}

bool IsActorBusy(unsigned int serial) {
  return ActorFlagOwner(true, serial, PlaythroughSession::Generation()) != 0;
}

int DialogueGate(unsigned int serial) {
  const unsigned long generation = PlaythroughSession::Generation();
  return Stobe::AddonProtocol::ActorGate(
      Stobe::AddonProtocol::ACTOR_DIALOGUE,
      ActorFlagOwner(false, serial, generation),
      ActorFlagOwner(true, serial, generation), 0);
}

bool IsAgentExcluded(unsigned int serial) {
  return AgentMode(serial, PlaythroughSession::Generation(), NULL) ==
         STOBE_AGENT_UNREGISTERED;
}

size_t TakeRefreshRequests(RefreshRequest *out, size_t max) {
  State &state = Get();
  std::vector<std::pair<unsigned int, unsigned int> > taken;
  {
    Lock lock(state.lock);
    state.refresh.Take(PlaythroughSession::Generation(), max, taken);
  }
  for (size_t i = 0; i < taken.size(); ++i) {
    out[i].serial = taken[i].first;
    out[i].parts = taken[i].second;
  }
  return taken.size();
}

void QueueExternalAction(unsigned int actorSerial, const std::string &rawCommand,
                         const std::string &parameter,
                         unsigned int followupAid) {
  WorkItem item;
  item.kind = WORK_EXT_ACTION;
  item.number = actorSerial != 0 ? followupAid : 0;
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
  if (version != STOBE_ADDON_API_VERSION &&
      version != STOBE_ADDON_API_VERSION_2 &&
      version != STOBE_ADDON_API_VERSION_3 &&
      version != STOBE_ADDON_API_VERSION_4) {
    return NULL;
  }
  static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
  InitOnceExecuteOnce(&once, Stobe::Addon::InitApiTable, NULL, NULL);
  // Version 1 keeps its own unchanged table; version 2 embeds a copy of it.
  if (version == STOBE_ADDON_API_VERSION_4) {
    return &Stobe::Addon::Get().apiV4.v3.v2.v1;
  }
  if (version == STOBE_ADDON_API_VERSION_3) {
    return &Stobe::Addon::Get().apiV3.v2.v1;
  }
  return version == STOBE_ADDON_API_VERSION ? &Stobe::Addon::Get().api
                                            : &Stobe::Addon::Get().apiV2.v1;
}
