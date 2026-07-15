#include "AutonomyProtocol.h"

#include "StobeText.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace Stobe {
namespace Autonomy {
namespace {

std::string Trim(std::string value) {
  const size_t start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

bool ParseBool(const std::string &value) {
  std::string normalized = Trim(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return normalized == "true" || normalized == "1";
}

long long ParseLongLong(const std::string &value, long long fallback) {
  const std::string normalized = Trim(value);
  if (normalized.empty()) {
    return fallback;
  }
  char *end = NULL;
#if defined(_MSC_VER)
  const long long parsed = _strtoi64(normalized.c_str(), &end, 10);
#else
  const long long parsed = strtoll(normalized.c_str(), &end, 10);
#endif
  return end != normalized.c_str() && *end == '\0' ? parsed : fallback;
}

} // namespace

ControlSnapshot::ControlSnapshot()
    : valid(false), enabled(false), desiredState("DISABLED"),
      controlRevision(0), npcId(0), stopMode("normal") {}

RuntimeFacts::RuntimeFacts()
    : serverAvailable(true), requiresExplicitResume(false),
      manualPauseLatched(false), found(false), identityMatches(false),
      playerCharacter(false), dead(false),
      unconscious(false), hasOrdersReceiver(false), canTakeOrders(false),
      hasPlayerOrders(false) {}

StateDecision::StateDecision(const std::string &stateValue,
                             const std::string &reasonValue)
    : state(stateValue), reason(reasonValue) {}

bool ParseControlResponse(const std::string &response, ControlSnapshot &out) {
  ControlSnapshot parsed;
  if (!ParseBool(Text::JsonReadField(response, "ok"))) {
    return false;
  }
  const std::string session = Text::JsonReadField(response, "session");
  if (session.empty() || session[0] != '{') {
    return false;
  }

  parsed.enabled = ParseBool(Text::JsonReadField(session, "enabled"));
  parsed.desiredState = Text::JsonReadField(session, "desired_state");
  parsed.controlRevision =
      ParseLongLong(Text::JsonReadField(session, "control_revision"), -1);
  parsed.npcId = static_cast<int>(
      ParseLongLong(Text::JsonReadField(session, "npc_id"), 0));
  parsed.npcStorageId = Text::JsonReadField(session, "npc_storage_id");
  parsed.npcName = Text::JsonReadField(session, "npc_name");
  parsed.stopMode = Text::JsonReadField(session, "stop_mode");
  if (parsed.controlRevision < 0 || parsed.desiredState.empty()) {
    return false;
  }
  parsed.valid = true;
  out = parsed;
  return true;
}

unsigned int ParseStorageSerial(const std::string &storageId) {
  const std::string prefix = "hand_";
  if (storageId.size() <= prefix.size() ||
      storageId.compare(0, prefix.size(), prefix) != 0) {
    return 0;
  }
  unsigned long long value = 0;
  for (size_t i = prefix.size(); i < storageId.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(storageId[i]);
    if (ch < '0' || ch > '9') {
      return 0;
    }
    value = value * 10ull + static_cast<unsigned long long>(ch - '0');
    if (value > 0xffffffffull) {
      return 0;
    }
  }
  return static_cast<unsigned int>(value);
}

StateDecision EvaluatePhase1State(const ControlSnapshot &control,
                                  const RuntimeFacts &facts) {
  if (!control.valid || !control.enabled || control.desiredState == "DISABLED") {
    return StateDecision("DISABLED", "autonomy_disabled");
  }
  if (control.desiredState == "PAUSED_USER") {
    return StateDecision("PAUSED_USER", "paused_by_user");
  }
  if (!facts.serverAvailable) {
    return StateDecision("PAUSED_UNSAFE", "server_unavailable");
  }
  if (facts.manualPauseLatched) {
    return StateDecision("PAUSED_USER", "manual_player_order_detected");
  }
  if (facts.requiresExplicitResume || control.desiredState == "PAUSED_UNSAFE") {
    return StateDecision("PAUSED_UNSAFE", "explicit_resume_required");
  }
  if (control.desiredState != "ARMING") {
    return StateDecision("ERROR", "unsupported_phase_1_desired_state");
  }
  if (ParseStorageSerial(control.npcStorageId) == 0) {
    return StateDecision("ERROR", "invalid_storage_identity");
  }
  if (!facts.found) {
    return StateDecision("PAUSED_UNSAFE", "selected_npc_not_loaded");
  }
  if (!facts.identityMatches) {
    return StateDecision("ERROR", "npc_identity_mismatch");
  }
  if (!facts.playerCharacter) {
    return StateDecision("ERROR", "selected_npc_not_player_faction");
  }
  if (facts.dead) {
    return StateDecision("PAUSED_UNSAFE", "selected_npc_dead");
  }
  if (facts.unconscious) {
    return StateDecision("PAUSED_UNSAFE", "selected_npc_unconscious");
  }
  if (!facts.hasOrdersReceiver) {
    return StateDecision("PAUSED_UNSAFE", "orders_receiver_unavailable");
  }
  if (!facts.canTakeOrders) {
    return StateDecision("PAUSED_UNSAFE",
                         "selected_npc_cannot_take_orders");
  }
  if (facts.hasPlayerOrders) {
    return StateDecision("PAUSED_USER", "manual_player_order_detected");
  }
  return StateDecision("OBSERVING", "phase_1_observation_active");
}

} // namespace Autonomy
} // namespace Stobe
