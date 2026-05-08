#pragma once

#include <string>

namespace Stobe {
namespace Text {

std::string EscapeJSON(const std::string &value);
std::string UnescapeJSON(const std::string &value);
std::string JsonReadField(const std::string &json, const std::string &key);
std::string SanitizeDialogueForEventStream(const std::string &value);

} // namespace Text
} // namespace Stobe
