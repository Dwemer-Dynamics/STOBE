#pragma once
#include "ChatUIGlobals.h"

namespace Stobe {
namespace UI {

extern MyGUI::Window *g_settingsWindow;

void CreateSettingsUI();
void CloseSettingsUI();
void PopulateSettingsUI(const std::string &json);

void OnSettingsWindowButtonPressed(MyGUI::Window *sender,
                                   const std::string &name);

} // namespace UI
} // namespace Stobe
