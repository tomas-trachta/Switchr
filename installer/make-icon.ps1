# installer/make-icon.ps1 -- regenerate installer\Switchr.ico from scratch.
#
# Renders the icon at the standard Windows sizes (16/24/32/48/64/128/256)
# using System.Drawing, then packs the PNG-encoded frames into a multi-image
# .ico. Run this if you tweak the design; the produced Switchr.ico is
# committed so the build doesn't depend on having .NET drawing primitives
# at compile time.
[CmdletBinding()]
param(
    [string]$OutPath = (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'Switchr.ico')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$sizes = @(16, 24, 32, 48, 64, 128, 256)

function Render-Frame([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

    $radius = [Math]::Max(1, [int]($size * 0.18))
    $d = $radius * 2
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0,          0,          $d, $d, 180, 90)
    $path.AddArc($size - $d, 0,          $d, $d, 270, 90)
    $path.AddArc($size - $d, $size - $d, $d, $d,   0, 90)
    $path.AddArc(0,          $size - $d, $d, $d,  90, 90)
    $path.CloseFigure()

    $bg = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 16, 18, 27))
    $g.FillPath($bg, $path)
    $bg.Dispose()

    if ($size -ge 32) {
        $stroke = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(60, 255, 255, 255)), 1
        $g.DrawPath($stroke, $path)
        $stroke.Dispose()
    }

    # Two overlapping rounded tiles, evoking window-switching stacked cards.
    $pad    = [Math]::Max(2, [int]($size * 0.22))
    $tileW  = [int]($size * 0.5)
    $tileH  = [int]($size * 0.5)
    $tileR  = if ($size -ge 32) { [int]($size * 0.06) } else { 0 }

    $backRect  = New-Object System.Drawing.RectangleF($pad, $pad, $tileW, $tileH)
    $frontRect = New-Object System.Drawing.RectangleF(($size - $pad - $tileW), ($size - $pad - $tileH), $tileW, $tileH)

    function New-RoundedRectPath([System.Drawing.RectangleF]$r, [float]$radius) {
        $rp = New-Object System.Drawing.Drawing2D.GraphicsPath
        if ($radius -le 0) {
            $rp.AddRectangle($r)
            return $rp
        }
        $dd = $radius * 2
        $rp.AddArc($r.X,               $r.Y,                $dd, $dd, 180, 90)
        $rp.AddArc($r.Right - $dd,     $r.Y,                $dd, $dd, 270, 90)
        $rp.AddArc($r.Right - $dd,     $r.Bottom - $dd,     $dd, $dd,   0, 90)
        $rp.AddArc($r.X,               $r.Bottom - $dd,     $dd, $dd,  90, 90)
        $rp.CloseFigure()
        return $rp
    }

    $backPath = New-RoundedRectPath $backRect $tileR
    $backBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(160, 122, 162, 247))
    $g.FillPath($backBrush, $backPath)
    $backBrush.Dispose()
    $backPath.Dispose()

    $frontPath = New-RoundedRectPath $frontRect $tileR
    $frontBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 122, 162, 247))
    $g.FillPath($frontBrush, $frontPath)
    $frontBrush.Dispose()
    $frontPath.Dispose()

    $g.Dispose()
    $path.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return ,$ms.ToArray()
}

$frames = @{}
foreach ($s in $sizes) { $frames[$s] = Render-Frame $s }

$out = New-Object System.IO.MemoryStream
$bw  = New-Object System.IO.BinaryWriter($out)
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$sizes.Count)

$offset = 6 + 16 * $sizes.Count
foreach ($s in $sizes) {
    $png = $frames[$s]
    $dim = [byte]($s -band 0xff)
    $bw.Write([byte]$dim)
    $bw.Write([byte]$dim)
    $bw.Write([byte]0)
    $bw.Write([byte]0)
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]$png.Length)
    $bw.Write([uint32]$offset)
    $offset += $png.Length
}
foreach ($s in $sizes) { $bw.Write($frames[$s]) }
$bw.Flush()

[System.IO.File]::WriteAllBytes($OutPath, $out.ToArray())
$bw.Dispose()
$out.Dispose()

Write-Host "wrote $OutPath ($([int]((Get-Item $OutPath).Length / 1KB)) KB, $($sizes.Count) frames)" -ForegroundColor Green
