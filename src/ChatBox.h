#pragma once
#include "ChatUIGlobals.h"

class GameWorld;
class Character;
class PlayerInterface;

namespace Stobe {
namespace UI {

extern MyGUI::Window *g_chatWindow;
extern MyGUI::EditBox *g_chatInput;
extern MyGUI::ComboBox *g_chatModeCombo;
extern MyGUI::ComboBox *g_chatTargetCombo;
extern MyGUI::ComboBox *g_chatActionCombo;
extern MyGUI::ComboBox *g_chatProfileModelCombo;
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
void SubmitChatTextForCurrentContext(const std::string &submittedText,
                                     bool fromVoice = false);
void SubmitVoiceChatText(const std::string &submittedText,
                         const std::string &speakerName,
                         const std::string &speakerSerial,
                         const std::string &targetName,
                         const std::string &targetSerial,
                         const std::string &mode);
void OnChatCancelClick(MyGUI::Widget *sender);
void OnChatModeChange(MyGUI::ComboBox *sender, size_t index);
void OnChatProfileModelChange(MyGUI::ComboBox *sender, size_t index);
void OnChatTargetChange(MyGUI::ComboBox *sender, size_t index);
void OnChatActionChange(MyGUI::ComboBox *sender, size_t index);
void OnAutoChatToggleClick(MyGUI::Widget *sender);
bool IsAiRequestActive();
void RefreshChatModeControls();
void OnBoredEventClick(MyGUI::Widget *sender);
void OnWriteDiaryClick(MyGUI::Widget *sender);
void OnWriteNarratorDiaryClick(MyGUI::Widget *sender);
int GetActiveProfileModelSlot();
std::string GetActiveProfileModelLabel();
void RequestProfileModelSlotRefresh(bool force = false);
void ApplyProfileModelSlotUpdate(const std::string &data);
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
void UpdateNpcContextRenameAction(PlayerInterface *playerInterface);
void ResetNpcContextRenameAction(bool destroyWidget = true);

} // namespace UI
} // namespace Stobe

