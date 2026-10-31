[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',
    [string] $ReleaseDirectory,
    [string] $OutputDirectory,
    [string] $Version
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildSpec = Get-Content -LiteralPath (Join-Path $ProjectRoot 'buildspec.json') -Raw |
    ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = [string] $BuildSpec.version
}
if ([string]::IsNullOrWhiteSpace($ReleaseDirectory)) {
    $ReleaseDirectory = Join-Path $ProjectRoot "release/$Configuration"
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $ProjectRoot 'release'
}

$ReleaseDirectory = [System.IO.Path]::GetFullPath($ReleaseDirectory)
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$PluginDirectory = Join-Path $ReleaseDirectory 'obs-airplay-cast'
$Csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'

if (!(Test-Path -LiteralPath $Csc -PathType Leaf)) {
    throw "找不到 .NET Framework x64 C# 编译器：$Csc"
}

$RequiredFiles = @(
    'bin/64bit/obs-airplay-cast.dll'
    'bin/64bit/obs-airplay-cast-bridge.exe'
    'bin/64bit/obs-airplay-aac-eld.exe'
    'data/locale/en-US.ini'
    'data/locale/zh-CN.ini'
    'LICENSE'
    'README.md'
    'THIRD_PARTY_NOTICES.md'
    'licenses/doubletake-LICENSE.txt'
    'licenses/GPL-3.0.txt'
    'licenses/libxaac-LICENSE.txt'
    'licenses/libxaac-NOTICE.txt'
)

foreach ($RelativePath in $RequiredFiles) {
    $Path = Join-Path $PluginDirectory $RelativePath
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "插件安装目录不完整，缺少：$Path"
    }
}

$TemporaryRoot = Join-Path $env:TEMP "obs-airplay-sender-installer-$([guid]::NewGuid().ToString('N'))"
$PayloadRoot = Join-Path $TemporaryRoot 'payload'
$PayloadZip = Join-Path $TemporaryRoot 'payload.zip'
$VersionFile = Join-Path $TemporaryRoot 'VersionInfo.cs'
$UninstallerPath = Join-Path $TemporaryRoot 'Uninstall.exe'
$SetupName = "$($BuildSpec.name)-$Version-windows-x64-Setup.exe"
$SetupPath = Join-Path $OutputDirectory $SetupName
$HashPath = "$SetupPath.sha256"

try {
    New-Item -ItemType Directory -Path $PayloadRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

    Copy-Item -LiteralPath $PluginDirectory -Destination $PayloadRoot -Recurse
    Get-ChildItem -LiteralPath $PayloadRoot -Recurse -File -Include '*.pdb', '*.ilk', '*.exp' |
        Remove-Item -Force

    Compress-Archive -Path (Join-Path $PayloadRoot 'obs-airplay-cast/*') `
        -DestinationPath $PayloadZip -CompressionLevel Optimal

    $AssemblyVersion = $Version
    if ($AssemblyVersion -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') {
        $AssemblyVersion = '0.0.0.0'
    }
    elseif (($AssemblyVersion -split '\.').Count -eq 3) {
        $AssemblyVersion += '.0'
    }

    @"
using System.Reflection;

[assembly: AssemblyTitle("OBS AirPlay Sender Setup")]
[assembly: AssemblyDescription("OBS AirPlay Sender 一键安装程序")]
[assembly: AssemblyProduct("OBS AirPlay Sender")]
[assembly: AssemblyCompany("gudqs7")]
[assembly: AssemblyCopyright("GPL-2.0")]
[assembly: AssemblyVersion("$AssemblyVersion")]
[assembly: AssemblyFileVersion("$AssemblyVersion")]
[assembly: AssemblyInformationalVersion("$Version")]
"@ | Set-Content -LiteralPath $VersionFile -Encoding UTF8

    $CommonCscArguments = @(
        '/nologo'
        '/target:winexe'
        '/platform:x64'
        '/optimize+'
        '/codepage:65001'
        "/win32manifest:$PSScriptRoot\app.manifest"
        "/reference:System.IO.Compression.dll"
        "/reference:System.IO.Compression.FileSystem.dll"
    )

    $UninstallerCscArguments = $CommonCscArguments + @(
        '/define:UNINSTALLER'
        "/out:$UninstallerPath"
        (Join-Path $PSScriptRoot 'Setup.cs')
        $VersionFile
    )

    & $Csc @UninstallerCscArguments
    if ($LASTEXITCODE -ne 0) {
        throw "C# 卸载程序编译失败，退出码：$LASTEXITCODE"
    }

    $SetupCscArguments = $CommonCscArguments + @(
        "/resource:$PayloadZip,OBSAirPlay.Payload.zip"
        "/resource:$UninstallerPath,OBSAirPlay.Uninstaller.exe"
        "/out:$SetupPath"
        (Join-Path $PSScriptRoot 'Setup.cs')
        $VersionFile
    )

    & $Csc @SetupCscArguments
    if ($LASTEXITCODE -ne 0) {
        throw "C# 安装程序编译失败，退出码：$LASTEXITCODE"
    }

    $Hash = (Get-FileHash -LiteralPath $SetupPath -Algorithm SHA256).Hash.ToLowerInvariant()
    "$Hash  $SetupName" | Set-Content -LiteralPath $HashPath -Encoding ASCII

    Write-Host "Installer: $SetupPath"
    Write-Host "SHA256:    $Hash"
}
finally {
    if (Test-Path -LiteralPath $TemporaryRoot) {
        Remove-Item -LiteralPath $TemporaryRoot -Recurse -Force
    }
}
