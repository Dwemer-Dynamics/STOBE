#pragma once

#include <string>

class Character;
class GameWorld;

namespace Stobe {
namespace Director {

const char *ApiManifest();
bool ValidateScript(const std::string &script, std::string &errorOut);
bool QueueScript(const std::string &requestId, const std::string &summary,
                 const std::string &script, bool mutating,
                 std::string &errorOut);
void Update(GameWorld *world, Character *selectedCharacter);
void Reset(const std::string &reason);

} // namespace Director
} // namespace Stobe
