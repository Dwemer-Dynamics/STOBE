#pragma once

namespace Stobe {
namespace DialogueMenuTts {

// Polls Kenshi's native dialogue window and manages pre-generation/playback.
void Update();
void Reset(const char *reason);
void NotifySelection();

} // namespace DialogueMenuTts
} // namespace Stobe
