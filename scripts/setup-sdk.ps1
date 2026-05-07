# setup-sdk.ps1
# Descarga y organiza el SDK de desarrollo necesario para compilar Stobe.
#
# Obtiene automáticamente:
#   - KenshiLib.lib + headers  (GitHub release KenshiReclaimer/KenshiLib)
#   - MyGUIEngine_x64.lib      (GitHub repo KenshiReclaimer/KenshiLib/Libraries/mygui)
#   - OgreMain_x64.lib         (GitHub repo KenshiReclaimer/KenshiLib/Libraries/ogre)
#   - KenshiLib.dll runtime    (GitHub release KenshiReclaimer/KenshiLib)
#
# Las DLLs de MyGUI y Ogre se copian desde la instalación local de Kenshi si
# se especifica -KenshiDir, o se omiten (solo se necesitan en runtime del juego).
#
# Estructura resultante en vendor\stobe-sdk\:
#   Include\                   <- headers de KenshiLib
#   KenshiLib.lib
#   KenshiLib.dll
#   Libraries\mygui\MyGUIEngine_x64.lib
#   Libraries\ogre\OgreMain_x64.lib
#   Runtime\KenshiLib.dll      <- alias del runtime (esperado por build-stobe-sdk.ps1)
#
# Uso:
#   .\scripts\setup-sdk.ps1
#   .\scripts\setup-sdk.ps1 -KenshiDir "C:\Juegos\Kenshi"
#   .\scripts\setup-sdk.ps1 -KenshiLibVersion v0.2.1 -Force

param(
    [string]$KenshiLibVersion = 'v0.2.1',
    [string]$KenshiDir        = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Write-Step([string]$msg) { Write-Host "`n[SDK] $msg" -ForegroundColor Cyan }
function Write-OK([string]$msg)   { Write-Host "  OK: $msg"  -ForegroundColor Green }
function Write-Warn([string]$msg) { Write-Host "  WARN: $msg" -ForegroundColor Yellow }
function Fail([string]$msg)       { Write-Host "  FAIL: $msg" -ForegroundColor Red; exit 1 }

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$sdkRoot  = Join-Path $repoRoot 'vendor\stobe-sdk'
$tmpDir   = Join-Path $env:TEMP 'stobe-sdk-setup'

# ── Verificar si el SDK ya existe ────────────────────────────────────────────
if ((Test-Path $sdkRoot) -and -not $Force) {
    $existing = @(
        (Join-Path $sdkRoot 'KenshiLib.lib'),
        (Join-Path $sdkRoot 'Include'),
        (Join-Path $sdkRoot 'Libraries\mygui\MyGUIEngine_x64.lib'),
        (Join-Path $sdkRoot 'Libraries\ogre\OgreMain_x64.lib'),
        (Join-Path $sdkRoot 'Runtime\KenshiLib.dll')
    )
    if ($existing | Where-Object { Test-Path $_ } | Measure-Object | Select-Object -ExpandProperty Count | ForEach-Object { $_ -eq $existing.Count }) {
        Write-Host "[SDK] SDK ya esta completo en: $sdkRoot" -ForegroundColor Green
        Write-Host "      Usa -Force para reinstalar."
        exit 0
    }
}

Write-Step "Configurando SDK en: $sdkRoot"
Write-Host "  KenshiLib version: $KenshiLibVersion"

# ── Preparar directorios ──────────────────────────────────────────────────────
New-Item -ItemType Directory -Path $tmpDir   -Force | Out-Null
New-Item -ItemType Directory -Path $sdkRoot  -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $sdkRoot 'Libraries\mygui') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $sdkRoot 'Libraries\ogre')  -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $sdkRoot 'Runtime')         -Force | Out-Null

# ── 1. Descargar release de KenshiLib ────────────────────────────────────────
Write-Step "Descargando KenshiLib release $KenshiLibVersion..."
$releaseZip = Join-Path $tmpDir "KenshiLib_$KenshiLibVersion.zip"
$releaseUrl = "https://github.com/KenshiReclaimer/KenshiLib/releases/download/$KenshiLibVersion/KenshiLib_$KenshiLibVersion.zip"

if (-not (Test-Path $releaseZip)) {
    Write-Host "  -> $releaseUrl"
    Invoke-WebRequest -Uri $releaseUrl -OutFile $releaseZip -UseBasicParsing
}

$releaseExtract = Join-Path $tmpDir "KenshiLib_release"
if (Test-Path $releaseExtract) { Remove-Item $releaseExtract -Recurse -Force }
Expand-Archive -Path $releaseZip -DestinationPath $releaseExtract -Force
Write-OK "Release extraido en: $releaseExtract"

# Copiar KenshiLib.lib y KenshiLib.dll al SDK root
$libSrc = Join-Path $releaseExtract 'KenshiLib.lib'
$dllSrc = Join-Path $releaseExtract 'KenshiLib.dll'

if (-not (Test-Path $libSrc)) { Fail "KenshiLib.lib no encontrado en el release ZIP" }
if (-not (Test-Path $dllSrc)) { Fail "KenshiLib.dll no encontrado en el release ZIP" }

Copy-Item $libSrc -Destination (Join-Path $sdkRoot 'KenshiLib.lib') -Force
Copy-Item $dllSrc -Destination (Join-Path $sdkRoot 'KenshiLib.dll') -Force
Copy-Item $dllSrc -Destination (Join-Path $sdkRoot 'Runtime\KenshiLib.dll') -Force
Write-OK "KenshiLib.lib y KenshiLib.dll copiados"

# ── 2. Descargar headers del repo KenshiLib ───────────────────────────────────
# ── 2. Descargar headers del fork BFrizzleFoShizzle/KenshiLib (rama RE_Kenshi_mods)
#       Este fork tiene los headers completos: kenshi/Enums.h, RootObject.h, etc.
Write-Step "Descargando headers de KenshiLib (BFrizzleFoShizzle/RE_Kenshi_mods)..."
$repoZip     = Join-Path $tmpDir 'KenshiLib_repo.zip'
$repoUrl     = 'https://github.com/BFrizzleFoShizzle/KenshiLib/archive/refs/heads/RE_Kenshi_mods.zip'
$repoExtract = Join-Path $tmpDir 'KenshiLib_repo'

if (-not (Test-Path $repoZip)) {
    Write-Host "  -> $repoUrl"
    Invoke-WebRequest -Uri $repoUrl -OutFile $repoZip -UseBasicParsing
}

if (Test-Path $repoExtract) { Remove-Item $repoExtract -Recurse -Force }
Expand-Archive -Path $repoZip -DestinationPath $repoExtract -Force

# El ZIP extrae como KenshiLib-RE_Kenshi_mods/
$repoInner = Get-ChildItem $repoExtract -Directory | Select-Object -First 1

$includeSrc = Join-Path $repoInner.FullName 'Include'
if (-not (Test-Path $includeSrc)) { Fail "Carpeta Include no encontrada en el repo ZIP" }

$includeDst = Join-Path $sdkRoot 'Include'
if (Test-Path $includeDst) { Remove-Item $includeDst -Recurse -Force }
Copy-Item $includeSrc -Destination $includeDst -Recurse -Force
Write-OK "Headers copiados a: $includeDst"

# ── 2b. Descargar Boost 1.60 headers (requerido por kenshi/GameData.h y otros) ─
Write-Step "Descargando Boost 1.60.0 headers..."
$boostDst = Join-Path $sdkRoot 'boost_1_60_0'
$boostZip = Join-Path $tmpDir 'boost_1_60_0.zip'
$boostUrl = 'https://sourceforge.net/projects/boost/files/boost/1.60.0/boost_1_60_0.zip/download'

if (-not (Test-Path $boostZip)) {
    Write-Host "  -> $boostUrl"
    Invoke-WebRequest -Uri $boostUrl -OutFile $boostZip -UseBasicParsing
}

$boostExtract = Join-Path $tmpDir 'boost_extract'
if (Test-Path $boostExtract) { Remove-Item $boostExtract -Recurse -Force }
Write-Host "  Extrayendo Boost (puede tardar)..."
Expand-Archive -Path $boostZip -DestinationPath $boostExtract -Force

$boostInner = Get-ChildItem $boostExtract -Directory | Select-Object -First 1
if (-not $boostInner) { Fail "No se encontro directorio raiz de Boost despues de extraer" }

if (Test-Path $boostDst) { Remove-Item $boostDst -Recurse -Force }
Move-Item $boostInner.FullName $boostDst -Force
Write-OK "Boost headers en: $boostDst"

# ── 3. Descargar MyGUIEngine_x64.lib y OgreMain_x64.lib del repo ──────────────
Write-Step "Descargando MyGUIEngine_x64.lib desde repo KenshiLib..."
$myguiUrl = "https://github.com/KenshiReclaimer/KenshiLib/raw/master/Libraries/mygui/MyGUIEngine_x64.lib"
$myguiDst = Join-Path $sdkRoot 'Libraries\mygui\MyGUIEngine_x64.lib'
Invoke-WebRequest -Uri $myguiUrl -OutFile $myguiDst -UseBasicParsing
Write-OK "MyGUIEngine_x64.lib descargado"

Write-Step "Descargando OgreMain_x64.lib desde repo KenshiLib..."
$ogreUrl = "https://github.com/KenshiReclaimer/KenshiLib/raw/master/Libraries/ogre/OgreMain_x64.lib"
$ogreDst = Join-Path $sdkRoot 'Libraries\ogre\OgreMain_x64.lib'
Invoke-WebRequest -Uri $ogreUrl -OutFile $ogreDst -UseBasicParsing
Write-OK "OgreMain_x64.lib descargado"

# ── 4. Copiar DLLs runtime desde instalacion local de Kenshi (opcional) ───────
if ($KenshiDir -ne '') {
    Write-Step "Copiando DLLs runtime desde instalacion de Kenshi: $KenshiDir"
    $runtimeDir = Join-Path $sdkRoot 'Runtime'

    $runtimeFiles = @(
        'MyGUIEngine_x64.dll',
        'OgreMain_x64.dll',
        'OgreMeshLodGenerator_x64.dll',
        'OgreOverlay_x64.dll'
    )

    foreach ($dll in $runtimeFiles) {
        $src = Join-Path $KenshiDir $dll
        if (Test-Path $src) {
            Copy-Item $src -Destination (Join-Path $runtimeDir $dll) -Force
            Write-OK "Copiado: $dll"
        } else {
            Write-Warn "No encontrado (no critico): $dll en $KenshiDir"
        }
    }
} else {
    Write-Warn "KenshiDir no especificado: DLLs de MyGUI/Ogre no copiadas."
    Write-Warn "Solo son necesarias si usas -Deploy para instalar en Kenshi."
    Write-Warn "Para incluirlas: .\scripts\setup-sdk.ps1 -KenshiDir 'C:\Juegos\Kenshi'"
}

# ── 5. Verificar estructura final ─────────────────────────────────────────────
Write-Step "Verificando estructura del SDK..."
$required = @{
    'KenshiLib.lib'                        = Join-Path $sdkRoot 'KenshiLib.lib'
    'KenshiLib.dll (runtime)'              = Join-Path $sdkRoot 'Runtime\KenshiLib.dll'
    'Include/'                             = Join-Path $sdkRoot 'Include'
    'Libraries\mygui\MyGUIEngine_x64.lib'  = Join-Path $sdkRoot 'Libraries\mygui\MyGUIEngine_x64.lib'
    'Libraries\ogre\OgreMain_x64.lib'      = Join-Path $sdkRoot 'Libraries\ogre\OgreMain_x64.lib'
    'boost_1_60_0/'                        = Join-Path $sdkRoot 'boost_1_60_0'
}

$allOk = $true
foreach ($entry in $required.GetEnumerator()) {
    if (Test-Path $entry.Value) {
        Write-OK $entry.Key
    } else {
        Write-Host "  MISSING: $($entry.Key) -> $($entry.Value)" -ForegroundColor Red
        $allOk = $false
    }
}

if (-not $allOk) {
    Fail "SDK incompleto. Revisa los errores anteriores."
}

# ── Limpiar temporales ────────────────────────────────────────────────────────
Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "`n[SDK] SDK listo. Ahora puedes compilar con:" -ForegroundColor Green
Write-Host "      .\scripts\build-stobe-sdk.ps1" -ForegroundColor White
