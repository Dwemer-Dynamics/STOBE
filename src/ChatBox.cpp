#include "ChatBox.h"
#include "AudioPlayback.h"
#include "Comm.h"
#include "Context.h"
#include "Functions.h"
#include "Globals.h"
#include "StobeChatMode.h"
#include "StobeTiming.h"
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
#include <mygui/MyGUI_RenderManager.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Window.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <set>
#include <vector>

namespace Stobe {
namespace UI {

MyGUI::Window *g_chatWindow = nullptr;
MyGUI::EditBox *g_chatInput = nullptr;
MyGUI::ComboBox *g_chatModeCombo = nullptr;
MyGUI::ComboBox *g_chatTargetCombo = nullptr;
MyGUI::ComboBox *g_chatActionCombo = nullptr;
MyGUI::ComboBox *g_chatProfileModelCombo = nullptr;
MyGUI::EditBox *g_chatActionArgInput = nullptr;
MyGUI::Button *g_chatAutoChatToggle = nullptr;
MyGUI::TextBox *g_chatLabel = nullptr;
MyGUI::Window *g_renameWindow = nullptr;
MyGUI::EditBox *g_renameInput = nullptr;
MyGUI::TextBox *g_renameLabel = nullptr;
std::string g_chatTargetHandleStr = "";
std::string g_chatTargetNameStr = "";
std::string g_chatLastRealTargetHandleStr = "";
std::string g_chatLastRealTargetNameStr = "";
std::string g_chatPlayerNameStr = "";
std::string g_renameTargetNameStr = "";
std::string g_renameTargetHandleStr = "";
std::string g_renameSpeakerNameStr = "";
size_t g_lastChatModeIndex = 1;
bool g_chatJustOpened = false;
bool g_chatPausedGame = false;
bool g_renamePausedGame = false;
bool g_chatTargetRefreshInProgress = false;
LONG g_activeChatStreamCount = 0;
LONG g_profileModelSlot = 1;
LONG g_profileModelRevision = 0;
LONG g_profileModelRefreshInFlight = 0;
DWORD g_profileModelLastRefreshTick = 0;
bool g_profileModelComboRefreshInProgress = false;
const float kWhisperRangeUnits = 20.0f;
const float kCheatRangeUnits = 100.0f;
const char *kNarratorName = "The Narrator";
const char *kProfileModelLabels[] = {
    "Standard", "Fast", "Powerful", "Experimental"};

class ActiveChatStreamScope {
public:
  ActiveChatStreamScope() { InterlockedIncrement(&g_activeChatStreamCount); }
  ~ActiveChatStreamScope() { InterlockedDecrement(&g_activeChatStreamCount); }
};

struct ChatTargetOption {
  std::string name;
  std::string handle;
  std::string label;
  float distance;
  bool isNarrator;
};

struct ProfileModelRequestTask {
  bool write;
  int requestedSlot;
  int fallbackSlot;
  LONG revision;
};

std::vector<ChatTargetOption> g_chatTargetOptions;

std::string TrimChatLine(const std::string &value);
Character *ResolveChatTargetCharacter(GameWorld *world,
                                      const std::string &targetName,
                                      const std::string &handleHint);
bool EqualsIgnoreCase(const std::string &lhs, const std::string &rhs);
void CloseRenameUI();
void CreateRenameUI(const std::string &targetName,
                    const std::string &speakerName,
                    const std::string &targetHandle);
void OnRenameClick(MyGUI::Widget *sender);
void OnRenameInputChange(MyGUI::EditBox *sender);
void OnRenameInputAccept(MyGUI::EditBox *sender);
void OnRenameConfirmClick(MyGUI::Widget *sender);
void OnRenameCancelClick(MyGUI::Widget *sender);
void OnRenameWindowButtonPressed(MyGUI::Window *sender,
                                 const std::string &name);

int NormalizeProfileModelSlot(int slot) {
  return slot >= 1 && slot <= 4 ? slot : 1;
}

int GetActiveProfileModelSlot() {
  return NormalizeProfileModelSlot(
      static_cast<int>(InterlockedCompareExchange(&g_profileModelSlot, 0, 0)));
}

std::string GetActiveProfileModelLabel() {
  return kProfileModelLabels[GetActiveProfileModelSlot() - 1];
}

void QueueProfileModelSlotUpdate(LONG revision, int slot) {
  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.push_back("CMD: SET_PROFILE_MODEL:" + ToString((int)revision) +
                           "|" + ToString(NormalizeProfileModelSlot(slot)));
  LeaveCriticalSection(&g_msgMutex);
}

void QueueProfileModelNotification(const std::string &message) {
  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.push_back("NOTIFY:" + message);
  LeaveCriticalSection(&g_msgMutex);
}

// Confirms model-slot reads and writes off the game thread before updating UI.
DWORD WINAPI ProfileModelRequestThread(LPVOID lpParam) {
  ProfileModelRequestTask *task =
      static_cast<ProfileModelRequestTask *>(lpParam);
  if (!task) {
    return 0;
  }

  std::string response;
  if (task->write) {
    const std::string payload =
        "{\"id\":\"stobe_profile_model\",\"value\":\"" +
        ToString(task->requestedSlot) + "\",\"only_if_changed\":true}";
    response = PostToStobeWithResponse(L"/conf_opts", payload);
  } else {
    response = PostToStobeWithResponse(
        L"/conf_opts?id=stobe_profile_model", "");
  }

  const std::string status = TrimChatLine(JsonReadField(response, "status"));
  const int responseSlot =
      atoi(TrimChatLine(JsonReadField(response, "value")).c_str());
  const bool validResponse =
      status == "ok" && responseSlot >= 1 && responseSlot <= 4 &&
      (!task->write || responseSlot == task->requestedSlot);

  if (validResponse) {
    QueueProfileModelSlotUpdate(task->revision, responseSlot);
    Log("PROFILE_MODEL: synchronized slot=" + ToString(responseSlot) +
        " source=" + std::string(task->write ? "chatbox" : "server"));
  } else if (task->write) {
    QueueProfileModelSlotUpdate(task->revision, task->fallbackSlot);
    QueueProfileModelNotification(
        "Unable to change the response mode. Check StobeServer.");
    Log("PROFILE_MODEL: change failed requested=" +
        ToString(task->requestedSlot));
  } else if (!response.empty()) {
    Log("PROFILE_MODEL: ignored invalid server response");
  }

  if (!task->write) {
    InterlockedExchange(&g_profileModelRefreshInFlight, 0);
  }
  delete task;
  return 0;
}

bool StartProfileModelRequest(ProfileModelRequestTask *task) {
  HANDLE thread =
      CreateThread(NULL, 0, ProfileModelRequestThread, task, 0, NULL);
  if (!thread) {
    if (!task->write) {
      InterlockedExchange(&g_profileModelRefreshInFlight, 0);
    }
    delete task;
    Log("PROFILE_MODEL: failed to start request thread");
    return false;
  }
  CloseHandle(thread);
  return true;
}

void RequestProfileModelSlotRefresh(bool force) {
  const DWORD now = GetTickCount();
  if (!force && g_profileModelLastRefreshTick != 0 &&
      now - g_profileModelLastRefreshTick < 30000) {
    return;
  }
  if (InterlockedCompareExchange(&g_profileModelRefreshInFlight, 1, 0) != 0) {
    return;
  }

  g_profileModelLastRefreshTick = now;
  ProfileModelRequestTask *task = new ProfileModelRequestTask();
  task->write = false;
  task->requestedSlot = 0;
  task->fallbackSlot = GetActiveProfileModelSlot();
  task->revision =
      InterlockedCompareExchange(&g_profileModelRevision, 0, 0);
  StartProfileModelRequest(task);
}

void ApplyProfileModelSlotUpdate(const std::string &data) {
  const size_t separator = data.find('|');
  if (separator == std::string::npos) {
    return;
  }
  const LONG revision = atol(data.substr(0, separator).c_str());
  const int slot = atoi(data.substr(separator + 1).c_str());
  if (slot < 1 || slot > 4 ||
      revision !=
          InterlockedCompareExchange(&g_profileModelRevision, 0, 0)) {
    return;
  }

  InterlockedExchange(&g_profileModelSlot, slot);
  if (g_chatProfileModelCombo) {
    g_profileModelComboRefreshInProgress = true;
    g_chatProfileModelCombo->setIndexSelected(static_cast<size_t>(slot - 1));
    g_profileModelComboRefreshInProgress = false;
  }
}

bool TryReleaseUserPauseSafe(GameWorld *world) {
  if (!world) {
    return true;
  }
  __try {
    world->userPause(false);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool TryRequestUserPauseSafe(GameWorld *world, bool *pausedByUs) {
  if (pausedByUs) {
    *pausedByUs = false;
  }
  if (!world) {
    return true;
  }
  __try {
    if (!world->isPaused()) {
      world->userPause(true);
      if (pausedByUs) {
        *pausedByUs = true;
      }
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool TryDestroyWidgetSafe(MyGUI::Widget *widget) {
  if (!widget) {
    return true;
  }
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) {
    return true;
  }
  __try {
    gui->destroyWidget(widget);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

float ScreenPixelsToRealHeight(int pixels) {
  MyGUI::RenderManager *renderManager = MyGUI::RenderManager::getInstancePtr();
  if (!renderManager || pixels <= 0) {
    return 0.0f;
  }

  const MyGUI::IntSize &viewSize = renderManager->getViewSize();
  if (viewSize.height <= 0) {
    return 0.0f;
  }

  return (float)pixels / (float)viewSize.height;
}

float ParentPixelsToRealHeight(MyGUI::Widget *parent, int pixels) {
  if (!parent || pixels <= 0) {
    return 0.0f;
  }

  int parentHeight = parent->getHeight();
  if (parentHeight <= 0) {
    return 0.0f;
  }

  return (float)pixels / (float)parentHeight;
}

std::string BuildStorageIdForCharacter(Character *character) {
  if (!character || (uintptr_t)character <= 0x1000) {
    return "";
  }
  try {
    unsigned int serial = character->getHandle().serial;
    if (serial == 0) {
      return "";
    }
    return "hand_" + ToString(serial);
  } catch (...) {
    return "";
  }
}

int ResolveCurrentGameTsSafe(GameWorld *world) {
  if (!world) {
    return 0;
  }
  int gameTs = 0;
  try {
    TimeOfDay tod = world->getTimeStamp_inGameHours();
    gameTs = (int)tod.getTotalSeconds();
  } catch (...) {
    gameTs = 0;
  }
  if (gameTs < 0) {
    gameTs = 0;
  }
  return gameTs;
}

bool TryRenameCharacterAndSync(GameWorld *world, const std::string &targetNameHint,
                               const std::string &targetHandleHint,
                               const std::string &newNameRaw,
                               std::string &statusMessage) {
  statusMessage.clear();
  if (!world) {
    statusMessage = "Rename failed: world is unavailable.";
    return false;
  }

  std::string newName = TrimChatLine(newNameRaw);
  if (newName.empty()) {
    statusMessage = "Rename failed: provide a name.";
    return false;
  }

  Character *target = ResolveChatTargetCharacter(world, targetNameHint, targetHandleHint);
  if (!target || (uintptr_t)target <= 0x1000) {
    target = ResolveChatTargetCharacter(world, targetNameHint, "");
  }
  if (!target || (uintptr_t)target <= 0x1000) {
    statusMessage = "Rename failed: target not found.";
    return false;
  }

  std::string oldName = targetNameHint;
  try {
    std::string actualName = target->getName();
    if (!actualName.empty()) {
      oldName = actualName;
    }
  } catch (...) {
  }
  if (oldName.empty()) {
    oldName = "Unknown NPC";
  }

  if (EqualsIgnoreCase(oldName, newName)) {
    statusMessage = oldName + " already has that name.";
    return false;
  }

  try {
    target->setName(newName);
  } catch (...) {
    statusMessage = "Rename failed: unable to rename in-game target.";
    return false;
  }

  std::string storageId = BuildStorageIdForCharacter(target);
  std::string contextJson = BuildNpcContextEnvelope(target);
  if (contextJson.empty()) {
    contextJson = "{}";
  }
  int gameTs = ResolveCurrentGameTsSafe(world);

  std::string renJson =
      "{\"old_name\": \"" + EscapeJSON(oldName) + "\", "
      "\"new_name\": \"" + EscapeJSON(newName) + "\", "
      "\"storage_id\": \"" + EscapeJSON(storageId) + "\", "
      "\"game_ts\": " + ToString(gameTs) + ", "
      "\"context\": " + contextJson + "}";
  AsyncPostToStobe(L"/rename", renJson);

  g_chatPlayerNameStr = newName;
  g_renameTargetNameStr = newName;
  g_renameSpeakerNameStr = newName;
  statusMessage = oldName + " has been renamed to " + newName;
  Log("RENAME: " + statusMessage + " storage_id=" + storageId);
  return true;
}

enum ManualChatActionType {
  MANUAL_CHAT_ACTION_NONE = 0,
  MANUAL_CHAT_ACTION_REMOVE_LIMB = 1,
  MANUAL_CHAT_ACTION_CUT_HORNS = 2,
  MANUAL_CHAT_ACTION_GIVE_CATS = 3,
  MANUAL_CHAT_ACTION_GIVE_ITEM = 4,
  MANUAL_CHAT_ACTION_ROLEPLAY_ACTION = 5,
  MANUAL_CHAT_ACTION_DRINK_ITEM = 6,
  MANUAL_CHAT_ACTION_USE_DRUGS = 7,
  MANUAL_CHAT_ACTION_KILL = 8,
  MANUAL_CHAT_ACTION_FORCE_DRINK = 9,
  MANUAL_CHAT_ACTION_KNOCKOUT = 10
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
    {MANUAL_CHAT_ACTION_FORCE_DRINK, "force drink item", "", "", false},
    {MANUAL_CHAT_ACTION_USE_DRUGS, "use drugs", "", "", false},
    {MANUAL_CHAT_ACTION_REMOVE_LIMB, "remove left arm (hacksaw)",
     "remove_limb_left_arm", "LEFT_ARM", false},
    {MANUAL_CHAT_ACTION_REMOVE_LIMB, "remove right arm (hacksaw)",
     "remove_limb_right_arm", "RIGHT_ARM", false},
    {MANUAL_CHAT_ACTION_REMOVE_LIMB, "remove left leg (hacksaw)",
     "remove_limb_left_leg", "LEFT_LEG", false},
    {MANUAL_CHAT_ACTION_REMOVE_LIMB, "remove right leg (hacksaw)",
     "remove_limb_right_leg", "RIGHT_LEG", false},
    {MANUAL_CHAT_ACTION_CUT_HORNS, "cut horns (hacksaw)", "cut_horns", "",
     false},
    {MANUAL_CHAT_ACTION_KNOCKOUT, "knockout", "knockout", "", false},
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

std::string BuildManualCutHornsActionToken(const std::string &targetName,
                                           const std::string &targetHandle) {
  std::string normalizedTargetName = targetName;
  if (normalizedTargetName.empty()) {
    return "";
  }
  std::string targetToken = normalizedTargetName;
  if (!targetHandle.empty()) {
    targetToken += "|" + targetHandle;
  }
  return "CUT_HORNS@" + targetToken;
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

std::string BuildManualForceDrinkActionToken(const std::string &targetName,
                                             const std::string &targetHandle,
                                             const std::string &itemName) {
  std::string normalizedTargetName = TrimManualActionArg(targetName);
  std::string normalizedItemName = TrimManualActionArg(itemName);
  if (normalizedTargetName.empty()) {
    return "";
  }
  if (normalizedItemName.empty()) {
    normalizedItemName = "Cactus Rum";
  }
  std::string targetToken = normalizedTargetName;
  if (!targetHandle.empty()) {
    targetToken += "|" + targetHandle;
  }
  return "FORCE_DRINK@" + targetToken + "@" + normalizedItemName;
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

std::string BuildManualKnockoutActionToken(const std::string &targetName,
                                           const std::string &targetHandle) {
  std::string normalizedTargetName = TrimManualActionArg(targetName);
  if (normalizedTargetName.empty()) {
    return "";
  }
  std::string targetToken = normalizedTargetName;
  if (!targetHandle.empty()) {
    targetToken += "|" + targetHandle;
  }
  return "KNOCKOUT@" + targetToken;
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

bool IsNarratorName(const std::string &name) {
  return EqualsIgnoreCase(name, kNarratorName);
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

float ResolveChatInteractionDistance(Character *speaker, Character *target) {
  if (!speaker || !target || (uintptr_t)speaker <= 0x1000 ||
      (uintptr_t)target <= 0x1000) {
    return -1.0f;
  }
  float distance = speaker->getPosition().distance(target->getPosition());
  if (!IsAnimalCharacterSafe(target, nullptr)) {
    return distance;
  }
  try {
    float combinedRadius = speaker->getRadius() + target->getRadius();
    if (combinedRadius > 0.0f) {
      distance -= combinedRadius;
    }
  } catch (...) {
  }
  return distance > 0.0f ? distance : 0.0f;
}

bool IsCharacterUnavailableForConversation(Character *character) {
  if (!character || (uintptr_t)character <= 0x1000) {
    return true;
  }
  try {
    if (character->isDead() || character->isUnconcious()) {
      return true;
    }
  } catch (...) {
    return true;
  }
  try {
    if (character->isDown()) {
      return true;
    }
  } catch (...) {
    return true;
  }
  return false;
}

bool IsDigitsOnlyToken(const std::string &value) {
  if (value.empty()) {
    return false;
  }
  for (size_t i = 0; i < value.size(); ++i) {
    unsigned char ch = (unsigned char)value[i];
    if (ch < '0' || ch > '9') {
      return false;
    }
  }
  return true;
}

std::string BuildTalkTargetMetadataToken(const std::string &listenerName,
                                         const std::string &listenerHandle) {
  std::string normalizedHandle = TrimChatLine(listenerHandle);
  if (IsDigitsOnlyToken(normalizedHandle)) {
    return "serial:" + normalizedHandle;
  }

  std::string normalizedName = TrimChatLine(listenerName);
  if (normalizedName.empty()) {
    return "";
  }
  for (size_t i = 0; i < normalizedName.size(); ++i) {
    if (normalizedName[i] == '[') {
      normalizedName[i] = '(';
    } else if (normalizedName[i] == ']') {
      normalizedName[i] = ')';
    }
  }
  return TrimChatLine(normalizedName);
}

std::string ResolveConversationStateSuffix(Character *character) {
  if (!character || (uintptr_t)character <= 0x1000) {
    return "";
  }
  bool isDead = false;
  bool isUnconscious = false;
  bool isKnockedOut = false;
  try {
    isDead = character->isDead();
  } catch (...) {
    isDead = false;
  }
  if (isDead) {
    return " [DEAD]";
  }
  try {
    isUnconscious = character->isUnconcious();
  } catch (...) {
    isUnconscious = false;
  }
  if (isUnconscious) {
    return " [UNCONSCIOUS]";
  }
  try {
    isKnockedOut = character->isDown();
  } catch (...) {
    isKnockedOut = false;
  }
  if (isKnockedOut) {
    return " [KNOCKED OUT]";
  }
  return "";
}

float GetSearchRadiusForMode(const std::string &mode) {
  if (mode == "whisper")
    return kWhisperRangeUnits;
  if (mode == "shout")
    return g_shoutRadius;
  if (mode == "cheat")
    return kCheatRangeUnits;
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
  if (g_enableAnimalTalks && IsAnimalCharacterSafe(candidate, nullptr)) {
    // Outdoor wildlife can report terrain levels as floors. Distance remains
    // authoritative when neither participant is inside a building.
    return true;
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
  const bool narratorMode = (mode == "narrator");
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
    if (!targetHandle.empty() && !narratorMode)
      AppendUniquePerson(people, targetName + "|" + targetHandle);
    else
      AppendUniquePerson(people, targetName);
  }

  if (narratorMode) {
    AppendUniquePerson(people, kNarratorName);
  }

  if (player && mode != "whisper" && mode != "narrator") {
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

std::string BuildChatTargetLabel(GameWorld *world, const std::string &targetName,
                                 const std::string &targetHandle) {
  std::string label = targetName.empty() ? "Unknown" : targetName;
  if (!world) {
    return label;
  }
  Character *target = ResolveChatTargetCharacter(world, targetName, targetHandle);
  if (!target || (uintptr_t)target <= 0x1000) {
    return label;
  }
  try {
    std::string resolvedName = target->getName();
    if (!resolvedName.empty()) {
      label = resolvedName;
    }
  } catch (...) {
  }
  return label + ResolveConversationStateSuffix(target);
}

Character *ResolveSelectedChatSpeaker(GameWorld *world) {
  if (!world || !world->player) {
    return nullptr;
  }
  const hand &selectedHandle = world->player->selectedCharacter;
  if (!selectedHandle.isValid() || selectedHandle.serial == 0) {
    return nullptr;
  }
  return ResolveChatTargetCharacter(world, "", ToString(selectedHandle.serial));
}

Character *ResolveNearestPlayerSpeaker(GameWorld *world, Character *target) {
  if (!world || !world->player || world->player->playerCharacters.size() == 0) {
    return nullptr;
  }

  Character *fallback = nullptr;
  for (uint32_t i = 0; i < world->player->playerCharacters.size(); ++i) {
    Character *candidate = world->player->playerCharacters[i];
    if (!candidate || (uintptr_t)candidate <= 0x1000) {
      continue;
    }
    if (IsCharacterUnavailableForConversation(candidate)) {
      continue;
    }
    fallback = candidate;
    break;
  }
  if (!fallback) {
    fallback = world->player->playerCharacters[0];
  }
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
    if (IsCharacterUnavailableForConversation(candidate)) {
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
  Character *primary = world->player->playerCharacters[0];
  if (g_useNearestPlayerSpeaker) {
    Character *nearest = ResolveNearestPlayerSpeaker(world, target);
    if (nearest && !IsCharacterUnavailableForConversation(nearest)) {
      return nearest;
    }
  } else if (primary && !IsCharacterUnavailableForConversation(primary)) {
    return primary;
  }

  for (uint32_t i = 0; i < world->player->playerCharacters.size(); ++i) {
    Character *candidate = world->player->playerCharacters[i];
    if (!candidate || (uintptr_t)candidate <= 0x1000) {
      continue;
    }
    if (!IsCharacterUnavailableForConversation(candidate)) {
      return candidate;
    }
  }
  return primary;
}

Character *ResolveSelectedOrConfiguredPlayerSpeaker(GameWorld *world,
                                                    Character *target) {
  Character *selected = ResolveSelectedChatSpeaker(world);
  if (selected && (uintptr_t)selected > 0x1000) {
    bool isPlayerCharacter = false;
    try {
      isPlayerCharacter = selected->isPlayerCharacter();
    } catch (...) {
      isPlayerCharacter = false;
    }
    if (isPlayerCharacter && !IsCharacterUnavailableForConversation(selected)) {
      return selected;
    }
  }
  return ResolveConfiguredPlayerSpeaker(world, target);
}

bool ResolveActiveChatSpeaker(GameWorld *world, const std::string &targetName,
                              const std::string &targetHandle,
                              std::string &speakerNameOut,
                              std::string &speakerHandleOut) {
  speakerNameOut.clear();
  speakerHandleOut.clear();
  if (!world) {
    return false;
  }

  Character *targetNpc = ResolveChatTargetCharacter(world, targetName, targetHandle);
  Character *speakerNpc = ResolveSelectedOrConfiguredPlayerSpeaker(world, targetNpc);
  if (!speakerNpc || (uintptr_t)speakerNpc <= 0x1000) {
    std::string preferredSpeakerName = TrimChatLine(g_chatPlayerNameStr);
    if (!preferredSpeakerName.empty() && !IsNarratorName(preferredSpeakerName)) {
      speakerNpc = ResolveChatTargetCharacter(world, preferredSpeakerName, "");
    }
  }
  if (!speakerNpc || (uintptr_t)speakerNpc <= 0x1000) {
    return false;
  }

  try {
    speakerNameOut = TrimChatLine(speakerNpc->getName());
    speakerHandleOut = ToString(speakerNpc->getHandle().serial);
  } catch (...) {
    speakerNameOut.clear();
    speakerHandleOut.clear();
  }
  if (speakerNameOut.empty() || IsNarratorName(speakerNameOut)) {
    return false;
  }
  return true;
}

struct RechatResponderChoice {
  std::string name;
  std::string serial;
  std::string source;
};

bool TrySelectRechatResponder(GameWorld *world, Character *player,
                              Character *speakerNpc,
                              const std::string &speakerName,
                              const std::string &preferredName,
                              const std::string &preferredHandle,
                              RechatResponderChoice &outChoice) {
  outChoice.name.clear();
  outChoice.serial.clear();
  outChoice.source.clear();
  if (!world || !speakerNpc || (uintptr_t)speakerNpc <= 0x1000) {
    return false;
  }

  const float maxPlayerDistance =
      (g_proximityRadius < 10.0f) ? 10.0f : g_proximityRadius;
  const float speakerPriorityDistance = 800.0f;

  auto isEligible = [&](Character *candidate, std::string &nameOut,
                        std::string &serialOut, float &speakerDistanceOut,
                        float &playerDistanceOut) -> bool {
    nameOut.clear();
    serialOut.clear();
    speakerDistanceOut = 0.0f;
    playerDistanceOut = 0.0f;
    if (!candidate || (uintptr_t)candidate <= 0x1000) {
      return false;
    }
    if (candidate == speakerNpc) {
      return false;
    }
    if (player && candidate == player) {
      return false;
    }
    if (IsCharacterUnavailableForConversation(candidate)) {
      return false;
    }
    if (!ShouldIncludeAnimalForTalk(candidate)) {
      return false;
    }
    if (!IsConversationAreaCompatible(speakerNpc, candidate)) {
      return false;
    }

    try {
      nameOut = TrimChatLine(candidate->getName());
      serialOut = ToString(candidate->getHandle().serial);
      speakerDistanceOut =
          speakerNpc->getPosition().distance(candidate->getPosition());
      if (player && (uintptr_t)player > 0x1000) {
        playerDistanceOut = player->getPosition().distance(candidate->getPosition());
      }
    } catch (...) {
      return false;
    }

    if (nameOut.empty() || IsNarratorName(nameOut)) {
      return false;
    }
    if (!speakerName.empty() && EqualsIgnoreCase(nameOut, speakerName)) {
      return false;
    }
    if (player && (uintptr_t)player > 0x1000) {
      if (playerDistanceOut <= 1.0f || playerDistanceOut > maxPlayerDistance) {
        return false;
      }
    }
    return true;
  };

  Character *preferred =
      ResolveChatTargetCharacter(world, preferredName, preferredHandle);
  if (preferred && (uintptr_t)preferred > 0x1000) {
    std::string preferredResolvedName = "";
    std::string preferredResolvedSerial = "";
    float preferredSpeakerDistance = 0.0f;
    float preferredPlayerDistance = 0.0f;
    if (isEligible(preferred, preferredResolvedName, preferredResolvedSerial,
                   preferredSpeakerDistance, preferredPlayerDistance)) {
      outChoice.name = preferredResolvedName;
      outChoice.serial = preferredResolvedSerial;
      outChoice.source = "preferred_listener";
      return true;
    }
  }

  struct Candidate {
    std::string name;
    std::string serial;
    float speakerDistance;
    float playerDistance;
  };
  std::vector<Candidate> candidates;
  std::set<std::string> seenSerials;
  const ogre_unordered_set<Character *>::type &chars =
      world->getCharacterUpdateList();
  for (auto it = chars.begin(); it != chars.end(); ++it) {
    Character *candidate = *it;
    std::string candidateName = "";
    std::string candidateSerial = "";
    float candidateSpeakerDistance = 0.0f;
    float candidatePlayerDistance = 0.0f;
    if (!isEligible(candidate, candidateName, candidateSerial,
                    candidateSpeakerDistance, candidatePlayerDistance)) {
      continue;
    }
    if (!candidateSerial.empty()) {
      if (seenSerials.count(candidateSerial) > 0) {
        continue;
      }
      seenSerials.insert(candidateSerial);
    }
    Candidate entry;
    entry.name = candidateName;
    entry.serial = candidateSerial;
    entry.speakerDistance = candidateSpeakerDistance;
    entry.playerDistance = candidatePlayerDistance;
    candidates.push_back(entry);
  }

  if (candidates.empty()) {
    return false;
  }

  static bool seeded = false;
  if (!seeded) {
    seeded = true;
    srand((unsigned int)GetTickCount());
  }

  std::vector<size_t> firstPass;
  std::vector<size_t> secondPass;
  firstPass.reserve(candidates.size());
  secondPass.reserve(candidates.size());
  for (size_t i = 0; i < candidates.size(); ++i) {
    secondPass.push_back(i);
    if (candidates[i].speakerDistance <= speakerPriorityDistance) {
      firstPass.push_back(i);
    }
  }

  size_t selectedIndex = (size_t)-1;
  std::string selectedSource = "";
  if (!firstPass.empty()) {
    selectedIndex = firstPass[(size_t)(rand() % firstPass.size())];
    selectedSource = "speaker_proximity";
  } else if (!secondPass.empty()) {
    selectedIndex = secondPass[(size_t)(rand() % secondPass.size())];
    selectedSource = "player_distance_fallback";
  }

  if (selectedIndex == (size_t)-1 || selectedIndex >= candidates.size()) {
    return false;
  }

  outChoice.name = candidates[selectedIndex].name;
  outChoice.serial = candidates[selectedIndex].serial;
  outChoice.source = selectedSource;
  return !outChoice.name.empty();
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

  if (IsCharacterUnavailableForConversation(player)) {
    failReason = "Selected speaker cannot talk right now.";
    Log("CHAT_VALIDATE: fail speaker unavailable");
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
    // Allow one-way dialogue attempts to dead/unconscious targets. They will not
    // respond, but the player can still open/send chat context to them.
    Log("CHAT_VALIDATE: target dead_or_unconscious; allowing one_way_send");
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
    float dist = ResolveChatInteractionDistance(player, target);
    Log("CHAT_VALIDATE: cheat distance_check dist=" + ToString(dist) +
        " allowed=" + ToString((int)kCheatRangeUnits));
    if (dist > kCheatRangeUnits) {
      failReason = "Target is out of range for cheat mode (100m maximum).";
      return false;
    }
    Log("CHAT_VALIDATE: bypass area checks for cheat mode within 100m");
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
  float dist = ResolveChatInteractionDistance(player, target);
  Log("CHAT_VALIDATE: distance_check end dist=" + ToString(dist));
  if (dist > allowedRange) {
    failReason = "Target is out of range for " + selectedMode + ".";
    return false;
  }

  Log("CHAT_VALIDATE: pass");
  return true;
}

std::string BuildChatTargetDistanceSuffix(float distance) {
  if (distance < 0.0f) {
    return "";
  }
  int roundedDistance = (int)(distance + 0.5f);
  if (roundedDistance < 0) {
    roundedDistance = 0;
  }
  return " (" + ToString(roundedDistance) + "m)";
}

void SetActiveChatTarget(const std::string &targetName,
                         const std::string &targetHandle,
                         bool rememberRealTarget) {
  g_chatTargetNameStr = TrimChatLine(targetName);
  g_chatTargetHandleStr = TrimChatLine(targetHandle);

  if (rememberRealTarget && !g_chatTargetNameStr.empty() &&
      !IsNarratorName(g_chatTargetNameStr)) {
    g_chatLastRealTargetNameStr = g_chatTargetNameStr;
    g_chatLastRealTargetHandleStr = g_chatTargetHandleStr;
  }

  if (g_chatTargetNameStr.empty() || IsNarratorName(g_chatTargetNameStr)) {
    g_talkTargetHand = hand();
    return;
  }

  GameWorld *world = GetWorldSafe();
  Character *target =
      ResolveChatTargetCharacter(world, g_chatTargetNameStr, g_chatTargetHandleStr);
  if (target && (uintptr_t)target > 0x1000) {
    try {
      g_talkTargetHand = target->getHandle();
      return;
    } catch (...) {
    }
  }

  g_talkTargetHand = hand();
}

bool DoesChatTargetOptionMatch(const ChatTargetOption &option,
                               const std::string &targetName,
                               const std::string &targetHandle) {
  if (option.isNarrator) {
    return IsNarratorName(targetName);
  }
  if (!targetHandle.empty() && option.handle == targetHandle) {
    return true;
  }
  if (!targetName.empty() && EqualsIgnoreCase(option.name, targetName)) {
    return true;
  }
  return false;
}

bool IsDropdownTargetEligible(GameWorld *world, Character *speaker,
                              Character *target,
                              const std::string &selectedMode,
                              float &distanceOut) {
  distanceOut = -1.0f;
  if (!world || !speaker || !target || (uintptr_t)speaker <= 0x1000 ||
      (uintptr_t)target <= 0x1000) {
    return false;
  }

  if (IsCharacterUnavailableForConversation(speaker)) {
    return false;
  }

  if (target == speaker) {
    return false;
  }

  unsigned int targetAnimalSerial = 0;
  bool targetIsAnimal = IsAnimalCharacterSafe(target, &targetAnimalSerial);
  if (targetIsAnimal && !g_enableAnimalTalks) {
    return false;
  }

  try {
    distanceOut = ResolveChatInteractionDistance(speaker, target);
  } catch (...) {
    distanceOut = -1.0f;
    return false;
  }

  if (selectedMode == "cheat") {
    return distanceOut <= kCheatRangeUnits;
  }

  bool areaCompatible = IsConversationAreaCompatible(speaker, target);
  if (!areaCompatible) {
    if (targetIsAnimal) {
      Log("ANIMAL_TALKS: dropdown rejected area name=" + target->getName());
    }
    return false;
  }

  float allowedRange = GetSearchRadiusForMode(selectedMode);
  if (allowedRange < 1.0f) {
    allowedRange = 1.0f;
  }
  if (distanceOut > allowedRange) {
    if (targetIsAnimal) {
      Log("ANIMAL_TALKS: dropdown rejected range name=" + target->getName() +
          " surface_distance=" + ToString(distanceOut) +
          " allowed=" + ToString(allowedRange));
    }
    return false;
  }

  if (targetIsAnimal) {
    Log("ANIMAL_TALKS: dropdown eligible name=" + target->getName() +
        " surface_distance=" + ToString(distanceOut));
  }

  return true;
}

bool TryBuildChatTargetOption(GameWorld *world, Character *candidate,
                              const std::string &selectedMode,
                              ChatTargetOption &optionOut) {
  if (!world || !candidate || (uintptr_t)candidate <= 0x1000) {
    return false;
  }

  std::string candidateName = "";
  try {
    candidateName = TrimChatLine(candidate->getName());
  } catch (...) {
    candidateName.clear();
  }
  if (candidateName.empty()) {
    return false;
  }

  Character *speaker = ResolveSelectedOrConfiguredPlayerSpeaker(world, candidate);
  if (!speaker || (uintptr_t)speaker <= 0x1000) {
    return false;
  }

  float distance = -1.0f;
  if (!IsDropdownTargetEligible(world, speaker, candidate, selectedMode,
                                distance)) {
    return false;
  }

  optionOut.name = candidateName;
  optionOut.handle = ResolveCharacterSerialToken(candidate);
  optionOut.distance = distance;
  optionOut.isNarrator = false;
  optionOut.label =
      candidateName + ResolveConversationStateSuffix(candidate) +
      BuildChatTargetDistanceSuffix(distance);
  return true;
}

void RefreshChatHeaderLabel() {
  if (!g_chatLabel) {
    return;
  }

  GameWorld *world = GetWorldSafe();
  std::string speakerName = g_chatPlayerNameStr.empty() ? "Player" : g_chatPlayerNameStr;
  std::string selectedMode = Stobe::ChatMode::Normalize(g_chatMode);

  if (selectedMode == "narrator") {
    Character *selectedSpeakerNpc = ResolveSelectedChatSpeaker(world);
    if (selectedSpeakerNpc && (uintptr_t)selectedSpeakerNpc > 0x1000) {
      try {
        std::string selectedSpeakerName =
            TrimChatLine(selectedSpeakerNpc->getName());
        if (!selectedSpeakerName.empty()) {
          speakerName = selectedSpeakerName;
          g_chatPlayerNameStr = selectedSpeakerName;
        }
      } catch (...) {
      }
    }
  } else {
    Character *targetNpc = ResolveChatTargetCharacter(
        world, g_chatTargetNameStr, g_chatTargetHandleStr);
    Character *bestSpeaker =
        ResolveSelectedOrConfiguredPlayerSpeaker(world, targetNpc);
    if (bestSpeaker && (uintptr_t)bestSpeaker > 0x1000) {
      try {
        std::string resolvedSpeakerName = TrimChatLine(bestSpeaker->getName());
        if (!resolvedSpeakerName.empty()) {
          speakerName = resolvedSpeakerName;
          g_chatPlayerNameStr = resolvedSpeakerName;
        }
      } catch (...) {
      }
    }
  }

  g_chatLabel->setCaption(WideFromUtf8("Speaker: " + speakerName).c_str());
}

void RefreshAvailableChatTargets(bool preserveSelection) {
  if (!g_chatTargetCombo) {
    return;
  }

  std::string selectedMode = Stobe::ChatMode::Normalize(g_chatMode);
  std::string preferredName = TrimChatLine(g_chatTargetNameStr);
  std::string preferredHandle = TrimChatLine(g_chatTargetHandleStr);

  if (selectedMode == "narrator") {
    preferredName = kNarratorName;
    preferredHandle.clear();
  } else if (preferredName.empty() || IsNarratorName(preferredName)) {
    preferredName = TrimChatLine(g_chatLastRealTargetNameStr);
    preferredHandle = TrimChatLine(g_chatLastRealTargetHandleStr);
  }

  std::vector<ChatTargetOption> options;
  if (selectedMode == "narrator") {
    ChatTargetOption narratorOption;
    narratorOption.name = kNarratorName;
    narratorOption.handle.clear();
    narratorOption.label = GetNarratorDisplayName();
    narratorOption.distance = -1.0f;
    narratorOption.isNarrator = true;
    options.push_back(narratorOption);
  } else {
    GameWorld *world = GetWorldSafe();
    if (world) {
      std::set<std::string> seenTargets;
      auto appendCandidate = [&](Character *candidate) {
        ChatTargetOption option;
        if (!TryBuildChatTargetOption(world, candidate, selectedMode, option)) {
          return;
        }
        std::string targetKey =
            option.handle.empty() ? option.name : option.handle;
        if (targetKey.empty() || seenTargets.count(targetKey) > 0) {
          return;
        }
        seenTargets.insert(targetKey);
        options.push_back(option);
      };

      if (selectedMode == "cheat") {
        Character *speaker =
            ResolveSelectedOrConfiguredPlayerSpeaker(world, nullptr);
        if (speaker && (uintptr_t)speaker > 0x1000) {
          lektor<RootObject *> nearbyResults;
          world->getCharactersWithinSphere(
              nearbyResults, speaker->getPosition(), kCheatRangeUnits, 0.0f,
              0.0f, 0x10, 0, speaker);
          for (uint32_t i = 0; i < nearbyResults.size(); ++i) {
            appendCandidate((Character *)nearbyResults.stuff[i]);
          }
        }
      } else {
        const ogre_unordered_set<Character *>::type &chars =
            world->getCharacterUpdateList();
        for (auto it = chars.begin(); it != chars.end(); ++it) {
          appendCandidate(*it);
        }
      }

      if (g_enableAnimalTalks && selectedMode == "cheat") {
        // Sphere queries used by cheat mode can omit wildlife.
        const ogre_unordered_set<Character *>::type &chars =
            world->getCharacterUpdateList();
        for (auto it = chars.begin(); it != chars.end(); ++it) {
          unsigned int animalSerial = 0;
          if (IsAnimalCharacterSafe(*it, &animalSerial)) {
            appendCandidate(*it);
          }
        }
      }
    }

    std::sort(options.begin(), options.end(),
              [](const ChatTargetOption &lhs,
                 const ChatTargetOption &rhs) -> bool {
                if (lhs.distance != rhs.distance) {
                  return lhs.distance < rhs.distance;
                }
                if (lhs.name != rhs.name) {
                  return lhs.name < rhs.name;
                }
                return lhs.handle < rhs.handle;
              });
  }

  g_chatTargetRefreshInProgress = true;
  g_chatTargetCombo->removeAllItems();
  g_chatTargetOptions = options;

  if (g_chatTargetOptions.empty()) {
    g_chatTargetCombo->addItem(
        WideFromUtf8("No valid targets for " + selectedMode).c_str());
    g_chatTargetCombo->setIndexSelected(0);
    g_chatTargetNameStr.clear();
    g_chatTargetHandleStr.clear();
    g_talkTargetHand = hand();
    g_chatTargetRefreshInProgress = false;
    return;
  }

  for (size_t i = 0; i < g_chatTargetOptions.size(); ++i) {
    g_chatTargetCombo->addItem(
        WideFromUtf8(g_chatTargetOptions[i].label).c_str());
  }

  size_t selectedIndex = 0;
  if (preserveSelection) {
    for (size_t i = 0; i < g_chatTargetOptions.size(); ++i) {
      if (DoesChatTargetOptionMatch(g_chatTargetOptions[i], preferredName,
                                    preferredHandle)) {
        selectedIndex = i;
        break;
      }
    }
  }

  g_chatTargetCombo->setIndexSelected(selectedIndex);
  const ChatTargetOption &selectedOption = g_chatTargetOptions[selectedIndex];
  SetActiveChatTarget(selectedOption.name, selectedOption.handle,
                      !selectedOption.isNarrator);
  g_chatTargetRefreshInProgress = false;
}

void CloseChatUI() {
  if (g_chatPausedGame) {
    GameWorld *world = GetWorldSafe();
    if (!TryReleaseUserPauseSafe(world)) {
      Log("UI_WARN: CloseChatUI pause-release failed during world transition.");
    }
    g_chatPausedGame = false;
  }

  if (g_chatWindow) {
    if (!TryDestroyWidgetSafe(g_chatWindow)) {
      Log("UI_WARN: CloseChatUI destroyWidget failed; clearing stale pointer.");
    }
    g_chatWindow = nullptr;
    g_chatInput = nullptr;
    g_chatModeCombo = nullptr;
    g_chatTargetCombo = nullptr;
    g_chatActionCombo = nullptr;
    g_chatProfileModelCombo = nullptr;
    g_chatActionArgInput = nullptr;
    g_chatAutoChatToggle = nullptr;
    g_chatLabel = nullptr;
  }
  g_chatTargetOptions.clear();
  g_chatTargetRefreshInProgress = false;
  g_chatTargetHandleStr.clear();
  g_chatTargetNameStr.clear();
  g_chatLastRealTargetHandleStr.clear();
  g_chatLastRealTargetNameStr.clear();
  g_chatPlayerNameStr.clear();
  g_chatJustOpened = false;
}

void CloseRenameUI() {
  if (g_renamePausedGame) {
    GameWorld *world = GetWorldSafe();
    if (!TryReleaseUserPauseSafe(world)) {
      Log("UI_WARN: CloseRenameUI pause-release failed during world transition.");
    }
    g_renamePausedGame = false;
  }

  if (g_renameWindow) {
    if (!TryDestroyWidgetSafe(g_renameWindow)) {
      Log("UI_WARN: CloseRenameUI destroyWidget failed; clearing stale pointer.");
    }
    g_renameWindow = nullptr;
    g_renameInput = nullptr;
    g_renameLabel = nullptr;
  }

  g_renameTargetNameStr.clear();
  g_renameTargetHandleStr.clear();
  g_renameSpeakerNameStr.clear();
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
  std::string localPlayerSpeech;
  std::string peopleJson;
  std::string previousSpeaker;
  std::string previousSpeakerHandle;
  std::string initiatorSpeaker;
  std::string initiatorSpeakerHandle;
  std::string requestMode;
  LONG generation;
  int rechatDepth;
  bool allowUnavailableTargetSpeech;
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

static std::string NormalizeDialogueEchoKey(const std::string &value) {
  std::string key = TrimChatLine(value);
  if (key.empty()) {
    return "";
  }

  std::string collapsed;
  collapsed.reserve(key.length());
  bool inSpace = false;
  for (size_t i = 0; i < key.length(); ++i) {
    unsigned char ch = static_cast<unsigned char>(key[i]);
    if (std::isspace(ch)) {
      if (!inSpace) {
        collapsed.push_back(' ');
        inSpace = true;
      }
      continue;
    }
    inSpace = false;
    collapsed.push_back(static_cast<char>(std::tolower(ch)));
  }
  key.swap(collapsed);
  key = TrimChatLine(key);
  if (key.empty()) {
    return "";
  }

  while (!key.empty()) {
    char tail = key[key.length() - 1];
    if (tail == '.' || tail == '!' || tail == '?' || tail == ',' ||
        tail == ';' || tail == ':' || tail == '"' || tail == '\'' ||
        tail == ')' || tail == ']' || tail == '}') {
      key.erase(key.length() - 1);
      continue;
    }
    break;
  }

  return TrimChatLine(key);
}

static bool IsLikelyLocalSpeechEcho(const StreamChatParseState *state,
                                    const std::string &actor,
                                    const std::string &subtitle) {
  if (!state || !state->task || !state->firstLine) {
    return false;
  }

  const std::string localSpeech = TrimChatLine(state->task->localPlayerSpeech);
  if (localSpeech.empty()) {
    return false;
  }

  const std::string previousSpeaker =
      TrimChatLine(state->task->previousSpeaker);
  if (previousSpeaker.empty() || !EqualsIgnoreCase(actor, previousSpeaker)) {
    return false;
  }

  std::string localKey = NormalizeDialogueEchoKey(localSpeech);
  std::string streamKey = NormalizeDialogueEchoKey(subtitle);
  if (localKey.empty() || streamKey.empty()) {
    return false;
  }
  return localKey == streamKey;
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

std::string ParseUtteranceIdToken(const std::string &token) {
  std::string trimmed = TrimChatLine(token);
  if (trimmed.find("uid=") != 0) {
    return "";
  }
  std::string value = TrimChatLine(trimmed.substr(4));
  if (value.empty() || value.length() > 80) {
    return "";
  }
  for (size_t i = 0; i < value.length(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(value[i]);
    bool isSafe =
        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    if (!isSafe) {
      return "";
    }
  }
  return value;
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

bool QueueChatPipeLine(const std::string &line, LONG generation) {
  if (generation > 0 && !IsChatInterruptGenerationCurrent(generation)) {
    return false;
  }
  EnterCriticalSection(&g_msgMutex);
  if (generation > 0 && !IsChatInterruptGenerationCurrent(generation)) {
    LeaveCriticalSection(&g_msgMutex);
    return false;
  }
  g_messageQueue.push_back(line);
  g_lastDialogueTick = GetTickCount();
  LeaveCriticalSection(&g_msgMutex);
  if (line.find("PLAYER_SAY: ") == 0 || line.find("PLAYER_TTS: ") == 0 ||
      line.find("NPC_SAY: ") == 0 || line.find("NPC_ACTION: ") == 0) {
    Log("CHAT_TIMING: pipe queued gen=" + ToString((int)generation) +
        " line=" + line.substr(0, std::min<size_t>(line.length(), 120)));
  }
  return true;
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

bool TryParseGeoBoolToken(const std::string &rawValue, bool &valueOut) {
  std::string value = TrimChatLine(rawValue);
  if (value.empty()) {
    return false;
  }
  std::string lowered = value;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
  if (lowered == "1" || lowered == "true" || lowered == "yes" ||
      lowered == "on") {
    valueOut = true;
    return true;
  }
  if (lowered == "0" || lowered == "false" || lowered == "no" ||
      lowered == "off") {
    valueOut = false;
    return true;
  }
  return false;
}

void AppendGeoQueryFromPlayer(std::wstring &endpoint, Character *player) {
  if (!player || (uintptr_t)player <= 0x1000) {
    return;
  }

  std::string contextJson = BuildNpcContextEnvelope(player, "player");
  if (contextJson.empty() || contextJson == "{}") {
    return;
  }

  std::string rawTown = JsonReadField(contextJson, "town");
  std::string rawZone = JsonReadField(contextJson, "zone");
  std::string rawRegion = JsonReadField(contextJson, "region");
  std::string town = NormalizeGeoToken(rawTown);
  std::string zone = NormalizeGeoToken(rawZone);
  std::string region = NormalizeGeoToken(rawRegion);
  std::string environmentJson = JsonReadField(contextJson, "environment");
  std::string rawBuilding = JsonReadField(environmentJson, "building_name");
  std::string rawEnvZone = JsonReadField(environmentJson, "zone_name");
  std::string rawEnvRegionName = JsonReadField(environmentJson, "region_name");
  std::string rawEnvRegion = JsonReadField(environmentJson, "region");
  std::string building = NormalizeGeoToken(rawBuilding);
  if (zone.empty()) {
    zone = NormalizeGeoToken(rawEnvZone);
  }
  if (region.empty()) {
    region = NormalizeGeoToken(rawEnvRegionName);
  }
  if (region.empty()) {
    region = NormalizeGeoToken(rawEnvRegion);
  }
  if (zone.empty()) {
    zone = town;
  }
  auto safeGeoToken = [](const std::string &value) {
    return value.empty() ? std::string("(empty)") : value;
  };

  std::string floorToken = NormalizeGeoToken(JsonReadField(environmentJson, "floor"));
  std::string xToken = NormalizeGeoToken(JsonReadField(environmentJson, "x"));
  std::string yToken = NormalizeGeoToken(JsonReadField(environmentJson, "y"));
  std::string zToken = NormalizeGeoToken(JsonReadField(environmentJson, "z"));

  bool indoorsValue = false;
  bool outdoorsValue = false;
  bool inTownValue = false;
  bool indoorsKnown =
      TryParseGeoBoolToken(JsonReadField(environmentJson, "indoors"), indoorsValue);
  bool outdoorsKnown =
      TryParseGeoBoolToken(JsonReadField(environmentJson, "outdoors"), outdoorsValue);
  bool inTownKnown =
      TryParseGeoBoolToken(JsonReadField(environmentJson, "in_town"), inTownValue);
  bool useBuilding = IsIndoorsHandleValid(player->isIndoors());
  if (indoorsKnown && indoorsValue) {
    useBuilding = true;
  }
  if ((outdoorsKnown && outdoorsValue) || (indoorsKnown && !indoorsValue)) {
    useBuilding = false;
  }
  if (!useBuilding) {
    building.clear();
  }
  std::string playerName = "Unknown";
  try {
    playerName = player->getName();
  } catch (...) {
    playerName = "Unknown";
  }

  std::string location = "";
  if (useBuilding && !building.empty() && !zone.empty()) {
    location = building + ", " + zone;
  } else if (useBuilding && !building.empty()) {
    location = building;
  } else if (!zone.empty()) {
    location = zone;
  } else if (!region.empty()) {
    location = region;
  }

  if (!location.empty()) {
    endpoint += L"&location=" + ToWide(UrlEncode(location));
  }
  if (!zone.empty()) {
    endpoint += L"&city=" + ToWide(UrlEncode(zone));
  }
  if (!region.empty()) {
    endpoint += L"&region=" + ToWide(UrlEncode(region));
  }
  if (!building.empty()) {
    endpoint += L"&loc_building=" + ToWide(UrlEncode(building));
  }
  if (!zone.empty()) {
    endpoint += L"&loc_zone=" + ToWide(UrlEncode(zone));
  }
  if (!region.empty()) {
    endpoint += L"&loc_region=" + ToWide(UrlEncode(region));
  }
  endpoint += L"&loc_indoors=" + std::wstring(useBuilding ? L"1" : L"0");
  if (!floorToken.empty()) {
    endpoint += L"&loc_floor=" + ToWide(UrlEncode(floorToken));
  }
  if (!xToken.empty()) {
    endpoint += L"&loc_x=" + ToWide(UrlEncode(xToken));
  }
  if (!yToken.empty()) {
    endpoint += L"&loc_y=" + ToWide(UrlEncode(yToken));
  }
  if (!zToken.empty()) {
    endpoint += L"&loc_z=" + ToWide(UrlEncode(zToken));
  }
  Log("GEO_DEBUG_QUERY: source=chatbox actor=" + playerName +
      " raw_town=" + safeGeoToken(rawTown) + " raw_zone=" + safeGeoToken(rawZone) +
      " raw_region=" + safeGeoToken(rawRegion) +
      " raw_building=" + safeGeoToken(rawBuilding) +
      " raw_env_zone=" + safeGeoToken(rawEnvZone) +
      " raw_env_region_name=" + safeGeoToken(rawEnvRegionName) +
      " raw_env_region=" + safeGeoToken(rawEnvRegion) +
      " location=" + safeGeoToken(location) +
      " building=" + safeGeoToken(building) + " zone=" + safeGeoToken(zone) +
      " region=" + safeGeoToken(region) +
      " indoors=" + std::string(useBuilding ? "1" : "0") +
      " in_town=" +
      std::string((inTownKnown && inTownValue) ? "1" : (inTownKnown ? "0" : "?")) +
      " floor=" + safeGeoToken(floorToken) + " x=" + safeGeoToken(xToken) +
      " y=" + safeGeoToken(yToken) + " z=" + safeGeoToken(zToken));
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
  if (!Stobe::ChatMode::AllowsAutomaticRechat(currentTask.requestMode)) {
    Log("RECHAT: skipped (" + currentTask.requestMode + " mode)");
    return;
  }

  std::string speaker = TrimChatLine(lastSpeaker);
  std::string speakerHandle = TrimChatLine(lastSpeakerHandle);
  if (speakerHandle.empty() && EqualsIgnoreCase(speaker, currentTask.npcName)) {
    speakerHandle = TrimChatLine(currentTask.handleStr);
  }
  if (IsNarratorName(speaker)) {
    Log("RECHAT: skipped (narrator speaker)");
    return;
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
  GameWorld *world = GetWorldSafe();
  Character *player = nullptr;
  Character *speakerNpc = nullptr;
  if (world && world->player && world->player->playerCharacters.size() > 0) {
    player = world->player->playerCharacters[0];
  }
  if (world) {
    speakerNpc = ResolveChatTargetCharacter(world, speaker, speakerHandle);
  }

  RechatResponderChoice responderChoice;
  bool responderSelected =
      TrySelectRechatResponder(world, player, speakerNpc, speaker, previousSpeaker,
                               previousSpeakerHandle, responderChoice);
  std::string selectedResponder = responderChoice.name;
  std::string selectedResponderHandle = responderChoice.serial;
  if (!responderSelected || selectedResponder.empty()) {
    selectedResponder.clear();
    selectedResponderHandle.clear();
    responderChoice.source = "server_authoritative_fallback";
    Log("RECHAT: local responder hint unavailable; deferring selection to server speaker=" +
        speaker + " preferred_listener=" + previousSpeaker +
        " preferred_listener_serial=" + previousSpeakerHandle);
  }
  Character *responderNpc = nullptr;
  if (world && !selectedResponder.empty()) {
    responderNpc = ResolveChatTargetCharacter(world, selectedResponder,
                                              selectedResponderHandle);
  }
  if (!responderNpc || (uintptr_t)responderNpc <= 0x1000) {
    selectedResponderHandle.clear();
  }

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
                          L"&profile=" + ToWide(UrlEncode(selectedResponder)) +
                          L"&tts_enabled=" + (g_ttsEnabled ? L"1" : L"0") +
                          L"&rechat_depth=" + ToWide(ToString(nextRechatDepth));
  if (!selectedResponder.empty()) {
    endpoint += L"&rechat_target=" + ToWide(UrlEncode(selectedResponder));
  }
  if (!selectedResponderHandle.empty()) {
    endpoint +=
        L"&rechat_target_sid=" + ToWide(UrlEncode(selectedResponderHandle));
  }
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
  std::string peopleSource = "minimal_rechat_pair";
  if (speakerNpc && (uintptr_t)speakerNpc > 0x1000) {
    // Rebuild nearby conversation context around the actual current speaker.
    // The server is authoritative for responder selection; the client only sends hints.
    peopleJson =
        BuildPeopleJson(world, speaker, previousSpeaker, previousSpeakerHandle,
                        "chat", speakerNpc);
    if (!peopleJson.empty()) {
      peopleSource = "rebuilt_from_current_speaker";
    }
  }
  if (peopleJson.empty()) {
    std::vector<std::string> minimalPeople;
    if (!speaker.empty()) {
      if (!speakerHandle.empty()) {
        AppendUniquePerson(minimalPeople,
                           speaker + "|" + speakerHandle);
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
  AppendGeoQueryFromPlayer(endpoint, player);

  StreamChatTask *nextTask = new StreamChatTask();
  nextTask->endpoint = endpoint;
  nextTask->npcName = selectedResponder;
  nextTask->handleStr = selectedResponderHandle;
  nextTask->peopleJson = peopleJson;
  nextTask->previousSpeaker = speaker;
  nextTask->previousSpeakerHandle = speakerHandle;
  nextTask->initiatorSpeaker = initiatorSpeaker;
  nextTask->initiatorSpeakerHandle = initiatorSpeakerHandle;
  nextTask->requestMode = currentTask.requestMode;
  nextTask->generation = currentTask.generation;
  nextTask->rechatDepth = nextRechatDepth;
  nextTask->allowUnavailableTargetSpeech = false;

  HANDLE followupThread =
      CreateThread(NULL, 0, StreamChatResponseThread, nextTask, 0, NULL);
  if (followupThread) {
    CloseHandle(followupThread);
    Log("RECHAT: dispatched follow-up speaker=" + speaker +
        " selected_responder=" + selectedResponder +
        " selected_responder_serial=" + selectedResponderHandle +
        " selection_source=" + responderChoice.source +
        " previous_listener=" + previousSpeaker +
        " people_source=" + peopleSource +
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
      "DROPNPC",          "DROP_NPC",      "DROP-NPC",
      "PUTDOWNNPC",       "PUT_DOWN_NPC",  "PUT-DOWN-NPC",
      "RELEASENPC",       "RELEASE_NPC",   "RELEASE-NPC",
      "PICKUP_NPC",       "PICKUPNPC",     "PICKUP-NPC",
      "GIVE_CATS",        "TAKE_CATS",     "TAKE_ITEM",        "GIVE_ITEM",
      "DROP_ITEM",        "DRINK_ITEM",    "DRINKITEM",        "DRINK-ITEM",
      "FORCE_DRINK",      "FORCEDRINK",    "FORCE-DRINK",
      "USE_DRUGS",        "USEDRUGS",      "USE-DRUGS",
      "ROLEPLAY_ACTION",  "ROLEPLAYACTION","ROLEPLAY-ACTION",
      "NOTIFY",           "FACTION_RELATIONS","TRAVEL_LOCATION",
      "TRAVELLOCATION",   "USE_OBJECT",    "USEOBJECT",
      "USE-OBJECT",       "KNOCKOUT",      "KNOCK_OUT",
      "KNOCK-OUT",        "KO",            "KILL",
      "KILLTARGET",
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

static std::string MaybeInjectGiveItemTarget(const StreamChatParseState *state,
                                             const std::string &actor,
                                             const std::string &rawAction) {
  std::string actionLine = TrimChatLine(rawAction);
  if (!state || !state->task || actionLine.empty()) {
    return actionLine;
  }

  size_t atPos = actionLine.find('@');
  if (atPos == std::string::npos || atPos == 0 || atPos + 1 >= actionLine.size()) {
    return actionLine;
  }

  std::string command = actionLine.substr(0, atPos);
  std::transform(command.begin(), command.end(), command.begin(),
                 [](unsigned char c) { return (char)std::toupper(c); });
  if (command != "GIVE_ITEM") {
    return actionLine;
  }

  std::string payload = TrimChatLine(actionLine.substr(atPos + 1));
  if (payload.empty() || payload.find('@') != std::string::npos) {
    // Already has an explicit target (or malformed multi-token payload).
    return actionLine;
  }

  std::string listenerName = TrimChatLine(state->task->previousSpeaker);
  if (listenerName.empty() || EqualsIgnoreCase(listenerName, actor)) {
    return actionLine;
  }

  std::string listenerHandle = TrimChatLine(state->task->previousSpeakerHandle);
  bool handleIsNumeric = !listenerHandle.empty();
  for (size_t i = 0; i < listenerHandle.size(); ++i) {
    unsigned char ch = (unsigned char)listenerHandle[i];
    if (ch < '0' || ch > '9') {
      handleIsNumeric = false;
      break;
    }
  }
  if (!handleIsNumeric) {
    listenerHandle.clear();
  }

  std::string listenerToken = listenerName;
  if (!listenerHandle.empty()) {
    listenerToken += "|" + listenerHandle;
  }

  std::string rewritten =
      "GIVE_ITEM@" + listenerToken + "@" + payload;
  Log("CHAT_ACTION: injected GIVE_ITEM target actor=" + actor +
      " listener=" + listenerToken + " payload=" + payload);
  return rewritten;
}

static bool QueueStreamActionIfNew(StreamChatParseState *state,
                                   const std::string &actor,
                                   const std::string &speakerHeader,
                                   const std::string &rawAction) {
  if (!state) {
    return false;
  }
  std::string actionLine = MaybeInjectGiveItemTarget(state, actor, rawAction);
  actionLine = TrimChatLine(actionLine);
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
  std::string utteranceId = "";

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
          std::string parsedUtteranceId = ParseUtteranceIdToken(token);
          if (!parsedUtteranceId.empty()) {
            utteranceId = parsedUtteranceId;
          } else {
            int parsedDuration = ParseTtsDurationToken(token);
            if (parsedDuration > 0) {
              ttsDurationMs = parsedDuration;
            }
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
    bool isActionPayload = EqualsIgnoreCase(actionKind, "ActionQueue") ||
                           EqualsIgnoreCase(actionKind, "Action");
    if (!isActionPayload) {
      size_t slashPos = subtitle.find('/');
      if (slashPos != std::string::npos) {
        subtitle = TrimChatLine(subtitle.substr(0, slashPos));
      }
    }
  }

  std::string speakerHeader = actor;
  bool narratorSpeaker = IsNarratorName(actor);
  if (!state->task->handleStr.empty() && actor == state->task->npcName) {
    speakerHeader = actor + "|" + state->task->handleStr;
  }
  if ((EqualsIgnoreCase(actionKind, "ActionQueue") ||
       EqualsIgnoreCase(actionKind, "Action")) &&
      !subtitle.empty()) {
    if (narratorSpeaker) {
      Log("CHAT_TIMING: narrator action ignored actor=" + actor +
          " action=" + subtitle + " gen=" + ToString((int)state->generation));
      return true;
    }
    QueueStreamActionIfNew(state, actor, speakerHeader, subtitle);
    return true;
  }

  std::vector<std::string> extractedActions;
  if (!narratorSpeaker) {
    ExtractActionTags(subtitle, extractedActions);
    for (size_t i = 0; i < extractedActions.size(); ++i) {
      QueueStreamActionIfNew(state, actor, speakerHeader, extractedActions[i]);
    }
  }

  if (!subtitle.empty()) {
    if (IsLikelyLocalSpeechEcho(state, actor, subtitle)) {
      if (!utteranceId.empty()) {
        PostSpeechDeliveryState(utteranceId, "cancelled");
      }
      Log("CHAT_TIMING: STREAM_LINE dropped local echo actor=" + actor +
          " subtitle_len=" + ToString((int)subtitle.length()) +
          " gen=" + ToString((int)state->generation));
      state->firstLine = false;
      return true;
    }
    if (!IsChatInterruptGenerationCurrent(state->generation)) {
      if (!utteranceId.empty()) {
        PostSpeechDeliveryState(utteranceId, "cancelled");
      }
      return false;
    }
    std::string queueLine = "";
    std::string explicitTalkTargetToken = "";
    if (narratorSpeaker) {
      queueLine = "NARRATOR_NOTIFY:" + GetNarratorDisplayName() + ": " + subtitle;
    } else {
      queueLine = "NPC_SAY: " + speakerHeader + ": " + subtitle;
      if (speakerHeader == actor) {
        GameWorld *worldForSpeaker = GetWorldSafe();
        if (worldForSpeaker) {
          Character *resolvedSpeaker =
              ResolveChatTargetCharacter(worldForSpeaker, actor, "");
          if (resolvedSpeaker && (uintptr_t)resolvedSpeaker > 0x1000) {
            speakerHeader =
                actor + "|" + ToString(resolvedSpeaker->getHandle().serial);
          }
        }
        queueLine = "NPC_SAY: " + speakerHeader + ": " + subtitle;
      }

      if (state->task) {
        std::string listenerName = TrimChatLine(state->task->previousSpeaker);
        std::string listenerHandle =
            TrimChatLine(state->task->previousSpeakerHandle);
        if (!listenerName.empty() && !EqualsIgnoreCase(listenerName, actor)) {
          explicitTalkTargetToken =
              BuildTalkTargetMetadataToken(listenerName, listenerHandle);
        }
      }
    }
    if (!explicitTalkTargetToken.empty()) {
      queueLine += " [TALKTARGET:" + explicitTalkTargetToken + "]";
    }
    if (g_ttsEnabled && !ttsHash.empty()) {
      queueLine += " [TTSHASH:" + ttsHash + "]";
    }
    if (g_ttsEnabled && ttsDurationMs > 0) {
      queueLine += " [TTSDUR:" + ToString(ttsDurationMs) + "]";
    }
    if (!utteranceId.empty()) {
      queueLine += " [UTTERANCEID:" + utteranceId + "]";
    }
    if (!narratorSpeaker && state->task->allowUnavailableTargetSpeech &&
        EqualsIgnoreCase(actor, state->task->npcName)) {
      queueLine += " [ALLOW_UNAVAILABLE_SPEECH]";
    }
    Log("CHAT_TIMING: STREAM_LINE actor=" + actor +
        " subtitle_len=" + ToString((int)subtitle.length()) +
        " tts_hash=" + ShortHashForLog(ttsHash) +
        " tts_dur_ms=" + ToString(ttsDurationMs) +
        " tts_enabled=" + std::string(g_ttsEnabled ? "1" : "0") +
        " gen=" + ToString((int)state->generation));
    if (!QueueChatPipeLine(queueLine, state->generation)) {
      if (!utteranceId.empty()) {
        PostSpeechDeliveryState(utteranceId, "cancelled");
      }
      return false;
    }
    std::string speakerHandle = "";
    if (!narratorSpeaker) {
      size_t headerPipePos = speakerHeader.find('|');
      if (headerPipePos != std::string::npos) {
        speakerHandle = TrimChatLine(speakerHeader.substr(headerPipePos + 1));
      } else if (EqualsIgnoreCase(actor, state->task->npcName)) {
        speakerHandle = TrimChatLine(state->task->handleStr);
      }
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
  ActiveChatStreamScope activeStream;

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
    if (task->requestMode == "inject") {
      Log("CHAT_THREAD: Event injection stored successfully.");
      QueueUiNotifyAction("Event injected.");
      delete task;
      return 0;
    }
    Log("CHAT_THREAD: Empty stream response from server.");
    delete task;
    return 0;
  }

  Log("CHAT_THREAD: Queued " + ToString(parseState.lineCount) +
      " streamed chat lines and " + ToString(parseState.actionCount) +
      " action lines.");
  if (parseState.lineCount > 0 && IsChatInterruptGenerationCurrent(generation)) {
    DWORD followupDelayMs = static_cast<DWORD>(
        Stobe::Timing::ResolveRechatDispatchDelayMs(parseState.interLineDelayMs));
    bool waitForPlayback =
        Stobe::Timing::ShouldWaitForPlaybackBeforeRechatDispatch();
    Log("RECHAT_TIMING: follow-up dispatch scheduled delay_ms=" +
        ToString((int)followupDelayMs) +
        " wait_for_playback=" + std::string(waitForPlayback ? "1" : "0") +
        " gen=" + ToString((int)generation));
    if (followupDelayMs > 0) {
      SleepIfPaused(followupDelayMs);
    }

    if (waitForPlayback) {
      DWORD playbackWaitStart = GetTickCount();
      while (IsChatInterruptGenerationCurrent(generation) &&
             IsTtsPlaybackActive()) {
        SleepIfPaused(50);
        if ((GetTickCount() - playbackWaitStart) > 600000) {
          Log("RECHAT_TIMING: follow-up playback wait timed out");
          break;
        }
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

bool IsAiRequestActive() {
  return InterlockedCompareExchange(&g_activeChatStreamCount, 0, 0) > 0;
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
    std::string newName = TrimChatLine(text.substr(6));
    std::string renameSpeakerName = "";
    std::string renameSpeakerHandle = "";
    ResolveActiveChatSpeaker(world, npcName, handleStr, renameSpeakerName,
                             renameSpeakerHandle);

    std::string renameStatus = "";
    bool renamed = TryRenameCharacterAndSync(world, renameSpeakerName,
                                             renameSpeakerHandle, newName,
                                             renameStatus);
    if (world && !renameStatus.empty()) {
      world->showPlayerAMessage_withLog(renameStatus, true);
    }
    if (renamed) {
      if (g_chatWindow) {
        g_chatWindow->setCaption(WideFromUtf8("Chat Box").c_str());
      }
      RefreshAvailableChatTargets(true);
      RefreshChatHeaderLabel();

      g_chatInput->setCaption("");
      return;
    }
  }

  std::string playerName = g_chatPlayerNameStr;
  if (playerName.empty()) {
    playerName = "Player";
  }
  std::string narratorSpeakerName = TrimChatLine(g_chatPlayerNameStr);
  if (narratorSpeakerName.empty()) {
    narratorSpeakerName = playerName;
  }
  std::string narratorSpeakerHandle = "";

  std::string sanitizedText = SanitizeDialogueForEventStream(text);
  if (!sanitizedText.empty() && sanitizedText != text) {
    Log("CHAT_SANITIZE: stripped trailing noise from player text old_len=" +
        ToString((int)text.length()) +
        " new_len=" + ToString((int)sanitizedText.length()));
    text = sanitizedText;
  }

  std::string selectedMode = Stobe::ChatMode::Normalize(g_chatMode);
  bool narratorModeSelected = (selectedMode == "narrator");
  if (narratorModeSelected) {
    npcName = kNarratorName;
    handleStr = "";
  }
  std::string mode =
      Stobe::ChatMode::ResolveRequestMode(selectedMode, g_autoChatEnabled);

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
  if (!Stobe::ChatMode::AllowsManualActions(selectedMode) &&
      manualActionSelected) {
    if (world) {
      world->showPlayerAMessage_withLog(
          "Chat blocked: Manual actions are unavailable in " +
              Stobe::ChatMode::DisplayLabel(selectedMode) + " mode.",
          true);
    }
    Log("CHAT_GATE: blocked manual action in " + selectedMode + " mode");
    return;
  }
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
  Character *targetNpc = nullptr;
  Character *narratorSpeakerNpc = nullptr;
  if (narratorModeSelected) {
    Character *selectedSpeakerNpc = ResolveSelectedChatSpeaker(world);
    if (selectedSpeakerNpc && (uintptr_t)selectedSpeakerNpc > 0x1000) {
      narratorSpeakerNpc = selectedSpeakerNpc;
      narratorSpeakerName = selectedSpeakerNpc->getName();
      narratorSpeakerHandle = ToString(selectedSpeakerNpc->getHandle().serial);
    } else {
      narratorSpeakerNpc =
          ResolveChatTargetCharacter(world, narratorSpeakerName, narratorSpeakerHandle);
    }
  } else {
    targetNpc = ResolveChatTargetCharacter(world, npcName, handleStr);
  }
  if (narratorModeSelected) {
    if (narratorSpeakerNpc && (uintptr_t)narratorSpeakerNpc > 0x1000) {
      player = narratorSpeakerNpc;
      playerName = narratorSpeakerNpc->getName();
      if (playerName.empty()) {
        playerName = narratorSpeakerName;
      }
      g_chatPlayerNameStr = playerName;
    }
  } else {
    Character *bestSpeaker =
        ResolveSelectedOrConfiguredPlayerSpeaker(world, targetNpc);
    if (bestSpeaker && (uintptr_t)bestSpeaker > 0x1000) {
      player = bestSpeaker;
      playerName = bestSpeaker->getName();
      g_chatPlayerNameStr = playerName;
    }
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
  bool requireStrictTalkValidation =
      !manualActionSelected && selectedMode != "inject";
  bool validationOk = true;
  if (narratorModeSelected) {
    validationOk = (narratorSpeakerNpc && (uintptr_t)narratorSpeakerNpc > 0x1000);
    if (!validationOk) {
      sendFailReason = "Select a valid speaker before using narrator mode.";
    } else if (IsCharacterUnavailableForConversation(narratorSpeakerNpc)) {
      validationOk = false;
      sendFailReason = "Selected speaker cannot talk right now.";
    }
  } else {
    validationOk = ValidatePlayerChatSend(world, player, targetNpc, selectedMode,
                                          requireStrictTalkValidation,
                                          sendFailReason);
  }
  if (!validationOk) {
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
  if (narratorModeSelected) {
    targetName = kNarratorName;
  }
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
  if (narratorModeSelected) {
    resolvedTargetHandle = "";
  } else if (targetNpc && (uintptr_t)targetNpc > 0x1000) {
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
      bool targetIsDead = false;
      if (!IsTakeItemLootTargetValid(world, targetNpc, invalidReason,
                                     targetIsDead)) {
        if (invalidReason.empty()) {
          invalidReason =
              "target must be dead, knocked out, unconscious, imprisoned, or "
              "carried";
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
    } else if (manualActionChoice.type == MANUAL_CHAT_ACTION_CUT_HORNS) {
      manualActionCommand =
          BuildManualCutHornsActionToken(targetName, resolvedTargetHandle);
      if (manualActionCommand.empty()) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Could not build cut-horns action.", true);
        }
        Log("CHAT_GATE: blocked manual cut horns invalid_action_token target='" +
            targetName + "' handle='" + resolvedTargetHandle + "'");
        return;
      }
      if (!CharacterHasHacksaw(player)) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Speaker needs a hacksaw to cut horns.", true);
        }
        Log("CHAT_GATE: blocked manual cut horns missing_hacksaw actor='" +
            playerName + "'");
        return;
      }
      std::string invalidReason = "";
      bool targetIsDead = false;
      if (!IsTakeItemLootTargetValid(world, targetNpc, invalidReason,
                                     targetIsDead)) {
        if (invalidReason.empty()) {
          invalidReason =
              "target must be dead, knocked out, unconscious, imprisoned, or "
              "carried";
        }
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: " + invalidReason + ".", true);
        }
        Log("CHAT_GATE: blocked manual cut horns reason='" + invalidReason +
            "' actor='" + playerName + "' target='" + targetName + "'");
        return;
      }
      if (!IsCharacterShekRace(targetNpc)) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Only Shek have horns to cut.", true);
        }
        Log("CHAT_GATE: blocked manual cut horns reason=target_not_shek actor='" +
            playerName + "' target='" + targetName + "'");
        return;
      }
      float hornAverage = 0.0f;
      if (!TryGetCharacterHornAverage(targetNpc, hornAverage)) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Could not inspect target horn sliders.", true);
        }
        Log("CHAT_GATE: blocked manual cut horns reason=horn_data_unavailable "
            "actor='" +
            playerName + "' target='" + targetName + "'");
        return;
      }
      if (hornAverage >= 0.999f) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Their horns are already cut off.", true);
        }
        Log("CHAT_GATE: blocked manual cut horns reason=already_cut actor='" +
            playerName + "' target='" + targetName +
            "' average=" + ToString(hornAverage));
        return;
      }
      manualActionPromptEligible = true;
    } else if (manualActionChoice.type == MANUAL_CHAT_ACTION_KNOCKOUT) {
      manualActionCommand =
          BuildManualKnockoutActionToken(targetName, resolvedTargetHandle);
      if (manualActionCommand.empty()) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Could not build knockout action.", true);
        }
        Log("CHAT_GATE: blocked manual knockout invalid_action_token target='" +
            targetName + "' handle='" + resolvedTargetHandle + "'");
        return;
      }
      const bool selfTarget = (targetNpc == player);
      std::string invalidReason = "";
      if (!selfTarget && !IsRemoveLimbTargetValid(world, targetNpc, invalidReason)) {
        if (invalidReason.empty()) {
          invalidReason =
              "target must be knocked out, unconscious, imprisoned, or carried";
        }
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: " + invalidReason + ".", true);
        }
        Log("CHAT_GATE: blocked manual knockout reason='" + invalidReason +
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
      if (IsCharacterSkeletonRace(player)) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Skeleton race cannot drink.", true);
        }
        Log("CHAT_GATE: blocked manual drink item reason=skeleton_race actor='" +
            playerName + "'");
        return;
      }
      std::string requestedDrinkQuery =
          manualActionTextArg.empty() ? "Cactus Rum" : manualActionTextArg;
      std::string matchedDrinkItemName = "";
      if (!ResolveCharacterDrinkItemMatch(player, requestedDrinkQuery,
                                          matchedDrinkItemName)) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Drink item must be Bloodrum, Cactus Rum, Grog, or "
              "Sake in speaker inventory/equipment.",
              true);
        }
        Log("CHAT_GATE: blocked manual drink item no_inventory_match query='" +
            requestedDrinkQuery + "' actor='" + playerName + "'");
        return;
      }
      manualActionCommand = BuildManualDrinkItemActionToken(matchedDrinkItemName);
      if (manualActionCommand.empty()) {
        manualActionPromptSkipReason = "invalid_action_token";
      }
    } else if (manualActionChoice.type == MANUAL_CHAT_ACTION_FORCE_DRINK) {
      if (IsCharacterSkeletonRace(player)) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Skeleton race cannot force drinks.", true);
        }
        Log("CHAT_GATE: blocked manual force drink reason=skeleton_race actor='" +
            playerName + "'");
        return;
      }
      std::string invalidReason = "";
      bool targetIsDead = false;
      if (!IsTakeItemLootTargetValid(world, targetNpc, invalidReason,
                                     targetIsDead)) {
        if (invalidReason.empty()) {
          invalidReason =
              "target must be dead, knocked out, unconscious, imprisoned, or "
              "carried";
        }
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: " + invalidReason + ".", true);
        }
        Log("CHAT_GATE: blocked manual force drink reason='" + invalidReason +
            "' actor='" + playerName + "' target='" + targetName + "'");
        return;
      }
      std::string requestedDrinkQuery =
          manualActionTextArg.empty() ? "Cactus Rum" : manualActionTextArg;
      std::string matchedDrinkItemName = "";
      if (!ResolveCharacterDrinkItemMatch(player, requestedDrinkQuery,
                                          matchedDrinkItemName)) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Drink item must be Bloodrum, Cactus Rum, Grog, or "
              "Sake in speaker inventory/equipment.",
              true);
        }
        Log("CHAT_GATE: blocked manual force drink no_inventory_match query='" +
            requestedDrinkQuery + "' actor='" + playerName + "'");
        return;
      }
      manualActionCommand = BuildManualForceDrinkActionToken(
          targetName, resolvedTargetHandle, matchedDrinkItemName);
      if (manualActionCommand.empty()) {
        if (world) {
          world->showPlayerAMessage_withLog(
              "Chat blocked: Could not build force-drink action.", true);
        }
        Log("CHAT_GATE: blocked manual force drink invalid_action_token actor='" +
            playerName + "'");
        return;
      }
      manualActionPromptEligible = true;
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

  bool shouldQueueLocalPlayerSpeech =
      Stobe::ChatMode::ShouldQueueLocalPlayerSpeech(mode);
  if (shouldQueueLocalPlayerSpeech) {
    EnterCriticalSection(&g_msgMutex);
    g_messageQueue.push_back("PLAYER_SAY: " + text);
    LeaveCriticalSection(&g_msgMutex);
    Log("CHAT_TIMING: PLAYER_SAY queued immediately (no TTSDUR yet), text_len=" +
        ToString((int)text.length()) + " gen=" + ToString((int)chatGeneration));
  } else {
    Log("CHAT_TIMING: PLAYER_SAY suppressed locally for " + mode +
        " mode gen=" + ToString((int)chatGeneration));
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
      ToWide(BuildStreamQueryData(Stobe::ChatMode::EventTypeForRequest(mode),
                                  eventData, gameTs)) +
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
  streamTask->localPlayerSpeech = shouldQueueLocalPlayerSpeech ? text : "";
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
  streamTask->allowUnavailableTargetSpeech =
      manualActionChoice.type == MANUAL_CHAT_ACTION_REMOVE_LIMB;
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

void CreateRenameUI(const std::string &targetName, const std::string &speakerName,
                    const std::string &targetHandle) {
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) {
    return;
  }

  if (g_chatWindow) {
    CloseChatUI();
  }
  if (g_renameWindow) {
    CloseRenameUI();
  }

  g_renameTargetNameStr = targetName;
  g_renameTargetHandleStr = targetHandle;
  g_renameSpeakerNameStr = speakerName;
  if (g_renameSpeakerNameStr.empty()) {
    g_renameSpeakerNameStr = "Player";
  }
  g_renamePausedGame = false;

  GameWorld *world = GetWorldSafe();
  bool pausedByUs = false;
  if (world && !TryRequestUserPauseSafe(world, &pausedByUs)) {
    Log("UI_WARN: CreateRenameUI pause request failed.");
  }
  g_renamePausedGame = pausedByUs;

  const float renameWindowW = 0.36f;
  const float renameWindowH = 0.16f;
  const float renameWindowX = (1.0f - renameWindowW) * 0.5f;
  const float renameWindowY = (1.0f - renameWindowH) * 0.5f;
  g_renameWindow = gui->createWidgetReal<MyGUI::Window>(
      "Kenshi_WindowCX", renameWindowX, renameWindowY, renameWindowW,
      renameWindowH, MyGUI::Align::Top | MyGUI::Align::Left, "Overlapped",
      "Stobe_RenameWindow");
  g_renameWindow->setCaption(WideFromUtf8("Rename NPC").c_str());
  g_renameWindow->eventWindowButtonPressed +=
      MyGUI::newDelegate(OnRenameWindowButtonPressed);

  MyGUI::Widget *client = g_renameWindow->getClientWidget();
  std::string labelText = "Rename " + targetName + " to:";
  g_renameLabel = client->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", 0.05f, 0.10f, 0.9f, 0.22f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_RenameLabel");
  g_renameLabel->setCaption(WideFromUtf8(labelText).c_str());

  g_renameInput = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", 0.05f, 0.38f, 0.9f, 0.24f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_RenameInput");
  g_renameInput->setEditMultiLine(false);
  g_renameInput->setEditWordWrap(false);
  g_renameInput->setVisibleVScroll(false);
  g_renameInput->setTextAlign(MyGUI::Align::Default);
  g_renameInput->setFontHeight(18);
  g_renameInput->setCaption("");
  g_renameInput->eventEditTextChange += MyGUI::newDelegate(OnRenameInputChange);
  g_renameInput->eventEditSelectAccept +=
      MyGUI::newDelegate(OnRenameInputAccept);

  MyGUI::Button *renameBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.70f, 0.42f, 0.20f,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_RenameConfirmBtn");
  renameBtn->setCaption(WideFromUtf8("Rename").c_str());
  renameBtn->eventMouseButtonClick += MyGUI::newDelegate(OnRenameConfirmClick);

  MyGUI::Button *cancelBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.53f, 0.70f, 0.42f, 0.20f,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_RenameCancelBtn");
  cancelBtn->setCaption(WideFromUtf8("Cancel").c_str());
  cancelBtn->eventMouseButtonClick += MyGUI::newDelegate(OnRenameCancelClick);

  MyGUI::InputManager::getInstance().setKeyFocusWidget(g_renameInput);
}

void OnRenameClick(MyGUI::Widget *sender) {
  GameWorld *world = GetWorldSafe();
  std::string targetName = TrimChatLine(g_chatTargetNameStr);
  std::string targetHandle = TrimChatLine(g_chatTargetHandleStr);
  std::string speakerName = "";
  std::string speakerHandle = "";
  if (!ResolveActiveChatSpeaker(world, targetName, targetHandle, speakerName,
                                speakerHandle)) {
    if (world) {
      world->showPlayerAMessage_withLog(
          "Rename failed: no valid speaker selected.", true);
    }
    return;
  }
  g_chatPlayerNameStr = speakerName;
  CreateRenameUI(speakerName, speakerName, speakerHandle);
}

void OnRenameInputChange(MyGUI::EditBox *sender) {
  if (!sender) {
    return;
  }
  std::string text = sender->getCaption().asUTF8();
  if (!text.empty() && (text[text.length() - 1] == '\n' ||
                        text[text.length() - 1] == '\r')) {
    while (!text.empty() &&
           (text[text.length() - 1] == '\n' || text[text.length() - 1] == '\r')) {
      text.erase(text.length() - 1);
    }
    sender->setCaption(text);
    OnRenameConfirmClick(sender);
  }
}

void OnRenameInputAccept(MyGUI::EditBox *sender) { OnRenameConfirmClick(sender); }

void OnRenameConfirmClick(MyGUI::Widget *sender) {
  if (!g_renameInput) {
    return;
  }

  std::string newName = TrimChatLine(g_renameInput->getCaption().asUTF8());
  if (newName.empty()) {
    GameWorld *world = GetWorldSafe();
    if (world) {
      world->showPlayerAMessage_withLog(
          "Rename failed: provide a new name.", true);
    }
    return;
  }

  GameWorld *world = GetWorldSafe();
  std::string renameStatus = "";
  bool renamed = TryRenameCharacterAndSync(
      world, g_renameTargetNameStr, g_renameTargetHandleStr, newName, renameStatus);
  if (world && !renameStatus.empty()) {
    world->showPlayerAMessage_withLog(renameStatus, true);
  }
  if (renamed) {
    CloseRenameUI();
  }
}

void OnRenameCancelClick(MyGUI::Widget *sender) { CloseRenameUI(); }

void OnRenameWindowButtonPressed(MyGUI::Window *sender,
                                 const std::string &name) {
  if (name == "close") {
    CloseRenameUI();
  }
}

void OnBoredEventClick(MyGUI::Widget *sender) {
  GameWorld *world = GetWorldSafe();
  std::string targetName = TrimChatLine(g_chatTargetNameStr);
  std::string targetSerial = TrimChatLine(g_chatTargetHandleStr);
  std::string preferredSpeakerName = targetName;
  std::string preferredSpeakerSerial = targetSerial;
  std::string preferredListenerName = TrimChatLine(g_chatPlayerNameStr);
  std::string preferredListenerSerial = "";

  auto sameIdentity = [&](const std::string &lhsName, const std::string &lhsSerial,
                          const std::string &rhsName,
                          const std::string &rhsSerial) -> bool {
    if (!lhsSerial.empty() && !rhsSerial.empty()) {
      return lhsSerial == rhsSerial;
    }
    if (!lhsName.empty() && !rhsName.empty()) {
      return EqualsIgnoreCase(lhsName, rhsName);
    }
    return false;
  };

  if (world) {
    Character *targetNpc =
        ResolveChatTargetCharacter(world, preferredSpeakerName, preferredSpeakerSerial);
    if (targetNpc && (uintptr_t)targetNpc > 0x1000) {
      std::string resolvedTargetName = TrimChatLine(targetNpc->getName());
      if (!resolvedTargetName.empty()) {
        preferredSpeakerName = resolvedTargetName;
      }
      preferredSpeakerSerial = ResolveCharacterSerialToken(targetNpc);
    }

    Character *bestListener =
        ResolveSelectedOrConfiguredPlayerSpeaker(world, targetNpc);
    if (bestListener && (uintptr_t)bestListener > 0x1000) {
      std::string resolvedListenerName = TrimChatLine(bestListener->getName());
      std::string resolvedListenerSerial = ResolveCharacterSerialToken(bestListener);
      if (!sameIdentity(resolvedListenerName, resolvedListenerSerial,
                        preferredSpeakerName, preferredSpeakerSerial)) {
        preferredListenerName = resolvedListenerName;
        preferredListenerSerial = resolvedListenerSerial;
      }
    }

    if (!preferredListenerName.empty() && preferredListenerSerial.empty() &&
        !sameIdentity(preferredListenerName, "", preferredSpeakerName,
                      preferredSpeakerSerial)) {
      Character *resolvedListener =
          ResolveChatTargetCharacter(world, preferredListenerName, "");
      if (resolvedListener && (uintptr_t)resolvedListener > 0x1000) {
        preferredListenerSerial = ResolveCharacterSerialToken(resolvedListener);
      }
    }

    if ((preferredListenerName.empty() || sameIdentity(preferredListenerName,
                                                       preferredListenerSerial,
                                                       preferredSpeakerName,
                                                       preferredSpeakerSerial)) &&
        world->player && world->player->playerCharacters.size() > 0) {
      Character *fallbackListener = world->player->playerCharacters[0];
      if (fallbackListener && (uintptr_t)fallbackListener > 0x1000) {
        std::string fallbackName = TrimChatLine(fallbackListener->getName());
        std::string fallbackSerial =
            ResolveCharacterSerialToken(fallbackListener);
        if (!sameIdentity(fallbackName, fallbackSerial, preferredSpeakerName,
                          preferredSpeakerSerial)) {
          preferredListenerName = fallbackName;
          preferredListenerSerial = fallbackSerial;
        }
      }
    }
  }

  if (sameIdentity(preferredListenerName, preferredListenerSerial,
                   preferredSpeakerName, preferredSpeakerSerial)) {
    preferredListenerName.clear();
    preferredListenerSerial.clear();
  }

  Log("BORED_EVENT: manual continue requested target_speaker=" +
      preferredSpeakerName + " speaker_serial=" + preferredSpeakerSerial +
      " preferred_listener=" + preferredListenerName +
      " preferred_listener_serial=" + preferredListenerSerial +
      " target=" + targetName + " target_serial=" + targetSerial);
  LONG generation = BeginChatInterruptGeneration();

  EnterCriticalSection(&g_stateMutex);
  g_triggerBoredEvent = false;
  g_lastBoredEventTick = GetTickCount();
  if (world) {
    try {
      TimeOfDay nowTod = world->getTimeStamp_inGameHours();
      g_lastBoredEventGameTs = (int)nowTod.getTotalSeconds();
    } catch (...) {
      g_lastBoredEventGameTs = 0;
    }
  } else {
    g_lastBoredEventGameTs = 0;
  }
  LeaveCriticalSection(&g_stateMutex);

  bool dispatched =
      TriggerBoredEvent(world, true, preferredSpeakerName, preferredSpeakerSerial,
                        generation, preferredListenerName,
                        preferredListenerSerial);
  if (!dispatched) {
    Log("BORED_EVENT: button trigger failed for selected NPC '" +
        preferredSpeakerName + "' serial=" + preferredSpeakerSerial);
  }
  CloseChatUI();
}

void OnWriteDiaryClick(MyGUI::Widget *sender) {
  GameWorld *world = GetWorldSafe();
  if (!world || !world->player ||
      !world->player->selectedCharacter.isValid()) {
    Log("DIARY: button trigger failed (no selected NPC).");
    QueueUiNotifyAction("Diary: select an NPC first.");
    CloseChatUI();
    return;
  }

  hand selectedHandle = world->player->selectedCharacter;
  Character *targetNpc = ResolveLiveCharacter(world, selectedHandle);
  if (!targetNpc || (uintptr_t)targetNpc <= 0x1000) {
    Log("DIARY: button trigger failed (selected NPC unavailable).");
    QueueUiNotifyAction("Diary: the selected NPC is unavailable.");
    CloseChatUI();
    return;
  }

  std::string targetNpcName;
  try {
    targetNpcName = TrimChatLine(targetNpc->getName());
  } catch (...) {
    targetNpcName.clear();
  }
  if (targetNpcName.empty()) {
    Log("DIARY: button trigger failed (selected NPC has no name).");
    QueueUiNotifyAction("Diary: the selected NPC has no name.");
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
  std::string targetHandle = ToString(selectedHandle.serial);
  Character *bestSpeaker =
      ResolveSelectedOrConfiguredPlayerSpeaker(world, targetNpc);
  if (bestSpeaker && (uintptr_t)bestSpeaker > 0x1000) {
    player = bestSpeaker;
    playerName = bestSpeaker->getName();
  }
  std::string selectedMode = Stobe::ChatMode::Normalize(g_chatMode);
  if (selectedMode == "narrator" ||
      Stobe::ChatMode::IsInjectionMode(selectedMode)) {
    selectedMode = "chat";
  }
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
    Log("DIARY: manual diary trigger dispatched for selected NPC '" +
        targetNpcName + "' serial=" + targetHandle +
        " people_len=" + ToString((int)peopleJson.length()));
  } else {
    delete task;
    Log("DIARY: failed to start manual diary response thread for '" +
        targetNpcName + "'");
    QueueUiNotifyAction("Diary: failed to start request for " + targetNpcName +
                        ".");
  }
  CloseChatUI();
}

void OnWriteNarratorDiaryClick(MyGUI::Widget *sender) {
  GameWorld *world = GetWorldSafe();
  Character *player = nullptr;
  if (world && world->player && world->player->playerCharacters.size() > 0) {
    player = world->player->playerCharacters[0];
  }
  std::string playerName = TrimChatLine(g_chatPlayerNameStr);
  if (playerName.empty() && player && (uintptr_t)player > 0x1000) {
    try {
      playerName = TrimChatLine(player->getName());
    } catch (...) {
      playerName.clear();
    }
  }
  if (playerName.empty()) {
    playerName = "Player";
  }

  std::string peopleJson =
      "[\"" + EscapeJSON(playerName) + "\",\"" + kNarratorName + "\"]";
  std::wstring endpoint =
      L"/StobeServer/stream.php?DATA=" +
      ToWide(BuildStreamQueryData("diary_narrator", kNarratorName,
                                  ResolveCurrentGameTs())) +
      L"&profile=" + ToWide(UrlEncode(kNarratorName)) +
      L"&tts_enabled=" + (g_ttsEnabled ? L"1" : L"0") +
      L"&people=" + ToWide(UrlEncode(peopleJson));
  AppendGeoQueryFromPlayer(endpoint, player);

  ManualDiaryTask *task = new ManualDiaryTask();
  task->endpoint = endpoint;
  task->targetNpcName = GetNarratorDisplayName();
  task->peopleJson = peopleJson;
  task->requestStartTick = GetTickCount();
  QueueUiNotifyAction("Diary: narrator request sent.");
  HANDLE diaryThread =
      CreateThread(NULL, 0, ManualDiaryResponseThread, task, 0, NULL);
  if (diaryThread) {
    CloseHandle(diaryThread);
    Log("DIARY: manual narrator diary trigger dispatched.");
  } else {
    delete task;
    QueueUiNotifyAction("Diary: failed to start narrator request.");
    Log("DIARY: failed to start manual narrator diary request.");
  }
  CloseChatUI();
}

bool TriggerBoredEvent(GameWorld *world, bool forceDirectorMode,
                       const std::string &preferredSpeakerName,
                       const std::string &preferredSpeakerSerial,
                       LONG generationOverride,
                       const std::string &preferredListenerName,
                       const std::string &preferredListenerSerial) {
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
  std::string preferredName = TrimChatLine(preferredSpeakerName);
  std::string preferredSerial = TrimChatLine(preferredSpeakerSerial);
  bool hasPreferred = !preferredName.empty() || !preferredSerial.empty();
  const bool targetLockedSpeaker = forceDirectorMode && hasPreferred;
  Character *preferredCharacter = nullptr;
  if (hasPreferred) {
    preferredCharacter =
        ResolveChatTargetCharacter(world, preferredName, preferredSerial);
    if (!preferredCharacter || (uintptr_t)preferredCharacter <= 0x1000) {
      Log("BORED_EVENT: preferred target unresolved name=" + preferredName +
          " serial=" + preferredSerial);
    }
  }

  Character *searchAnchor = player;
  if (forceDirectorMode && preferredCharacter &&
      (uintptr_t)preferredCharacter > 0x1000) {
    searchAnchor = preferredCharacter;
  }

  const hand &anchorIndoorsHandle = searchAnchor->isIndoors();
  bool anchorIsIndoors = IsIndoorsHandleValid(anchorIndoorsHandle);
  float searchRadius = anchorIsIndoors ? g_boredEventRange : g_proximityRadius;
  if (searchRadius < 10.0f) {
    searchRadius = 10.0f;
  }

  bool preferredPresent = false;

  const ogre_unordered_set<Character *>::type &chars =
      world->getCharacterUpdateList();
  for (ogre_unordered_set<Character *>::type::const_iterator it = chars.begin();
       it != chars.end(); ++it) {
    Character *other = *it;
    if (!other || (uintptr_t)other <= 0x1000) {
      continue;
    }

    std::string otherName = "";
    std::string serial = "";
    bool preferredMatch = false;
    try {
      otherName = other->getName();
      serial = ToString(other->getHandle().serial);
      preferredMatch =
          (!preferredSerial.empty() && serial == preferredSerial) ||
          (!preferredName.empty() && EqualsIgnoreCase(otherName, preferredName));
    } catch (...) {
      continue;
    }

    if (otherName.empty()) {
      continue;
    }
    if (other == searchAnchor && !preferredMatch) {
      continue;
    }
    if (!ShouldIncludeAnimalForTalk(other)) {
      continue;
    }

    try {
      if ((other->isDead() || other->isUnconcious()) &&
          !(targetLockedSpeaker && preferredMatch)) {
        continue;
      }
    } catch (...) {
      continue;
    }

    // For bored events we only exclude the actual controlled player character.
    // Other player-faction squadmates are valid NPC speakers/listeners.
    bool isActualPlayerCharacter = (other == player);
    if (isActualPlayerCharacter && !(forceDirectorMode && preferredMatch)) {
      continue;
    }

    const hand &otherIndoorsHandle = other->isIndoors();
    bool otherIsIndoors = IsIndoorsHandleValid(otherIndoorsHandle);
    unsigned int otherBuildingSerial =
        otherIsIndoors ? otherIndoorsHandle.serial : 0;
    float dist = searchAnchor->getPosition().distance(other->getPosition());
    bool areaCompatible = IsConversationAreaCompatible(searchAnchor, other);
    if (!areaCompatible && !(forceDirectorMode && preferredMatch)) {
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
    if (preferredMatch) {
      preferredPresent = true;
    }
  }

  if (hasPreferred && !preferredPresent && preferredCharacter &&
      (uintptr_t)preferredCharacter > 0x1000 &&
      (targetLockedSpeaker ||
       !IsCharacterUnavailableForConversation(preferredCharacter)) &&
      ShouldIncludeAnimalForTalk(preferredCharacter)) {
    try {
      CandidateNpc c;
      c.name = preferredCharacter->getName();
      if (!c.name.empty()) {
        c.serial = ToString(preferredCharacter->getHandle().serial);
        c.distance =
            searchAnchor->getPosition().distance(preferredCharacter->getPosition());
        c.isPreferred = true;
        const hand &preferredIndoorsHandle = preferredCharacter->isIndoors();
        c.indoors = IsIndoorsHandleValid(preferredIndoorsHandle);
        c.buildingSerial = c.indoors ? preferredIndoorsHandle.serial : 0;
        c.floor = preferredCharacter->getFloor();
        candidates.push_back(c);
        preferredPresent = true;
        Log("BORED_EVENT: preferred speaker injected from target resolver name=" +
            c.name + " serial=" + c.serial + " director_mode=" +
            std::string(forceDirectorMode ? "1" : "0"));
      }
    } catch (...) {
      Log("BORED_EVENT: preferred speaker injection failed (exception) name=" +
          preferredName + " serial=" + preferredSerial);
    }
  }

  if (candidates.empty()) {
    Log("BORED_EVENT: skipped (no nearby NPC candidates) preferred_name=" +
        preferredName + " preferred_serial=" + preferredSerial +
        " director_mode=" + std::string(forceDirectorMode ? "1" : "0"));
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

  auto sameIdentity = [&](const std::string &lhsName, const std::string &lhsSerial,
                          const std::string &rhsName,
                          const std::string &rhsSerial) -> bool {
    if (!lhsSerial.empty() && !rhsSerial.empty()) {
      return lhsSerial == rhsSerial;
    }
    if (!lhsName.empty() && !rhsName.empty()) {
      return EqualsIgnoreCase(lhsName, rhsName);
    }
    return false;
  };

  std::string listener = "";
  std::string listenerSerial = "";
  std::string playerName = player->getName();
  std::string playerSerial = ToString(player->getHandle().serial);
  std::string preferredListenerNameTrim = TrimChatLine(preferredListenerName);
  std::string preferredListenerSerialTrim =
      TrimChatLine(preferredListenerSerial);
  std::vector<size_t> listenerIndices;
  listenerIndices.reserve(candidates.size());
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (i == speakerIndex) {
      continue;
    }
    listenerIndices.push_back(i);
  }
  if (listenerIndices.empty()) {
    if (!targetLockedSpeaker) {
      Log("BORED_EVENT: skipped (no eligible NPC listener) speaker=" +
          speaker.name + " candidate_count=" + ToString((int)candidates.size()));
      return false;
    }
  }

  if (targetLockedSpeaker) {
    if (!preferredListenerNameTrim.empty() || !preferredListenerSerialTrim.empty()) {
      if (!sameIdentity(preferredListenerNameTrim, preferredListenerSerialTrim,
                        speaker.name, speaker.serial) &&
          !playerName.empty() &&
          sameIdentity(preferredListenerNameTrim, preferredListenerSerialTrim,
                       playerName, playerSerial)) {
        listener = playerName;
        listenerSerial = playerSerial;
      }

      if (listener.empty()) {
        for (size_t i = 0; i < listenerIndices.size(); ++i) {
          size_t candidateIndex = listenerIndices[i];
          if (candidateIndex >= candidates.size()) {
            continue;
          }
          const CandidateNpc &candidate = candidates[candidateIndex];
          if (sameIdentity(preferredListenerNameTrim, preferredListenerSerialTrim,
                           candidate.name, candidate.serial)) {
            listener = candidate.name;
            listenerSerial = candidate.serial;
            break;
          }
        }
      }
    }

    if (listener.empty() && !playerName.empty() &&
        !sameIdentity(playerName, playerSerial, speaker.name, speaker.serial)) {
      listener = playerName;
      listenerSerial = playerSerial;
    }

    if (listener.empty() && !listenerIndices.empty()) {
      size_t bestListenerIndex = listenerIndices[0];
      float bestListenerDistance = 1e30f;
      for (size_t i = 0; i < listenerIndices.size(); ++i) {
        size_t candidateIndex = listenerIndices[i];
        if (candidateIndex >= candidates.size()) {
          continue;
        }
        if (candidates[candidateIndex].distance < bestListenerDistance) {
          bestListenerDistance = candidates[candidateIndex].distance;
          bestListenerIndex = candidateIndex;
        }
      }
      if (bestListenerIndex < candidates.size()) {
        listener = candidates[bestListenerIndex].name;
        listenerSerial = candidates[bestListenerIndex].serial;
      }
    }
  } else {
    size_t listenerIndex =
        listenerIndices[(size_t)(rand() % listenerIndices.size())];
    if (listenerIndex >= candidates.size()) {
      Log("BORED_EVENT: skipped (listener index out of bounds) speaker=" +
          speaker.name + " candidate_count=" + ToString((int)candidates.size()));
      return false;
    }
    listener = candidates[listenerIndex].name;
    listenerSerial = candidates[listenerIndex].serial;
  }

  if (listener.empty()) {
    Log("BORED_EVENT: skipped (no resolved listener) speaker=" + speaker.name +
        " candidate_count=" + ToString((int)candidates.size()) +
        " director_mode=" + std::string(forceDirectorMode ? "1" : "0"));
    return false;
  }

  std::vector<std::string> people;
  AppendUniquePerson(people, speaker.name + "|" + speaker.serial);
  if (!listener.empty()) {
    if (!listenerSerial.empty()) {
      AppendUniquePerson(people, listener + "|" + listenerSerial);
    } else {
      AppendUniquePerson(people, listener);
    }
  }
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
  task->previousSpeakerHandle = listenerSerial;
  if (task->previousSpeakerHandle.empty() && listener == playerName &&
      !playerSerial.empty()) {
    task->previousSpeakerHandle = playerSerial;
  }
  if (task->previousSpeakerHandle.empty()) {
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
  task->allowUnavailableTargetSpeech = false;

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
      ToString(speaker.floor) + " listener_serial=" +
      task->previousSpeakerHandle + " gen=" +
      ToString((int)task->generation));
  return true;
}

bool TriggerNarratorWelcomeOnLoad(GameWorld *world, Character *preferredSpeaker,
                                  LONG generationOverride) {
  if (!world || !world->player || world->player->playerCharacters.size() == 0) {
    return false;
  }

  Character *speaker = nullptr;
  if (preferredSpeaker && (uintptr_t)preferredSpeaker > 0x1000) {
    bool isPlayerCharacter = false;
    try {
      isPlayerCharacter = preferredSpeaker->isPlayerCharacter();
    } catch (...) {
      isPlayerCharacter = false;
    }
    if (isPlayerCharacter) {
      speaker = preferredSpeaker;
    }
  }
  if (!speaker) {
    speaker = world->player->playerCharacters[0];
  }
  if (!speaker || (uintptr_t)speaker <= 0x1000) {
    return false;
  }

  std::string speakerName = TrimChatLine(speaker->getName());
  if (speakerName.empty()) {
    speakerName = "Player";
  }
  std::string speakerSerial = ResolveCharacterSerialToken(speaker);

  std::string peopleJson = "[]";
  std::string eventData = "game load detected";
  std::wstring endpoint =
      L"/StobeServer/stream.php?DATA=" +
      ToWide(BuildStreamQueryData("init", eventData, ResolveCurrentGameTs())) +
      L"&mode=narrator" + L"&tts_enabled=" + (g_ttsEnabled ? L"1" : L"0") +
      L"&people=" + ToWide(UrlEncode(peopleJson));
  // Save-load callbacks can still expose stale streamed objects. The first
  // explicit interaction will attach a fully warmed-up location context.

  StreamChatTask *task = new StreamChatTask();
  task->endpoint = endpoint;
  task->npcName = kNarratorName;
  task->handleStr = "";
  task->peopleJson = peopleJson;
  task->previousSpeaker = speakerName;
  task->previousSpeakerHandle = speakerSerial;
  task->initiatorSpeaker = speakerName;
  task->initiatorSpeakerHandle = speakerSerial;
  task->requestMode = "narrator";
  task->generation = generationOverride > 0 ? generationOverride
                                             : GetChatInterruptGeneration();
  task->rechatDepth = 0;
  task->allowUnavailableTargetSpeech = false;

  HANDLE thread = CreateThread(NULL, 0, StreamChatResponseThread, task, 0, NULL);
  if (!thread) {
    delete task;
    Log("LOAD_SYNC: failed to start narrator welcome stream thread");
    return false;
  }
  CloseHandle(thread);

  Log("LOAD_SYNC: dispatched narrator welcome init speaker=" + speakerName +
      " serial=" + speakerSerial + " gen=" + ToString((int)task->generation));
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
  if (g_renameWindow)
    CloseRenameUI();
  if (g_chatWindow)
    CloseChatUI();

  g_chatPlayerNameStr = playerName;
  SetActiveChatTarget(npcName, handleStr, true);
  g_chatJustOpened = true;
  g_chatPausedGame = false;
  RequestProfileModelSlotRefresh(true);

  GameWorld *world = GetWorldSafe();
  if (world) {
    Character *targetNpc = ResolveChatTargetCharacter(world, npcName, handleStr);
    Character *bestSpeaker =
        ResolveSelectedOrConfiguredPlayerSpeaker(world, targetNpc);
    if (bestSpeaker && (uintptr_t)bestSpeaker > 0x1000) {
      g_chatPlayerNameStr = bestSpeaker->getName();
    }
  }
  bool pausedByUs = false;
  if (world && !TryRequestUserPauseSafe(world, &pausedByUs)) {
    Log("UI_WARN: CreateChatUI pause request failed.");
  }
  g_chatPausedGame = pausedByUs;
  if (g_chatPlayerNameStr.empty()) {
    g_chatPlayerNameStr = playerName.empty() ? "Player" : playerName;
  }

  // Identity renames are queued and handled asynchronously by
  // RenameWorker, so CreateChatUI never blocks on HTTP.
  const float chatWindowW = 0.46f;
  const float chatWindowH = 0.24f + ScreenPixelsToRealHeight(20);
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
      "Kenshi_TextboxStandardText", 0.05f, 0.04f, 0.44f, 0.13f,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_ChatLabel");
  g_chatLabel->setCaption(WideFromUtf8("Speaker: " + g_chatPlayerNameStr).c_str());
  const float targetComboHeight = 0.13f + ParentPixelsToRealHeight(client, 20);
  g_chatTargetCombo = client->createWidgetReal<MyGUI::ComboBox>(
      "Kenshi_ComboBox", 0.53f, 0.04f, 0.42f, targetComboHeight,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_ChatTargetCombo");
  g_chatTargetCombo->setComboModeDrop(true);
  g_chatTargetCombo->eventComboAccept += MyGUI::newDelegate(OnChatTargetChange);
  g_chatTargetCombo->eventComboChangePosition +=
      MyGUI::newDelegate(OnChatTargetChange);
  g_chatInput = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", 0.05f, 0.22f, 0.9f, 0.18f,
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

  const float inputY = 0.22f;
  const float inputH = 0.18f;
  const float rowGap = 0.025f;
  const float rowH = 0.16f;
  const float primaryRowY = inputY + inputH + rowGap;
  const float secondaryRowY = primaryRowY + rowH + rowGap;
  const float controlRowY = secondaryRowY + rowH + rowGap;
  const float primaryRowLeftX = 0.05f;
  const float primaryRowGap = 0.02f;
  const float primaryBtnW = 0.29f;
  const float primaryAutoX = primaryRowLeftX;
  const float primaryBoredX = primaryAutoX + primaryBtnW + primaryRowGap;
  const float primaryRenameX = primaryBoredX + primaryBtnW + primaryRowGap;
  const float secondaryRowLeftX = 0.05f;
  const float secondaryRowGap = 0.02f;
  const float secondaryDiaryW = 0.28f;
  const float secondaryDiaryX = secondaryRowLeftX;
  const float secondaryNarratorDiaryX =
      secondaryDiaryX + secondaryDiaryW + secondaryRowGap;
  const float profileModelX =
      secondaryNarratorDiaryX + secondaryDiaryW + secondaryRowGap;
  const float profileModelW = 0.31f;
  const float controlRowLeftX = 0.05f;
  const float controlRowGap = 0.02f;
  const float modeWidth = 0.20f;
  const float actionWidth = 0.30f;
  const float actionArgWidth = 0.14f;
  const float sendWidth = 0.20f;
  const float actionX = controlRowLeftX + modeWidth + controlRowGap;
  const float actionArgX = actionX + actionWidth + controlRowGap;
  const float sendX = actionArgX + actionArgWidth + controlRowGap;

  g_chatModeCombo = client->createWidgetReal<MyGUI::ComboBox>(
      "Kenshi_ComboBox", controlRowLeftX, controlRowY, modeWidth, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_ChatModeCombo");
  g_chatModeCombo->setComboModeDrop(true);
  g_chatModeCombo->addItem(WideFromUtf8("chat").c_str());
  g_chatModeCombo->addItem(WideFromUtf8("whisper").c_str());
  g_chatModeCombo->addItem(WideFromUtf8("shout").c_str());
  g_chatModeCombo->addItem(WideFromUtf8("cheat").c_str());
  g_chatModeCombo->addItem(WideFromUtf8("narrator").c_str());
  g_chatModeCombo->addItem(WideFromUtf8("inject").c_str());
  g_chatModeCombo->addItem(WideFromUtf8("inject & chat").c_str());
  g_chatModeCombo->eventComboAccept += MyGUI::newDelegate(OnChatModeChange);
  g_chatModeCombo->eventComboChangePosition +=
      MyGUI::newDelegate(OnChatModeChange);

  g_chatActionCombo = client->createWidgetReal<MyGUI::ComboBox>(
      "Kenshi_ComboBox", actionX, controlRowY, actionWidth, rowH,
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
      "Kenshi_EditBox", actionArgX, controlRowY, actionArgWidth, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_ChatActionArgInput");
  g_chatActionArgInput->setEditMultiLine(false);
  g_chatActionArgInput->setEditWordWrap(false);
  g_chatActionArgInput->setVisibleVScroll(false);
  g_chatActionArgInput->setTextAlign(MyGUI::Align::Default);
  g_chatActionArgInput->setFontHeight(16);
  g_chatActionArgInput->setCaption("");
  OnChatActionChange(g_chatActionCombo, g_chatActionCombo->getIndexSelected());

  g_chatAutoChatToggle = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", primaryAutoX, primaryRowY, primaryBtnW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_ChatAutoToggle");
  g_chatAutoChatToggle->eventMouseButtonClick +=
      MyGUI::newDelegate(OnAutoChatToggleClick);

  RefreshChatModeControls();

  MyGUI::Button *sendBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", sendX, controlRowY, sendWidth, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_ChatSendBtn");
  sendBtn->setCaption(WideFromUtf8(T("Send")).c_str());
  sendBtn->eventMouseButtonClick += MyGUI::newDelegate(OnChatSendClick);

  MyGUI::Button *boredEventBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", primaryBoredX, primaryRowY, primaryBtnW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left,
      "Stobe_ChatBoredBtn");
  boredEventBtn->setCaption(WideFromUtf8(T("Continue Chat")).c_str());
  boredEventBtn->eventMouseButtonClick += MyGUI::newDelegate(OnBoredEventClick);

  MyGUI::Button *writeDiaryBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", secondaryDiaryX, secondaryRowY, secondaryDiaryW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left,
      "Stobe_ChatDiaryBtn");
  writeDiaryBtn->setCaption(WideFromUtf8(T("Write Diary")).c_str());
  writeDiaryBtn->eventMouseButtonClick += MyGUI::newDelegate(OnWriteDiaryClick);

  MyGUI::Button *writeNarratorDiaryBtn =
      client->createWidgetReal<MyGUI::Button>(
          "Kenshi_Button1", secondaryNarratorDiaryX, secondaryRowY,
          secondaryDiaryW, rowH,
          MyGUI::Align::Top | MyGUI::Align::Left,
          "Stobe_ChatNarratorDiaryBtn");
  writeNarratorDiaryBtn->setCaption(
      WideFromUtf8(T("Narrator Diary")).c_str());
  writeNarratorDiaryBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnWriteNarratorDiaryClick);

  MyGUI::Button *renameBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", primaryRenameX, primaryRowY, primaryBtnW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_ChatRenameBtn");
  renameBtn->setCaption(WideFromUtf8("Rename").c_str());
  renameBtn->eventMouseButtonClick += MyGUI::newDelegate(OnRenameClick);

  g_chatProfileModelCombo = client->createWidgetReal<MyGUI::ComboBox>(
      "Kenshi_ComboBox", profileModelX, secondaryRowY, profileModelW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left,
      "Stobe_ChatProfileModelCombo");
  g_chatProfileModelCombo->setComboModeDrop(true);
  for (int slot = 1; slot <= 4; ++slot) {
    g_chatProfileModelCombo->addItem(
        WideFromUtf8("Response: " +
                     std::string(kProfileModelLabels[slot - 1]))
            .c_str());
  }
  g_chatProfileModelCombo->setIndexSelected(
      static_cast<size_t>(GetActiveProfileModelSlot() - 1));
  g_chatProfileModelCombo->eventComboAccept +=
      MyGUI::newDelegate(OnChatProfileModelChange);
  g_chatProfileModelCombo->eventComboChangePosition +=
      MyGUI::newDelegate(OnChatProfileModelChange);
}

void OnChatModeChange(MyGUI::ComboBox *sender, size_t index) {
  if (!sender || index == MyGUI::ITEM_NONE)
    return;

  std::string selectedMode = sender->getItemNameAt(index);
  g_chatMode = Stobe::ChatMode::Normalize(selectedMode);
  g_lastChatModeIndex = Stobe::ChatMode::ToIndex(g_chatMode);
  SaveStobeRuntimeConfig();
  RefreshChatModeControls();
}

void OnChatProfileModelChange(MyGUI::ComboBox *sender, size_t index) {
  if (g_profileModelComboRefreshInProgress || !sender ||
      index == MyGUI::ITEM_NONE || index >= 4) {
    return;
  }

  const int requestedSlot = static_cast<int>(index) + 1;
  const int previousSlot = GetActiveProfileModelSlot();
  if (requestedSlot == previousSlot) {
    return;
  }

  const LONG revision = InterlockedIncrement(&g_profileModelRevision);
  InterlockedExchange(&g_profileModelSlot, requestedSlot);

  ProfileModelRequestTask *task = new ProfileModelRequestTask();
  task->write = true;
  task->requestedSlot = requestedSlot;
  task->fallbackSlot = previousSlot;
  task->revision = revision;
  if (!StartProfileModelRequest(task)) {
    QueueProfileModelSlotUpdate(revision, previousSlot);
    QueueProfileModelNotification(
        "Unable to change the response mode. Check StobeServer.");
  }
}

void OnChatTargetChange(MyGUI::ComboBox *sender, size_t index) {
  if (g_chatTargetRefreshInProgress || !sender || index == MyGUI::ITEM_NONE) {
    return;
  }
  if (index >= g_chatTargetOptions.size()) {
    return;
  }

  const ChatTargetOption &selectedOption = g_chatTargetOptions[index];
  SetActiveChatTarget(selectedOption.name, selectedOption.handle,
                      !selectedOption.isNarrator);
  RefreshChatHeaderLabel();
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
  } else if (choice.type == MANUAL_CHAT_ACTION_DRINK_ITEM ||
             choice.type == MANUAL_CHAT_ACTION_FORCE_DRINK) {
    g_chatActionArgInput->setCaption("Cactus Rum");
  } else {
    g_chatActionArgInput->setCaption("");
  }
}

void OnAutoChatToggleClick(MyGUI::Widget *sender) {
  if (Stobe::ChatMode::IsInjectionMode(g_chatMode)) {
    return;
  }
  g_autoChatEnabled = !g_autoChatEnabled;
  SaveStobeRuntimeConfig();
  RefreshChatModeControls();
}

void RefreshChatModeControls() {
  g_chatMode = Stobe::ChatMode::Normalize(g_chatMode);
  g_lastChatModeIndex = Stobe::ChatMode::ToIndex(g_chatMode);
  if (g_chatModeCombo) {
    if (g_chatModeCombo->getIndexSelected() != g_lastChatModeIndex) {
      g_chatModeCombo->setIndexSelected(g_lastChatModeIndex);
    }
  }

  const bool injectionMode = Stobe::ChatMode::IsInjectionMode(g_chatMode);
  const bool manualActionsAllowed =
      Stobe::ChatMode::AllowsManualActions(g_chatMode);
  if (!manualActionsAllowed) {
    if (g_chatActionCombo && g_chatActionCombo->getIndexSelected() != 0) {
      g_chatActionCombo->setIndexSelected(0);
    }
    if (g_chatActionArgInput) {
      g_chatActionArgInput->setCaption("");
    }
  }
  if (g_chatActionCombo) {
    g_chatActionCombo->setEnabled(manualActionsAllowed);
  }
  if (g_chatActionArgInput) {
    g_chatActionArgInput->setEnabled(manualActionsAllowed);
  }

  if (g_chatAutoChatToggle) {
    g_chatAutoChatToggle->setEnabled(!injectionMode);
    const std::string autoChatCaption =
        injectionMode
            ? "Auto Chat: [N/A]"
            : std::string("Auto Chat: ") +
                  (g_autoChatEnabled ? "[ON]" : "[OFF]");
    g_chatAutoChatToggle->setCaption(WideFromUtf8(autoChatCaption).c_str());
  }
  RefreshAvailableChatTargets(true);
  RefreshChatHeaderLabel();
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

