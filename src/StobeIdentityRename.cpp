#include "StobeIdentityRename.h"

#include <algorithm>
#include <cctype>

namespace {

std::string TrimCopy(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.pop_back();
  }
  return value;
}

std::string ToLowerAsciiCopy(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char ch) -> char { return static_cast<char>(std::tolower(ch)); });
  return value;
}

} // namespace

namespace Stobe {
namespace IdentityRename {

bool IsQueueEligibleName(const std::string &name) {
  std::string trimmed = TrimCopy(name);
  if (trimmed.empty()) {
    return false;
  }

  if (trimmed.find('[') != std::string::npos ||
      trimmed.find(']') != std::string::npos) {
    return false;
  }

  return ToLowerAsciiCopy(trimmed) != "unknown";
}

bool IsAttemptReady(std::uint32_t nowTick, std::uint32_t nextAttemptTick) {
  if (nextAttemptTick == 0) {
    return true;
  }
  return static_cast<std::int32_t>(nowTick - nextAttemptTick) >= 0;
}

std::uint32_t ResolveQueuedAttemptHoldMs() { return 20000; }

std::uint32_t ResolveRetryDelayMs() { return 30000; }

std::uint32_t ResolveQueuedAttemptDeadline(std::uint32_t nowTick) {
  return nowTick + ResolveQueuedAttemptHoldMs();
}

std::uint32_t ResolveRetryAttemptDeadline(std::uint32_t nowTick) {
  return nowTick + ResolveRetryDelayMs();
}

BatchStatus ParseBatchStatus(const std::string &status) {
  const std::string normalized = ToLowerAsciiCopy(TrimCopy(status));
  if (normalized == "rename") {
    return BATCH_STATUS_RENAME;
  }
  if (normalized == "ok" || normalized == "complete" || normalized == "noop") {
    return BATCH_STATUS_COMPLETE;
  }
  return BATCH_STATUS_RETRY;
}

} // namespace IdentityRename
} // namespace Stobe
