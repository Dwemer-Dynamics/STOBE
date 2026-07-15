#include "AutonomySafetyProbePolicy.h"
#include "AutonomyMonitor.h"
#include "AutonomyProtocol.h"
#include "StobeIdentityRename.h"
#include "StobeText.h"
#include "StobeTiming.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void ExpectEq(const std::string &name, const std::string &actual,
              const std::string &expected) {
  if (actual == expected) {
    std::cout << "PASS: " << name << "\n";
    return;
  }

  ++g_failures;
  std::cerr << "FAIL: " << name << "\n"
            << "  expected: " << expected << "\n"
            << "  actual:   " << actual << "\n";
}

void ExpectUInt32(const std::string &name, unsigned int actual,
                  unsigned int expected) {
  if (actual == expected) {
    std::cout << "PASS: " << name << "\n";
    return;
  }

  ++g_failures;
  std::cerr << "FAIL: " << name << "\n"
            << "  expected: " << expected << "\n"
            << "  actual:   " << actual << "\n";
}

void ExpectBool(const std::string &name, bool actual, bool expected) {
  if (actual == expected) {
    std::cout << "PASS: " << name << "\n";
    return;
  }

  ++g_failures;
  std::cerr << "FAIL: " << name << "\n"
            << "  expected: " << (expected ? "true" : "false") << "\n"
            << "  actual:   " << (actual ? "true" : "false") << "\n";
}

} // namespace

int main() {
  using Stobe::AutonomySafetyProbe::COMMAND_IDLE;
  using Stobe::AutonomySafetyProbe::COMMAND_NONE;
  using Stobe::AutonomySafetyProbe::COMMAND_TRAVEL;
  using Stobe::AutonomySafetyProbe::MutationPreconditions;
  using Stobe::AutonomySafetyProbe::ParseCommand;
  using Stobe::AutonomySafetyProbe::VALIDATION_CANNOT_TAKE_ORDERS;
  using Stobe::AutonomySafetyProbe::VALIDATION_DISABLED;
  using Stobe::AutonomySafetyProbe::VALIDATION_EXISTING_ORDER;
  using Stobe::AutonomySafetyProbe::VALIDATION_NO_BOUND_TARGET;
  using Stobe::AutonomySafetyProbe::VALIDATION_NO_ORDERS_RECEIVER;
  using Stobe::AutonomySafetyProbe::VALIDATION_NOT_PLAYER_CHARACTER;
  using Stobe::AutonomySafetyProbe::VALIDATION_OK;
  using Stobe::AutonomySafetyProbe::VALIDATION_TARGET_DEAD;
  using Stobe::AutonomySafetyProbe::VALIDATION_TARGET_UNCONSCIOUS;
  using Stobe::AutonomySafetyProbe::VALIDATION_TRAVEL_DESTINATION_NOT_SET;
  using Stobe::AutonomySafetyProbe::ValidateMutation;
  using Stobe::Autonomy::ControlSnapshot;
  using Stobe::Autonomy::DecisionEnvelope;
  using Stobe::Autonomy::EvaluatePhase1State;
  using Stobe::Autonomy::EvaluateActionMonitor;
  using Stobe::Autonomy::MONITOR_COMPLETED;
  using Stobe::Autonomy::MONITOR_FAILED;
  using Stobe::Autonomy::MONITOR_INTERRUPTED;
  using Stobe::Autonomy::MONITOR_RUNNING;
  using Stobe::Autonomy::MONITOR_TIMED_OUT;
  using Stobe::Autonomy::MonitorFacts;
  using Stobe::Autonomy::OrderFingerprint;
  using Stobe::Autonomy::ParseControlResponse;
  using Stobe::Autonomy::ParseDecisionResponse;
  using Stobe::Autonomy::ParseStorageSerial;
  using Stobe::Autonomy::RuntimeFacts;
  using Stobe::Autonomy::ValidateDecisionEnvelope;
  using Stobe::IdentityRename::BatchStatus;
  using Stobe::IdentityRename::IsAttemptReady;
  using Stobe::IdentityRename::IsQueueEligibleName;
  using Stobe::IdentityRename::ParseBatchStatus;
  using Stobe::Timing::ResolveRechatDispatchDelayMs;
  using Stobe::Timing::ShouldWaitForPlaybackBeforeRechatDispatch;
  using Stobe::Text::EscapeJSON;
  using Stobe::Text::JsonReadField;
  using Stobe::Text::SanitizeDialogueForEventStream;
  using Stobe::Text::UnescapeJSON;

  ExpectEq("EscapeJSON escapes quotes, slashes, and newlines",
           EscapeJSON("He said \"hi\"\\there\n"), "He said \\\"hi\\\"\\\\there\\n");
  ExpectEq("UnescapeJSON restores unicode and escaped text",
           UnescapeJSON("Kenshi\\n\\u263A"), "Kenshi\n\xE2\x98\xBA");
  ExpectEq("JsonReadField reads escaped string values",
           JsonReadField("{\"text\":\"hello \\\"wanderer\\\"\"}", "text"),
           "hello \"wanderer\"");
  ExpectEq("JsonReadField preserves nested arrays",
           JsonReadField("{\"people\":[{\"name\":\"Hobbs\"}],\"ok\":true}",
                         "people"),
           "[{\"name\":\"Hobbs\"}]");
  ExpectEq("SanitizeDialogue trims engine corruption",
           SanitizeDialogueForEventStream("  Need help??  "), "Need help");
  ExpectEq("SanitizeDialogue strips leaked numeric tail",
           SanitizeDialogueForEventStream("Follow me 7"), "Follow me");
  ExpectEq("SanitizeDialogue keeps intentional single question",
           SanitizeDialogueForEventStream("Ready?"), "Ready?");
  ExpectBool("Identity rename queue accepts generic names",
             IsQueueEligibleName("Shek Warrior"), true);
  ExpectBool("Identity rename queue rejects renamed bracket names",
             IsQueueEligibleName("Marek [Shek Warrior]"), false);
  ExpectBool("Identity rename queue rejects unknown names",
             IsQueueEligibleName("Unknown"), false);
  ExpectBool("Identity rename cooldown blocks future retry tick",
             IsAttemptReady(1000, 2000), false);
  ExpectBool("Identity rename cooldown allows expired retry tick",
             IsAttemptReady(3000, 2000), true);
  ExpectUInt32("Identity rename batch status parses retry",
               static_cast<unsigned int>(ParseBatchStatus("retry")),
               static_cast<unsigned int>(Stobe::IdentityRename::BATCH_STATUS_RETRY));
  ExpectUInt32("Identity rename batch status parses ok as complete",
               static_cast<unsigned int>(ParseBatchStatus("ok")),
               static_cast<unsigned int>(Stobe::IdentityRename::BATCH_STATUS_COMPLETE));
  ExpectUInt32("Rechat dispatch delay ignores line pacing",
               ResolveRechatDispatchDelayMs(2720), 0);
  ExpectUInt32("Rechat dispatch delay stays immediate for long lines",
               ResolveRechatDispatchDelayMs(6600), 0);
  ExpectBool("Rechat dispatch does not wait for playback",
             ShouldWaitForPlaybackBeforeRechatDispatch(), false);

  ExpectUInt32("Autonomy probe parses trimmed case-insensitive command",
               static_cast<unsigned int>(ParseCommand("  travel  ")),
               static_cast<unsigned int>(COMMAND_TRAVEL));
  ExpectUInt32("Autonomy probe rejects unknown command names",
               static_cast<unsigned int>(ParseCommand("raw_task")),
               static_cast<unsigned int>(COMMAND_NONE));

  ControlSnapshot control;
  ExpectBool("Autonomy control parses server session",
             ParseControlResponse(
                 "{\"ok\":true,\"phase\":1,\"session\":{\"enabled\":true,"
                 "\"desired_state\":\"ARMING\",\"control_revision\":7,"
                 "\"npc_id\":12,\"npc_storage_id\":\"hand_884422\","
                 "\"npc_name\":\"Ruka\",\"stop_mode\":\"normal\"}}",
                 control),
             true);
  ExpectUInt32("Autonomy control keeps exact NPC id",
               static_cast<unsigned int>(control.npcId), 12);
  ExpectUInt32("Autonomy storage identity parses serial",
               ParseStorageSerial("hand_884422"), 884422);
  ExpectUInt32("Autonomy storage identity rejects malformed ids",
               ParseStorageSerial("serial:884422"), 0);

  RuntimeFacts runtimeReady;
  runtimeReady.found = true;
  runtimeReady.identityMatches = true;
  runtimeReady.playerCharacter = true;
  runtimeReady.hasOrdersReceiver = true;
  runtimeReady.canTakeOrders = true;
  ExpectEq("Phase 1 observes an available exact player NPC",
           EvaluatePhase1State(control, runtimeReady).state, "OBSERVING");

  RuntimeFacts runtimeManualOrder = runtimeReady;
  runtimeManualOrder.hasPlayerOrders = true;
  ExpectEq("Phase 1 pauses when the player gives a manual order",
           EvaluatePhase1State(control, runtimeManualOrder).state,
           "PAUSED_USER");

  RuntimeFacts runtimeManualPauseLatched = runtimeReady;
  runtimeManualPauseLatched.requiresExplicitResume = true;
  runtimeManualPauseLatched.manualPauseLatched = true;
  ExpectEq("Phase 1 retains manual pause until a new revision",
           EvaluatePhase1State(control, runtimeManualPauseLatched).state,
           "PAUSED_USER");

  RuntimeFacts runtimeAfterLoad = runtimeReady;
  runtimeAfterLoad.requiresExplicitResume = true;
  ExpectEq("Phase 1 requires explicit resume after load",
           EvaluatePhase1State(control, runtimeAfterLoad).state,
           "PAUSED_UNSAFE");

  RuntimeFacts runtimeServerLost = runtimeReady;
  runtimeServerLost.serverAvailable = false;
  ExpectEq("Phase 1 fails closed when server is unavailable",
           EvaluatePhase1State(control, runtimeServerLost).state,
           "PAUSED_UNSAFE");

  RuntimeFacts runtimeWrongNpc = runtimeReady;
  runtimeWrongNpc.identityMatches = false;
  ExpectEq("Phase 1 errors on runtime identity mismatch",
           EvaluatePhase1State(control, runtimeWrongNpc).state, "ERROR");

  MutationPreconditions probeReady;
  probeReady.enabled = true;
  probeReady.hasBoundTarget = true;
  probeReady.isPlayerCharacter = true;
  probeReady.hasOrdersReceiver = true;
  probeReady.canTakeOrders = true;
  probeReady.travelDestinationSet = true;
  ExpectUInt32("Autonomy probe permits idle for an available bound player",
               static_cast<unsigned int>(ValidateMutation(COMMAND_IDLE, probeReady)),
               static_cast<unsigned int>(VALIDATION_OK));

  MutationPreconditions probeDisabled = probeReady;
  probeDisabled.enabled = false;
  ExpectUInt32("Autonomy probe rejects mutation while disabled",
               static_cast<unsigned int>(
                   ValidateMutation(COMMAND_IDLE, probeDisabled)),
               static_cast<unsigned int>(VALIDATION_DISABLED));

  MutationPreconditions probeUnbound = probeReady;
  probeUnbound.hasBoundTarget = false;
  ExpectUInt32("Autonomy probe rejects mutation without a bound target",
               static_cast<unsigned int>(
                   ValidateMutation(COMMAND_IDLE, probeUnbound)),
               static_cast<unsigned int>(VALIDATION_NO_BOUND_TARGET));

  MutationPreconditions probeNonPlayer = probeReady;
  probeNonPlayer.isPlayerCharacter = false;
  ExpectUInt32("Autonomy probe rejects non-player characters",
               static_cast<unsigned int>(
                   ValidateMutation(COMMAND_IDLE, probeNonPlayer)),
               static_cast<unsigned int>(VALIDATION_NOT_PLAYER_CHARACTER));

  MutationPreconditions probeDead = probeReady;
  probeDead.isDead = true;
  ExpectUInt32("Autonomy probe rejects dead characters",
               static_cast<unsigned int>(
                   ValidateMutation(COMMAND_IDLE, probeDead)),
               static_cast<unsigned int>(VALIDATION_TARGET_DEAD));

  MutationPreconditions probeUnconscious = probeReady;
  probeUnconscious.isUnconscious = true;
  ExpectUInt32("Autonomy probe rejects unconscious characters",
               static_cast<unsigned int>(
                   ValidateMutation(COMMAND_IDLE, probeUnconscious)),
               static_cast<unsigned int>(VALIDATION_TARGET_UNCONSCIOUS));

  MutationPreconditions probeMissingReceiver = probeReady;
  probeMissingReceiver.hasOrdersReceiver = false;
  ExpectUInt32("Autonomy probe requires an orders receiver",
               static_cast<unsigned int>(
                   ValidateMutation(COMMAND_IDLE, probeMissingReceiver)),
               static_cast<unsigned int>(VALIDATION_NO_ORDERS_RECEIVER));

  MutationPreconditions probeCannotTakeOrders = probeReady;
  probeCannotTakeOrders.canTakeOrders = false;
  ExpectUInt32("Autonomy probe respects Kenshi order availability",
               static_cast<unsigned int>(
                   ValidateMutation(COMMAND_IDLE, probeCannotTakeOrders)),
               static_cast<unsigned int>(VALIDATION_CANNOT_TAKE_ORDERS));

  MutationPreconditions probeBusy = probeReady;
  probeBusy.hasOrders = true;
  ExpectUInt32("Autonomy probe preserves existing orders by rejecting mutation",
               static_cast<unsigned int>(ValidateMutation(COMMAND_IDLE, probeBusy)),
               static_cast<unsigned int>(VALIDATION_EXISTING_ORDER));

  MutationPreconditions probeTravelUnset = probeReady;
  probeTravelUnset.travelDestinationSet = false;
  ExpectUInt32("Autonomy travel requires an explicitly armed destination",
               static_cast<unsigned int>(
                   ValidateMutation(COMMAND_TRAVEL, probeTravelUnset)),
               static_cast<unsigned int>(
                   VALIDATION_TRAVEL_DESTINATION_NOT_SET));

  const std::string travelDecisionJson =
      "{\"ok\":true,\"phase\":2,\"decision\":{"
      "\"decision_id\":\"5df4f15b-b99a-4fea-bfce-3ed6ef816e5e\","
      "\"control_revision\":7,\"npc_id\":12,"
      "\"npc_storage_id\":\"hand_884422\",\"runtime_serial\":884422,"
      "\"command\":\"TRAVEL_LOCATION\","
      "\"arguments\":{\"location_zone_id\":44,\"zone_name\":\"Squin\","
      "\"x\":123.5,\"y\":4,\"z\":-77.25,\"arrival_radius\":8},"
      "\"context_hash\":\"phase2-7-1-884422\",\"context_game_ts\":1234,"
      "\"dispatch_deadline_ts\":2000,\"action_deadline_ts\":3000}}";
  DecisionEnvelope travelDecision;
  bool hasDecision = false;
  ExpectBool("Phase 2 parses a typed travel decision",
             ParseDecisionResponse(travelDecisionJson, travelDecision,
                                   hasDecision),
             true);
  ExpectBool("Phase 2 identifies a present decision", hasDecision, true);
  ExpectUInt32("Phase 2 preserves the exact visited location id",
               static_cast<unsigned int>(travelDecision.locationZoneId), 44);
  ExpectUInt32(
      "Phase 2 validates matching revision and identity",
      static_cast<unsigned int>(ValidateDecisionEnvelope(
          travelDecision, control, 884422, 1900)),
      static_cast<unsigned int>(Stobe::Autonomy::DECISION_VALID));
  ExpectUInt32(
      "Phase 2 rejects an expired dispatch",
      static_cast<unsigned int>(ValidateDecisionEnvelope(
          travelDecision, control, 884422, 2100)),
      static_cast<unsigned int>(Stobe::Autonomy::DECISION_EXPIRED));

  DecisionEnvelope noDecision;
  bool hasNoDecision = true;
  ExpectBool("Phase 2 accepts an empty pilot tick",
             ParseDecisionResponse(
                 "{\"ok\":true,\"phase\":2,\"decision\":null}",
                 noDecision, hasNoDecision),
             true);
  ExpectBool("Phase 2 leaves an empty tick actionless", hasNoDecision, false);

  DecisionEnvelope invalidDecision;
  bool invalidPresent = false;
  ExpectBool(
      "Phase 2 rejects non-finite travel coordinates",
      ParseDecisionResponse(
          "{\"ok\":true,\"decision\":{\"decision_id\":\"bad\","
          "\"control_revision\":7,\"npc_id\":12,"
          "\"npc_storage_id\":\"hand_884422\",\"runtime_serial\":884422,"
          "\"command\":\"TRAVEL_LOCATION\",\"arguments\":{"
          "\"location_zone_id\":1,\"x\":1e999,\"y\":0,\"z\":0,"
          "\"arrival_radius\":8},\"context_hash\":\"phase2-invalid\","
          "\"context_game_ts\":1234,\"dispatch_deadline_ts\":2000,"
          "\"action_deadline_ts\":3000}}",
          invalidDecision, invalidPresent),
      false);

  static const char *phase3CatalogCommands[] = {
      "ATTACK",       "SUICIDE",          "FOLLOW",
      "STOP_FOLLOW",  "JOIN_PARTY",       "LEAVE",
      "STOP_CARRYING", "PICKUP_NPC",      "GIVE_CATS",
      "TAKE_CATS",    "TAKE_ITEM",        "GIVE_ITEM",
      "DROP_ITEM",    "ROLEPLAY_ACTION",  "FACTION_RELATIONS",
      "SET_BLOCK",    "SET_HOLD",         "SET_PASSIVE",
      "SET_JOBS",     "SET_RANGED",       "SET_TAUNT",
      "SET_SNEAK",    "SET_RESOURCE",     "SET_MEDIC",
      "REMOVE_LIMB",  "CUT_HORNS",        "KNOCKOUT",
      "KILL",         "USE_OBJECT",       "USE_DRUGS",
      "DRINK",        "FORCE_DRINK",      "TALK"};
  for (size_t i = 0;
       i < sizeof(phase3CatalogCommands) / sizeof(phase3CatalogCommands[0]);
       ++i) {
    const std::string command = phase3CatalogCommands[i];
    const std::string catalogJson =
        "{\"ok\":true,\"phase\":3,\"decision\":{"
        "\"decision_id\":\"phase3-catalog\",\"control_revision\":7,"
        "\"npc_id\":12,\"npc_storage_id\":\"hand_884422\","
        "\"runtime_serial\":884422,\"command\":\"" + command +
        "\",\"arguments\":{\"legacy_argument\":\"Dust Bandit@5\"},"
        "\"context_hash\":\"phase3-catalog-context\","
        "\"context_game_ts\":1234,\"dispatch_deadline_ts\":2000,"
        "\"action_deadline_ts\":3000}}";
    DecisionEnvelope catalogDecision;
    bool catalogPresent = false;
    ExpectBool("Phase 3 parses catalog action " + command,
               ParseDecisionResponse(catalogJson, catalogDecision,
                                     catalogPresent),
               true);
    ExpectBool("Phase 3 identifies catalog action " + command,
               catalogPresent, true);
    ExpectUInt32(
        "Phase 3 classifies generic catalog action " + command,
        static_cast<unsigned int>(catalogDecision.command),
        static_cast<unsigned int>(
            Stobe::Autonomy::DECISION_COMMAND_CATALOG_ACTION));
    ExpectEq("Phase 3 preserves catalog command " + command,
             catalogDecision.commandName, command);
    ExpectEq("Phase 3 preserves adapter argument " + command,
             catalogDecision.actionArgument, "Dust Bandit@5");
    ExpectUInt32(
        "Phase 3 validates catalog envelope " + command,
        static_cast<unsigned int>(ValidateDecisionEnvelope(
            catalogDecision, control, 884422, 1900)),
        static_cast<unsigned int>(Stobe::Autonomy::DECISION_VALID));
  }

  DecisionEnvelope unregisteredDecision;
  bool unregisteredPresent = false;
  ExpectBool(
      "Phase 3 rejects unregistered catalog commands",
      ParseDecisionResponse(
          "{\"ok\":true,\"phase\":3,\"decision\":{"
          "\"decision_id\":\"phase3-unknown\",\"control_revision\":7,"
          "\"npc_id\":12,\"npc_storage_id\":\"hand_884422\","
          "\"runtime_serial\":884422,\"command\":\"SPAWN_ITEM\","
          "\"arguments\":{\"legacy_argument\":\"Bread\"},"
          "\"context_hash\":\"phase3-unknown-context\","
          "\"context_game_ts\":1234,\"dispatch_deadline_ts\":2000,"
          "\"action_deadline_ts\":3000}}",
          unregisteredDecision, unregisteredPresent),
      false);

  OrderFingerprint ownedOrder;
  ownedOrder.taskType = 106;
  ownedOrder.x = travelDecision.x;
  ownedOrder.y = travelDecision.y;
  ownedOrder.z = travelDecision.z;
  ownedOrder.runtimePointer = 1234;
  ownedOrder.orderCount = 1;

  MonitorFacts arrived;
  arrived.found = true;
  arrived.identityMatches = true;
  arrived.playerCharacter = true;
  arrived.stationarySamples = 2;
  arrived.x = travelDecision.x;
  arrived.y = travelDecision.y;
  arrived.z = travelDecision.z;
  arrived.currentOrder = ownedOrder;
  ExpectUInt32(
      "Phase 2 travel completes by distance and stable movement",
      static_cast<unsigned int>(
          EvaluateActionMonitor(travelDecision, ownedOrder, arrived, 900000)
              .outcome),
      static_cast<unsigned int>(MONITOR_COMPLETED));

  MonitorFacts interrupted = arrived;
  interrupted.x = travelDecision.x + 50.0;
  interrupted.currentOrder.taskType = 99;
  ExpectUInt32(
      "Phase 2 detects a replacement player order",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    travelDecision, ownedOrder, interrupted,
                                    900000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_INTERRUPTED));

  MonitorFacts pathFailed = arrived;
  pathFailed.x = travelDecision.x + 50.0;
  pathFailed.stationarySamples = 0;
  pathFailed.pathFailedSamples = 3;
  ExpectUInt32(
      "Phase 2 bounds repeated path failure",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    travelDecision, ownedOrder, pathFailed,
                                    900000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_FAILED));

  MonitorFacts noProgress = pathFailed;
  noProgress.pathFailedSamples = 0;
  noProgress.noProgressElapsedMs = 30000;
  ExpectUInt32(
      "Phase 2 bounds travel with no meaningful progress",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    travelDecision, ownedOrder, noProgress,
                                    900000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_FAILED));

  MonitorFacts timedOut = pathFailed;
  timedOut.pathFailedSamples = 0;
  timedOut.activeElapsedMs = 900000;
  ExpectUInt32(
      "Phase 2 enforces the active-time deadline",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    travelDecision, ownedOrder, timedOut,
                                    900000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_TIMED_OUT));

  DecisionEnvelope idleDecision;
  idleDecision.command = Stobe::Autonomy::DECISION_COMMAND_IDLE;
  idleDecision.idleDurationMs = 1500;
  MonitorFacts idleStable = arrived;
  idleStable.stationarySamples = 3;
  idleStable.activeElapsedMs = 1000;
  ExpectUInt32(
      "Phase 2 IDLE waits for its declared duration",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    idleDecision, ownedOrder, idleStable,
                                    20000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_RUNNING));
  idleStable.activeElapsedMs = 1500;
  ExpectUInt32(
      "Phase 2 IDLE completes after duration and stable samples",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    idleDecision, ownedOrder, idleStable,
                                    20000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_COMPLETED));

  if (g_failures != 0) {
    std::cerr << g_failures << " portable C++ tests failed.\n";
    return EXIT_FAILURE;
  }

  std::cout << "All portable C++ tests passed.\n";
  return EXIT_SUCCESS;
}
