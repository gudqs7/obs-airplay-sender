[CmdletBinding()]
param(
    [string] $SetupPath,
    [switch] $UseInstalledObs
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class InstallerUiNative
{
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr SendMessageTimeout(
        IntPtr hWnd,
        uint message,
        IntPtr wParam,
        IntPtr lParam,
        uint flags,
        uint timeout,
        out IntPtr result);

    [DllImport("user32.dll")]
    public static extern IntPtr FindWindowEx(
        IntPtr parent,
        IntPtr childAfter,
        string className,
        string windowName);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
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

$TemporaryRoot = Join-Path $env:TEMP "obs-airplay-installer-ui-test-$([guid]::NewGuid().ToString('N'))"
$InstallDirectory = Join-Path $TemporaryRoot 'plugins/obs-airplay-cast'
$FakeObsPath = Join-Path $TemporaryRoot 'fake-obs64.exe'
$FakeObsSource = Join-Path $TemporaryRoot 'fake-obs64.cs'
$RegistryKey = "Software\obs-airplay-installer-ui-test\$([guid]::NewGuid().ToString('N'))"
$EnvironmentNames = @(
    'OBS_AIRPLAY_SENDER_INSTALL_DIR'
    'OBS_AIRPLAY_SENDER_SKIP_ELEVATION'
    'OBS_AIRPLAY_SENDER_SKIP_PROCESS_CHECK'
    'OBS_AIRPLAY_SENDER_REGISTRY_HIVE'
    'OBS_AIRPLAY_SENDER_UNINSTALL_KEY'
    'OBS_AIRPLAY_SENDER_PROGRESS_DELAY_MS'
    'OBS_AIRPLAY_SENDER_OBS_PATH'
    'OBS_AIRPLAY_SENDER_LAUNCH_DELAY_MS'
)
$PreviousEnvironment = @{}
foreach ($Name in $EnvironmentNames) {
    $PreviousEnvironment[$Name] = [Environment]::GetEnvironmentVariable($Name, 'Process')
}

function Find-Control {
    param(
        [System.Windows.Automation.AutomationElement] $Root,
        [string] $Name,
        [System.Windows.Automation.ControlType] $ControlType
    )
    $NameCondition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty,
        $Name)
    $TypeCondition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        $ControlType)
    $Condition = New-Object System.Windows.Automation.AndCondition($NameCondition, $TypeCondition)
    return $Root.FindFirst(
        [System.Windows.Automation.TreeScope]::Descendants,
        $Condition)
}

$Process = $null
$ObsProcessIdsBefore = @()
try {
    New-Item -ItemType Directory -Path $TemporaryRoot -Force | Out-Null
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_INSTALL_DIR', $InstallDirectory, 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_SKIP_ELEVATION', '1', 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_SKIP_PROCESS_CHECK', '1', 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_REGISTRY_HIVE', 'HKCU', 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_UNINSTALL_KEY', $RegistryKey, 'Process')
    [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_PROGRESS_DELAY_MS', '40', 'Process')
    $ExpectedLaunchDelay = 1400
    if ($UseInstalledObs) {
        [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_OBS_PATH', $null, 'Process')
        [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_LAUNCH_DELAY_MS', '0', 'Process')
        $ExpectedLaunchDelay = 0
    }
    else {
        [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_OBS_PATH', $FakeObsPath, 'Process')
        [Environment]::SetEnvironmentVariable('OBS_AIRPLAY_SENDER_LAUNCH_DELAY_MS', '1400', 'Process')

        @'
internal static class FakeObs
{
    private static void Main()
    {
    }
}
'@ | Set-Content -LiteralPath $FakeObsSource -Encoding UTF8
        $Csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
        & $Csc /nologo /target:winexe /platform:x64 "/out:$FakeObsPath" $FakeObsSource
        if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $FakeObsPath -PathType Leaf)) {
            throw "Failed to compile fake OBS launcher"
        }
    }

    $ObsProcessIdsBefore = @(
        Get-Process -Name 'obs64' -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty Id
    )
    $StartInfo = New-Object System.Diagnostics.ProcessStartInfo
    $StartInfo.FileName = $SetupPath
    $StartInfo.UseShellExecute = $false
    $Process = [System.Diagnostics.Process]::Start($StartInfo)

    $Deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ($Process.MainWindowHandle -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $Deadline) {
        Start-Sleep -Milliseconds 50
        $Process.Refresh()
    }
    if ($Process.MainWindowHandle -eq [IntPtr]::Zero) {
        throw "Installer window did not appear"
    }

    $Window = [System.Windows.Automation.AutomationElement]::FromHandle($Process.MainWindowHandle)
    $InstallButtonName = [string]::Concat([char] 0x5B89, [char] 0x88C5)
    $InstallNameCondition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty,
        $InstallButtonName)
    $InstallButton = $null
    $ButtonDeadline = [DateTime]::UtcNow.AddSeconds(3)
    while ($null -eq $InstallButton -and [DateTime]::UtcNow -lt $ButtonDeadline) {
        $InstallButton = $Window.FindFirst(
            [System.Windows.Automation.TreeScope]::Descendants,
            $InstallNameCondition)
        if ($null -eq $InstallButton) {
            Start-Sleep -Milliseconds 100
        }
    }
    if ($null -eq $InstallButton) {
        $ElementCount = $Window.FindAll(
            [System.Windows.Automation.TreeScope]::Descendants,
            [System.Windows.Automation.Condition]::TrueCondition).Count
        $ElementSummary = $Window.FindAll(
            [System.Windows.Automation.TreeScope]::Descendants,
            [System.Windows.Automation.Condition]::TrueCondition) |
            ForEach-Object {
                "$($_.Current.ControlType.ProgrammaticName):$($_.Current.Name)"
            }
        throw "Install button was not found through UI Automation; window='$($Window.Current.Name)' elements=$ElementCount summary=$($ElementSummary -join ' | ')"
    }

    $InitialElements = $Window.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    $VisibleInitialProgress = $InitialElements |
        Where-Object {
            !$_.Current.IsOffscreen -and $_.Current.Name -match '\d+%'
        } |
        Select-Object -First 1
    if ($null -ne $VisibleInitialProgress) {
        throw "Progress feedback was visible before the install button was pressed"
    }

    $LaunchControl = $InitialElements |
        Where-Object {
            $_.Current.Name -like '*OBS Studio*' -and
            $_.Current.ClassName -like '*BUTTON*' -and
            $_.Current.NativeWindowHandle -ne 0
        } |
        Select-Object -First 1
    if ($null -eq $LaunchControl) {
        throw "Launch OBS checkbox was not found through UI Automation"
    }
    try {
        $Invoke = $InstallButton.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        $Invoke.Invoke()
    }
    catch {
        $ButtonHandle = [IntPtr] $InstallButton.Current.NativeWindowHandle
        if ($ButtonHandle -eq [IntPtr]::Zero) {
            throw "Install button exposes neither InvokePattern nor a native window handle"
        }
        $null = [InstallerUiNative]::SendMessage(
            $ButtonHandle,
            0x00F5,
            [IntPtr]::Zero,
            [IntPtr]::Zero)
    }

    $ObservedValues = New-Object System.Collections.Generic.List[int]
    $SawCompletion = $false
    $SawLaunchStage = $false
    $Stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $LaunchStageAt = [long] -1
    $CompletionAt = [long] -1
    $Deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $Deadline) {
        try {
            $MessageResult = [IntPtr]::Zero
            $Responsive = [InstallerUiNative]::SendMessageTimeout(
                [IntPtr] $Window.Current.NativeWindowHandle,
                0,
                [IntPtr]::Zero,
                [IntPtr]::Zero,
                0x0002,
                750,
                [ref] $MessageResult)
            if ($Responsive -eq [IntPtr]::Zero) {
                throw "Installer window stopped responding while the install was running"
            }

            $AllElements = $Window.FindAll(
                [System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.Condition]::TrueCondition)
            foreach ($Element in $AllElements) {
                try {
                    $Range = $Element.GetCurrentPattern(
                        [System.Windows.Automation.RangeValuePattern]::Pattern)
                    $Value = [int] $Range.Current.Value
                    if ($ObservedValues.Count -eq 0 -or
                        $ObservedValues[$ObservedValues.Count - 1] -ne $Value) {
                        $ObservedValues.Add($Value)
                    }
                    if ($Value -eq 100) {
                        $SawCompletion = $true
                        $CompletionAt = $Stopwatch.ElapsedMilliseconds
                    }
                    elseif ($Value -eq 98 -and !$SawLaunchStage) {
                        $SawLaunchStage = $true
                        $LaunchStageAt = $Stopwatch.ElapsedMilliseconds
                    }
                    break
                }
                catch {
                }
            }
            foreach ($Element in $AllElements) {
                if ($Element.Current.Name -match '(\d+)%') {
                    $Value = [int] $Matches[1]
                    if ($ObservedValues.Count -eq 0 -or
                        $ObservedValues[$ObservedValues.Count - 1] -ne $Value) {
                        $ObservedValues.Add($Value)
                    }
                    if ($Value -eq 100) {
                        $SawCompletion = $true
                        $CompletionAt = $Stopwatch.ElapsedMilliseconds
                    }
                    elseif ($Value -eq 98 -and !$SawLaunchStage) {
                        $SawLaunchStage = $true
                        $LaunchStageAt = $Stopwatch.ElapsedMilliseconds
                    }
                }
            }
            if ($SawCompletion) {
                break
            }
        }
        catch {
            if ($_ -and $_.Exception.Message -like 'Installer window stopped responding*') {
                throw
            }
            $Process.Refresh()
            if ($Process.HasExited) {
                break
            }
        }
        Start-Sleep -Milliseconds 20
    }

    if (!$SawCompletion) {
        throw "UI progress did not reach 100%; observed: $($ObservedValues -join ', ')"
    }
    if (!$SawLaunchStage) {
        throw "UI progress did not expose the OBS launch stage"
    }
    if ($ExpectedLaunchDelay -gt 0 -and
        $CompletionAt - $LaunchStageAt -lt ($ExpectedLaunchDelay - 200)) {
        throw "Completion was reported before the OBS launch delay ended: launch=$LaunchStageAt ms completion=$CompletionAt ms"
    }
    if ($ObservedValues.Count -lt 2) {
        throw "UI progress did not visibly advance; observed: $($ObservedValues -join ', ')"
    }
    $RequiredFile = Join-Path $InstallDirectory 'bin/64bit/obs-airplay-cast.dll'
    if (!(Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "UI installation did not install the plugin payload"
    }
    if ($UseInstalledObs) {
        $ObsDeadline = [DateTime]::UtcNow.AddSeconds(5)
        $LaunchedObs = @()
        while ($LaunchedObs.Count -eq 0 -and [DateTime]::UtcNow -lt $ObsDeadline) {
            $LaunchedObs = @(
                Get-Process -Name 'obs64' -ErrorAction SilentlyContinue |
                    Where-Object { $ObsProcessIdsBefore -notcontains $_.Id }
            )
            if ($LaunchedObs.Count -eq 0) {
                Start-Sleep -Milliseconds 100
            }
        }
        if ($LaunchedObs.Count -eq 0) {
            throw "Installer did not launch the installed OBS Studio"
        }
    }

    $MessageResult = [IntPtr]::Zero
    $ResponsiveAtCompletion = [InstallerUiNative]::SendMessageTimeout(
        [IntPtr] $Window.Current.NativeWindowHandle,
        0,
        [IntPtr]::Zero,
        [IntPtr]::Zero,
        0x0002,
        750,
        [ref] $MessageResult)
    if ($ResponsiveAtCompletion -eq [IntPtr]::Zero) {
        throw "Installer window was blocked after progress reached 100%"
    }

    $DialogElement = $null
    $DialogTitleCondition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty,
        'OBS AirPlay Sender')
    $DialogProcessCondition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
        $Process.Id)
    $DialogTypeCondition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Window)
    $DialogCondition = New-Object System.Windows.Automation.AndCondition(
        $DialogTitleCondition,
        $DialogProcessCondition,
        $DialogTypeCondition)
    $DialogDeadline = [DateTime]::UtcNow.AddSeconds(3)
    while ($null -eq $DialogElement -and [DateTime]::UtcNow -lt $DialogDeadline) {
        $DialogElement = $Window.FindFirst(
            [System.Windows.Automation.TreeScope]::Descendants,
            $DialogCondition)
        if ($null -eq $DialogElement) {
            Start-Sleep -Milliseconds 50
        }
    }
    if ($null -eq $DialogElement) {
        $TopWindows = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
            [System.Windows.Automation.TreeScope]::Children,
            [System.Windows.Automation.Condition]::TrueCondition) |
            ForEach-Object {
                "$($_.Current.ProcessId):$($_.Current.ClassName):$($_.Current.Name)"
            }
        throw "Completion dialog did not appear after progress reached 100%; top windows=$($TopWindows -join ' | ')"
    }
    $DialogAt = $Stopwatch.ElapsedMilliseconds
    if ($DialogAt - $CompletionAt -gt 1500) {
        throw "Completion dialog took too long after 100%: completion=$CompletionAt ms dialog=$DialogAt ms"
    }

    $OkButtonElement = $DialogElement.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition) |
        Where-Object {
            $_.Current.ClassName -eq 'Button' -and
            $_.Current.NativeWindowHandle -ne 0 -and
            ($_.Current.Name -eq '确定' -or $_.Current.Name -eq 'OK')
        } |
        Select-Object -First 1
    if ($null -ne $OkButtonElement) {
        $null = [InstallerUiNative]::SendMessage(
            [IntPtr] $OkButtonElement.Current.NativeWindowHandle,
            0x00F5,
            [IntPtr]::Zero,
            [IntPtr]::Zero)
    }
    else {
        $OkCondition = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button)
        $OkButton = $DialogElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Descendants,
            $OkCondition)
        if ($null -eq $OkButton) {
            throw "Completion dialog did not expose an OK button"
        }
        $OkInvoke = $OkButton.GetCurrentPattern(
            [System.Windows.Automation.InvokePattern]::Pattern)
        $OkInvoke.Invoke()
    }

    if (!$Process.WaitForExit(5000)) {
        throw "Installer did not exit within 5 seconds after dismissing completion dialog"
    }

    Write-Host "Installer UI progress test passed: $($ObservedValues -join ', '); launch-to-completion=$($CompletionAt - $LaunchStageAt) ms; completion-to-dialog=$($DialogAt - $CompletionAt) ms"
}
finally {
    if ($null -ne $Process -and !$Process.HasExited) {
        $Process.CloseMainWindow() | Out-Null
        if (!$Process.WaitForExit(3000)) {
            Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    foreach ($Name in $EnvironmentNames) {
        [Environment]::SetEnvironmentVariable($Name, $PreviousEnvironment[$Name], 'Process')
    }
    if ($UseInstalledObs) {
        Get-Process -Name 'obs64' -ErrorAction SilentlyContinue |
            Where-Object { $ObsProcessIdsBefore -notcontains $_.Id } |
            Stop-Process -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath "HKCU:\$RegistryKey" -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $TemporaryRoot) {
        Remove-Item -LiteralPath $TemporaryRoot -Recurse -Force
    }
}
