#include "DirectorConsole.h"

#include "Comm.h"
#include "DirectorRuntime.h"
#include "Functions.h"
#include "Globals.h"
#include "StobeText.h"
#include "Utils.h"

#include <kenshi/Character.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_InputManager.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Window.h>
#include <sstream>
#include <string>
#include <windows.h>

namespace Stobe {
namespace UI {

MyGUI::Window *g_directorWindow = nullptr;

namespace {

struct DirectorRequestTask {
  std::string json;
  LONG generation;
};

MyGUI::EditBox *g_directorPrompt = nullptr;
MyGUI::EditBox *g_directorScript = nullptr;
MyGUI::TextBox *g_directorStatus = nullptr;
MyGUI::Button *g_directorGenerate = nullptr;
MyGUI::Button *g_directorExecute = nullptr;
bool g_directorPausedGame = false;
volatile LONG g_directorRequestActive = 0;
volatile LONG g_directorGeneration = 1;
std::string g_pendingResponse;
std::string g_planRequestId;
std::string g_planSummary;
std::string g_planScript;
bool g_planMutating = false;

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

bool TryReleaseDirectorPause(GameWorld *world) {
  if (!world) {
    return true;
  }
  __try {
    if (world->isPaused()) {
      world->userPause(false);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool TryRequestDirectorPause(GameWorld *world, bool &pausedByDirector) {
  pausedByDirector = false;
  if (!world) {
    return true;
  }
  __try {
    if (!world->isPaused()) {
      world->userPause(true);
      pausedByDirector = true;
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    pausedByDirector = false;
    return false;
  }
}

void SetStatus(const std::string &value) {
  if (g_directorStatus) {
    g_directorStatus->setCaption(WideFromUtf8(value).c_str());
  }
}

std::string BuildCharacterContext(GameWorld *world,
                                  Character *selectedCharacter) {
  std::ostringstream json;
  json << "{\"selected_serial\":";
  unsigned int selectedSerial = 0;
  if (selectedCharacter &&
      reinterpret_cast<uintptr_t>(selectedCharacter) > 0x1000) {
    try {
      selectedSerial = selectedCharacter->getHandle().serial;
    } catch (...) {
      selectedSerial = 0;
    }
  }
  json << selectedSerial << ",\"player_characters\":[";
  bool first = true;
  if (world && world->player) {
    const lektor<Character *> &characters = world->player->playerCharacters;
    for (size_t i = 0; i < characters.size(); ++i) {
      Character *character = characters[i];
      if (!character || reinterpret_cast<uintptr_t>(character) <= 0x1000) {
        continue;
      }
      try {
        const Ogre::Vector3 position = character->getPosition();
        if (!first) {
          json << ',';
        }
        first = false;
        json << "{\"serial\":" << character->getHandle().serial
             << ",\"name\":\""
             << Text::EscapeJSON(character->getName()) << "\",\"x\":"
             << position.x << ",\"y\":" << position.y << ",\"z\":"
             << position.z << ",\"selected\":"
             << (character == selectedCharacter ? "true" : "false") << '}';
      } catch (...) {
      }
    }
  }
  json << "]}";
  return json.str();
}

std::string BuildDirectorRequest(GameWorld *world, Character *selectedCharacter,
                                 const std::string &prompt) {
  return "{\"request_id\":\"" +
         ToString(static_cast<unsigned int>(GetTickCount())) +
         "\",\"prompt\":\"" + Text::EscapeJSON(prompt) +
         "\",\"api_manifest\":\"" +
         Text::EscapeJSON(Director::ApiManifest()) + "\",\"context\":" +
         BuildCharacterContext(world, selectedCharacter) + "}";
}

DWORD WINAPI DirectorRequestWorker(LPVOID parameter) {
  DirectorRequestTask *task = static_cast<DirectorRequestTask *>(parameter);
  std::string response =
      PostToStobeWithResponse(L"/director_request", task->json);
  LONG generation = task->generation;
  delete task;
  EnterCriticalSection(&g_msgMutex);
  if (generation == InterlockedCompareExchange(&g_directorGeneration, 0, 0)) {
    g_pendingResponse = response;
    InterlockedExchange(&g_directorRequestActive, 0);
  }
  LeaveCriticalSection(&g_msgMutex);
  return 0;
}

void OnDirectorGenerateClick(MyGUI::Widget *) {
  if (!g_directorPrompt ||
      InterlockedCompareExchange(&g_directorRequestActive, 0, 0) != 0) {
    return;
  }
  std::string prompt = g_directorPrompt->getCaption().asUTF8();
  size_t first = prompt.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    SetStatus("Enter a request first.");
    return;
  }
  if (prompt.length() > 2000) {
    SetStatus("Requests are limited to 2,000 characters.");
    return;
  }
  GameWorld *world = GetWorldSafe();
  Character *selected = nullptr;
  if (world && world->player && world->player->selectedCharacter.isValid()) {
    selected = ResolveLiveCharacter(world, world->player->selectedCharacter);
  }
  DirectorRequestTask *task = new DirectorRequestTask();
  task->json = BuildDirectorRequest(world, selected, prompt);
  task->generation =
      InterlockedCompareExchange(&g_directorGeneration, 0, 0);
  InterlockedExchange(&g_directorRequestActive, 1);
  g_planScript.clear();
  if (g_directorExecute) {
    g_directorExecute->setEnabled(false);
  }
  SetStatus("Asking StobeServer to plan a sandboxed Lua script...");
  HANDLE worker = CreateThread(NULL, 0, DirectorRequestWorker, task, 0, NULL);
  if (!worker) {
    delete task;
    InterlockedExchange(&g_directorRequestActive, 0);
    SetStatus("Could not start the StobeServer request.");
    return;
  }
  CloseHandle(worker);
}

void OnDirectorExecuteClick(MyGUI::Widget *) {
  if (g_planScript.empty()) {
    SetStatus("Generate a valid script before executing it.");
    return;
  }
  std::string error;
  if (!Director::QueueScript(g_planRequestId, g_planSummary, g_planScript,
                             g_planMutating, error)) {
    SetStatus("Script rejected: " + error);
    return;
  }
  CloseDirectorConsole();
}

void OnDirectorCancelClick(MyGUI::Widget *) { CloseDirectorConsole(); }

void OnDirectorWindowButtonPressed(MyGUI::Window *, const std::string &name) {
  if (name == "close") {
    CloseDirectorConsole();
  }
}

void ApplyPlannerResponse(const std::string &response) {
  std::string ok = Text::JsonReadField(response, "ok");
  if (ok != "true" && ok != "1") {
    std::string error = Text::JsonReadField(response, "error");
    if (error.empty()) {
      error = response.empty() ? "Unable to contact StobeServer."
                               : "StobeServer returned an invalid response.";
    }
    SetStatus("Planner failed: " + error);
    return;
  }

  g_planRequestId = Text::JsonReadField(response, "request_id");
  g_planSummary = Text::JsonReadField(response, "summary");
  g_planScript = Text::JsonReadField(response, "script");
  std::string mutating = Text::JsonReadField(response, "mutating");
  g_planMutating = mutating == "true" || mutating == "1";
  std::string error;
  if (!Director::ValidateScript(g_planScript, error)) {
    g_planScript.clear();
    SetStatus("Planner script rejected: " + error);
    return;
  }
  if (g_directorScript) {
    g_directorScript->setCaption(WideFromUtf8(g_planScript).c_str());
  }
  if (g_directorExecute) {
    g_directorExecute->setEnabled(true);
  }
  SetStatus((g_planMutating ? "Mutating script - review before Execute: "
                            : "Read-only script - review before Execute: ") +
            g_planSummary);
}

} // namespace

void CreateDirectorConsole(GameWorld *world, Character *) {
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui || !world) {
    return;
  }
  CloseDirectorConsole();
  g_planRequestId.clear();
  g_planSummary.clear();
  g_planScript.clear();
  g_planMutating = false;
  g_directorPausedGame = false;
  if (!TryRequestDirectorPause(world, g_directorPausedGame)) {
    Log("UI_WARN: STOBE Director could not pause the game.");
  }

  g_directorWindow = gui->createWidgetReal<MyGUI::Window>(
      "Kenshi_WindowCX", 0.12f, 0.08f, 0.76f, 0.84f,
      MyGUI::Align::Center, "Overlapped", "Stobe_DirectorWindow");
  if (!g_directorWindow) {
    return;
  }
  g_directorWindow->setCaption(WideFromUtf8("STOBE Live Director").c_str());
  g_directorWindow->eventWindowButtonPressed +=
      MyGUI::newDelegate(OnDirectorWindowButtonPressed);
  MyGUI::Widget *client = g_directorWindow->getClientWidget();
  if (!client) {
    CloseDirectorConsole();
    return;
  }

  MyGUI::TextBox *help = client->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", 0.03f, 0.02f, 0.94f, 0.06f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_DirectorHelp");
  help->setCaption(WideFromUtf8(
      "Describe a live action. StobeServer writes Lua; nothing runs until you review and Execute.")
                       .c_str());

  g_directorPrompt = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", 0.03f, 0.09f, 0.94f, 0.16f,
      MyGUI::Align::Top | MyGUI::Align::HStretch, "Stobe_DirectorPrompt");
  g_directorPrompt->setEditMultiLine(true);
  g_directorPrompt->setEditWordWrap(true);
  g_directorPrompt->setCaption(WideFromUtf8("Teleport my selected character to the coordinates I provide, then notify me when it is done.").c_str());

  g_directorScript = client->createWidgetReal<MyGUI::EditBox>(
      "Kenshi_EditBox", 0.03f, 0.28f, 0.94f, 0.45f,
      MyGUI::Align::Stretch, "Stobe_DirectorScript");
  g_directorScript->setEditMultiLine(true);
  g_directorScript->setEditWordWrap(false);
  g_directorScript->setEditReadOnly(true);
  g_directorScript->setCaption(
      WideFromUtf8("-- Generated sandboxed Lua appears here").c_str());

  g_directorStatus = client->createWidgetReal<MyGUI::TextBox>(
      "Kenshi_TextboxStandardText", 0.03f, 0.75f, 0.94f, 0.08f,
      MyGUI::Align::Bottom | MyGUI::Align::HStretch, "Stobe_DirectorStatus");
  SetStatus("Ready. " + g_directorHotkeyStr +
            " closes this console without running anything.");

  g_directorGenerate = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.03f, 0.86f, 0.28f, 0.10f,
      MyGUI::Align::Bottom | MyGUI::Align::Left, "Stobe_DirectorGenerate");
  g_directorGenerate->setCaption(WideFromUtf8("Generate Script").c_str());
  g_directorGenerate->eventMouseButtonClick +=
      MyGUI::newDelegate(OnDirectorGenerateClick);

  g_directorExecute = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.36f, 0.86f, 0.28f, 0.10f,
      MyGUI::Align::Bottom, "Stobe_DirectorExecute");
  g_directorExecute->setCaption(WideFromUtf8("Execute Reviewed Script").c_str());
  g_directorExecute->setEnabled(false);
  g_directorExecute->eventMouseButtonClick +=
      MyGUI::newDelegate(OnDirectorExecuteClick);

  MyGUI::Button *cancel = client->createWidgetReal<MyGUI::Button>(
      "Kenshi_Button1", 0.69f, 0.86f, 0.28f, 0.10f,
      MyGUI::Align::Bottom | MyGUI::Align::Right, "Stobe_DirectorCancel");
  cancel->setCaption(WideFromUtf8("Cancel").c_str());
  cancel->eventMouseButtonClick += MyGUI::newDelegate(OnDirectorCancelClick);
  MyGUI::InputManager::getInstance().setKeyFocusWidget(g_directorPrompt);
}

void CloseDirectorConsole() {
  InterlockedIncrement(&g_directorGeneration);
  InterlockedExchange(&g_directorRequestActive, 0);
  EnterCriticalSection(&g_msgMutex);
  g_pendingResponse.clear();
  LeaveCriticalSection(&g_msgMutex);
  if (g_directorPausedGame) {
    GameWorld *world = GetWorldSafe();
    if (!TryReleaseDirectorPause(world)) {
      Log("UI_WARN: STOBE Director could not release its pause.");
    }
    g_directorPausedGame = false;
  }
  if (g_directorWindow && !TryDestroyWidgetSafe(g_directorWindow)) {
    Log("UI_WARN: failed to destroy STOBE Director window.");
  }
  g_directorWindow = nullptr;
  g_directorPrompt = nullptr;
  g_directorScript = nullptr;
  g_directorStatus = nullptr;
  g_directorGenerate = nullptr;
  g_directorExecute = nullptr;
}

void UpdateDirectorConsole(GameWorld *, Character *) {
  std::string response;
  EnterCriticalSection(&g_msgMutex);
  if (!g_pendingResponse.empty()) {
    response.swap(g_pendingResponse);
  }
  LeaveCriticalSection(&g_msgMutex);
  if (!response.empty() && g_directorWindow) {
    ApplyPlannerResponse(response);
  }
}

void ResetDirectorConsole(const std::string &reason) {
  CloseDirectorConsole();
  EnterCriticalSection(&g_msgMutex);
  g_pendingResponse.clear();
  InterlockedExchange(&g_directorRequestActive, 0);
  LeaveCriticalSection(&g_msgMutex);
  g_planRequestId.clear();
  g_planSummary.clear();
  g_planScript.clear();
  g_planMutating = false;
  Director::Reset(reason);
}

} // namespace UI
} // namespace Stobe
