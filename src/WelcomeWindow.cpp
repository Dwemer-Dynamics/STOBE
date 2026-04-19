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
MyGUI::ComboBox *g_welcomeGeneralHotkeyCombo = nullptr;

namespace {
std::string BuildWelcomeInfoText() {
  const std::string pluginVersion =
      GetStobePluginVersion() ? GetStobePluginVersion() : "";
  return "Welcome to Stobe.\nPlugin Version: " + pluginVersion +
         "\n\nSTOBE Settings Key: [" + g_generalHotkeyStr +
         "]\nChat Key: [" + g_chatHotkeyStr +
         "]\nUse Settings for plugin options.";
}

void RefreshWelcomeInfoText() {
  if (!g_welcomeWindow) {
    return;
  }
  MyGUI::Widget *client = g_welcomeWindow->getClientWidget();
  if (!client) {
    return;
  }
  MyGUI::Widget *infoWidget = client->findWidget("Stobe_WelcomeInfo");
  if (!infoWidget || !infoWidget->castType<MyGUI::TextBox>(false)) {
    return;
  }
  infoWidget->castType<MyGUI::TextBox>()->setCaption(
      WideFromUtf8(BuildWelcomeInfoText()).c_str());
}

void PopulateWelcomeGeneralHotkeyCombo() {
  if (!g_welcomeGeneralHotkeyCombo) {
    return;
  }
  g_welcomeGeneralHotkeyCombo->removeAllItems();
  g_welcomeGeneralHotkeyCombo->addItem(WideFromUtf8("=").c_str());
  g_welcomeGeneralHotkeyCombo->addItem(WideFromUtf8("F7").c_str());
  g_welcomeGeneralHotkeyCombo->addItem(WideFromUtf8("F8").c_str());
  g_welcomeGeneralHotkeyCombo->addItem(WideFromUtf8("F11").c_str());
  g_welcomeGeneralHotkeyCombo->addItem(WideFromUtf8("F12").c_str());
  g_welcomeGeneralHotkeyCombo->addItem(WideFromUtf8("O").c_str());
  g_welcomeGeneralHotkeyCombo->addItem(WideFromUtf8("[").c_str());
  g_welcomeGeneralHotkeyCombo->addItem(WideFromUtf8("}").c_str());

  std::string current = g_generalHotkeyStr;
  if (current == "]") {
    current = "}";
  }
  size_t selected = 0;
  for (size_t i = 0; i < g_welcomeGeneralHotkeyCombo->getItemCount(); ++i) {
    if (g_welcomeGeneralHotkeyCombo->getItemNameAt(i) == current) {
      selected = i;
      break;
    }
  }
  g_welcomeGeneralHotkeyCombo->setIndexSelected(selected);
}

void OnWelcomeGeneralHotkeyChanged(MyGUI::ComboBox *sender, size_t index) {
  if (!sender || index == MyGUI::ITEM_NONE) {
    return;
  }
  SetGeneralHotkeyFromString(sender->getItemNameAt(index));
  SaveStobeRuntimeConfig();
  RefreshWelcomeInfoText();
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

void CloseWelcomeUI() {
  if (g_welcomeWindow) {
    Log("UI: destroying welcome window.");
    if (!TryDestroyWidgetSafe(g_welcomeWindow)) {
      Log("UI_WARN: CloseWelcomeUI destroyWidget failed; clearing stale pointer.");
    }
    g_welcomeWindow = nullptr;
    g_welcomeCheckbox = nullptr;
    g_welcomeGeneralHotkeyCombo = nullptr;
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
      "Kenshi_WindowCX", 0.34f, 0.10f, 0.34f, 0.46f, MyGUI::Align::Center,
      "Overlapped", "Stobe_WelcomeWindow");
  g_welcomeWindow->setCaption(WideFromUtf8("Stobe MOTD").c_str());
  g_welcomeWindow->eventWindowButtonPressed +=
      MyGUI::newDelegate(OnWelcomeWindowButtonPressed);

  MyGUI::Widget *client = g_welcomeWindow->getClientWidget();
  MyGUI::TextBox *info = client->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", 0.05f, 0.07f, 0.9f, 0.25f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_WelcomeInfo");
  info->setCaption(WideFromUtf8(BuildWelcomeInfoText()).c_str());
  info->setTextAlign(MyGUI::Align::Center);

  MyGUI::TextBox *hotkeyLabel = client->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", 0.05f, 0.34f, 0.45f, 0.07f,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_WelcomeGeneralHotkeyLabel");
  hotkeyLabel->setCaption(WideFromUtf8("STOBE Settings Key").c_str());
  hotkeyLabel->setTextAlign(MyGUI::Align::Left);

  g_welcomeGeneralHotkeyCombo = client->createWidgetReal<MyGUI::ComboBox>(
      "Kenshi_ComboBox", 0.52f, 0.34f, 0.43f, 0.07f,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_WelcomeGeneralHotkeyCombo");
  g_welcomeGeneralHotkeyCombo->setComboModeDrop(true);
  g_welcomeGeneralHotkeyCombo->eventComboAccept +=
      MyGUI::newDelegate(OnWelcomeGeneralHotkeyChanged);
  g_welcomeGeneralHotkeyCombo->eventComboChangePosition +=
      MyGUI::newDelegate(OnWelcomeGeneralHotkeyChanged);
  PopulateWelcomeGeneralHotkeyCombo();

  const bool distroConnected = IsDwemerDistroConnected();
  std::string distroStatus =
      std::string("DwemerDistro [") +
      (distroConnected ? "Connected" : "Disconnected") + "]";
  MyGUI::TextBox *distro = client->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", 0.05f, 0.43f, 0.9f, 0.06f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_WelcomeDistroStatus");
  distro->setCaption(WideFromUtf8(distroStatus).c_str());
  distro->setTextAlign(MyGUI::Align::Center);
  distro->setTextColour(distroConnected ? MyGUI::Colour(0.30f, 0.90f, 0.35f)
                                        : MyGUI::Colour(0.95f, 0.30f, 0.30f));

  g_welcomeCheckbox = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.08f, 0.51f, 0.84f, 0.10f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_WelcomeToggle");
  g_welcomeCheckbox->setCaption(WideFromUtf8(g_enableWelcome
                                               ? T("MOTD On: [ON]")
                                               : T("MOTD On: [OFF]"))
                                    .c_str());
  g_welcomeCheckbox->eventMouseButtonClick +=
      MyGUI::newDelegate(OnWelcomeToggleClick);

  MyGUI::Button *serverHomeBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.08f, 0.63f, 0.84f, 0.10f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_WelcomeServerHomeBtn");
  serverHomeBtn->setCaption(WideFromUtf8("Open server page").c_str());
  serverHomeBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnWelcomeOpenServerHomeClick);

  MyGUI::Button *closeBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.30f, 0.76f, 0.40f, 0.12f,
      MyGUI::Align::Bottom | MyGUI::Align::HCenter, "Stobe_WelcomeCloseBtn");
  closeBtn->setCaption(WideFromUtf8("Close").c_str());
  closeBtn->eventMouseButtonClick += MyGUI::newDelegate(OnWelcomeCloseClick);

  Log("UI: welcome window initialized.");
}

} // namespace UI
} // namespace Stobe

