# create_icon.ps1
# Generates app.ico from WPF path data (no external tools needed)
# Run: powershell -STA -ExecutionPolicy Bypass -File tools\create_icon.ps1

Add-Type -AssemblyName PresentationFramework
Add-Type -AssemblyName PresentationCore
Add-Type -AssemblyName WindowsBase

$outPath = Join-Path (Split-Path $PSScriptRoot -Parent) "app.ico"

$svgW = 96.58
$svgH = 128.46

$pathWhite1 = "M15.27,71.45c-3.43-1.75-6.86-3.5-9.44-4.83v-18.47l42.46,11.16,42.46-11.16v18.47c-2.58,1.32-6.01,3.08-9.45,4.83-4.9,2.51-9.81,5.01-12.22,6.24l-20.79-11.61-20.79,11.61c-2.42-1.23-7.32-3.73-12.23-6.25Z"
$pathWhite2 = "M96.57,31.03h0s0,3.55,0,3.55l-48.29,12.69L0,34.58v-3.55h0s0-11.08,0-11.08h0C12.26,7.19,29.18-.01,46.87,0h1.42S49.71,0,49.71,0c17.69-.01,34.62,7.19,46.87,19.95h0s0,11.08,0,11.08Z"
$pathWhite3 = "M96.58,76.74v24.85s-12.87,10.2-22.79,18.03c-7.26,5.73-16.25,8.85-25.5,8.84,0,0,0,0,0,0,0,0,0,0,0,0-9.25,0-18.24-3.11-25.5-8.84C12.87,111.79,0,101.61,0,101.59v-24.85c.16.08.32.16.49.25,2.59,1.32,6.04,3.09,9.48,4.84,6.89,3.52,13.79,7.04,13.79,7.05l3.99,2.02,20.53-11.47,20.53,11.47,4-2.03s6.89-3.52,13.78-7.03c3.45-1.76,6.9-3.52,9.49-4.84.17-.09.34-.17.5-.26Z"
$pathPink   = "M48.29,59.31L5.83,48.16v18.47c2.59,1.32,6.01,3.08,9.44,4.83,4.91,2.51,9.81,5.02,12.23,6.25l20.79-11.61,20.79,11.61c2.41-1.23,7.32-3.73,12.22-6.24,3.44-1.75,6.87-3.51,9.45-4.83v-18.47l-42.46,11.16Z"

$colorBg    = [System.Windows.Media.Color]::FromRgb(0x0E, 0x0E, 0x1C)
$colorWhite = [System.Windows.Media.Color]::FromRgb(0xFD, 0xFD, 0xFD)
$colorPink  = [System.Windows.Media.Color]::FromRgb(0xFF, 0x2A, 0xFF)

function Make-Path($data, $color) {
    $p = New-Object System.Windows.Shapes.Path
    $p.Fill = New-Object System.Windows.Media.SolidColorBrush($color)
    $p.Data = [System.Windows.Media.Geometry]::Parse($data)
    return $p
}

function Render-Size([int]$size) {
    $canvas = New-Object System.Windows.Controls.Canvas
    $canvas.Width  = $svgW
    $canvas.Height = $svgH

    $bg = New-Object System.Windows.Shapes.Rectangle
    $bg.Width  = $svgW
    $bg.Height = $svgH
    $bg.Fill   = New-Object System.Windows.Media.SolidColorBrush($colorBg)
    [void]$canvas.Children.Add($bg)
    [void]$canvas.Children.Add((Make-Path $pathWhite1 $colorWhite))
    [void]$canvas.Children.Add((Make-Path $pathWhite2 $colorWhite))
    [void]$canvas.Children.Add((Make-Path $pathWhite3 $colorWhite))
    [void]$canvas.Children.Add((Make-Path $pathPink   $colorPink))

    $vb = New-Object System.Windows.Controls.Viewbox
    $vb.Child   = $canvas
    $vb.Width   = [double]$size
    $vb.Height  = [double]$size
    $vb.Stretch = [System.Windows.Media.Stretch]::Uniform

    $vb.Measure([System.Windows.Size]::new([double]$size, [double]$size))
    $vb.Arrange([System.Windows.Rect]::new(0.0, 0.0, [double]$size, [double]$size))
    $vb.UpdateLayout()

    $rtb = New-Object -TypeName System.Windows.Media.Imaging.RenderTargetBitmap `
        -ArgumentList @([int]$size, [int]$size, 96.0, 96.0, ([System.Windows.Media.PixelFormats]::Pbgra32))
    $rtb.Render($vb)

    $enc = New-Object System.Windows.Media.Imaging.PngBitmapEncoder
    $frame = [System.Windows.Media.Imaging.BitmapFrame]::Create([System.Windows.Media.Imaging.BitmapSource]$rtb)
    $enc.Frames.Add($frame)

    $ms = New-Object System.IO.MemoryStream
    $enc.Save($ms)
    [byte[]]$bytes = $ms.ToArray()
    $ms.Dispose()
    return $bytes
}

$sizes = @(16, 32, 48, 256)
# Use string keys to avoid PowerShell integer-index ambiguity
$pngs    = @{}
$offsets = @{}

foreach ($s in $sizes) {
    Write-Host "  Rendering ${s}x${s}..."
    $pngs["$s"] = Render-Size $s
    Write-Host "    -> $($pngs["$s"].Length) bytes"
}

# Compute offsets
$headerSize = 6
$entrySize  = 16
$cur = $headerSize + $entrySize * $sizes.Count
foreach ($s in $sizes) {
    $offsets["$s"] = $cur
    $cur += $pngs["$s"].Length
}

# Build ICO binary using MemoryStream directly (avoids BinaryWriter overload ambiguity)
function Write-U8($stream, [int]$v) {
    $stream.WriteByte([byte]($v -band 0xFF))
}
function Write-U16($stream, [int]$v) {
    $stream.WriteByte([byte]($v -band 0xFF))
    $stream.WriteByte([byte](($v -shr 8) -band 0xFF))
}
function Write-U32($stream, [long]$v) {
    $stream.WriteByte([byte]($v -band 0xFF))
    $stream.WriteByte([byte](($v -shr 8)  -band 0xFF))
    $stream.WriteByte([byte](($v -shr 16) -band 0xFF))
    $stream.WriteByte([byte](($v -shr 24) -band 0xFF))
}

$ms2 = New-Object System.IO.MemoryStream

# ICONDIR header
Write-U16 $ms2 0
Write-U16 $ms2 1
Write-U16 $ms2 $sizes.Count

# ICONDIRENTRY (16 bytes each)
foreach ($s in $sizes) {
    $wb = if ($s -eq 256) { 0 } else { $s }
    Write-U8  $ms2 $wb
    Write-U8  $ms2 $wb
    Write-U8  $ms2 0
    Write-U8  $ms2 0
    Write-U16 $ms2 1
    Write-U16 $ms2 32
    Write-U32 $ms2 ([long]$pngs["$s"].Length)
    Write-U32 $ms2 ([long]$offsets["$s"])
}

# PNG image data
foreach ($s in $sizes) {
    $ms2.Write([byte[]]$pngs["$s"], 0, $pngs["$s"].Length)
}

[System.IO.File]::WriteAllBytes($outPath, $ms2.ToArray())
$ms2.Dispose()

Write-Host ""
Write-Host "Done: $outPath  ($([int]((New-Object System.IO.FileInfo($outPath)).Length / 1024)) KB)"
