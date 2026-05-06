# setup-runner.ps1
# Instala y registra un GitHub Actions self-hosted runner Windows con el toolchain
# necesario para compilar Stobe (MSVC v100 / VS 2010 toolset).
#
# Prerrequisitos:
#   - Ejecutar como Administrador en Windows.
#   - Tener acceso a internet para descargar instaladores y el runner.
#
# Uso:
#   .\setup-runner.ps1 -RepoUrl "https://github.com/Dwemer-Dynamics/Stobe" -Token "RUNNER_TOKEN"
#
# El token se genera en: GitHub repo -> Settings -> Actions -> Runners -> New self-hosted runner

param(
    [Parameter(Mandatory=$true)]
    [string]$RepoUrl,

    [Parameter(Mandatory=$true)]
    [string]$Token,

    [string]$RunnerName    = "stobe-v100-runner",
    [string]$RunnerLabels  = "self-hosted,Windows,X64,stobe-v100",
    [string]$RunnerFolder  = "C:\actions-runner",
    [string]$RunnerVersion = "2.323.0"
)

$ErrorActionPreference = "Stop"

# ── Helper ──────────────────────────────────────────────────────────────────
function Write-Step($msg) { Write-Host "`n[SETUP] $msg" -ForegroundColor Cyan }
function Write-OK($msg)   { Write-Host "  OK: $msg"  -ForegroundColor Green }
function Write-Warn($msg) { Write-Host "  WARN: $msg" -ForegroundColor Yellow }

# ── 1. Validar que se ejecuta como administrador ─────────────────────────────
Write-Step "Verificando permisos de administrador..."
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Este script debe ejecutarse como Administrador."
}
Write-OK "Permisos correctos"

# ── 2. Verificar MSVC v100 ───────────────────────────────────────────────────
Write-Step "Verificando MSVC v100 (VS 2010 toolset)..."
$v100Path = "C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\bin\amd64\cl.exe"
if (-not (Test-Path $v100Path)) {
    Write-Warn "MSVC v100 NO encontrado en: $v100Path"
    Write-Host @"

  Para instalarlo:
  1. Descarga Windows SDK 7.1:
     https://www.microsoft.com/en-us/download/details.aspx?id=8279
     Ejecuta: setup\SdkSetup.exe

  2. Descarga VS 2010 SP1 Compiler Update:
     https://www.microsoft.com/en-us/download/details.aspx?id=4422
     Ejecuta: VC-Compiler-KB2519277.exe

  3. Vuelve a ejecutar este script.
"@ -ForegroundColor Yellow
    throw "MSVC v100 requerido. Instala los componentes indicados y vuelve a ejecutar."
}
Write-OK "MSVC v100 encontrado: $v100Path"

# ── 3. Verificar/Instalar CMake ──────────────────────────────────────────────
Write-Step "Verificando CMake >= 3.20..."
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCmd) {
    $cmakeVersion = (cmake --version 2>&1 | Select-String "(\d+\.\d+\.\d+)").Matches[0].Value
    Write-OK "CMake $cmakeVersion encontrado"
} else {
    Write-Warn "CMake no encontrado. Instalando via winget..."
    winget install --id Kitware.CMake --silent --accept-package-agreements --accept-source-agreements
    Write-OK "CMake instalado"
}

# ── 4. Descargar y configurar GitHub Actions Runner ──────────────────────────
Write-Step "Preparando carpeta del runner: $RunnerFolder"
New-Item -ItemType Directory -Path $RunnerFolder -Force | Out-Null

$runnerArchive = "$RunnerFolder\actions-runner-win-x64-$RunnerVersion.zip"
$runnerUrl = "https://github.com/actions/runner/releases/download/v$RunnerVersion/actions-runner-win-x64-$RunnerVersion.zip"

if (-not (Test-Path $runnerArchive)) {
    Write-Step "Descargando runner v$RunnerVersion..."
    Invoke-WebRequest -Uri $runnerUrl -OutFile $runnerArchive -UseBasicParsing
    Write-OK "Descarga completada: $runnerArchive"
} else {
    Write-OK "Archivo del runner ya existe, omitiendo descarga"
}

Write-Step "Extrayendo runner..."
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($runnerArchive, $RunnerFolder)
Write-OK "Extraído en $RunnerFolder"

# ── 5. Configurar el runner ───────────────────────────────────────────────────
Write-Step "Configurando runner..."
Push-Location $RunnerFolder
.\config.cmd `
    --url   $RepoUrl `
    --token $Token `
    --name  $RunnerName `
    --labels $RunnerLabels `
    --work  "_work" `
    --unattended
Pop-Location
Write-OK "Runner configurado con labels: $RunnerLabels"

# ── 6. Instalar como servicio de Windows ────────────────────────────────────
Write-Step "Instalando runner como servicio de Windows..."
Push-Location $RunnerFolder
.\svc.cmd install
.\svc.cmd start
Pop-Location
Write-OK "Servicio iniciado. El runner está listo."

# ── 7. Resumen final ─────────────────────────────────────────────────────────
Write-Host "`n══════════════════════════════════════════════════════" -ForegroundColor Green
Write-Host "  Runner instalado y en ejecucion" -ForegroundColor Green
Write-Host "  Nombre:   $RunnerName" -ForegroundColor Green
Write-Host "  Labels:   $RunnerLabels" -ForegroundColor Green
Write-Host "  Carpeta:  $RunnerFolder" -ForegroundColor Green
Write-Host "  Verifica en: $RepoUrl/settings/actions/runners" -ForegroundColor Green
Write-Host "══════════════════════════════════════════════════════`n" -ForegroundColor Green
