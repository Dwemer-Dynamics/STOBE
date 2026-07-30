#pragma once

#include <cstddef>
#include <string>

namespace Stobe {
namespace ChatMode {

std::string Normalize(const std::string &mode);
std::size_t ToIndex(const std::string &mode);
std::string DisplayLabel(const std::string &mode);
std::string ResolveRequestMode(const std::string &selectedMode,
                               bool autoChatEnabled);
bool IsInjectionMode(const std::string &mode);
bool AllowsManualActions(const std::string &mode);
bool ShouldQueueLocalPlayerSpeech(const std::string &requestMode);
bool AllowsAutomaticRechat(const std::string &requestMode);
std::string EventTypeForRequest(const std::string &requestMode);

} // namespace ChatMode
} // namespace Stobe
