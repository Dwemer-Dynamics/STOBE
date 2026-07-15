# Stobe Build Contract (Locked SDK)

## Recommended Command

Run from monorepo root:

```powershell
.\scripts\build-stobe-sdk.ps1
```

Build and deploy:

```powershell
.\scripts\build-stobe-sdk.ps1 -Deploy
```

Build with a diagnostic profile:

```powershell
.\scripts\build-stobe-sdk.ps1 -DiagProfile no_hook -Deploy
```

## Toolchain Prerequisites (MSVC 2010 x64 / `v100`)

Stobe is built with toolset `v100` for ABI compatibility with Kenshi/MyGUI binaries.

- Required compiler: `C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\bin\amd64\cl.exe`
- The build script (`scripts/build-stobe-sdk.ps1`) validates this and fails early if missing.

Install requirements:

1. Install Windows SDK 7.1 (`GRMSDKX_EN_DVD.iso`), running `setup\SdkSetup.exe`.
2. Install VS 2010 SP1 compiler update: `VC-Compiler-KB2519277.exe`.
3. Re-run `.\scripts\build-stobe-sdk.ps1`.

Notes:

- `build-stobe-sdk.ps1` is self-contained and uses `-Toolset v100` by default.
- Visual Studio 2022 can host the build, but the actual compiler toolset remains `v100`.

## What The Wrapper Does

`build-stobe-sdk.ps1` prepares a local locked SDK snapshot and builds Stobe
against that snapshot only. This prevents mixed include/lib/runtime inputs.

Snapshot root:

- `Stobe\vendor\stobe-sdk\Include`
- `Stobe\vendor\stobe-sdk\KenshiLib.lib`
- `Stobe\vendor\stobe-sdk\Libraries\mygui\MyGUIEngine_x64.lib`
- `Stobe\vendor\stobe-sdk\Libraries\ogre\OgreMain_x64.lib`
- `Stobe\vendor\stobe-sdk\Runtime\KenshiLib.dll`
- `Stobe\vendor\stobe-sdk\sdk-manifest.json`

## KenshiLib Revision

Stobe is compiled against the exact KenshiLib commit recorded in
`KENSHILIB_REVISION`. The SDK preparation step rejects a standalone KenshiLib
checkout at any other revision so newer headers cannot be paired with an older
runtime DLL or import library.
