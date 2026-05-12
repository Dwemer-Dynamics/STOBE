#pragma once

#include <cstdint>
#include <string>

namespace Stobe {
namespace IdentityRename {

enum BatchStatus {
  BATCH_STATUS_COMPLETE = 0,
  BATCH_STATUS_RENAME = 1,
  BATCH_STATUS_RETRY = 2,
};

bool IsQueueEligibleName(const std::string &name);
bool IsAttemptReady(std::uint32_t nowTick, std::uint32_t nextAttemptTick);
std::uint32_t ResolveQueuedAttemptHoldMs();
std::uint32_t ResolveRetryDelayMs();
std::uint32_t ResolveQueuedAttemptDeadline(std::uint32_t nowTick);
std::uint32_t ResolveRetryAttemptDeadline(std::uint32_t nowTick);
BatchStatus ParseBatchStatus(const std::string &status);

} // namespace IdentityRename
} // namespace Stobe
