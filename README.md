# Stobe

This is the Kenshi DLL mod for Stobe AI Framework.

## Runtime Flow

1. `RE_Kenshi.exe` loads `Stobe.dll`.
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
- [KenshiLib](https://github.com/KenshiReclaimer/KenshiLib)
- [nlohmann/json](https://github.com/nlohmann/json)

## Build

From repo root, use the locked SDK wrapper:

Practical guide for day-to-day improvements, CI build, version control and rollback:

- [docs/GUIA_MEJORAS_Y_RELEASE.md](docs/GUIA_MEJORAS_Y_RELEASE.md)

CI workflows:

- Official release-compatible build: [.github/workflows/build-windows.yml](.github/workflows/build-windows.yml)
- Fast validation build: [.github/workflows/build-quick.yml](.github/workflows/build-quick.yml)

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
