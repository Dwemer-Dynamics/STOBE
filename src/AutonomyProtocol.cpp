#include "AutonomyProtocol.h"

#include "StobeText.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#if defined(_MSC_VER) && _MSC_VER < 1800
#include <float.h>
#endif

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

bool ParseDouble(const std::string &value, double &out) {
  const std::string normalized = Trim(value);
  if (normalized.empty()) {
    return false;
  }
  char *end = NULL;
  const double parsed = strtod(normalized.c_str(), &end);
  bool finite = true;
#if defined(_MSC_VER) && _MSC_VER < 1800
  finite = _finite(parsed) != 0;
#else
  finite = std::isfinite(parsed);
#endif
  if (end == normalized.c_str() || *end != '\0' || !finite) {
    return false;
  }
  out = parsed;
  return true;
}

bool IsCatalogCommand(const std::string &command) {
  static const char *commands[] = {
      "SUICIDE",      "FOLLOW",
      "STOP_FOLLOW",  "JOIN_PARTY",       "LEAVE",
      "STOP_CARRYING", "PICKUP_NPC",      "GIVE_CATS",
      "TAKE_CATS",    "GIVE_ITEM",
      "DROP_ITEM",    "ROLEPLAY_ACTION",  "FACTION_RELATIONS",
      "SET_BLOCK",    "SET_HOLD",         "SET_PASSIVE",
      "SET_JOBS",     "SET_RANGED",       "SET_TAUNT",
      "SET_SNEAK",    "SET_RESOURCE",     "SET_MEDIC",
      "USE_OBJECT",   "USE_DRUGS",
      "DRINK",        "FORCE_DRINK",      "TALK"};
  for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
    if (command == commands[i]) {
      return true;
    }
  }
  return false;
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

DecisionEnvelope::DecisionEnvelope()
    : valid(false), controlRevision(-1), npcId(0), runtimeSerial(0),
      command(DECISION_COMMAND_NONE), contextGameTs(0), dispatchDeadlineTs(0),
      actionDeadlineTs(0), idleDurationMs(1500), itemAmount(1),
      maxTotalPrice(0), minTotalPrice(0), limbCode(0), locationZoneId(0),
      targetRuntimeSerial(0), resourceRuntimeSerial(0), x(0.0), y(0.0),
      z(0.0), arrivalRadius(8.0), safeRadius(70.0) {}

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

bool ParseDecisionResponse(const std::string &response, DecisionEnvelope &out,
                           bool &hasDecision) {
  hasDecision = false;
  if (!ParseBool(Text::JsonReadField(response, "ok"))) {
    return false;
  }
  const std::string decisionJson = Text::JsonReadField(response, "decision");
  if (decisionJson.empty() || Trim(decisionJson) == "null") {
    out = DecisionEnvelope();
    return true;
  }
  if (decisionJson[0] != '{') {
    return false;
  }

  DecisionEnvelope parsed;
  parsed.decisionId = Trim(Text::JsonReadField(decisionJson, "decision_id"));
  parsed.controlRevision = ParseLongLong(
      Text::JsonReadField(decisionJson, "control_revision"), -1);
  parsed.npcId = static_cast<int>(ParseLongLong(
      Text::JsonReadField(decisionJson, "npc_id"), 0));
  parsed.npcStorageId =
      Trim(Text::JsonReadField(decisionJson, "npc_storage_id"));
  const long long runtimeSerial = ParseLongLong(
      Text::JsonReadField(decisionJson, "runtime_serial"), 0);
  if (runtimeSerial > 0 && runtimeSerial <= 0xffffffffLL) {
    parsed.runtimeSerial = static_cast<unsigned int>(runtimeSerial);
  }
  parsed.commandName = Trim(Text::JsonReadField(decisionJson, "command"));
  if (parsed.commandName == "IDLE") {
    parsed.command = DECISION_COMMAND_IDLE;
  } else if (parsed.commandName == "TRAVEL_LOCATION") {
    parsed.command = DECISION_COMMAND_TRAVEL_LOCATION;
  } else if (parsed.commandName == "MOVE_NEARBY") {
    parsed.command = DECISION_COMMAND_MOVE_NEARBY;
  } else if (parsed.commandName == "FLEE") {
    parsed.command = DECISION_COMMAND_FLEE;
  } else if (parsed.commandName == "FIRST_AID") {
    parsed.command = DECISION_COMMAND_FIRST_AID;
  } else if (parsed.commandName == "REST") {
    parsed.command = DECISION_COMMAND_REST;
  } else if (parsed.commandName == "ATTACK") {
    parsed.command = DECISION_COMMAND_ATTACK;
  } else if (parsed.commandName == "TAKE_ITEM") {
    parsed.command = DECISION_COMMAND_TAKE_ITEM;
  } else if (parsed.commandName == "EQUIP_ITEM") {
    parsed.command = DECISION_COMMAND_EQUIP_ITEM;
  } else if (parsed.commandName == "KNOCKOUT") {
    parsed.command = DECISION_COMMAND_KNOCKOUT;
  } else if (parsed.commandName == "KILL") {
    parsed.command = DECISION_COMMAND_KILL;
  } else if (parsed.commandName == "REMOVE_LIMB") {
    parsed.command = DECISION_COMMAND_REMOVE_LIMB;
  } else if (parsed.commandName == "CUT_HORNS") {
    parsed.command = DECISION_COMMAND_CUT_HORNS;
  } else if (parsed.commandName == "BUY_ITEM") {
    parsed.command = DECISION_COMMAND_BUY_ITEM;
  } else if (parsed.commandName == "SELL_ITEM") {
    parsed.command = DECISION_COMMAND_SELL_ITEM;
  } else if (parsed.commandName == "WORK_RESOURCE") {
    parsed.command = DECISION_COMMAND_WORK_RESOURCE;
  } else if (parsed.commandName == "PROSPECT") {
    parsed.command = DECISION_COMMAND_PROSPECT;
  } else if (IsCatalogCommand(parsed.commandName)) {
    parsed.command = DECISION_COMMAND_CATALOG_ACTION;
  }
  parsed.contextHash = Trim(Text::JsonReadField(decisionJson, "context_hash"));
  parsed.contextGameTs = ParseLongLong(
      Text::JsonReadField(decisionJson, "context_game_ts"), 0);
  parsed.dispatchDeadlineTs = ParseLongLong(
      Text::JsonReadField(decisionJson, "dispatch_deadline_ts"), 0);
  parsed.actionDeadlineTs = ParseLongLong(
      Text::JsonReadField(decisionJson, "action_deadline_ts"), 0);

  const std::string arguments = Text::JsonReadField(decisionJson, "arguments");
  if (arguments.empty() || arguments[0] != '{') {
    return false;
  }
  parsed.idleDurationMs = static_cast<int>(ParseLongLong(
      Text::JsonReadField(arguments, "duration_ms"), 1500));
  parsed.actionArgument = Trim(
      Text::JsonReadField(arguments, "legacy_argument"));
  parsed.targetName = Trim(Text::JsonReadField(arguments, "target"));
  parsed.itemName = Trim(Text::JsonReadField(arguments, "item"));
  parsed.limbName = Trim(Text::JsonReadField(arguments, "limb"));
  parsed.itemAmount = static_cast<int>(ParseLongLong(
      Text::JsonReadField(arguments, "amount"), 1));
  parsed.maxTotalPrice = static_cast<int>(ParseLongLong(
      Text::JsonReadField(arguments, "max_total_price"), 0));
  parsed.minTotalPrice = static_cast<int>(ParseLongLong(
      Text::JsonReadField(arguments, "min_total_price"), 0));
  if (parsed.limbName == "LEFT_ARM") {
    parsed.limbCode = 1;
  } else if (parsed.limbName == "RIGHT_ARM") {
    parsed.limbCode = 2;
  } else if (parsed.limbName == "LEFT_LEG") {
    parsed.limbCode = 3;
  } else if (parsed.limbName == "RIGHT_LEG") {
    parsed.limbCode = 4;
  }
  parsed.locationZoneId = ParseLongLong(
      Text::JsonReadField(arguments, "location_zone_id"), 0);
  const long long targetRuntimeSerial = ParseLongLong(
      Text::JsonReadField(arguments, "target_runtime_serial"), 0);
  if (targetRuntimeSerial > 0 && targetRuntimeSerial <= 0xffffffffLL) {
    parsed.targetRuntimeSerial = static_cast<unsigned int>(targetRuntimeSerial);
  }
  const long long resourceRuntimeSerial = ParseLongLong(
      Text::JsonReadField(arguments, "resource_runtime_serial"), 0);
  if (resourceRuntimeSerial > 0 &&
      resourceRuntimeSerial <= 0xffffffffLL) {
    parsed.resourceRuntimeSerial =
        static_cast<unsigned int>(resourceRuntimeSerial);
  }
  parsed.locationLabel = Trim(Text::JsonReadField(arguments, "zone_name"));
  if (parsed.locationLabel.empty()) {
    parsed.locationLabel = Trim(Text::JsonReadField(arguments, "city_name"));
  }
  if (parsed.command == DECISION_COMMAND_TRAVEL_LOCATION ||
      parsed.command == DECISION_COMMAND_MOVE_NEARBY ||
      parsed.command == DECISION_COMMAND_FLEE) {
    if (!ParseDouble(Text::JsonReadField(arguments, "x"), parsed.x) ||
        !ParseDouble(Text::JsonReadField(arguments, "y"), parsed.y) ||
        !ParseDouble(Text::JsonReadField(arguments, "z"), parsed.z) ||
        !ParseDouble(Text::JsonReadField(arguments, "arrival_radius"),
                     parsed.arrivalRadius)) {
      return false;
    }
    if (parsed.command == DECISION_COMMAND_FLEE &&
        !ParseDouble(Text::JsonReadField(arguments, "safe_radius"),
                     parsed.safeRadius)) {
      return false;
    }
  }

  if (parsed.decisionId.empty() || parsed.controlRevision < 0 ||
      parsed.npcId <= 0 || parsed.npcStorageId.empty() ||
      parsed.runtimeSerial == 0 || parsed.command == DECISION_COMMAND_NONE ||
      parsed.contextHash.empty() || parsed.contextHash.size() > 128 ||
      parsed.dispatchDeadlineTs <= 0 || parsed.actionDeadlineTs <= 0 ||
      parsed.actionDeadlineTs < parsed.dispatchDeadlineTs ||
      (parsed.command == DECISION_COMMAND_IDLE &&
       (parsed.idleDurationMs < 250 || parsed.idleDurationMs > 30000)) ||
      (parsed.command == DECISION_COMMAND_TRAVEL_LOCATION &&
       (parsed.locationZoneId <= 0 || parsed.arrivalRadius < 1.0 ||
        parsed.arrivalRadius > 100.0 || std::fabs(parsed.x) > 10000000.0 ||
        std::fabs(parsed.y) > 10000000.0 ||
         std::fabs(parsed.z) > 10000000.0)) ||
      ((parsed.command == DECISION_COMMAND_MOVE_NEARBY ||
        parsed.command == DECISION_COMMAND_FLEE) &&
       (parsed.arrivalRadius < 1.0 || parsed.arrivalRadius > 100.0 ||
        std::fabs(parsed.x) > 10000000.0 ||
        std::fabs(parsed.y) > 10000000.0 ||
        std::fabs(parsed.z) > 10000000.0)) ||
      (parsed.command == DECISION_COMMAND_FLEE &&
       (parsed.safeRadius < 10.0 || parsed.safeRadius > 500.0)) ||
      (parsed.command == DECISION_COMMAND_FIRST_AID &&
       parsed.targetRuntimeSerial == 0) ||
      ((parsed.command == DECISION_COMMAND_ATTACK ||
        parsed.command == DECISION_COMMAND_TAKE_ITEM ||
        parsed.command == DECISION_COMMAND_KNOCKOUT ||
        parsed.command == DECISION_COMMAND_KILL ||
        parsed.command == DECISION_COMMAND_REMOVE_LIMB ||
        parsed.command == DECISION_COMMAND_CUT_HORNS) &&
       (parsed.targetRuntimeSerial == 0 || parsed.targetName.empty() ||
        parsed.targetName.size() > 160)) ||
      ((parsed.command == DECISION_COMMAND_TAKE_ITEM ||
        parsed.command == DECISION_COMMAND_EQUIP_ITEM ||
        parsed.command == DECISION_COMMAND_BUY_ITEM ||
        parsed.command == DECISION_COMMAND_SELL_ITEM) &&
       (parsed.itemName.empty() || parsed.itemName.size() > 160 ||
        parsed.itemAmount < 1 || parsed.itemAmount > 20)) ||
      ((parsed.command == DECISION_COMMAND_BUY_ITEM ||
        parsed.command == DECISION_COMMAND_SELL_ITEM) &&
       (parsed.targetRuntimeSerial == 0 || parsed.targetName.empty() ||
        parsed.itemAmount != 1 || parsed.maxTotalPrice < 0 ||
        parsed.maxTotalPrice > 10000000 || parsed.minTotalPrice < 0 ||
        parsed.minTotalPrice > 10000000)) ||
      (parsed.command == DECISION_COMMAND_BUY_ITEM &&
       parsed.maxTotalPrice <= 0) ||
      ((parsed.command == DECISION_COMMAND_WORK_RESOURCE ||
        parsed.command == DECISION_COMMAND_PROSPECT) &&
       parsed.resourceRuntimeSerial == 0) ||
      (parsed.command == DECISION_COMMAND_REMOVE_LIMB &&
       parsed.limbCode == 0) ||
      (parsed.command == DECISION_COMMAND_CATALOG_ACTION &&
       parsed.actionArgument.size() > 1200)) {
    return false;
  }
  parsed.valid = true;
  out = parsed;
  hasDecision = true;
  return true;
}

DecisionValidation ValidateDecisionEnvelope(const DecisionEnvelope &decision,
                                             const ControlSnapshot &control,
                                             unsigned int runtimeSerial,
                                             long long nowEpochSeconds) {
  if (!decision.valid || decision.decisionId.empty() ||
      decision.command == DECISION_COMMAND_NONE) {
    return DECISION_INVALID;
  }
  if (decision.controlRevision != control.controlRevision) {
    return DECISION_STALE_REVISION;
  }
  if (decision.npcId != control.npcId ||
      decision.npcStorageId != control.npcStorageId) {
    return DECISION_IDENTITY_MISMATCH;
  }
  if (runtimeSerial == 0 || decision.runtimeSerial != runtimeSerial) {
    return DECISION_RUNTIME_SERIAL_MISMATCH;
  }
  if (nowEpochSeconds <= 0 || nowEpochSeconds > decision.dispatchDeadlineTs) {
    return DECISION_EXPIRED;
  }
  return DECISION_VALID;
}

const char *DecisionValidationName(DecisionValidation validation) {
  switch (validation) {
  case DECISION_VALID:
    return "valid";
  case DECISION_INVALID:
    return "invalid_decision";
  case DECISION_STALE_REVISION:
    return "stale_control_revision";
  case DECISION_IDENTITY_MISMATCH:
    return "npc_identity_mismatch";
  case DECISION_RUNTIME_SERIAL_MISMATCH:
    return "runtime_serial_mismatch";
  case DECISION_EXPIRED:
    return "dispatch_deadline_expired";
  default:
    return "unknown_validation";
  }
}

} // namespace Autonomy
} // namespace Stobe
