#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

// Game-independent parsing and control state shared by the addon runtime and
// server package sync. Kept free of Windows and KenshiLib types for portable
// tests.
namespace Stobe {
namespace AddonProtocol {

struct ExtCommand {
  std::string command; // Full command as received, original case.
  std::string bridge;
  std::string action;
};

// Accepts ExtCmd<Bridge>_<Action> (prefix case-insensitive). The bridge is a
// valid name without '_', the action is non-empty.
bool ParseExtCommand(const std::string &rawCommand, ExtCommand &out);
bool IsExtCommand(const std::string &rawCommand);

// [A-Za-z0-9] only, 1..48 bytes. Used for bridge names.
bool IsValidBridgeName(const std::string &value);
// [A-Za-z0-9_.-], 1..48 bytes, not starting with '.' or '-'.
bool IsValidToken(const std::string &value);
std::string LowerAscii(const std::string &value);

// Whole token must be ASCII decimal digits forming a nonzero value that fits
// in 32 bits. No sign, whitespace, suffix, or overflow wrap is accepted.
bool ParseStrictSerial(const std::string &value, unsigned int &out);

// Speaker serial for a streamed ExtCmd action, or 0 when it is not exact.
// sidToken is the server's "sid=" value ("" from older servers). primarySerial
// is the request's own NPC serial when the line's actor is that NPC, else 0.
// sidListed says "<actor>|<sid>" is one of this request's people identities.
// A sid that conflicts with the primary serial or was not listed yields 0.
unsigned int SelectExtActionSerial(const std::string &sidToken,
                                   unsigned int primarySerial, bool sidListed);

// Addon follow-up contract stobe.addon_followup.v1 (StobeServer
// docs/plugin-runtime.md). Every "sid=" token of a line, in order; returns the
// common value, or "" when absent or when two tokens disagree (fail closed).
std::string SelectSidToken(const std::vector<std::string> &sidTokens);
// True when sid tokens are present but do not name one strict serial; such an
// action line is dropped. Absent tokens (legacy servers) are never rejected.
bool SidTokensRejected(const std::vector<std::string> &sidTokens);
// The line's aid, or 0. Requires exactly one "aid=" token that is a strict
// uint32, a strict sid, and an action bound to exactly that serial
// (boundSerial from SelectExtActionSerial). Duplicates fail closed; a line
// with any aid token that yields 0 must be dropped, not run as legacy.
unsigned int SelectFollowupAid(const std::vector<std::string> &aidTokens,
                               const std::string &sidToken,
                               unsigned int boundSerial);
// The aid travels on the queued NPC_ACTION speaker header "<name>|<serial>"
// as "<name>|<serial>|aid=<n>", never in the action parameter.
// TakeHeaderAid removes that suffix from a header and returns true with aid 0
// when there is none; false (header unchanged) for a malformed aid suffix.
std::string AppendHeaderAid(const std::string &header, unsigned int aid);
bool TakeHeaderAid(std::string &header, unsigned int &aid);
// Query suffix appended to the unchanged funcret request for an aid outcome.
// Empty when any id is 0 or peopleJson is empty.
std::string BuildFollowupQuery(unsigned int aid, unsigned int sid,
                               unsigned int arid, const std::string &peopleJson,
                               bool ttsEnabled);
// True when a follow-up stream line may be applied: the actor is exactly the
// captured name and any sid token equals the captured serial.
bool FollowupLineAllowed(const std::string &actor, const std::string &sidToken,
                         const std::string &capturedName,
                         unsigned int capturedSerial);

// Bounded outcome reports waiting for the follow-up transport. One entry per
// aid (a second outcome for the same aid is dropped). Completed entries wait
// for an idle dialogue pipeline; failures only need the transport.
struct FollowupReport {
  unsigned int aid;
  unsigned int sid;
  unsigned int arid;
  unsigned long generation;  // load generation
  long interactionEpoch;
  bool completed;
  unsigned int owner;        // addon that reported a completion, else 0
  std::string actorName;
  std::string query;         // funcret DATA query, already encoded
};
class FollowupQueue {
public:
  explicit FollowupQueue(size_t capacity) : capacity_(capacity) {}
  // False (nothing stored) when full, aid is 0 or aid is already queued.
  bool Add(const FollowupReport &report);
  // Drops entries of other load generations, and completed entries whose
  // interaction epoch differs (the server would skip them anyway). Returns
  // the number dropped.
  size_t Prune(unsigned long generation, long interactionEpoch);
  // Drops the owner's completed entries (unregister or fault); failures stay
  // so their server rows still close. Returns the number dropped.
  size_t RemoveCompleted(unsigned int owner);
  // Takes the oldest failure, or when pipelineIdle the oldest entry.
  bool Take(bool pipelineIdle, FollowupReport &out);
  size_t Size() const { return entries_.size(); }

private:
  std::vector<FollowupReport> entries_;
  size_t capacity_;
};

// Matches the server package manager's plugin-name and version rules.
bool IsSafePackageName(const std::string &value);
bool IsSafePackageVersion(const std::string &value);
// ".dwpkg" or ".zip" (case-insensitive); returns the version stem.
bool SplitPackageArchiveName(const std::string &fileName, std::string &stemOut);

// Minimal readers for the flat package API responses. They return the first
// occurrence of the key anywhere in the body.
std::string JsonStringValue(const std::string &body, const std::string &key);
bool JsonBooleanValue(const std::string &body, const std::string &key,
                      bool fallback);
std::string JsonEscape(const std::string &value);

// Per-actor addon flags (dialogue locks or animation-busy flags) keyed by
// serial within one load generation. Entries from another generation count as
// absent. Not synchronized; the runtime guards it. Set returns STOBE_OK,
// STOBE_E_CONFLICT (another owner) or STOBE_E_LIMIT (capacity reached).
class ActorLockTable {
public:
  explicit ActorLockTable(size_t capacity) : capacity_(capacity) {}
  int Set(unsigned int owner, unsigned int serial, unsigned long generation,
          bool locked);
  unsigned int Owner(unsigned int serial, unsigned long generation) const;
  size_t RemoveOwner(unsigned int owner);
  size_t RemoveOtherGenerations(unsigned long generation);
  size_t Size() const { return entries_.size(); }

private:
  struct Entry {
    unsigned int owner;
    unsigned long generation;
  };
  std::map<unsigned int, Entry> entries_;
  size_t capacity_;
};

// Work Stobe performs for an actor, as gated by addon locks and busy flags.
enum ActorWork {
  ACTOR_DIALOGUE,       // selection, request starts and spoken lines
  ACTOR_BUILTIN_ACTION, // Stobe's own actions, follow/travel/move, autonomy
  ACTOR_ADDON_ACTION    // ExtCmd dispatched to the bridge owned by addon
};

// STOBE_OK when the work may run, else STOBE_E_LOCKED or STOBE_E_ACTOR_BUSY.
// The dialogue lock gates only dialogue; the busy flag gates dialogue and
// actions, except the busy owner's own ExtCmd actions. Owners are 0 when unset.
int ActorGate(ActorWork work, unsigned int lockOwner, unsigned int busyOwner,
              unsigned int addon);

// Sets owner's agent registration mode (STOBE_AGENT_*) for serial, kept as
// one entry in either registered or unregistered. Another owner's entry in
// either table is STOBE_E_CONFLICT and nothing changes; AUTO clears the
// owner's entry. Returns STOBE_OK, STOBE_E_CONFLICT or STOBE_E_LIMIT.
int SetAgentMode(ActorLockTable &registered, ActorLockTable &unregistered,
                 unsigned int owner, unsigned int serial,
                 unsigned long generation, unsigned int mode);

// Exact ASCII case-insensitive name lookup among (name, serial) agents.
// STOBE_OK with serialOut, STOBE_E_NOT_FOUND, or STOBE_E_AMBIGUOUS when
// different serials share the name. When the list is not complete (the scan
// stopped at its bound) a unique match or no match is STOBE_E_LIMIT, since an
// unscanned namesake may exist. Names never route to a guessed actor.
int MatchAgentName(const std::vector<std::pair<std::string, unsigned int> > &agents,
                   const std::string &name, bool complete,
                   unsigned int &serialOut);

// Index of the first option not excluded: the option the chat target dropdown
// selects by itself (options are nearest first). Returns excluded.size() when
// every option is excluded, so nothing is selected until the player picks.
size_t FirstIncludedIndex(const std::vector<bool> &excluded);

// Pending refresh parts per actor, coalesced per actor but kept per requesting
// owner so one owner's cleanup leaves other owners' requests. At most capacity
// actors; owners are registered addons, so each entry holds few shares.
class RefreshTable {
public:
  explicit RefreshTable(size_t capacity) : capacity_(capacity) {}
  // True when Add for serial would fit.
  bool CanAdd(unsigned int serial) const;
  // Merges parts into owner's share of serial; an entry from another
  // generation is replaced. False and no change when full.
  bool Add(unsigned int owner, unsigned int serial, unsigned long generation,
           unsigned int parts);
  // Clears parts from owner's share of serial, dropping empty shares/entries.
  void Cancel(unsigned int owner, unsigned int serial, unsigned int parts);
  void RemoveOwner(unsigned int owner);
  size_t RemoveOtherGenerations(unsigned long generation);
  // Moves up to max entries of generation into out as (serial, all owners'
  // parts), discarding other generations it passes.
  size_t Take(unsigned long generation, size_t max,
              std::vector<std::pair<unsigned int, unsigned int> > &out);
  size_t Size() const { return entries_.size(); }

private:
  struct Entry {
    unsigned long generation;
    std::map<unsigned int, unsigned int> parts; // owner -> parts
  };
  std::map<unsigned int, Entry> entries_;
  size_t capacity_;
};

// State of an interaction-change ticket for request sequence ticketSeq, given
// the newest requested sequence, the newest settled sequence and the status
// recorded when it settled (0 Off, 1 On, 3 failed). Returns a STOBE_CONTROL_*
// state and sets reason to a STOBE_E_* code or 0.
unsigned int ResolveInteractionTicket(long ticketSeq, long requestedSeq,
                                      long settledSeq, int settledStatus,
                                      int &reason);

} // namespace AddonProtocol
} // namespace Stobe
