param(
    [string]$TestFilter = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build-tests"

$cmakeCommand = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmakeCommand) {
    $vsCmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path -LiteralPath $vsCmake) {
        $cmakeCommand = $vsCmake
    }
}
if (-not $cmakeCommand) {
    throw "cmake was not found on PATH or in the Visual Studio bundled CMake location."
}
$ctestCommand = (Get-Command ctest -ErrorAction SilentlyContinue).Source
if (-not $ctestCommand) {
    $candidateCtest = Join-Path (Split-Path -Parent $cmakeCommand) "ctest.exe"
    if (Test-Path -LiteralPath $candidateCtest) {
        $ctestCommand = $candidateCtest
    }
}
if (-not $ctestCommand) {
    throw "ctest was not found on PATH or next to the selected cmake executable."
}

& $cmakeCommand -S (Join-Path $repoRoot "tests/cpp") -B $buildDir -G "Visual Studio 17 2022" -A x64
& $cmakeCommand --build $buildDir --config Release --parallel

if ($TestFilter) {
    & $ctestCommand --test-dir $buildDir -C Release --output-on-failure -R $TestFilter
} else {
    & $ctestCommand --test-dir $buildDir -C Release --output-on-failure
}
