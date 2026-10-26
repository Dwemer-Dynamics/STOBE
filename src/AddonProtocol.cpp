#include "AddonProtocol.h"

#include "StobeAddonApi.h"

#include <cstdio>

namespace Stobe {
namespace AddonProtocol {
namespace {

const size_t kMaxNameBytes = 48;
const char kExtPrefix[] = "extcmd";
const size_t kExtPrefixLength = 6;

bool IsAsciiAlnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

} // namespace

std::string LowerAscii(const std::string &value) {
  std::string out = value;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] >= 'A' && out[i] <= 'Z') {
      out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
  }
  return out;
}

bool IsExtCommand(const std::string &rawCommand) {
  return rawCommand.size() > kExtPrefixLength &&
         LowerAscii(rawCommand.substr(0, kExtPrefixLength)) == kExtPrefix;
}

bool ParseExtCommand(const std::string &rawCommand, ExtCommand &out) {
  if (!IsExtCommand(rawCommand)) {
    return false;
  }
  const size_t separator = rawCommand.find('_', kExtPrefixLength);
  if (separator == std::string::npos || separator == kExtPrefixLength ||
      separator + 1 >= rawCommand.size()) {
    return false;
  }
  const std::string bridge =
      rawCommand.substr(kExtPrefixLength, separator - kExtPrefixLength);
  const std::string action = rawCommand.substr(separator + 1);
  if (!IsValidBridgeName(bridge) || !IsValidToken(action)) {
    return false;
  }
  out.command = rawCommand;
  out.bridge = bridge;
  out.action = action;
  return true;
}

bool IsValidBridgeName(const std::string &value) {
  if (value.empty() || value.size() > kMaxNameBytes) {
    return false;
  }
  for (size_t i = 0; i < value.size(); ++i) {
    if (!IsAsciiAlnum(value[i])) {
      return false;
    }
  }
  return true;
}

bool ParseStrictSerial(const std::string &value, unsigned int &out) {
  out = 0;
  if (value.empty()) {
    return false;
  }
  unsigned long long parsed = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10 + static_cast<unsigned long long>(c - '0');
    if (parsed > 0xFFFFFFFFull) {
      return false;
    }
  }
  if (parsed == 0) {
    return false;
  }
  out = static_cast<unsigned int>(parsed);
  return true;
}

unsigned int SelectExtActionSerial(const std::string &sidToken,
                                   unsigned int primarySerial, bool sidListed) {
  if (sidToken.empty()) {
    return primarySerial;
  }
  unsigned int sid = 0;
  if (!ParseStrictSerial(sidToken, sid)) {
    return 0;
  }
  if (primarySerial != 0) {
    return sid == primarySerial ? sid : 0;
  }
  return sidListed ? sid : 0;
}

bool IsValidToken(const std::string &value) {
  if (value.empty() || value.size() > kMaxNameBytes || value[0] == '.' ||
      value[0] == '-') {
    return false;
  }
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (!IsAsciiAlnum(c) && c != '_' && c != '.' && c != '-') {
      return false;
    }
  }
  return true;
}

// Server rule: ^[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}$, no trailing '.' or ' '.
bool IsSafePackageName(const std::string &value) {
  if (value.empty() || value.size() > 64 || !IsAsciiAlnum(value[0])) {
    return false;
  }
  for (size_t i = 1; i < value.size(); ++i) {
    const char c = value[i];
    if (!IsAsciiAlnum(c) && c != ' ' && c != '.' && c != '_' && c != '-') {
      return false;
    }
  }
  const char last = value[value.size() - 1];
  return last != '.' && last != ' ';
}

// Server rule: ^[0-9A-Za-z][0-9A-Za-z._+-]{0,63}$
bool IsSafePackageVersion(const std::string &value) {
  if (value.empty() || value.size() > 64 || !IsAsciiAlnum(value[0])) {
    return false;
  }
  for (size_t i = 1; i < value.size(); ++i) {
    const char c = value[i];
    if (!IsAsciiAlnum(c) && c != '.' && c != '_' && c != '+' && c != '-') {
      return false;
    }
  }
  return true;
}

bool SplitPackageArchiveName(const std::string &fileName, std::string &stemOut) {
  const size_t dot = fileName.rfind('.');
  if (dot == std::string::npos || dot == 0) {
    return false;
  }
  const std::string extension = LowerAscii(fileName.substr(dot));
  if (extension != ".dwpkg" && extension != ".zip") {
    return false;
  }
  stemOut = fileName.substr(0, dot);
  return true;
}

std::string JsonStringValue(const std::string &body, const std::string &key) {
  const std::string needle = "\"" + key + "\"";
  const size_t keyPosition = body.find(needle);
  if (keyPosition == std::string::npos) {
    return "";
  }
  const size_t colon = body.find(':', keyPosition + needle.size());
  if (colon == std::string::npos) {
    return "";
  }
  const size_t quote = body.find_first_not_of(" \t\r\n", colon + 1);
  if (quote == std::string::npos || body[quote] != '"') {
    return "";
  }
  std::string value;
  bool escaped = false;
  for (size_t i = quote + 1; i < body.size(); ++i) {
    const char c = body[i];
    if (escaped) {
      if (c == 'n') {
        value.push_back('\n');
      } else if (c == 'r') {
        value.push_back('\r');
      } else if (c == 't') {
        value.push_back('\t');
      } else {
        value.push_back(c);
      }
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '"') {
      return value;
    } else {
      value.push_back(c);
    }
  }
  return "";
}

bool JsonBooleanValue(const std::string &body, const std::string &key,
                      bool fallback) {
  const std::string needle = "\"" + key + "\"";
  const size_t keyPosition = body.find(needle);
  if (keyPosition == std::string::npos) {
    return fallback;
  }
  const size_t colon = body.find(':', keyPosition + needle.size());
  if (colon == std::string::npos) {
    return fallback;
  }
  const size_t value = body.find_first_not_of(" \t\r\n", colon + 1);
  if (value == std::string::npos) {
    return fallback;
  }
  if (body.compare(value, 4, "true") == 0) {
    return true;
  }
  if (body.compare(value, 5, "false") == 0) {
    return false;
  }
  return fallback;
}

std::string JsonEscape(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20) {
      char buffer[8];
      sprintf_s(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
      out += buffer;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

int ActorLockTable::Set(unsigned int owner, unsigned int serial,
                        unsigned long generation, bool locked) {
  std::map<unsigned int, Entry>::iterator it = entries_.find(serial);
  if (it != entries_.end() && it->second.generation != generation) {
    entries_.erase(it); // A lock never survives a load.
    it = entries_.end();
  }
  if (it != entries_.end() && it->second.owner != owner) {
    return STOBE_E_CONFLICT;
  }
  if (!locked) {
    if (it != entries_.end()) {
      entries_.erase(it);
    }
    return STOBE_OK;
  }
  if (it == entries_.end()) {
    if (entries_.size() >= capacity_) {
      return STOBE_E_LIMIT;
    }
    Entry entry;
    entry.owner = owner;
    entry.generation = generation;
    entries_[serial] = entry;
  }
  return STOBE_OK;
}

unsigned int ActorLockTable::Owner(unsigned int serial,
                                   unsigned long generation) const {
  std::map<unsigned int, Entry>::const_iterator it = entries_.find(serial);
  return it != entries_.end() && it->second.generation == generation
             ? it->second.owner
             : 0;
}

size_t ActorLockTable::RemoveOwner(unsigned int owner) {
  size_t removed = 0;
  for (std::map<unsigned int, Entry>::iterator it = entries_.begin();
       it != entries_.end();) {
    if (it->second.owner == owner) {
      entries_.erase(it++);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

size_t ActorLockTable::RemoveOtherGenerations(unsigned long generation) {
  size_t removed = 0;
  for (std::map<unsigned int, Entry>::iterator it = entries_.begin();
       it != entries_.end();) {
    if (it->second.generation != generation) {
      entries_.erase(it++);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

int ActorGate(ActorWork work, unsigned int lockOwner, unsigned int busyOwner,
              unsigned int addon) {
  if (work == ACTOR_DIALOGUE) {
    return lockOwner != 0   ? STOBE_E_LOCKED
           : busyOwner != 0 ? STOBE_E_ACTOR_BUSY
                            : STOBE_OK;
  }
  if (busyOwner == 0 || (work == ACTOR_ADDON_ACTION && busyOwner == addon)) {
    return STOBE_OK;
  }
  return STOBE_E_ACTOR_BUSY;
}

int SetAgentMode(ActorLockTable &registered, ActorLockTable &unregistered,
                 unsigned int owner, unsigned int serial,
                 unsigned long generation, unsigned int mode) {
  const unsigned int inOwner = registered.Owner(serial, generation);
  const unsigned int outOwner = unregistered.Owner(serial, generation);
  if ((inOwner != 0 && inOwner != owner) ||
      (outOwner != 0 && outOwner != owner)) {
    return STOBE_E_CONFLICT;
  }
  ActorLockTable &keep = mode == STOBE_AGENT_UNREGISTERED ? unregistered
                                                         : registered;
  ActorLockTable &drop = mode == STOBE_AGENT_UNREGISTERED ? registered
                                                         : unregistered;
  if (mode == STOBE_AGENT_AUTO) {
    registered.Set(owner, serial, generation, false);
    unregistered.Set(owner, serial, generation, false);
    return STOBE_OK;
  }
  const int code = keep.Set(owner, serial, generation, true);
  if (code == STOBE_OK) {
    drop.Set(owner, serial, generation, false);
  }
  return code;
}

size_t FirstIncludedIndex(const std::vector<bool> &excluded) {
  for (size_t i = 0; i < excluded.size(); ++i) {
    if (!excluded[i]) {
      return i;
    }
  }
  return excluded.size();
}

bool AddonMessageModes(unsigned int mode, std::string &selectedOut,
                       std::string &requestOut) {
  switch (mode) {
  case STOBE_MESSAGE_NORMAL:
    selectedOut = "chat";
    requestOut = "talk";
    return true;
  case STOBE_MESSAGE_WHISPER:
    selectedOut = requestOut = "whisper";
    return true;
  case STOBE_MESSAGE_SHOUT:
    selectedOut = requestOut = "shout";
    return true;
  case STOBE_MESSAGE_CONTEXT:
    selectedOut = requestOut = "inject";
    return true;
  default:
    return false;
  }
}

int ReactionEligibility(unsigned int eligibility, bool boredEventsEnabled,
                        const ReactionActor &speaker,
                        const ReactionActor &listener) {
  if (eligibility == STOBE_REACTION_EXPLICIT) {
    return STOBE_OK;
  }
  if (eligibility != STOBE_REACTION_ELIGIBLE) {
    return STOBE_E_INVALID_ARGUMENT;
  }
  const ReactionActor *actors[2] = {&speaker, &listener};
  for (int i = 0; i < 2; ++i) {
    const ReactionActor &actor = *actors[i];
    if (actor.named &&
        (actor.excluded || !(actor.autoAgent || actor.registered))) {
      return STOBE_E_INELIGIBLE;
    }
  }
  return boredEventsEnabled ? STOBE_OK : STOBE_E_INELIGIBLE;
}

int MatchAgentName(const std::vector<std::pair<std::string, unsigned int> > &agents,
                   const std::string &name, bool complete,
                   unsigned int &serialOut) {
  serialOut = 0;
  const std::string wanted = LowerAscii(name);
  for (size_t i = 0; i < agents.size(); ++i) {
    if (agents[i].second == 0 || LowerAscii(agents[i].first) != wanted) {
      continue;
    }
    if (serialOut != 0 && serialOut != agents[i].second) {
      serialOut = 0;
      return STOBE_E_AMBIGUOUS;
    }
    serialOut = agents[i].second;
  }
  if (!complete) {
    serialOut = 0;
    return STOBE_E_LIMIT;
  }
  return serialOut != 0 ? STOBE_OK : STOBE_E_NOT_FOUND;
}

bool RefreshTable::CanAdd(unsigned int serial) const {
  return entries_.count(serial) != 0 || entries_.size() < capacity_;
}

bool RefreshTable::Add(unsigned int owner, unsigned int serial,
                       unsigned long generation, unsigned int parts) {
  if (!CanAdd(serial)) {
    return false;
  }
  Entry &entry = entries_[serial];
  if (entry.parts.empty() || entry.generation != generation) {
    entry.parts.clear();
    entry.generation = generation;
  }
  entry.parts[owner] |= parts;
  return true;
}

void RefreshTable::Cancel(unsigned int owner, unsigned int serial,
                          unsigned int parts) {
  std::map<unsigned int, Entry>::iterator it = entries_.find(serial);
  if (it == entries_.end()) {
    return;
  }
  std::map<unsigned int, unsigned int>::iterator share =
      it->second.parts.find(owner);
  if (share != it->second.parts.end() && (share->second &= ~parts) == 0) {
    it->second.parts.erase(share);
  }
  if (it->second.parts.empty()) {
    entries_.erase(it);
  }
}

void RefreshTable::RemoveOwner(unsigned int owner) {
  for (std::map<unsigned int, Entry>::iterator it = entries_.begin();
       it != entries_.end();) {
    it->second.parts.erase(owner);
    if (it->second.parts.empty()) {
      entries_.erase(it++);
    } else {
      ++it;
    }
  }
}

size_t RefreshTable::RemoveOtherGenerations(unsigned long generation) {
  size_t removed = 0;
  for (std::map<unsigned int, Entry>::iterator it = entries_.begin();
       it != entries_.end();) {
    if (it->second.generation != generation) {
      entries_.erase(it++);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

size_t RefreshTable::Take(
    unsigned long generation, size_t max,
    std::vector<std::pair<unsigned int, unsigned int> > &out) {
  out.clear();
  for (std::map<unsigned int, Entry>::iterator it = entries_.begin();
       it != entries_.end() && out.size() < max;) {
    if (it->second.generation == generation) {
      unsigned int parts = 0;
      for (std::map<unsigned int, unsigned int>::const_iterator share =
               it->second.parts.begin();
           share != it->second.parts.end(); ++share) {
        parts |= share->second;
      }
      out.push_back(std::make_pair(it->first, parts));
    }
    entries_.erase(it++);
  }
  return out.size();
}

unsigned int ResolveInteractionTicket(long ticketSeq, long requestedSeq,
                                      long settledSeq, int settledStatus,
                                      int &reason) {
  reason = 0;
  if (settledSeq == ticketSeq) {
    if (settledStatus == 0 || settledStatus == 1) {
      return STOBE_CONTROL_COMPLETED;
    }
    reason = STOBE_E_UNCONFIRMED;
    return STOBE_CONTROL_FAILED;
  }
  if (requestedSeq != ticketSeq) {
    reason = STOBE_E_SUPERSEDED;
    return STOBE_CONTROL_CANCELLED;
  }
  return STOBE_CONTROL_PENDING;
}

std::string SelectSidToken(const std::vector<std::string> &sidTokens) {
  for (size_t i = 1; i < sidTokens.size(); ++i) {
    if (sidTokens[i] != sidTokens[0]) {
      return "";
    }
  }
  return sidTokens.empty() ? std::string() : sidTokens[0];
}

bool SidTokensRejected(const std::vector<std::string> &sidTokens) {
  unsigned int sid = 0;
  return !sidTokens.empty() && !ParseStrictSerial(SelectSidToken(sidTokens), sid);
}

unsigned int SelectFollowupAid(const std::vector<std::string> &aidTokens,
                               const std::string &sidToken,
                               unsigned int boundSerial) {
  unsigned int aid = 0;
  unsigned int sid = 0;
  // The server writes canonical decimals; a leading zero is not its aid.
  if (aidTokens.size() != 1 || aidTokens[0].empty() || aidTokens[0][0] == '0' ||
      !ParseStrictSerial(aidTokens[0], aid) ||
      !ParseStrictSerial(sidToken, sid) || sid != boundSerial) {
    return 0;
  }
  return aid;
}

namespace {
const char kHeaderAid[] = "|aid=";
std::string Decimal(unsigned int value) {
  char buffer[16];
  int n = 0;
  do {
    buffer[n++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  std::string out;
  while (n > 0) {
    out += buffer[--n];
  }
  return out;
}

std::string QueryEncode(const std::string &value) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}
} // namespace

std::string BuildFollowupQuery(unsigned int aid, unsigned int sid,
                               unsigned int arid, const std::string &peopleJson,
                               bool ttsEnabled) {
  if (aid == 0 || sid == 0 || arid == 0 || peopleJson.empty()) {
    return "";
  }
  return "&addon_followup=1&aid=" + Decimal(aid) + "&sid=" + Decimal(sid) +
         "&arid=" + Decimal(arid) + "&people=" + QueryEncode(peopleJson) +
         "&tts_enabled=" + (ttsEnabled ? "1" : "0");
}

std::string AppendHeaderAid(const std::string &header, unsigned int aid) {
  return aid == 0 ? header : header + kHeaderAid + Decimal(aid);
}

bool TakeHeaderAid(std::string &header, unsigned int &aid) {
  aid = 0;
  const size_t pos = header.find(kHeaderAid);
  if (pos == std::string::npos) {
    return true;
  }
  // Only after "<name>|<serial>"; one canonical positive id ends the header.
  const std::string value = header.substr(pos + sizeof(kHeaderAid) - 1);
  const size_t pipe = header.find('|');
  if (pipe == pos || value.empty() || value[0] == '0' ||
      !ParseStrictSerial(value, aid)) {
    aid = 0;
    return false;
  }
  header.erase(pos);
  return true;
}

bool FollowupLineAllowed(const std::string &actor, const std::string &sidToken,
                         const std::string &capturedName,
                         unsigned int capturedSerial) {
  if (actor.empty() || actor != capturedName || capturedSerial == 0) {
    return false;
  }
  unsigned int sid = 0;
  return sidToken.empty() ||
         (ParseStrictSerial(sidToken, sid) && sid == capturedSerial);
}

size_t FollowupQueue::RemoveCompleted(unsigned int owner) {
  size_t dropped = 0;
  for (size_t i = 0; i < entries_.size();) {
    if (owner != 0 && entries_[i].completed && entries_[i].owner == owner) {
      entries_.erase(entries_.begin() + i);
      ++dropped;
    } else {
      ++i;
    }
  }
  return dropped;
}

bool FollowupQueue::Add(const FollowupReport &report) {
  if (report.aid == 0 || entries_.size() >= capacity_) {
    return false;
  }
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].aid == report.aid) {
      return false;
    }
  }
  entries_.push_back(report);
  return true;
}

size_t FollowupQueue::Prune(unsigned long generation, long interactionEpoch) {
  size_t dropped = 0;
  for (size_t i = 0; i < entries_.size();) {
    const FollowupReport &entry = entries_[i];
    if (entry.generation != generation ||
        (entry.completed && entry.interactionEpoch != interactionEpoch)) {
      entries_.erase(entries_.begin() + i);
      ++dropped;
    } else {
      ++i;
    }
  }
  return dropped;
}

bool FollowupQueue::Take(bool pipelineIdle, FollowupReport &out) {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (!entries_[i].completed || pipelineIdle) {
      out = entries_[i];
      entries_.erase(entries_.begin() + i);
      return true;
    }
  }
  return false;
}

} // namespace AddonProtocol
} // namespace Stobe
