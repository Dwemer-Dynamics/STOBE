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

  if (g_failures != 0) {
    std::cerr << g_failures << " portable C++ tests failed.\n";
    return EXIT_FAILURE;
  }

  std::cout << "All portable C++ tests passed.\n";
  return EXIT_SUCCESS;
}
