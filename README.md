# Stobe

This is the Kenshi DLL mod for Stobe AI Framework.

## Runtime Flow

1. `RE_Kenshi.exe` loads `Stobe.dll`.
2. Stobe captures gameplay context and chat events.
3. Stobe sends HTTP requests to `StobeServer`.
4. Stobe receives streamed dialogue/action lines and applies them in-game.
5. Optional TTS audio is fetched and played while speech bubbles stay in sync.

## Live Director

Press `Home` during a loaded game to open the Live Director. Describe an action,
generate a Lua script through the default StobeServer LLM connector, review the
source, and choose **Execute Reviewed Script**. The game remains paused while
the console is open, and no generated script runs without that confirmation.

The embedded Lua 5.4 runtime exposes only a bounded `kenshi` API for live world
inspection, notifications, saves, game speed, movement, and player-squad
teleports. File, process, network, package, debug, native-module, raw-pointer,
and arbitrary-memory access are unavailable. Scripts have 24 KiB source, 8 MiB
memory, and 250,000-instruction limits. Mutating scripts request a recovery save
named `STOBE Director Recovery` before execution.

Set `EnableDirector=0` in `StobeCustom.ini` to disable it, or set
`DirectorHotkey` to `HOME` or `F6`-`F12`.

## Configuration (Update-Safe)

Stobe uses layered INI config under `Kenshi\\mods\\Stobe\\`:

- `Stobe.ini`: baseline defaults shipped with the mod and safe to overwrite on update.
- `StobeCustom.ini`: user/runtime overrides persisted across updates.

Load and save behavior:

- Load order is `Stobe.ini` first, then `StobeCustom.ini` overrides.
- Runtime/UI saves write to `StobeCustom.ini` only.
- On first run, if `StobeCustom.ini` does not exist, Stobe seeds it from `Stobe.ini` (or creates a minimal `[Settings]` file).
- `ServerHost`/`ServerPort` can be overridden in the INI to point Stobe at a different machine; local defaults continue to use launcher discovery/fallback.

This preserves user settings while still allowing newly added defaults to flow in from updated `Stobe.ini`.

## Dependencies

- [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi)
- [KenshiLib](https://github.com/KenshiReclaimer/KenshiLib)
- [nlohmann/json](https://github.com/nlohmann/json)
- [Lua 5.4](https://www.lua.org/)

## Build

From repo root, use the locked SDK wrapper:

```powershell
.\scripts\build-stobe-sdk.ps1
```

Build and deploy to Kenshi in one command:

```powershell
.\scripts\build-stobe-sdk.ps1 -Deploy
```

### Toolset Requirement (`v100`)

Stobe requires MSVC 2010 x64 toolset (`v100`) for ABI compatibility with Kenshi/MyGUI dependencies.

- Enforced by build script: `scripts/build-stobe-sdk.ps1`
- Required compiler path: `C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\bin\amd64\cl.exe`

If missing, install:

1. Windows SDK 7.1 (`GRMSDKX_EN_DVD.iso`, run `setup\SdkSetup.exe`)
2. VS 2010 SP1 compiler update (`VC-Compiler-KB2519277.exe`)

Then rerun:

```powershell
.\scripts\build-stobe-sdk.ps1
```

Note: deploy copies `mod/*` into `Kenshi\\mods\\Stobe` and will replace `Stobe.ini` there by design. User changes should live in `StobeCustom.ini`.

`build-stobe-sdk.ps1` is self-contained (prepare SDK snapshot, build, optional deploy) and does not depend on the legacy wrapper scripts.

## PR Submissions

Building AI systems is complex, and changes can unintentionally affect other connected systems. Before opening a pull request, follow the repository PR template and make sure the change has been discussed with either `RANGROO` or `tyler.maister` in Discord. When adding new features, prefer making them optional or toggleable where practical.
