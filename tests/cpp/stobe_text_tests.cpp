#include "StobeText.h"

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

} // namespace

int main() {
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

  if (g_failures != 0) {
    std::cerr << g_failures << " portable C++ tests failed.\n";
    return EXIT_FAILURE;
  }

  std::cout << "All portable C++ tests passed.\n";
  return EXIT_SUCCESS;
}
