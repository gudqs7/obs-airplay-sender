[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',
    [string] $ReleaseDirectory,
    [string] $OutputDirectory,
    [string] $Version,
    [string] $CompilerPath
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

$ReleaseDirectory = [IO.Path]::GetFullPath($ReleaseDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$PayloadDirectory = Join-Path $ReleaseDirectory 'obs-airplay-cast'
$InstallerScript = Join-Path $PSScriptRoot 'obs-airplay-cast.iss'
$IconPath = Join-Path $PSScriptRoot 'assets\obs-airplay-sender.ico'
$SetupName = "$($BuildSpec.name)-$Version-windows-x64-Setup.exe"
$SetupPath = Join-Path $OutputDirectory $SetupName
$HashPath = "$SetupPath.sha256"
$ZipName = "$($BuildSpec.name)-$Version-windows-x64.zip"
$ZipPath = Join-Path $OutputDirectory $ZipName
$ZipHashPath = "$ZipPath.sha256"

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

foreach ($AssetPath in @($InstallerScript, $IconPath)) {
    if (!(Test-Path -LiteralPath $AssetPath -PathType Leaf)) {
        throw "安装器资源缺失，缺少：$AssetPath"
    }
}

foreach ($RelativePath in $RequiredFiles) {
    $Path = Join-Path $PayloadDirectory $RelativePath
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "插件安装目录不完整，缺少：$Path"
    }
}

if ([string]::IsNullOrWhiteSpace($CompilerPath)) {
    $Candidates = @(
        $env:INNO_SETUP_COMPILER
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe')
    ) | Where-Object { !( [string]::IsNullOrWhiteSpace($_) ) }

    $CompilerPath = $Candidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($CompilerPath) -or
    !(Test-Path -LiteralPath $CompilerPath -PathType Leaf)) {
    throw '找不到 Inno Setup 6 的 ISCC.exe。请安装 Inno Setup 6，或传入 -CompilerPath。'
}
$CompilerPath = [IO.Path]::GetFullPath($CompilerPath)

$VersionParts = @($Version -split '\.')
$VersionInfoVersion = '0.0.0.0'
if ($VersionParts.Count -ge 1 -and $VersionParts.Count -le 4 -and
    @($VersionParts | Where-Object { $_ -notmatch '^\d+$' }).Count -eq 0) {
    $NumericParts = @($VersionParts | ForEach-Object { [int] $_ })
    while ($NumericParts.Count -lt 4) {
        $NumericParts += 0
    }
    $VersionInfoVersion = $NumericParts[0..3] -join '.'
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$OutputBaseFilename = [IO.Path]::GetFileNameWithoutExtension($SetupName)
$CompilerArguments = @(
    "/DMyAppVersion=$Version"
    "/DMyVersionInfoVersion=$VersionInfoVersion"
    "/DMyReleaseDirectory=$PayloadDirectory"
    "/DMyOutputDirectory=$OutputDirectory"
    "/DMyOutputBaseFilename=$OutputBaseFilename"
    $InstallerScript
)

& $CompilerPath @CompilerArguments
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup 编译失败，退出码：$LASTEXITCODE"
}
if (!(Test-Path -LiteralPath $SetupPath -PathType Leaf)) {
    throw "Inno Setup 未生成预期文件：$SetupPath"
}

$Hash = (Get-FileHash -LiteralPath $SetupPath -Algorithm SHA256).Hash.ToLowerInvariant()
"$Hash  $SetupName" | Set-Content -LiteralPath $HashPath -Encoding ASCII

$ZipTemporaryRoot = Join-Path $env:TEMP (
    "obs-airplay-zip-" + [guid]::NewGuid().ToString('N'))
$ZipSourceDirectory = Join-Path $ZipTemporaryRoot 'obs-airplay-cast'
try {
    Copy-Item -LiteralPath $PayloadDirectory -Destination $ZipSourceDirectory -Recurse
    Get-ChildItem -LiteralPath $ZipSourceDirectory -Recurse -File |
        Where-Object { $_.Extension -in @('.pdb', '.ilk', '.exp') } |
        Remove-Item -Force

    Compress-Archive -LiteralPath $ZipSourceDirectory -DestinationPath $ZipPath `
        -CompressionLevel Optimal -Force
    $ZipHash = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    "$ZipHash  $ZipName" | Set-Content -LiteralPath $ZipHashPath -Encoding ASCII
}
finally {
    if (Test-Path -LiteralPath $ZipTemporaryRoot) {
        Remove-Item -LiteralPath $ZipTemporaryRoot -Recurse -Force
    }
}

Write-Host "Compiler: $CompilerPath"
Write-Host "Installer: $SetupPath"
Write-Host "SHA256:    $Hash"
Write-Host "Archive:   $ZipPath"
Write-Host "ZIP SHA256: $ZipHash"
