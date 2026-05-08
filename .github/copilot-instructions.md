# Stobe Copilot Instructions

- This repository is the native Kenshi DLL mod for the Stobe AI framework.
- `src/main.cpp` is the primary hook/bootstrap entry point. It installs Kenshi hooks, starts the main worker threads, and coordinates startup behavior such as CSV import and chat event flow.
- `src/Comm.*` handles HTTP communication with StobeServer.
- `src/Context.*` gathers gameplay context and serializes it for server requests.
- `src/ChatUI.*`, `src/ChatBox.*`, `src/StartingWindow.*`, `src/SettingsWindow.*`, `src/WelcomeWindow.*`, and `src/AiNpcInfoWindow.*` implement the in-game UI layer.
- `src/AudioPlayback.*` handles audio playback for streamed speech.
- `src/Globals.*`, `src/Functions.*`, `src/Utils.*`, and `src/KenshiRvaCompat.cpp` hold shared runtime state, helpers, and engine compatibility logic.
- `mod/` is the shipped Kenshi mod package and intentionally tracks release assets, including `mod/Stobe.dll`, `mod/Stobe.ini`, `mod/RE_Kenshi.json`, `mod/Stobe.mod`, and `mod/mod.info`.

- Treat hook safety as a hard constraint. Kenshi engine writes must stay on the main thread and inside the correct hook/runtime flow. Avoid casual refactors around hook installation, thread ownership, or global state mutation.
- Do not treat `build/`, `build-map/`, `x64/`, or `vendor/stobe-sdk/` as source files. They are generated or local build artifacts.
- If you change native code that affects the shipped mod, keep `mod/Stobe.dll` aligned with the intended release build.

- Repository-local build metadata lives in `BUILD.md`, `CMakeLists.txt`, and `Stobe.vcxproj`.
- In the Dwemer Dynamics monorepo environment, the verified Windows build command is:

```powershell
.\scripts\build-stobe-sdk.ps1 -Configuration Release
```

- That build succeeded in the current local environment and refreshed `Stobe\mod\Stobe.dll` from the built `Stobe.dll`.
- The build requires Visual Studio 2022 as host tooling plus the MSVC 2010 x64 `v100` compiler for ABI compatibility.
- The required compiler path is `C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\bin\amd64\cl.exe`.
- The build wrapper prepares a locked SDK snapshot under `vendor/stobe-sdk/` and configures CMake with explicit `KENSHI_LIB_*` and `BOOST_INCLUDE_DIR` cache variables. Do not replace that with ad hoc library path guessing unless you are intentionally changing the build contract.

- `CSV Samples/` contains import examples and format references for Stobe-side content import behavior.
- There are no repository-local GitHub Actions workflows under `.github/`; only a PR template is present.

- When making changes:
- keep hook behavior changes tightly scoped and reason through game-thread ownership
- update UI code in the corresponding `src/*Window*` or `src/ChatUI*` files instead of mixing UI logic into unrelated hook code
- update comm/protocol changes in both `Comm.*` and the relevant context/builders
- trust these instructions first and only search more broadly when they are incomplete
