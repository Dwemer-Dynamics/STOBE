#include "AiNpcInfoWindow.h"
#include "Comm.h"
#include "Globals.h"
#include "Utils.h"
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_ListBox.h>
#include <mygui/MyGUI_Window.h>
#include <sstream>
#include <string>
#include <vector>

namespace Stobe {
namespace UI {

MyGUI::Window *g_aiNpcInfoWindow = nullptr;
MyGUI::ListBox *g_aiNpcInfoList = nullptr;
MyGUI::ListBox *g_aiNpcInfoText = nullptr;
MyGUI::Button *g_aiNpcInfoCloseButton = nullptr;
std::vector<std::string> g_aiNpcInfoStorageIds;
MyGUI::Window *g_aiDiaryWindow = nullptr;
MyGUI::ListBox *g_aiDiaryList = nullptr;
MyGUI::ListBox *g_aiDiaryText = nullptr;
MyGUI::Button *g_aiDiaryCloseButton = nullptr;
std::vector<std::string> g_aiDiaryKeys;

namespace {
std::string SanitizeAiNpcInfoText(std::string input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    unsigned char ch = static_cast<unsigned char>(input[i]);
    if (ch == '\0') {
      continue;
    }
    if (ch < 32 && ch != '\n' && ch != '\r' && ch != '\t') {
      continue;
    }
    out.push_back(static_cast<char>(ch));
  }
  return out;
}

void TrimInline(std::string &s) {
  if (s.empty()) {
    return;
  }
  size_t first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    s.clear();
    return;
  }
  size_t last = s.find_last_not_of(" \t\r\n");
  s = s.substr(first, last - first + 1);
}

void ParsePipeEntryArray(const std::string &dataInput,
                         std::vector<std::string> &displayOut,
                         std::vector<std::string> &valueOut) {
  displayOut.clear();
  valueOut.clear();

  std::string data = dataInput;
  if (!data.empty() && data[0] == '[') {
    data = data.substr(1);
  }
  if (!data.empty() && data[data.length() - 1] == ']') {
    data = data.substr(0, data.length() - 1);
  }

  size_t cur = 0;
  while (cur < data.length()) {
    size_t next = data.find(",", cur);
    std::string entry = (next == std::string::npos) ? data.substr(cur)
                                                     : data.substr(cur, next - cur);
    TrimInline(entry);
    if (!entry.empty()) {
      if (entry[0] == '"') {
        entry = entry.substr(1);
      }
      if (!entry.empty() && entry[entry.length() - 1] == '"') {
        entry = entry.substr(0, entry.length() - 1);
      }

      size_t pipePos = entry.find("|");
      std::string display = entry;
      std::string value = entry;
      if (pipePos != std::string::npos) {
        display = entry.substr(0, pipePos);
        value = entry.substr(pipePos + 1);
      }
      TrimInline(display);
      TrimInline(value);
      if (!display.empty()) {
        displayOut.push_back(display);
        valueOut.push_back(value.empty() ? display : value);
      }
    }
    if (next == std::string::npos) {
      break;
    }
    cur = next + 1;
  }
}
} // namespace

void CloseAiNpcInfoUI() {
  if (g_aiNpcInfoWindow) {
    if (MyGUI::Gui::getInstancePtr()) {
      MyGUI::Gui::getInstancePtr()->destroyWidget(g_aiNpcInfoWindow);
    }
    g_aiNpcInfoWindow = nullptr;
    g_aiNpcInfoList = nullptr;
    g_aiNpcInfoText = nullptr;
    g_aiNpcInfoCloseButton = nullptr;
    g_aiNpcInfoStorageIds.clear();
  }
}

void PopulateAiNpcInfoUI(const std::string &dataInput) {
  if (!g_aiNpcInfoList) {
    return;
  }

  size_t selectedIndex = g_aiNpcInfoList->getIndexSelected();
  std::string selectedSid = "";
  if (selectedIndex != MyGUI::ITEM_NONE &&
      selectedIndex < g_aiNpcInfoStorageIds.size()) {
    selectedSid = g_aiNpcInfoStorageIds[selectedIndex];
  }

  g_aiNpcInfoList->removeAllItems();
  g_aiNpcInfoStorageIds.clear();

  std::vector<std::string> displays;
  std::vector<std::string> values;
  ParsePipeEntryArray(dataInput, displays, values);
  for (size_t i = 0; i < displays.size() && i < values.size(); ++i) {
    g_aiNpcInfoList->addItem(WideFromUtf8(displays[i]).c_str());
    g_aiNpcInfoStorageIds.push_back(values[i]);
  }

  if (!selectedSid.empty()) {
    for (size_t i = 0; i < g_aiNpcInfoStorageIds.size(); ++i) {
      if (g_aiNpcInfoStorageIds[i] == selectedSid) {
        g_aiNpcInfoList->setIndexSelected(i);
        break;
      }
    }
  }
}

void SetAiNpcInfoText(const std::string &data) {
  if (!g_aiNpcInfoText) {
    return;
  }
  std::string payload = SanitizeAiNpcInfoText(data);
  if (!payload.empty() && payload[0] == ' ') {
    payload = payload.substr(1);
  }
  g_aiNpcInfoText->removeAllItems();
  std::stringstream ss(payload);
  std::string line;
  bool any = false;
  while (std::getline(ss, line)) {
    g_aiNpcInfoText->addItem(WideFromUtf8(line).c_str());
    any = true;
  }
  if (!any) {
    g_aiNpcInfoText->addItem(WideFromUtf8(payload).c_str());
  }
}

void OnAiNpcInfoNPCSelect(MyGUI::ListBox *sender, size_t index) {
  if (index == MyGUI::ITEM_NONE || index >= g_aiNpcInfoStorageIds.size()) {
    return;
  }

  std::string displayName = sender->getItemNameAt(index);
  std::string sid = g_aiNpcInfoStorageIds[index];

  if (g_aiNpcInfoText) {
    g_aiNpcInfoText->removeAllItems();
    g_aiNpcInfoText->addItem(
        WideFromUtf8(T("Loading profile for ") + displayName + "...").c_str());
  }

  AiNpcInfoTask *task = new AiNpcInfoTask();
  task->npcName = displayName;
  task->json = "{\"action\":\"detail\",\"sid\":\"" + EscapeJSON(sid) + "\"}";
  CreateThread(NULL, 0, AiNpcInfoDetailThread, task, 0, NULL);
}

void OnAiNpcInfoWindowButtonPressed(MyGUI::Window *sender,
                                  const std::string &name) {
  if (name == "close") {
    CloseAiNpcInfoUI();
  }
}

DWORD WINAPI AiNpcInfoListThread(LPVOID lpParam) {
  std::string response =
      PostToStobeWithResponse(L"/ai_npcs/list", "{\"action\":\"list\"}");
  if (response.empty()) {
    return 0;
  }

  std::string names = JsonReadField(response, "names");
  if (names.empty()) {
    names = JsonReadField(response, "characters");
  }
  if (names.empty()) {
    return 0;
  }

  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.push_back("CMD: POPULATE_AINPCINFO: " + names);
  LeaveCriticalSection(&g_msgMutex);
  return 0;
}

DWORD WINAPI AiNpcInfoDetailThread(LPVOID lpParam) {
  AiNpcInfoTask *task = (AiNpcInfoTask *)lpParam;
  std::string response = PostToStobeWithResponse(L"/ai_npcs/detail", task->json);
  delete task;
  if (response.empty()) {
    return 0;
  }

  std::string content = JsonReadField(response, "text");
  if (content.empty()) {
    content = JsonReadField(response, "content");
  }
  content = SanitizeAiNpcInfoText(content);
  Log("AI_NPCS_DETAIL: response_len=" + ToString((unsigned int)response.length()) +
      " parsed_len=" + ToString((unsigned int)content.length()));
  if (content.empty()) {
    // Debug fallback: show raw payload so UI never appears blank/stuck.
    content = "Failed to parse AI NPC detail response.\n\nRaw response:\n" +
              SanitizeAiNpcInfoText(response);
  }
  if (content.empty()) {
    return 0;
  }

  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.push_back("CMD: SET_AINPCINFO_TEXT: " + content);
  LeaveCriticalSection(&g_msgMutex);
  return 0;
}

void OnAiNpcInfoCloseClick(MyGUI::Widget *sender) { CloseAiNpcInfoUI(); }

void CreateAiNpcInfoUI() {
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) {
    return;
  }
  if (g_aiNpcInfoWindow) {
    CloseAiNpcInfoUI();
  }

  g_aiNpcInfoWindow = gui->createWidgetReal<MyGUI::Window>(
      "Kenshi_WindowCX", 0.12f, 0.08f, 0.76f, 0.84f, MyGUI::Align::Center,
      "Popup", "Stobe_AiNpcWindow");
  if (!g_aiNpcInfoWindow) {
    return;
  }

  g_aiNpcInfoWindow->setCaption(WideFromUtf8(T("Stobe NPCs")).c_str());
  g_aiNpcInfoWindow->eventWindowButtonPressed +=
      MyGUI::newDelegate(OnAiNpcInfoWindowButtonPressed);

  MyGUI::Widget *client = g_aiNpcInfoWindow->getClientWidget();
  if (!client) {
    return;
  }

  g_aiNpcInfoList = client->createWidgetReal<MyGUI::ListBox>(
      "Kenshi_ListBox", 0.02f, 0.03f, 0.30f, 0.82f,
      MyGUI::Align::Left | MyGUI::Align::VStretch, "Stobe_AiNpcList");
  g_aiNpcInfoList->eventListSelectAccept += MyGUI::newDelegate(OnAiNpcInfoNPCSelect);
  g_aiNpcInfoList->eventListChangePosition +=
      MyGUI::newDelegate(OnAiNpcInfoNPCSelect);

  g_aiNpcInfoText = client->createWidgetReal<MyGUI::ListBox>(
      "Kenshi_ListBox", 0.34f, 0.03f, 0.64f, 0.82f, MyGUI::Align::Default,
      "Stobe_AiNpcDetail");
  g_aiNpcInfoText->addItem(
      WideFromUtf8(
          T("Select an NPC to view backstory, personality, speech style, "
            "occupation, appearance, goals, and relationships."))
          .c_str());

  g_aiNpcInfoCloseButton = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.84f, 0.88f, 0.14f, 0.08f,
      MyGUI::Align::Top | MyGUI::Align::Left,
      "Stobe_AiNpcCloseBtn");
  g_aiNpcInfoCloseButton->setCaption(WideFromUtf8(T("Close")).c_str());
  g_aiNpcInfoCloseButton->eventMouseButtonClick +=
      MyGUI::newDelegate(OnAiNpcInfoCloseClick);

  CreateThread(NULL, 0, AiNpcInfoListThread, NULL, 0, NULL);
}

void CloseAiDiaryUI() {
  if (g_aiDiaryWindow) {
    if (MyGUI::Gui::getInstancePtr()) {
      MyGUI::Gui::getInstancePtr()->destroyWidget(g_aiDiaryWindow);
    }
    g_aiDiaryWindow = nullptr;
    g_aiDiaryList = nullptr;
    g_aiDiaryText = nullptr;
    g_aiDiaryCloseButton = nullptr;
    g_aiDiaryKeys.clear();
  }
}

void PopulateAiDiaryUI(const std::string &dataInput) {
  if (!g_aiDiaryList) {
    return;
  }

  size_t selectedIndex = g_aiDiaryList->getIndexSelected();
  std::string selectedKey = "";
  if (selectedIndex != MyGUI::ITEM_NONE && selectedIndex < g_aiDiaryKeys.size()) {
    selectedKey = g_aiDiaryKeys[selectedIndex];
  }

  g_aiDiaryList->removeAllItems();
  g_aiDiaryKeys.clear();

  std::vector<std::string> displays;
  std::vector<std::string> values;
  ParsePipeEntryArray(dataInput, displays, values);
  for (size_t i = 0; i < displays.size() && i < values.size(); ++i) {
    g_aiDiaryList->addItem(WideFromUtf8(displays[i]).c_str());
    g_aiDiaryKeys.push_back(values[i]);
  }

  if (!selectedKey.empty()) {
    for (size_t i = 0; i < g_aiDiaryKeys.size(); ++i) {
      if (g_aiDiaryKeys[i] == selectedKey) {
        g_aiDiaryList->setIndexSelected(i);
        break;
      }
    }
  }
}

void SetAiDiaryText(const std::string &data) {
  if (!g_aiDiaryText) {
    return;
  }
  std::string payload = SanitizeAiNpcInfoText(data);
  if (!payload.empty() && payload[0] == ' ') {
    payload = payload.substr(1);
  }
  g_aiDiaryText->removeAllItems();
  std::stringstream ss(payload);
  std::string line;
  bool any = false;
  while (std::getline(ss, line)) {
    g_aiDiaryText->addItem(WideFromUtf8(line).c_str());
    any = true;
  }
  if (!any) {
    g_aiDiaryText->addItem(WideFromUtf8(payload).c_str());
  }
}

void OnAiDiarySelect(MyGUI::ListBox *sender, size_t index) {
  if (index == MyGUI::ITEM_NONE || index >= g_aiDiaryKeys.size()) {
    return;
  }

  std::string displayName = sender->getItemNameAt(index);
  std::string sid = g_aiDiaryKeys[index];

  if (g_aiDiaryText) {
    g_aiDiaryText->removeAllItems();
    g_aiDiaryText->addItem(
        WideFromUtf8(T("Loading diaries for ") + displayName + "...").c_str());
  }

  AiDiaryTask *task = new AiDiaryTask();
  task->json = "{\"action\":\"detail\",\"sid\":\"" + EscapeJSON(sid) + "\"}";
  CreateThread(NULL, 0, AiDiaryDetailThread, task, 0, NULL);
}

void OnAiDiaryWindowButtonPressed(MyGUI::Window *sender,
                                  const std::string &name) {
  if (name == "close") {
    CloseAiDiaryUI();
  }
}

DWORD WINAPI AiDiaryListThread(LPVOID lpParam) {
  std::string response =
      PostToStobeWithResponse(L"/ai_diaries/list", "{\"action\":\"list\"}");
  if (response.empty()) {
    return 0;
  }

  std::string names = JsonReadField(response, "names");
  if (names.empty()) {
    names = JsonReadField(response, "entries");
  }
  if (names.empty()) {
    return 0;
  }

  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.push_back("CMD: POPULATE_AIDIARIES: " + names);
  LeaveCriticalSection(&g_msgMutex);
  return 0;
}

DWORD WINAPI AiDiaryDetailThread(LPVOID lpParam) {
  AiDiaryTask *task = (AiDiaryTask *)lpParam;
  std::string response = PostToStobeWithResponse(L"/ai_diaries/detail", task->json);
  delete task;
  if (response.empty()) {
    return 0;
  }

  std::string content = JsonReadField(response, "text");
  if (content.empty()) {
    content = JsonReadField(response, "content");
  }
  content = SanitizeAiNpcInfoText(content);
  if (content.empty()) {
    content = "Failed to parse AI Diary detail response.\n\nRaw response:\n" +
              SanitizeAiNpcInfoText(response);
  }
  if (content.empty()) {
    return 0;
  }

  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.push_back("CMD: SET_AIDIARY_TEXT: " + content);
  LeaveCriticalSection(&g_msgMutex);
  return 0;
}

void OnAiDiaryCloseClick(MyGUI::Widget *sender) { CloseAiDiaryUI(); }

void CreateAiDiaryUI() {
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) {
    return;
  }
  if (g_aiDiaryWindow) {
    CloseAiDiaryUI();
  }

  g_aiDiaryWindow = gui->createWidgetReal<MyGUI::Window>(
      "Kenshi_WindowCX", 0.12f, 0.08f, 0.76f, 0.84f, MyGUI::Align::Center,
      "Popup", "Stobe_AiDiaryWindow");
  if (!g_aiDiaryWindow) {
    return;
  }

  g_aiDiaryWindow->setCaption(WideFromUtf8(T("Stobe Diaries")).c_str());
  g_aiDiaryWindow->eventWindowButtonPressed +=
      MyGUI::newDelegate(OnAiDiaryWindowButtonPressed);

  MyGUI::Widget *client = g_aiDiaryWindow->getClientWidget();
  if (!client) {
    return;
  }

  g_aiDiaryList = client->createWidgetReal<MyGUI::ListBox>(
      "Kenshi_ListBox", 0.02f, 0.03f, 0.30f, 0.82f,
      MyGUI::Align::Left | MyGUI::Align::VStretch, "Stobe_AiDiaryList");
  g_aiDiaryList->eventListSelectAccept += MyGUI::newDelegate(OnAiDiarySelect);
  g_aiDiaryList->eventListChangePosition += MyGUI::newDelegate(OnAiDiarySelect);

  g_aiDiaryText = client->createWidgetReal<MyGUI::ListBox>(
      "Kenshi_ListBox", 0.34f, 0.03f, 0.64f, 0.82f, MyGUI::Align::Default,
      "Stobe_AiDiaryDetail");
  g_aiDiaryText->addItem(
      WideFromUtf8(T("Select an NPC to view recent diary entries.")).c_str());

  g_aiDiaryCloseButton = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.84f, 0.88f, 0.14f, 0.08f,
      MyGUI::Align::Top | MyGUI::Align::Left,
      "Stobe_AiDiaryCloseBtn");
  g_aiDiaryCloseButton->setCaption(WideFromUtf8(T("Close")).c_str());
  g_aiDiaryCloseButton->eventMouseButtonClick +=
      MyGUI::newDelegate(OnAiDiaryCloseClick);

  CreateThread(NULL, 0, AiDiaryListThread, NULL, 0, NULL);
}

} // namespace UI
} // namespace Stobe

