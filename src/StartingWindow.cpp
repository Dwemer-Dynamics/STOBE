#include "Interaction.h"
#include "StartingWindow.h"
#include "Globals.h"
#include "AiNpcInfoWindow.h"
#include "JournalWindow.h"
#include "SettingsWindow.h"
#include "SupportReportLauncher.h"
#include "Utils.h"
#include "WelcomeWindow.h"
#include <kenshi/GameWorld.h>
#include <windows.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_Colour.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_InputManager.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Window.h>

namespace Stobe {
namespace UI {

MyGUI::Window *g_startingWindow = nullptr;
bool g_startingPausedGame = false;

namespace {
MyGUI::Window *g_reportConfirmation = nullptr;
std::string g_reportStatus;

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

void CloseReportConfirmation() {
  if (g_reportConfirmation) {
    MyGUI::InputManager *input = MyGUI::InputManager::getInstancePtr();
    if (input) input->removeWidgetModal(g_reportConfirmation);
    TryDestroyWidgetSafe(g_reportConfirmation);
    g_reportConfirmation = nullptr;
  }
}

void OnReportCancel(MyGUI::Widget *) { CloseReportConfirmation(); }

void OnReportWindowClose(MyGUI::Window *, const std::string &name) {
  if (name == "close") CloseReportConfirmation();
}

void OnReportGenerate(MyGUI::Widget *) {
  CloseReportConfirmation();
  SupportReportLauncher::Start();
  g_reportStatus = "Generating logs...";
  RefreshStartingUI();
}

void OnReportKey(MyGUI::Widget *sender, MyGUI::KeyCode key, MyGUI::Char) {
  if (key == MyGUI::KeyCode::Escape) {
    CloseReportConfirmation();
  } else if (key == MyGUI::KeyCode::Return) {
    if (sender->getName() == "Stobe_ReportGenerate") OnReportGenerate(sender);
    else CloseReportConfirmation();
  } else if (key == MyGUI::KeyCode::Tab && g_reportConfirmation) {
    const char *next = sender->getName() == "Stobe_ReportGenerate"
        ? "Stobe_ReportCancel" : "Stobe_ReportGenerate";
    MyGUI::InputManager::getInstance().setKeyFocusWidget(
        g_reportConfirmation->getClientWidget()->findWidget(next));
  }
}

void OnStartingGenerateLogsClick(MyGUI::Widget *) {
  if (SupportReportLauncher::IsRunning()) return;
  CloseStobeChildWindows();
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) return;
  g_reportConfirmation = gui->createWidgetReal<MyGUI::Window>(
      "Kenshi_WindowCX", 0.28f, 0.32f, 0.44f, 0.30f,
      MyGUI::Align::Center, "Popup", "Stobe_ReportConfirmation");
  g_reportConfirmation->setCaption(WideFromUtf8("Generate Logs").c_str());
  g_reportConfirmation->eventWindowButtonPressed += MyGUI::newDelegate(OnReportWindowClose);
  MyGUI::Widget *client = g_reportConfirmation->getClientWidget();
  MyGUI::EditBox *copy = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", 0.05f, 0.05f, 0.9f, 0.60f,
      MyGUI::Align::Default, "Stobe_ReportCopy");
  copy->setCaption(WideFromUtf8("Generate debugging logs, including available Stobe, server and AI logs. Saved to your Desktop.").c_str());
  copy->setTextAlign(MyGUI::Align::Left | MyGUI::Align::Top);
  copy->setEditReadOnly(true);
  copy->setEditStatic(true);
  copy->setEditMultiLine(true);
  copy->setEditWordWrap(true);
  MyGUI::Button *cancel = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.72f, 0.42f, 0.22f,
      MyGUI::Align::Default, "Stobe_ReportCancel");
  cancel->setCaption(WideFromUtf8("Cancel").c_str());
  cancel->eventMouseButtonClick += MyGUI::newDelegate(OnReportCancel);
  MyGUI::Button *generate = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.53f, 0.72f, 0.42f, 0.22f,
      MyGUI::Align::Default, "Stobe_ReportGenerate");
  generate->setCaption(WideFromUtf8("Generate").c_str());
  generate->eventMouseButtonClick += MyGUI::newDelegate(OnReportGenerate);
  cancel->eventKeyButtonPressed += MyGUI::newDelegate(OnReportKey);
  generate->eventKeyButtonPressed += MyGUI::newDelegate(OnReportKey);
  copy->eventKeyButtonPressed += MyGUI::newDelegate(OnReportKey);
  g_reportConfirmation->eventKeyButtonPressed += MyGUI::newDelegate(OnReportKey);
  MyGUI::InputManager::getInstance().addWidgetModal(g_reportConfirmation);
  MyGUI::InputManager::getInstance().setKeyFocusWidget(cancel);
}
} // namespace

void OnInteractionClick(MyGUI::Widget *) {
  Stobe::Interaction::Toggle();
  RefreshInteractionUI();
}

void RefreshInteractionUI() {
  const int state = Stobe::Interaction::Status();
  const std::string caption = state == 1 ? "Stobe: On" : state == 0 ? "Stobe: Off"
      : state == 2 ? "Stobe: Syncing..." : "Stobe is off. Retry";
  MyGUI::Window *windows[] = {g_startingWindow, g_settingsWindow};
  const char *names[] = {"Stobe_Interaction", "Stobe_SettingsInteraction"};
  for (int i = 0; i < 2; ++i) {
    if (!windows[i]) continue;
    MyGUI::Widget *widget = windows[i]->getClientWidget()->findWidget(names[i]);
    if (!widget) continue;
    MyGUI::Button *button = widget->castType<MyGUI::Button>();
    button->setCaption(WideFromUtf8(caption).c_str());
    button->setEnabled(state != 2);
    button->setTextColour(state == 1 ? MyGUI::Colour(0.25f, 1.f, 0.35f) : MyGUI::Colour(1.f, 0.25f, 0.25f));
  }
  if (g_startingWindow) {
    MyGUI::TextBox *hint = g_startingWindow->getClientWidget()->findWidget("Stobe_StartingHotkeys")->castType<MyGUI::TextBox>();
    if (state != 1) {
      hint->setCaption(WideFromUtf8(state == 3 ? "Could not sync. Stobe is off.\nGame events are still recorded."
          : "AI dialogue and actions are off.\nGame events are still recorded.").c_str());
      hint->setTextColour(MyGUI::Colour(1.f, 0.25f, 0.25f));
    }
  }
}

void UpdateSupportReportUI() {
  Stobe::Interaction::Update();
  static int previousState = -1;
  const int state = Stobe::Interaction::Status();
  if (state != previousState) { previousState = state; RefreshStartingUI(); RefreshInteractionUI(); }
  if (!SupportReportLauncher::TakeResult(g_reportStatus)) return;
  RefreshStartingUI();
  GameWorld *world = GetWorldSafe();
  if (world) world->showPlayerAMessage_withLog("[STOBE] " + g_reportStatus, true);
}

void CloseStartingUI() {
  CloseReportConfirmation();
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
  CloseReportConfirmation();
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
      "Kenshi_WindowCX", 0.03f, 0.07f, 0.20f, 0.76f,
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
      "Kenshi_TextboxStandardText", 0.05f, 0.023f, 0.9f, 0.147f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_StartingHotkeys");
  hotkeyLabel->setCaption(WideFromUtf8("Menu Key [" + g_generalHotkeyStr +
                                       "]: Open/Close Menu\nChat Hotkey: " +
                                       g_chatHotkeyStr)
                              .c_str());
  hotkeyLabel->setTextColour(MyGUI::Colour(1.0f, 0.86f, 0.20f));
  hotkeyLabel->setTextAlign(MyGUI::Align::Center);

  MyGUI::Button *interaction = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.18f, 0.9f, 0.075f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_Interaction");
  interaction->eventMouseButtonClick += MyGUI::newDelegate(OnInteractionClick);

  MyGUI::Button *aiNpcsBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.265f, 0.9f, 0.075f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_StartingAiNpcsBtn");
  aiNpcsBtn->setCaption(WideFromUtf8(T("Stobe NPCs")).c_str());
  aiNpcsBtn->eventMouseButtonClick += MyGUI::newDelegate(OnStartingAiNpcsClick);

  MyGUI::Button *aiDiariesBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.350f, 0.9f, 0.075f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_StartingAiDiariesBtn");
  aiDiariesBtn->setCaption(WideFromUtf8(T("Stobe Diaries")).c_str());
  aiDiariesBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnStartingAiDiariesClick);

  MyGUI::Button *historyBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.435f, 0.9f, 0.075f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_StartingHistoryBtn");
  historyBtn->setCaption(WideFromUtf8(T("Recent History")).c_str());
  historyBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnStartingHistoryClick);

  MyGUI::Button *pluginSettingsBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.520f, 0.9f, 0.075f,
      MyGUI::Align::Top | MyGUI::Align::HStretch,
      "Stobe_StartingPluginSetBtn");
  pluginSettingsBtn->setCaption(WideFromUtf8(T("Settings")).c_str());
  pluginSettingsBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnStartingPluginSettingsClick);

  MyGUI::Button *logsBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.605f, 0.9f, 0.075f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_GenerateLogsBtn");
  logsBtn->eventMouseButtonClick += MyGUI::newDelegate(OnStartingGenerateLogsClick);

  MyGUI::Button *statusHudBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.690f, 0.9f, 0.075f,
      MyGUI::Align::Top | MyGUI::Align::HStretch,
      "Stobe_StartingStatusHudBtn");
  statusHudBtn->setCaption(
      WideFromUtf8(std::string("Status HUD: ") +
                   (g_enableStatusHud ? "[ON]" : "[OFF]"))
          .c_str());
  statusHudBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnStartingStatusHudClick);

  MyGUI::Button *welcomeBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, 0.775f, 0.9f, 0.075f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_StartingWelBtn");
  welcomeBtn->setCaption(WideFromUtf8(T("MOTD")).c_str());
  welcomeBtn->eventMouseButtonClick += MyGUI::newDelegate(OnStartingWelcomeClick);

  MyGUI::EditBox *status = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", 0.05f, 0.88f, 0.9f, 0.115f,
      MyGUI::Align::Default, "Stobe_ReportStatus");
  status->setEditReadOnly(true);
  status->setEditMultiLine(true);
  status->setEditWordWrap(true);
  RefreshStartingUI();
  RefreshInteractionUI();
  Log("UI: starting window created.");
}

void RefreshStartingUI() {
  if (!g_startingWindow)
    return;
  g_startingWindow->setCaption(WideFromUtf8(T("STOBE")).c_str());
  MyGUI::Widget *client = g_startingWindow->getClientWidget();
  if (!client)
    return;

  MyGUI::Button *logs = client->findWidget("Stobe_GenerateLogsBtn")->castType<MyGUI::Button>();
  const bool running = SupportReportLauncher::IsRunning();
  logs->setEnabled(!running);
  logs->setCaption(WideFromUtf8(running ? "Generating logs..." : "Generate Logs").c_str());
  MyGUI::Widget *status = client->findWidget("Stobe_ReportStatus");
  status->setVisible(!g_reportStatus.empty());
  status->castType<MyGUI::TextBox>()->setCaption(WideFromUtf8(g_reportStatus).c_str());

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

