#include "StartingWindow.h"
#include "Globals.h"
#include "AiNpcInfoWindow.h"
#include "JournalWindow.h"
#include "SettingsWindow.h"
#include "Utils.h"
#include "WelcomeWindow.h"
#include <kenshi/GameWorld.h>
#include <windows.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_Colour.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Window.h>

namespace Stobe {
namespace UI {

MyGUI::Window *g_startingWindow = nullptr;
bool g_startingPausedGame = false;

namespace {
bool TryReleaseUserPauseIfNeeded(GameWorld *world) {
  if (!world) {
    return true;
  }
  __try {
    if (world->isPaused()) {
      world->userPause(false);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool TryRequestUserPauseIfNeeded(GameWorld *world, bool *pausedByUs) {
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
} // namespace

void CloseStartingUI() {
  if (g_startingPausedGame) {
    GameWorld *world = GetWorldSafe();
    if (!TryReleaseUserPauseIfNeeded(world)) {
      Log("UI_WARN: CloseStartingUI pause-release failed during world transition.");
    }
    g_startingPausedGame = false;
  }

  if (g_startingWindow) {
    if (!TryDestroyWidgetSafe(g_startingWindow)) {
      Log("UI_WARN: CloseStartingUI destroyWidget failed; clearing stale pointer.");
    }
    g_startingWindow = nullptr;
  }
}

void CloseStobeChildWindows() {
  CloseAiNpcInfoUI();
  CloseAiDiaryUI();
  CloseRecentHistoryUI();
  CloseSettingsUI();
  CloseWelcomeUI();
}

void CloseAllStobeMenuUI() {
  CloseStobeChildWindows();
  CloseStartingUI();
}

bool IsAnyStobeMenuUIOpen() {
  return g_startingWindow || g_aiNpcInfoWindow || g_aiDiaryWindow ||
         g_recentHistoryWindow || g_settingsWindow || g_welcomeWindow;
}

void OnStartingAiNpcsClick(MyGUI::Widget *sender) {
  CloseStobeChildWindows();
  CreateAiNpcInfoUI();
}
void OnStartingAiDiariesClick(MyGUI::Widget *sender) {
  CloseStobeChildWindows();
  CreateAiDiaryUI();
}
void OnStartingHistoryClick(MyGUI::Widget *sender) {
  CloseStobeChildWindows();
  CreateRecentHistoryUI();
}
void OnStartingPluginSettingsClick(MyGUI::Widget *sender) {
  CloseStobeChildWindows();
  CreateSettingsUI();
}
void OnStartingStatusHudClick(MyGUI::Widget *sender) {
  SetStatusHudEnabled(!g_enableStatusHud);
  RefreshStartingUI();
}
void OnStartingWelcomeClick(MyGUI::Widget *sender) {
  CloseStobeChildWindows();
  CreateWelcomeUI();
}

void OnStartingWindowButtonPressed(MyGUI::Window *sender,
                                   const std::string &name) {
  if (name == "close")
    CloseAllStobeMenuUI();
}

void CreateStartingUI() {
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui)
    return;
  if (g_startingWindow)
    CloseStartingUI();
  g_startingPausedGame = false;

  GameWorld *world = GetWorldSafe();
  bool pausedByUs = false;
  if (world && !TryRequestUserPauseIfNeeded(world, &pausedByUs)) {
    Log("UI_WARN: CreateStartingUI pause request failed.");
  }
  g_startingPausedGame = pausedByUs;

  g_startingWindow = gui->createWidgetReal<MyGUI::Window>(
      "Kenshi_WindowCX", 0.03f, 0.07f, 0.20f, 0.86f,
      MyGUI::Align::Left | MyGUI::Align::Top, "Popup", "Stobe_AIHub");

  if (!g_startingWindow) {
    Log("UI_ERROR: window pointer is null.");
    return;
  }

  g_startingWindow->setCaption(WideFromUtf8("STOBE").c_str());
  g_startingWindow->eventWindowButtonPressed +=
      MyGUI::newDelegate(OnStartingWindowButtonPressed);

  MyGUI::Widget *client = g_startingWindow->getClientWidget();
  if (!client) {
    Log("UI_ERROR: starting client widget is null.");
    return;
  }

  MyGUI::TextBox *hotkeyLabel = client->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", 0.05f, 0.02f, 0.9f, 0.13f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_StartingHotkeys");
  hotkeyLabel->setCaption(WideFromUtf8("Menu Key [" + g_generalHotkeyStr +
                                       "]: Open/Close Menu\nChat Hotkey: " +
                                       g_chatHotkeyStr)
                              .c_str());
  hotkeyLabel->setTextColour(MyGUI::Colour(1.0f, 0.86f, 0.20f));
  hotkeyLabel->setTextAlign(MyGUI::Align::Center);

  MyGUI::Button *aiNpcsBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.18f, 0.9f, 0.085f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_StartingAiNpcsBtn");
  aiNpcsBtn->setCaption(WideFromUtf8(T("Stobe NPCs")).c_str());
  aiNpcsBtn->eventMouseButtonClick += MyGUI::newDelegate(OnStartingAiNpcsClick);

  MyGUI::Button *aiDiariesBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.285f, 0.9f, 0.085f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_StartingAiDiariesBtn");
  aiDiariesBtn->setCaption(WideFromUtf8(T("Stobe Diaries")).c_str());
  aiDiariesBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnStartingAiDiariesClick);

  MyGUI::Button *historyBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.39f, 0.9f, 0.085f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_StartingHistoryBtn");
  historyBtn->setCaption(WideFromUtf8(T("Recent History")).c_str());
  historyBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnStartingHistoryClick);

  MyGUI::Button *pluginSettingsBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.495f, 0.9f, 0.085f,
      MyGUI::Align::Top | MyGUI::Align::HStretch,
      "Stobe_StartingPluginSetBtn");
  pluginSettingsBtn->setCaption(WideFromUtf8(T("Settings")).c_str());
  pluginSettingsBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnStartingPluginSettingsClick);

  MyGUI::Button *statusHudBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.60f, 0.9f, 0.085f,
      MyGUI::Align::Top | MyGUI::Align::HStretch,
      "Stobe_StartingStatusHudBtn");
  statusHudBtn->setCaption(
      WideFromUtf8(std::string("Status HUD: ") +
                   (g_enableStatusHud ? "[ON]" : "[OFF]"))
          .c_str());
  statusHudBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnStartingStatusHudClick);

  MyGUI::Button *welcomeBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.705f, 0.9f, 0.085f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_StartingWelBtn");
  welcomeBtn->setCaption(WideFromUtf8(T("MOTD")).c_str());
  welcomeBtn->eventMouseButtonClick += MyGUI::newDelegate(OnStartingWelcomeClick);

  Log("UI: starting window created.");
}

void RefreshStartingUI() {
  if (!g_startingWindow)
    return;
  g_startingWindow->setCaption(WideFromUtf8(T("STOBE")).c_str());
  MyGUI::Widget *client = g_startingWindow->getClientWidget();
  if (!client)
    return;

  struct RefreshMap {
    std::string name;
    std::string key;
  };
  RefreshMap items[] = {{"Stobe_StartingAiNpcsBtn", "Stobe NPCs"},
                         {"Stobe_StartingAiDiariesBtn", "Stobe Diaries"},
                         {"Stobe_StartingHistoryBtn", "Recent History"},
                         {"Stobe_StartingPluginSetBtn", "Settings"},
                         {"Stobe_StartingWelBtn", "MOTD"}};

  for (int i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
    const RefreshMap &item = items[i];
    MyGUI::Widget *w = client->findWidget(item.name);
    if (w) {
      if (w->castType<MyGUI::Button>(false))
        w->castType<MyGUI::Button>()->setCaption(
            WideFromUtf8(T(item.key)).c_str());
      else if (w->castType<MyGUI::TextBox>(false))
        w->castType<MyGUI::TextBox>()->setCaption(
            WideFromUtf8(T(item.key)).c_str());
    }
  }

  MyGUI::Widget *statusWidget =
      client->findWidget("Stobe_StartingStatusHudBtn");
  if (statusWidget && statusWidget->castType<MyGUI::Button>(false)) {
    statusWidget->castType<MyGUI::Button>()->setCaption(
        WideFromUtf8(std::string("Status HUD: ") +
                     (g_enableStatusHud ? "[ON]" : "[OFF]"))
            .c_str());
  }

  MyGUI::Widget *hkWidget = client->findWidget("Stobe_StartingHotkeys");
  if (hkWidget && hkWidget->castType<MyGUI::TextBox>(false)) {
    hkWidget->castType<MyGUI::TextBox>()->setCaption(
        WideFromUtf8("Menu Key [" + g_generalHotkeyStr +
                     "]: Open/Close Menu\nChat Hotkey: " + g_chatHotkeyStr)
            .c_str());
    hkWidget->castType<MyGUI::TextBox>()->setTextColour(
        MyGUI::Colour(1.0f, 0.86f, 0.20f));
  }
}

} // namespace UI
} // namespace Stobe

