param(
  [ValidateSet('normal','ui_only','no_hook','hook_no_orig','no_context','no_hook_no_context')]
  [string]$DiagProfile = 'normal',
  [string]$Toolset = 'v100',
  [string]$Generator = 'Visual Studio 17 2022',
  [switch]$Deploy,
  [string]$BuildDir = 'build',
  [string]$KenshiModDir = ''
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
  Write-Error $Message
  exit 1
}

function Require-Path([string]$Path, [string]$Label) {
  if (-not (Test-Path $Path)) {
    Fail "$Label not found: $Path"
  }
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $repoRoot

Write-Host "[INFO] Repo root: $repoRoot"
Write-Host "[INFO] Profile:   $DiagProfile"
Write-Host "[INFO] Toolset:   $Toolset"

if ($Toolset -eq 'v100') {
  $clPath = 'C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\bin\amd64\cl.exe'
  if (-not (Test-Path $clPath)) {
    Fail "MSVC v100 not found at: $clPath"
  }
  Write-Host "[OK] MSVC v100 found"
}

$sdkRoot = Join-Path $repoRoot 'vendor\stobe-sdk'
$includeDir = Join-Path $sdkRoot 'Include'
$kenshiLib = Join-Path $sdkRoot 'KenshiLib.lib'
$myguiLib = Join-Path $sdkRoot 'Libraries\mygui\MyGUIEngine_x64.lib'
$ogreLib = Join-Path $sdkRoot 'Libraries\ogre\OgreMain_x64.lib'
$runtimeDll = Join-Path $sdkRoot 'Runtime\KenshiLib.dll'

Require-Path $sdkRoot 'SDK root'
Require-Path $includeDir 'SDK include dir'
Require-Path $kenshiLib 'KenshiLib.lib'
Require-Path $myguiLib 'MyGUIEngine_x64.lib'
Require-Path $ogreLib 'OgreMain_x64.lib'
Require-Path $runtimeDll 'KenshiLib.dll runtime'

Write-Host "[OK] Locked SDK snapshot detected"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  Fail 'cmake not found in PATH'
}

$buildPath = Join-Path $repoRoot $BuildDir
if (-not (Test-Path $buildPath)) {
  New-Item -ItemType Directory -Path $buildPath | Out-Null
}

$cmakeArgs = @(
  '-G', $Generator
)
if ($Generator -like 'Visual Studio*') {
  $cmakeArgs += @('-A', 'x64', '-T', $Toolset)
} elseif ($Generator -eq 'Ninja') {
  $cmakeArgs += @(
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_CXX_COMPILER=$clPath"
  )
}
$cmakeArgs += @(
  '-S', $repoRoot,
  '-B', $buildPath,
  "-DKENSHI_LIB_INCLUDE_DIR=$includeDir",
  "-DKENSHI_LIB_LIBRARY=$kenshiLib",
  "-DBOOST_INCLUDE_DIR=$includeDir",
  "-DSTOBE_DIAG_PROFILE=$DiagProfile"
)

Write-Host "[INFO] Configuring CMake..."
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
  Fail 'CMake configure failed'
}

Write-Host "[INFO] Building Release..."
& cmake --build $buildPath --config Release
if ($LASTEXITCODE -ne 0) {
  Fail 'Build failed'
}

$dllCandidates = @(
  (Join-Path $buildPath 'Release\Stobe.dll'),
  (Join-Path $buildPath 'Stobe.dll')
)
$dll = $null
foreach ($candidate in $dllCandidates) {
  if (Test-Path $candidate) {
    $dll = $candidate
    break
  }
}
if (-not $dll) {
  $dll = (Get-ChildItem -Path $buildPath -Recurse -Filter 'Stobe.dll' | Select-Object -First 1).FullName
}
if (-not $dll) {
  Fail 'Stobe.dll not found after build'
}

Write-Host "[OK] Built DLL: $dll"

if ($Deploy) {
  if ([string]::IsNullOrWhiteSpace($KenshiModDir)) {
    $KenshiModDir = 'C:\Program Files (x86)\Steam\steamapps\common\Kenshi\mods\Stobe'
  }
  if (-not (Test-Path $KenshiModDir)) {
    Fail "Kenshi mod dir not found: $KenshiModDir"
  }

  Write-Host "[INFO] Deploying to: $KenshiModDir"
  Copy-Item $dll (Join-Path $KenshiModDir 'Stobe.dll') -Force
  if (Test-Path (Join-Path $repoRoot 'mod')) {
    Copy-Item (Join-Path $repoRoot 'mod\*') $KenshiModDir -Recurse -Force
  }
  Write-Host '[OK] Deploy complete'
}

Write-Host '[DONE] build-stobe-sdk.ps1 finished successfully'
