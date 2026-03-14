#pragma once
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_ComboBox.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_ListBox.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Window.h>
#include <string>
#include <vector>
#include <windows.h>

namespace Stobe {
namespace UI {

struct ChatTask {
  std::string json;
  std::string npcName;
  std::string handleStr;
};

struct AiNpcInfoTask {
  std::string npcName;
  std::string json;
};

struct AiDiaryTask {
  std::string json;
};

// Shared across modules (e.g., Welcome and Settings share these)
extern std::string g_allModelsJson;
extern bool g_welcomeShown;
extern bool g_enableWelcome;
extern MyGUI::Button *g_welcomeCheckbox;

} // namespace UI
} // namespace Stobe

