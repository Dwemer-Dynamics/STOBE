# Stobe architecture and diagnostics

## Runtime flow

Kenshi loads RE_Kenshi, which loads `Stobe.dll` using `RE_Kenshi.json`. The plugin captures player input and world/NPC context, sends HTTP requests to StobeServer, then consumes dialogue/action responses and optional speech audio. StobeServer owns model connectors, prompts, stored memories, and speech synthesis. The client owns live game objects, local microphone capture, UI, action execution, and playback.

## Source map

Paths below are relative to the [client source checkout](https://github.com/Dwemer-Dynamics/STOBE).

| Task | Start here |
| --- | --- |
| Plugin startup, hooks and game loop | `src/main.cpp` |
| Requests and streamed replies | `src/Comm.cpp`, `src/PlaythroughSession.cpp` |
| NPC/world snapshots and identity | `src/Context.cpp`, `src/StobeIdentityRename.cpp` |
| Input and interaction | `src/ChatUI.cpp`, `src/Interaction.cpp`, `src/ChatBox.cpp` |
| Settings, logging and paths | `src/Utils.cpp`, `src/Globals.cpp`, `src/SettingsWindow.cpp` |
| Microphone and playback | `src/VoiceCapture.cpp`, `src/AudioPlayback.cpp`, `src/DialogueMenuTts.cpp` |
| Action execution and autonomy | `src/Functions.cpp`, `src/AutonomyController.cpp`, `src/AutonomyExecutor.cpp` |
| World state and player base | `src/WorldStateRuntime.cpp`, `src/PlayerBaseState.cpp` |
| Package and dependencies | `mod/`, `CMakeLists.txt`, `KENSHILIB_REVISION` |

The [server repository](https://github.com/Dwemer-Dynamics/StobeServer) has its own `AGENTS.md` and `docs/agent-guide.md`. Inspect it independently for server changes; similarly named CHIM and Dialectic components are not interchangeable.

## Configuration and diagnostics

The usual mod directory is `<Kenshi>/mods/Stobe/`. `Stobe.ini` loads first, then `StobeCustom.ini` overrides it. First launch seeds the custom file if absent. Updates can replace defaults but must preserve the custom file. `ServerHost` and `ServerPort` control remote-server overrides; inspect discovery/fallback logic in `src/Comm.cpp` before assuming localhost is in use.

The client writes `stobe.log` inside the mod directory and beside the game executable; it also writes legacy `Stobe_SDK.log` relative to the process working directory. Compare timestamps with `<Kenshi>/kenshi.log`, `RE_Kenshi_log.txt`, and the server's `log/` and Apache/PHP logs. Redact credentials, endpoints, and personal conversation data before sharing.

- **Plugin does not load:** check RE_Kenshi registration, matching x64 dependencies and RVA data, then loader logs. Do not infer a server failure from a loader error.
- **No reply:** establish whether input/context reached the server, inspect its processor/connector result, then the client's stream handling.
- **Missing speech:** separate microphone capture, speech-to-text upload, model response, speech synthesis, download, and playback using matching timestamps.
- **Wrong NPC or squad context:** compare captured identity and snapshot payloads with the server's stored identity before changing routing.

Record the source commit, installed version, reproduction steps, and logs used. A DLL build or HTTP response alone does not prove an in-game fix.

## Making custom plugins

For a separate native Kenshi plugin, start with [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) and [KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) and their current examples/instructions. Stobe's `mod/RE_Kenshi.json` demonstrates the `Plugins` DLL list, and `src/main.cpp` demonstrates its loader entry point and hook setup. Use your own mod directory and DLL name; do not overwrite Stobe's DLL or registration. Confirm compatible toolchain, dependencies and hook behavior with both plugins installed.

Stobe does not provide a documented stable third-party native addon ABI. A feature that changes its internal game actions, requests or UI normally requires a focused STOBE source contribution. For custom prompts, model/speech providers or server logic, start with [StobeServer](https://github.com/Dwemer-Dynamics/StobeServer), its agent/extension guide and its actual dispatcher/call sites; a CHIM extension is not automatically compatible.

Use existing settings and import formats when they meet the need. Test a native addon in a separate game/mod setup and test server extensions against disposable data. Preserve user saves and configuration, and report separately whether compilation, loading, hook interaction and gameplay were exercised.
