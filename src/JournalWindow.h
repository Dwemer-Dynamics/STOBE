#pragma once
#include "ChatUIGlobals.h"

class GameWorld;

namespace Stobe {
namespace UI {

extern MyGUI::Window *g_recentHistoryWindow;
extern MyGUI::Window *g_statusHudWindow;

void CreateRecentHistoryUI();
void CloseRecentHistoryUI();
void SetRecentHistoryText(const std::string &data);

void SetStatusHudEnabled(bool enabled);
void UpdateStatusHud(GameWorld *world);
void CloseStatusHud();

} // namespace UI
} // namespace Stobe
