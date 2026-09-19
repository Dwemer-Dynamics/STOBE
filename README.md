# Stobe

This is the Kenshi DLL mod for Stobe AI Framework.

## Runtime Flow

1. RE_Kenshi loads `Stobe.dll`.
2. Stobe captures gameplay context and chat events.
3. Stobe sends HTTP requests to `StobeServer`.
4. Stobe receives streamed dialogue/action lines and applies them in-game.
5. Optional TTS audio is fetched and played while speech bubbles stay in sync.

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
- [KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib)
- [nlohmann/json](https://github.com/nlohmann/json)

## Build and agent guidance

For AI assistants and coding agents, start with [AGENTS.md](AGENTS.md). See the [build guide](mod/docs/Stobe/building.md) for standalone CMake commands and the distinction between public files and the optional monorepo wrapper. The canonical guides ship under [mod/docs/Stobe](mod/docs/Stobe/AGENTS.md).

## PR Submissions

Building AI systems is complex, and changes can unintentionally affect other connected systems. Before opening a pull request, follow the repository PR template and make sure the change has been discussed with either `RANGROO` or `tyler.maister` in Discord. When adding new features, prefer making them optional or toggleable where practical.
