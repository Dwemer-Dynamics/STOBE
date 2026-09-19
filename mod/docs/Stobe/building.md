# Building and packaging Stobe

## Prerequisites

Build from a [STOBE source checkout](https://github.com/Dwemer-Dynamics/STOBE), not an installed mod folder. Paths and commands below assume its root.

- Windows, CMake 3.21 or newer for the Visual Studio 2022 commands below, Visual Studio/MSBuild, and the MSVC 2010 x64 (`v100`) toolchain. Visual Studio 2022 may host the build, but the compiler must remain v100 for Kenshi/MyGUI ABI compatibility.
- [KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib), `RE_Kenshi_mods` branch at the exact commit in `KENSHILIB_REVISION`; build its matching x64 v100 import library and runtime DLL using its instructions.
- Matching KenshiLib headers and MyGUI/Ogre libraries, plus Boost 1.60.0 headers. Supply one coherent SDK tree; mixing headers and runtime binaries can compile successfully and fail in game.
- Compatible [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) and its matching RVA data for runtime testing.

The SDK is not bundled in this checkout. Obtain these dependencies before configuring. Installing Windows SDK 7.1 and the VS 2010 SP1 compiler update is part of the established v100 setup; verify the actual compiler selected by MSBuild.

## Standalone CMake build

Set these environment variables to your prepared dependency paths: `STOBE_SDK_ROOT` (containing `Include`, `KenshiLib.lib`, `Libraries/mygui/MyGUIEngine_x64.lib`, and `Libraries/ogre/OgreMain_x64.lib`) and `STOBE_BOOST_ROOT` (containing `boost/atomic.hpp`). Then run in PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -T v100 `
  "-DKENSHI_LIB_INCLUDE_DIR=$env:STOBE_SDK_ROOT/Include" `
  "-DKENSHI_LIB_LIBRARY=$env:STOBE_SDK_ROOT/KenshiLib.lib" `
  "-DBOOST_INCLUDE_DIR=$env:STOBE_BOOST_ROOT" `
  -DSTOBE_DIAG_PROFILE=normal
# Continue only if configure succeeds.
cmake --build build --config Release
```

The expected output is `build/Release/Stobe.dll`. Confirm x64 architecture, selected compiler, source commit, and dependency revision before deployment. CMake does not itself prepare or verify the locked SDK revision. The legacy `Stobe.vcxproj` contains monorepo-relative dependency paths; prefer CMake for a standalone checkout.

Maintainers with the separate Dwemer monorepo may use its `scripts/build-stobe-sdk.ps1` wrapper from the **monorepo root**. That wrapper is not included in the public STOBE checkout. It prepares the locked SDK, builds, stages the DLL in `mod/`, and can deploy when explicitly requested. Do not copy a personal maintainer path into public instructions or treat missing wrappers as a reason to invent a command.

## Focused validation

Portable C++ tests use a modern compiler and do not require the game SDK:

```powershell
cmake -S tests/cpp -B build-tests -G "Visual Studio 17 2022" -A x64
cmake --build build-tests --config Release --parallel
ctest --test-dir build-tests -C Release --output-on-failure
```

Stop on a failed command. The equivalent convenience entry point is `scripts/test-cpp.ps1`. Portable tests cover extracted logic, not the v100 DLL, RE_Kenshi loading, networking, or in-game behavior. Run tests appropriate to the changed code and report those limits.

## Package contract

`mod/` is the package source. A release includes `mod.info`, `RE_Kenshi.json`, `Stobe.dll`, `Stobe.ini`, `Stobe.mod`, `README.md`, and `docs/Stobe/{AGENTS.md,agent-guide.md,building.md}`. Keep this documentation subtree intact and validate the extracted archive contains it with working relative links. These are the canonical guides; no generated copies need synchronization.

Stage the verified release DLL when preparing a release, but do not commit generated binaries. Preserve existing `StobeCustom.ini` during deployment. Do not package a user's custom settings, logs, caches, or `DwemerDistro.exe`. Packaging tools with explicit file lists must include the four documentation files; copying only the original five runtime files omits the agent guide.

Server files are distributed separately from [StobeServer](https://github.com/Dwemer-Dynamics/StobeServer). Packaging is not release authorization, and a staged archive is not proof of deployment or gameplay.
