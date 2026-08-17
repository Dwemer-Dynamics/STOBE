#include "DirectorRuntime.h"

#include "Functions.h"
#include "Globals.h"
#include "Utils.h"

#include <cmath>
#include <cstring>
#include <cstdlib>
#if defined(_MSC_VER) && _MSC_VER < 1800
#include <float.h>
#endif
#include <kenshi/Character.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/SaveManager.h>
#include <ogre/OgreVector3.h>
#include <string>
#include <windows.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace Stobe {
namespace Director {
namespace {

const size_t kMaxScriptBytes = 24 * 1024;
const size_t kMaxLuaBytes = 8 * 1024 * 1024;
const int kInstructionLimit = 250000;
const int kInstructionHookStep = 1000;

struct LuaMemoryBudget {
  size_t used;
  size_t limit;

  LuaMemoryBudget() : used(0), limit(kMaxLuaBytes) {}
};

struct PendingScript {
  std::string requestId;
  std::string summary;
  std::string source;
  bool queued;
  bool mutating;

  PendingScript() : queued(false), mutating(false) {}
};

PendingScript g_pending;
GameWorld *g_activeWorld = nullptr;
Character *g_activeSelected = nullptr;
int g_instructionCount = 0;

void *LuaAllocate(void *userData, void *pointer, size_t oldSize,
                  size_t newSize) {
  LuaMemoryBudget *budget = static_cast<LuaMemoryBudget *>(userData);
  if (newSize == 0) {
    if (pointer) {
      std::free(pointer);
      budget->used = oldSize <= budget->used ? budget->used - oldSize : 0;
    }
    return nullptr;
  }

  size_t nextUsed = budget->used;
  if (pointer) {
    nextUsed = oldSize <= nextUsed ? nextUsed - oldSize : 0;
  }
  if (newSize > budget->limit || nextUsed > budget->limit - newSize) {
    return nullptr;
  }

  void *next = std::realloc(pointer, newSize);
  if (next) {
    budget->used = nextUsed + newSize;
  }
  return next;
}

void InstructionHook(lua_State *state, lua_Debug *) {
  g_instructionCount += kInstructionHookStep;
  if (g_instructionCount > kInstructionLimit) {
    luaL_error(state, "instruction limit exceeded");
  }
}

bool IsFiniteCoordinate(lua_Number value) {
  const double number = static_cast<double>(value);
#if defined(_MSC_VER) && _MSC_VER < 1800
  const bool finite = _finite(number) != 0;
#else
  const bool finite = std::isfinite(number);
#endif
  return finite &&
         std::fabs(static_cast<double>(value)) <= 10000000.0;
}

Character *ResolvePlayerCharacter(unsigned int serial) {
  if (!g_activeWorld || !g_activeWorld->player || serial == 0) {
    return nullptr;
  }
  const lektor<Character *> &characters =
      g_activeWorld->player->playerCharacters;
  for (size_t i = 0; i < characters.size(); ++i) {
    Character *character = characters[i];
    if (!character || reinterpret_cast<uintptr_t>(character) <= 0x1000) {
      continue;
    }
    try {
      if (character->getHandle().serial == serial) {
        return character;
      }
    } catch (...) {
    }
  }
  return nullptr;
}

Character *RequirePlayerCharacter(lua_State *state, int argument) {
  lua_Integer rawSerial = luaL_checkinteger(state, argument);
  if (rawSerial <= 0 || rawSerial > 0xFFFFFFFFLL) {
    luaL_error(state, "invalid player character serial");
  }
  Character *character =
      ResolvePlayerCharacter(static_cast<unsigned int>(rawSerial));
  if (!character) {
    luaL_error(state, "player character is no longer available");
  }
  return character;
}

Ogre::Vector3 ReadPosition(lua_State *state, int firstArgument) {
  lua_Number x = luaL_checknumber(state, firstArgument);
  lua_Number y = luaL_checknumber(state, firstArgument + 1);
  lua_Number z = luaL_checknumber(state, firstArgument + 2);
  if (!IsFiniteCoordinate(x) || !IsFiniteCoordinate(y) ||
      !IsFiniteCoordinate(z)) {
    luaL_error(state, "position is outside the supported coordinate range");
  }
  return Ogre::Vector3(static_cast<float>(x), static_cast<float>(y),
                       static_cast<float>(z));
}

bool TryNotify(GameWorld *world, const char *message, size_t length) {
  try {
    world->showPlayerAMessage_withLog(std::string(message, length), true);
    return true;
  } catch (...) {
    return false;
  }
}

bool TrySave(SaveManager *manager, const char *name, size_t length) {
  try {
    manager->save(std::string(name, length), false);
    return true;
  } catch (...) {
    return false;
  }
}

bool TrySetGameSpeed(GameWorld *world, float speed) {
  try {
    world->setGameSpeed(speed, false);
    return true;
  } catch (...) {
    return false;
  }
}

bool TryTeleport(Character *character, const Ogre::Vector3 &target) {
  try {
    const Ogre::Vector3 current = character->getPosition();
    character->teleport(target - current);
    return true;
  } catch (...) {
    return false;
  }
}

bool TryMove(Character *character, const Ogre::Vector3 &target) {
  try {
    character->setDestination(target, false);
    return true;
  } catch (...) {
    return false;
  }
}

bool IsPlayerCharacterSafe(Character *character) {
  try {
    return character && character->isPlayerCharacter();
  } catch (...) {
    return false;
  }
}

int LuaNotify(lua_State *state) {
  const char *message = luaL_checkstring(state, 1);
  if (!g_activeWorld) {
    return luaL_error(state, "world is unavailable");
  }
  size_t length = message ? std::strlen(message) : 0;
  if (length == 0 || length > 500) {
    return luaL_error(state, "notification must contain 1-500 characters");
  }
  if (!TryNotify(g_activeWorld, message, length)) {
    return luaL_error(state, "notification failed");
  }
  return 0;
}

int LuaWorldSummary(lua_State *state) {
  if (!g_activeWorld || !g_activeWorld->player) {
    return luaL_error(state, "world is unavailable");
  }
  lua_newtable(state);
  lua_pushboolean(state, g_activeWorld->isPaused());
  lua_setfield(state, -2, "paused");
  lua_pushinteger(state,
                  static_cast<lua_Integer>(
                      g_activeWorld->player->playerCharacters.size()));
  lua_setfield(state, -2, "player_character_count");
  if (g_activeSelected &&
      reinterpret_cast<uintptr_t>(g_activeSelected) > 0x1000) {
    try {
      lua_pushinteger(state, g_activeSelected->getHandle().serial);
      lua_setfield(state, -2, "selected_serial");
      lua_pushstring(state, g_activeSelected->getName().c_str());
      lua_setfield(state, -2, "selected_name");
    } catch (...) {
    }
  }
  return 1;
}

void PushCharacter(lua_State *state, Character *character) {
  lua_newtable(state);
  try {
    const Ogre::Vector3 position = character->getPosition();
    lua_pushinteger(state, character->getHandle().serial);
    lua_setfield(state, -2, "serial");
    lua_pushstring(state, character->getName().c_str());
    lua_setfield(state, -2, "name");
    lua_pushnumber(state, position.x);
    lua_setfield(state, -2, "x");
    lua_pushnumber(state, position.y);
    lua_setfield(state, -2, "y");
    lua_pushnumber(state, position.z);
    lua_setfield(state, -2, "z");
    lua_pushboolean(state, character == g_activeSelected);
    lua_setfield(state, -2, "selected");
    lua_pushboolean(state, character->isDead());
    lua_setfield(state, -2, "dead");
    lua_pushboolean(state, character->isUnconcious());
    lua_setfield(state, -2, "unconscious");
  } catch (...) {
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "available");
  }
}

int LuaPlayerCharacters(lua_State *state) {
  if (!g_activeWorld || !g_activeWorld->player) {
    return luaL_error(state, "world is unavailable");
  }
  lua_newtable(state);
  const lektor<Character *> &characters =
      g_activeWorld->player->playerCharacters;
  int outputIndex = 1;
  for (size_t i = 0; i < characters.size(); ++i) {
    Character *character = characters[i];
    if (!character || reinterpret_cast<uintptr_t>(character) <= 0x1000) {
      continue;
    }
    PushCharacter(state, character);
    lua_rawseti(state, -2, outputIndex++);
  }
  return 1;
}

int LuaSave(lua_State *state) {
  const char *rawName = luaL_optstring(state, 1, "STOBE Director Recovery");
  size_t nameLength = rawName ? std::strlen(rawName) : 0;
  if (nameLength == 0 || nameLength > 80 ||
      std::strpbrk(rawName, "\\/:*?\"<>|") != nullptr) {
    return luaL_error(state, "save name is invalid");
  }
  SaveManager *manager = SaveManager::getSingleton();
  if (!manager) {
    return luaL_error(state, "save manager is unavailable");
  }
  if (!TrySave(manager, rawName, nameLength)) {
    return luaL_error(state, "save failed");
  }
  return 0;
}

int LuaSetGameSpeed(lua_State *state) {
  if (!g_activeWorld) {
    return luaL_error(state, "world is unavailable");
  }
  lua_Number speed = luaL_checknumber(state, 1);
  if (!IsFiniteCoordinate(speed) || speed < 0.0 || speed > 5.0) {
    return luaL_error(state, "game speed must be between 0 and 5");
  }
  if (!TrySetGameSpeed(g_activeWorld, static_cast<float>(speed))) {
    return luaL_error(state, "game speed change failed");
  }
  return 0;
}

int LuaTeleport(lua_State *state) {
  Character *character = RequirePlayerCharacter(state, 1);
  if (!TryTeleport(character, ReadPosition(state, 2))) {
    return luaL_error(state, "teleport failed");
  }
  return 0;
}

int LuaTeleportSelected(lua_State *state) {
  if (!IsPlayerCharacterSafe(g_activeSelected)) {
    return luaL_error(state, "selected character is not a player character");
  }
  if (!TryTeleport(g_activeSelected, ReadPosition(state, 1))) {
    return luaL_error(state, "teleport failed");
  }
  return 0;
}

int LuaMoveTo(lua_State *state) {
  Character *character = RequirePlayerCharacter(state, 1);
  if (!TryMove(character, ReadPosition(state, 2))) {
    return luaL_error(state, "movement order failed");
  }
  return 0;
}

int LuaMoveSelected(lua_State *state) {
  if (!IsPlayerCharacterSafe(g_activeSelected)) {
    return luaL_error(state, "selected character is not a player character");
  }
  if (!TryMove(g_activeSelected, ReadPosition(state, 1))) {
    return luaL_error(state, "movement order failed");
  }
  return 0;
}

void RegisterFunction(lua_State *state, const char *name,
                      lua_CFunction function) {
  lua_pushcfunction(state, function);
  lua_setfield(state, -2, name);
}

lua_State *CreateSandbox(LuaMemoryBudget &budget) {
  lua_State *state = lua_newstate(LuaAllocate, &budget);
  if (!state) {
    return nullptr;
  }
  luaL_requiref(state, "_G", luaopen_base, 1);
  lua_pop(state, 1);
  luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(state, 1);
  luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(state, 1);
  luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(state, 1);
  luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  lua_pop(state, 1);

  const char *blockedBaseFunctions[] = {"collectgarbage", "dofile", "load",
                                        "loadfile"};
  for (size_t i = 0;
       i < sizeof(blockedBaseFunctions) / sizeof(blockedBaseFunctions[0]);
       ++i) {
    lua_pushnil(state);
    lua_setglobal(state, blockedBaseFunctions[i]);
  }

  lua_newtable(state);
  RegisterFunction(state, "notify", LuaNotify);
  RegisterFunction(state, "world_summary", LuaWorldSummary);
  RegisterFunction(state, "player_characters", LuaPlayerCharacters);
  RegisterFunction(state, "save", LuaSave);
  RegisterFunction(state, "set_game_speed", LuaSetGameSpeed);
  RegisterFunction(state, "teleport", LuaTeleport);
  RegisterFunction(state, "teleport_selected", LuaTeleportSelected);
  RegisterFunction(state, "move_to", LuaMoveTo);
  RegisterFunction(state, "move_selected", LuaMoveSelected);
  lua_setglobal(state, "kenshi");
  return state;
}

bool LoadScript(lua_State *state, const std::string &script,
                std::string &errorOut) {
  int status = luaL_loadbuffer(state, script.c_str(), script.length(),
                               "stobe_director");
  if (status != LUA_OK) {
    const char *message = lua_tostring(state, -1);
    errorOut = message ? message : "Lua compilation failed";
    return false;
  }
  return true;
}

} // namespace

const char *ApiManifest() {
  return
      "Lua 5.4 sandbox. The only game API is the global kenshi table. "
      "Available calls:\n"
      "kenshi.world_summary() -> {paused, player_character_count, "
      "selected_serial?, selected_name?}\n"
      "kenshi.player_characters() -> array of {serial,name,x,y,z,selected,"
      "dead,unconscious}\n"
      "kenshi.notify(message)\n"
      "kenshi.save(optional_name)\n"
      "kenshi.set_game_speed(speed_0_to_5)\n"
      "kenshi.teleport(player_serial, x, y, z)\n"
      "kenshi.teleport_selected(x, y, z)\n"
      "kenshi.move_to(player_serial, x, y, z)\n"
      "kenshi.move_selected(x, y, z)\n"
      "Capabilities: inspect, notify, save, time, teleport, movement. "
      "No file, process, network, package, debug, native module, raw pointer, "
      "or arbitrary memory API is available.";
}

bool ValidateScript(const std::string &script, std::string &errorOut) {
  errorOut.clear();
  if (script.empty()) {
    errorOut = "The generated script is empty.";
    return false;
  }
  if (script.length() > kMaxScriptBytes) {
    errorOut = "The generated script exceeds the 24 KiB limit.";
    return false;
  }
  LuaMemoryBudget budget;
  lua_State *state = CreateSandbox(budget);
  if (!state) {
    errorOut = "Unable to create the Lua sandbox.";
    return false;
  }
  bool valid = LoadScript(state, script, errorOut);
  lua_close(state);
  return valid;
}

bool QueueScript(const std::string &requestId, const std::string &summary,
                 const std::string &script, bool mutating,
                 std::string &errorOut) {
  if (!ValidateScript(script, errorOut)) {
    return false;
  }
  g_pending.requestId = requestId;
  g_pending.summary = summary;
  g_pending.source = script;
  g_pending.mutating = mutating;
  g_pending.queued = true;
  return true;
}

void Update(GameWorld *world, Character *selectedCharacter) {
  if (!g_pending.queued || !world) {
    return;
  }

  PendingScript script = g_pending;
  g_pending = PendingScript();
  if (script.mutating) {
    SaveManager *manager = SaveManager::getSingleton();
    if (!manager) {
      world->showPlayerAMessage_withLog(
          "STOBE Director stopped because it could not create a recovery save.",
          true);
      return;
    }
    try {
      manager->save("STOBE Director Recovery", false);
      Log("DIRECTOR: recovery save requested before mutating script");
    } catch (...) {
      world->showPlayerAMessage_withLog(
          "STOBE Director stopped because its recovery save failed.", true);
      return;
    }
  }
  LuaMemoryBudget budget;
  lua_State *state = CreateSandbox(budget);
  if (!state) {
    world->showPlayerAMessage_withLog("STOBE Director could not create its Lua sandbox.", true);
    return;
  }

  std::string error;
  if (!LoadScript(state, script.source, error)) {
    lua_close(state);
    world->showPlayerAMessage_withLog("STOBE Director rejected the script: " + error,
                                      true);
    return;
  }

  g_activeWorld = world;
  g_activeSelected = selectedCharacter;
  g_instructionCount = 0;
  lua_sethook(state, InstructionHook, LUA_MASKCOUNT, kInstructionHookStep);
  Log("DIRECTOR: executing request=" + script.requestId + " summary=" +
      script.summary);
  int status = lua_pcall(state, 0, 0, 0);
  lua_sethook(state, nullptr, 0, 0);
  g_activeWorld = nullptr;
  g_activeSelected = nullptr;

  if (status != LUA_OK) {
    const char *rawError = lua_tostring(state, -1);
    error = rawError ? rawError : "unknown Lua error";
    Log("DIRECTOR_ERROR: request=" + script.requestId + " error=" + error);
    world->showPlayerAMessage_withLog("STOBE Director stopped: " + error, true);
  } else {
    Log("DIRECTOR: completed request=" + script.requestId);
    world->showPlayerAMessage_withLog("STOBE Director completed: " +
                                          script.summary,
                                      true);
  }
  lua_close(state);
}

void Reset(const std::string &reason) {
  if (g_pending.queued) {
    Log("DIRECTOR: cleared queued script reason=" + reason);
  }
  g_pending = PendingScript();
  g_activeWorld = nullptr;
  g_activeSelected = nullptr;
}

} // namespace Director
} // namespace Stobe
