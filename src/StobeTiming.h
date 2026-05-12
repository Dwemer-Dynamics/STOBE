#pragma once

#include <cstdint>

namespace Stobe {
namespace Timing {

std::uint32_t ResolveRechatDispatchDelayMs(std::uint32_t lineDelayMs);
bool ShouldWaitForPlaybackBeforeRechatDispatch();

} // namespace Timing
} // namespace Stobe
