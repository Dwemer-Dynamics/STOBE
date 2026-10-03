# Stobe native addon API and server package sync

Stobe exposes a small, versioned C interface so a separate RE_Kenshi plugin (an "addon") can ask Stobe for dialogue, log context and handle `ExtCmd` actions chosen by StobeServer. The header is [`include/StobeAddonApi.h`](../include/StobeAddonApi.h); the runtime is `src/AddonRuntime.cpp`. A working example is in [`examples/plugin-parity`](../examples/plugin-parity/README.md).

Stobe also uploads server packages carried by active addon mods to the selected StobeServer (`src/ServerPluginSync.cpp`).

## Getting the API

```c
HMODULE stobe = GetModuleHandleA("Stobe.dll");
StobeGetApiFn get = stobe ? (StobeGetApiFn)GetProcAddress(stobe, STOBE_GET_API_EXPORT) : NULL;
const StobeAddonApiV1 *api = get ? get(STOBE_ADDON_API_VERSION) : NULL;
```

`Stobe_GetApi` is an unmangled `extern "C"` export. It returns `NULL` for an unsupported version. RE_Kenshi does not guarantee plugin load order, so poll briefly from a worker thread (the example waits up to two minutes) and stay inert if Stobe or the export is absent. Never link against `Stobe.lib`.

Check `api->struct_size >= sizeof(StobeAddonApiV1)` before use. Later versions only append fields; a breaking change gets a new version number.

To use [version 2](#version-2-control), ask for it first and fall back:

```c
const StobeAddonApiV1 *base = get ? get(STOBE_ADDON_API_VERSION_2) : NULL;
const StobeAddonApiV2 *v2 = (base && base->api_version >= 2 &&
    base->struct_size >= sizeof(StobeAddonApiV2)) ? (const StobeAddonApiV2 *)base : NULL;
if (!v2) base = get ? get(STOBE_ADDON_API_VERSION) : NULL;  /* older Stobe */
```

[Version 3](#version-3-agents-and-context) is requested the same way with `STOBE_ADDON_API_VERSION_3`, checking `api_version >= 3` and `struct_size >= sizeof(StobeAddonApiV3)`, and falling back to 2 then 1. A Stobe without version 2 returns `NULL` for it. `Stobe_GetApi(1)` keeps returning the unchanged version 1 table. The version 2 table starts with a complete version 1 table, so `&v2->v1` can be used wherever a `StobeAddonApiV1` is expected. Test the `capabilities` bit for a feature before calling it.

## ABI rules

- Plain C, x64, MSVC calling convention (`__cdecl`, the single x64 convention). The addon may use any MSVC toolset; Stobe itself stays on v100.
- No C++ or STL objects cross the boundary. Struct sizes are fixed and checked by `static_assert` in Stobe: `StobeActorRef` 8, `StobeAddonInfo` 24, `StobeActorState` 8, `StobeActionRequest` 56, `StobeAddonApiV1` 120 bytes; version 2 adds `StobeControlStatus` 24 and `StobeAddonApiV2` 208 bytes (`capabilities` at offset 120); version 3 adds `StobeAgentInfo` 72 and `StobeAddonApiV3` 256 bytes (first new call at offset 208). The version 1 and 2 tables are unchanged.
- Strings are UTF-8/ANSI, NUL-terminated. Names are at most 48 bytes and text at most 1000 bytes; longer values are rejected, never truncated.
- Stobe copies every input before returning. Strings in callbacks belong to Stobe and are valid only during the callback. Nothing is freed across modules.

## Threading and lifetime

Every function may be called from any thread except `GetActorState`, which is valid only inside a Stobe callback. Game-affecting calls are copied into a bounded queue (128 items, 8 processed per frame) and executed during Stobe's `PlayerInterface::update` hook while the world is stable. `STOBE_QUEUED` means accepted, not delivered: a queued request can still be dropped (see the Stobe log, prefix `ADDON_API:`). Network work stays on Stobe's existing background HTTP workers.

Addon callbacks (action handlers, `QueueGameThreadCallback` and the version 2 control callback) always run on the game thread with no Stobe lock held. A callback that faults is caught and its addon is unregistered. `UnregisterAddon` and `UnregisterActionBridge` wait for an in-flight callback when called from another thread, so a DLL can unload after they return. Do not block the game thread inside a callback.

## Actor identity and stale work

Actors are addressed by `StobeActorRef { serial, generation }`. `serial` is the Kenshi hand serial Stobe already uses for NPC identity and server snapshots. `MakeActorRef(serial)` stamps the current Stobe load generation, which changes on every save load or new game. `SendPlayerInput`, `RequestContextualResponse`, `SendEvent` and `ReportActionResult` return `STOBE_E_STALE` immediately for a reference or request from an earlier generation. Work that was queued before a load is discarded at dispatch with a log line only, because the addon already received `STOBE_QUEUED`. There is no name-based routing: a provided speaker or listener serial is authoritative even when another loaded NPC has the same name.

## Operations

| Function | Behavior |
| --- | --- |
| `RegisterAddon` / `UnregisterAddon` | Unique addon name (`[A-Za-z0-9_.-]`, ≤ 48). Up to 16 addons. Unregistering removes bridges and queued work, and reports the addon's unreported accepted requests as `failed: addon unregistered`. |
| `MakeActorRef` | See identity above. |
| `GetRuntimeFlags` | Interaction toggle on, AI response streaming, director/continue scene active, world updated within 2 s. |
| `GetActorState` | Loaded, alive, conscious, player squad, and whether Stobe currently routes speech to the actor (talk target or queued line). Game thread only. |
| `SendPlayerInput` | The player character says `text` to the target through Stobe's normal chat request (`chat` mode, same path as voice input). Dropped when interaction is off, the chat window is open, the target is stale/not loaded, or no conscious player character exists. The chat UI's remembered target and mode are restored afterwards. |
| `RequestContextualResponse` | Speaker-locked request with no player input, using Stobe's continue/director route; `listener.serial == 0` lets Stobe choose. Dropped (not queued for later) while a response streams or a director scene is active, so it never interrupts dialogue. |
| `SendEvent` | `STOBE_EVENT_INFO`: `infoaction` stream event (`Actor: text`, or the addon name when no actor). `STOBE_EVENT_PLUGIN_STATE`: `addon_state` stream event with `<key>=<value>` from the addon name. Rate limited to 20 events per 10 s per addon. |
| `CancelDialogue` | Starts a new Stobe interrupt generation: clears queued speech/actions and stops playing TTS, like a new player send. |
| `RegisterActionBridge` | Owns `ExtCmd<Bridge>_*` actions. Bridge names are `[A-Za-z0-9]`, ≤ 48, matched case-insensitively, one owner each, 64 total. |
| `ReportActionResult` | Reports the outcome of an accepted request. The first report wins; a repeat or a report after the timeout returns `STOBE_E_NOT_FOUND`, and a request from an earlier load `STOBE_E_STALE`. Results bypass the work queue, so a full queue cannot lose them. |
| `QueueGameThreadCallback` | Runs a function once on the game thread unless the load generation changes first. |

StobeServer builds that include the plugin runtime acknowledge `addon_state` and `funcret` with `ok`. They do not store them as unhandled events or call the model; a server extension's `prerequest.php` observes them (see the paired `parity_probe` example). Older servers store `addon_state` through the unhandled-event path with a warning per event. Keep plugin-state events infrequent.

## Version 2: control

Version 2 adds interaction control, per-actor dialogue locks and animation-busy flags, and feedback tickets. Its version 1 functions behave as before.

| Function | Behavior |
| --- | --- |
| `GetInteractionState` | `STOBE_INTERACTION_OFF` 0, `ON` 1, `UPDATING` 2 (change being synchronized with StobeServer), `FAILED` 3 (sync failed; Stobe is locally off and retries every 5 s). Only On lets Stobe start AI work. Any thread. |
| `SetInteractionEnabled` | Requests On or Off exactly like the player's toggle (same server sync, interrupt and epoch change). Queued for the game thread; returns `STOBE_QUEUED`. |
| `SetActorLock` / `GetActorLockOwner` | Takes or releases a dialogue lock owned by the calling addon. Immediate, any thread. `STOBE_CAP_ACTOR_LOCKS`. |
| `SetActorBusy` / `GetActorBusyOwner` | Sets or clears an animation-busy flag owned by the calling addon. Immediate, any thread. `STOBE_CAP_ACTOR_BUSY`. |
| `SendPlayerInputTracked`, `RequestContextualResponseTracked` | The version 1 operations with an optional ticket. |
| `SetControlCallback` / `GetControlStatus` | Ticket feedback by callback, by polling, or both. |

### Interaction state

The player's toggle and every addon request set one desired state; the newest request wins. Each change of the desired state gets a new request sequence. A sync result is applied only if no newer request arrived while it was in flight; otherwise the same background sync immediately runs again for the newest state (up to four passes, then the normal 5 s retry), so a request made during `UPDATING` is never lost or overwritten by an older reply. Repeating the desired state, or requesting the state StobeServer already confirmed, does not start another sync. A request from a `FAILED` state retries. The player can change the state again at any time, and the toggle UI still ignores clicks while syncing. Interaction requests are carried out during a loaded world's update; one queued before a load is cancelled.

Turning interaction off gates AI dialogue, actions and speech exactly as the player's toggle does: queued lines and actions are discarded (utterances reported cancelled), in-flight replies stop delivering lines, and the line already playing finishes. It never stops microphone recording, speech-to-text upload or transcription, and does not discard a pending transcript; a transcript that arrives while interaction is not On is refused at chat submission with "Stobe is off.", as for the player's toggle.

### Dialogue locks and busy flags

An actor can carry a dialogue lock and an animation-busy flag, independently and possibly from different addons. Each lasts until its owner clears it, the owner unregisters or is disabled after a fault, or a save load or new game starts.

`SetActorLock(id, actor, 1)` keeps the actor out of Stobe's AI dialogue. While locked:

- Bored-event/director speaker and listener selection and rechat responder selection skip the actor; a requested preferred speaker that is locked cancels the event.
- Autonomy bound to the actor cancels its current action and reports `OBSERVING` with reason `actor_locked`, as for a pause; it resumes when the lock is released.
- Player chat to the actor is refused with "That character is busy."; addon `SendPlayerInput` and `RequestContextualResponse` naming it as target, speaker or listener are rejected with `STOBE_E_LOCKED`.
- Response lines for the actor are dropped (utterances are reported as cancelled), including ones already queued. Physical actions, built-in or `ExtCmd`, are not gated by the lock.

`SetActorBusy(id, actor, 1)` stops Stobe from moving or acting with the actor while the addon plays an animation. While busy:

- Built-in actions for the actor from the server are dropped before they are queued, and queued ones are dropped before they run; each is logged (`HOOK_MSG_PROC`/`ACTION_TIMING`), and an autonomy action is reported to autonomy as failed with `actor_busy`. Actions naming the actor only as target are not affected.
- Follow and travel orders are not reissued until the flag is cleared; a bounded move-to ends as "stopped moving to" without halting the actor.
- Autonomy pauses with reason `actor_busy`, as for a lock.
- An `ExtCmd` action for the actor runs only if its bridge belongs to the busy owner, so the owner can coordinate and complete its own animation. Other bridges are not called and the action is reported failed with `actor busy`. An action a bridge already accepted is unaffected; its `ReportActionResult` is delivered as usual.
- AI dialogue is excluded as for a lock: selection, rechat, request starts and response lines (queued ones included) skip the actor; addon `SendPlayerInput` and `RequestContextualResponse` naming it are rejected with `STOBE_E_ACTOR_BUSY` (with `STOBE_E_LOCKED` if also locked).

Only the owner can clear a lock or busy flag; another addon gets `STOBE_E_CONFLICT` for both set and clear. Repeating a set, or clearing an unset actor, returns `STOBE_OK`. At most 64 actors are locked and 64 busy at a time (`STOBE_E_LIMIT`). Neither ever redirects work to another actor or bypasses Stobe's own checks for dead, unconscious, unloaded or stale actors, loads or menus. Group requests can still list a locked actor as nearby context; the server may write a line for it, which is then dropped. A line or action already running finishes.

Lookups are lock-free when none exist and otherwise a shared-lock map lookup; there is no per-frame scan.

### Tickets and feedback

Pass a non-NULL `out_ticket` to get a ticket. Each ticket leaves `STOBE_CONTROL_PENDING` exactly once:

| Kind | Final states |
| --- | --- |
| Player input, contextual request | `ACCEPTED`: Stobe started the request on its network worker. `REJECTED` with `STOBE_E_DISABLED`, `BUSY`, `LOCKED`, `ACTOR_BUSY`, `NOT_FOUND`, `STALE` or `INELIGIBLE` (Stobe's chat path declined it, for example out of range). `CANCELLED` with `STOBE_E_STALE` when a load discarded it before dispatch. |
| Interaction | `COMPLETED` when StobeServer confirmed the requested state (or it was already confirmed). `FAILED` with `STOBE_E_UNCONFIRMED` when the sync failed. `CANCELLED` with `STOBE_E_SUPERSEDED` when a newer request (from any addon or the player) replaced it first, or `STOBE_E_STALE` for a load before dispatch. |

`ACCEPTED` is final: Stobe has no acknowledgement that a reply was generated, applied or spoken, and never reports one.

With a callback set, Stobe calls it on the game thread once per ticket when it leaves `PENDING`, together with other addon work (8 items per frame). A callback can arrive before a call made on another thread has returned. `GetControlStatus` reads a ticket from any thread. Stobe keeps the newest 128 tickets: a full table evicts the oldest finished ticket whose callback has run, and returns `STOBE_E_LIMIT` when every retained ticket is still pending. Unregistering removes the addon's tickets without callbacks.

## Version 3: agents and context

Version 3 (`STOBE_CAP_AGENTS`, `STOBE_CAP_CONTEXT_REFRESH`) adds agent queries, agent registration and context refresh. The table starts with a complete version 2 table whose `capabilities` include the new bits.

An agent is a loaded character Stobe's normal chat would offer as a target (alive, conscious, in talk range and area of the player speaker, using the chat dropdown rules), plus actors an addon registered, minus actors an addon unregistered. Stobe keeps no separate actor registry: queries walk Kenshi's loaded-character update list when called (at most 512 characters examined), never per frame and never the whole world. Past that bound `ListAgents` and `FindClosestAgent` answer from the characters examined; `FindAgentByName` returns `STOBE_E_LIMIT` instead, because an unexamined character could share the name.

| Function | Behavior |
| --- | --- |
| `ListAgents` | Game thread only. Up to `capacity` (≤ 64) agents, nearest first; registered actors outside talk range come last with `distance` −1. Each entry has a current-load `StobeActorRef`, `STOBE_ACTOR_*` flags plus `STOBE_ACTOR_AUTO_AGENT`, the registration mode and a display name (truncated to 47 bytes, never for routing). Set `out[0].struct_size`. |
| `FindAgentByName` | Game thread only. Exact ASCII case-insensitive match among agents. `STOBE_E_NOT_FOUND`, or `STOBE_E_AMBIGUOUS` when different actors share the name; Stobe never picks one. `STOBE_E_LIMIT` (no actor) when the scan bound left characters unexamined. |
| `FindClosestAgent` | Game thread only. Nearest agent to `origin` (serial 0: by chat distance from the player speaker; otherwise straight-line distance from that actor, excluding it). |
| `SetAgentRegistration` / `GetAgentRegistration` | Any thread, immediate. See below. |
| `RequestContextRefresh` | Any thread. Queues a context and/or inventory resend. See below. |

### Registration

`SetAgentRegistration(id, actor, mode)` stores one override per actor, owned by the calling addon:

- `STOBE_AGENT_REGISTERED`: the actor is an agent even outside talk range (it must still be loaded, alive and conscious), and Stobe queues a basic profile upload: the same identity bootstrap and context snapshot it sends for NPCs the player meets. Returns `STOBE_QUEUED` once the upload is queued (or merged with a pending one). If the refresh queue is full the call returns `STOBE_E_LIMIT` and the registration is not changed; a conflict with another addon never queues anything.
- `STOBE_AGENT_UNREGISTERED`: Stobe never chooses the actor as an AI speaker or listener by itself. Agent queries, the rechat responder and bored-event speaker and listener selection skip it. The chat target dropdown still lists it but never selects it by itself: the default is the nearest other option, and when every option is unregistered nothing is selected until the player picks one. A streamed reply line or action from it is dropped unless it is the request's own speaker (matched by exact serial), or, for a line whose speaker is matched only by name, the player's chat target. Explicit chat to it, an addon request naming it as speaker or listener, and a bored event naming it as preferred speaker still work. It stays in the nearby-people list sent with a player chat request, so the server still knows it is physically present, and world-event sweeps, profiles, memories, followers, factions and squad membership are unchanged.
- `STOBE_AGENT_AUTO`: removes the caller's override.

Stobe's automatic eligibility never overrides a registration: a registered actor that walks out of range stays an agent, and an unregistered actor that walks into range stays excluded. Another addon's override is `STOBE_E_CONFLICT` for every mode. At most 64 actors are `REGISTERED` and 64 `UNREGISTERED` at a time (128 overrides combined); a set past either limit is `STOBE_E_LIMIT` and leaves the actor's current mode unchanged. Overrides end when the owner sets `AUTO`, unregisters or faults, or a save load or new game starts. Registration reports Stobe's local state only; the profile upload is fire-and-forget and has no server confirmation.

### Context refresh

`RequestContextRefresh(id, actor, parts)` with `STOBE_REFRESH_CONTEXT` and/or `STOBE_REFRESH_INVENTORY` returns `STOBE_QUEUED` once the request is recorded. Requests for one actor merge into one entry (at most 32 actors pending; `STOBE_E_LIMIT` beyond that), which remembers each addon's parts: when an addon unregisters or faults, only its parts are withdrawn, and setting `AUTO` or `UNREGISTERED` withdraws only that addon's pending registration profile upload. Profiles already uploaded are never deleted. They run during Stobe's existing inventory sweep (about every 6 s, up to 4 actors per sweep) on the game thread, if the actor is still loaded: a context snapshot is uploaded and the inventory goes through the existing hash-checked inventory sync, which skips an unchanged inventory sent in the last few seconds. Uploads use Stobe's existing background HTTP worker; the API call never touches the network or the engine. Pending requests are dropped on load. `STOBE_QUEUED` is not a server confirmation and no ticket is issued.

### Not in this version

`sendMessageToActor` whisper/shout types and separate explicit versus automatic contextual-request entry points are deferred to a later version; `SendPlayerInput` stays normal chat.

## ExtCmd actions

When StobeServer returns an action `ExtCmd<Bridge>_<Action>@<parameter>` for an NPC, Stobe:

1. Parses it in the existing NPC action path, after speaker resolution, duplicate suppression and the existing `action command received` `infoaction` event. Unlike built-in actions, an `ExtCmd` action never uses the name, prefix or talk-target fallbacks: the action header must carry the exact speaker serial. Otherwise Stobe reports `failed: speaker unresolved` without calling the addon.
2. Queues dispatch to the game-thread tick. An actor that is no longer loaded, or is dead or unconscious, fails with `actor not loaded` or `actor unavailable`. Otherwise Stobe calls the bridge handler with `StobeActionRequest` (request id, actor reference and display name, full command in its original case, bridge, action and raw parameter).
3. Treats a handler return of `STOBE_ACTION_ACCEPTED` as pending until `ReportActionResult`.

### Speaker serial

The request's own NPC uses the handle Stobe sent with the request. For any other speaker in a streamed (group, rechat or continue) response, StobeServer appends `sid=<serial>` metadata to the `ExtCmd` action line: `<actor>|ActionQueue|ExtCmd…@<parameter>|sid=<serial>`. The server takes the serial only from the request's `people` list and omits it when that name is unlisted, listed without a serial, or listed with more than one serial. Stobe accepts the serial only when `<actor>|<serial>` is one of the identities it sent with that request (ASCII case-insensitive) and, for the request's own NPC, it equals the request handle. A missing, malformed, unlisted or conflicting serial fails as `speaker unresolved`; Stobe never picks an NPC by name. Built-in action lines carry no `sid` and are unchanged.

Compatibility: older clients ignore the unknown metadata token. With an older server, only the request's own NPC can run `ExtCmd` actions, as before.

### Outcomes

Within the load that created it, each accepted request gets exactly one outcome. Stobe sends it when the addon reports success or failure, or fails the request itself: `rejected by handler`, `handler fault` (the addon is also disabled), `addon unregistered`, or `timed out` after 10 minutes without a report. Whichever happens first wins; later reports return `STOBE_E_NOT_FOUND`. Outcomes are also sent when no request is created: no bridge is registered, too many requests are pending (128), the speaker is unresolved, the actor is unavailable, the command is malformed or the parameter exceeds 1000 bytes. Each outcome produces:

- `funcret` stream event with CHIM's data shape `command@<Command>@<parameter>@<completed|failed[: detail]>` (`@` and newlines in fields become spaces), for server extensions that observe completions.
- `infoaction` event `external action <Command> completed|failed[: detail]` for the NPC's event history.

Outcomes are reported on the game thread, at most 8 per frame together with other addon work, with no Stobe lock held. A save load or new game cancels pending requests and unsent outcomes locally: nothing from an earlier load is sent to the server, and Stobe's transport also refuses requests while the new playthrough session is not ready. An `ExtCmd` line dropped because the addon work queue is full (128) is logged only. Dialectic reports `speaker_not_in_current_scene` and times out after 30 seconds instead of 10 minutes, so detail text and timing differ between products. Built-in Stobe actions keep their existing paths unchanged.

### Follow-up turns (`stobe.addon_followup.v1`)

Stobe opts into StobeServer's addon follow-up contract by adding `addon_followup=1` to its chat, rechat and bored stream requests. For a code registered with a follow-up, the server appends `aid=<n>` after `sid=` on the action line: `<actor>|ActionQueue|ExtCmd…@<parameter>|sid=<serial>|aid=<n>`. Stobe keeps the `aid` only when the line has exactly one canonical positive `aid`, its `sid` tokens agree, and the action is bound to exactly that serial as described above. An action line whose `sid` tokens are present but malformed or conflicting, or whose `aid` is present but duplicate, malformed or unbound, is dropped; it never falls back to the request's own actor or runs as a legacy action. Internally the `aid` travels on the queued action's speaker header (`<name>|<serial>|aid=<n>`), never in the parameter, so an addon parameter is passed through byte for byte. Older servers send no `aid`, so nothing changes for them.

The `aid` stays with the queued action, the pending request and its outcome, within the load that created it. If the addon unregisters or faults, its queued completed reports are dropped and a later completion is sent as a plain `funcret`; failure reports for its unfinished requests are still sent so the server rows close. The outcome keeps the `funcret` packet above unchanged and appends `&addon_followup=1&aid=<n>&sid=<serial>&arid=<id>&people=["<name>|<serial>"]&tts_enabled=<0|1>`, using the actor's exact name and serial. `arid` is the bridge request id. A failure before any bridge accepted the request (no handler, actor busy or unavailable, too many requests, malformed command) uses a fresh id from the same sequence; no handler ever received it, so it cannot be mistaken for a handler's request. Only an accepted request's `ReportActionResult` can report `completed`; Stobe never fabricates a completion.

These reports go through a game-thread queue of at most 16 entries, one per `aid`. They are sent from the existing addon tick on the native stream path and parsed by the normal stream parser, not the fire-and-forget path. Nothing is sent while another stream request or a Director scene runs; a follow-up holds the same stream slot from the moment it is started, so only one runs at a time. A completed outcome also waits until interaction is On, the chat window is closed and no speech is queued or playing, so its reply never interrupts the player, another conversation or speech. A load drops queued reports. A completed report is also dropped when the interaction state changes, because the server would skip it anyway. A failed report is still sent to close the server's row; its stream applies no line. When the queue is full, Stobe sends the plain `funcret` instead and the server row expires unclaimed.

A follow-up stream applies only `ScriptQueue` lines whose actor is exactly the captured name and whose `sid`, if present, is the captured serial. Its action lines appear only when the server enables actions for the code (`use_functions_again`, off by default); they are applied as normal actions bound to the captured actor and serial, and any `aid` on them is discarded, so a follow-up cannot chain another follow-up. It never starts a rechat. The dialogue lock and busy flag still apply; a locked or busy actor gets the report without speech. The server's `X-Stobe-Addon-Followup` header (`v1 accepted aid=<n>`, `v1 rejected`, `v1 skipped …`) is diagnostic: the stream callback does not see headers, and non-accepted replies have an `ok` body that applies nothing. The server accepts the transport for one claim per `aid`. That acceptance says the result was correlated; the action outcome itself is the `completed`/`failed` text the addon reported.

The autonomy catalog adapter only recognizes built-in queued actions. If an autonomy decision selects an `ExtCmd` action, the adapter reports that decision as failed (`catalog_adapter_no_queued_action`) after the action is dispatched; addon bridges are intended for dialogue-selected actions.

## CHIM capability mapping

CHIM's public extension surface is its Papyrus natives in `AIAgentFunctions.psc` plus the `ExtCmd` branch of `Plugin/Commands.cpp` and server package sync. Mapping for Stobe v1:

| CHIM | Stobe v1 |
| --- | --- |
| `sendMessageToActor(msg, type, actor)` | `SendPlayerInput` (normal chat only; whisper/shout/injection types are not exposed) |
| `requestMessageForActor`, `requestMessageForEligibleActor` | `RequestContextualResponse` (speaker-locked; rejected while busy instead of queued) |
| `logMessage`, `logMessageForActor` | `SendEvent(STOBE_EVENT_INFO)` |
| `PostGameData(json)` | Partial: `SendEvent(STOBE_EVENT_PLUGIN_STATE)` bounded key/value records |
| `ExtCmd` → `<Bridge>.DispatchExternalCommand(npc, command, parameter)` | `RegisterActionBridge`; receives the actor reference instead of the NPC name |
| `commandEnded`, `commandEndedForActor` | `ReportActionResult` per request id |
| `isActorTalking(name)` | `GetActorState` → `STOBE_ACTOR_IN_STOBE_SPEECH` by actor reference |
| `getAgentByName`, nearby-agent queries | v3 `FindAgentByName` (fails on ambiguity), `ListAgents`, `FindClosestAgent` |
| `setDrivenByAI`, `addBasicProfile`, `removeAgentByName` | v3 `SetAgentRegistration` by actor reference (runtime membership only) |
| context/inventory resend | v3 `RequestContextRefresh` |
| `stopAllDialogue` | `CancelDialogue` |
| `getChimInteractionState` | `GetRuntimeFlags` → `STOBE_RUNTIME_INTERACTION_ENABLED`; v2 `GetInteractionState` |
| `setChimInteractionEnabled` | v2 `SetInteractionEnabled` |
| `setLocked` | v2 `SetActorLock` |
| `setAnimationBusy` | v2 `SetActorBusy` |
| `Data/CHIM/server-plugins/<pkg>/<ver>.dwpkg` | `mods/<Addon>/Stobe/server-plugins/<pkg>/<ver>.dwpkg` (below) |

Not exposed:

- `sendMessage`, `requestMessage` and other untargeted or name-addressed calls (name strings in the `…ForActor` variants): Stobe routes by actor reference only; v3 `FindAgentByName` turns an unambiguous name into a reference first.
- Skyrim engine helpers (location markers, `SayTo`, furniture/container checks, music scenes, bounty/arrest/item-transfer confirmations, screenshots/Soul Gaze, `IntCmd`, `WebCmd`, JSON form helpers).
- CHIM UI and configuration plumbing (MCM snapshots, Prisma/history/overlay panels, `setConf`/`get_conf_i`, microphone recording/open mic, `sendLocationFast`/`sendFactionFast`/`sendNPCFast`). Stobe owns its UI, settings and context capture.

## Server package sync

Ship server files with an addon mod as:

```text
<addon mod folder>/Stobe/server-plugins/<package>/<version>.dwpkg   (or .zip)
```

The archive is the schema-4 package format accepted by StobeServer's `lib/plugin_package_manager.php`. It contains server files only, never game DLLs.

Discovery uses Kenshi's own active-mod list (`GameWorld::activeMods`, in load order, including Workshop mods), not a scan of every folder under `mods/`. Base game data entries are skipped. For each active mod Stobe uses `ModInfo::path`, falling back to the folder of `ModInfo::file`. A package folder name must match the server's plugin-name rule and the file stem its version rule; with several archives the newest modification time wins, as in Dialectic. If two active mods carry the same package name, the later one in load order wins and a warning is logged. At most 32 packages of up to 512 MiB are considered.

Sync runs once per game process, on the first stable-world update while Stobe interaction is On, in a background thread. It resolves the same configured/discovered StobeServer target as other Stobe requests, then for each package calls `/StobeServer/ui/api/plugin_packages.php`:

1. `probe` (POST `{name, version}`); skip when `upload_required` is false.
2. `start-upload` (POST `{name, version, archive_name, size, total_chunks}`) → `upload_id`.
3. `upload-chunk&upload_id=…&index=…` with 1 MiB octet-stream chunks; the final response carries the install job.
4. `status&job_id=…` (GET, up to 30 polls 2 s apart) only if the job is not already `completed` or `failed`.

A 404/405 or non-API response stops the whole sync with one log line: the server predates the package API. Network failures and HTTP 5xx retry once after 30 s; other errors are logged per package. No credentials or Stobe session headers are sent. Logs use the prefix `SERVER_PLUGIN_SYNC:` and include package names and versions, not response bodies.

Limitations: changing the server target during a session needs a game restart to resync. `ModInfo::path` and Workshop paths come from the locked KenshiLib headers and have not yet been exercised in game.

## Validation status

Portable tests cover `ExtCmd` parsing, strict serials, the `ExtCmd` speaker-serial decision, package name/version/archive rules, the package API field readers, the version 1, 2 and 3 table layouts, lock and busy-flag ownership, agent registration ownership, ambiguous agent names, the lock/busy gate for dialogue, built-in and `ExtCmd` actions, and interaction ticket resolution. The v100 x64 build checks compilation, ABI sizes and the export. Loading, hook timing, chat/continue dispatch, `ExtCmd` round trips, result events, package upload, interaction sync against a live server, lock and busy enforcement, agent queries, registration effects on selection and refresh uploads in game require in-game testing.
