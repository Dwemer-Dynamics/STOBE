#include "WelcomeWindow.h"
#include "Comm.h"
#include "Globals.h"
#include "Utils.h"

#include <shellapi.h>
#include <windows.h>

#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_ComboBox.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Window.h>

namespace Stobe {
namespace UI {

MyGUI::Window *g_welcomeWindow = nullptr;

namespace {
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

void CloseWelcomeUI() {
  if (g_welcomeWindow) {
    Log("UI: destroying welcome window.");
    if (!TryDestroyWidgetSafe(g_welcomeWindow)) {
      Log("UI_WARN: CloseWelcomeUI destroyWidget failed; clearing stale pointer.");
    }
    g_welcomeWindow = nullptr;
    g_welcomeCheckbox = nullptr;
  }
}

void OnWelcomeDiscordClick(MyGUI::Widget *sender) {
  ShellExecuteA(NULL, "open", "https://discord.gg/B9YgRk8AE8", NULL, NULL,
                SW_SHOWNORMAL);
}

void OnWelcomeOpenServerHomeClick(MyGUI::Widget *sender) {
  const std::string url = GetStobeServerHomeUrl();
  ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

void OnWelcomeToggleClick(MyGUI::Widget *sender) {
  g_enableWelcome = !g_enableWelcome;
  SaveStobeRuntimeConfig();
  ((MyGUI::Button *)sender)
      ->setCaption(g_enableWelcome
                       ? WideFromUtf8(T("MOTD On: [ON]")).c_str()
                       : WideFromUtf8(T("MOTD On: [OFF]")).c_str());
}

void OnWelcomeCloseClick(MyGUI::Widget *sender) { CloseWelcomeUI(); }

void OnWelcomeWindowButtonPressed(MyGUI::Window *sender,
                                  const std::string &name) {
  if (name == "close")
    CloseWelcomeUI();
}

DWORD WINAPI WelcomeResponseThread(LPVOID lpParam) {
  Log("WELCOME_THREAD: Fetching initial config...");
  std::string response = PostToStobeWithResponse(L"/settings", "");
  if (response.empty()) {
    Log("WELCOME_THREAD: Server not responding.");
    return 0;
  }

  std::string pipeMsg = "CMD: POPULATE_WELCOME: " + response;
  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.push_back(pipeMsg);
  LeaveCriticalSection(&g_msgMutex);

  return 0;
}

void CreateWelcomeUI() {
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) {
    Log("UI_WARN: CreateWelcomeUI requested but MyGUI is unavailable.");
    return;
  }
  if (g_welcomeWindow)
    CloseWelcomeUI();

  Log("UI: creating welcome window.");
  g_welcomeWindow = gui->createWidgetReal<MyGUI::Window>(
      "Kenshi_WindowCX", 0.34f, 0.14f, 0.34f, 0.38f, MyGUI::Align::Center,
      "Popup", "Stobe_WelcomeWindow");
  g_welcomeWindow->setCaption(WideFromUtf8("Stobe MOTD").c_str());
  g_welcomeWindow->eventWindowButtonPressed +=
      MyGUI::newDelegate(OnWelcomeWindowButtonPressed);

  MyGUI::Widget *client = g_welcomeWindow->getClientWidget();
  MyGUI::TextBox *info = client->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", 0.05f, 0.08f, 0.9f, 0.34f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_WelcomeInfo");
  std::string motdInfo =
      "Welcome to Stobe.\n\nGeneral Settings Key: [=]\nChat Key: [" +
      g_chatHotkeyStr + "]\nUse General Settings for plugin options.";
  info->setCaption(WideFromUtf8(motdInfo).c_str());
  info->setTextAlign(MyGUI::Align::Center);

  const bool distroConnected = IsDwemerDistroConnected();
  std::string distroStatus =
      std::string("DwemerDistro [") +
      (distroConnected ? "Connected" : "Disconnected") + "]";
  MyGUI::TextBox *distro = client->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", 0.05f, 0.44f, 0.9f, 0.06f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_WelcomeDistroStatus");
  distro->setCaption(WideFromUtf8(distroStatus).c_str());
  distro->setTextAlign(MyGUI::Align::Center);
  distro->setTextColour(distroConnected ? MyGUI::Colour(0.30f, 0.90f, 0.35f)
                                        : MyGUI::Colour(0.95f, 0.30f, 0.30f));

  g_welcomeCheckbox = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.08f, 0.52f, 0.84f, 0.12f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_WelcomeToggle");
  g_welcomeCheckbox->setCaption(WideFromUtf8(g_enableWelcome
                                               ? T("MOTD On: [ON]")
                                               : T("MOTD On: [OFF]"))
                                    .c_str());
  g_welcomeCheckbox->eventMouseButtonClick +=
      MyGUI::newDelegate(OnWelcomeToggleClick);

  MyGUI::Button *serverHomeBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.08f, 0.66f, 0.84f, 0.12f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_WelcomeServerHomeBtn");
  serverHomeBtn->setCaption(WideFromUtf8("Open server page").c_str());
  serverHomeBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnWelcomeOpenServerHomeClick);

  MyGUI::Button *closeBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.30f, 0.80f, 0.40f, 0.14f,
      MyGUI::Align::Bottom | MyGUI::Align::HCenter, "Stobe_WelcomeCloseBtn");
  closeBtn->setCaption(WideFromUtf8("Close").c_str());
  closeBtn->eventMouseButtonClick += MyGUI::newDelegate(OnWelcomeCloseClick);

  Log("UI: welcome window initialized.");
}

} // namespace UI
} // namespace Stobe

