#pragma once
#include "ChatUIGlobals.h"

class GameWorld;
class Character;

namespace Stobe {
namespace UI {

extern MyGUI::Window *g_chatWindow;
extern MyGUI::EditBox *g_chatInput;
extern MyGUI::ComboBox *g_chatModeCombo;
extern MyGUI::ComboBox *g_chatTargetCombo;
extern MyGUI::ComboBox *g_chatActionCombo;
extern MyGUI::EditBox *g_chatActionArgInput;
extern MyGUI::Button *g_chatAutoChatToggle;
extern MyGUI::TextBox *g_chatLabel;
extern std::string g_chatTargetHandleStr;
extern std::string g_chatTargetNameStr;
extern std::string g_chatPlayerNameStr;
extern size_t g_lastChatModeIndex;

void CreateChatUI(const std::string &npcName, const std::string &playerName,
                  const std::string &handleStr);
void CloseChatUI();
void SendChatToStobeServer(GameWorld *world, Character *sel,
                      const std::string &npcName, const std::string &playerName,
                      const std::string &text, const std::string &mode,
                      const std::string &npcsJson,
                      const std::string &nearbyFullJson);

DWORD WINAPI DialogResponseWorker(LPVOID lpParam);
void OnChatInputChange(MyGUI::EditBox *sender);
void OnChatInputAccept(MyGUI::EditBox *sender);
void OnChatSendClick(MyGUI::Widget *sender);
void OnChatCancelClick(MyGUI::Widget *sender);
void OnChatModeChange(MyGUI::ComboBox *sender, size_t index);
void OnChatTargetChange(MyGUI::ComboBox *sender, size_t index);
void OnChatActionChange(MyGUI::ComboBox *sender, size_t index);
void OnAutoChatToggleClick(MyGUI::Widget *sender);
void RefreshChatModeControls();
void OnBoredEventClick(MyGUI::Widget *sender);
void OnWriteDiaryClick(MyGUI::Widget *sender);
bool TriggerBoredEvent(GameWorld *world, bool forceDirectorMode,
                       const std::string &preferredSpeakerName = "",
                       const std::string &preferredSpeakerSerial = "",
                       LONG generationOverride = 0,
                       const std::string &preferredListenerName = "",
                       const std::string &preferredListenerSerial = "");
bool TriggerNarratorWelcomeOnLoad(GameWorld *world,
                                  Character *preferredSpeaker = nullptr,
                                  LONG generationOverride = 0);
void OnChatWindowButtonPressed(MyGUI::Window *sender, const std::string &name);

} // namespace UI
} // namespace Stobe

