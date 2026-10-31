[CmdletBinding()]
param(
    [string] $SetupPath,
    [switch] $KeepTemporary
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SetupPath)) {
    $SetupPath = Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'release') `
        -Filter '*-Setup.exe' -File |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if ([string]::IsNullOrWhiteSpace($SetupPath) -or
    !(Test-Path -LiteralPath $SetupPath -PathType Leaf)) {
    throw "Installer not found: $SetupPath"
}

$TemporaryRoot = Join-Path $env:TEMP "obs-airplay-installer-test-$([guid]::NewGuid().ToString('N'))"
$InstallDirectory = Join-Path $TemporaryRoot 'plugins/obs-airplay-cast'
$ProgressFile = Join-Path $TemporaryRoot 'progress.txt'
$StdoutFile = Join-Path $TemporaryRoot 'installer.stdout.txt'
$StderrFile = Join-Path $TemporaryRoot 'installer.stderr.txt'
$RegistryKey = "Software\obs-airplay-installer-test\$([guid]::NewGuid().ToString('N'))"
$EnvironmentNames = @(
    'OBS_AIRPLAY_SENDER_INSTALL_DIR'
    'OBS_AIRPLAY_SENDER_SKIP_ELEVATION'
    'OBS_AIRPLAY_SENDER_SKIP_PROCESS_CHECK'
    'OBS_AIRPLAY_SENDER_REGISTRY_HIVE'
    'OBS_AIRPLAY_SENDER_UNINSTALL_KEY'
    'OBS_AIRPLAY_SENDER_PROGRESS_FILE'
)
$PreviousEnvironment = @{}
foreach ($Name in $EnvironmentNames) {
    $PreviousEnvironment[$Name] = [Environment]::GetEnvironmentVariable($Name, 'Process')
}

try {
    New-Item -ItemType Directory -Path $TemporaryRoot -Force | Out-Null
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_INSTALL_DIR', $InstallDirectory, 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_SKIP_ELEVATION', '1', 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_SKIP_PROCESS_CHECK', '1', 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_REGISTRY_HIVE', 'HKCU', 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_UNINSTALL_KEY', $RegistryKey, 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_PROGRESS_FILE', $ProgressFile, 'Process')

    $StartInfo = New-Object System.Diagnostics.ProcessStartInfo
    $StartInfo.FileName = $SetupPath
    $StartInfo.Arguments = '--silent'
    $StartInfo.UseShellExecute = $false
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    $Process = [System.Diagnostics.Process]::Start($StartInfo)
    $Process.WaitForExit()
    $Process.StandardOutput.ReadToEnd() | Set-Content -LiteralPath $StdoutFile -Encoding UTF8
    $Process.StandardError.ReadToEnd() | Set-Content -LiteralPath $StderrFile -Encoding UTF8
    if ($Process.ExitCode -ne 0) {
        $ErrorText = if (Test-Path -LiteralPath $StderrFile) {
            Get-Content -LiteralPath $StderrFile -Raw
        } else {
            ''
        }
        throw "Silent installer exited with code $($Process.ExitCode): $ErrorText"
    }

    $RequiredFile = Join-Path $InstallDirectory 'bin/64bit/obs-airplay-cast.dll'
    if (!(Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "Installed payload is missing: $RequiredFile"
    }
    if (!(Test-Path -LiteralPath $ProgressFile -PathType Leaf)) {
        throw "Installer did not report progress"
    }

    $Updates = @()
    foreach ($Line in Get-Content -LiteralPath $ProgressFile) {
        if ($Line -notmatch '^(\d+)\t(.+)$') {
            throw "Invalid progress line: $Line"
        }
        $Updates += [pscustomobject]@{
            Percent = [int] $Matches[1]
            Message = [string] $Matches[2]
        }
    }
    if ($Updates.Count -lt 8) {
        throw "Expected multiple progress updates, got $($Updates.Count)"
    }
    if ($Updates[0].Percent -ne 0 -or $Updates[-1].Percent -ne 100) {
        throw "Progress did not run from 0 to 100"
    }
    for ($Index = 1; $Index -lt $Updates.Count; $Index++) {
        if ($Updates[$Index].Percent -lt $Updates[$Index - 1].Percent) {
            throw "Progress moved backwards at update $Index"
        }
    }

    foreach ($ExpectedPercent in @(10, 52, 58, 64, 80, 90, 100)) {
        if (@($Updates | Where-Object { $_.Percent -eq $ExpectedPercent }).Count -eq 0) {
            throw "Missing progress checkpoint $ExpectedPercent"
        }
    }

    Write-Host "Installer progress test passed: $($Updates.Count) updates, 0-100"
}
finally {
    foreach ($Name in $EnvironmentNames) {
        [Environment]::SetEnvironmentVariable($Name, $PreviousEnvironment[$Name], 'Process')
    }
    Remove-Item -LiteralPath "HKCU:\$RegistryKey" -Recurse -Force -ErrorAction SilentlyContinue
    if (!$KeepTemporary -and (Test-Path -LiteralPath $TemporaryRoot)) {
        Remove-Item -LiteralPath $TemporaryRoot -Recurse -Force
    }
    elseif ($KeepTemporary) {
        Write-Host "Temporary installer test data kept at: $TemporaryRoot"
    }
}
