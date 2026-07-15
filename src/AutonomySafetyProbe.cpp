#include "AutonomySafetyProbe.h"

#include "AutonomySafetyProbePolicy.h"
#include "Utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/CharMovement.h>
#include <kenshi/Character.h>
#include <kenshi/GameWorld.h>
#include <kenshi/util/hand.h>
#include <ogre/OgreVector3.h>

namespace {

const char *kProbeSection = "AutonomySafetyProbe";
const DWORD kConfigRefreshMs = 500;
const DWORD kMinTelemetryIntervalMs = 500;
const DWORD kMaxTelemetryIntervalMs = 60000;

struct ProbeConfig {
  bool enabled;
  unsigned int targetSerial;
  DWORD telemetryIntervalMs;
  std::string commandText;
  unsigned int commandNonce;
  bool travelDestinationSet;
  Ogre::Vector3 travelDestination;

  ProbeConfig()
      : enabled(false), targetSerial(0), telemetryIntervalMs(2000),
        commandNonce(0), travelDestinationSet(false), travelDestination(0, 0, 0) {}
};

struct ProbeRuntimeState {
  bool configInitialized;
  bool wasEnabled;
  unsigned int configuredTargetSerial;
  unsigned int boundSerial;
  hand boundHandle;
  unsigned int lastCommandNonce;
  DWORD lastConfigTick;
  DWORD lastTelemetryTick;
  DWORD lastWaitingLogTick;
  bool invariantWatchActive;
  bool invariantViolationLogged;
  bool invariantJobsEnabled;
  int invariantPermajobCount;
  std::string invariantPermajobs;
  std::string invariantCommand;
  DWORD invariantStartedTick;
  ProbeConfig config;

  ProbeRuntimeState()
      : configInitialized(false), wasEnabled(false), configuredTargetSerial(0),
        boundSerial(0), lastCommandNonce(0), lastConfigTick(0),
        lastTelemetryTick(0), lastWaitingLogTick(0),
        invariantWatchActive(false), invariantViolationLogged(false),
        invariantJobsEnabled(false), invariantPermajobCount(0),
        invariantStartedTick(0) {}
};

ProbeRuntimeState g_probe;

std::string GetExecutableDir() {
  char path[MAX_PATH];
  DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return ".";
  }
  std::string result(path, length);
  size_t separator = result.find_last_of("\\/");
  return separator == std::string::npos ? "." : result.substr(0, separator);
}

std::string GetBaseIniPath() {
  const std::string executableDir = GetExecutableDir();
  const std::string runtimePath =
      executableDir + "\\mods\\Stobe\\Stobe.ini";
  if (GetFileAttributesA(runtimePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return runtimePath;
  }

  // RE_Kenshi runs from a child directory while mods remain in Kenshi\mods.
  const size_t separator = executableDir.find_last_of("\\/");
  if (separator != std::string::npos) {
    const std::string installedPath = executableDir.substr(0, separator) +
                                      "\\mods\\Stobe\\Stobe.ini";
    if (GetFileAttributesA(installedPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return installedPath;
    }
  }

  return runtimePath;
}

std::string GetCustomIniPath() {
  return GetExecutableDir() + "\\mods\\Stobe\\StobeCustom.ini";
}

std::string ReadLayeredString(const char *key, const char *defaultValue) {
  char baseValue[256];
  char customValue[256];
  const std::string basePath = GetBaseIniPath();
  const std::string customPath = GetCustomIniPath();
  GetPrivateProfileStringA(kProbeSection, key, defaultValue, baseValue,
                           sizeof(baseValue), basePath.c_str());
  GetPrivateProfileStringA(kProbeSection, key, baseValue, customValue,
                           sizeof(customValue), customPath.c_str());
  return std::string(customValue);
}

unsigned int ParseUnsigned(const std::string &value,
                           unsigned int defaultValue) {
  if (value.empty()) {
    return defaultValue;
  }
  char *end = NULL;
  unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0') {
    return defaultValue;
  }
  return static_cast<unsigned int>(parsed);
}

float ParseFloat(const std::string &value, float defaultValue) {
  if (value.empty()) {
    return defaultValue;
  }
  char *end = NULL;
  double parsed = std::strtod(value.c_str(), &end);
  if (end == value.c_str() || *end != '\0' || parsed < -10000000.0 ||
      parsed > 10000000.0) {
    return defaultValue;
  }
  return static_cast<float>(parsed);
}

ProbeConfig ReadConfig() {
  ProbeConfig config;
  config.enabled = ParseUnsigned(ReadLayeredString("Enabled", "0"), 0) != 0;
  config.targetSerial =
      ParseUnsigned(ReadLayeredString("TargetSerial", "0"), 0);
  config.telemetryIntervalMs =
      ParseUnsigned(ReadLayeredString("TelemetryIntervalMs", "2000"), 2000);
  config.telemetryIntervalMs =
      std::max(kMinTelemetryIntervalMs,
               std::min(kMaxTelemetryIntervalMs, config.telemetryIntervalMs));
  config.commandText = ReadLayeredString("Command", "OBSERVE");
  config.commandNonce =
      ParseUnsigned(ReadLayeredString("CommandNonce", "0"), 0);
  config.travelDestinationSet =
      ParseUnsigned(ReadLayeredString("TravelDestinationSet", "0"), 0) != 0;
  config.travelDestination.x =
      ParseFloat(ReadLayeredString("TravelX", "0"), 0.0f);
  config.travelDestination.y =
      ParseFloat(ReadLayeredString("TravelY", "0"), 0.0f);
  config.travelDestination.z =
      ParseFloat(ReadLayeredString("TravelZ", "0"), 0.0f);
  return config;
}

bool IsValidCharacterPointer(Character *character) {
  return character && reinterpret_cast<uintptr_t>(character) > 0x1000;
}

Character *ResolveCharacterBySerial(GameWorld *world, unsigned int serial) {
  if (!world || serial == 0) {
    return NULL;
  }
  try {
    const auto &characters = world->getCharacterUpdateList();
    for (auto it = characters.begin(); it != characters.end(); ++it) {
      Character *candidate = *it;
      if (!IsValidCharacterPointer(candidate)) {
        continue;
      }
      try {
        if (candidate->getHandle().serial == serial) {
          return candidate;
        }
      } catch (...) {
      }
    }
  } catch (...) {
  }
  return NULL;
}

bool TryReadCharacterSerial(Character *character, unsigned int &serialOut,
                            hand &handleOut) {
  serialOut = 0;
  handleOut = hand();
  if (!IsValidCharacterPointer(character)) {
    return false;
  }
  try {
    handleOut = character->getHandle();
    serialOut = handleOut.serial;
  } catch (...) {
    return false;
  }
  return serialOut != 0 && handleOut.isValid();
}

std::string SafeCharacterName(Character *character) {
  if (!IsValidCharacterPointer(character)) {
    return "Unknown";
  }
  try {
    return character->getName();
  } catch (...) {
    return "Unknown";
  }
}

const char *TaskName(TaskType task) {
  switch (task) {
  case NULL_TASK:
    return "NULL_TASK";
  case IDLE:
    return "IDLE";
  case MOVE_CUS_ORDERED:
    return "MOVE_CUS_ORDERED";
  default:
    return "OTHER";
  }
}

struct AiSnapshot {
  bool playerCharacter;
  bool dead;
  bool unconscious;
  bool canTakeOrders;
  bool hasOrdersReceiver;
  bool hasOrders;
  bool hasPlayerOrders;
  bool jobsEnabled;
  int permajobCount;
  int currentGoalKey;
  std::string currentGoal;
  std::string firstOrder;
  std::string permajobs;
  Ogre::Vector3 position;
  bool hasMovement;
  bool movementIdle;
  bool pathOk;
  bool pathFailed;
  bool destinationReached;
  Ogre::Vector3 destination;

  AiSnapshot()
      : playerCharacter(false), dead(false), unconscious(false),
        canTakeOrders(false), hasOrdersReceiver(false), hasOrders(false),
        hasPlayerOrders(false), jobsEnabled(false), permajobCount(0),
        currentGoalKey((int)NULL_TASK), currentGoal("unavailable"),
        firstOrder("none"), permajobs("[]"), position(0, 0, 0),
        hasMovement(false), movementIdle(false), pathOk(false),
        pathFailed(false), destinationReached(false), destination(0, 0, 0) {}
};

AiSnapshot CaptureSnapshot(Character *character) {
  AiSnapshot snapshot;
  if (!IsValidCharacterPointer(character)) {
    return snapshot;
  }

  try {
    snapshot.playerCharacter = character->isPlayerCharacter();
  } catch (...) {
  }
  try {
    snapshot.dead = character->isDead();
  } catch (...) {
  }
  try {
    snapshot.unconscious = character->isUnconcious();
  } catch (...) {
  }
  try {
    snapshot.canTakeOrders = character->canTakePlayerOrdersAtThisTime();
  } catch (...) {
  }
  try {
    snapshot.position = character->getPosition();
  } catch (...) {
  }

  OrdersReceiver *orders = NULL;
  try {
    orders = character->getOrdersReciever();
  } catch (...) {
    orders = NULL;
  }
  if (orders && reinterpret_cast<uintptr_t>(orders) > 0x1000) {
    snapshot.hasOrdersReceiver = true;
    try {
      snapshot.hasOrders = orders->hasOrders();
    } catch (...) {
    }
    try {
      snapshot.hasPlayerOrders = orders->hasPlayerOrders();
    } catch (...) {
    }
    try {
      snapshot.jobsEnabled = orders->isJobsEnabled();
    } catch (...) {
    }
    try {
      snapshot.permajobCount = orders->getPermajobCount();
    } catch (...) {
      snapshot.permajobCount = 0;
    }
    try {
      Tasker *firstOrder = orders->getFirstOrder();
      snapshot.firstOrder =
          firstOrder && reinterpret_cast<uintptr_t>(firstOrder) > 0x1000
              ? "present"
              : "none";
    } catch (...) {
      snapshot.firstOrder = "unreadable";
    }
    try {
      TaskType currentGoalType = orders->getCurrentGoal().key();
      snapshot.currentGoalKey = (int)currentGoalType;
      snapshot.currentGoal = TaskName(currentGoalType);
    } catch (...) {
    }

    std::ostringstream jobs;
    jobs << "[";
    for (int i = 0; i < snapshot.permajobCount; ++i) {
      if (i > 0) {
        jobs << ",";
      }
      try {
        jobs << "\"" << orders->getPermajobName(i) << "\"";
      } catch (...) {
        jobs << "\"unreadable\"";
      }
    }
    jobs << "]";
    snapshot.permajobs = jobs.str();
  }

  try {
    CharMovement *movement = character->getMovement();
    if (movement && reinterpret_cast<uintptr_t>(movement) > 0x1000) {
      snapshot.hasMovement = true;
      snapshot.movementIdle = movement->isIdle();
      snapshot.pathOk = movement->pathOk();
      snapshot.pathFailed = movement->pathFailed();
      snapshot.destinationReached = movement->isDestinationReached();
      snapshot.destination = movement->getDestination();
    }
  } catch (...) {
    snapshot.hasMovement = false;
  }

  return snapshot;
}

std::string BoolText(bool value) { return value ? "1" : "0"; }

std::string VectorText(const Ogre::Vector3 &value) {
  return "(" + ToString(value.x) + "," + ToString(value.y) + "," +
         ToString(value.z) + ")";
}

void LogSnapshot(const char *eventName, Character *character,
                 unsigned int serial, const AiSnapshot &snapshot) {
  Log(std::string("AUTONOMY_SPIKE_STATE: event=") + eventName +
      " serial=" + ToString(serial) + " name=\"" +
      SafeCharacterName(character) + "\" player=" +
      BoolText(snapshot.playerCharacter) + " dead=" + BoolText(snapshot.dead) +
      " unconscious=" + BoolText(snapshot.unconscious) +
      " can_take_orders=" + BoolText(snapshot.canTakeOrders) +
      " has_orders_receiver=" + BoolText(snapshot.hasOrdersReceiver) +
      " has_orders=" + BoolText(snapshot.hasOrders) +
      " has_player_orders=" + BoolText(snapshot.hasPlayerOrders) +
      " first_order=" + snapshot.firstOrder +
      " current_goal_key=" + ToString(snapshot.currentGoalKey) +
      " current_goal=\"" + snapshot.currentGoal + "\" jobs_enabled=" +
      BoolText(snapshot.jobsEnabled) +
      " permajob_count=" + ToString(snapshot.permajobCount) +
      " permajobs=" + snapshot.permajobs + " position=" +
      VectorText(snapshot.position) +
      " movement=" + BoolText(snapshot.hasMovement) +
      " movement_idle=" + BoolText(snapshot.movementIdle) +
      " path_ok=" + BoolText(snapshot.pathOk) +
      " path_failed=" + BoolText(snapshot.pathFailed) +
      " destination_reached=" + BoolText(snapshot.destinationReached) +
      " destination=" + VectorText(snapshot.destination));
}

void ShowProbeMessage(GameWorld *world, const std::string &message) {
  if (!world) {
    return;
  }
  try {
    world->showPlayerAMessage_withLog(message, true);
  } catch (...) {
  }
}

void ClearBinding(const char *reason) {
  if (g_probe.boundSerial != 0) {
    Log(std::string("AUTONOMY_SPIKE: target unbound serial=") +
        ToString(g_probe.boundSerial) + " reason=" + (reason ? reason : "none"));
  }
  g_probe.boundSerial = 0;
  g_probe.boundHandle = hand();
  g_probe.lastTelemetryTick = 0;
  g_probe.invariantWatchActive = false;
  g_probe.invariantViolationLogged = false;
  g_probe.invariantCommand.clear();
  g_probe.invariantStartedTick = 0;
}

void CheckJobsInvariant(Character *character, const AiSnapshot &snapshot) {
  if (!g_probe.invariantWatchActive || !character) {
    return;
  }

  const bool preserved =
      snapshot.jobsEnabled == g_probe.invariantJobsEnabled &&
      snapshot.permajobCount == g_probe.invariantPermajobCount &&
      snapshot.permajobs == g_probe.invariantPermajobs;
  if (!preserved && !g_probe.invariantViolationLogged) {
    g_probe.invariantViolationLogged = true;
    Log("AUTONOMY_SPIKE_INVARIANT: jobs changed while probe order active serial=" +
        ToString(g_probe.boundSerial) + " command=" + g_probe.invariantCommand +
        " expected_jobs_enabled=" + BoolText(g_probe.invariantJobsEnabled) +
        " actual_jobs_enabled=" + BoolText(snapshot.jobsEnabled) +
        " expected_permajobs=" + g_probe.invariantPermajobs +
        " actual_permajobs=" + snapshot.permajobs);
  }

  if (!snapshot.hasOrders &&
      GetTickCount() - g_probe.invariantStartedTick >= 250) {
    Log("AUTONOMY_SPIKE_INVARIANT: order watch complete serial=" +
        ToString(g_probe.boundSerial) + " command=" + g_probe.invariantCommand +
        " jobs_preserved=" + BoolText(preserved) +
        " violation=" + BoolText(g_probe.invariantViolationLogged));
    g_probe.invariantWatchActive = false;
  }
}

struct PlayerOrderState {
  unsigned int serial;
  bool hasOrders;
  bool hasPlayerOrders;

  PlayerOrderState() : serial(0), hasOrders(false), hasPlayerOrders(false) {}
};

std::vector<PlayerOrderState>
CaptureOtherPlayerOrderStates(GameWorld *world, unsigned int excludedSerial) {
  std::vector<PlayerOrderState> states;
  if (!world) {
    return states;
  }
  try {
    const auto &characters = world->getCharacterUpdateList();
    for (auto it = characters.begin(); it != characters.end(); ++it) {
      Character *candidate = *it;
      if (!IsValidCharacterPointer(candidate)) {
        continue;
      }
      PlayerOrderState state;
      hand candidateHandle;
      if (!TryReadCharacterSerial(candidate, state.serial, candidateHandle) ||
          state.serial == excludedSerial) {
        continue;
      }
      bool isPlayer = false;
      try {
        isPlayer = candidate->isPlayerCharacter();
      } catch (...) {
      }
      if (!isPlayer) {
        continue;
      }
      try {
        OrdersReceiver *orders = candidate->getOrdersReciever();
        if (orders && reinterpret_cast<uintptr_t>(orders) > 0x1000) {
          state.hasOrders = orders->hasOrders();
          state.hasPlayerOrders = orders->hasPlayerOrders();
        }
      } catch (...) {
      }
      states.push_back(state);
    }
  } catch (...) {
  }
  return states;
}

bool OtherPlayerOrdersUnchanged(const std::vector<PlayerOrderState> &before,
                                const std::vector<PlayerOrderState> &after) {
  for (size_t i = 0; i < before.size(); ++i) {
    for (size_t j = 0; j < after.size(); ++j) {
      if (before[i].serial != after[j].serial) {
        continue;
      }
      if (before[i].hasOrders != after[j].hasOrders ||
          before[i].hasPlayerOrders != after[j].hasPlayerOrders) {
        Log("AUTONOMY_SPIKE_INVARIANT: secondary player order changed serial=" +
            ToString(before[i].serial) +
            " before_has_orders=" + BoolText(before[i].hasOrders) +
            " after_has_orders=" + BoolText(after[j].hasOrders) +
            " before_player_orders=" + BoolText(before[i].hasPlayerOrders) +
            " after_player_orders=" + BoolText(after[j].hasPlayerOrders));
        return false;
      }
      break;
    }
  }
  return true;
}

Character *BindOrResolveTarget(GameWorld *world, Character *selectedCharacter) {
  if (g_probe.boundSerial != 0) {
    Character *bound = ResolveCharacterBySerial(world, g_probe.boundSerial);
    if (!bound) {
      return NULL;
    }
    unsigned int liveSerial = 0;
    hand liveHandle;
    if (!TryReadCharacterSerial(bound, liveSerial, liveHandle) ||
        liveSerial != g_probe.boundSerial) {
      return NULL;
    }
    return bound;
  }

  Character *candidate = NULL;
  if (g_probe.config.targetSerial != 0) {
    candidate = ResolveCharacterBySerial(world, g_probe.config.targetSerial);
  } else {
    candidate = selectedCharacter;
  }

  unsigned int serial = 0;
  hand handle;
  if (!TryReadCharacterSerial(candidate, serial, handle)) {
    return NULL;
  }

  bool isPlayer = false;
  try {
    isPlayer = candidate->isPlayerCharacter();
  } catch (...) {
  }
  if (!isPlayer) {
    DWORD now = GetTickCount();
    if (now - g_probe.lastWaitingLogTick >= 2000) {
      g_probe.lastWaitingLogTick = now;
      Log("AUTONOMY_SPIKE: refusing target serial=" + ToString(serial) +
          " name=\"" + SafeCharacterName(candidate) +
          "\" reason=not_player_character");
    }
    return NULL;
  }

  g_probe.boundSerial = serial;
  g_probe.boundHandle = handle;
  g_probe.lastTelemetryTick = 0;
  Log("AUTONOMY_SPIKE: target bound serial=" + ToString(serial) +
      " name=\"" + SafeCharacterName(candidate) +
      "\" source=" +
      std::string(g_probe.config.targetSerial != 0 ? "configured_serial"
                                                   : "selected_character"));
  return candidate;
}

Stobe::AutonomySafetyProbe::MutationPreconditions
BuildMutationPreconditions(const AiSnapshot &snapshot) {
  Stobe::AutonomySafetyProbe::MutationPreconditions result;
  result.enabled = g_probe.config.enabled;
  result.hasBoundTarget = g_probe.boundSerial != 0;
  result.isPlayerCharacter = snapshot.playerCharacter;
  result.isDead = snapshot.dead;
  result.isUnconscious = snapshot.unconscious;
  result.hasOrdersReceiver = snapshot.hasOrdersReceiver;
  result.canTakeOrders = snapshot.canTakeOrders;
  result.hasOrders = snapshot.hasOrders;
  result.travelDestinationSet = g_probe.config.travelDestinationSet;
  return result;
}

void ExecuteCommand(GameWorld *world, Character *character,
                    Stobe::AutonomySafetyProbe::CommandType command) {
  using namespace Stobe::AutonomySafetyProbe;

  if (command == COMMAND_NONE) {
    Log("AUTONOMY_SPIKE_COMMAND: rejected reason=unknown_command value=\"" +
        g_probe.config.commandText + "\"");
    ShowProbeMessage(world, "Autonomy probe rejected an unknown command.");
    return;
  }
  if (command == COMMAND_RESET) {
    ClearBinding("reset_command");
    ShowProbeMessage(world, "Autonomy probe target binding reset.");
    return;
  }
  if (!character || g_probe.boundSerial == 0) {
    Log(std::string("AUTONOMY_SPIKE_COMMAND: command=") + CommandName(command) +
        " rejected reason=no_bound_target");
    ShowProbeMessage(world, "Autonomy probe has no valid bound target.");
    return;
  }

  AiSnapshot before = CaptureSnapshot(character);
  LogSnapshot("before_command", character, g_probe.boundSerial, before);
  if (command == COMMAND_OBSERVE) {
    Log("AUTONOMY_SPIKE_COMMAND: command=OBSERVE accepted serial=" +
        ToString(g_probe.boundSerial));
    ShowProbeMessage(world, "Autonomy probe observation written to stobe.log.");
    return;
  }

  ValidationResult validation =
      ValidateMutation(command, BuildMutationPreconditions(before));
  if (validation != VALIDATION_OK) {
    Log(std::string("AUTONOMY_SPIKE_COMMAND: command=") + CommandName(command) +
        " rejected serial=" + ToString(g_probe.boundSerial) + " reason=" +
        ValidationResultName(validation));
    ShowProbeMessage(world, std::string("Autonomy probe rejected ") +
                                CommandName(command) + ": " +
                                ValidationResultName(validation) + ".");
    return;
  }

  OrdersReceiver *orders = NULL;
  try {
    orders = character->getOrdersReciever();
  } catch (...) {
  }
  if (!orders || reinterpret_cast<uintptr_t>(orders) <= 0x1000) {
    Log(std::string("AUTONOMY_SPIKE_COMMAND: command=") + CommandName(command) +
        " rejected serial=" + ToString(g_probe.boundSerial) +
        " reason=orders_receiver_lost");
    return;
  }

  const TaskType task = command == COMMAND_IDLE ? IDLE : MOVE_CUS_ORDERED;
  const Ogre::Vector3 location =
      command == COMMAND_IDLE ? before.position : g_probe.config.travelDestination;
  const std::vector<PlayerOrderState> otherOrdersBefore =
      CaptureOtherPlayerOrderStates(world, g_probe.boundSerial);
  bool issued = false;
  try {
    hand noSubject;
    orders->addOrder(task, noSubject, location, false, false);
    character->reThinkCurrentAIAction();
    issued = true;
  } catch (...) {
    issued = false;
  }

  AiSnapshot after = CaptureSnapshot(character);
  const std::vector<PlayerOrderState> otherOrdersAfter =
      CaptureOtherPlayerOrderStates(world, g_probe.boundSerial);
  LogSnapshot("after_command", character, g_probe.boundSerial, after);
  const bool jobsPreserved =
      before.jobsEnabled == after.jobsEnabled &&
      before.permajobCount == after.permajobCount &&
      before.permajobs == after.permajobs;
  const bool otherOrdersPreserved =
      OtherPlayerOrdersUnchanged(otherOrdersBefore, otherOrdersAfter);

  Log(std::string("AUTONOMY_SPIKE_COMMAND: command=") + CommandName(command) +
      " serial=" + ToString(g_probe.boundSerial) +
      " task=" + TaskName(task) + " location=" + VectorText(location) +
      " issued=" + BoolText(issued) +
      " clear_old=0 shift=0 jobs_preserved=" + BoolText(jobsPreserved) +
      " other_player_orders_preserved=" + BoolText(otherOrdersPreserved) +
      " before_has_orders=" + BoolText(before.hasOrders) +
      " after_has_orders=" + BoolText(after.hasOrders));

  if (!jobsPreserved) {
    Log("AUTONOMY_SPIKE_INVARIANT: jobs changed during command serial=" +
        ToString(g_probe.boundSerial) + " command=" + CommandName(command));
  }
  if (issued) {
    g_probe.invariantWatchActive = true;
    g_probe.invariantViolationLogged = !jobsPreserved || !otherOrdersPreserved;
    g_probe.invariantJobsEnabled = before.jobsEnabled;
    g_probe.invariantPermajobCount = before.permajobCount;
    g_probe.invariantPermajobs = before.permajobs;
    g_probe.invariantCommand = CommandName(command);
    g_probe.invariantStartedTick = GetTickCount();
  }
  ShowProbeMessage(
      world, std::string("Autonomy probe ") + CommandName(command) +
                 (issued ? " issued to " : " failed for ") +
                 SafeCharacterName(character) + ".");
}

} // namespace

void ResetAutonomySafetyProbe(const char *reason) {
  ClearBinding(reason ? reason : "runtime_reset");
}

void UpdateAutonomySafetyProbe(GameWorld *world, Character *selectedCharacter) {
  if (!world) {
    return;
  }

  const DWORD now = GetTickCount();
  if (!g_probe.configInitialized ||
      now - g_probe.lastConfigTick >= kConfigRefreshMs) {
    ProbeConfig updated = ReadConfig();
    g_probe.lastConfigTick = now;

    if (!g_probe.configInitialized) {
      g_probe.configInitialized = true;
      g_probe.lastCommandNonce = updated.commandNonce;
      Log("AUTONOMY_SPIKE: config initialized enabled=" +
          BoolText(updated.enabled) + " command_nonce=" +
          ToString(updated.commandNonce) +
          "; increment CommandNonce after startup to issue a command");
    }

    if (g_probe.configuredTargetSerial != updated.targetSerial) {
      ClearBinding("configured_target_changed");
      g_probe.configuredTargetSerial = updated.targetSerial;
    }
    g_probe.config = updated;
  }

  if (!g_probe.config.enabled) {
    if (g_probe.wasEnabled) {
      Log("AUTONOMY_SPIKE: disabled");
    }
    g_probe.wasEnabled = false;
    g_probe.lastCommandNonce = g_probe.config.commandNonce;
    ClearBinding("disabled");
    return;
  }

  if (!g_probe.wasEnabled) {
    g_probe.wasEnabled = true;
    Log("AUTONOMY_SPIKE: enabled; probe is local-only and does not call the LLM");
  }

  Character *target = BindOrResolveTarget(world, selectedCharacter);
  if (!target) {
    if (now - g_probe.lastWaitingLogTick >= 2000) {
      g_probe.lastWaitingLogTick = now;
      Log("AUTONOMY_SPIKE: waiting for target configured_serial=" +
          ToString(g_probe.config.targetSerial) +
          " bound_serial=" + ToString(g_probe.boundSerial));
    }
  } else if (g_probe.lastTelemetryTick == 0 ||
             now - g_probe.lastTelemetryTick >=
                 g_probe.config.telemetryIntervalMs) {
    g_probe.lastTelemetryTick = now;
    AiSnapshot periodic = CaptureSnapshot(target);
    LogSnapshot("periodic", target, g_probe.boundSerial, periodic);
    CheckJobsInvariant(target, periodic);
  }

  if (g_probe.config.commandNonce != g_probe.lastCommandNonce) {
    g_probe.lastCommandNonce = g_probe.config.commandNonce;
    ExecuteCommand(world, target,
                   Stobe::AutonomySafetyProbe::ParseCommand(
                       g_probe.config.commandText));
  }
}
