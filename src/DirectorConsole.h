#pragma once

#include <mygui/MyGUI_Window.h>
#include <string>

class Character;
class GameWorld;

namespace Stobe {
namespace UI {

extern MyGUI::Window *g_directorWindow;

void CreateDirectorConsole(GameWorld *world, Character *selectedCharacter);
void CloseDirectorConsole();
void UpdateDirectorConsole(GameWorld *world, Character *selectedCharacter);
void ResetDirectorConsole(const std::string &reason);

} // namespace UI
} // namespace Stobe
