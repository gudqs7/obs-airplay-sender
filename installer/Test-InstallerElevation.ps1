[CmdletBinding()]
param(
    [string] $SetupPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class InstallerFocusNative
{
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool AllowSetForegroundWindow(int processId);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder text, int maxCount);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr SendMessageTimeout(
        IntPtr hWnd,
        uint message,
        IntPtr wParam,
        IntPtr lParam,
        uint flags,
        uint timeout,
        out IntPtr result);
}
"@

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

$TemporaryRoot = Join-Path $env:TEMP "obs-airplay-installer-focus-test-$([guid]::NewGuid().ToString('N'))"
$InstallDirectory = Join-Path $TemporaryRoot 'plugins/obs-airplay-cast'
$RegistryKey = "Software\obs-airplay-installer-focus-test\$([guid]::NewGuid().ToString('N'))"
$EnvironmentNames = @(
    'OBS_AIRPLAY_SENDER_INSTALL_DIR'
    'OBS_AIRPLAY_SENDER_SKIP_PROCESS_CHECK'
    'OBS_AIRPLAY_SENDER_REGISTRY_HIVE'
    'OBS_AIRPLAY_SENDER_UNINSTALL_KEY'
    'OBS_AIRPLAY_SENDER_TEST_RELAUNCH'
)
$PreviousEnvironment = @{}
foreach ($Name in $EnvironmentNames) {
    $PreviousEnvironment[$Name] = [Environment]::GetEnvironmentVariable($Name, 'Process')
}

$Parent = $null
$ChildProcessId = [uint32] 0
$ForegroundForm = $null
try {
    New-Item -ItemType Directory -Path $TemporaryRoot -Force | Out-Null
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_INSTALL_DIR', $InstallDirectory, 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_SKIP_PROCESS_CHECK', '1', 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_REGISTRY_HIVE', 'HKCU', 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_UNINSTALL_KEY', $RegistryKey, 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_TEST_RELAUNCH', '1', 'Process')

    # Give the test process a real foreground window before launching the
    # unelevated parent. The parent then transfers that right to the simulated
    # elevated child, matching the UAC handoff without automating the secure
    # desktop.
    $ForegroundForm = New-Object System.Windows.Forms.Form
    $ForegroundForm.Text = 'OBS AirPlay installer focus test'
    $ForegroundForm.ShowInTaskbar = $false
    $ForegroundForm.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::None
    $ForegroundForm.Opacity = 0.01
    $ForegroundForm.Size = New-Object System.Drawing.Size(1, 1)
    $ForegroundForm.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
    $ForegroundForm.Location = New-Object System.Drawing.Point(-10000, -10000)
    $ForegroundForm.Show()
    $ForegroundForm.Activate()
    $null = [InstallerFocusNative]::SetForegroundWindow($ForegroundForm.Handle)
    [System.Windows.Forms.Application]::DoEvents()
    $null = [InstallerFocusNative]::AllowSetForegroundWindow(-1)
    $StartInfo = New-Object System.Diagnostics.ProcessStartInfo
    $StartInfo.FileName = $SetupPath
    $StartInfo.UseShellExecute = $false
    $Parent = [System.Diagnostics.Process]::Start($StartInfo)

    $TitleCondition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty,
        'OBS AirPlay Sender 安装程序')
    $TypeCondition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Window)
    $WindowCondition = New-Object System.Windows.Automation.AndCondition(
        $TitleCondition,
        $TypeCondition)

    $Window = $null
    $Deadline = [DateTime]::UtcNow.AddSeconds(12)
    while ($null -eq $Window -and [DateTime]::UtcNow -lt $Deadline) {
        $Candidate = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Children,
            $WindowCondition)
        if ($null -ne $Candidate) {
            $CandidateProcessId = [uint32] $Candidate.Current.ProcessId
            if ($CandidateProcessId -ne $Parent.Id -and
                $Candidate.Current.NativeWindowHandle -ne 0) {
                $Window = $Candidate
                $ChildProcessId = $CandidateProcessId
            }
        }
        if ($null -eq $Window) {
            Start-Sleep -Milliseconds 50
        }
    }
    if ($null -eq $Window) {
        throw "Elevated installer child window did not appear"
    }

    $WindowHandle = [IntPtr] $Window.Current.NativeWindowHandle
    $Deadline = [DateTime]::UtcNow.AddSeconds(3)
    while ([InstallerFocusNative]::GetForegroundWindow() -ne $WindowHandle -and
        [DateTime]::UtcNow -lt $Deadline) {
        Start-Sleep -Milliseconds 50
    }
    if ([InstallerFocusNative]::GetForegroundWindow() -ne $WindowHandle) {
        $ForegroundHandle = [InstallerFocusNative]::GetForegroundWindow()
        $ForegroundProcessId = [uint32] 0
        $null = [InstallerFocusNative]::GetWindowThreadProcessId(
            $ForegroundHandle,
            [ref] $ForegroundProcessId)
        $ForegroundTitle = New-Object System.Text.StringBuilder 256
        $null = [InstallerFocusNative]::GetWindowText(
            $ForegroundHandle,
            $ForegroundTitle,
            $ForegroundTitle.Capacity)
        throw "Relaunched installer window did not become foreground after the elevation handoff; actual=$ForegroundHandle pid=$ForegroundProcessId title='$ForegroundTitle' expected=$WindowHandle pid=$ChildProcessId"
    }

    $MessageResult = [IntPtr]::Zero
    $Responsive = [InstallerFocusNative]::SendMessageTimeout(
        $WindowHandle,
        0,
        [IntPtr]::Zero,
        [IntPtr]::Zero,
        0x0002,
        1000,
        [ref] $MessageResult)
    if ($Responsive -eq [IntPtr]::Zero) {
        throw "Relaunched installer window is not responsive"
    }

    $null = [InstallerFocusNative]::SendMessageTimeout(
        $WindowHandle,
        0x0010,
        [IntPtr]::Zero,
        [IntPtr]::Zero,
        0x0002,
        2000,
        [ref] $MessageResult)
    if (!$Parent.WaitForExit(5000)) {
        throw "Elevation parent did not exit after the child installer closed"
    }

    Write-Host "Installer elevation focus test passed: child PID $ChildProcessId acquired foreground"
}
finally {
    if ($null -ne $ForegroundForm) {
        $ForegroundForm.Close()
        $ForegroundForm.Dispose()
    }
    if ($ChildProcessId -ne 0) {
        Stop-Process -Id $ChildProcessId -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $Parent -and !$Parent.HasExited) {
        Stop-Process -Id $Parent.Id -Force -ErrorAction SilentlyContinue
    }
    foreach ($Name in $EnvironmentNames) {
        [Environment]::SetEnvironmentVariable($Name, $PreviousEnvironment[$Name], 'Process')
    }
    Remove-Item -LiteralPath "HKCU:\$RegistryKey" -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $TemporaryRoot) {
        Remove-Item -LiteralPath $TemporaryRoot -Recurse -Force
    }
}
