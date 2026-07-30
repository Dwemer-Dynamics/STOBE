#include "StobeChatMode.h"

#include <algorithm>
#include <cctype>

namespace Stobe {
namespace ChatMode {
namespace {

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

} // namespace

std::string Normalize(const std::string &mode) {
  const std::string normalized = LowerAscii(mode);
  if (normalized == "whisper" || normalized == "shout" ||
      normalized == "cheat" || normalized == "narrator" ||
      normalized == "inject") {
    return normalized;
  }
  if (normalized == "event inject" || normalized == "injection_log") {
    return "inject";
  }
  if (normalized == "inject_chat" || normalized == "inject & chat" ||
      normalized == "inject and chat" || normalized == "injection_chat") {
    return "inject_chat";
  }
  return "chat";
}

std::size_t ToIndex(const std::string &mode) {
  const std::string normalized = Normalize(mode);
  if (normalized == "whisper")
    return 1;
  if (normalized == "shout")
    return 2;
  if (normalized == "cheat")
    return 3;
  if (normalized == "narrator")
    return 4;
  if (normalized == "inject")
    return 5;
  if (normalized == "inject_chat")
    return 6;
  return 0;
}

std::string DisplayLabel(const std::string &mode) {
  const std::string normalized = Normalize(mode);
  return normalized == "inject_chat" ? "inject & chat" : normalized;
}

std::string ResolveRequestMode(const std::string &selectedMode,
                               bool autoChatEnabled) {
  const std::string normalized = Normalize(selectedMode);
  if (normalized == "cheat" || normalized == "narrator" ||
      IsInjectionMode(normalized)) {
    return normalized;
  }
  if (autoChatEnabled) {
    return "autochat";
  }
  if (normalized == "whisper" || normalized == "shout") {
    return normalized;
  }
  return "talk";
}

bool IsInjectionMode(const std::string &mode) {
  const std::string normalized = Normalize(mode);
  return normalized == "inject" || normalized == "inject_chat";
}

bool AllowsManualActions(const std::string &mode) {
  const std::string normalized = Normalize(mode);
  return normalized != "narrator" && !IsInjectionMode(normalized);
}

bool ShouldQueueLocalPlayerSpeech(const std::string &requestMode) {
  return requestMode == "talk" || requestMode == "whisper" ||
         requestMode == "shout";
}

bool AllowsAutomaticRechat(const std::string &requestMode) {
  return requestMode != "whisper" && requestMode != "narrator" &&
         requestMode != "inject" && requestMode != "inject_chat";
}

std::string EventTypeForRequest(const std::string &requestMode) {
  return IsInjectionMode(requestMode) ? "injection" : "inputtext";
}

} // namespace ChatMode
} // namespace Stobe
