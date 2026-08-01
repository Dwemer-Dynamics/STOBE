#include "DialogueMenuTts.h"

#include "AudioPlayback.h"
#include "Comm.h"
#include "Globals.h"
#include "StobeText.h"
#include "Utils.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <kenshi/Character.h>
#include <kenshi/Dialogue.h>
#include <kenshi/Globals.h>
#include <kenshi/gui/DialogueWindow.h>
#include <kenshi/gui/ForgottenGUI.h>

namespace Stobe {
namespace DialogueMenuTts {
namespace {

const size_t kMaxPrefetchLines = 64;
const size_t kMaxTraversalNodes = 192;
const int kMaxTraversalDepth = 8;
const DWORD kPollIntervalMs = 150;
const DWORD kRetryDelayMs = 10000;

struct DialogueSnapshot {
  std::string actorName;
  std::string actorStorageId;
  unsigned int actorSerial;
  std::string visibleLine;
  std::vector<std::string> prefetchLines;
};

struct DialogueTtsTask {
  std::string key;
  std::string actorName;
  std::string actorStorageId;
  std::string line;
  LONG generation;
};

struct DialogueTtsResult {
  std::string hash;
  int durationMs;
};

struct TraversalNode {
  DialogLineData *line;
  int depth;
};

LONG g_initialized = 0;
CRITICAL_SECTION g_mutex;
HANDLE g_signal = NULL;
HANDLE g_worker = NULL;
std::deque<DialogueTtsTask> g_queue;
std::set<std::string> g_pending;
std::map<std::string, DialogueTtsResult> g_cache;
std::map<std::string, DWORD> g_retryAfterTick;
LONG g_generation = 1;
bool g_menuActive = false;
std::string g_actorStorageId;
std::string g_visibleLineKey;
std::string g_suppressedLineKey;
DWORD g_lastPollTick = 0;

bool IsUsablePointer(const void *value) {
  return value && reinterpret_cast<uintptr_t>(value) > 0x10000;
}

std::string TrimCopy(const std::string &value) {
  if (value.empty()) {
    return "";
  }
  size_t start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string NormalizeLine(const std::string &value) {
  std::string cleaned = Stobe::Text::SanitizeDialogueForEventStream(value);
  cleaned = TrimCopy(cleaned);
  if (cleaned.length() > 2000) {
    cleaned.resize(2000);
  }
  return cleaned;
}

std::string MakeLineKey(const std::string &actorStorageId,
                        const std::string &line) {
  std::string normalized = line;
  for (size_t i = 0; i < normalized.length(); ++i) {
    normalized[i] = static_cast<char>(
        std::tolower(static_cast<unsigned char>(normalized[i])));
  }
  return actorStorageId + "|" + normalized;
}

bool AddUniqueLine(const std::string &rawLine, std::set<std::string> &seen,
                   std::vector<std::string> &lines) {
  std::string line = NormalizeLine(rawLine);
  if (line.empty()) {
    return false;
  }
  std::string dedupe = line;
  for (size_t i = 0; i < dedupe.length(); ++i) {
    dedupe[i] = static_cast<char>(
        std::tolower(static_cast<unsigned char>(dedupe[i])));
  }
  if (!seen.insert(dedupe).second) {
    return false;
  }
  lines.push_back(line);
  return true;
}

void CollectReachableNpcLines(Dialogue *dialogue, Character *target,
                              DialogLineData *currentLine,
                              std::vector<std::string> &lines) {
  if (!IsUsablePointer(dialogue) || !IsUsablePointer(currentLine)) {
    return;
  }

  std::deque<TraversalNode> pending;
  std::set<DialogLineData *> visited;
  std::set<std::string> seenLines;
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string key = lines[i];
    for (size_t c = 0; c < key.length(); ++c) {
      key[c] = static_cast<char>(
          std::tolower(static_cast<unsigned char>(key[c])));
    }
    seenLines.insert(key);
  }

  TraversalNode first;
  first.line = currentLine;
  first.depth = 0;
  pending.push_back(first);

  size_t traversed = 0;
  while (!pending.empty() && traversed < kMaxTraversalNodes &&
         lines.size() < kMaxPrefetchLines) {
    TraversalNode node = pending.front();
    pending.pop_front();
    if (!IsUsablePointer(node.line) || node.depth > kMaxTraversalDepth ||
        !visited.insert(node.line).second) {
      continue;
    }
    ++traversed;

    if (node.line != currentLine && node.line->speaker == T_ME &&
        node.line->lineCount > 0 && node.line->lineCount <= 64 &&
        IsUsablePointer(node.line->texts)) {
      for (int i = 0; i < node.line->lineCount &&
                      lines.size() < kMaxPrefetchLines;
           ++i) {
        std::string expanded = node.line->texts[i];
        try {
          dialogue->insertWordSwaps(expanded, target, false, node.line);
        } catch (...) {
        }
        AddUniqueLine(expanded, seenLines, lines);
      }
    }

    if (node.depth >= kMaxTraversalDepth ||
        !IsUsablePointer(node.line->children)) {
      continue;
    }
    lektor<DialogLineData *> &children =
        node.line->children->conversationChoices;
    if (!children.valid() || children.size() > 256) {
      continue;
    }
    for (uint32_t i = 0; i < children.size(); ++i) {
      TraversalNode child;
      child.line = children[i];
      child.depth = node.depth + 1;
      pending.push_back(child);
    }
  }
}

bool CaptureSnapshotUnsafe(DialogueSnapshot &snapshot) {
  if (!IsUsablePointer(::gui) || !IsUsablePointer(::gui->dialogue)) {
    return false;
  }
  DialogueWindow *window = ::gui->dialogue;
  if (!window->getVisible() || !IsUsablePointer(window->dialogue)) {
    return false;
  }

  Dialogue *dialogue = window->dialogue;
  Character *actor = dialogue->getCharacter();
  if (!IsUsablePointer(actor) || actor->isPlayerCharacter()) {
    return false;
  }

  hand actorHandle = actor->getHandle();
  if (actorHandle.serial == 0) {
    return false;
  }

  snapshot.actorName = TrimCopy(actor->getName());
  if (snapshot.actorName.empty()) {
    return false;
  }
  snapshot.actorSerial = actorHandle.serial;
  snapshot.actorStorageId = "hand_" + ToString(actorHandle.serial);
  snapshot.visibleLine = NormalizeLine(dialogue->npcReplyText);
  if (!snapshot.visibleLine.empty()) {
    snapshot.prefetchLines.push_back(snapshot.visibleLine);
  }

  Character *target = nullptr;
  try {
    target = dialogue->getConversationTarget().getCharacter();
  } catch (...) {
    target = nullptr;
  }
  CollectReachableNpcLines(dialogue, target, dialogue->currentLine,
                           snapshot.prefetchLines);
  return true;
}

bool CaptureSnapshot(DialogueSnapshot &snapshot) {
  __try {
    return CaptureSnapshotUnsafe(snapshot);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void PlayResultLocked(const std::string &key,
                      const DialogueTtsResult &result) {
  if (!g_menuActive || key.empty() || key != g_visibleLineKey ||
      result.hash.empty()) {
    return;
  }
  InterruptTtsPlayback();
  if (!QueueTtsPlayback(result.hash, -1, 0, 1.0f,
                        TTS_PLAYBACK_OWNER_DIALOGUE_MENU)) {
    g_visibleLineKey.clear();
  }
}

DWORD WINAPI WorkerThread(LPVOID) {
  while (true) {
    WaitForSingleObject(g_signal, INFINITE);

    while (true) {
      DialogueTtsTask task;
      bool hasTask = false;
      EnterCriticalSection(&g_mutex);
      if (!g_queue.empty()) {
        task = g_queue.front();
        g_queue.pop_front();
        hasTask = true;
      }
      LeaveCriticalSection(&g_mutex);
      if (!hasTask) {
        break;
      }

      std::string payload = "{\"actor\":\"" +
                            Stobe::Text::EscapeJSON(task.actorName) +
                            "\",\"storage_id\":\"" +
                            Stobe::Text::EscapeJSON(task.actorStorageId) +
                            "\",\"text\":\"" +
                            Stobe::Text::EscapeJSON(task.line) + "\"}";
      DWORD started = GetTickCount();
      std::string response =
          PostToStobeWithResponse(L"/dialogue_tts", payload);
      DWORD elapsed = GetTickCount() - started;
      std::string ok = TrimCopy(Stobe::Text::JsonReadField(response, "ok"));
      std::string hash =
          TrimCopy(Stobe::Text::JsonReadField(response, "hash"));
      int durationMs =
          atoi(TrimCopy(Stobe::Text::JsonReadField(response, "duration_ms"))
                   .c_str());

      EnterCriticalSection(&g_mutex);
      const LONG currentGeneration =
          InterlockedCompareExchange(&g_generation, 0, 0);
      bool current = task.generation == currentGeneration &&
                     g_menuActive && task.actorStorageId == g_actorStorageId;
      if (task.generation == currentGeneration) {
        g_pending.erase(task.key);
      }
      if (current && (ok == "true" || ok == "1") && hash.length() == 32) {
        DialogueTtsResult result;
        result.hash = hash;
        result.durationMs = durationMs;
        g_cache[task.key] = result;
        g_retryAfterTick.erase(task.key);
        PlayResultLocked(task.key, result);
      } else if (current) {
        g_retryAfterTick[task.key] = GetTickCount() + kRetryDelayMs;
      }
      LeaveCriticalSection(&g_mutex);

      Log("DIALOGUE_TTS: generation result actor=" + task.actorName +
          " current=" + (current ? "1" : "0") +
          " ok=" + ((ok == "true" || ok == "1") ? "1" : "0") +
          " request_ms=" + ToString((int)elapsed) +
          " line_len=" + ToString((int)task.line.length()));
    }
  }
  return 0;
}

void EnsureInitialized() {
  if (InterlockedCompareExchange(&g_initialized, 1, 0) != 0) {
    return;
  }
  InitializeCriticalSection(&g_mutex);
  g_signal = CreateEventA(NULL, FALSE, FALSE, NULL);
  if (!g_signal) {
    Log("DIALOGUE_TTS: failed to create worker signal.");
    return;
  }
  g_worker = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
  if (!g_worker) {
    CloseHandle(g_signal);
    g_signal = NULL;
    Log("DIALOGUE_TTS: failed to create worker thread.");
  }
}

void QueueLine(const DialogueSnapshot &snapshot, const std::string &line,
               bool priority) {
  if (line.empty() || !g_signal) {
    return;
  }
  std::string key = MakeLineKey(snapshot.actorStorageId, line);

  EnterCriticalSection(&g_mutex);
  if (g_cache.find(key) != g_cache.end()) {
    LeaveCriticalSection(&g_mutex);
    return;
  }
  std::map<std::string, DWORD>::const_iterator retry =
      g_retryAfterTick.find(key);
  if (retry != g_retryAfterTick.end() &&
      static_cast<LONG>(GetTickCount() - retry->second) < 0) {
    LeaveCriticalSection(&g_mutex);
    return;
  }
  if (g_pending.find(key) != g_pending.end()) {
    if (priority) {
      for (std::deque<DialogueTtsTask>::iterator it = g_queue.begin();
           it != g_queue.end(); ++it) {
        if (it->key == key) {
          DialogueTtsTask existing = *it;
          g_queue.erase(it);
          g_queue.push_front(existing);
          break;
        }
      }
    }
    LeaveCriticalSection(&g_mutex);
    return;
  }

  DialogueTtsTask task;
  task.key = key;
  task.actorName = snapshot.actorName;
  task.actorStorageId = snapshot.actorStorageId;
  task.line = line;
  task.generation = InterlockedCompareExchange(&g_generation, 0, 0);
  g_pending.insert(key);
  if (priority) {
    g_queue.push_front(task);
  } else {
    g_queue.push_back(task);
  }
  LeaveCriticalSection(&g_mutex);
  SetEvent(g_signal);
}

void BeginSession(const DialogueSnapshot &snapshot) {
  InterlockedIncrement(&g_generation);
  EnterCriticalSection(&g_mutex);
  g_queue.clear();
  g_pending.clear();
  g_cache.clear();
  g_retryAfterTick.clear();
  g_menuActive = true;
  g_actorStorageId = snapshot.actorStorageId;
  g_visibleLineKey.clear();
  g_suppressedLineKey.clear();
  LeaveCriticalSection(&g_mutex);
  Log("DIALOGUE_TTS: dialogue opened actor=" + snapshot.actorName +
      " storage_id=" + snapshot.actorStorageId +
      " prefetch_count=" + ToString((int)snapshot.prefetchLines.size()));
}

void SetVisibleLine(const DialogueSnapshot &snapshot) {
  std::string key = MakeLineKey(snapshot.actorStorageId, snapshot.visibleLine);
  EnterCriticalSection(&g_mutex);
  if (!g_suppressedLineKey.empty()) {
    if (key == g_suppressedLineKey) {
      LeaveCriticalSection(&g_mutex);
      return;
    }
    g_suppressedLineKey.clear();
  }
  if (key == g_visibleLineKey) {
    LeaveCriticalSection(&g_mutex);
    return;
  }
  g_visibleLineKey = key;
  InterruptTtsPlaybackIfOwner(TTS_PLAYBACK_OWNER_DIALOGUE_MENU);
  std::map<std::string, DialogueTtsResult>::const_iterator cached =
      g_cache.find(key);
  if (cached != g_cache.end()) {
    PlayResultLocked(key, cached->second);
  }
  LeaveCriticalSection(&g_mutex);
  QueueLine(snapshot, snapshot.visibleLine, true);
}

} // namespace

void Update() {
  EnsureInitialized();
  if (!g_ttsEnabled || !g_enableDialogueMenuTts || !g_signal) {
    Reset("disabled");
    return;
  }

  DWORD nowTick = GetTickCount();
  if (g_lastPollTick != 0 &&
      (nowTick - g_lastPollTick) < kPollIntervalMs) {
    return;
  }
  g_lastPollTick = nowTick;

  DialogueSnapshot snapshot;
  if (!CaptureSnapshot(snapshot)) {
    Reset("menu_closed");
    return;
  }

  bool needsSession = false;
  EnterCriticalSection(&g_mutex);
  needsSession = !g_menuActive || snapshot.actorStorageId != g_actorStorageId;
  LeaveCriticalSection(&g_mutex);
  if (needsSession) {
    BeginSession(snapshot);
  }

  if (!snapshot.visibleLine.empty()) {
    SetVisibleLine(snapshot);
  }
  for (size_t i = 0; i < snapshot.prefetchLines.size(); ++i) {
    if (snapshot.prefetchLines[i] != snapshot.visibleLine) {
      QueueLine(snapshot, snapshot.prefetchLines[i], false);
    }
  }
}

void Reset(const char *reason) {
  EnsureInitialized();
  bool wasActive = false;
  EnterCriticalSection(&g_mutex);
  wasActive = g_menuActive || !g_queue.empty() || !g_pending.empty();
  if (!wasActive) {
    LeaveCriticalSection(&g_mutex);
    return;
  }
  InterlockedIncrement(&g_generation);
  g_queue.clear();
  g_pending.clear();
  g_cache.clear();
  g_retryAfterTick.clear();
  g_menuActive = false;
  g_actorStorageId.clear();
  g_visibleLineKey.clear();
  g_suppressedLineKey.clear();
  LeaveCriticalSection(&g_mutex);
  InterruptTtsPlaybackIfOwner(TTS_PLAYBACK_OWNER_DIALOGUE_MENU);
  if (wasActive) {
    Log("DIALOGUE_TTS: dialogue session cancelled reason=" +
        std::string(reason ? reason : "reset"));
  }
}

void NotifySelection() {
  EnsureInitialized();
  bool wasActive = false;
  EnterCriticalSection(&g_mutex);
  wasActive = g_menuActive;
  if (wasActive) {
    g_suppressedLineKey = g_visibleLineKey;
    g_visibleLineKey.clear();
    InterruptTtsPlaybackIfOwner(TTS_PLAYBACK_OWNER_DIALOGUE_MENU);
  }
  LeaveCriticalSection(&g_mutex);
  if (wasActive) {
    Log("DIALOGUE_TTS: response selected; current line interrupted.");
  }
}

} // namespace DialogueMenuTts
} // namespace Stobe
