#pragma once

#include <string>

// Game-independent parsing shared by the addon runtime and server package
// sync. Kept free of Windows and KenshiLib types for portable tests.
namespace Stobe {
namespace AddonProtocol {

struct ExtCommand {
  std::string command; // Full command as received, original case.
  std::string bridge;
  std::string action;
};

// Accepts ExtCmd<Bridge>_<Action> (prefix case-insensitive). The bridge is a
// valid name without '_', the action is non-empty.
bool ParseExtCommand(const std::string &rawCommand, ExtCommand &out);
bool IsExtCommand(const std::string &rawCommand);

// [A-Za-z0-9] only, 1..48 bytes. Used for bridge names.
bool IsValidBridgeName(const std::string &value);
// [A-Za-z0-9_.-], 1..48 bytes, not starting with '.' or '-'.
bool IsValidToken(const std::string &value);
std::string LowerAscii(const std::string &value);

// Matches the server package manager's plugin-name and version rules.
bool IsSafePackageName(const std::string &value);
bool IsSafePackageVersion(const std::string &value);
// ".dwpkg" or ".zip" (case-insensitive); returns the version stem.
bool SplitPackageArchiveName(const std::string &fileName, std::string &stemOut);

// Minimal readers for the flat package API responses. They return the first
// occurrence of the key anywhere in the body.
std::string JsonStringValue(const std::string &body, const std::string &key);
bool JsonBooleanValue(const std::string &body, const std::string &key,
                      bool fallback);
std::string JsonEscape(const std::string &value);

} // namespace AddonProtocol
} // namespace Stobe
