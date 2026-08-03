param(
    [ValidateSet("Status", "Enable", "Observe", "Idle", "Travel", "Reset", "Disable", "Analyze")]
    [string]$Action = "Status",
    [string]$KenshiPath = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [uint32]$TargetSerial = 0,
    [double]$TravelX,
    [double]$TravelY,
    [double]$TravelZ
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class StobePhase0Ini
{
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi)]
    public static extern uint GetPrivateProfileString(
        string section, string key, string defaultValue,
        StringBuilder result, uint size, string filePath);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi)]
    public static extern bool WritePrivateProfileString(
        string section, string key, string value, string filePath);
}
"@

$section = "AutonomySafetyProbe"
$installedModPath = Join-Path $KenshiPath "mods\STOBE"
$reKenshiModPath = Join-Path $KenshiPath "RE_Kenshi\mods\Stobe"
$runtimeModPath = if (Test-Path -LiteralPath (Join-Path $KenshiPath "RE_Kenshi")) {
    $reKenshiModPath
} else {
    $installedModPath
}
$baseIniCandidates = @(
    (Join-Path $runtimeModPath "Stobe.ini"),
    (Join-Path $installedModPath "Stobe.ini")
)
$baseIni = $baseIniCandidates |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
$customIni = Join-Path $runtimeModPath "StobeCustom.ini"
$logPath = Join-Path $runtimeModPath "stobe.log"

if (-not $baseIni) {
    throw "Stobe.ini was not found in the runtime or installed mod directories. Build and deploy Stobe first."
}

function Read-IniValue {
    param(
        [string]$Key,
        [string]$DefaultValue
    )

    $baseBuffer = New-Object System.Text.StringBuilder 512
    [void][StobePhase0Ini]::GetPrivateProfileString(
        $section, $Key, $DefaultValue, $baseBuffer, 512, $baseIni)

    if (-not (Test-Path -LiteralPath $customIni)) {
        return $baseBuffer.ToString()
    }

    $customBuffer = New-Object System.Text.StringBuilder 512
    [void][StobePhase0Ini]::GetPrivateProfileString(
        $section, $Key, $baseBuffer.ToString(), $customBuffer, 512, $customIni)
    return $customBuffer.ToString()
}

function Write-IniValue {
    param(
        [string]$Key,
        [string]$Value
    )

    if (-not (Test-Path -LiteralPath $customIni)) {
        [void](New-Item -ItemType Directory -Path $runtimeModPath -Force)
        [void](New-Item -ItemType File -Path $customIni -Force)
    }
    if (-not [StobePhase0Ini]::WritePrivateProfileString(
            $section, $Key, $Value, $customIni)) {
        throw "Failed to write [$section] $Key to $customIni"
    }
}

function Get-ProbeStatus {
    [pscustomobject]@{
        Enabled = Read-IniValue "Enabled" "0"
        TargetSerial = Read-IniValue "TargetSerial" "0"
        TelemetryIntervalMs = Read-IniValue "TelemetryIntervalMs" "2000"
        Command = Read-IniValue "Command" "OBSERVE"
        CommandNonce = Read-IniValue "CommandNonce" "0"
        TravelDestinationSet = Read-IniValue "TravelDestinationSet" "0"
        TravelX = Read-IniValue "TravelX" "0"
        TravelY = Read-IniValue "TravelY" "0"
        TravelZ = Read-IniValue "TravelZ" "0"
        GameRunning = [bool](Get-Process -Name "kenshi_x64" -ErrorAction SilentlyContinue)
        LogExists = Test-Path -LiteralPath $logPath
        RuntimeModPath = $runtimeModPath
        BaseIni = $baseIni
        CustomIni = $customIni
    }
}

function Invoke-ProbeCommand {
    param([string]$Command)

    $currentNonceText = Read-IniValue "CommandNonce" "0"
    [uint32]$currentNonce = 0
    if (-not [uint32]::TryParse($currentNonceText, [ref]$currentNonce)) {
        throw "CommandNonce is not an unsigned integer: $currentNonceText"
    }
    if ($currentNonce -eq [uint32]::MaxValue) {
        throw "CommandNonce reached its maximum value. Set it to 0 while Kenshi is not running."
    }

    Write-IniValue "Command" $Command
    Write-IniValue "CommandNonce" ([string]($currentNonce + 1))
    Write-Host "Queued one-shot $Command with nonce $($currentNonce + 1)." -ForegroundColor Cyan
}

function Show-Analysis {
    if (-not (Test-Path -LiteralPath $logPath)) {
        Write-Host "No stobe.log exists yet. Start Kenshi and load a save." -ForegroundColor Yellow
        return
    }

    $lines = Get-Content -LiteralPath $logPath
    $probeLines = @($lines | Select-String -Pattern "AUTONOMY_SPIKE")
    $bindings = @($lines | Select-String -Pattern "AUTONOMY_SPIKE: target bound")
    $states = @($lines | Select-String -Pattern "AUTONOMY_SPIKE_STATE:")
    $commands = @($lines | Select-String -Pattern "AUTONOMY_SPIKE_COMMAND:")
    $violations = @($lines | Select-String -Pattern "AUTONOMY_SPIKE_INVARIANT:" |
        Where-Object { $_.Line -notmatch "order watch complete" })
    $completedWatches = @($lines | Select-String -Pattern "order watch complete")
    $issuedCommands = @($commands | Where-Object { $_.Line -match "issued=1" })
    $preservedCommands = @($commands | Where-Object {
            $_.Line -match "issued=1" -and
            $_.Line -match "jobs_preserved=1" -and
            $_.Line -match "other_player_orders_preserved=1"
        })

    $serials = @($bindings | ForEach-Object {
            if ($_.Line -match "serial=([0-9]+)") { $Matches[1] }
        } | Sort-Object -Unique)

    [pscustomobject]@{
        ProbeLogLines = $probeLines.Count
        BoundSerials = if ($serials.Count -gt 0) { $serials -join "," } else { "none" }
        StateSnapshots = $states.Count
        CommandRecords = $commands.Count
        IssuedCommands = $issuedCommands.Count
        PreservedDispatches = $preservedCommands.Count
        CompletedOrderWatches = $completedWatches.Count
        InvariantViolations = $violations.Count
        BaselineTelemetryPass = ($bindings.Count -gt 0 -and $states.Count -gt 0)
        ImmediateInvariantPass = ($issuedCommands.Count -gt 0 -and
            $issuedCommands.Count -eq $preservedCommands.Count -and
            $violations.Count -eq 0)
    } | Format-List

    if ($violations.Count -gt 0) {
        Write-Host "Invariant violations:" -ForegroundColor Red
        $violations | ForEach-Object { Write-Host $_.Line -ForegroundColor Red }
    }

    Write-Host "Recent probe records:" -ForegroundColor DarkCyan
    $probeLines | Select-Object -Last 30 | ForEach-Object { Write-Host $_.Line }
}

switch ($Action) {
    "Status" {
        Get-ProbeStatus | Format-List
    }
    "Enable" {
        Write-IniValue "TargetSerial" ([string]$TargetSerial)
        Write-IniValue "Enabled" "1"
        Write-Host "Probe enabled. TargetSerial=$TargetSerial." -ForegroundColor Cyan
        Write-Host "When TargetSerial is 0, select the intended player character before loading completes." -ForegroundColor Yellow
        Get-ProbeStatus | Format-List
    }
    "Observe" {
        Invoke-ProbeCommand "OBSERVE"
    }
    "Idle" {
        Invoke-ProbeCommand "IDLE"
    }
    "Travel" {
        $provided = $PSBoundParameters.ContainsKey("TravelX") -and
            $PSBoundParameters.ContainsKey("TravelY") -and
            $PSBoundParameters.ContainsKey("TravelZ")
        if (-not $provided) {
            throw "Travel requires -TravelX, -TravelY, and -TravelZ."
        }
        Write-IniValue "TravelX" ([string]::Format(
                [Globalization.CultureInfo]::InvariantCulture, "{0:R}", $TravelX))
        Write-IniValue "TravelY" ([string]::Format(
                [Globalization.CultureInfo]::InvariantCulture, "{0:R}", $TravelY))
        Write-IniValue "TravelZ" ([string]::Format(
                [Globalization.CultureInfo]::InvariantCulture, "{0:R}", $TravelZ))
        Write-IniValue "TravelDestinationSet" "1"
        Invoke-ProbeCommand "TRAVEL"
    }
    "Reset" {
        Invoke-ProbeCommand "RESET"
    }
    "Disable" {
        Write-IniValue "Enabled" "0"
        Write-IniValue "TravelDestinationSet" "0"
        Write-Host "Probe disabled." -ForegroundColor Green
    }
    "Analyze" {
        Show-Analysis
    }
}
