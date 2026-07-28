#include "JournalWindow.h"
#include "Comm.h"
#include "Functions.h"
#include "Globals.h"
#include "StartingWindow.h"
#include "Utils.h"
#include <algorithm>
#include <cctype>
#include <kenshi/Character.h>
#include <kenshi/Faction.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_ComboBox.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_ListBox.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Window.h>
#include <string>
#include <vector>

namespace Stobe {
namespace UI {

MyGUI::Window *g_recentHistoryWindow = nullptr;
MyGUI::Window *g_worldJournalWindow = nullptr;
MyGUI::Window *g_statusHudWindow = nullptr;

namespace {
struct JournalTask {
  std::wstring endpoint;
  std::string json;
  std::string command;
  std::string key;
  std::string field;
  bool includeTravel;
};

MyGUI::ComboBox *g_historyFilter = nullptr;
MyGUI::EditBox *g_historyText = nullptr;
std::string g_historyPendingFilter;

MyGUI::ComboBox *g_worldView = nullptr;
MyGUI::ListBox *g_worldList = nullptr;
MyGUI::EditBox *g_worldText = nullptr;
MyGUI::Button *g_worldTravelButton = nullptr;
std::vector<std::string> g_worldEntryIds;
std::string g_worldMode = "locations";
std::string g_worldPendingKey;
std::string g_worldTravelPayload;

MyGUI::TextBox *g_statusHudText = nullptr;
DWORD g_statusHudLastUpdateTick = 0;

void TrimInline(std::string &value) {
  size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    value.clear();
    return;
  }
  size_t last = value.find_last_not_of(" \t\r\n");
  value = value.substr(first, last - first + 1);
}

void ParsePipeEntryArray(const std::string &dataInput,
                         std::vector<std::string> &displayOut,
                         std::vector<std::string> &valueOut) {
  displayOut.clear();
  valueOut.clear();
  std::string data = dataInput;
  if (!data.empty() && data[0] == '[') {
    data.erase(0, 1);
  }
  if (!data.empty() && data[data.length() - 1] == ']') {
    data.erase(data.length() - 1);
  }

  size_t cur = 0;
  while (cur < data.length()) {
    size_t next = data.find(",", cur);
    std::string entry = next == std::string::npos
                            ? data.substr(cur)
                            : data.substr(cur, next - cur);
    TrimInline(entry);
    if (!entry.empty()) {
      if (entry[0] == '"') {
        entry.erase(0, 1);
      }
      if (!entry.empty() && entry[entry.length() - 1] == '"') {
        entry.erase(entry.length() - 1);
      }
      size_t pipe = entry.find("|");
      std::string display = pipe == std::string::npos ? entry
                                                       : entry.substr(0, pipe);
      std::string value = pipe == std::string::npos ? entry
                                                     : entry.substr(pipe + 1);
      TrimInline(display);
      TrimInline(value);
      if (!display.empty()) {
        displayOut.push_back(display);
        valueOut.push_back(value);
      }
    }
    if (next == std::string::npos) {
      break;
    }
    cur = next + 1;
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

void ConfigureReadOnlyText(MyGUI::EditBox *box) {
  if (!box) {
    return;
  }
  box->setEditReadOnly(true);
  box->setEditMultiLine(true);
  box->setEditWordWrap(true);
  box->setVisibleVScroll(true);
  box->setVisibleHScroll(false);
  box->setNeedKeyFocus(false);
}

void SetReadOnlyText(MyGUI::EditBox *box, const std::string &text) {
  if (!box) {
    return;
  }
  box->setOnlyText(WideFromUtf8(text).c_str());
  box->setVScrollPosition(0);
}

void QueueUiCommand(const std::string &command, const std::string &data) {
  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.push_back("CMD: " + command + ":" + data);
  LeaveCriticalSection(&g_msgMutex);
}

void NotifyPlayer(const std::string &message) {
  GameWorld *world = GetWorldSafe();
  if (world) {
    try {
      world->showPlayerAMessage_withLog(message, true);
      return;
    } catch (...) {
    }
  }
  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.push_back("NOTIFY:" + message);
  LeaveCriticalSection(&g_msgMutex);
}

DWORD WINAPI JournalRequestThread(LPVOID lpParam) {
  JournalTask *task = static_cast<JournalTask *>(lpParam);
  std::string response = PostToStobeWithResponse(task->endpoint, task->json);
  std::string value = JsonReadField(response, task->field);
  std::string travel =
      task->includeTravel ? JsonReadField(response, "travel") : "";
  if (value.empty()) {
    value = JsonReadField(response, "error");
  }
  if (value.empty()) {
    value = task->field == "entries"
                ? "[]"
                : (response.empty() ? "Unable to contact StobeServer."
                                    : "The server returned no readable data.");
  }
  std::string command = task->command;
  std::string data = task->key + "\n";
  if (task->includeTravel) {
    data += travel + "\n";
  }
  data += value;
  delete task;
  QueueUiCommand(command, data);
  return 0;
}

void StartJournalRequest(const std::wstring &endpoint, const std::string &json,
                         const std::string &command, const std::string &key,
                         const std::string &field, bool includeTravel = false) {
  JournalTask *task = new JournalTask();
  task->endpoint = endpoint;
  task->json = json;
  task->command = command;
  task->key = key;
  task->field = field;
  task->includeTravel = includeTravel;
  HANDLE thread = CreateThread(NULL, 0, JournalRequestThread, task, 0, NULL);
  if (!thread) {
    delete task;
    Log("UI_WARN: failed to start journal request worker.");
    return;
  }
  CloseHandle(thread);
}

std::string SelectedHistoryFilter() {
  if (!g_historyFilter ||
      g_historyFilter->getIndexSelected() == MyGUI::ITEM_NONE) {
    return "all";
  }
  switch (g_historyFilter->getIndexSelected()) {
  case 1:
    return "dialogue";
  case 2:
    return "actions";
  case 3:
    return "travel";
  case 4:
    return "combat";
  case 5:
    return "trade";
  default:
    return "all";
  }
}

void RefreshHistory() {
  g_historyPendingFilter = SelectedHistoryFilter();
  SetReadOnlyText(g_historyText, "Loading recent events...");
  StartJournalRequest(
      L"/ai_history",
      "{\"filter\":\"" + g_historyPendingFilter + "\",\"limit\":60}",
      "SET_STOBE_HISTORY", g_historyPendingFilter, "text");
}

void OnHistoryFilterChanged(MyGUI::ComboBox *sender, size_t index) {
  if (index != MyGUI::ITEM_NONE) {
    RefreshHistory();
  }
}

void OnHistoryRefreshClick(MyGUI::Widget *sender) { RefreshHistory(); }

void OnHistoryWindowButtonPressed(MyGUI::Window *sender,
                                  const std::string &name) {
  if (name == "close") {
    CloseRecentHistoryUI();
  }
}

void RefreshWorldView() {
  if (!g_worldList || !g_worldText) {
    return;
  }
  g_worldList->removeAllItems();
  g_worldEntryIds.clear();
  g_worldTravelPayload.clear();
  g_worldPendingKey = g_worldMode;
  if (g_worldTravelButton) {
    g_worldTravelButton->setVisible(false);
  }
  SetReadOnlyText(g_worldText, "Loading world journal...");

  if (g_worldMode == "events") {
    g_worldList->setVisible(false);
    StartJournalRequest(L"/ai_world", "{\"action\":\"events\"}",
                        "SET_STOBE_WORLD_DETAIL", "events", "text", true);
    return;
  }

  g_worldList->setVisible(true);
  std::string action = g_worldMode == "factions" ? "factions" : "locations";
  StartJournalRequest(L"/ai_world", "{\"action\":\"" + action + "\"}",
                      "POPULATE_STOBE_WORLD_LIST", g_worldMode, "entries");
}

void OnWorldViewChanged(MyGUI::ComboBox *sender, size_t index) {
  if (index == MyGUI::ITEM_NONE) {
    return;
  }
  g_worldMode = index == 1 ? "factions" : (index == 2 ? "events" : "locations");
  RefreshWorldView();
}

void OnWorldEntrySelect(MyGUI::ListBox *sender, size_t index) {
  if (index == MyGUI::ITEM_NONE || index >= g_worldEntryIds.size()) {
    return;
  }
  std::string id = g_worldEntryIds[index];
  g_worldPendingKey = g_worldMode + ":" + id;
  g_worldTravelPayload.clear();
  if (g_worldTravelButton) {
    g_worldTravelButton->setVisible(false);
  }
  SetReadOnlyText(g_worldText, "Loading journal entry...");

  std::string action = g_worldMode == "factions" ? "faction" : "location";
  StartJournalRequest(
      L"/ai_world",
      "{\"action\":\"" + action + "\",\"id\":" + id + "}",
      "SET_STOBE_WORLD_DETAIL", g_worldPendingKey, "text", true);
}

void OnWorldTravelClick(MyGUI::Widget *sender) {
  if (g_worldTravelPayload.empty()) {
    NotifyPlayer("Select a visited location first.");
    return;
  }

  GameWorld *world = GetWorldSafe();
  if (!world || !world->player ||
      !world->player->selectedCharacter.isValid() ||
      world->player->selectedCharacter.serial == 0) {
    NotifyPlayer("Select a player-faction squad member before traveling.");
    return;
  }

  Character *actor =
      ResolveLiveCharacter(world, world->player->selectedCharacter);
  bool playerFaction = false;
  if (actor && reinterpret_cast<uintptr_t>(actor) > 0x1000) {
    try {
      Faction *faction = actor->getFaction();
      playerFaction = faction && faction->isThePlayer();
    } catch (...) {
      playerFaction = false;
    }
  }
  if (!playerFaction) {
    NotifyPlayer("Travel Here only works for a selected player-faction squad member.");
    return;
  }

  hand actorHandle;
  try {
    actorHandle = actor->getHandle();
  } catch (...) {
    actorHandle = hand();
  }
  if (!actorHandle.isValid() || actorHandle.serial == 0) {
    NotifyPlayer("The selected squad member is not available.");
    return;
  }

  EnterCriticalSection(&g_uiMutex);
  QueuedAction action;
  action.type = ACT_TRAVEL_LOCATION;
  action.actor = actorHandle;
  action.target = hand();
  action.message = g_worldTravelPayload;
  g_uiActionQueue.push_back(action);
  LeaveCriticalSection(&g_uiMutex);

  Log("UI_TRAVEL: queued actor_serial=" + ToString(actorHandle.serial) +
      " payload='" + g_worldTravelPayload + "'");
  CloseAllStobeMenuUI();
}

void OnWorldWindowButtonPressed(MyGUI::Window *sender,
                                const std::string &name) {
  if (name == "close") {
    CloseWorldJournalUI();
  }
}

void OnWorldCloseClick(MyGUI::Widget *sender) { CloseWorldJournalUI(); }

void OnStatusHudWindowButtonPressed(MyGUI::Window *sender,
                                    const std::string &name) {
  if (name == "close") {
    SetStatusHudEnabled(false);
  }
}
} // namespace

void CloseRecentHistoryUI() {
  if (g_recentHistoryWindow &&
      !TryDestroyWidgetSafe(g_recentHistoryWindow)) {
    Log("UI_WARN: failed to destroy Recent History window.");
  }
  g_recentHistoryWindow = nullptr;
  g_historyFilter = nullptr;
  g_historyText = nullptr;
  g_historyPendingFilter.clear();
}

void SetRecentHistoryText(const std::string &data) {
  size_t split = data.find('\n');
  if (split == std::string::npos) {
    SetReadOnlyText(g_historyText, data);
    return;
  }
  std::string key = data.substr(0, split);
  if (key != g_historyPendingFilter) {
    Log("UI: ignored stale history response filter=" + key);
    return;
  }
  SetReadOnlyText(g_historyText, data.substr(split + 1));
}

void CreateRecentHistoryUI() {
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) {
    return;
  }
  CloseRecentHistoryUI();

  g_recentHistoryWindow = gui->createWidgetReal<MyGUI::Window>(
      "Kenshi_WindowCX", 0.15f, 0.10f, 0.70f, 0.80f, MyGUI::Align::Center,
      "Popup", "Stobe_RecentHistoryWindow");
  if (!g_recentHistoryWindow) {
    return;
  }
  g_recentHistoryWindow->setCaption(WideFromUtf8("Recent History").c_str());
  g_recentHistoryWindow->eventWindowButtonPressed +=
      MyGUI::newDelegate(OnHistoryWindowButtonPressed);
  MyGUI::Widget *client = g_recentHistoryWindow->getClientWidget();
  if (!client) {
    return;
  }

  g_historyFilter = client->createWidgetReal<MyGUI::ComboBox>(
      "Kenshi_ComboBox", 0.03f, 0.03f, 0.24f, 0.07f,
      MyGUI::Align::Default, "Stobe_HistoryFilter");
  g_historyFilter->setComboModeDrop(true);
  g_historyFilter->addItem(WideFromUtf8("All Events").c_str());
  g_historyFilter->addItem(WideFromUtf8("Dialogue").c_str());
  g_historyFilter->addItem(WideFromUtf8("Actions").c_str());
  g_historyFilter->addItem(WideFromUtf8("Travel").c_str());
  g_historyFilter->addItem(WideFromUtf8("Combat").c_str());
  g_historyFilter->addItem(WideFromUtf8("Trade").c_str());
  g_historyFilter->setIndexSelected(0);
  g_historyFilter->eventComboChangePosition +=
      MyGUI::newDelegate(OnHistoryFilterChanged);

  MyGUI::Button *refreshButton = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.29f, 0.03f, 0.16f, 0.07f,
      MyGUI::Align::Default, "Stobe_HistoryRefresh");
  refreshButton->setCaption(WideFromUtf8("Refresh").c_str());
  refreshButton->eventMouseButtonClick +=
      MyGUI::newDelegate(OnHistoryRefreshClick);

  g_historyText = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", 0.03f, 0.12f, 0.94f, 0.74f,
      MyGUI::Align::Default, "Stobe_HistoryText");
  ConfigureReadOnlyText(g_historyText);
  RefreshHistory();
}

void CloseWorldJournalUI() {
  if (g_worldJournalWindow &&
      !TryDestroyWidgetSafe(g_worldJournalWindow)) {
    Log("UI_WARN: failed to destroy World Journal window.");
  }
  g_worldJournalWindow = nullptr;
  g_worldView = nullptr;
  g_worldList = nullptr;
  g_worldText = nullptr;
  g_worldTravelButton = nullptr;
  g_worldEntryIds.clear();
  g_worldPendingKey.clear();
  g_worldTravelPayload.clear();
}

void PopulateWorldJournalList(const std::string &data) {
  size_t split = data.find('\n');
  if (split == std::string::npos || !g_worldList) {
    return;
  }
  std::string key = data.substr(0, split);
  if (key != g_worldMode) {
    Log("UI: ignored stale world list response mode=" + key);
    return;
  }

  g_worldList->removeAllItems();
  g_worldEntryIds.clear();
  std::vector<std::string> displays;
  std::vector<std::string> values;
  ParsePipeEntryArray(data.substr(split + 1), displays, values);
  for (size_t i = 0; i < displays.size() && i < values.size(); ++i) {
    g_worldList->addItem(WideFromUtf8(displays[i]).c_str());
    g_worldEntryIds.push_back(values[i]);
  }
  SetReadOnlyText(g_worldText,
                  g_worldEntryIds.empty()
                      ? "No entries have been recorded for this view."
                      : "Select an entry to view its details.");
}

void SetWorldJournalDetail(const std::string &data) {
  size_t first = data.find('\n');
  size_t second =
      first == std::string::npos ? std::string::npos : data.find('\n', first + 1);
  if (first == std::string::npos || second == std::string::npos) {
    SetReadOnlyText(g_worldText, data);
    return;
  }
  std::string key = data.substr(0, first);
  if (key != g_worldPendingKey) {
    Log("UI: ignored stale world detail response key=" + key);
    return;
  }

  g_worldTravelPayload = data.substr(first + 1, second - first - 1);
  SetReadOnlyText(g_worldText, data.substr(second + 1));
  if (g_worldTravelButton) {
    g_worldTravelButton->setVisible(g_worldMode == "locations" &&
                                    !g_worldTravelPayload.empty());
  }
}

void CreateWorldJournalUI() {
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) {
    return;
  }
  CloseWorldJournalUI();

  g_worldJournalWindow = gui->createWidgetReal<MyGUI::Window>(
      "Kenshi_WindowCX", 0.10f, 0.08f, 0.80f, 0.84f, MyGUI::Align::Center,
      "Popup", "Stobe_WorldJournalWindow");
  if (!g_worldJournalWindow) {
    return;
  }
  g_worldJournalWindow->setCaption(WideFromUtf8("World Journal").c_str());
  g_worldJournalWindow->eventWindowButtonPressed +=
      MyGUI::newDelegate(OnWorldWindowButtonPressed);
  MyGUI::Widget *client = g_worldJournalWindow->getClientWidget();
  if (!client) {
    return;
  }

  g_worldView = client->createWidgetReal<MyGUI::ComboBox>(
      "Kenshi_ComboBox", 0.03f, 0.03f, 0.28f, 0.07f,
      MyGUI::Align::Default, "Stobe_WorldView");
  g_worldView->setComboModeDrop(true);
  g_worldView->addItem(WideFromUtf8("Visited Locations").c_str());
  g_worldView->addItem(WideFromUtf8("Faction Relations").c_str());
  g_worldView->addItem(WideFromUtf8("World Events").c_str());
  g_worldView->setIndexSelected(0);
  g_worldView->eventComboChangePosition +=
      MyGUI::newDelegate(OnWorldViewChanged);

  g_worldList = client->createWidgetReal<MyGUI::ListBox>(
      "Kenshi_ListBox", 0.03f, 0.12f, 0.35f, 0.73f,
      MyGUI::Align::Default, "Stobe_WorldList");
  g_worldList->eventListChangePosition +=
      MyGUI::newDelegate(OnWorldEntrySelect);

  g_worldText = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", 0.40f, 0.12f, 0.57f, 0.73f,
      MyGUI::Align::Default, "Stobe_WorldDetail");
  ConfigureReadOnlyText(g_worldText);

  g_worldTravelButton = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.61f, 0.88f, 0.20f, 0.08f,
      MyGUI::Align::Default, "Stobe_WorldTravel");
  g_worldTravelButton->setCaption(WideFromUtf8("Travel Here").c_str());
  g_worldTravelButton->setVisible(false);
  g_worldTravelButton->eventMouseButtonClick +=
      MyGUI::newDelegate(OnWorldTravelClick);

  MyGUI::Button *closeButton = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.83f, 0.88f, 0.14f, 0.08f,
      MyGUI::Align::Default, "Stobe_WorldClose");
  closeButton->setCaption(WideFromUtf8("Close").c_str());
  closeButton->eventMouseButtonClick +=
      MyGUI::newDelegate(OnWorldCloseClick);

  g_worldMode = "locations";
  RefreshWorldView();
}

void CloseStatusHud() {
  if (g_statusHudWindow && !TryDestroyWidgetSafe(g_statusHudWindow)) {
    Log("UI_WARN: failed to destroy Status HUD.");
  }
  g_statusHudWindow = nullptr;
  g_statusHudText = nullptr;
  g_statusHudLastUpdateTick = 0;
}

void SetStatusHudEnabled(bool enabled) {
  g_enableStatusHud = enabled;
  SaveStobeRuntimeConfig();
  if (!enabled) {
    CloseStatusHud();
  } else {
    UpdateStatusHud(GetWorldSafe());
  }
}

void UpdateStatusHud(GameWorld *world) {
  if (!g_enableStatusHud) {
    CloseStatusHud();
    return;
  }
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui || !world) {
    return;
  }

  if (!g_statusHudWindow) {
    g_statusHudWindow = gui->createWidgetReal<MyGUI::Window>(
        "Kenshi_WindowCX", 0.76f, 0.03f, 0.22f, 0.16f,
        MyGUI::Align::Right | MyGUI::Align::Top, "Popup",
        "Stobe_StatusHudWindow");
    if (!g_statusHudWindow) {
      return;
    }
    g_statusHudWindow->setCaption(WideFromUtf8("STOBE").c_str());
    g_statusHudWindow->eventWindowButtonPressed +=
        MyGUI::newDelegate(OnStatusHudWindowButtonPressed);
    MyGUI::Widget *client = g_statusHudWindow->getClientWidget();
    if (!client) {
      CloseStatusHud();
      return;
    }
    g_statusHudText = client->createWidgetReal<MyGUI::TextBox>(
        "Kenshi_TextboxStandardText", 0.04f, 0.08f, 0.92f, 0.84f,
        MyGUI::Align::Stretch, "Stobe_StatusHudText");
  }

  DWORD now = GetTickCount();
  if (now - g_statusHudLastUpdateTick < 500) {
    return;
  }
  g_statusHudLastUpdateTick = now;

  std::string selectedName = "None";
  if (world->player && world->player->selectedCharacter.isValid()) {
    Character *selected =
        ResolveLiveCharacter(world, world->player->selectedCharacter);
    if (selected && reinterpret_cast<uintptr_t>(selected) > 0x1000) {
      try {
        selectedName = selected->getName();
      } catch (...) {
        selectedName = "Unavailable";
      }
    }
  }

  size_t queuedActions = 0;
  EnterCriticalSection(&g_uiMutex);
  queuedActions = g_uiActionQueue.size();
  LeaveCriticalSection(&g_uiMutex);
  std::string caption =
      "Server: " + g_serverHost + ":" + ToString(g_serverPort) +
      "\nMode: " + g_chatMode + " | TTS " + (g_ttsEnabled ? "ON" : "OFF") +
      "\nSelected: " + selectedName +
      "\nQueued actions: " + ToString(static_cast<unsigned int>(queuedActions));
  if (g_statusHudText) {
    g_statusHudText->setCaption(WideFromUtf8(caption).c_str());
  }
}

} // namespace UI
} // namespace Stobe
