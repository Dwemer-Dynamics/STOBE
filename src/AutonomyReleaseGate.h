#pragma once

namespace Stobe {
namespace AutonomyReleaseGate {

// Keep unfinished autonomy code compiled but unreachable in public beta builds.
static const bool kEnabled = false;

} // namespace AutonomyReleaseGate
} // namespace Stobe
