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

  const std::string phase4Prefix =
      "{\"ok\":true,\"phase\":4,\"decision\":{"
      "\"decision_id\":\"phase4-typed\",\"control_revision\":7,"
      "\"npc_id\":12,\"npc_storage_id\":\"hand_884422\","
      "\"runtime_serial\":884422,";
  const std::string phase4Suffix =
      ",\"context_hash\":\"phase4-context\",\"context_game_ts\":1234,"
      "\"dispatch_deadline_ts\":2000,\"action_deadline_ts\":3000}}";
  DecisionEnvelope moveNearbyDecision;
  bool phase4Present = false;
  ExpectBool("Phase 4 parses MOVE_NEARBY",
             ParseDecisionResponse(
                 phase4Prefix +
                     "\"command\":\"MOVE_NEARBY\",\"arguments\":{"
                     "\"x\":25,\"y\":2,\"z\":10,\"arrival_radius\":4}" +
                     phase4Suffix,
                 moveNearbyDecision, phase4Present),
             true);
  ExpectUInt32("Phase 4 classifies MOVE_NEARBY as typed",
               static_cast<unsigned int>(moveNearbyDecision.command),
               static_cast<unsigned int>(
                   Stobe::Autonomy::DECISION_COMMAND_MOVE_NEARBY));

  DecisionEnvelope fleeDecision;
  ExpectBool("Phase 4 parses FLEE",
             ParseDecisionResponse(
                 phase4Prefix +
                     "\"command\":\"FLEE\",\"arguments\":{\"x\":80,"
                     "\"y\":2,\"z\":20,\"arrival_radius\":6,"
                     "\"safe_radius\":70}" + phase4Suffix,
                 fleeDecision, phase4Present),
             true);
  ExpectUInt32("Phase 4 classifies FLEE as typed",
               static_cast<unsigned int>(fleeDecision.command),
               static_cast<unsigned int>(
                   Stobe::Autonomy::DECISION_COMMAND_FLEE));

  DecisionEnvelope firstAidDecision;
  ExpectBool("Phase 4 parses FIRST_AID",
             ParseDecisionResponse(
                 phase4Prefix +
                     "\"command\":\"FIRST_AID\",\"arguments\":{"
                     "\"target_runtime_serial\":9911}" + phase4Suffix,
                 firstAidDecision, phase4Present),
             true);
  ExpectUInt32("Phase 4 binds FIRST_AID target serial",
               firstAidDecision.targetRuntimeSerial, 9911);

  DecisionEnvelope restDecision;
  ExpectBool("Phase 4 parses REST",
             ParseDecisionResponse(phase4Prefix +
                                       "\"command\":\"REST\","
                                       "\"arguments\":{}" + phase4Suffix,
                                   restDecision, phase4Present),
             true);

  const std::string phase5Prefix =
      "{\"ok\":true,\"phase\":5,\"decision\":{"
      "\"decision_id\":\"phase5-typed\",\"control_revision\":7,"
      "\"npc_id\":12,\"npc_storage_id\":\"hand_884422\","
      "\"runtime_serial\":884422,";
  const std::string phase5Suffix =
      ",\"context_hash\":\"phase5-context\",\"context_game_ts\":1234,"
      "\"dispatch_deadline_ts\":2000,\"action_deadline_ts\":3000}}";
  DecisionEnvelope attackDecision;
  bool phase5Present = false;
  ExpectBool("Phase 5 parses ATTACK",
             ParseDecisionResponse(
                 phase5Prefix +
                     "\"command\":\"ATTACK\",\"arguments\":{"
                     "\"target\":\"Dust Bandit\","
                     "\"target_runtime_serial\":9911}" + phase5Suffix,
                 attackDecision, phase5Present),
             true);
  ExpectUInt32("Phase 5 classifies ATTACK as typed",
               static_cast<unsigned int>(attackDecision.command),
               static_cast<unsigned int>(
                   Stobe::Autonomy::DECISION_COMMAND_ATTACK));
  ExpectUInt32("Phase 5 binds ATTACK target serial",
               attackDecision.targetRuntimeSerial, 9911);

  DecisionEnvelope takeItemDecision;
  ExpectBool("Phase 5 parses constrained TAKE_ITEM",
             ParseDecisionResponse(
                 phase5Prefix +
                     "\"command\":\"TAKE_ITEM\",\"arguments\":{"
                     "\"target\":\"Dust Bandit\","
                     "\"target_runtime_serial\":9911,"
                     "\"item\":\"Bread\",\"amount\":2}" + phase5Suffix,
                 takeItemDecision, phase5Present),
             true);
  ExpectUInt32("Phase 5 classifies TAKE_ITEM as typed",
               static_cast<unsigned int>(takeItemDecision.command),
               static_cast<unsigned int>(
                   Stobe::Autonomy::DECISION_COMMAND_TAKE_ITEM));
  ExpectEq("Phase 5 preserves the specific loot item",
           takeItemDecision.itemName, "Bread");
  ExpectUInt32("Phase 5 preserves bounded loot amount",
               static_cast<unsigned int>(takeItemDecision.itemAmount), 2);

  DecisionEnvelope equipItemDecision;
  ExpectBool("Phase 5 parses EQUIP_ITEM",
             ParseDecisionResponse(
                 phase5Prefix +
                     "\"command\":\"EQUIP_ITEM\",\"arguments\":{"
                     "\"item\":\"Nodachi\"}" + phase5Suffix,
                 equipItemDecision, phase5Present),
             true);
  ExpectUInt32("Phase 5 classifies EQUIP_ITEM as typed",
               static_cast<unsigned int>(equipItemDecision.command),
               static_cast<unsigned int>(
                   Stobe::Autonomy::DECISION_COMMAND_EQUIP_ITEM));

  DecisionEnvelope removeLimbDecision;
  ExpectBool("Phase 5 parses REMOVE_LIMB",
             ParseDecisionResponse(
                 phase5Prefix +
                     "\"command\":\"REMOVE_LIMB\",\"arguments\":{"
                     "\"target\":\"Dust Bandit\","
                     "\"target_runtime_serial\":9911,"
                     "\"limb\":\"LEFT_ARM\"}" + phase5Suffix,
                 removeLimbDecision, phase5Present),
             true);
  ExpectUInt32("Phase 5 maps the limb enum",
               static_cast<unsigned int>(removeLimbDecision.limbCode), 1);

  static const char *phase3CatalogCommands[] = {
      "SUICIDE",      "FOLLOW",           "STOP_FOLLOW",
      "JOIN_PARTY",   "LEAVE",
      "STOP_CARRYING", "PICKUP_NPC",      "GIVE_CATS",
      "TAKE_CATS",    "GIVE_ITEM",
      "DROP_ITEM",    "ROLEPLAY_ACTION",  "FACTION_RELATIONS",
      "SET_BLOCK",    "SET_HOLD",         "SET_PASSIVE",
      "SET_JOBS",     "SET_RANGED",       "SET_TAUNT",
      "SET_SNEAK",    "SET_RESOURCE",     "SET_MEDIC",
      "USE_OBJECT",   "USE_DRUGS",
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

  DecisionEnvelope broadLootDecision;
  bool broadLootPresent = false;
  ExpectBool(
      "Phase 5 rejects broad TAKE_ITEM queries",
      ParseDecisionResponse(
          phase5Prefix +
              "\"command\":\"TAKE_ITEM\",\"arguments\":{"
              "\"target\":\"Dust Bandit\","
              "\"target_runtime_serial\":9911,\"item\":\"\",\"amount\":1}" +
              phase5Suffix,
          broadLootDecision, broadLootPresent),
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

  MonitorFacts localArrival = arrived;
  localArrival.x = moveNearbyDecision.x;
  localArrival.y = moveNearbyDecision.y;
  localArrival.z = moveNearbyDecision.z;
  localArrival.stationarySamples = 2;
  ExpectUInt32(
      "Phase 4 MOVE_NEARBY completes at its derived destination",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    moveNearbyDecision, ownedOrder,
                                    localArrival, 60000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_COMPLETED));

  OrderFingerprint attackOrder;
  attackOrder.taskType = 124;
  attackOrder.subjectSerial = 9911;
  attackOrder.runtimePointer = 5678;
  attackOrder.orderCount = 1;
  MonitorFacts combatActive = arrived;
  combatActive.currentOrder = attackOrder;
  combatActive.targetFound = true;
  combatActive.inCombat = true;
  combatActive.attackTargetSerial = 9911;
  ExpectUInt32(
      "Phase 5 ATTACK remains active while native combat owns the target",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    attackDecision, attackOrder,
                                    combatActive, 180000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_RUNNING));
  combatActive.targetUnconscious = true;
  ExpectUInt32(
      "Phase 5 ATTACK completes when the target is neutralized",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    attackDecision, attackOrder,
                                    combatActive, 180000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_COMPLETED));
  combatActive.targetUnconscious = false;
  combatActive.attackTargetSerial = 7722;
  ExpectUInt32(
      "Phase 5 ATTACK stops when native combat replaces the target",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    attackDecision, attackOrder,
                                    combatActive, 180000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_INTERRUPTED));

  MonitorFacts escaped = arrived;
  escaped.hostileObserved = false;
  escaped.nearestHostileDistance = 1000000.0;
  escaped.fleeDistanceTravelled = 25.0;
  ExpectUInt32(
      "Phase 4 FLEE completes after threats leave observation",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    fleeDecision, ownedOrder, escaped, 90000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_COMPLETED));
  MonitorFacts scanDropout = arrived;
  scanDropout.hostileObserved = false;
  scanDropout.nearestHostileDistance = 1000000.0;
  scanDropout.fleeDistanceTravelled = 0.0;
  ExpectUInt32(
      "Phase 4 FLEE does not complete on a transient hostile scan dropout",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    fleeDecision, ownedOrder, scanDropout,
                                    90000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_RUNNING));
  MonitorFacts followed = localArrival;
  followed.hostileObserved = true;
  followed.nearestHostileDistance = 15.0;
  followed.x = fleeDecision.x;
  followed.y = fleeDecision.y;
  followed.z = fleeDecision.z;
  ExpectUInt32(
      "Phase 4 FLEE rejects an unsafe reached waypoint",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    fleeDecision, ownedOrder, followed, 90000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_FAILED));

  MonitorFacts treated = arrived;
  treated.targetFound = true;
  treated.targetFirstAidNeed = 0.0;
  treated.targetBleedRate = 0.0;
  ExpectUInt32(
      "Phase 4 FIRST_AID completes from live patient health",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    firstAidDecision, ownedOrder, treated,
                                    180000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_COMPLETED));
  treated.targetFirstAidNeed = 4.0;
  treated.noProgressElapsedMs = 45000;
  ExpectUInt32(
      "Phase 4 FIRST_AID fails after bounded no progress",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    firstAidDecision, ownedOrder, treated,
                                    180000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_FAILED));

  MonitorFacts rested = arrived;
  rested.fullyRested = true;
  ExpectUInt32(
      "Phase 4 REST completes from live recovery state",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    restDecision, ownedOrder, rested, 600000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_COMPLETED));
  MonitorFacts healingInBed = arrived;
  healingInBed.fullyRested = false;
  healingInBed.inBed = true;
  healingInBed.activeElapsedMs = 5000;
  healingInBed.currentOrder = OrderFingerprint();
  ExpectUInt32(
      "Phase 4 REST keeps monitoring after the bed order is consumed",
      static_cast<unsigned int>(EvaluateActionMonitor(
                                    restDecision, ownedOrder, healingInBed,
                                    600000)
                                    .outcome),
      static_cast<unsigned int>(MONITOR_RUNNING));

  if (g_failures != 0) {
    std::cerr << g_failures << " portable C++ tests failed.\n";
    return EXIT_FAILURE;
  }

  std::cout << "All portable C++ tests passed.\n";
  return EXIT_SUCCESS;
}
