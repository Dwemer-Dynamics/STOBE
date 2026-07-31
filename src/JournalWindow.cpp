#include "JournalWindow.h"
#include "AudioPlayback.h"
#include "ChatBox.h"
#include "Comm.h"
#include "Functions.h"
#include "Globals.h"
#include "PlayerBaseState.h"
#include "StobeChatMode.h"
#include "Utils.h"
#include <algorithm>
#include <kenshi/Character.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_ListBox.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Window.h>
#include <string>
#include <vector>

namespace Stobe {
namespace UI {

MyGUI::Window *g_recentHistoryWindow = nullptr;
MyGUI::Window *g_statusHudWindow = nullptr;

namespace {
struct JournalTask {
  std::wstring endpoint;
  std::string json;
  std::string command;
  std::string key;
  std::string field;
};

MyGUI::ListBox *g_historyList = nullptr;
std::vector<std::string> g_historyEntries;
std::string g_historyPendingFilter;

MyGUI::TextBox *g_statusHudText = nullptr;
DWORD g_statusHudLastUpdateTick = 0;

std::string ResolveHudActorState(Character *character) {
  if (!character || reinterpret_cast<uintptr_t>(character) <= 0x1000) {
    return "Unavailable";
  }
  try {
    if (character->isDead()) {
      return "Dead";
    }
    if (character->isUnconcious()) {
      return "Down";
    }
    if (character->isInCombatMode(true, true)) {
      return "Combat";
    }
    if (character->getMovementSpeed() > 0.1f) {
      return "Moving";
    }
    return "Idle";
  } catch (...) {
    return "Unavailable";
  }
}

float ResolveHudActorDistance(Character *anchor, Character *target) {
  if (!anchor || !target || reinterpret_cast<uintptr_t>(anchor) <= 0x1000 ||
      reinterpret_cast<uintptr_t>(target) <= 0x1000) {
    return -1.0f;
  }
  try {
    return anchor->getPosition().distance(target->getPosition());
  } catch (...) {
    return -1.0f;
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

std::string TrimHistoryLine(const std::string &value) {
  size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string BuildHistorySummary(const std::string &entry) {
  size_t firstBreak = entry.find('\n');
  std::string heading =
      TrimHistoryLine(firstBreak == std::string::npos
                          ? entry
                          : entry.substr(0, firstBreak));
  std::string content;
  size_t cursor =
      firstBreak == std::string::npos ? entry.length() : firstBreak + 1;
  while (cursor < entry.length()) {
    size_t next = entry.find('\n', cursor);
    std::string line =
        TrimHistoryLine(next == std::string::npos
                            ? entry.substr(cursor)
                            : entry.substr(cursor, next - cursor));
    if (!line.empty() && line.compare(0, 7, "People:") != 0 &&
        line.compare(0, 9, "Location:") != 0) {
      content = line;
      break;
    }
    if (next == std::string::npos) {
      break;
    }
    cursor = next + 1;
  }

  std::string summary = heading;
  if (!content.empty()) {
    summary += " | " + content;
  }
  if (summary.length() > 240) {
    summary = summary.substr(0, 237) + "...";
  }
  return summary.empty() ? "Recorded event" : summary;
}

void PopulateHistoryEntries(const std::string &text) {
  g_historyEntries.clear();
  std::string current;
  size_t cursor = 0;
  while (cursor <= text.length()) {
    size_t next = text.find('\n', cursor);
    std::string line =
        next == std::string::npos ? text.substr(cursor)
                                  : text.substr(cursor, next - cursor);
    if (!line.empty() && line[line.length() - 1] == '\r') {
      line.erase(line.length() - 1);
    }
    if (TrimHistoryLine(line).empty()) {
      std::string entry = TrimHistoryLine(current);
      if (!entry.empty()) {
        g_historyEntries.push_back(entry);
      }
      current.clear();
    } else {
      if (!current.empty()) {
        current += "\n";
      }
      current += line;
    }
    if (next == std::string::npos) {
      break;
    }
    cursor = next + 1;
  }
  std::string trailingEntry = TrimHistoryLine(current);
  if (!trailingEntry.empty()) {
    g_historyEntries.push_back(trailingEntry);
  }
}

void QueueUiCommand(const std::string &command, const std::string &data) {
  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.push_back("CMD: " + command + ":" + data);
  LeaveCriticalSection(&g_msgMutex);
}

DWORD WINAPI JournalRequestThread(LPVOID lpParam) {
  JournalTask *task = static_cast<JournalTask *>(lpParam);
  std::string response = PostToStobeWithResponse(task->endpoint, task->json);
  std::string value = JsonReadField(response, task->field);
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
  std::string data = task->key + "\n" + value;
  delete task;
  QueueUiCommand(command, data);
  return 0;
}

void StartJournalRequest(const std::wstring &endpoint, const std::string &json,
                         const std::string &command, const std::string &key,
                         const std::string &field) {
  JournalTask *task = new JournalTask();
  task->endpoint = endpoint;
  task->json = json;
  task->command = command;
  task->key = key;
  task->field = field;
  HANDLE thread = CreateThread(NULL, 0, JournalRequestThread, task, 0, NULL);
  if (!thread) {
    delete task;
    Log("UI_WARN: failed to start journal request worker.");
    return;
  }
  CloseHandle(thread);
}

void RefreshHistory() {
  g_historyPendingFilter = "default";
  if (g_historyList) {
    g_historyList->removeAllItems();
    g_historyList->addItem(WideFromUtf8("Loading recent events...").c_str());
  }
  g_historyEntries.clear();
  StartJournalRequest(
      L"/ai_history",
      "{\"filter\":\"" + g_historyPendingFilter + "\",\"limit\":60}",
      "SET_STOBE_HISTORY", g_historyPendingFilter, "text");
}

void OnHistoryRefreshClick(MyGUI::Widget *sender) { RefreshHistory(); }

void OnHistoryWindowButtonPressed(MyGUI::Window *sender,
                                  const std::string &name) {
  if (name == "close") {
    CloseRecentHistoryUI();
  }
}

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
  g_historyList = nullptr;
  g_historyEntries.clear();
  g_historyPendingFilter.clear();
}

void SetRecentHistoryText(const std::string &data) {
  size_t split = data.find('\n');
  if (split == std::string::npos) {
    if (g_historyList) {
      g_historyList->removeAllItems();
      g_historyList->addItem(WideFromUtf8(data).c_str());
    }
    return;
  }
  std::string key = data.substr(0, split);
  if (key != g_historyPendingFilter) {
    Log("UI: ignored stale history response filter=" + key);
    return;
  }
  PopulateHistoryEntries(data.substr(split + 1));
  if (g_historyList) {
    g_historyList->removeAllItems();
    for (size_t i = 0; i < g_historyEntries.size(); ++i) {
      g_historyList->addItem(
          WideFromUtf8(BuildHistorySummary(g_historyEntries[i])).c_str());
    }
  }
  if (g_historyEntries.empty()) {
    if (g_historyList) {
      g_historyList->addItem(
          WideFromUtf8("No recent events have been recorded.").c_str());
    }
  }
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

  MyGUI::Button *refreshButton = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.03f, 0.03f, 0.16f, 0.07f,
      MyGUI::Align::Default, "Stobe_HistoryRefresh");
  refreshButton->setCaption(WideFromUtf8("Refresh").c_str());
  refreshButton->eventMouseButtonClick +=
      MyGUI::newDelegate(OnHistoryRefreshClick);

  g_historyList = client->createWidgetReal<MyGUI::ListBox>(
      "Kenshi_ListBox", 0.03f, 0.12f, 0.94f, 0.84f,
      MyGUI::Align::Default, "Stobe_HistoryList");
  RefreshHistory();
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
  RequestProfileModelSlotRefresh();
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui || !world) {
    return;
  }

  if (!g_statusHudWindow) {
    g_statusHudWindow = gui->createWidgetReal<MyGUI::Window>(
        "Kenshi_WindowCX", 0.78f, 0.03f, 0.20f, 0.17f,
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

  Character *selectedCharacter = nullptr;
  std::string selectedName = "None";
  if (world->player && world->player->selectedCharacter.isValid()) {
    selectedCharacter =
        ResolveLiveCharacter(world, world->player->selectedCharacter);
    if (selectedCharacter &&
        reinterpret_cast<uintptr_t>(selectedCharacter) > 0x1000) {
      try {
        selectedName = selectedCharacter->getName();
      } catch (...) {
        selectedName = "Unavailable";
        selectedCharacter = nullptr;
      }
    }
  }

  std::string targetedSummary = "None";
  if (g_talkTargetHand.isValid() && g_talkTargetHand.serial != 0) {
    Character *targeted = ResolveLiveCharacter(world, g_talkTargetHand);
    if (targeted && reinterpret_cast<uintptr_t>(targeted) > 0x1000) {
      std::string targetedName = "Unavailable";
      try {
        targetedName = targeted->getName();
      } catch (...) {
        targeted = nullptr;
      }
      if (targeted) {
        Character *distanceAnchor = selectedCharacter;
        if (!distanceAnchor && world->player &&
            world->player->playerCharacters.size() > 0) {
          distanceAnchor = world->player->playerCharacters[0];
        }
        targetedSummary = targetedName;
        float distance = ResolveHudActorDistance(distanceAnchor, targeted);
        if (distance >= 0.0f) {
          targetedSummary +=
              " | " + ToString(static_cast<int>(distance + 0.5f)) + "m";
        }
        targetedSummary += " | " + ResolveHudActorState(targeted);
      }
    }
  }

  std::string aiStatus = "Idle";
  if (IsTtsPlaybackActive()) {
    aiStatus = "Speaking";
  } else if (IsAiRequestActive()) {
    aiStatus = "Thinking";
  }

  std::string caption =
      "Mode: " + Stobe::ChatMode::DisplayLabel(g_chatMode) +
      "\nResponse: " + GetActiveProfileModelLabel() +
      "\nTTS: " + (g_ttsEnabled ? "ON" : "OFF") +
      "\nAI: " + aiStatus +
       "\nSelected: " + selectedName +
       "\nTargeted: " + targetedSummary;
  Stobe::PlayerBase::Snapshot playerBase;
  const bool showPlayerBase =
      Stobe::PlayerBase::GetSelectedSnapshot(playerBase) && playerBase.inside;
  if (showPlayerBase) {
    caption += "\nBase: " + playerBase.name + " | Power " +
               ToString(static_cast<int>(playerBase.powerGenerated + 0.5f)) +
               "/" +
               ToString(static_cast<int>(playerBase.powerRequired + 0.5f));
  }
  if (g_autoChatEnabled) {
    caption += "\nAuto Chat: On";
  }
  g_statusHudWindow->setSize(
      g_statusHudWindow->getWidth(),
      showPlayerBase || g_autoChatEnabled
          ? static_cast<int>(g_statusHudWindow->getParentSize().height * 0.19f)
          : static_cast<int>(g_statusHudWindow->getParentSize().height * 0.17f));
  if (g_statusHudText) {
    g_statusHudText->setCaption(WideFromUtf8(caption).c_str());
  }
}

} // namespace UI
} // namespace Stobe
