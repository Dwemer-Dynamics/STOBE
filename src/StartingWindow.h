#pragma once
#include "ChatUIGlobals.h"

namespace Stobe {
namespace UI {

extern MyGUI::Window *g_startingWindow;

void CreateStartingUI();
void CloseStartingUI();

void OnStartingAiNpcsClick(MyGUI::Widget *sender);
void OnStartingAiDiariesClick(MyGUI::Widget *sender);
void OnStartingPluginSettingsClick(MyGUI::Widget *sender);
void OnStartingWelcomeClick(MyGUI::Widget *sender);
void OnStartingWindowButtonPressed(MyGUI::Window *sender,
                                   const std::string &name);

} // namespace UI
} // namespace Stobe

