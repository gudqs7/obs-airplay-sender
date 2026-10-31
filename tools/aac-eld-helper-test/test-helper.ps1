[CmdletBinding()]
param(
    [string]$HelperPath = (
        Join-Path $PSScriptRoot '..\..\build_x64\RelWithDebInfo\obs-airplay-aac-eld.exe'
    ),
    [ValidateRange(1, 20000)][int]$Frames = 20
)

$ErrorActionPreference = 'Stop'

$HelperPath = [System.IO.Path]::GetFullPath($HelperPath)
if (-not (Test-Path -LiteralPath $HelperPath)) {
    throw "Helper not found: $HelperPath"
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $HelperPath
$startInfo.WorkingDirectory = Split-Path -Parent $HelperPath
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $startInfo
if (-not $process.Start()) {
    throw "Unable to start helper: $HelperPath"
}

try {
    $ready = $process.StandardOutput.ReadLine()
    if ($ready -ne 'READY 480') {
        throw "Unexpected helper readiness: $ready"
    }

    $output = [System.IO.MemoryStream]::new()
    $copyTask = $process.StandardOutput.BaseStream.CopyToAsync($output)

    $constants = @{
        SampleRate = 44100.0
        Frames = $Frames
        SamplesPerFrame = 480
        Channels = 2
    }
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $stdin = $process.StandardInput.BaseStream
    for ($frame = 0; $frame -lt $constants.Frames; $frame++) {
        $pcm = [byte[]]::new($constants.SamplesPerFrame * $constants.Channels * 2)
        for ($sample = 0; $sample -lt $constants.SamplesPerFrame; $sample++) {
            $index = $frame * $constants.SamplesPerFrame + $sample
            $phase = 2.0 * [Math]::PI * 1000.0 * $index / $constants.SampleRate
            $value = [int16][Math]::Round([Math]::Sin($phase) * 0.5 * 32767.0)
            for ($channel = 0; $channel -lt $constants.Channels; $channel++) {
                $offset = ($sample * $constants.Channels + $channel) * 2
                $pcm[$offset] = [byte]($value -band 0xff)
                $pcm[$offset + 1] = [byte](($value -shr 8) -band 0xff)
            }
        }
        $stdin.Write($pcm, 0, $pcm.Length)
    }
    $stdin.Close()

    $null = $copyTask.GetAwaiter().GetResult()
    $process.WaitForExit()
    $stopwatch.Stop()
    $stderr = $process.StandardError.ReadToEnd()
    if ($process.ExitCode -ne 0) {
        throw "Helper exited with $($process.ExitCode): $stderr"
    }

    $encoded = $output.ToArray()
    $stream = [System.IO.MemoryStream]::new($encoded, $false)
    $lengthBytes = [byte[]]::new(4)
    $frameSizes = [System.Collections.Generic.List[int]]::new()
    while ($stream.Position -lt $stream.Length) {
        if ($stream.Read($lengthBytes, 0, 4) -ne 4) {
            throw 'Truncated frame length'
        }
        $frameLength = [System.BitConverter]::ToUInt32($lengthBytes, 0)
        if ($frameLength -eq 0 -or $frameLength -gt 8192) {
            throw "Invalid frame length: $frameLength"
        }
        if ($stream.Position + $frameLength -gt $stream.Length) {
            throw "Truncated frame payload: $frameLength bytes"
        }
        $stream.Seek($frameLength, [System.IO.SeekOrigin]::Current) | Out-Null
        $frameSizes.Add([int]$frameLength)
    }

    if ($frameSizes.Count -ne $constants.Frames) {
        throw "Expected $($constants.Frames) frames, got $($frameSizes.Count)"
    }

    $minimum = ($frameSizes | Measure-Object -Minimum).Minimum
    $maximum = ($frameSizes | Measure-Object -Maximum).Maximum
    $audioSeconds = ($frameSizes.Count * $constants.SamplesPerFrame) / $constants.SampleRate
    $realtimeFactor = $audioSeconds / $stopwatch.Elapsed.TotalSeconds
    Write-Output (
        "ready=480 frames={0} bytes={1} frame-bytes={2}-{3} elapsed-ms={4} realtime={5:N2}x" -f
        $frameSizes.Count, $encoded.Length, $minimum, $maximum,
        $stopwatch.ElapsedMilliseconds, $realtimeFactor
    )
}
finally {
    if (-not $process.HasExited) {
        $process.Kill()
    }
    $process.Dispose()
}
