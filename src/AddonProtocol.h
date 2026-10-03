#pragma once

#include <map>
#include <string>

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

// State of an interaction-change ticket for request sequence ticketSeq, given
// the newest requested sequence, the newest settled sequence and the status
// recorded when it settled (0 Off, 1 On, 3 failed). Returns a STOBE_CONTROL_*
// state and sets reason to a STOBE_E_* code or 0.
unsigned int ResolveInteractionTicket(long ticketSeq, long requestedSeq,
                                      long settledSeq, int settledStatus,
                                      int &reason);

} // namespace AddonProtocol
} // namespace Stobe
