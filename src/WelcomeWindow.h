#pragma once
#include "ChatUIGlobals.h"

namespace Stobe {
namespace UI {

extern MyGUI::Window *g_welcomeWindow;

void CreateWelcomeUI();
void CloseWelcomeUI();
void OnWelcomeDiscordClick(MyGUI::Widget *sender);
void OnWelcomeWindowButtonPressed(MyGUI::Window *sender,
                                  const std::string &name);
DWORD WINAPI WelcomeResponseThread(LPVOID lpParam);

} // namespace UI
} // namespace Stobe
