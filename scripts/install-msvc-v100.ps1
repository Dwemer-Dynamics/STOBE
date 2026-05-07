# install-msvc-v100.ps1
# Descarga e instala silenciosamente el toolset MSVC v100 (VS 2010 x64).
#
# Componentes instalados:
#   1. Windows SDK 7.1       -> incluye cl.exe amd64, link.exe, libs
#   2. KB2519277             -> VS 2010 SP1 compiler update (correcciones al compilador)
#   3. CMake >= 3.20         -> si no esta instalado
#
# Uso:
#   .\scripts\install-msvc-v100.ps1
#   .\scripts\install-msvc-v100.ps1 -SkipCMake
#
# Requiere: ejecutar como Administrador en Windows Server 2022 / Windows 10+

param(
    [switch]$SkipCMake,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Write-Step([string]$msg) { Write-Host "`n[INSTALL] $msg" -ForegroundColor Cyan }
function Write-OK([string]$msg)   { Write-Host "  OK: $msg"  -ForegroundColor Green }
function Write-Warn([string]$msg) { Write-Host "  WARN: $msg" -ForegroundColor Yellow }
function Fail([string]$msg)       { Write-Host "  FAIL: $msg" -ForegroundColor Red; exit 1 }

# ── Verificar administrador ───────────────────────────────────────────────────
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail "Este script requiere permisos de Administrador."
}

$tmpDir = 'C:\stobe-install-tmp'
New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null

$v100Compiler = 'C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\bin\amd64\cl.exe'

# ── 1. Windows SDK 7.1 ───────────────────────────────────────────────────────
Write-Step "Windows SDK 7.1..."

if ((Test-Path $v100Compiler) -and -not $Force) {
    Write-OK "cl.exe ya presente, saltando instalacion del SDK."
} else {
    $sdkInstaller = Join-Path $tmpDir 'winsdk_web.exe'
    $sdkUrl = 'https://download.microsoft.com/download/A/6/A/A6AC035D-DA3F-4F0C-ADA4-37C8E5D34E3D/winsdk_web.exe'

    Write-Host "  Descargando Windows SDK 7.1..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $sdkUrl -OutFile $sdkInstaller -UseBasicParsing
    Write-OK "Descargado: $sdkInstaller"

    Write-Host "  Instalando (puede tardar 5-15 min)..."
    # /q = silencioso, /norestart, instalar solo componentes de compilacion x64
    $proc = Start-Process -FilePath $sdkInstaller `
        -ArgumentList '/q', '/norestart', `
                      '/features', 'OptionId.WindowsSDKFor64BitTools,OptionId.WindowsSDKHeadersLibsForDesktop' `
        -Wait -PassThru
    if ($proc.ExitCode -notin @(0, 3010)) {
        # 3010 = exito, requiere reinicio (no critico para compilacion)
        # Algunos instaladores devuelven 5638 en Server Core (package ya presente)
        if ($proc.ExitCode -eq 5638) {
            Write-Warn "ExitCode 5638 (componente ya instalado). Continuando."
        } else {
            Write-Warn "ExitCode inesperado: $($proc.ExitCode). Puede ser normal en Server 2022."
        }
    }
    Write-OK "Windows SDK 7.1 instalado (ExitCode: $($proc.ExitCode))"
}

# ── 2. VS 2010 SP1 Compiler Update (KB2519277) ───────────────────────────────
Write-Step "VS 2010 SP1 Compiler Update (KB2519277)..."

$kbInstaller = Join-Path $tmpDir 'VC-Compiler-KB2519277.exe'
$kbUrl = 'https://download.microsoft.com/download/4/3/7/43724B5E-57AB-4A7E-B786-8559AE6E60FF/VC-Compiler-KB2519277.exe'

if (-not (Test-Path $kbInstaller)) {
    Write-Host "  Descargando KB2519277..."
    Invoke-WebRequest -Uri $kbUrl -OutFile $kbInstaller -UseBasicParsing
    Write-OK "Descargado: $kbInstaller"
}

$proc = Start-Process -FilePath $kbInstaller -ArgumentList '/quiet', '/norestart' -Wait -PassThru
if ($proc.ExitCode -notin @(0, 3010, 1641)) {
    Write-Warn "ExitCode KB2519277: $($proc.ExitCode). Puede indicar que ya estaba aplicado."
} else {
    Write-OK "KB2519277 aplicado (ExitCode: $($proc.ExitCode))"
}

# ── 3. Verificar cl.exe ──────────────────────────────────────────────────────
Write-Step "Verificando compilador v100..."
if (Test-Path $v100Compiler) {
    $clVersion = & $v100Compiler 2>&1 | Select-String 'Version' | Select-Object -First 1
    Write-OK "cl.exe encontrado: $v100Compiler"
    Write-OK "Version: $clVersion"
} else {
    # En Windows Server el SDK puede instalarse en ruta ligeramente distinta
    $altPaths = @(
        'C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\bin\x86_amd64\cl.exe',
        'C:\Program Files\Microsoft SDKs\Windows\v7.1\Bin\x64\cl.exe'
    )
    $found = $altPaths | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($found) {
        Write-Warn "cl.exe en ruta alternativa: $found"
        Write-Warn "El build script espera la ruta estandar. Considera crear un symlink."
    } else {
        Write-Host "`n  POSIBLES CAUSAS:" -ForegroundColor Yellow
        Write-Host "  - Windows SDK 7.1 no soporta Windows Server 2022 directamente." -ForegroundColor Yellow
        Write-Host "  - En ese caso usa BuildTools de VS2010 via ISO offline." -ForegroundColor Yellow
        Fail "cl.exe no encontrado en ninguna ruta conocida."
    }
}

# ── 4. CMake ─────────────────────────────────────────────────────────────────
if (-not $SkipCMake) {
    Write-Step "Verificando CMake >= 3.20..."
    $cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmakeCmd) {
        $cmakeVer = (cmake --version 2>&1 | Select-String '(\d+\.\d+\.\d+)').Matches[0].Value
        Write-OK "CMake $cmakeVer ya instalado"
    } else {
        Write-Host "  Instalando CMake via winget..."
        $winget = Get-Command winget -ErrorAction SilentlyContinue
        if ($winget) {
            winget install --id Kitware.CMake --version 3.29.6 --silent `
                --accept-package-agreements --accept-source-agreements
            # Recargar PATH
            $env:PATH = [System.Environment]::GetEnvironmentVariable('PATH','Machine') + ';' + `
                        [System.Environment]::GetEnvironmentVariable('PATH','User')
            Write-OK "CMake instalado via winget"
        } else {
            # Fallback: descarga directa del MSI
            $cmakeMsi = Join-Path $tmpDir 'cmake-3.29.6-windows-x86_64.msi'
            $cmakeUrl = 'https://github.com/Kitware/CMake/releases/download/v3.29.6/cmake-3.29.6-windows-x86_64.msi'
            Write-Host "  winget no disponible. Descargando CMake MSI..."
            Invoke-WebRequest -Uri $cmakeUrl -OutFile $cmakeMsi -UseBasicParsing
            Start-Process -FilePath 'msiexec.exe' `
                -ArgumentList '/i', $cmakeMsi, '/quiet', '/norestart', 'ADD_CMAKE_TO_PATH=System' `
                -Wait
            Write-OK "CMake instalado via MSI"
        }
    }
}

# ── 5. Git ────────────────────────────────────────────────────────────────────
Write-Step "Verificando Git..."
$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if ($gitCmd) {
    Write-OK "Git: $(git --version)"
} else {
    Write-Host "  Instalando Git via winget..."
    winget install --id Git.Git --silent --accept-package-agreements --accept-source-agreements
    $env:PATH = [System.Environment]::GetEnvironmentVariable('PATH','Machine') + ';' + `
                [System.Environment]::GetEnvironmentVariable('PATH','User')
    Write-OK "Git instalado"
}

# ── Limpiar temporales ────────────────────────────────────────────────────────
Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "`n[INSTALL] Toolchain listo. Siguiente paso:" -ForegroundColor Green
Write-Host "  cd C:\Stobe" -ForegroundColor White
Write-Host "  .\scripts\setup-sdk.ps1" -ForegroundColor White
Write-Host "  .\scripts\setup-runner.ps1 -RepoUrl https://github.com/freddyeleazar/Stobe -Token <TOKEN>" -ForegroundColor White
