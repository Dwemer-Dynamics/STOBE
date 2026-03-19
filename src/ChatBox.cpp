#include "ChatBox.h"
#include "AudioPlayback.h"
#include "Comm.h"
#include "Context.h"
#include "Functions.h"
#include "Globals.h"
#include "Utils.h"

#include <kenshi/Character.h>
#include <kenshi/Faction.h>
#include <kenshi/GameData.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Item.h>
#include <kenshi/Kenshi.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RaceData.h>
#include <kenshi/RootObject.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/util/OgreUnordered.h>
#include <kenshi/util/hand.h>

#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_ComboBox.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_InputManager.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Window.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <set>

namespace Stobe {
namespace UI {

MyGUI::Window *g_chatWindow = nullptr;
MyGUI::EditBox *g_chatInput = nullptr;
MyGUI::ComboBox *g_chatModeCombo = nullptr;
MyGUI::ComboBox *g_chatActionCombo = nullptr;
MyGUI::EditBox *g_chatActionArgInput = nullptr;
MyGUI::Button *g_chatAutoChatToggle = nullptr;
MyGUI::TextBox *g_chatLabel = nullptr;
std::string g_chatTargetHandleStr = "";
std::string g_chatTargetNameStr = "";
std::string g_chatPlayerNameStr = "";
size_t g_lastChatModeIndex = 1;
bool g_chatJustOpened = false;
bool g_chatPausedGame = false;
const float kWhisperRangeUnits = 20.0f;

enum ManualChatActionType {
  MANUAL_CHAT_ACTION_NONE = 0,
  MANUAL_CHAT_ACTION_REMOVE_LIMB = 1,
  MANUAL_CHAT_ACTION_GIVE_CATS = 2,
  MANUAL_CHAT_ACTION_GIVE_ITEM = 3,
  MANUAL_CHAT_ACTION_ROLEPLAY_ACTION = 4,
  MANUAL_CHAT_ACTION_DRINK_ITEM = 5,
  MANUAL_CHAT_ACTION_USE_DRUGS = 6,
  MANUAL_CHAT_ACTION_KILL = 7
};

struct ManualChatActionChoice {
  ManualChatActionType type;
  const char *label;
  const char *manualActionKey;
  const char *limbToken;
  bool requiresAmount;
};

const ManualChatActionChoice kManualChatActionChoices[] = {
    {MANUAL_CHAT_ACTION_NONE, "none", "", "", false},
    {MANUAL_CHAT_ACTION_GIVE_CATS, "give cats", "", "", true},
    {MANUAL_CHAT_ACTION_GIVE_ITEM, "give item", "", "", false},
    {MANUAL_CHAT_ACTION_ROLEPLAY_ACTION, "roleplay action", "", "", false},
    {MANUAL_CHAT_ACTION_DRINK_ITEM, "drink item", "", "", false},
    {MANUAL_CHAT_ACTION_USE_DRUGS, "use drugs", "", "", false},
    {MANUAL_CHAT_ACTION_REMOVE_LIMB, "remove left arm (hacksaw)",
     "remove_limb_left_arm", "LEFT_ARM", false},
    {MANUAL_CHAT_ACTION_REMOVE_LIMB, "remove right arm (hacksaw)",
     "remove_limb_right_arm", "RIGHT_ARM", false},
    {MANUAL_CHAT_ACTION_REMOVE_LIMB, "remove left leg (hacksaw)",
     "remove_limb_left_leg", "LEFT_LEG", false},
    {MANUAL_CHAT_ACTION_REMOVE_LIMB, "remove right leg (hacksaw)",
     "remove_limb_right_leg", "RIGHT_LEG", false},
    {MANUAL_CHAT_ACTION_KILL, "kill", "kill", "", false},
};

size_t ManualChatActionChoiceCount() {
  return sizeof(kManualChatActionChoices) / sizeof(kManualChatActionChoices[0]);
}

size_t SanitizeManualChatActionIndex(size_t index) {
  if (index >= ManualChatActionChoiceCount()) {
    return 0;
  }
  return index;
}

const ManualChatActionChoice &GetManualChatActionChoice(size_t index) {
  return kManualChatActionChoices[SanitizeManualChatActionIndex(index)];
}

std::string ResolveCharacterSerialToken(Character *character) {
  if (!character || (uintptr_t)character <= 0x1000) {
    return "";
  }
  try {
    unsigned int serial = character->getHandle().serial;
    if (serial == 0) {
      return "";
    }
    return ToString(serial);
  } catch (...) {
    return "";
  }
}

std::string BuildManualRemoveLimbActionToken(const std::string &targetName,
                                             const std::string &targetHandle,
                                             const std::string &limbToken) {
  std::string normalizedTargetName = targetName;
  std::string normalizedLimbToken = limbToken;
  if (normalizedTargetName.empty() || normalizedLimbToken.empty()) {
    return "";
  }
  std::string targetToken = normalizedTargetName;
  if (!targetHandle.empty()) {
    targetToken += "|" + targetHandle;
  }
  return "REMOVE_LIMB@" + targetToken + "@" + normalizedLimbToken;
}

std::string BuildManualGiveCatsActionToken(const std::string &targetName,
                                           const std::string &targetHandle,
                                           int catsAmount) {
  std::string normalizedTargetName = targetName;
  if (normalizedTargetName.empty() || catsAmount <= 0) {
    return "";
  }
  std::string targetToken = normalizedTargetName;
  if (!targetHandle.empty()) {
    targetToken += "|" + targetHandle;
  }
  return "GIVE_CATS@" + targetToken + "@" + ToString(catsAmount);
}

std::string TrimManualActionArg(const std::string &rawValue) {
  std::string value = rawValue;
  size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string BuildManualGiveItemActionToken(const std::string &targetName,
                                           const std::string &targetHandle,
                                           const std::string &itemName) {
  std::string normalizedTargetName = TrimManualActionArg(targetName);
  std::string normalizedItemName = TrimManualActionArg(itemName);
  if (normalizedTargetName.empty() || normalizedItemName.empty()) {
    return "";
  }
  std::string targetToken = normalizedTargetName;
  if (!targetHandle.empty()) {
    targetToken += "|" + targetHandle;
  }
  return "GIVE_ITEM@" + targetToken + "@" + normalizedItemName;
}

std::string BuildManualRoleplayActionToken(const std::string &noticeText) {
  std::string normalizedNotice = TrimManualActionArg(noticeText);
  if (normalizedNotice.empty()) {
    return "";
  }
  return "ROLEPLAY_ACTION@" + normalizedNotice;
}

std::string BuildManualDrinkItemActionToken(const std::string &itemName) {
  std::string normalizedItem = TrimManualActionArg(itemName);
  if (normalizedItem.empty()) {
    return "";
  }
  return "DRINK_ITEM@" + normalizedItem;
}

std::string BuildManualUseDrugsActionToken(const std::string &itemName) {
  std::string normalizedItem = TrimManualActionArg(itemName);
  if (normalizedItem.empty()) {
    return "";
  }
  return "USE_DRUGS@" + normalizedItem;
}

std::string BuildManualKillActionToken(const std::string &targetName,
                                       const std::string &targetHandle) {
  std::string normalizedTargetName = TrimManualActionArg(targetName);
  if (normalizedTargetName.empty()) {
    return "";
  }
  std::string targetToken = normalizedTargetName;
  if (!targetHandle.empty()) {
    targetToken += "|" + targetHandle;
  }
  return "KILL@" + targetToken;
}

bool TryParseActionAmount(const std::string &rawValue, int &amountOut) {
  amountOut = 0;
  std::string value = rawValue;
  size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return false;
  }
  size_t last = value.find_last_not_of(" \t\r\n");
  value = value.substr(first, last - first + 1);
  if (value.empty()) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    unsigned char c = (unsigned char)value[i];
    if (c < '0' || c > '9') {
      return false;
    }
  }
  int amount = atoi(value.c_str());
  if (amount <= 0) {
    return false;
  }
  amountOut = amount;
  return true;
}

int ResolveSpeakerCatsForTransfer(Character *speaker) {
  if (!speaker || (uintptr_t)speaker <= 0x1000) {
    return 0;
  }
  int cats = 0;
  try {
    cats = speaker->getMoney();
  } catch (...) {
    cats = 0;
  }
  if (cats < 0) {
    cats = 0;
  }
  return cats;
}

std::string NormalizeManualItemQuery(const std::string &rawValue) {
  std::string value = rawValue;
  size_t first = value.find_first_not_of(" \t\n\r\"'");
  if (first == std::string::npos) {
    return "";
  }
  value.erase(0, first);
  size_t last = value.find_last_not_of(" \t\n\r\"'");
  if (last != std::string::npos) {
    value.erase(last + 1);
  } else {
    value.clear();
  }
  std::transform(value.begin(), value.end(), value.begin(), ::tolower);
  return value;
}

bool ResolveSpeakerGiveItemMatch(Character *speaker, const std::string &rawQuery,
                                 std::string &matchedNameOut) {
  matchedNameOut.clear();
  if (!speaker || (uintptr_t)speaker <= 0x1000) {
    return false;
  }
  std::string query = NormalizeManualItemQuery(rawQuery);
  if (query.empty()) {
    return false;
  }
  std::vector<Item *> items;
  try {
    GetAllCharacterItems(speaker, items);
  } catch (...) {
    return false;
  }
  for (size_t i = 0; i < items.size(); ++i) {
    Item *item = items[i];
    if (!item || (uintptr_t)item <= 0x1000) {
      continue;
    }
    std::string itemName = "";
    try {
      itemName = item->getName();
    } catch (...) {
      itemName = "";
    }
    if (itemName.empty()) {
      continue;
    }
    std::string itemNameNormalized = itemName;
    std::transform(itemNameNormalized.begin(), itemNameNormalized.end(),
                   itemNameNormalized.begin(), ::tolower);
    if (itemNameNormalized.find(query) != std::string::npos) {
      matchedNameOut = itemName;
      return true;
    }
  }
  return false;
}

size_t ChatModeToIndex(const std::string &mode) {
  if (mode == "whisper")
    return 1;
  if (mode == "shout")
    return 2;
  if (mode == "cheat")
    return 3;
  return 0; // chat
}

std::string NormalizeChatMode(const std::string &mode) {
  std::string normalized = mode;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 ::tolower);
  if (normalized == "whisper")
    return "whisper";
  if (normalized == "shout")
    return "shout";
  if (normalized == "cheat")
    return "cheat";
  if (normalized == "talk" || normalized == "chat")
    return "chat";
  return "chat";
}

bool EqualsIgnoreCase(const std::string &lhs, const std::string &rhs) {
  if (lhs.length() != rhs.length()) {
    return false;
  }
  for (size_t i = 0; i < lhs.length(); ++i) {
    unsigned char a = static_cast<unsigned char>(lhs[i]);
    unsigned char b = static_cast<unsigned char>(rhs[i]);
    if (std::tolower(a) != std::tolower(b)) {
      return false;
    }
  }
  return true;
}

bool IsAnimalCharacterSafe(Character *character, unsigned int *serialOut) {
  if (serialOut) {
    *serialOut = 0;
  }
  if (!character || (uintptr_t)character <= 0x1000) {
    return false;
  }
  try {
    if (serialOut) {
      *serialOut = character->getHandle().serial;
    }
    return character->isAnimal() != 0;
  } catch (...) {
    return false;
  }
}

bool ShouldIncludeAnimalForTalk(Character *character) {
  unsigned int serial = 0;
  if (!IsAnimalCharacterSafe(character, &serial)) {
    return true;
  }
  if (!g_enableAnimalTalks || serial == 0) {
    return false;
  }
  return IsAnimalActivated(serial);
}

bool IsCharacterUnavailableForConversation(Character *character) {
  if (!character || (uintptr_t)character <= 0x1000) {
    return true;
  }
  try {
    return character->isDead() || character->isUnconcious();
  } catch (...) {
    return true;
  }
}

float GetSearchRadiusForMode(const std::string &mode) {
  if (mode == "whisper")
    return kWhisperRangeUnits;
  if (mode == "shout")
    return g_shoutRadius;
  return g_proximityRadius;
}

bool IsIndoorsHandleValid(const hand &indoorsHandle) {
  return indoorsHandle.isValid() && !indoorsHandle.isNull();
}

bool TryGetSpatialState(Character *character, bool &hasBuilding,
                        unsigned int &buildingSerial, int &floorValue) {
  hasBuilding = false;
  buildingSerial = 0;
  floorValue = 0;
  if (!character || (uintptr_t)character <= 0x1000) {
    return false;
  }
#if defined(_MSC_VER)
  __try {
#endif
    const hand &indoorsHandle = character->isIndoors();
    // Only treat true indoor occupancy as "same building" gating. Using
    // standing-on-building outdoors can misclassify terrain/platform handles.
    hasBuilding = IsIndoorsHandleValid(indoorsHandle);
    buildingSerial = hasBuilding ? indoorsHandle.serial : 0;
    floorValue = character->getFloor();
    return true;
#if defined(_MSC_VER)
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#endif
}

bool IsConversationAreaCompatible(Character *anchor, Character *candidate) {
  if (!anchor || !candidate || (uintptr_t)anchor <= 0x1000 ||
      (uintptr_t)candidate <= 0x1000) {
    return false;
  }

  bool anchorHasBuilding = false;
  bool candidateHasBuilding = false;
  unsigned int anchorBuildingSerial = 0;
  unsigned int candidateBuildingSerial = 0;
  int anchorFloor = 0;
  int candidateFloor = 0;
  if (!TryGetSpatialState(anchor, anchorHasBuilding, anchorBuildingSerial,
                          anchorFloor)) {
    return false;
  }
  if (!TryGetSpatialState(candidate, candidateHasBuilding, candidateBuildingSerial,
                          candidateFloor)) {
    return false;
  }

  if (anchorHasBuilding) {
    if (!candidateHasBuilding) {
      return false;
    }
    if (anchorBuildingSerial != 0 && candidateBuildingSerial != 0) {
      if (anchorBuildingSerial != candidateBuildingSerial) {
        return false;
      }
    } else {
      return false;
    }
    int floorDelta = anchorFloor - candidateFloor;
    if (floorDelta < 0) {
      floorDelta = -floorDelta;
    }
    // Indoor/building floor reports can jitter by +/-1 during movement.
    return floorDelta <= 1;
  }

  if (candidateHasBuilding) {
    return false;
  }
  // Outdoors: ignore NPCs on a higher floor/level than the speaker.
  if (candidateFloor > anchorFloor + 1) {
    return false;
  }
  return true;
}

void AppendUniquePerson(std::vector<std::string> &people,
                        const std::string &entry) {
  if (entry.empty())
    return;
  if (std::find(people.begin(), people.end(), entry) != people.end())
    return;
  people.push_back(entry);
}

std::string BuildPeopleJson(GameWorld *world, const std::string &playerName,
                            const std::string &targetName,
                            const std::string &targetHandle,
                            const std::string &mode,
                            Character *speakerOverride = nullptr) {
  std::vector<std::string> people;
  Character *player = nullptr;
  std::string playerHandle = "";

  if (speakerOverride && (uintptr_t)speakerOverride > 0x1000) {
    player = speakerOverride;
  } else if (world && world->player && world->player->playerCharacters.size() > 0) {
    player = world->player->playerCharacters[0];
  }
  if (player) {
    playerHandle = ToString(player->getHandle().serial);
  }

  if (!playerName.empty()) {
    if (!playerHandle.empty()) {
      AppendUniquePerson(people, playerName + "|" + playerHandle);
    } else {
      AppendUniquePerson(people, playerName);
    }
  }

  if (!targetName.empty()) {
    if (!targetHandle.empty())
      AppendUniquePerson(people, targetName + "|" + targetHandle);
    else
      AppendUniquePerson(people, targetName);
  }

  if (player && mode != "whisper") {
    float searchRadius = GetSearchRadiusForMode(mode);
    const hand &playerIndoorsHandle = player->isIndoors();
    bool playerIsIndoors = IsIndoorsHandleValid(playerIndoorsHandle);
    if (!playerIsIndoors) {
      // Outdoors participant discovery should always use talk-range.
      searchRadius = g_proximityRadius;
    }

    // Prefer sphere query over full update-list scan to avoid touching stale
    // character pointers.
    lektor<RootObject *> nearbyResults;
    world->getCharactersWithinSphere(nearbyResults, player->getPosition(),
                                     searchRadius, 0.0f, 0.0f, 0x10, 0, player);

    for (uint32_t i = 0; i < nearbyResults.size(); ++i) {
      Character *other = (Character *)nearbyResults.stuff[i];
      if (!other || (uintptr_t)other <= 0x1000 || other == player) {
        continue;
      }
      if (IsCharacterUnavailableForConversation(other)) {
        continue;
      }
      if (!ShouldIncludeAnimalForTalk(other)) {
        continue;
      }

      std::string otherName = other->getName();
      if (otherName.empty())
        continue;
      if (!targetName.empty() && otherName == targetName)
        continue;
      if (!IsConversationAreaCompatible(player, other))
        continue;

      float dist = player->getPosition().distance(other->getPosition());
      if (dist > searchRadius)
        continue;

      AppendUniquePerson(
          people, otherName + "|" + ToString(other->getHandle().serial));
    }
  }

  std::string peopleJson = "[";
  for (size_t i = 0; i < people.size(); ++i) {
    if (i > 0)
      peopleJson += ",";
    peopleJson += "\"" + EscapeJSON(people[i]) + "\"";
  }
  peopleJson += "]";
  return peopleJson;
}

bool TryParseSerial(const std::string &value, unsigned int &outSerial) {
  outSerial = 0;
  if (value.empty()) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isdigit((unsigned char)value[i])) {
      return false;
    }
  }
  outSerial = (unsigned int)strtoul(value.c_str(), nullptr, 10);
  return outSerial != 0;
}

Character *ResolveChatTargetCharacter(GameWorld *world,
                                      const std::string &targetName,
                                      const std::string &targetHandle) {
  if (!world) {
    return nullptr;
  }

  unsigned int serial = 0;
  bool hasSerial = TryParseSerial(targetHandle, serial);
  auto matchesTarget = [&](Character *candidate) -> bool {
    if (!candidate || (uintptr_t)candidate <= 0x1000) {
      return false;
    }
    if (hasSerial) {
      return candidate->getHandle().serial == serial;
    }
    if (!targetName.empty() && EqualsIgnoreCase(candidate->getName(), targetName)) {
      return true;
    }
    return false;
  };

  // First pass: scan active world characters without distance filtering.
  // Needed for cheat mode and long-range directed actions.
  const ogre_unordered_set<Character *>::type &chars =
      world->getCharacterUpdateList();
  for (auto it = chars.begin(); it != chars.end(); ++it) {
    Character *candidate = *it;
    if (matchesTarget(candidate)) {
      return candidate;
    }
  }

  // Resolve only from a fresh world query each send. Avoid cached hand pointers
  // that can become stale across streaming/unload boundaries.
  if (world->player && world->player->playerCharacters.size() > 0) {
    Character *player = world->player->playerCharacters[0];
    if (player && (uintptr_t)player > 0x1000) {
      lektor<RootObject *> nearbyResults;
      world->getCharactersWithinSphere(nearbyResults, player->getPosition(),
                                       2500.0f, 0.0f, 0.0f, 0x10, 0, player);
      for (uint32_t i = 0; i < nearbyResults.size(); ++i) {
        Character *candidate = (Character *)nearbyResults.stuff[i];
        if (matchesTarget(candidate)) {
          return candidate;
        }
      }
    }
  }

  return nullptr;
}

Character *ResolveNearestPlayerSpeaker(GameWorld *world, Character *target) {
  if (!world || !world->player || world->player->playerCharacters.size() == 0) {
    return nullptr;
  }

  Character *fallback = world->player->playerCharacters[0];
  if (!target || (uintptr_t)target <= 0x1000) {
    return fallback;
  }

  Character *best = nullptr;
  float bestDist = 1e30f;
  for (uint32_t i = 0; i < world->player->playerCharacters.size(); ++i) {
    Character *candidate = world->player->playerCharacters[i];
    if (!candidate || (uintptr_t)candidate <= 0x1000) {
      continue;
    }
    // If the target is a squadmate, pick another squadmate as the speaker.
    if (candidate == target) {
      continue;
    }
    float dist = candidate->getPosition().distance(target->getPosition());
    if (!best || dist < bestDist) {
      best = candidate;
      bestDist = dist;
    }
  }

  return best ? best : fallback;
}

Character *ResolveConfiguredPlayerSpeaker(GameWorld *world, Character *target) {
  if (!world || !world->player || world->player->playerCharacters.size() == 0) {
    return nullptr;
  }
  if (!g_useNearestPlayerSpeaker) {
    return world->player->playerCharacters[0];
  }
  return ResolveNearestPlayerSpeaker(world, target);
}

bool ValidatePlayerChatSend(GameWorld *world, Character *player, Character *target,
                            const std::string &selectedMode,
                            bool requireStrictTalkValidation,
                            std::string &failReason) {
  Log("CHAT_VALIDATE: start");
  if (!world || !player || !target || (uintptr_t)player <= 0x1000 ||
      (uintptr_t)target <= 0x1000) {
    failReason = "Target not available.";
    Log("CHAT_VALIDATE: fail target unavailable");
    return false;
  }

  if (target == player) {
    failReason = "Cannot talk to yourself.";
    Log("CHAT_VALIDATE: fail self-target");
    return false;
  }

  if (!requireStrictTalkValidation) {
    Log("CHAT_VALIDATE: pass manual-action path (strict talk checks skipped)");
    return true;
  }

  if (IsCharacterUnavailableForConversation(target)) {
    failReason = "Target is dead or unconscious.";
    Log("CHAT_VALIDATE: fail target unavailable state");
    return false;
  }

  unsigned int targetAnimalSerial = 0;
  bool targetIsAnimal = IsAnimalCharacterSafe(target, &targetAnimalSerial);
  if (targetIsAnimal) {
    if (!g_enableAnimalTalks) {
      failReason = "Animal Talks is OFF.";
      Log("CHAT_VALIDATE: fail animal talks disabled serial=" +
          ToString(targetAnimalSerial));
      return false;
    }
    if (!IsAnimalActivated(targetAnimalSerial)) {
      MarkAnimalActivated(targetAnimalSerial);
      Log("ANIMAL_TALKS: activated from player send serial=" +
          ToString(targetAnimalSerial));
    }
  }

  if (selectedMode == "cheat") {
    Log("CHAT_VALIDATE: bypass spatial/range checks for cheat mode");
    return true;
  }

  Log("CHAT_VALIDATE: area_check begin");
  bool areaOk = IsConversationAreaCompatible(player, target);
  Log("CHAT_VALIDATE: area_check end ok=" + std::string(areaOk ? "1" : "0"));
  if (!areaOk) {
    failReason =
        "Target is not in the same valid area (building/floor/level).";
    return false;
  }

  float allowedRange = GetSearchRadiusForMode(selectedMode);
  if (allowedRange < 1.0f) {
    allowedRange = 1.0f;
  }
  Log("CHAT_VALIDATE: distance_check begin allowed=" + ToString((int)allowedRange));
  float dist = player->getPosition().distance(target->getPosition());
  Log("CHAT_VALIDATE: distance_check end dist=" + ToString(dist));
  if (dist > allowedRange) {
    failReason = "Target is out of range for " + selectedMode + ".";
    return false;
  }

  Log("CHAT_VALIDATE: pass");
  return true;
}

void CloseChatUI() {
  if (g_chatPausedGame) {
    GameWorld *world = GetWorldSafe();
    if (world) {
      world->userPause(false);
    }
    g_chatPausedGame = false;
  }

  if (g_chatWindow) {
    if (MyGUI::Gui::getInstancePtr())
      MyGUI::Gui::getInstancePtr()->destroyWidget(g_chatWindow);
    g_chatWindow = nullptr;
    g_chatInput = nullptr;
    g_chatModeCombo = nullptr;
    g_chatActionCombo = nullptr;
    g_chatActionArgInput = nullptr;
    g_chatAutoChatToggle = nullptr;
    g_chatLabel = nullptr;
  }
}

DWORD WINAPI DialogResponseWorker(LPVOID lpParam) {
  ChatTask *t = (ChatTask *)lpParam;
  Log("CHAT_THREAD: Sending chat request for " + t->npcName);

  std::string response = PostToStobeWithResponse(L"/chat", t->json);

  if (response.empty()) {
    Log("CHAT_THREAD: Empty response from server.");
    delete t;
    return 0;
  }

  Log("CHAT_THREAD: Got response: " + response.substr(0, 200));

  std::string npcText = JsonReadField(response, "text");
  std::vector<std::string> actions;
  std::string actionsJson = JsonReadField(response, "actions");
  if (!actionsJson.empty() && actionsJson[0] == '[') {
    size_t s = 0;
    while ((s = actionsJson.find("\"", s)) != std::string::npos) {
      s++;
      size_t e = s;
      while (e < actionsJson.size()) {
        if (actionsJson[e] == '\\' && e + 1 < actionsJson.size()) {
          e += 2;
        } else if (actionsJson[e] == '\"') {
          break;
        } else {
          e++;
        }
      }
      if (e < actionsJson.size()) {
        actions.push_back(UnescapeJSON(actionsJson.substr(s, e - s)));
        s = e + 1;
      } else {
        break;
      }
    }
  }

  if (!npcText.empty()) {
    std::stringstream ss(npcText);
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
      if (line.empty())
        continue;

      std::string pipeLine;
      size_t colonPos = line.find(':');
      if (colonPos != std::string::npos && colonPos < 64 && colonPos > 0) {
        std::string speakerName = line.substr(0, colonPos);
        // Trim
        speakerName.erase(0, speakerName.find_first_not_of(" "));
        speakerName.erase(speakerName.find_last_not_of(" ") + 1);

        std::string speech = line.substr(colonPos + 1);
        speech.erase(0, speech.find_first_not_of(" "));

        if (speakerName.find('|') != std::string::npos) {
          pipeLine = "NPC_SAY: " + speakerName + ": " + speech;
        } else if (speakerName == t->npcName) {
          pipeLine =
              "NPC_SAY: " + speakerName + "|" + t->handleStr + ": " + speech;
        } else {
          // Cross-reference nearby NPCs for a handle? For now, let main.cpp
          // resolve by name.
          pipeLine = "NPC_SAY: " + speakerName + ": " + speech;
        }
      } else {
        pipeLine = "NPC_SAY: " + t->npcName + "|" + t->handleStr + ": " + line;
      }

      if (!first)
        SleepIfPaused(g_dialogueSpeedSeconds * 1000);

      EnterCriticalSection(&g_msgMutex);
      g_messageQueue.push_back(pipeLine);
      g_lastDialogueTick = GetTickCount();
      LeaveCriticalSection(&g_msgMutex);
      first = false;
    }
  }

  for (size_t i = 0; i < actions.size(); i++) {
    Sleep(50);
    std::string actLine =
        "NPC_ACTION: " + t->npcName + "|" + t->handleStr + ": " + actions[i];
    EnterCriticalSection(&g_msgMutex);
    g_messageQueue.push_back(actLine);
    LeaveCriticalSection(&g_msgMutex);
  }

  delete t;
  return 0;
}

struct StreamChatTask {
  std::wstring endpoint;
  std::string npcName;
  std::string handleStr;
  std::string peopleJson;
  std::string previousSpeaker;
  std::string previousSpeakerHandle;
  std::string initiatorSpeaker;
  std::string initiatorSpeakerHandle;
  std::string requestMode;
  LONG generation;
  int rechatDepth;
};

struct PlayerTtsTask {
  std::wstring endpoint;
  LONG generation;
  DWORD requestStartTick;
  std::string playerText;
};

struct ManualDiaryTask {
  std::wstring endpoint;
  std::string targetNpcName;
  std::string peopleJson;
  DWORD requestStartTick;
};

DWORD WINAPI StreamChatResponseThread(LPVOID lpParam);

struct StreamChatParseState {
  StreamChatTask *task;
  LONG generation;
  bool firstLine;
  DWORD interLineDelayMs;
  int lineCount;
  int actionCount;
  std::string lastSpeaker;
  std::string lastSpeakerHandle;
  std::string lastSubtitle;
  std::set<std::string> seenActions;
};

std::string TrimChatLine(const std::string &value) {
  if (value.empty())
    return "";
  size_t start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string ShortHashForLog(const std::string &hash) {
  if (hash.length() <= 8) {
    return hash;
  }
  return hash.substr(0, 8);
}

std::string ParseTtsHashToken(const std::string &token) {
  std::string trimmed = TrimChatLine(token);
  if (trimmed.find("tts=") != 0) {
    return "";
  }
  std::string hash = TrimChatLine(trimmed.substr(4));
  if (hash.length() != 32) {
    return "";
  }
  for (size_t i = 0; i < hash.length(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(hash[i]);
    bool isHex =
        (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
        (ch >= 'A' && ch <= 'F');
    if (!isHex) {
      return "";
    }
  }
  return hash;
}

int ParseTtsDurationToken(const std::string &token) {
  std::string trimmed = TrimChatLine(token);
  if (trimmed.find("ttsd=") != 0) {
    return 0;
  }
  std::string durationStr = TrimChatLine(trimmed.substr(5));
  if (durationStr.empty()) {
    return 0;
  }
  for (size_t i = 0; i < durationStr.length(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(durationStr[i]);
    if (ch < '0' || ch > '9') {
      return 0;
    }
  }
  int durationMs = atoi(durationStr.c_str());
  if (durationMs <= 0) {
    return 0;
  }
  // Guard upper bound to avoid absurdly long bubble timers.
  if (durationMs > 600000) {
    return 600000;
  }
  return durationMs;
}

void QueueUiNotifyAction(const std::string &message) {
  std::string text = TrimChatLine(message);
  if (text.empty()) {
    return;
  }
  EnterCriticalSection(&g_uiMutex);
  QueuedAction act;
  act.type = ACT_NOTIFY;
  act.actor = hand();
  act.target = hand();
  act.message = text;
  act.taskValue = 0;
  g_uiActionQueue.push_back(act);
  LeaveCriticalSection(&g_uiMutex);
}

DWORD EstimateLineDelayMsFromText(const std::string &line) {
  std::string text = TrimChatLine(line);
  if (text.empty()) {
    int dialogueSpeedSeconds = g_dialogueSpeedSeconds > 0 ? g_dialogueSpeedSeconds : 1;
    return static_cast<DWORD>(dialogueSpeedSeconds * 1000);
  }

  int words = 0;
  bool inWord = false;
  int punctuationPauseMs = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    unsigned char ch = static_cast<unsigned char>(text[i]);
    bool wordChar = std::isalnum(ch) != 0;
    if (wordChar && !inWord) {
      ++words;
      inWord = true;
    } else if (!wordChar) {
      inWord = false;
    }
    if (ch == '.' || ch == '!' || ch == '?') {
      punctuationPauseMs += 240;
    } else if (ch == ',' || ch == ';' || ch == ':') {
      punctuationPauseMs += 120;
    } else if (ch == '\n' || ch == '\r') {
      punctuationPauseMs += 180;
    }
  }

  int chars = static_cast<int>(text.length());
  int delayMs = (words > 0 ? words * 350 : chars * 55) + punctuationPauseMs + 200;
  if (delayMs < 900) {
    delayMs = 900;
  } else if (delayMs > 45000) {
    delayMs = 45000;
  }
  return static_cast<DWORD>(delayMs);
}

DWORD ResolveLineDelayMs(int ttsDurationMs, const std::string &line = "") {
  DWORD delayMs = 0;
  if (g_ttsEnabled && ttsDurationMs > 0) {
    delayMs = static_cast<DWORD>(ttsDurationMs) + 120;
  } else {
    delayMs = EstimateLineDelayMsFromText(line) + 120;
  }
  if (delayMs < 250) {
    delayMs = 250;
  } else if (delayMs > 600000) {
    delayMs = 600000;
  }
  return delayMs;
}

void QueueChatPipeLine(const std::string &line, LONG generation) {
  if (generation > 0 && !IsChatInterruptGenerationCurrent(generation)) {
    return;
  }
  EnterCriticalSection(&g_msgMutex);
  if (generation > 0 && !IsChatInterruptGenerationCurrent(generation)) {
    LeaveCriticalSection(&g_msgMutex);
    return;
  }
  g_messageQueue.push_back(line);
  g_lastDialogueTick = GetTickCount();
  LeaveCriticalSection(&g_msgMutex);
  if (line.find("PLAYER_SAY: ") == 0 || line.find("PLAYER_TTS: ") == 0 ||
      line.find("NPC_SAY: ") == 0 || line.find("NPC_ACTION: ") == 0) {
    Log("CHAT_TIMING: pipe queued gen=" + ToString((int)generation) +
        " line=" + line.substr(0, std::min<size_t>(line.length(), 120)));
  }
}

int ResolveCurrentGameTs() {
  int gameTs = 0;
  GameWorld *world = GetWorldSafe();
  if (!world) {
    return 0;
  }
  TimeOfDay tod = world->getTimeStamp_inGameHours();
  gameTs = (int)tod.getTotalSeconds();
  if (gameTs < 0) {
    gameTs = 0;
  }
  return gameTs;
}

std::string NormalizeGeoToken(const std::string &rawValue) {
  std::string value = TrimChatLine(rawValue);
  if (value.empty()) {
    return "";
  }
  std::string lowered = value;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
  if (lowered == "unknown" || lowered == "none" || lowered == "null" ||
      lowered == "n/a") {
    return "";
  }
  return value;
}

void AppendGeoQueryFromPlayer(std::wstring &endpoint, Character *player) {
  if (!player || (uintptr_t)player <= 0x1000) {
    return;
  }

  std::string contextJson = BuildNpcContextEnvelope(player, "player");
  if (contextJson.empty() || contextJson == "{}") {
    return;
  }

  std::string town = NormalizeGeoToken(JsonReadField(contextJson, "town"));
  std::string environmentJson = JsonReadField(contextJson, "environment");
  std::string building =
      NormalizeGeoToken(JsonReadField(environmentJson, "building_name"));
  std::string zone = NormalizeGeoToken(JsonReadField(environmentJson, "zone_name"));
  std::string region = NormalizeGeoToken(JsonReadField(environmentJson, "region"));
  std::string resolvedRegion = zone.empty() ? region : zone;

  std::string location = "";
  if (!building.empty() && !town.empty()) {
    location = building + ", " + town;
  } else if (!town.empty()) {
    location = town;
  } else if (!building.empty()) {
    location = building;
  } else if (!resolvedRegion.empty()) {
    location = resolvedRegion;
  }

  if (!location.empty()) {
    endpoint += L"&location=" + ToWide(UrlEncode(location));
  }
  if (!town.empty()) {
    endpoint += L"&city=" + ToWide(UrlEncode(town));
  }
  if (!resolvedRegion.empty()) {
    endpoint += L"&region=" + ToWide(UrlEncode(resolvedRegion));
  }
}

bool TryMarkRechatDispatch(LONG generation) {
  if (!IsChatInterruptGenerationCurrent(generation)) {
    return false;
  }
  DWORD nowTick = GetTickCount();
  bool allowDispatch = false;
  EnterCriticalSection(&g_stateMutex);
  DWORD cooldownMs = g_rechatDispatchCooldownMs > 0
                         ? (DWORD)g_rechatDispatchCooldownMs
                         : 350;
  if (g_lastRechatDispatchTick == 0 ||
      (nowTick - g_lastRechatDispatchTick) >= cooldownMs) {
    g_lastRechatDispatchTick = nowTick;
    allowDispatch = true;
  }
  LeaveCriticalSection(&g_stateMutex);
  return allowDispatch && IsChatInterruptGenerationCurrent(generation);
}

void DispatchRechatFollowup(const StreamChatTask &currentTask,
                            const std::string &lastSpeaker,
                            const std::string &lastSpeakerHandle,
                            const std::string &lastSubtitle) {
  if (!IsChatInterruptGenerationCurrent(currentTask.generation)) {
    return;
  }
  if (currentTask.requestMode == "whisper") {
    Log("RECHAT: skipped (whisper mode)");
    return;
  }

  std::string speaker = TrimChatLine(lastSpeaker);
  std::string speakerHandle = TrimChatLine(lastSpeakerHandle);
  if (speakerHandle.empty() && EqualsIgnoreCase(speaker, currentTask.npcName)) {
    speakerHandle = TrimChatLine(currentTask.handleStr);
  }
  std::string subtitle = TrimChatLine(lastSubtitle);
  subtitle = SanitizeDialogueForEventStream(subtitle);
  if (speaker.empty() || subtitle.empty()) {
    return;
  }

  if (!TryMarkRechatDispatch(currentTask.generation)) {
    return;
  }

  std::string previousSpeaker = TrimChatLine(currentTask.previousSpeaker);
  if (previousSpeaker.empty()) {
    previousSpeaker = "Player";
  }
  std::string previousSpeakerHandle = TrimChatLine(currentTask.previousSpeakerHandle);

  std::string eventData = speaker + ": " + subtitle +
                          " (talking to: " + previousSpeaker + ")";
  int nextRechatDepth = currentTask.rechatDepth + 1;
  if (nextRechatDepth < 1) {
    nextRechatDepth = 1;
  }
  Log("RECHAT_TIMING: dispatch candidate speaker=" + speaker +
      " listener=" + previousSpeaker +
      " subtitle_len=" + ToString((int)subtitle.length()) +
      " gen=" + ToString((int)currentTask.generation) +
      " depth=" + ToString(nextRechatDepth));
  int gameTs = ResolveCurrentGameTs();

  std::wstring endpoint = L"/StobeServer/stream.php?DATA=" +
                          ToWide(BuildStreamQueryData("rechat", eventData, gameTs)) +
                          L"&profile=" + ToWide(UrlEncode(speaker)) +
                          L"&tts_enabled=" + (g_ttsEnabled ? L"1" : L"0") +
                          L"&rechat_depth=" + ToWide(ToString(nextRechatDepth));
  std::string initiatorSpeaker = TrimChatLine(currentTask.initiatorSpeaker);
  std::string initiatorSpeakerHandle =
      TrimChatLine(currentTask.initiatorSpeakerHandle);
  if (!initiatorSpeaker.empty()) {
    endpoint += L"&initiator=" + ToWide(UrlEncode(initiatorSpeaker));
  }
  if (!initiatorSpeakerHandle.empty()) {
    endpoint += L"&initiator_sid=" + ToWide(UrlEncode(initiatorSpeakerHandle));
  }
  if (!currentTask.requestMode.empty()) {
    endpoint += L"&mode=" + ToWide(UrlEncode(currentTask.requestMode));
  }
  std::string peopleJson = "";
  std::string peopleSource = "minimal_speaker_target";
  GameWorld *world = GetWorldSafe();
  Character *speakerNpc = nullptr;
  Character *listenerNpc = nullptr;
  if (world) {
    speakerNpc = ResolveChatTargetCharacter(world, speaker, speakerHandle);
    listenerNpc =
        ResolveChatTargetCharacter(world, previousSpeaker, previousSpeakerHandle);
  }
  if (speakerNpc && (uintptr_t)speakerNpc > 0x1000) {
    std::string speakerNameForPeople = speaker;
    if (speakerNameForPeople.empty()) {
      speakerNameForPeople = speakerNpc->getName();
    }
    std::string listenerNameForPeople = previousSpeaker;
    std::string listenerHandleForPeople = previousSpeakerHandle;
    if (listenerNpc && (uintptr_t)listenerNpc > 0x1000) {
      if (listenerNameForPeople.empty()) {
        listenerNameForPeople = listenerNpc->getName();
      }
      if (listenerHandleForPeople.empty()) {
        listenerHandleForPeople = ToString(listenerNpc->getHandle().serial);
      }
    }
    // Rebuild listeners per hop around the current speaker so only nearby NPCs
    // in talk range can hear/respond.
    peopleJson =
        BuildPeopleJson(world, speakerNameForPeople, listenerNameForPeople,
                        listenerHandleForPeople, "chat", speakerNpc);
    if (!peopleJson.empty()) {
      peopleSource = "rebuilt_from_current_speaker";
      if (speakerHandle.empty()) {
        speakerHandle = ToString(speakerNpc->getHandle().serial);
      }
    }
  }
  if (peopleJson.empty()) {
    std::vector<std::string> minimalPeople;
    if (!speaker.empty()) {
      if (!speakerHandle.empty()) {
        AppendUniquePerson(minimalPeople, speaker + "|" + speakerHandle);
      } else {
        AppendUniquePerson(minimalPeople, speaker);
      }
    }
    if (!previousSpeaker.empty()) {
      if (!previousSpeakerHandle.empty()) {
        AppendUniquePerson(minimalPeople,
                           previousSpeaker + "|" + previousSpeakerHandle);
      } else {
        AppendUniquePerson(minimalPeople, previousSpeaker);
      }
    }
    peopleJson = "[";
    for (size_t i = 0; i < minimalPeople.size(); ++i) {
      if (i > 0) {
        peopleJson += ",";
      }
      peopleJson += "\"" + EscapeJSON(minimalPeople[i]) + "\"";
    }
    peopleJson += "]";
  }
  if (!peopleJson.empty()) {
    endpoint += L"&people=" + ToWide(UrlEncode(peopleJson));
  }
  Character *player = nullptr;
  if (world && world->player && world->player->playerCharacters.size() > 0) {
    player = world->player->playerCharacters[0];
  }
  AppendGeoQueryFromPlayer(endpoint, player);

  StreamChatTask *nextTask = new StreamChatTask();
  nextTask->endpoint = endpoint;
  nextTask->npcName = speaker;
  nextTask->handleStr = speakerHandle;
  nextTask->peopleJson = peopleJson;
  nextTask->previousSpeaker = speaker;
  nextTask->previousSpeakerHandle = speakerHandle;
  nextTask->initiatorSpeaker = initiatorSpeaker;
  nextTask->initiatorSpeakerHandle = initiatorSpeakerHandle;
  nextTask->requestMode = currentTask.requestMode;
  nextTask->generation = currentTask.generation;
  nextTask->rechatDepth = nextRechatDepth;

  HANDLE followupThread =
      CreateThread(NULL, 0, StreamChatResponseThread, nextTask, 0, NULL);
  if (followupThread) {
    CloseHandle(followupThread);
    Log("RECHAT: dispatched follow-up for speaker " + speaker +
        " targeting " + previousSpeaker + " people_source=" + peopleSource +
        " people_len=" + ToString((int)peopleJson.length()) +
        " depth=" + ToString(nextRechatDepth));
  } else {
    delete nextTask;
    Log("RECHAT: failed to start follow-up stream thread.");
  }
}

DWORD WINAPI PlayerTtsResponseThread(LPVOID lpParam) {
  PlayerTtsTask *task = (PlayerTtsTask *)lpParam;
  if (!task) {
    return 0;
  }

  if (!g_ttsEnabled) {
    delete task;
    Log("CHAT_TIMING: PLAYER_TTS request dropped because TTS is disabled.");
    return 0;
  }

  LONG generation = task->generation;
  DWORD requestStartTick = task->requestStartTick;
  std::string playerText = task->playerText;
  std::string response = PostToStobeWithResponse(task->endpoint, "");
  DWORD requestMs = GetTickCount() - requestStartTick;
  delete task;
  if (!IsChatInterruptGenerationCurrent(generation)) {
    Log("CHAT_TIMING: PLAYER_TTS response discarded (stale generation) after " +
        ToString((int)requestMs) + " ms");
    return 0;
  }
  if (response.empty()) {
    Log("CHAT_TIMING: PLAYER_TTS empty response after " +
        ToString((int)requestMs) + " ms");
    return 0;
  }
  if (!g_ttsEnabled) {
    Log("CHAT_TIMING: PLAYER_TTS response discarded because TTS is now disabled.");
    return 0;
  }

  std::string okValue = TrimChatLine(JsonReadField(response, "ok"));
  bool ok = (okValue == "true" || okValue == "1");
  if (!ok) {
    Log("CHAT_TIMING: PLAYER_TTS response not ok after " +
        ToString((int)requestMs) + " ms");
    return 0;
  }

  std::string hash = ParseTtsHashToken("tts=" + TrimChatLine(JsonReadField(response, "hash")));
  int durationMs =
      ParseTtsDurationToken("ttsd=" + TrimChatLine(JsonReadField(response, "duration_ms")));
  if (hash.empty()) {
    Log("CHAT_TIMING: PLAYER_TTS missing/invalid hash after " +
        ToString((int)requestMs) + " ms");
    return 0;
  }

  Log("CHAT_TIMING: PLAYER_TTS resolved hash=" + ShortHashForLog(hash) +
      " dur_ms=" + ToString(durationMs) +
      " req_ms=" + ToString((int)requestMs) +
      " text_len=" + ToString((int)playerText.length()) +
      " gen=" + ToString((int)generation));

  if (!IsChatInterruptGenerationCurrent(generation)) {
    Log("CHAT_TIMING: PLAYER_TTS dropped before queue (stale generation)");
    return 0;
  }

  std::string msg = "PLAYER_TTS: " + hash + "|" + ToString(durationMs);
  QueueChatPipeLine(msg, generation);
  return 0;
}

DWORD WINAPI ManualDiaryResponseThread(LPVOID lpParam) {
  ManualDiaryTask *task = (ManualDiaryTask *)lpParam;
  if (!task) {
    return 0;
  }

  std::string targetNpcName = task->targetNpcName;
  std::string peopleJson = task->peopleJson;
  DWORD requestStartTick = task->requestStartTick;
  std::string response = PostToStobeWithResponse(task->endpoint, "");
  DWORD requestMs = GetTickCount() - requestStartTick;
  delete task;

  if (response.empty()) {
    Log("DIARY: manual trigger empty response target='" + targetNpcName +
        "' req_ms=" + ToString((int)requestMs));
    QueueUiNotifyAction("Diary: request failed for " + targetNpcName + ".");
    return 0;
  }

  std::string okValue = TrimChatLine(JsonReadField(response, "ok"));
  bool ok = (okValue == "true" || okValue == "1");
  int attempted = atoi(TrimChatLine(JsonReadField(response, "attempted")).c_str());
  int generated = atoi(TrimChatLine(JsonReadField(response, "generated")).c_str());
  int skipped = atoi(TrimChatLine(JsonReadField(response, "skipped")).c_str());
  int failed = atoi(TrimChatLine(JsonReadField(response, "failed")).c_str());
  std::string reason = TrimChatLine(JsonReadField(response, "reason"));
  std::string statusMessage = TrimChatLine(JsonReadField(response, "status_message"));
  if (statusMessage.empty()) {
    statusMessage = ok ? "Diary request processed." : "Diary generation failed.";
  }

  std::string responsePreview = response;
  if (responsePreview.length() > 420) {
    responsePreview = responsePreview.substr(0, 420) + "...";
  }
  Log("DIARY: manual trigger result target='" + targetNpcName + "' ok=" +
      std::string(ok ? "1" : "0") + " attempted=" + ToString(attempted) +
      " generated=" + ToString(generated) + " skipped=" + ToString(skipped) +
      " failed=" + ToString(failed) + " reason='" + reason + "'" +
      " req_ms=" + ToString((int)requestMs) +
      " people_len=" + ToString((int)peopleJson.length()) +
      " response=" + responsePreview);

  std::string notifyMessage = "Diary: " + statusMessage;
  if (!ok && !reason.empty() && reason != "null") {
    notifyMessage += " (" + reason + ")";
  }
  QueueUiNotifyAction(notifyMessage);
  return 0;
}

void ExtractActionTags(std::string &speech, std::vector<std::string> &actions) {
  static const char *commandNames[] = {
      "ATTACK",           "FOLLOW",        "STOP_FOLLOW",
      "JOIN_PARTY",       "LEAVE",
      "IDLE",             "STOP_CARRYING", "RELEASE_PLAYER",
      "RELEASE_PRISONER",
      "GIVE_CATS",        "TAKE_CATS",     "TAKE_ITEM",        "GIVE_ITEM",
      "DROP_ITEM",        "DRINK_ITEM",    "DRINKITEM",        "DRINK-ITEM",
      "USE_DRUGS",        "USEDRUGS",      "USE-DRUGS",
      "ROLEPLAY_ACTION",  "ROLEPLAYACTION","ROLEPLAY-ACTION",
      "NOTIFY",           "FACTION_RELATIONS","TRAVEL_LOCATION",
      "TRAVELLOCATION",   "USE_OBJECT",    "USEOBJECT",
      "USE-OBJECT",       "KILL",          "KILLTARGET",
      "SPAWN_ITEM",
      "TASK",             "SET_BLOCK",     "SET_HOLD",         "SET_PASSIVE",
      "SET_JOBS",         "SET_RANGED",    "SET_TAUNT",        "SET_SNEAK",
      "SET_RESOURCE",     "SET_MEDIC",     0};

  while (true) {
    if (speech.empty()) {
      break;
    }

    std::string upperSpeech = speech;
    std::transform(upperSpeech.begin(), upperSpeech.end(), upperSpeech.begin(),
                   ::toupper);

    size_t bestPos = std::string::npos;
    size_t bestLen = 0;
    for (int i = 0; commandNames[i] != 0; ++i) {
      const std::string key = std::string(commandNames[i]) + "@";
      size_t pos = upperSpeech.find(key);
      while (pos != std::string::npos) {
        const bool hasBoundary =
            (pos == 0 || upperSpeech[pos - 1] == ' ' ||
             upperSpeech[pos - 1] == '\t' || upperSpeech[pos - 1] == '\n' ||
             upperSpeech[pos - 1] == '\r');
        if (hasBoundary && (bestPos == std::string::npos || pos < bestPos)) {
          bestPos = pos;
          bestLen = key.length();
          break;
        }
        pos = upperSpeech.find(key, pos + 1);
      }
    }

    if (bestPos == std::string::npos || bestLen == 0) {
      break;
    }

    size_t tokenEnd = speech.size();
    std::string token = TrimChatLine(speech.substr(bestPos, tokenEnd - bestPos));
    if (!token.empty()) {
      actions.push_back(token);
    }
    speech.erase(bestPos, tokenEnd - bestPos);
  }

  speech = TrimChatLine(speech);
}

static std::string NormalizeActionForDedupe(const std::string &actorHeader,
                                            const std::string &rawAction) {
  std::string actorKey = TrimChatLine(actorHeader);
  std::string actionKey = TrimChatLine(rawAction);
  if (actionKey.empty()) {
    return "";
  }
  std::string combined = actorKey + "|" + actionKey;
  std::transform(combined.begin(), combined.end(), combined.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return combined;
}

static bool QueueStreamActionIfNew(StreamChatParseState *state,
                                   const std::string &actor,
                                   const std::string &speakerHeader,
                                   const std::string &rawAction) {
  if (!state) {
    return false;
  }
  std::string actionLine = TrimChatLine(rawAction);
  if (actionLine.empty()) {
    return false;
  }
  std::string dedupeKey = NormalizeActionForDedupe(speakerHeader, actionLine);
  if (dedupeKey.empty()) {
    return false;
  }
  if (state->seenActions.count(dedupeKey) > 0) {
    Log("CHAT_TIMING: STREAM_ACTION duplicate dropped actor=" + actor +
        " action=" + actionLine + " gen=" + ToString((int)state->generation));
    return false;
  }
  state->seenActions.insert(dedupeKey);
  Log("CHAT_TIMING: STREAM_ACTION actor=" + actor + " action=" + actionLine +
      " gen=" + ToString((int)state->generation));
  QueueChatPipeLine("NPC_ACTION: " + speakerHeader + ": " + actionLine,
                    state->generation);
  state->actionCount++;
  state->firstLine = false;
  return true;
}

bool ProcessStreamChatResponseLine(StreamChatParseState *state,
                                   const std::string &rawLine) {
  if (!state || !state->task) {
    return false;
  }

  if (!IsChatInterruptGenerationCurrent(state->generation)) {
    return false;
  }

  std::string line = TrimChatLine(rawLine);
  if (line.empty()) {
    return true;
  }
  if (line == "ok" || line == "error") {
    return true;
  }

  std::string actor = state->task->npcName;
  std::string actionKind = "ScriptQueue";
  std::string subtitle = line;
  std::string ttsHash = "";
  int ttsDurationMs = 0;

  size_t bar1 = line.find('|');
  size_t bar2 =
      (bar1 == std::string::npos) ? std::string::npos : line.find('|', bar1 + 1);
  size_t bar3 =
      (bar2 == std::string::npos) ? std::string::npos : line.find('|', bar2 + 1);
  if (bar1 != std::string::npos && bar2 != std::string::npos) {
    actor = TrimChatLine(line.substr(0, bar1));
    actionKind = TrimChatLine(line.substr(bar1 + 1, bar2 - bar1 - 1));
    std::string payload = line.substr(bar2 + 1);
    if (bar3 != std::string::npos) {
      payload = line.substr(bar2 + 1);
    }

    size_t metaSep = payload.find('|');
    if (metaSep == std::string::npos) {
      subtitle = TrimChatLine(payload);
    } else {
      subtitle = TrimChatLine(payload.substr(0, metaSep));
      std::string metadata = payload.substr(metaSep + 1);
      size_t tokenStart = 0;
      while (tokenStart <= metadata.length()) {
        size_t tokenEnd = metadata.find('|', tokenStart);
        std::string token = (tokenEnd == std::string::npos)
                                ? metadata.substr(tokenStart)
                                : metadata.substr(tokenStart,
                                                  tokenEnd - tokenStart);
        std::string parsedHash = ParseTtsHashToken(token);
        if (!parsedHash.empty()) {
          ttsHash = parsedHash;
        } else {
          int parsedDuration = ParseTtsDurationToken(token);
          if (parsedDuration > 0) {
            ttsDurationMs = parsedDuration;
          }
        }
        if (tokenEnd == std::string::npos) {
          break;
        }
        tokenStart = tokenEnd + 1;
      }
    }
    if (!g_ttsEnabled) {
      ttsHash.clear();
      ttsDurationMs = 0;
    }
    size_t slashPos = subtitle.find('/');
    if (slashPos != std::string::npos) {
      subtitle = TrimChatLine(subtitle.substr(0, slashPos));
    }
  }

  std::string speakerHeader = actor;
  if (!state->task->handleStr.empty() && actor == state->task->npcName) {
    speakerHeader = actor + "|" + state->task->handleStr;
  }
  if ((EqualsIgnoreCase(actionKind, "ActionQueue") ||
       EqualsIgnoreCase(actionKind, "Action")) &&
      !subtitle.empty()) {
    QueueStreamActionIfNew(state, actor, speakerHeader, subtitle);
    return true;
  }

  std::vector<std::string> extractedActions;
  ExtractActionTags(subtitle, extractedActions);
  for (size_t i = 0; i < extractedActions.size(); ++i) {
    QueueStreamActionIfNew(state, actor, speakerHeader, extractedActions[i]);
  }

  if (!subtitle.empty()) {
    if (!IsChatInterruptGenerationCurrent(state->generation)) {
      return false;
    }
    std::string queueLine = "NPC_SAY: " + speakerHeader + ": " + subtitle;
    if (speakerHeader == actor) {
      GameWorld *worldForSpeaker = GetWorldSafe();
      if (worldForSpeaker && worldForSpeaker->player &&
          worldForSpeaker->player->playerCharacters.size() > 0) {
        Character *playerSpeaker = worldForSpeaker->player->playerCharacters[0];
        if (playerSpeaker && (uintptr_t)playerSpeaker > 0x1000 &&
            EqualsIgnoreCase(playerSpeaker->getName(), actor)) {
          speakerHeader = actor + "|" + ToString(playerSpeaker->getHandle().serial);
        }
      }
      queueLine = "NPC_SAY: " + speakerHeader + ": " + subtitle;
    }
    if (g_ttsEnabled && !ttsHash.empty()) {
      queueLine += " [TTSHASH:" + ttsHash + "]";
    }
    if (g_ttsEnabled && ttsDurationMs > 0) {
      queueLine += " [TTSDUR:" + ToString(ttsDurationMs) + "]";
    }
    Log("CHAT_TIMING: STREAM_LINE actor=" + actor +
        " subtitle_len=" + ToString((int)subtitle.length()) +
        " tts_hash=" + ShortHashForLog(ttsHash) +
        " tts_dur_ms=" + ToString(ttsDurationMs) +
        " tts_enabled=" + std::string(g_ttsEnabled ? "1" : "0") +
        " gen=" + ToString((int)state->generation));
    QueueChatPipeLine(queueLine, state->generation);
    std::string speakerHandle = "";
    size_t headerPipePos = speakerHeader.find('|');
    if (headerPipePos != std::string::npos) {
      speakerHandle = TrimChatLine(speakerHeader.substr(headerPipePos + 1));
    } else if (EqualsIgnoreCase(actor, state->task->npcName)) {
      speakerHandle = TrimChatLine(state->task->handleStr);
    }
    state->lastSpeaker = actor;
    state->lastSpeakerHandle = speakerHandle;
    state->lastSubtitle = subtitle;
    state->interLineDelayMs = ResolveLineDelayMs(ttsDurationMs, subtitle);
    Log("CHAT_TIMING: STREAM_LINE next_delay_ms=" +
        ToString((int)state->interLineDelayMs));
    state->lineCount++;
  }

  state->firstLine = false;
  return true;
}

bool OnStreamChatHttpLine(const std::string &line, void *userData) {
  return ProcessStreamChatResponseLine((StreamChatParseState *)userData, line);
}

DWORD WINAPI StreamChatResponseThread(LPVOID lpParam) {
  StreamChatTask *task = (StreamChatTask *)lpParam;
  if (!task)
    return 0;

  LONG generation = task->generation;
  StreamChatParseState parseState;
  parseState.task = task;
  parseState.generation = generation;
  parseState.firstLine = true;
  parseState.interLineDelayMs = ResolveLineDelayMs(0, "");
  parseState.lineCount = 0;
  parseState.actionCount = 0;
  parseState.lastSpeaker = "";
  parseState.lastSpeakerHandle = "";
  parseState.lastSubtitle = "";
  parseState.seenActions.clear();

  bool requestOk =
      PostToStobeWithResponseStream(task->endpoint, "", OnStreamChatHttpLine,
                                    &parseState);
  if (!IsChatInterruptGenerationCurrent(generation)) {
    delete task;
    return 0;
  }

  if (!requestOk && parseState.lineCount == 0 && parseState.actionCount == 0) {
    Log("CHAT_THREAD: Stream request failed or returned no data.");
    delete task;
    return 0;
  }
  if (parseState.lineCount == 0 && parseState.actionCount == 0) {
    Log("CHAT_THREAD: Empty stream response from server.");
    delete task;
    return 0;
  }

  Log("CHAT_THREAD: Queued " + ToString(parseState.lineCount) +
      " streamed chat lines and " + ToString(parseState.actionCount) +
      " action lines.");
  if (parseState.lineCount > 0 && IsChatInterruptGenerationCurrent(generation)) {
    DWORD followupDelayMs = parseState.interLineDelayMs;
    if (followupDelayMs < 250) {
      followupDelayMs = 250;
    } else if (followupDelayMs > 600000) {
      followupDelayMs = 600000;
    }
    Log("RECHAT_TIMING: follow-up wait start delay_ms=" +
        ToString((int)followupDelayMs) + " gen=" + ToString((int)generation));
    SleepIfPaused(followupDelayMs);

    DWORD playbackWaitStart = GetTickCount();
    while (IsChatInterruptGenerationCurrent(generation) && IsTtsPlaybackActive()) {
      SleepIfPaused(50);
      if ((GetTickCount() - playbackWaitStart) > 600000) {
        Log("RECHAT_TIMING: follow-up playback wait timed out");
        break;
      }
    }

    Log("RECHAT_TIMING: follow-up dispatch gate passed playback_active=" +
        std::string(IsTtsPlaybackActive() ? "1" : "0") +
        " remaining_ms=" + ToString(GetTtsPlaybackRemainingMs()) +
        " gen=" + ToString((int)generation));
    DispatchRechatFollowup(*task, parseState.lastSpeaker,
                           parseState.lastSpeakerHandle, parseState.lastSubtitle);
  }
  delete task;
  return 0;
}

void OnChatInputChange(MyGUI::EditBox *sender) {
  std::string text = sender->getCaption().asUTF8();

  if (g_chatJustOpened) {
    if (text.length() > 0) {
      std::string textUpper = text;
      textUpper[0] = toupper(textUpper[0]);
      std::string hkUpper = g_chatHotkeyStr;
      if (!hkUpper.empty())
        hkUpper[0] = toupper(hkUpper[0]);

      if (text == "\\" || text == "\n" || text == "\r" ||
          (text.length() == 1 && textUpper == hkUpper)) {
        sender->setCaption("");
        g_chatJustOpened = false;
        return;
      }

      // Some keyboard layouts leak the chat hotkey character into the input
      // immediately after opening. If the hotkey is still physically down,
      // treat the first single-char change as leaked input and clear it.
      if (text.length() == 1 && (GetAsyncKeyState(g_chatHotkey) & 0x8000)) {
        sender->setCaption("");
        g_chatJustOpened = false;
        return;
      }
      g_chatJustOpened = false;
    }
  }

  if (text == "\\" || text == "\n" || text == "\r") {
    sender->setCaption("");
    return;
  }

  // Support sending on Enter while in multi-line mode
  if (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    // Strip trailing newline and trigger send
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
      text.pop_back();

    sender->setCaption(text);
    OnChatSendClick(sender);
  }
}

void OnChatInputAccept(MyGUI::EditBox *sender) { OnChatSendClick(sender); }

void OnChatSendClick(MyGUI::Widget *sender) {
  if (!g_chatInput)
    return;
  std::string text = g_chatInput->getCaption().asUTF8();
  if (text.empty()) {
    CloseChatUI();
    return;
  }

  std::string npcName = g_chatTargetNameStr;
  std::string handleStr = g_chatTargetHandleStr;
  GameWorld *world = GetWorldSafe();

  // COMMAND SUPPORT: /name newName
  if (text.substr(0, 6) == "/name " && text.length() > 6) {
    std::string newName = text.substr(6);
    // Trim
    newName.erase(0, newName.find_first_not_of(" \t\r\n"));
    newName.erase(newName.find_last_not_of(" \t\r\n") + 1);

    if (!newName.empty()) {
      if (world) {
        Character *target = nullptr;
        const auto &chars = world->getCharacterUpdateList();
        for (auto it = chars.begin(); it != chars.end(); ++it) {
          if (*it && (uintptr_t)(*it) > 0x1000) {
            unsigned int serial = 0;
            if (TryParseSerial(handleStr, serial) &&
                (*it)->getHandle().serial == serial) {
              target = *it;
              break;
            }
          }
        }

        if (target) {
          std::string oldName = npcName;
          target->setName(newName);
          Log("RENAME: " + npcName + " is now " + newName);
          g_chatTargetNameStr = newName;

          std::string renJson =
              "{\"old_name\": \"" + EscapeJSON(oldName) + "\", ";
          renJson += "\"new_name\": \"" + EscapeJSON(newName) + "\", ";
          renJson += "\"context\": " + BuildNpcContextEnvelope(target) + "}";
          AsyncPostToStobe(L"/rename", renJson);

          if (g_chatWindow) {
            g_chatWindow->setCaption(WideFromUtf8("Chat Box").c_str());
          }
          if (g_chatLabel) {
            std::string speakerName = g_chatPlayerNameStr;
            if (speakerName.empty()) {
              speakerName = "Player";
            }
            g_chatLabel->setCaption(
                WideFromUtf8("Speaker: " + speakerName + " -> Target: " + newName)
                    .c_str());
          }

          g_chatInput->setCaption("");
          return;
        }
      }
    }
  }

  std::string playerName = g_chatPlayerNameStr;
  if (playerName.empty()) {
    playerName = "Player";
  }

  std::string sanitizedText = SanitizeDialogueForEventStream(text);
  if (!sanitizedText.empty() && sanitizedText != text) {
    Log("CHAT_SANITIZE: stripped trailing noise from player text old_len=" +
        ToString((int)text.length()) +
        " new_len=" + ToString((int)sanitizedText.length()));
    text = sanitizedText;
  }

  std::string selectedMode = NormalizeChatMode(g_chatMode);
  std::string mode = "talk";
  bool cheatModeSelected = (selectedMode == "cheat");
  if (cheatModeSelected) {
    mode = "cheat";
  } else if (g_autoChatEnabled) {
    mode = "autochat";
  } else if (selectedMode == "whisper") {
    mode = "whisper";
  } else if (selectedMode == "shout") {
    mode = "shout";
  }

  size_t selectedManualActionIndex = 0;
  if (g_chatActionCombo) {
    size_t rawIndex = g_chatActionCombo->getIndexSelected();
    if (rawIndex != MyGUI::ITEM_NONE) {
      selectedManualActionIndex = SanitizeManualChatActionIndex(rawIndex);
    }
  }
  const ManualChatActionChoice &manualActionChoice =
      GetManualChatActionChoice(selectedManualActionIndex);
  bool manualActionSelected =
      (manualActionChoice.type != MANUAL_CHAT_ACTION_NONE);
  std::string manualActionArgRaw =
      g_chatActionArgInput ? g_chatActionArgInput->getCaption().asUTF8() : "";
  if (g_chatActionCombo && g_chatActionCombo->getIndexSelected() != 0) {
    g_chatActionCombo->setIndexSelected(0);
  }
  if (g_chatActionArgInput) {
    g_chatActionArgInput->setCaption("");
  }

  // New player input preempts current dialogue flow: stop active TTS and
  // invalidate any queued rechat/follow-up work before further processing.
  LONG chatGeneration = BeginChatInterruptGeneration();
  Log("CHAT_INTERRUPT: new player send preempted active dialogue gen=" +
      ToString((int)chatGeneration));

  Log("CHAT_SEND_STAGE: begin mode=" + selectedMode + " request_mode=" + mode +
      " autochat=" + std::string(g_autoChatEnabled ? "1" : "0") +
      " manual_action=" +
      std::string(manualActionSelected ? manualActionChoice.label : "none") +
      " target_name=" + npcName +
      " target_handle=" + handleStr + " text_len=" + ToString((int)text.length()));

  Character *player = nullptr;
  if (world && world->player && world->player->playerCharacters.size() > 0) {
    player = world->player->playerCharacters[0];
  }
  Log("CHAT_SEND_STAGE: resolving_target");
  Character *targetNpc = ResolveChatTargetCharacter(world, npcName, handleStr);
  Character *bestSpeaker = ResolveConfiguredPlayerSpeaker(world, targetNpc);
  if (bestSpeaker && (uintptr_t)bestSpeaker > 0x1000) {
    player = bestSpeaker;
    playerName = bestSpeaker->getName();
    g_chatPlayerNameStr = playerName;
  }
  Log("CHAT_SEND_STAGE: resolved_target ptr=" +
      ToString((int)((uintptr_t)targetNpc & 0x7fffffff)) + " has_target=" +
      std::string(targetNpc ? "1" : "0"));
  if (player && targetNpc) {
    Log("CHAT_SEND_STAGE: resolved_speaker=" + player->getName() +
        " dist_to_target=" +
        ToString(player->getPosition().distance(targetNpc->getPosition())));
  }
  Log("CHAT_SEND_STAGE: validate_target");
  std::string sendFailReason;
  bool requireStrictTalkValidation = !manualActionSelected;
  if (!ValidatePlayerChatSend(world, player, targetNpc, selectedMode,
                              requireStrictTalkValidation,
                              sendFailReason)) {
    if (world && !sendFailReason.empty()) {
      world->showPlayerAMessage_withLog("Chat blocked: " + sendFailReason, true);
    }
    std::string targetLogName = npcName.empty() ? "<unknown>" : npcName;
    float dist = -1.0f;
    if (player && targetNpc) {
      dist = player->getPosition().distance(targetNpc->getPosition());
    }
    Log("CHAT_GATE: blocked mode=" + selectedMode + " target=" + targetLogName +
        " dist=" + ToString(dist) + " player_floor=" +
        (player ? ToString(player->getFloor()) : std::string("NA")) +
        " target_floor=" +
        (targetNpc ? ToString(targetNpc->getFloor()) : std::string("NA")) +
        " player_indoors=" +
        std::string((player && IsIndoorsHandleValid(player->isIndoors())) ? "1"
                                                                            : "0") +
        " target_indoors=" +
        std::string((targetNpc && IsIndoorsHandleValid(targetNpc->isIndoors()))
                        ? "1"
                        : "0") +
        " reason=" + sendFailReason);
    return;
  }

  std::string targetName = npcName;
  if (targetNpc && (uintptr_t)targetNpc > 0x1000) {
    std::string resolvedTargetName = targetNpc->getName();
    if (!resolvedTargetName.empty()) {
      targetName = resolvedTargetName;
    }
  }
  if (targetName.empty()) {
    targetName = "Unknown";
  }

  std::string resolvedTargetHandle = handleStr;
  if (targetNpc && (uintptr_t)targetNpc > 0x1000) {
    resolvedTargetHandle = ResolveCharacterSerialToken(targetNpc);
  }

  std::string speakerHandleForAction = ResolveCharacterSerialToken(player);
  std::string manualActionCommand = "";
  bool manualActionPromptEligible = false;
  std::string manualActionPromptSkipReason = "";
  std::string manualActionTextArg = TrimManualActionArg(manualActionArgRaw);
  int manualActionAmount = 0;
  bool manualActionAmountValid =
      TryParseActionAmount(manualActionArgRaw, manualActionAmount);
  if (manualActionSelected) {
    if (manualActionChoice.type == MANUAL_CHAT_ACTION_REMOVE_LIMB) {
      manualActionCommand = BuildManualRemoveLimbActionToken(
          targetName, resolvedTargetHandle, manualActionChoice.limbToken);
      if (manualActionCommand.empty()) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Could not build remove-limb action.", true);
        }
        Log("CHAT_GATE: blocked manual remove limb invalid_action_token target='" +
            targetName + "' handle='" + resolvedTargetHandle + "'");
        return;
      }
      if (!CharacterHasHacksaw(player)) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Speaker needs a hacksaw to remove limbs.", true);
        }
        Log("CHAT_GATE: blocked manual remove limb missing_hacksaw actor='" +
            playerName + "'");
        return;
      }
      std::string invalidReason = "";
      if (!IsRemoveLimbTargetValid(world, targetNpc, invalidReason)) {
        if (invalidReason.empty()) {
          invalidReason =
              "target must be knocked out, unconscious, imprisoned, or carried";
        }
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: " + invalidReason + ".", true);
        }
        Log("CHAT_GATE: blocked manual remove limb reason='" + invalidReason +
            "' actor='" + playerName + "' target='" + targetName + "'");
        return;
      }
      manualActionPromptEligible = true;
    } else if (manualActionChoice.type == MANUAL_CHAT_ACTION_KILL) {
      manualActionCommand =
          BuildManualKillActionToken(targetName, resolvedTargetHandle);
      if (manualActionCommand.empty()) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Could not build kill action.", true);
        }
        Log("CHAT_GATE: blocked manual kill invalid_action_token target='" +
            targetName + "' handle='" + resolvedTargetHandle + "'");
        return;
      }
      std::string invalidReason = "";
      if (!IsRemoveLimbTargetValid(world, targetNpc, invalidReason)) {
        if (invalidReason.empty()) {
          invalidReason =
              "target must be knocked out, unconscious, imprisoned, or carried";
        }
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: " + invalidReason + ".", true);
        }
        Log("CHAT_GATE: blocked manual kill reason='" + invalidReason +
            "' actor='" + playerName + "' target='" + targetName + "'");
        return;
      }
      manualActionPromptEligible = true;
    } else if (manualActionChoice.type == MANUAL_CHAT_ACTION_GIVE_CATS) {
      if (!manualActionAmountValid) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Provide a positive cats amount.", true);
        }
        Log("CHAT_GATE: blocked manual give cats invalid amount raw='" +
            manualActionArgRaw + "'");
        return;
      }
      int speakerCats = ResolveSpeakerCatsForTransfer(player);
      if (speakerCats <= 0) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Speaker has no cats to give.", true);
        }
        Log("CHAT_GATE: blocked manual give cats actor_has_no_cats actor='" +
            playerName + "'");
        return;
      }
      if (manualActionAmount > speakerCats) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Speaker only has " + ToString(speakerCats) +
                  " cats.",
              true);
        }
        Log("CHAT_GATE: blocked manual give cats amount_exceeds_actor_money "
            "requested=" +
            ToString(manualActionAmount) +
            " available=" + ToString(speakerCats) + " actor='" + playerName +
            "'");
        return;
      }
      manualActionCommand = BuildManualGiveCatsActionToken(
          targetName, resolvedTargetHandle, manualActionAmount);
      if (manualActionCommand.empty()) {
        manualActionPromptSkipReason = "invalid_action_token";
      }
    } else if (manualActionChoice.type == MANUAL_CHAT_ACTION_GIVE_ITEM) {
      if (manualActionTextArg.empty()) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Provide an item name for give item.", true);
        }
        Log("CHAT_GATE: blocked manual give item missing item name");
        return;
      }
      std::string matchedItemName = "";
      if (!ResolveSpeakerGiveItemMatch(player, manualActionTextArg,
                                       matchedItemName)) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Item not found in speaker inventory/equipment.",
              true);
        }
        Log("CHAT_GATE: blocked manual give item no_inventory_match query='" +
            manualActionTextArg + "' actor='" + playerName + "'");
        return;
      }
      manualActionCommand = BuildManualGiveItemActionToken(
          targetName, resolvedTargetHandle, matchedItemName);
      if (manualActionCommand.empty()) {
        manualActionPromptSkipReason = "invalid_action_token";
      }
    } else if (manualActionChoice.type == MANUAL_CHAT_ACTION_DRINK_ITEM) {
      if (manualActionTextArg.empty()) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Provide a drink item name.", true);
        }
        Log("CHAT_GATE: blocked manual drink item missing item name");
        return;
      }
      if (IsCharacterSkeletonRace(player)) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Skeleton race cannot drink.", true);
        }
        Log("CHAT_GATE: blocked manual drink item reason=skeleton_race actor='" +
            playerName + "'");
        return;
      }
      std::string matchedDrinkItemName = "";
      if (!ResolveCharacterDrinkItemMatch(player, manualActionTextArg,
                                          matchedDrinkItemName)) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Drink item must be Bloodrum, Cactus Rum, Grog, or "
              "Sake in speaker inventory/equipment.",
              true);
        }
        Log("CHAT_GATE: blocked manual drink item no_inventory_match query='" +
            manualActionTextArg + "' actor='" + playerName + "'");
        return;
      }
      manualActionCommand = BuildManualDrinkItemActionToken(matchedDrinkItemName);
      if (manualActionCommand.empty()) {
        manualActionPromptSkipReason = "invalid_action_token";
      }
    } else if (manualActionChoice.type == MANUAL_CHAT_ACTION_USE_DRUGS) {
      if (manualActionTextArg.empty()) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Provide a drug item name.", true);
        }
        Log("CHAT_GATE: blocked manual use drugs missing item name");
        return;
      }
      if (IsCharacterSkeletonRace(player)) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Skeleton race cannot use drugs.", true);
        }
        Log("CHAT_GATE: blocked manual use drugs reason=skeleton_race actor='" +
            playerName + "'");
        return;
      }
      std::string matchedDrugItemName = "";
      if (!ResolveCharacterDrugItemMatch(player, manualActionTextArg,
                                         matchedDrugItemName)) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Drug item must be Hashish in speaker "
              "inventory/equipment.",
              true);
        }
        Log("CHAT_GATE: blocked manual use drugs no_inventory_match query='" +
            manualActionTextArg + "' actor='" + playerName + "'");
        return;
      }
      manualActionCommand = BuildManualUseDrugsActionToken(matchedDrugItemName);
      if (manualActionCommand.empty()) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Could not build use-drugs action.", true);
        }
        Log("CHAT_GATE: blocked manual use drugs invalid_action_token actor='" +
            playerName + "'");
        return;
      }
    } else if (manualActionChoice.type == MANUAL_CHAT_ACTION_ROLEPLAY_ACTION) {
      if (manualActionTextArg.empty()) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Provide text for roleplay action.", true);
        }
        Log("CHAT_GATE: blocked manual roleplay action missing notice text");
        return;
      }
      manualActionCommand = BuildManualRoleplayActionToken(manualActionTextArg);
      if (manualActionCommand.empty()) {
        manualActionPromptSkipReason = "invalid_action_token";
      }
    }
  }

  CloseChatUI();

  bool shouldQueueLocalPlayerSpeech = (mode != "autochat" && mode != "cheat");
  if (shouldQueueLocalPlayerSpeech) {
    EnterCriticalSection(&g_msgMutex);
    g_messageQueue.push_back("PLAYER_SAY: " + text);
    LeaveCriticalSection(&g_msgMutex);
    Log("CHAT_TIMING: PLAYER_SAY queued immediately (no TTSDUR yet), text_len=" +
        ToString((int)text.length()) + " gen=" + ToString((int)chatGeneration));
  } else {
    if (mode == "cheat") {
      Log("CHAT_TIMING: PLAYER_SAY suppressed locally for cheat mode; "
          "autochat ignored and request sent raw gen=" +
          ToString((int)chatGeneration));
    } else {
      Log("CHAT_TIMING: PLAYER_SAY suppressed locally for autochat; awaiting "
          "server rewrite gen=" + ToString((int)chatGeneration));
    }
  }

  if (!manualActionCommand.empty()) {
    std::string actionSpeakerHeader = playerName;
    if (!speakerHandleForAction.empty()) {
      actionSpeakerHeader += "|" + speakerHandleForAction;
    }
    QueueChatPipeLine("NPC_ACTION: " + actionSpeakerHeader + ": " +
                          manualActionCommand,
                      chatGeneration);
    Log("CHAT_ACTION: queued manual action actor=" + actionSpeakerHeader +
        " command=" + manualActionCommand + " gen=" +
        ToString((int)chatGeneration));
    if (!manualActionPromptEligible) {
      Log("CHAT_ACTION: manual action prompt context skipped reason=" +
          (manualActionPromptSkipReason.empty() ? std::string("unknown")
                                                : manualActionPromptSkipReason));
    }
  }

  std::string profileName = targetName;
  if (profileName.empty()) {
    profileName = npcName;
  }
  if (profileName.empty()) {
    profileName = "Unknown";
  }

  std::string eventData =
      playerName + ": " + text + " (talking to: " + targetName + ")";
  int gameTs = 0;
  if (world) {
    TimeOfDay tod = world->getTimeStamp_inGameHours();
    gameTs = (int)tod.getTotalSeconds();
    if (gameTs < 0) {
      gameTs = 0;
    }
  }

  if (shouldQueueLocalPlayerSpeech && g_ttsEnabled && !text.empty() &&
      text[0] != '/') {
    std::wstring playerTtsEndpoint =
        L"/StobeServer/player_tts.php?actor=" + ToWide(UrlEncode(playerName)) +
        L"&text=" + ToWide(UrlEncode(text)) + L"&tts_enabled=1";
    PlayerTtsTask *playerTtsTask = new PlayerTtsTask();
    playerTtsTask->endpoint = playerTtsEndpoint;
    playerTtsTask->generation = chatGeneration;
    playerTtsTask->requestStartTick = GetTickCount();
    playerTtsTask->playerText = text;
    Log("CHAT_TIMING: PLAYER_TTS request dispatched, text_len=" +
        ToString((int)text.length()) + " gen=" + ToString((int)chatGeneration));
    HANDLE playerTtsThread =
        CreateThread(NULL, 0, PlayerTtsResponseThread, playerTtsTask, 0, NULL);
    if (playerTtsThread) {
      CloseHandle(playerTtsThread);
    } else {
      delete playerTtsTask;
      Log("CHAT_THREAD: failed to start player TTS response thread.");
    }
  } else if (shouldQueueLocalPlayerSpeech && !g_ttsEnabled) {
    Log("CHAT_TIMING: PLAYER_TTS skipped because TTS is disabled.");
  }

  std::wstring endpoint =
      L"/StobeServer/stream.php?DATA=" +
      ToWide(BuildStreamQueryData("inputtext", eventData, gameTs)) +
      L"&profile=" + ToWide(UrlEncode(profileName)) +
      L"&mode=" + ToWide(UrlEncode(mode)) +
      L"&tts_enabled=" + (g_ttsEnabled ? L"1" : L"0");
  if (manualActionPromptEligible &&
      manualActionChoice.manualActionKey &&
      manualActionChoice.manualActionKey[0] != '\0') {
    endpoint += L"&manual_action=" +
                ToWide(UrlEncode(manualActionChoice.manualActionKey));
    endpoint += L"&manual_action_actor=" + ToWide(UrlEncode(playerName));
    endpoint += L"&manual_action_target=" + ToWide(UrlEncode(targetName));
    if (!speakerHandleForAction.empty()) {
      endpoint +=
          L"&manual_action_actor_sid=" + ToWide(UrlEncode(speakerHandleForAction));
    }
    if (!resolvedTargetHandle.empty()) {
      endpoint +=
          L"&manual_action_target_sid=" + ToWide(UrlEncode(resolvedTargetHandle));
    }
  }
  Log("CHAT_SEND_STAGE: building_people_json");
  std::string peopleJson =
      BuildPeopleJson(world, playerName, targetName, resolvedTargetHandle,
                      selectedMode,
                      player);
  Log("CHAT_SEND_STAGE: built_people_json len=" +
      ToString((int)peopleJson.length()));
  endpoint += L"&people=" + ToWide(UrlEncode(peopleJson));
  AppendGeoQueryFromPlayer(endpoint, player);
  StreamChatTask *streamTask = new StreamChatTask();
  streamTask->endpoint = endpoint;
  streamTask->npcName = profileName;
  streamTask->handleStr = resolvedTargetHandle;
  streamTask->peopleJson = peopleJson;
  streamTask->previousSpeaker = playerName;
  streamTask->previousSpeakerHandle =
      (player && (uintptr_t)player > 0x1000)
          ? ToString(player->getHandle().serial)
          : "";
  streamTask->initiatorSpeaker = playerName;
  streamTask->initiatorSpeakerHandle = streamTask->previousSpeakerHandle;
  streamTask->requestMode = mode;
  streamTask->generation = chatGeneration;
  streamTask->rechatDepth = 0;
  HANDLE chatThread =
      CreateThread(NULL, 0, StreamChatResponseThread, streamTask, 0, NULL);
  if (chatThread) {
    CloseHandle(chatThread);
  } else {
    delete streamTask;
    Log("CHAT_THREAD: failed to start stream response thread.");
  }
  Log("CHAT: dispatched inputtext event to StobeServer stream endpoint. people=" +
      peopleJson);
}

void OnChatCancelClick(MyGUI::Widget *sender) { CloseChatUI(); }
void OnBoredEventClick(MyGUI::Widget *sender) {
  GameWorld *world = GetWorldSafe();
  std::string preferredSpeakerName = TrimChatLine(g_chatTargetNameStr);
  std::string preferredSpeakerSerial = TrimChatLine(g_chatTargetHandleStr);
  LONG generation = BeginChatInterruptGeneration();

  EnterCriticalSection(&g_stateMutex);
  g_triggerBoredEvent = false;
  g_lastBoredEventTick = GetTickCount();
  LeaveCriticalSection(&g_stateMutex);

  bool dispatched =
      TriggerBoredEvent(world, true, preferredSpeakerName, preferredSpeakerSerial,
                        generation);
  if (!dispatched) {
    Log("BORED_EVENT: button trigger failed for selected NPC '" +
        preferredSpeakerName + "' serial=" + preferredSpeakerSerial);
  }
  CloseChatUI();
}

void OnWriteDiaryClick(MyGUI::Widget *sender) {
  GameWorld *world = GetWorldSafe();
  std::string targetNpcName = TrimChatLine(g_chatTargetNameStr);
  if (targetNpcName.empty()) {
    Log("DIARY: button trigger failed (empty target NPC).");
    CloseChatUI();
    return;
  }

  Character *player = nullptr;
  if (world && world->player && world->player->playerCharacters.size() > 0) {
    player = world->player->playerCharacters[0];
  }

  std::string playerName = TrimChatLine(g_chatPlayerNameStr);
  if (playerName.empty()) {
    playerName = "Player";
  }
  std::string targetHandle = TrimChatLine(g_chatTargetHandleStr);
  Character *targetNpc =
      ResolveChatTargetCharacter(world, targetNpcName, targetHandle);
  Character *bestSpeaker = ResolveConfiguredPlayerSpeaker(world, targetNpc);
  if (bestSpeaker && (uintptr_t)bestSpeaker > 0x1000) {
    player = bestSpeaker;
    playerName = bestSpeaker->getName();
  }
  std::string selectedMode = NormalizeChatMode(g_chatMode);
  std::string peopleJson =
      BuildPeopleJson(world, playerName, targetNpcName, targetHandle, selectedMode,
                      player);

  std::wstring endpoint =
      L"/StobeServer/stream.php?DATA=" +
      ToWide(BuildStreamQueryData("diary", targetNpcName, ResolveCurrentGameTs())) +
      L"&profile=" + ToWide(UrlEncode(targetNpcName)) +
      L"&tts_enabled=" + (g_ttsEnabled ? L"1" : L"0");
  if (!peopleJson.empty()) {
    endpoint += L"&people=" + ToWide(UrlEncode(peopleJson));
  }
  AppendGeoQueryFromPlayer(endpoint, player);

  ManualDiaryTask *task = new ManualDiaryTask();
  task->endpoint = endpoint;
  task->targetNpcName = targetNpcName;
  task->peopleJson = peopleJson;
  task->requestStartTick = GetTickCount();
  QueueUiNotifyAction("Diary: request sent for " + targetNpcName + ".");

  HANDLE diaryThread =
      CreateThread(NULL, 0, ManualDiaryResponseThread, task, 0, NULL);
  if (diaryThread) {
    CloseHandle(diaryThread);
    Log("DIARY: manual diary trigger dispatched for '" + targetNpcName +
        "' people_len=" + ToString((int)peopleJson.length()));
  } else {
    delete task;
    Log("DIARY: failed to start manual diary response thread for '" +
        targetNpcName + "'");
    QueueUiNotifyAction("Diary: failed to start request for " + targetNpcName +
                        ".");
  }
  CloseChatUI();
}

bool TriggerBoredEvent(GameWorld *world, bool forceDirectorMode,
                       const std::string &preferredSpeakerName,
                       const std::string &preferredSpeakerSerial,
                       LONG generationOverride) {
  if (!world || !world->player || world->player->playerCharacters.size() == 0) {
    return false;
  }

  Character *player = world->player->playerCharacters[0];
  if (!player || (uintptr_t)player <= 0x1000) {
    return false;
  }

  struct CandidateNpc {
    std::string name;
    std::string serial;
    float distance;
    bool isPreferred;
    bool indoors;
    unsigned int buildingSerial;
    int floor;
  };

  std::vector<CandidateNpc> candidates;
  const hand &playerIndoorsHandle = player->isIndoors();
  bool playerIsIndoors = IsIndoorsHandleValid(playerIndoorsHandle);
  float searchRadius = playerIsIndoors ? g_boredEventRange : g_proximityRadius;
  if (searchRadius < 10.0f) {
    searchRadius = 10.0f;
  }
  std::string preferredName = TrimChatLine(preferredSpeakerName);
  std::string preferredSerial = TrimChatLine(preferredSpeakerSerial);
  bool hasPreferred = !preferredName.empty() || !preferredSerial.empty();

  const ogre_unordered_set<Character *>::type &chars =
      world->getCharacterUpdateList();
  for (ogre_unordered_set<Character *>::type::const_iterator it = chars.begin();
       it != chars.end(); ++it) {
    Character *other = *it;
    if (!other || (uintptr_t)other <= 0x1000 || other == player) {
      continue;
    }
    if (!ShouldIncludeAnimalForTalk(other)) {
      continue;
    }
    try {
      if (other->isPlayerCharacter() || other->isDead() ||
          other->isUnconcious()) {
        continue;
      }
    } catch (...) {
      continue;
    }
    std::string otherName = other->getName();
    if (otherName.empty()) {
      continue;
    }
    const hand &otherIndoorsHandle = other->isIndoors();
    bool otherIsIndoors = IsIndoorsHandleValid(otherIndoorsHandle);
    unsigned int otherBuildingSerial =
        otherIsIndoors ? otherIndoorsHandle.serial : 0;
    float dist = player->getPosition().distance(other->getPosition());
    std::string serial = ToString(other->getHandle().serial);
    bool preferredMatch =
        (!preferredSerial.empty() && serial == preferredSerial) ||
        (!preferredName.empty() && EqualsIgnoreCase(otherName, preferredName));
    bool areaCompatible = IsConversationAreaCompatible(player, other);
    if (!areaCompatible) {
      continue;
    }
    if (dist > searchRadius && !preferredMatch) {
      continue;
    }
    CandidateNpc c;
    c.name = otherName;
    c.serial = serial;
    c.distance = dist;
    c.isPreferred = preferredMatch;
    c.indoors = otherIsIndoors;
    c.buildingSerial = otherBuildingSerial;
    c.floor = other->getFloor();
    candidates.push_back(c);
  }

  if (candidates.empty()) {
    Log("BORED_EVENT: skipped (no nearby NPC candidates)");
    return false;
  }

  static bool seeded = false;
  if (!seeded) {
    seeded = true;
    srand((unsigned int)GetTickCount());
  }
  size_t speakerIndex = 0;
  bool speakerResolved = false;
  if (hasPreferred) {
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (candidates[i].isPreferred) {
        speakerIndex = i;
        speakerResolved = true;
        break;
      }
    }
    if (!speakerResolved) {
      Log("BORED_EVENT: preferred speaker not eligible name=" + preferredName +
          " serial=" + preferredSerial);
      return false;
    }
  } else {
    speakerIndex = (size_t)(rand() % candidates.size());
    speakerResolved = true;
  }
  CandidateNpc speaker = candidates[speakerIndex];

  std::string listener = "";
  if (candidates.size() > 1) {
    size_t listenerIndex = speakerIndex;
    float listenerDistance = 1e30f;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (i == speakerIndex) {
        continue;
      }
      if (candidates[i].distance < listenerDistance) {
        listenerDistance = candidates[i].distance;
        listenerIndex = i;
      }
    }
    if (listenerIndex != speakerIndex && listenerIndex < candidates.size()) {
      listener = candidates[listenerIndex].name;
    }
  }
  if (listener.empty()) {
    listener = "Player";
  }

  std::vector<std::string> people;
  std::string playerName = player->getName();
  std::string playerSerial = ToString(player->getHandle().serial);
  if (!playerName.empty() && !playerSerial.empty()) {
    AppendUniquePerson(people, playerName + "|" + playerSerial);
  }
  for (size_t i = 0; i < candidates.size(); ++i) {
    AppendUniquePerson(people, candidates[i].name + "|" + candidates[i].serial);
  }

  std::string peopleJson = "[";
  for (size_t i = 0; i < people.size(); ++i) {
    if (i > 0) {
      peopleJson += ",";
    }
    peopleJson += "\"" + EscapeJSON(people[i]) + "\"";
  }
  peopleJson += "]";

  std::string mode = forceDirectorMode ? "director" : "autochat";
  std::string eventData =
      speaker.name + ": [BORED_EVENT_TRIGGER] (talking to: " + listener + ")";
  std::wstring endpoint =
      L"/StobeServer/stream.php?DATA=" +
      ToWide(BuildStreamQueryData("bored", eventData, ResolveCurrentGameTs())) +
      L"&profile=" + ToWide(UrlEncode(speaker.name)) +
      L"&mode=" + ToWide(UrlEncode(mode)) +
      L"&tts_enabled=" + (g_ttsEnabled ? L"1" : L"0") +
      L"&people=" + ToWide(UrlEncode(peopleJson));
  AppendGeoQueryFromPlayer(endpoint, player);

  StreamChatTask *task = new StreamChatTask();
  task->endpoint = endpoint;
  task->npcName = speaker.name;
  task->handleStr = speaker.serial;
  task->peopleJson = peopleJson;
  task->previousSpeaker = listener;
  if (listener == playerName && !playerSerial.empty()) {
    task->previousSpeakerHandle = playerSerial;
  } else {
    task->previousSpeakerHandle = "";
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (EqualsIgnoreCase(candidates[i].name, listener)) {
        task->previousSpeakerHandle = candidates[i].serial;
        break;
      }
    }
  }
  task->initiatorSpeaker = "";
  task->initiatorSpeakerHandle = "";
  task->requestMode = mode;
  task->generation = generationOverride > 0 ? generationOverride
                                             : GetChatInterruptGeneration();
  task->rechatDepth = 0;

  HANDLE thread = CreateThread(NULL, 0, StreamChatResponseThread, task, 0, NULL);
  if (!thread) {
    delete task;
    Log("BORED_EVENT: failed to start stream thread");
    return false;
  }
  CloseHandle(thread);

  Log("BORED_EVENT: dispatched speaker=" + speaker.name + " listener=" +
      listener + " mode=" + mode + " people_count=" +
      ToString((int)people.size()) + " speaker_indoors=" +
      std::string(speaker.indoors ? "true" : "false") + " speaker_building=" +
      ToString((int)speaker.buildingSerial) + " speaker_floor=" +
      ToString(speaker.floor) + " gen=" +
      ToString((int)task->generation));
  return true;
}

void OnChatWindowButtonPressed(MyGUI::Window *sender, const std::string &name) {
  if (name == "close")
    CloseChatUI();
}

void CreateChatUI(const std::string &npcName, const std::string &playerName,
                  const std::string &handleStr) {
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui)
    return;
  if (g_chatWindow)
    CloseChatUI();

  g_chatTargetNameStr = npcName;
  g_chatPlayerNameStr = playerName;
  g_chatTargetHandleStr = handleStr;
  g_chatJustOpened = true;
  g_chatPausedGame = false;

  GameWorld *world = GetWorldSafe();
  if (world) {
    Character *targetNpc = ResolveChatTargetCharacter(world, npcName, handleStr);
    Character *bestSpeaker = ResolveConfiguredPlayerSpeaker(world, targetNpc);
    if (bestSpeaker && (uintptr_t)bestSpeaker > 0x1000) {
      g_chatPlayerNameStr = bestSpeaker->getName();
    }
  }
  if (world && !world->isPaused()) {
    world->userPause(true);
    g_chatPausedGame = true;
  }
  if (g_chatPlayerNameStr.empty()) {
    g_chatPlayerNameStr = playerName.empty() ? "Player" : playerName;
  }

  std::string actualNpcName = npcName;
  // Identity renames are queued and handled asynchronously by
  // RenameWorker, so CreateChatUI never blocks on HTTP.
  const float chatWindowW = 0.42f;
  const float chatWindowH = 0.18f;
  const float chatWindowX = (1.0f - chatWindowW) * 0.5f;
  const float chatWindowY = (1.0f - chatWindowH) * 0.5f;
  g_chatWindow = gui->createWidgetReal<MyGUI::Window>(
      "Kenshi_WindowCX", chatWindowX, chatWindowY, chatWindowW, chatWindowH,
      MyGUI::Align::Top | MyGUI::Align::Left,
      "Overlapped", "Stobe_ChatWindow");
  g_chatWindow->setCaption(WideFromUtf8("Chat Box").c_str());
  g_chatWindow->eventWindowButtonPressed +=
      MyGUI::newDelegate(OnChatWindowButtonPressed);
  MyGUI::Widget *client = g_chatWindow->getClientWidget();
  g_chatLabel = client->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", 0.05f, 0.08f, 0.9f, 0.16f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_ChatLabel");
  g_chatLabel->setCaption(
      WideFromUtf8("Speaker: " + g_chatPlayerNameStr + " -> Target: " + actualNpcName)
          .c_str());
  g_chatInput = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", 0.05f, 0.28f, 0.9f, 0.211f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_ChatInput");

  // Single-line style input.
  g_chatInput->setEditMultiLine(false);
  g_chatInput->setEditWordWrap(false);
  g_chatInput->setVisibleVScroll(false);
  g_chatInput->setTextAlign(MyGUI::Align::Default);
  g_chatInput->setFontHeight(18);

  g_chatInput->eventEditTextChange += MyGUI::newDelegate(OnChatInputChange);
  g_chatInput->eventEditSelectAccept += MyGUI::newDelegate(OnChatInputAccept);
  MyGUI::InputManager::getInstance().setKeyFocusWidget(g_chatInput);

  const float inputY = 0.28f;
  const float inputH = 0.211f;
  const float rowGap = 0.03f;
  const float rowH = 0.20f;
  const float topRowY = inputY + inputH + rowGap;
  const float bottomRowY = topRowY + rowH + rowGap;
  const float bottomRowLeftX = 0.05f;
  const float bottomRowGap = 0.02f;
  const float modeWidth = 0.20f;
  const float actionWidth = 0.30f;
  const float actionArgWidth = 0.14f;
  const float sendWidth = 0.20f;
  const float actionX = bottomRowLeftX + modeWidth + bottomRowGap;
  const float actionArgX = actionX + actionWidth + bottomRowGap;
  const float sendX = actionArgX + actionArgWidth + bottomRowGap;

  g_chatModeCombo = client->createWidgetReal<MyGUI::ComboBox>(
      "Kenshi_ComboBox", bottomRowLeftX, bottomRowY, modeWidth, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_ChatModeCombo");
  g_chatModeCombo->setComboModeDrop(true);
  g_chatModeCombo->addItem(WideFromUtf8("chat").c_str());
  g_chatModeCombo->addItem(WideFromUtf8("whisper").c_str());
  g_chatModeCombo->addItem(WideFromUtf8("shout").c_str());
  g_chatModeCombo->addItem(WideFromUtf8("cheat").c_str());
  g_chatModeCombo->eventComboAccept += MyGUI::newDelegate(OnChatModeChange);
  g_chatModeCombo->eventComboChangePosition +=
      MyGUI::newDelegate(OnChatModeChange);

  g_chatActionCombo = client->createWidgetReal<MyGUI::ComboBox>(
      "Kenshi_ComboBox", actionX, bottomRowY, actionWidth, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_ChatActionCombo");
  g_chatActionCombo->setComboModeDrop(true);
  for (size_t i = 0; i < ManualChatActionChoiceCount(); ++i) {
    g_chatActionCombo->addItem(
        WideFromUtf8(kManualChatActionChoices[i].label).c_str());
  }
  g_chatActionCombo->setIndexSelected(0);
  g_chatActionCombo->eventComboAccept += MyGUI::newDelegate(OnChatActionChange);
  g_chatActionCombo->eventComboChangePosition +=
      MyGUI::newDelegate(OnChatActionChange);

  g_chatActionArgInput = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", actionArgX, bottomRowY, actionArgWidth, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_ChatActionArgInput");
  g_chatActionArgInput->setEditMultiLine(false);
  g_chatActionArgInput->setEditWordWrap(false);
  g_chatActionArgInput->setVisibleVScroll(false);
  g_chatActionArgInput->setTextAlign(MyGUI::Align::Default);
  g_chatActionArgInput->setFontHeight(16);
  g_chatActionArgInput->setCaption("");
  OnChatActionChange(g_chatActionCombo, g_chatActionCombo->getIndexSelected());

  g_chatAutoChatToggle = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, topRowY, 0.28f, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_ChatAutoToggle");
  g_chatAutoChatToggle->eventMouseButtonClick +=
      MyGUI::newDelegate(OnAutoChatToggleClick);

  RefreshChatModeControls();

  MyGUI::Button *sendBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", sendX, bottomRowY, sendWidth, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_ChatSendBtn");
  sendBtn->setCaption(WideFromUtf8(T("Send")).c_str());
  sendBtn->eventMouseButtonClick += MyGUI::newDelegate(OnChatSendClick);

  MyGUI::Button *boredEventBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.36f, topRowY, 0.28f, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left,
      "Stobe_ChatBoredBtn");
  boredEventBtn->setCaption(WideFromUtf8(T("Trigger Bored Event")).c_str());
  boredEventBtn->eventMouseButtonClick += MyGUI::newDelegate(OnBoredEventClick);

  MyGUI::Button *writeDiaryBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.67f, topRowY, 0.28f, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left,
      "Stobe_ChatDiaryBtn");
  writeDiaryBtn->setCaption(WideFromUtf8(T("Write Diary")).c_str());
  writeDiaryBtn->eventMouseButtonClick += MyGUI::newDelegate(OnWriteDiaryClick);
}

void OnChatModeChange(MyGUI::ComboBox *sender, size_t index) {
  if (!sender || index == MyGUI::ITEM_NONE)
    return;

  std::string selectedMode = sender->getItemNameAt(index);
  g_chatMode = NormalizeChatMode(selectedMode);
  g_lastChatModeIndex = ChatModeToIndex(g_chatMode);
  SaveStobeRuntimeConfig();
  RefreshChatModeControls();
}

void OnChatActionChange(MyGUI::ComboBox *sender, size_t index) {
  if (!sender || index == MyGUI::ITEM_NONE) {
    return;
  }
  size_t safeIndex = SanitizeManualChatActionIndex(index);
  if (sender->getIndexSelected() != safeIndex) {
    sender->setIndexSelected(safeIndex);
  }
  const ManualChatActionChoice &choice = GetManualChatActionChoice(safeIndex);
  if (!g_chatActionArgInput) {
    return;
  }
  if (choice.requiresAmount) {
    int parsedAmount = 0;
    std::string currentValue = g_chatActionArgInput->getCaption().asUTF8();
    if (!TryParseActionAmount(currentValue, parsedAmount)) {
      g_chatActionArgInput->setCaption("100");
    }
  } else {
    g_chatActionArgInput->setCaption("");
  }
}

void OnAutoChatToggleClick(MyGUI::Widget *sender) {
  g_autoChatEnabled = !g_autoChatEnabled;
  SaveStobeRuntimeConfig();
  RefreshChatModeControls();
}

void RefreshChatModeControls() {
  if (g_chatModeCombo) {
    g_chatMode = NormalizeChatMode(g_chatMode);
    g_lastChatModeIndex = ChatModeToIndex(g_chatMode);
    if (g_chatModeCombo->getIndexSelected() != g_lastChatModeIndex) {
      g_chatModeCombo->setIndexSelected(g_lastChatModeIndex);
    }
  }

  if (g_chatAutoChatToggle) {
    g_chatAutoChatToggle->setCaption(
        WideFromUtf8(std::string("Auto Chat: ") +
                   (g_autoChatEnabled ? "[ON]" : "[OFF]"))
            .c_str());
  }
}

void SendChatToStobeServer(GameWorld *world, Character *sel,
                      const std::string &npcName, const std::string &playerName,
                      const std::string &text, const std::string &mode,
                      const std::string &npcsJson,
                      const std::string &nearbyFullJson) {
  Character *targetNpc = nullptr;
  if (world) {
    const ogre_unordered_set<Character *>::type &chars =
        world->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it) {
      if ((*it)) {
        std::string cn = (*it)->getName();
        if (cn == npcName) {
          targetNpc = *it;
          break;
        }
        // Handle piped names
        size_t p = npcName.find('|');
        if (p != std::string::npos && cn == npcName.substr(0, p)) {
          targetNpc = *it;
          break;
        }
        if (cn.empty() || cn == "Unknown Entity") {
          std::string dn = (*it)->displayName;
          if (!dn.empty() && (dn == npcName || (p != std::string::npos &&
                                                dn == npcName.substr(0, p)))) {
            targetNpc = *it;
            break;
          }
        }
      }
    }
  }

  std::string detailedContext = "{}";
  if (targetNpc)
    detailedContext = BuildNpcContextEnvelope(targetNpc);
  else if (sel && sel->getName() == npcName)
    detailedContext = BuildNpcContextEnvelope(sel);

  std::string json =
      "{\"npc\": \"" + EscapeJSON(npcName) + "\", \"npcs\": [" + npcsJson +
      "], \"nearby\": [" + nearbyFullJson + "], \"message\": \"" +
      EscapeJSON(text) + "\", \"player\": \"" + EscapeJSON(playerName) +
      "\", \"mode\": \"" + mode + "\", \"tts_enabled\": " +
      std::string(g_ttsEnabled ? "true" : "false") +
      ", \"context\": " + detailedContext + "}";
  AsyncPostToStobe(L"/chat", json);
}

} // namespace UI
} // namespace Stobe

