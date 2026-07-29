#include "JournalWindow.h"
#include "Comm.h"
#include "Functions.h"
#include "Globals.h"
#include "Utils.h"
#include <algorithm>
#include <kenshi/Character.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Gui.h>
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

MyGUI::EditBox *g_historyText = nullptr;
std::string g_historyPendingFilter;

MyGUI::TextBox *g_statusHudText = nullptr;
DWORD g_statusHudLastUpdateTick = 0;

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
  g_historyPendingFilter = "all";
  SetReadOnlyText(g_historyText, "Loading recent events...");
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

  MyGUI::Button *refreshButton = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.03f, 0.03f, 0.16f, 0.07f,
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
        "Kenshi_WindowCX", 0.76f, 0.03f, 0.22f, 0.22f,
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

  std::vector<std::string> activatedActors = SnapshotActivatedAiActorNames();
  std::string activatedActorText = "None";
  if (!activatedActors.empty()) {
    activatedActorText.clear();
    const size_t visibleActorLimit = 6;
    size_t visibleActors = std::min(activatedActors.size(), visibleActorLimit);
    for (size_t i = 0; i < visibleActors; ++i) {
      if (!activatedActorText.empty()) {
        activatedActorText += ", ";
      }
      activatedActorText += activatedActors[i];
    }
    if (activatedActors.size() > visibleActors) {
      activatedActorText += " (+" +
                            ToString(static_cast<unsigned int>(
                                activatedActors.size() - visibleActors)) +
                            " more)";
    }
  }
  std::string caption =
      "Mode: " + g_chatMode +
      "\nTTS: " + (g_ttsEnabled ? "ON" : "OFF") +
      "\nSelected: " + selectedName +
      "\nActivated AI actors (" +
      ToString(static_cast<unsigned int>(activatedActors.size())) +
      "):\n" + activatedActorText;
  if (g_statusHudText) {
    g_statusHudText->setCaption(WideFromUtf8(caption).c_str());
  }
}

} // namespace UI
} // namespace Stobe
