#include "AiNpcInfoWindow.h"
#include "Comm.h"
#include "Globals.h"
#include "Utils.h"
#include <algorithm>
#include <cctype>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_ListBox.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Window.h>
#include <string>
#include <vector>

namespace Stobe {
namespace UI {

MyGUI::Window *g_aiNpcInfoWindow = nullptr;
MyGUI::ListBox *g_aiNpcInfoList = nullptr;
MyGUI::EditBox *g_aiNpcInfoText = nullptr;
MyGUI::Button *g_aiNpcInfoCloseButton = nullptr;
std::vector<std::string> g_aiNpcInfoStorageIds;
MyGUI::Window *g_aiDiaryWindow = nullptr;
MyGUI::ListBox *g_aiDiaryList = nullptr;
MyGUI::ListBox *g_aiDiaryEntryList = nullptr;
MyGUI::EditBox *g_aiDiaryText = nullptr;
MyGUI::Button *g_aiDiaryCloseButton = nullptr;
std::vector<std::string> g_aiDiaryKeys;
std::vector<std::string> g_aiDiaryEntryIds;

namespace {
MyGUI::EditBox *g_aiNpcSearch = nullptr;
std::vector<std::string> g_aiNpcAllDisplays;
std::vector<std::string> g_aiNpcAllStorageIds;
std::string g_aiNpcPendingKey;
std::string g_aiDiaryPendingPerson;
std::string g_aiDiaryPendingEntry;

std::string SanitizeUiText(std::string input) {
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

void TrimInline(std::string &value) {
  size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    value.clear();
    return;
  }
  size_t last = value.find_last_not_of(" \t\r\n");
  value = value.substr(first, last - first + 1);
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
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
    std::string entry = next == std::string::npos
                            ? data.substr(cur)
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

void ConfigureReadOnlyText(MyGUI::EditBox *box) {
  if (!box) {
    return;
  }
  box->setEditReadOnly(true);
  box->setEditMultiLine(true);
  box->setEditWordWrap(true);
  box->setVisibleVScroll(true);
  box->setVisibleHScroll(false);
  box->setNeedKeyFocus(false);
}

void SetReadOnlyText(MyGUI::EditBox *box, const std::string &text) {
  if (!box) {
    return;
  }
  std::string payload = SanitizeUiText(text);
  if (!payload.empty() && payload[0] == ' ') {
    payload.erase(0, 1);
  }
  box->setOnlyText(WideFromUtf8(payload).c_str());
  box->setVScrollPosition(0);
}

void QueueUiCommand(const std::string &command, const std::string &data) {
  EnterCriticalSection(&g_msgMutex);
  g_messageQueue.push_back("CMD: " + command + ":" + data);
  LeaveCriticalSection(&g_msgMutex);
}

void StartUiWorker(LPTHREAD_START_ROUTINE worker, LPVOID task,
                   const std::string &name) {
  HANDLE thread = CreateThread(NULL, 0, worker, task, 0, NULL);
  if (!thread) {
    Log("UI_WARN: failed to start " + name + " worker.");
    return;
  }
  CloseHandle(thread);
}

void ApplyNpcFilter() {
  if (!g_aiNpcInfoList) {
    return;
  }

  std::string query =
      g_aiNpcSearch ? LowerAscii(g_aiNpcSearch->getCaption()) : "";
  TrimInline(query);
  g_aiNpcInfoList->removeAllItems();
  g_aiNpcInfoStorageIds.clear();
  for (size_t i = 0;
       i < g_aiNpcAllDisplays.size() && i < g_aiNpcAllStorageIds.size(); ++i) {
    if (!query.empty() &&
        LowerAscii(g_aiNpcAllDisplays[i]).find(query) == std::string::npos) {
      continue;
    }
    g_aiNpcInfoList->addItem(WideFromUtf8(g_aiNpcAllDisplays[i]).c_str());
    g_aiNpcInfoStorageIds.push_back(g_aiNpcAllStorageIds[i]);
  }

  if (g_aiNpcInfoStorageIds.empty()) {
    SetReadOnlyText(g_aiNpcInfoText, query.empty() ? "No NPC profiles found."
                                                  : "No NPCs match this search.");
  }
}

void OnAiNpcSearchChange(MyGUI::EditBox *sender) { ApplyNpcFilter(); }

void SetDiaryEntriesLoading(const std::string &message) {
  if (g_aiDiaryEntryList) {
    g_aiDiaryEntryList->removeAllItems();
    g_aiDiaryEntryIds.clear();
    g_aiDiaryEntryList->addItem(WideFromUtf8(message).c_str());
  }
}
} // namespace

void CloseAiNpcInfoUI() {
  if (g_aiNpcInfoWindow && !TryDestroyWidgetSafe(g_aiNpcInfoWindow)) {
    Log("UI_WARN: CloseAiNpcInfoUI destroyWidget failed; clearing stale pointer.");
  }
  g_aiNpcInfoWindow = nullptr;
  g_aiNpcInfoList = nullptr;
  g_aiNpcInfoText = nullptr;
  g_aiNpcInfoCloseButton = nullptr;
  g_aiNpcSearch = nullptr;
  g_aiNpcInfoStorageIds.clear();
  g_aiNpcAllDisplays.clear();
  g_aiNpcAllStorageIds.clear();
  g_aiNpcPendingKey.clear();
}

void PopulateAiNpcInfoUI(const std::string &dataInput) {
  ParsePipeEntryArray(dataInput, g_aiNpcAllDisplays, g_aiNpcAllStorageIds);
  ApplyNpcFilter();
}

void SetAiNpcInfoText(const std::string &data) {
  size_t split = data.find('\n');
  if (split != std::string::npos) {
    std::string key = data.substr(0, split);
    if (!g_aiNpcPendingKey.empty() && key != g_aiNpcPendingKey) {
      Log("UI: ignored stale NPC detail response key=" + key);
      return;
    }
    SetReadOnlyText(g_aiNpcInfoText, data.substr(split + 1));
    return;
  }
  SetReadOnlyText(g_aiNpcInfoText, data);
}

void OnAiNpcInfoNPCSelect(MyGUI::ListBox *sender, size_t index) {
  if (index == MyGUI::ITEM_NONE || index >= g_aiNpcInfoStorageIds.size()) {
    return;
  }

  std::string displayName = sender->getItemNameAt(index);
  std::string sid = g_aiNpcInfoStorageIds[index];
  g_aiNpcPendingKey = sid;
  SetReadOnlyText(g_aiNpcInfoText,
                  T("Loading profile for ") + displayName + "...");

  AiNpcInfoTask *task = new AiNpcInfoTask();
  task->npcName = displayName;
  task->requestKey = sid;
  task->json = "{\"action\":\"detail\",\"sid\":\"" + EscapeJSON(sid) + "\"}";
  StartUiWorker(AiNpcInfoDetailThread, task, "NPC detail");
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
    QueueUiCommand("SET_AINPCINFO_TEXT",
                   "Unable to load NPCs. Check the StobeServer connection.");
    return 0;
  }

  std::string names = JsonReadField(response, "names");
  if (names.empty()) {
    names = JsonReadField(response, "characters");
  }
  if (names.empty()) {
    std::string error = JsonReadField(response, "error");
    QueueUiCommand("SET_AINPCINFO_TEXT",
                   error.empty() ? "No NPC profiles found." : error);
    return 0;
  }
  QueueUiCommand("POPULATE_AINPCINFO", names);
  return 0;
}

DWORD WINAPI AiNpcInfoDetailThread(LPVOID lpParam) {
  AiNpcInfoTask *task = static_cast<AiNpcInfoTask *>(lpParam);
  std::string key = task->requestKey;
  std::string response = PostToStobeWithResponse(L"/ai_npcs/detail", task->json);
  delete task;

  std::string content = JsonReadField(response, "text");
  if (content.empty()) {
    content = JsonReadField(response, "content");
  }
  if (content.empty()) {
    content = JsonReadField(response, "error");
  }
  if (content.empty()) {
    content = response.empty() ? "Unable to load this NPC profile."
                               : "The server returned an unreadable NPC profile.";
  }
  QueueUiCommand("SET_AINPCINFO_TEXT", key + "\n" + SanitizeUiText(content));
  return 0;
}

void OnAiNpcInfoCloseClick(MyGUI::Widget *sender) { CloseAiNpcInfoUI(); }

void CreateAiNpcInfoUI() {
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) {
    return;
  }
  CloseAiNpcInfoUI();

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

  g_aiNpcSearch = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", 0.02f, 0.03f, 0.30f, 0.07f,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_AiNpcSearch");
  g_aiNpcSearch->setCaption(WideFromUtf8("").c_str());
  g_aiNpcSearch->setEditMultiLine(false);
  g_aiNpcSearch->setEditWordWrap(false);
  g_aiNpcSearch->setVisibleVScroll(false);
  g_aiNpcSearch->eventEditTextChange +=
      MyGUI::newDelegate(OnAiNpcSearchChange);

  g_aiNpcInfoList = client->createWidgetReal<MyGUI::ListBox>(
      "Kenshi_ListBox", 0.02f, 0.12f, 0.30f, 0.73f,
      MyGUI::Align::Left | MyGUI::Align::VStretch, "Stobe_AiNpcList");
  g_aiNpcInfoList->eventListChangePosition +=
      MyGUI::newDelegate(OnAiNpcInfoNPCSelect);

  g_aiNpcInfoText = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", 0.34f, 0.03f, 0.64f, 0.82f,
      MyGUI::Align::Default, "Stobe_AiNpcDetail");
  ConfigureReadOnlyText(g_aiNpcInfoText);
  SetReadOnlyText(
      g_aiNpcInfoText,
      T("Search or select an NPC to view their profile settings, biography, "
        "live status, relationships, and recent events."));

  g_aiNpcInfoCloseButton = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.84f, 0.88f, 0.14f, 0.08f,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_AiNpcCloseBtn");
  g_aiNpcInfoCloseButton->setCaption(WideFromUtf8(T("Close")).c_str());
  g_aiNpcInfoCloseButton->eventMouseButtonClick +=
      MyGUI::newDelegate(OnAiNpcInfoCloseClick);

  StartUiWorker(AiNpcInfoListThread, NULL, "NPC list");
}

void CloseAiDiaryUI() {
  if (g_aiDiaryWindow && !TryDestroyWidgetSafe(g_aiDiaryWindow)) {
    Log("UI_WARN: CloseAiDiaryUI destroyWidget failed; clearing stale pointer.");
  }
  g_aiDiaryWindow = nullptr;
  g_aiDiaryList = nullptr;
  g_aiDiaryEntryList = nullptr;
  g_aiDiaryText = nullptr;
  g_aiDiaryCloseButton = nullptr;
  g_aiDiaryKeys.clear();
  g_aiDiaryEntryIds.clear();
  g_aiDiaryPendingPerson.clear();
  g_aiDiaryPendingEntry.clear();
}

void PopulateAiDiaryUI(const std::string &dataInput) {
  if (!g_aiDiaryList) {
    return;
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
  if (g_aiDiaryKeys.empty()) {
    SetReadOnlyText(g_aiDiaryText, "No diary entries have been recorded.");
  }
}

void PopulateAiDiaryEntries(const std::string &data) {
  size_t split = data.find('\n');
  if (split == std::string::npos || !g_aiDiaryEntryList) {
    return;
  }
  std::string person = data.substr(0, split);
  if (person != g_aiDiaryPendingPerson) {
    Log("UI: ignored stale diary entry list for " + person);
    return;
  }

  g_aiDiaryEntryList->removeAllItems();
  g_aiDiaryEntryIds.clear();
  std::vector<std::string> displays;
  std::vector<std::string> values;
  ParsePipeEntryArray(data.substr(split + 1), displays, values);
  for (size_t i = 0; i < displays.size() && i < values.size(); ++i) {
    g_aiDiaryEntryList->addItem(WideFromUtf8(displays[i]).c_str());
    g_aiDiaryEntryIds.push_back(values[i]);
  }
  if (g_aiDiaryEntryIds.empty()) {
    SetReadOnlyText(g_aiDiaryText, "No diary entries found for this NPC.");
  }
}

void SetAiDiaryText(const std::string &data) {
  size_t split = data.find('\n');
  if (split != std::string::npos) {
    std::string key = data.substr(0, split);
    if (!g_aiDiaryPendingEntry.empty() && key != g_aiDiaryPendingEntry) {
      Log("UI: ignored stale diary detail response key=" + key);
      return;
    }
    SetReadOnlyText(g_aiDiaryText, data.substr(split + 1));
    return;
  }
  SetReadOnlyText(g_aiDiaryText, data);
}

void OnAiDiarySelect(MyGUI::ListBox *sender, size_t index) {
  if (index == MyGUI::ITEM_NONE || index >= g_aiDiaryKeys.size()) {
    return;
  }
  std::string person = g_aiDiaryKeys[index];
  g_aiDiaryPendingPerson = person;
  g_aiDiaryPendingEntry.clear();
  SetDiaryEntriesLoading("Loading...");
  SetReadOnlyText(g_aiDiaryText,
                  "Select a dated entry after the diary list loads.");

  AiDiaryTask *task = new AiDiaryTask();
  task->requestKey = person;
  task->action = "entries";
  task->json = "{\"action\":\"entries\",\"sid\":\"" + EscapeJSON(person) +
               "\"}";
  StartUiWorker(AiDiaryDetailThread, task, "diary entry list");
}

void OnAiDiaryEntrySelect(MyGUI::ListBox *sender, size_t index) {
  if (index == MyGUI::ITEM_NONE || index >= g_aiDiaryEntryIds.size()) {
    return;
  }
  std::string entryId = g_aiDiaryEntryIds[index];
  g_aiDiaryPendingEntry = entryId;
  SetReadOnlyText(g_aiDiaryText, "Loading diary entry...");

  AiDiaryTask *task = new AiDiaryTask();
  task->requestKey = entryId;
  task->action = "entry";
  task->json = "{\"action\":\"entry\",\"rowid\":" + entryId + "}";
  StartUiWorker(AiDiaryDetailThread, task, "diary detail");
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
    QueueUiCommand("SET_AIDIARY_TEXT",
                   "Unable to load diaries. Check the StobeServer connection.");
    return 0;
  }

  std::string names = JsonReadField(response, "names");
  if (names.empty()) {
    std::string error = JsonReadField(response, "error");
    QueueUiCommand("SET_AIDIARY_TEXT",
                   error.empty() ? "No diary entries have been recorded." : error);
    return 0;
  }
  QueueUiCommand("POPULATE_AIDIARIES", names);
  return 0;
}

DWORD WINAPI AiDiaryDetailThread(LPVOID lpParam) {
  AiDiaryTask *task = static_cast<AiDiaryTask *>(lpParam);
  std::string key = task->requestKey;
  std::string action = task->action;
  std::wstring endpoint =
      action == "entries" ? L"/ai_diaries/entries" : L"/ai_diaries/entry";
  std::string response = PostToStobeWithResponse(endpoint, task->json);
  delete task;

  if (action == "entries") {
    std::string entries = JsonReadField(response, "entries");
    if (entries.empty()) {
      entries = "[]";
    }
    QueueUiCommand("POPULATE_AIDIARY_ENTRIES", key + "\n" + entries);
    return 0;
  }

  std::string content = JsonReadField(response, "text");
  if (content.empty()) {
    content = JsonReadField(response, "error");
  }
  if (content.empty()) {
    content = response.empty() ? "Unable to load this diary entry."
                               : "The server returned an unreadable diary entry.";
  }
  QueueUiCommand("SET_AIDIARY_TEXT", key + "\n" + SanitizeUiText(content));
  return 0;
}

void OnAiDiaryCloseClick(MyGUI::Widget *sender) { CloseAiDiaryUI(); }

void CreateAiDiaryUI() {
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) {
    return;
  }
  CloseAiDiaryUI();

  g_aiDiaryWindow = gui->createWidgetReal<MyGUI::Window>(
      "Kenshi_WindowCX", 0.08f, 0.08f, 0.84f, 0.84f, MyGUI::Align::Center,
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

  MyGUI::TextBox *peopleLabel = client->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", 0.02f, 0.02f, 0.24f, 0.05f,
      MyGUI::Align::Default, "Stobe_AiDiaryPeopleLabel");
  peopleLabel->setCaption(WideFromUtf8("People").c_str());
  MyGUI::TextBox *entriesLabel = client->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", 0.28f, 0.02f, 0.30f, 0.05f,
      MyGUI::Align::Default, "Stobe_AiDiaryEntriesLabel");
  entriesLabel->setCaption(WideFromUtf8("Entries").c_str());
  MyGUI::TextBox *detailLabel = client->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", 0.60f, 0.02f, 0.38f, 0.05f,
      MyGUI::Align::Default, "Stobe_AiDiaryDetailLabel");
  detailLabel->setCaption(WideFromUtf8("Diary").c_str());

  g_aiDiaryList = client->createWidgetReal<MyGUI::ListBox>(
      "Kenshi_ListBox", 0.02f, 0.08f, 0.24f, 0.77f,
      MyGUI::Align::Default, "Stobe_AiDiaryList");
  g_aiDiaryList->eventListChangePosition +=
      MyGUI::newDelegate(OnAiDiarySelect);

  g_aiDiaryEntryList = client->createWidgetReal<MyGUI::ListBox>(
      "Kenshi_ListBox", 0.28f, 0.08f, 0.30f, 0.77f,
      MyGUI::Align::Default, "Stobe_AiDiaryEntryList");
  g_aiDiaryEntryList->eventListChangePosition +=
      MyGUI::newDelegate(OnAiDiaryEntrySelect);

  g_aiDiaryText = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", 0.60f, 0.08f, 0.38f, 0.77f,
      MyGUI::Align::Default, "Stobe_AiDiaryDetail");
  ConfigureReadOnlyText(g_aiDiaryText);
  SetReadOnlyText(g_aiDiaryText,
                  T("Select an NPC, then select a dated diary entry."));

  g_aiDiaryCloseButton = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.84f, 0.88f, 0.14f, 0.08f,
      MyGUI::Align::Top | MyGUI::Align::Left, "Stobe_AiDiaryCloseBtn");
  g_aiDiaryCloseButton->setCaption(WideFromUtf8(T("Close")).c_str());
  g_aiDiaryCloseButton->eventMouseButtonClick +=
      MyGUI::newDelegate(OnAiDiaryCloseClick);

  StartUiWorker(AiDiaryListThread, NULL, "diary people");
}

} // namespace UI
} // namespace Stobe
