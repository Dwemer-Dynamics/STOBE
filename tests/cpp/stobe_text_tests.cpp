#include "AutonomySafetyProbePolicy.h"
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
  using Stobe::Autonomy::EvaluatePhase1State;
  using Stobe::Autonomy::ParseControlResponse;
  using Stobe::Autonomy::ParseStorageSerial;
  using Stobe::Autonomy::RuntimeFacts;
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

  if (g_failures != 0) {
    std::cerr << g_failures << " portable C++ tests failed.\n";
    return EXIT_FAILURE;
  }

  std::cout << "All portable C++ tests passed.\n";
  return EXIT_SUCCESS;
}
