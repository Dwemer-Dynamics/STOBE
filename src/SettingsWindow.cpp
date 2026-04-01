#include "SettingsWindow.h"
#include "AudioPlayback.h"
#include "ChatBox.h"
#include "Comm.h"
#include "Globals.h"
#include "Utils.h"

#include <shellapi.h>
#include <windows.h>

#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_ComboBox.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Window.h>

#include <algorithm>
#include <cstdlib>

namespace Stobe {
namespace UI {

MyGUI::Window *g_settingsWindow = nullptr;

namespace {
MyGUI::ComboBox *g_hotkeyCombo = nullptr;
MyGUI::ComboBox *g_pluginChatModeCombo = nullptr;
MyGUI::ComboBox *g_speakerModeCombo = nullptr;
MyGUI::EditBox *g_talkRadiusEdit = nullptr;
MyGUI::EditBox *g_shoutRadiusEdit = nullptr;
MyGUI::EditBox *g_ttsVolumeEdit = nullptr;
MyGUI::EditBox *g_boredRangeEdit = nullptr;
MyGUI::EditBox *g_boredIntervalEdit = nullptr;
MyGUI::EditBox *g_dynamicProfileIntervalEdit = nullptr;
MyGUI::Button *g_autoChatToggle = nullptr;
MyGUI::Button *g_boredEventsToggle = nullptr;
MyGUI::Button *g_animalTalksToggle = nullptr;
MyGUI::Button *g_motdToggle = nullptr;
MyGUI::Button *g_ttsToggle = nullptr;
MyGUI::Button *g_speedDialogueToggle = nullptr;

bool g_pendingAutoChat = false;
bool g_pendingBoredEvents = true;
bool g_pendingAnimalTalks = false;
bool g_pendingNearestSpeaker = true;
bool g_pendingMOTD = true;
bool g_pendingTtsEnabled = true;
bool g_pendingSpeedDialogue = true;

int ClampInt(int value, int minValue, int maxValue) {
  if (value < minValue)
    return minValue;
  if (value > maxValue)
    return maxValue;
  return value;
}

int ParseIntOrDefault(const std::string &value, int fallback) {
  const char *raw = value.c_str();
  if (!raw || *raw == '\0')
    return fallback;
  char *end = nullptr;
  long parsed = strtol(raw, &end, 10);
  if (end == raw) {
    return fallback;
  }
  return static_cast<int>(parsed);
}

std::string NormalizeChatModeValue(const std::string &modeRaw) {
  std::string mode = modeRaw;
  std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
  if (mode == "whisper")
    return "whisper";
  if (mode == "shout")
    return "shout";
  if (mode == "cheat")
    return "cheat";
  if (mode == "narrator")
    return "narrator";
  return "chat";
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

void SetToggleCaption(MyGUI::Button *button, const std::string &label,
                      bool enabled) {
  if (!button)
    return;
  button->setCaption(
      WideFromUtf8(label + ": " + (enabled ? "[ON]" : "[OFF]")).c_str());
}

void PopulateSpeakerModeCombo() {
  if (!g_speakerModeCombo)
    return;
  g_speakerModeCombo->removeAllItems();
  g_speakerModeCombo->addItem(WideFromUtf8("Nearest to Target").c_str());
  g_speakerModeCombo->addItem(WideFromUtf8("First Squad Member").c_str());
  g_speakerModeCombo->setIndexSelected(g_pendingNearestSpeaker ? 0 : 1);
}

void PopulateHotkeyCombo() {
  if (!g_hotkeyCombo)
    return;
  g_hotkeyCombo->removeAllItems();
  g_hotkeyCombo->addItem(WideFromUtf8("/").c_str());
  g_hotkeyCombo->addItem(WideFromUtf8("-").c_str());
  g_hotkeyCombo->addItem(WideFromUtf8(".").c_str());
  g_hotkeyCombo->addItem(WideFromUtf8("[").c_str());
  g_hotkeyCombo->addItem(WideFromUtf8("]").c_str());
  g_hotkeyCombo->addItem(WideFromUtf8("O").c_str());
  g_hotkeyCombo->addItem(WideFromUtf8("P").c_str());

  std::string currentHotkey = g_chatHotkeyStr;
  if (currentHotkey == "o")
    currentHotkey = "O";
  else if (currentHotkey == "p")
    currentHotkey = "P";

  size_t selected = 0;
  for (size_t i = 0; i < g_hotkeyCombo->getItemCount(); ++i) {
    if (g_hotkeyCombo->getItemNameAt(i) == currentHotkey) {
      selected = i;
      break;
    }
  }
  g_hotkeyCombo->setIndexSelected(selected);
}

void PopulateChatModeCombo() {
  if (!g_pluginChatModeCombo)
    return;
  g_pluginChatModeCombo->removeAllItems();
  g_pluginChatModeCombo->addItem(WideFromUtf8("chat").c_str());
  g_pluginChatModeCombo->addItem(WideFromUtf8("whisper").c_str());
  g_pluginChatModeCombo->addItem(WideFromUtf8("shout").c_str());
  g_pluginChatModeCombo->addItem(WideFromUtf8("cheat").c_str());
  g_pluginChatModeCombo->addItem(WideFromUtf8("narrator").c_str());

  std::string currentMode = NormalizeChatModeValue(g_chatMode);
  size_t selected = 0;
  if (currentMode == "whisper")
    selected = 1;
  else if (currentMode == "shout")
    selected = 2;
  else if (currentMode == "cheat")
    selected = 3;
  else if (currentMode == "narrator")
    selected = 4;
  g_pluginChatModeCombo->setIndexSelected(selected);
}

void OnHotkeyComboChanged(MyGUI::ComboBox *sender, size_t index) {
  if (!sender || index == MyGUI::ITEM_NONE)
    return;
  std::string selected = sender->getItemNameAt(index);
  SetHotkeyFromString(selected);
}

void OnPluginModeComboChanged(MyGUI::ComboBox *sender, size_t index) {
  if (!sender || index == MyGUI::ITEM_NONE)
    return;
  g_chatMode = NormalizeChatModeValue(sender->getItemNameAt(index));
  if (g_chatMode == "whisper")
    g_lastChatModeIndex = 1;
  else if (g_chatMode == "shout")
    g_lastChatModeIndex = 2;
  else if (g_chatMode == "cheat")
    g_lastChatModeIndex = 3;
  else if (g_chatMode == "narrator")
    g_lastChatModeIndex = 4;
  else
    g_lastChatModeIndex = 0;
}

void OnSpeakerModeComboChanged(MyGUI::ComboBox *sender, size_t index) {
  if (!sender || index == MyGUI::ITEM_NONE)
    return;
  g_pendingNearestSpeaker = (index == 0);
}

void LoadPendingFromRuntime() {
  g_pendingAutoChat = g_autoChatEnabled;
  g_pendingBoredEvents = g_enableBoredEvents;
  g_pendingAnimalTalks = g_enableAnimalTalks;
  g_pendingNearestSpeaker = g_useNearestPlayerSpeaker;
  g_pendingMOTD = g_enableWelcome;
  g_pendingTtsEnabled = g_ttsEnabled;
  g_pendingSpeedDialogue = g_speedDialogue;
}

void RefreshPluginSettingsUI() {
  if (g_talkRadiusEdit) {
    g_talkRadiusEdit->setCaption(WideFromUtf8(ToString((int)g_proximityRadius))
                                     .c_str());
  }
  if (g_shoutRadiusEdit) {
    g_shoutRadiusEdit->setCaption(
        WideFromUtf8(ToString((int)g_shoutRadius)).c_str());
  }
  if (g_boredRangeEdit) {
    g_boredRangeEdit->setCaption(
        WideFromUtf8(ToString((int)g_boredEventRange)).c_str());
  }
  if (g_boredIntervalEdit) {
    g_boredIntervalEdit->setCaption(
        WideFromUtf8(ToString(g_boredEventIntervalSeconds)).c_str());
  }
  if (g_dynamicProfileIntervalEdit) {
    g_dynamicProfileIntervalEdit->setCaption(
        WideFromUtf8(ToString(g_dynamicProfileIntervalMinutes)).c_str());
  }
  if (g_ttsVolumeEdit) {
    g_ttsVolumeEdit->setCaption(
        WideFromUtf8(ToString(g_ttsVolumePercent)).c_str());
  }

  PopulateHotkeyCombo();
  PopulateChatModeCombo();
  PopulateSpeakerModeCombo();
  SetToggleCaption(g_autoChatToggle, "Auto Chat", g_pendingAutoChat);
  SetToggleCaption(g_boredEventsToggle, "Bored Events", g_pendingBoredEvents);
  SetToggleCaption(g_animalTalksToggle, "Animal Talks", g_pendingAnimalTalks);
  SetToggleCaption(g_motdToggle, "MOTD", g_pendingMOTD);
  SetToggleCaption(g_ttsToggle, "TTS", g_pendingTtsEnabled);
  SetToggleCaption(g_speedDialogueToggle, "Speed Dialogue",
                   g_pendingSpeedDialogue);
}

void OnSettingsSaveClick(MyGUI::Widget *sender) {
  if (!g_settingsWindow)
    return;

  if (g_hotkeyCombo && g_hotkeyCombo->getIndexSelected() != MyGUI::ITEM_NONE) {
    SetHotkeyFromString(
        g_hotkeyCombo->getItemNameAt(g_hotkeyCombo->getIndexSelected()));
  }

  if (g_pluginChatModeCombo &&
      g_pluginChatModeCombo->getIndexSelected() != MyGUI::ITEM_NONE) {
    g_chatMode = NormalizeChatModeValue(
        g_pluginChatModeCombo->getItemNameAt(
            g_pluginChatModeCombo->getIndexSelected()));
  }

  if (g_chatMode == "whisper")
    g_lastChatModeIndex = 1;
  else if (g_chatMode == "shout")
    g_lastChatModeIndex = 2;
  else if (g_chatMode == "cheat")
    g_lastChatModeIndex = 3;
  else if (g_chatMode == "narrator")
    g_lastChatModeIndex = 4;
  else
    g_lastChatModeIndex = 0;

  g_autoChatEnabled = g_pendingAutoChat;
  g_enableBoredEvents = g_pendingBoredEvents;
  g_enableAnimalTalks = g_pendingAnimalTalks;
  g_useNearestPlayerSpeaker = g_pendingNearestSpeaker;
  g_enableWelcome = g_pendingMOTD;
  bool previousTtsEnabled = g_ttsEnabled;
  g_ttsEnabled = g_pendingTtsEnabled;
  g_speedDialogue = g_pendingSpeedDialogue;

  int talkRadius = ParseIntOrDefault(
      g_talkRadiusEdit ? g_talkRadiusEdit->getCaption() : "", (int)g_proximityRadius);
  int shoutRadius = ParseIntOrDefault(
      g_shoutRadiusEdit ? g_shoutRadiusEdit->getCaption() : "", (int)g_shoutRadius);
  int boredRange = ParseIntOrDefault(
      g_boredRangeEdit ? g_boredRangeEdit->getCaption() : "", (int)g_boredEventRange);
  int boredInterval = ParseIntOrDefault(
      g_boredIntervalEdit ? g_boredIntervalEdit->getCaption() : "",
      g_boredEventIntervalSeconds);
  int ttsVolume = ParseIntOrDefault(
      g_ttsVolumeEdit ? g_ttsVolumeEdit->getCaption() : "",
      g_ttsVolumePercent);
  int dynamicProfileInterval = ParseIntOrDefault(
      g_dynamicProfileIntervalEdit ? g_dynamicProfileIntervalEdit->getCaption()
                                   : "",
      g_dynamicProfileIntervalMinutes);

  g_proximityRadius = (float)ClampInt(talkRadius, 1, 5000);
  g_shoutRadius = (float)ClampInt(shoutRadius, 1, 5000);
  g_boredEventRange = (float)ClampInt(boredRange, 1, 5000);
  g_boredEventIntervalSeconds = ClampInt(boredInterval, 5, 3600);
  g_ttsVolumePercent = ClampInt(ttsVolume, 0, 100);
  g_dynamicProfileIntervalMinutes = ClampInt(dynamicProfileInterval, 1, 720);

  SaveStobeRuntimeConfig();
  if (previousTtsEnabled && !g_ttsEnabled) {
    InterruptTtsPlayback();
    Log("CONFIG: TTS disabled from settings; active playback interrupted.");
  }
  RefreshChatModeControls();
  RefreshPluginSettingsUI();

  if (sender && sender->castType<MyGUI::Button>(false)) {
    sender->castType<MyGUI::Button>()->setCaption(WideFromUtf8("Saved").c_str());
  }
}

void OnPluginAutoChatToggleClick(MyGUI::Widget *sender) {
  g_pendingAutoChat = !g_pendingAutoChat;
  SetToggleCaption(g_autoChatToggle, "Auto Chat", g_pendingAutoChat);
}

void OnPluginBoredEventsToggleClick(MyGUI::Widget *sender) {
  g_pendingBoredEvents = !g_pendingBoredEvents;
  SetToggleCaption(g_boredEventsToggle, "Bored Events", g_pendingBoredEvents);
}

void OnPluginMOTDToggleClick(MyGUI::Widget *sender) {
  g_pendingMOTD = !g_pendingMOTD;
  SetToggleCaption(g_motdToggle, "MOTD", g_pendingMOTD);
}

void OnPluginAnimalTalksToggleClick(MyGUI::Widget *sender) {
  g_pendingAnimalTalks = !g_pendingAnimalTalks;
  g_enableAnimalTalks = g_pendingAnimalTalks;
  SaveStobeRuntimeConfig();
  SetToggleCaption(g_animalTalksToggle, "Animal Talks", g_pendingAnimalTalks);
  Log("CONFIG: AnimalTalks toggled to " +
      std::string(g_enableAnimalTalks ? "ON" : "OFF") + " (saved)");
}

void OnPluginTtsToggleClick(MyGUI::Widget *sender) {
  g_pendingTtsEnabled = !g_pendingTtsEnabled;
  SetToggleCaption(g_ttsToggle, "TTS", g_pendingTtsEnabled);
}

void OnPluginSpeedDialogueToggleClick(MyGUI::Widget *sender) {
  g_pendingSpeedDialogue = !g_pendingSpeedDialogue;
  SetToggleCaption(g_speedDialogueToggle, "Speed Dialogue",
                   g_pendingSpeedDialogue);
}

void OnSettingsOpenConfigClick(MyGUI::Widget *sender) {
  char path[MAX_PATH];
  GetModuleFileNameA(NULL, path, MAX_PATH);
  std::string dir = path;
  size_t lastBackslash = dir.find_last_of("\\/");
  if (lastBackslash != std::string::npos) {
    dir = dir.substr(0, lastBackslash);
  }
  std::string configPath = dir + "\\mods\\Stobe";
  ShellExecuteA(NULL, "open", configPath.c_str(), NULL, NULL, SW_SHOWDEFAULT);
}

void OnSettingsOpenServerFolderClick(MyGUI::Widget *sender) {
  const char *wslPath =
      "\\\\wsl.localhost\\DwemerAI4Skyrim3\\var\\www\\html\\StobeServer";
  HINSTANCE result =
      ShellExecuteA(NULL, "open", wslPath, NULL, NULL, SW_SHOWDEFAULT);
  if ((INT_PTR)result <= 32) {
    Log("UI_WARN: failed to open StobeServer WSL folder.");
  }
}

void OnSettingsOpenServerPageClick(MyGUI::Widget *sender) {
  const std::string url = GetStobeServerHomeUrl();
  HINSTANCE result =
      ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
  if ((INT_PTR)result <= 32) {
    Log("UI_WARN: failed to open Stobe server page: " + url);
  }
}

MyGUI::TextBox *CreateLabel(MyGUI::Widget *parent, float x, float y, float w,
                            float h, const std::string &text,
                            const std::string &name) {
  MyGUI::TextBox *label = parent->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", x, y, w, h,
      MyGUI::Align::Top | MyGUI::Align::Left, name);
  label->setCaption(WideFromUtf8(text).c_str());
  return label;
}
} // namespace

void CloseSettingsUI() {
  if (g_settingsWindow) {
    if (!TryDestroyWidgetSafe(g_settingsWindow)) {
      Log("UI_WARN: CloseSettingsUI destroyWidget failed; clearing stale pointer.");
    }
    g_settingsWindow = nullptr;
  }

  g_hotkeyCombo = nullptr;
  g_pluginChatModeCombo = nullptr;
  g_speakerModeCombo = nullptr;
  g_talkRadiusEdit = nullptr;
  g_shoutRadiusEdit = nullptr;
  g_ttsVolumeEdit = nullptr;
  g_boredRangeEdit = nullptr;
  g_boredIntervalEdit = nullptr;
  g_dynamicProfileIntervalEdit = nullptr;
  g_autoChatToggle = nullptr;
  g_boredEventsToggle = nullptr;
  g_animalTalksToggle = nullptr;
  g_motdToggle = nullptr;
  g_ttsToggle = nullptr;
  g_speedDialogueToggle = nullptr;
}

void OnSettingsWindowButtonPressed(MyGUI::Window *sender,
                                   const std::string &name) {
  if (name == "close")
    CloseSettingsUI();
}

void CreateSettingsUI() {
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) {
    Log("UI_WARN: CreateSettingsUI requested but MyGUI is unavailable.");
    return;
  }
  if (g_settingsWindow)
    CloseSettingsUI();

  LoadPendingFromRuntime();

  g_settingsWindow = gui->createWidgetReal<MyGUI::Window>(
      "Kenshi_WindowCX", 0.17f, 0.08f, 0.66f, 0.70f, MyGUI::Align::Center,
      "Overlapped", "Stobe_PluginSettingsWindow");
  g_settingsWindow->setCaption(WideFromUtf8("Plugin Settings").c_str());
  g_settingsWindow->eventWindowButtonPressed +=
      MyGUI::newDelegate(OnSettingsWindowButtonPressed);

  MyGUI::Widget *client = g_settingsWindow->getClientWidget();

  const float labelX = 0.05f;
  const float fieldX = 0.52f;
  const float labelW = 0.45f;
  const float fieldW = 0.43f;
  const float rowH = 0.055f;
  const float rowGap = 0.006f;
  const float rangeHintH = 0.04f;
  const float rangeHintGap = 0.045f;
  const float sectionGap = 0.015f;
  const float toggleH = 0.07f;
  const float actionBtnH = 0.08f;
  const float toggleRowGap = 0.082f;
  float y = 0.03f;

  CreateLabel(client, labelX, y, labelW, rowH, "Chat Hotkey",
              "Stobe_Plugin_HotkeyLabel");
  g_hotkeyCombo = client->createWidgetReal<MyGUI::ComboBox>(
      "Kenshi_ComboBox", fieldX, y, fieldW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_HotkeyCombo");
  g_hotkeyCombo->setComboModeDrop(true);
  g_hotkeyCombo->eventComboAccept += MyGUI::newDelegate(OnHotkeyComboChanged);
  g_hotkeyCombo->eventComboChangePosition +=
      MyGUI::newDelegate(OnHotkeyComboChanged);
  y += rowH + rowGap;

  CreateLabel(client, labelX, y, labelW, rowH, "Default Chat Mode",
              "Stobe_Plugin_ModeLabel");
  g_pluginChatModeCombo = client->createWidgetReal<MyGUI::ComboBox>(
      "Kenshi_ComboBox", fieldX, y, fieldW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_ModeCombo");
  g_pluginChatModeCombo->setComboModeDrop(true);
  g_pluginChatModeCombo->eventComboAccept +=
      MyGUI::newDelegate(OnPluginModeComboChanged);
  g_pluginChatModeCombo->eventComboChangePosition +=
      MyGUI::newDelegate(OnPluginModeComboChanged);
  y += rowH + rowGap;

  MyGUI::TextBox *rangeHint = CreateLabel(
      client, labelX, y, 0.90f, rangeHintH,
      "Range units: ~10 unit = ~1 meter. Whisper is fixed at 20 units.",
      "Stobe_Plugin_RangeUnitsHint");
  rangeHint->setTextColour(MyGUI::Colour(0.95f, 0.85f, 0.35f));
  y += rangeHintGap;

  CreateLabel(client, labelX, y, labelW, rowH, "Talk Radius (units)",
              "Stobe_Plugin_TalkLabel");
  g_talkRadiusEdit = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", fieldX, y, fieldW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_TalkEdit");
  y += rowH + rowGap;

  CreateLabel(client, labelX, y, labelW, rowH, "Shout Radius (units)",
              "Stobe_Plugin_ShoutLabel");
  g_shoutRadiusEdit = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", fieldX, y, fieldW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_ShoutEdit");
  y += rowH + rowGap;

  CreateLabel(client, labelX, y, labelW, rowH, "TTS Volume (0-100)",
              "Stobe_Plugin_TtsVolumeLabel");
  g_ttsVolumeEdit = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", fieldX, y, fieldW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_TtsVolumeEdit");
  y += rowH + rowGap;

  CreateLabel(client, labelX, y, labelW, rowH, "Bored Event Range",
              "Stobe_Plugin_BoredRangeLabel");
  g_boredRangeEdit = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", fieldX, y, fieldW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_BoredRangeEdit");
  y += rowH + rowGap;

  CreateLabel(client, labelX, y, labelW, rowH, "Bored Event Interval (sec)",
              "Stobe_Plugin_BoredIntervalLabel");
  g_boredIntervalEdit = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", fieldX, y, fieldW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left,
      "Stobe_Plugin_BoredIntervalEdit");
  y += rowH + rowGap;

  CreateLabel(client, labelX, y, labelW, rowH, "Dynamic Profile Interval (min)",
              "Stobe_Plugin_DynProfileIntervalLabel");
  g_dynamicProfileIntervalEdit = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", fieldX, y, fieldW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left,
      "Stobe_Plugin_DynProfileIntervalEdit");
  y += rowH + sectionGap;

  g_autoChatToggle = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, y, 0.28f, toggleH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_AutoChatToggle");
  g_autoChatToggle->eventMouseButtonClick +=
      MyGUI::newDelegate(OnPluginAutoChatToggleClick);

  g_boredEventsToggle = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.36f, y, 0.28f, toggleH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_BoredEventsToggle");
  g_boredEventsToggle->eventMouseButtonClick +=
      MyGUI::newDelegate(OnPluginBoredEventsToggleClick);

  g_motdToggle = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.67f, y, 0.28f, toggleH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_MOTDToggle");
  g_motdToggle->eventMouseButtonClick +=
      MyGUI::newDelegate(OnPluginMOTDToggleClick);
  y += toggleRowGap;

  g_animalTalksToggle = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, y, 0.28f, toggleH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_AnimalTalksToggle");
  g_animalTalksToggle->eventMouseButtonClick +=
      MyGUI::newDelegate(OnPluginAnimalTalksToggleClick);

  g_ttsToggle = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.36f, y, 0.28f, toggleH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_TTSToggle");
  g_ttsToggle->eventMouseButtonClick +=
      MyGUI::newDelegate(OnPluginTtsToggleClick);

  g_speedDialogueToggle = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.67f, y, 0.28f, toggleH,
      MyGUI::Align::Top | MyGUI::Align::Left,
      "Stobe_Plugin_SpeedDialogueToggle");
  g_speedDialogueToggle->eventMouseButtonClick +=
      MyGUI::newDelegate(OnPluginSpeedDialogueToggleClick);
  y += toggleRowGap;

  CreateLabel(client, labelX, y, labelW, rowH, "Speaker Mode",
              "Stobe_Plugin_SpeakerModeLabel");
  g_speakerModeCombo = client->createWidgetReal<MyGUI::ComboBox>(
      "Kenshi_ComboBox", fieldX, y, fieldW, rowH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_SpeakerModeCombo");
  g_speakerModeCombo->setComboModeDrop(true);
  g_speakerModeCombo->eventComboAccept +=
      MyGUI::newDelegate(OnSpeakerModeComboChanged);
  g_speakerModeCombo->eventComboChangePosition +=
      MyGUI::newDelegate(OnSpeakerModeComboChanged);
  y += rowH + sectionGap;

  MyGUI::Button *saveBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.05f, y, 0.21f, actionBtnH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_SaveBtn");
  saveBtn->setCaption(WideFromUtf8("Save Settings").c_str());
  saveBtn->eventMouseButtonClick += MyGUI::newDelegate(OnSettingsSaveClick);

  MyGUI::Button *openConfigBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.28f, y, 0.21f, actionBtnH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_OpenConfigBtn");
  openConfigBtn->setCaption(WideFromUtf8("Open Mod Folder").c_str());
  openConfigBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnSettingsOpenConfigClick);

  MyGUI::Button *openServerBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.51f, y, 0.21f, actionBtnH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_OpenServerBtn");
  openServerBtn->setCaption(WideFromUtf8("Open Server Folder").c_str());
  openServerBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnSettingsOpenServerFolderClick);

  MyGUI::Button *openServerPageBtn = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.74f, y, 0.21f, actionBtnH,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_Plugin_OpenServerPageBtn");
  openServerPageBtn->setCaption(WideFromUtf8("Open server page").c_str());
  openServerPageBtn->eventMouseButtonClick +=
      MyGUI::newDelegate(OnSettingsOpenServerPageClick);

  RefreshPluginSettingsUI();
}

void PopulateSettingsUI(const std::string &json) {
  // Kept for compatibility with existing command routing; this window is now
  // local plugin INI settings only.
  if (g_settingsWindow) {
    RefreshPluginSettingsUI();
  }
}

} // namespace UI
} // namespace Stobe
