#include "StobeTiming.h"

namespace Stobe {
namespace Timing {

std::uint32_t ResolveRechatDispatchDelayMs(std::uint32_t lineDelayMs) {
  (void)lineDelayMs;
  // Rechat should prefetch the next response immediately; playback ordering is
  // already enforced by the action/TTS queue.
  return 0;
}

bool ShouldWaitForPlaybackBeforeRechatDispatch() { return false; }

} // namespace Timing
} // namespace Stobe
