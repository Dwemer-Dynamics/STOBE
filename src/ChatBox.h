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
// requestModeOverride, when set, is the request mode sent instead of the one
// resolved from the selected mode and the autochat toggle.
void SubmitChatTextForCurrentContext(const std::string &submittedText,
                                     bool fromVoice = false,
                                     const std::string &requestModeOverride = "");
void SubmitVoiceChatText(const std::string &submittedText,
                         const std::string &speakerName,
                         const std::string &speakerSerial,
                         const std::string &targetName,
                         const std::string &targetSerial,
                         const std::string &mode,
                         const std::string &requestModeOverride = "");
void OnChatCancelClick(MyGUI::Widget *sender);
void OnChatModeChange(MyGUI::ComboBox *sender, size_t index);
void OnChatProfileModelChange(MyGUI::ComboBox *sender, size_t index);
void OnChatTargetChange(MyGUI::ComboBox *sender, size_t index);
void OnChatActionChange(MyGUI::ComboBox *sender, size_t index);
void OnAutoChatToggleClick(MyGUI::Widget *sender);
bool IsAiRequestActive();
// Sends an addon action outcome (stobe.addon_followup.v1) on the native stream
// path. Only lines of the captured actor/serial are applied (actions only if
// the server opted in, never with an aid); no rechat. The stream slot is
// reserved before return. False when the worker could not start.
bool StartAddonFollowupStream(const std::wstring &endpoint,
                              const std::string &actorName,
                              unsigned int actorSerial,
                              const std::string &peopleJson);
// Game thread. True when the loaded character is one Stobe's normal chat would
// offer as a target (alive, conscious, in talk range and area of the player
// speaker); distanceOut is the chat interaction distance.
bool IsAutoAgentCandidate(GameWorld *world, Character *candidate,
                          float &distanceOut);
// Game thread. True when other is alive, conscious, not down and in the
// conversation area and bored-event search range of anchor.
bool IsInConversationReach(Character *anchor, Character *other);
// Counts chat requests whose stream worker started; never decreases.
LONG ChatRequestStartCount();
bool IsDirectorSceneActive();
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
                       const std::string &preferredListenerSerial = "",
                       const std::string &direction = "",
                       bool exactActors = false,
                       bool reserveStreamSlot = false);
bool TriggerNarratorWelcomeOnLoad(GameWorld *world,
                                  Character *preferredSpeaker = nullptr,
                                  LONG generationOverride = 0);
void OnChatWindowButtonPressed(MyGUI::Window *sender, const std::string &name);
void UpdateNpcContextRenameAction(PlayerInterface *playerInterface);
void ResetNpcContextRenameAction(bool destroyWidget = true);

} // namespace UI
} // namespace Stobe

