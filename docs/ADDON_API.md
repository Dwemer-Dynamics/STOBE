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

## ABI rules

- Plain C, x64, MSVC calling convention (`__cdecl`, the single x64 convention). The addon may use any MSVC toolset; Stobe itself stays on v100.
- No C++ or STL objects cross the boundary. Struct sizes are fixed and checked by `static_assert` in Stobe: `StobeActorRef` 8, `StobeAddonInfo` 24, `StobeActorState` 8, `StobeActionRequest` 56, `StobeAddonApiV1` 120 bytes.
- Strings are UTF-8/ANSI, NUL-terminated. Names are at most 48 bytes and text at most 1000 bytes; longer values are rejected, never truncated.
- Stobe copies every input before returning. Strings in callbacks belong to Stobe and are valid only during the callback. Nothing is freed across modules.

## Threading and lifetime

Every function may be called from any thread except `GetActorState`, which is valid only inside a Stobe callback. Game-affecting calls are copied into a bounded queue (128 items, 8 processed per frame) and executed during Stobe's `PlayerInterface::update` hook while the world is stable. `STOBE_QUEUED` means accepted, not delivered: a queued request can still be dropped (see the Stobe log, prefix `ADDON_API:`). Network work stays on Stobe's existing background HTTP workers.

Addon callbacks (action handlers and `QueueGameThreadCallback`) always run on the game thread with no Stobe lock held. A callback that faults is caught and its addon is unregistered. `UnregisterAddon` and `UnregisterActionBridge` wait for an in-flight callback when called from another thread, so a DLL can unload after they return. Do not block the game thread inside a callback.

## Actor identity and stale work

Actors are addressed by `StobeActorRef { serial, generation }`. `serial` is the Kenshi hand serial Stobe already uses for NPC identity and server snapshots. `MakeActorRef(serial)` stamps the current Stobe load generation, which changes on every save load or new game. `SendPlayerInput`, `RequestContextualResponse`, `SendEvent` and `ReportActionResult` return `STOBE_E_STALE` immediately for a reference or request from an earlier generation. Work that was queued before a load is discarded at dispatch with a log line only, because the addon already received `STOBE_QUEUED`. There is no name-based routing: a provided speaker or listener serial is authoritative even when another loaded NPC has the same name.

## Operations

| Function | Behavior |
| --- | --- |
| `RegisterAddon` / `UnregisterAddon` | Unique addon name (`[A-Za-z0-9_.-]`, ≤ 48). Up to 16 addons. Unregistering removes bridges, queued work and pending requests. |
| `MakeActorRef` | See identity above. |
| `GetRuntimeFlags` | Interaction toggle on, AI response streaming, director/continue scene active, world updated within 2 s. |
| `GetActorState` | Loaded, alive, conscious, player squad, and whether Stobe currently routes speech to the actor (talk target or queued line). Game thread only. |
| `SendPlayerInput` | The player character says `text` to the target through Stobe's normal chat request (`chat` mode, same path as voice input). Dropped when interaction is off, the chat window is open, the target is stale/not loaded, or no conscious player character exists. The chat UI's remembered target and mode are restored afterwards. |
| `RequestContextualResponse` | Speaker-locked request with no player input, using Stobe's continue/director route; `listener.serial == 0` lets Stobe choose. Dropped (not queued for later) while a response streams or a director scene is active, so it never interrupts dialogue. |
| `SendEvent` | `STOBE_EVENT_INFO`: `infoaction` stream event (`Actor: text`, or the addon name when no actor). `STOBE_EVENT_PLUGIN_STATE`: `addon_state` stream event with `<key>=<value>` from the addon name. Rate limited to 20 events per 10 s per addon. |
| `CancelDialogue` | Starts a new Stobe interrupt generation: clears queued speech/actions and stops playing TTS, like a new player send. |
| `RegisterActionBridge` | Owns `ExtCmd<Bridge>_*` actions. Bridge names are `[A-Za-z0-9]`, ≤ 48, matched case-insensitively, one owner each, 64 total. |
| `ReportActionResult` | Reports one outcome for an accepted request. |
| `QueueGameThreadCallback` | Runs a function once on the game thread unless the load generation changes first. |

StobeServer builds that include the plugin runtime acknowledge `addon_state` and `funcret` with `ok`. They do not store them as unhandled events or call the model; a server extension's `prerequest.php` observes them (see the paired `parity_probe` example). Older servers store `addon_state` through the unhandled-event path with a warning per event. Keep plugin-state events infrequent.

## ExtCmd actions

When StobeServer returns an action `ExtCmd<Bridge>_<Action>@<parameter>` for an NPC, Stobe:

1. Parses it in the existing NPC action path, after speaker resolution, duplicate suppression and the existing `action command received` `infoaction` event. Unlike built-in actions, an `ExtCmd` action never uses the name, prefix or talk-target fallbacks: the action header must carry the speaker serial and that exact NPC must be loaded. Otherwise Stobe reports `failed: speaker unresolved` without calling the addon. Streamed responses currently carry a serial only for the conversation's primary NPC, so `ExtCmd` actions selected for other speakers in a group response fail this way. A resolved actor that is dead or unconscious is skipped and logged, like other speaker-bound actions.
2. Queues dispatch to the game-thread tick and calls the bridge handler with `StobeActionRequest` (request id, actor reference and display name, full command in its original case, bridge, action and raw parameter).
3. Treats a handler return of `STOBE_ACTION_ACCEPTED` as pending until `ReportActionResult`. Nothing is reported for a request that is merely accepted.

An outcome is sent only when it is known: the addon reports success/failure, the handler rejects or faults, no bridge is registered, the speaker is unresolved, the command is malformed or the parameter exceeds 1000 bytes. Each outcome produces:

- `funcret` stream event with CHIM's data shape `command@<Command>@<parameter>@<completed|failed[: detail]>` (`@` and newlines in fields become spaces), for server extensions that observe completions.
- `infoaction` event `external action <Command> completed|failed[: detail]` for the NPC's event history.

A pending request with no report expires after 10 minutes or on load with a local log line only; Stobe does not invent a result. An action whose resolved NPC unloads, dies or is knocked out before dispatch is also dropped with a log line and no outcome. Dialectic differs: it reports `speaker_not_in_current_scene` and times out unreported requests as failed after 30 seconds, so a shared server extension can see no `funcret` from Stobe where Dialectic sends a failure. Built-in Stobe actions keep their existing paths unchanged.

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
| `stopAllDialogue` | `CancelDialogue` |
| `getChimInteractionState` | `GetRuntimeFlags` → `STOBE_RUNTIME_INTERACTION_ENABLED` (read only) |
| `Data/CHIM/server-plugins/<pkg>/<ver>.dwpkg` | `mods/<Addon>/Stobe/server-plugins/<pkg>/<ver>.dwpkg` (below) |

Not exposed:

- `sendMessage`, `requestMessage` and other untargeted or name-addressed calls (`getAgentByName`, `removeAgentByName`, name strings in the `…ForActor` variants): Stobe routes by actor reference only.
- `setAnimationBusy`, `setLocked`: Stobe has no per-actor lock honored by speaker selection, autonomy and rechat. Adding one requires changes to those selectors; a flag the runtime ignored would be misleading. Use `GetActorState` and `GetRuntimeFlags` to avoid requesting dialogue for busy actors.
- `setChimInteractionEnabled`: the interaction toggle remains player-owned.
- CHIM agent management and Skyrim engine helpers (`setDrivenByAI`, `addBasicProfile`, nearby-agent queries, location markers, `SayTo`, furniture/container checks, music scenes, bounty/arrest/item-transfer confirmations, screenshots/Soul Gaze, `IntCmd`, `WebCmd`, JSON form helpers).
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

Portable tests cover `ExtCmd` parsing, package name/version/archive rules and the package API field readers. The v100 x64 build checks compilation, ABI sizes and the export. Loading, hook timing, chat/continue dispatch, `ExtCmd` round trips, result events and package upload against a live server require in-game testing.
