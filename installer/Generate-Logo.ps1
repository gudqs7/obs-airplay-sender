[CmdletBinding()]
param(
    [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot 'assets'
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

Add-Type -AssemblyName System.Drawing

function New-RoundedPath {
    param(
        [System.Drawing.RectangleF] $Rectangle,
        [float] $Radius
    )

    $diameter = $Radius * 2
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $path.AddArc($Rectangle.Left, $Rectangle.Top, $diameter, $diameter, 180, 90)
    $path.AddArc(
        $Rectangle.Right - $diameter,
        $Rectangle.Top,
        $diameter,
        $diameter,
        270,
        90
    )
    $path.AddArc(
        $Rectangle.Right - $diameter,
        $Rectangle.Bottom - $diameter,
        $diameter,
        $diameter,
        0,
        90
    )
    $path.AddArc(
        $Rectangle.Left,
        $Rectangle.Bottom - $diameter,
        $diameter,
        $diameter,
        90,
        90
    )
    $path.CloseFigure()
    return $path
}

$masterPath = Join-Path $OutputDirectory 'logo-512.png'
$bitmap = [System.Drawing.Bitmap]::new(
    512,
    512,
    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)

try {
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.CompositingQuality =
        [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode =
        [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic

    $backgroundPath = New-RoundedPath `
        -Rectangle ([System.Drawing.RectangleF]::new(16, 16, 480, 480)) `
        -Radius 112
    $backgroundBrush = [System.Drawing.SolidBrush]::new(
        [System.Drawing.Color]::FromArgb(255, 18, 34, 47)
    )
    try {
        $graphics.FillPath($backgroundBrush, $backgroundPath)
    }
    finally {
        $backgroundBrush.Dispose()
        $backgroundPath.Dispose()
    }

    $monitorPath = New-RoundedPath `
        -Rectangle ([System.Drawing.RectangleF]::new(104, 94, 304, 220)) `
        -Radius 42
    $monitorPen = [System.Drawing.Pen]::new(
        [System.Drawing.Color]::FromArgb(255, 246, 250, 252),
        28
    )
    try {
        $monitorPen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
        $graphics.DrawPath($monitorPen, $monitorPath)
    }
    finally {
        $monitorPen.Dispose()
        $monitorPath.Dispose()
    }

    $triangle = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $triangle.AddPolygon(
        [System.Drawing.PointF[]] @(
            [System.Drawing.PointF]::new(256, 164)
            [System.Drawing.PointF]::new(310, 266)
            [System.Drawing.PointF]::new(202, 266)
        )
    )
    $triangle.CloseFigure()
    $accentBrush = [System.Drawing.SolidBrush]::new(
        [System.Drawing.Color]::FromArgb(255, 53, 199, 229)
    )
    try {
        $graphics.FillPath($accentBrush, $triangle)
    }
    finally {
        $accentBrush.Dispose()
        $triangle.Dispose()
    }

    $barPath = New-RoundedPath `
        -Rectangle ([System.Drawing.RectangleF]::new(190, 300, 132, 26)) `
        -Radius 13
    $barBrush = [System.Drawing.SolidBrush]::new(
        [System.Drawing.Color]::FromArgb(255, 53, 199, 229)
    )
    try {
        $graphics.FillPath($barBrush, $barPath)
    }
    finally {
        $barBrush.Dispose()
        $barPath.Dispose()
    }

    $liveBrush = [System.Drawing.SolidBrush]::new(
        [System.Drawing.Color]::FromArgb(255, 255, 92, 97)
    )
    try {
        $graphics.FillEllipse($liveBrush, 362, 112, 36, 36)
    }
    finally {
        $liveBrush.Dispose()
    }

    $bitmap.Save($masterPath, [System.Drawing.Imaging.ImageFormat]::Png)
}
finally {
    $graphics.Dispose()
    $bitmap.Dispose()
}

$source = [System.Drawing.Bitmap]::new($masterPath)
try {
    foreach ($size in @(256, 128, 64)) {
        $target = Join-Path $OutputDirectory "logo-$size.png"
        $resized = [System.Drawing.Bitmap]::new(
            $size,
            $size,
            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
        )
        $resizeGraphics = [System.Drawing.Graphics]::FromImage($resized)
        try {
            $resizeGraphics.Clear([System.Drawing.Color]::Transparent)
            $resizeGraphics.SmoothingMode =
                [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
            $resizeGraphics.PixelOffsetMode =
                [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $resizeGraphics.CompositingQuality =
                [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
            $resizeGraphics.InterpolationMode =
                [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $resizeGraphics.DrawImage(
                $source,
                [System.Drawing.Rectangle]::new(0, 0, $size, $size)
            )
            $resized.Save($target, [System.Drawing.Imaging.ImageFormat]::Png)
        }
        finally {
            $resizeGraphics.Dispose()
            $resized.Dispose()
        }
    }
}
finally {
    $source.Dispose()
}

$iconSizes = @(64, 128, 256)
$iconImages = [System.Collections.Generic.List[byte[]]]::new()
foreach ($size in $iconSizes) {
    $iconImages.Add(
        [System.IO.File]::ReadAllBytes((Join-Path $OutputDirectory "logo-$size.png"))
    )
}

$iconPath = Join-Path $OutputDirectory 'obs-airplay-sender.ico'
$stream = [System.IO.File]::Create($iconPath)
$writer = [System.IO.BinaryWriter]::new($stream)
try {
    $writer.Write([ushort] 0)
    $writer.Write([ushort] 1)
    $writer.Write([ushort] $iconSizes.Count)

    $offset = 6 + ($iconSizes.Count * 16)
    for ($index = 0; $index -lt $iconSizes.Count; $index++) {
        $size = $iconSizes[$index]
        $dimension = if ($size -ge 256) { 0 } else { $size }
        $writer.Write([byte] $dimension)
        $writer.Write([byte] $dimension)
        $writer.Write([byte] 0)
        $writer.Write([byte] 0)
        $writer.Write([ushort] 1)
        $writer.Write([ushort] 32)
        $writer.Write([uint32] $iconImages[$index].Length)
        $writer.Write([uint32] $offset)
        $offset += $iconImages[$index].Length
    }

    foreach ($imageBytes in $iconImages) {
        $writer.Write($imageBytes)
    }
}
finally {
    $writer.Dispose()
    $stream.Dispose()
}

Get-ChildItem -LiteralPath $OutputDirectory -File |
    Where-Object {
        $_.Name -match '^logo-(64|128|256|512)\.png$' -or
        $_.Name -eq 'obs-airplay-sender.ico'
    } |
    Select-Object FullName, Length
