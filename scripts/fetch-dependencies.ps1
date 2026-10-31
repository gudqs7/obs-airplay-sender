[CmdletBinding()]
param(
    [string]$DepsRoot = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$env:GIT_TERMINAL_PROMPT = '0'

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $DepsRoot) {
    $DepsRoot = Join-Path $projectRoot '.deps\source'
}
$DepsRoot = [System.IO.Path]::GetFullPath($DepsRoot)

$libxaacRepository = 'https://github.com/ittiam-systems/libxaac.git'
$libxaacCommit = '2fbadd57bca46693b7a077bcf1513131106d960c'
$doubletakeRepository = 'https://github.com/omarroth/doubletake.git'
$doubletakeCommit = '5e2f1cbe3e1070bcefec8e549c7baf884727bc00'
$libxaacPatch = Join-Path $projectRoot 'patches\libxaac-eld-core.patch'
$doubletakePatch = Join-Path $projectRoot 'patches\doubletake-obs-airplay.patch'
$bridgeSource = Join-Path $projectRoot 'bridge\cmd\obsairplay-bridge'

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & git -C $WorkingDirectory @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed in $WorkingDirectory"
    }
}

function Initialize-Dependency {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$Commit
    )

    $target = Join-Path $DepsRoot $Name
    if (-not (Test-Path -LiteralPath $target)) {
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        Invoke-Git $target @('init', '--quiet')
    }
    elseif (-not (Test-Path -LiteralPath (Join-Path $target '.git'))) {
        throw "Existing dependency directory is not a Git checkout: $target"
    }

    Invoke-Git $target @('config', 'core.autocrlf', 'false')
    $head = (& git -C $target rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[0-9a-f]{40}$') {
        $head = ''
    }
    if ($head -eq $Commit) {
        return $target
    }

    if ($head -and -not $Force) {
        throw "Dependency $Name has unexpected commit $head. Re-run with -Force to reset it."
    }

    $remote = (& git -C $target remote get-url origin 2>$null)
    if ($LASTEXITCODE -ne 0) {
        Invoke-Git $target @('remote', 'add', 'origin', $Repository)
    }
    elseif ($Force -and $remote.Trim() -ne $Repository) {
        Invoke-Git $target @('remote', 'set-url', 'origin', $Repository)
    }

    if ($Force) {
        Invoke-Git $target @('fetch', '--depth', '1', 'origin', $Commit)
        Invoke-Git $target @('checkout', '--detach', '--force', 'FETCH_HEAD')
        Invoke-Git $target @('clean', '-fdx')
        return $target
    }

    Invoke-Git $target @('fetch', '--depth', '1', 'origin', $Commit)
    Invoke-Git $target @('checkout', '--detach', 'FETCH_HEAD')
    return $target
}

New-Item -ItemType Directory -Path $DepsRoot -Force | Out-Null

$libxaacPath = Initialize-Dependency -Name 'libxaac' -Repository $libxaacRepository -Commit $libxaacCommit
$doubletakePath = Initialize-Dependency -Name 'doubletake' -Repository $doubletakeRepository -Commit $doubletakeCommit

& git -C $libxaacPath apply --reverse --check $libxaacPatch 2>$null
$libxaacPatchApplied = $LASTEXITCODE -eq 0
if (-not $libxaacPatchApplied) {
    Invoke-Git $libxaacPath @('apply', '--check', $libxaacPatch)
    Invoke-Git $libxaacPath @('apply', $libxaacPatch)
}

& git -C $doubletakePath apply --reverse --check $doubletakePatch 2>$null
$patchApplied = $LASTEXITCODE -eq 0
if (-not $patchApplied) {
    Invoke-Git $doubletakePath @('apply', '--check', $doubletakePatch)
    Invoke-Git $doubletakePath @('apply', $doubletakePatch)
}

$bridgeTarget = Join-Path $doubletakePath 'cmd\obsairplay-bridge'
New-Item -ItemType Directory -Path $bridgeTarget -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $bridgeSource 'main.go') -Destination $bridgeTarget -Force
Copy-Item -LiteralPath (Join-Path $bridgeSource 'main_test.go') -Destination $bridgeTarget -Force

Write-Output "libxaac:   $libxaacCommit + patches/libxaac-eld-core.patch"
Write-Output "doubletake: $doubletakeCommit + patches/doubletake-obs-airplay.patch"
Write-Output "Dependencies ready in $DepsRoot"
