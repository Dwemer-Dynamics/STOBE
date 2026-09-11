#pragma once
#include <algorithm>
#include <windows.h>
#include <deque>
#include <map>

#include <string>

// Receives fixed server status codes on HTTP threads; the game thread drains HUD messages.
namespace PlaythroughNotices {
struct Notice { std::string id; std::string text; bool error; Notice() : error(false) {} };
struct State {
    CRITICAL_SECTION mutex;
    State() { InitializeCriticalSection(&mutex); }
    ~State() { DeleteCriticalSection(&mutex); }
    std::map<std::string, int> seen;
    std::deque<std::string> order;
    std::deque<Notice> pending;
};
inline BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID* context) { *context = new State(); return TRUE; }
inline State& Get() {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    PVOID context = NULL;
    InitOnceExecuteOnce(&once,Initialize,NULL,&context);
    return *static_cast<State*>(context);
}
struct Lock {
    CRITICAL_SECTION& mutex;
    explicit Lock(CRITICAL_SECTION& value) : mutex(value) { EnterCriticalSection(&mutex); }
    ~Lock() { LeaveCriticalSection(&mutex); }
};

inline bool Accept(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (value.size() > 80 || value.compare(0,3,"v1;") != 0 || value.size() < 37 || value[35] != ';') return false;
    const std::string id = value.substr(3,32);
    if (id.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    const std::string status = value.substr(36);
    Notice notice; notice.id = id;
    int rank = 0;
    if (status == "failed") { rank = 1; notice.error = true; notice.text = "Couldn't create a new Playthrough Save. No data has been rolled back."; }
    else if (status == "created") { rank = 2; notice.text = "New Playthrough Save created."; }
    else if (status == "rollback_failed") { rank = 3; notice.error = true; notice.text = "Playthrough Save created, but rollback couldn't finish. Mod processing paused."; }
    else if (status == "resumed") { rank = 4; notice.text = "New Playthrough Save created. Mod processing resumed."; }
    else return false;
    State& state = Get();
    Lock lock(state.mutex);
    std::map<std::string,int>::iterator found = state.seen.find(id);
    if (found != state.seen.end() && found->second >= rank) return false;
    if (found == state.seen.end()) {
        state.order.push_back(id);
        if (state.order.size() > 32) { state.seen.erase(state.order.front()); state.order.pop_front(); }
    }
    state.seen[id] = rank;
    for (std::deque<Notice>::iterator it=state.pending.begin(); it!=state.pending.end(); ++it) if (it->id == id) { *it = notice; return true; }
    if (state.pending.size() == 8) state.pending.pop_front();
    state.pending.push_back(notice);
    return true;
}

// Call only after the game's loading screen has closed, from its existing update loop.
inline bool Take(Notice& notice) {
    State& state = Get();
    Lock lock(state.mutex);
    if (state.pending.empty()) return false;
    notice = state.pending.front(); state.pending.pop_front();
    return true;
}

}
