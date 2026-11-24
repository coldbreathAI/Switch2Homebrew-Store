param(
    [Parameter(Mandatory)][string]$RegularFont,
    [Parameter(Mandatory)][string]$BoldFont,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$codepoints = (32..126) + @(
    0xc1, 0xc9, 0xcd, 0xd3, 0xda, 0xdc, 0xd1, 0xe1,
    0xe9, 0xed, 0xf3, 0xfa, 0xfc, 0xf1, 0xbf, 0xa1
)
$fonts = [System.Drawing.Text.PrivateFontCollection]::new()
$fonts.AddFontFile($RegularFont)
$fonts.AddFontFile($BoldFont)
$families = $fonts.Families
if ($families.Length -ne 1) { throw 'Could not load the Noto Sans font family.' }
$family = $families[0]
$format = [System.Drawing.StringFormat]::GenericTypographic.Clone()
$format.FormatFlags = $format.FormatFlags -bor [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces
$header = [System.Text.StringBuilder]::new()
[void]$header.AppendLine('#ifndef SWITCH2HOMEBREW_STORE_FONT_H')
[void]$header.AppendLine('#define SWITCH2HOMEBREW_STORE_FONT_H')
[void]$header.AppendLine('')
[void]$header.AppendLine('typedef struct { unsigned short width, advance; unsigned int offset; } StoreGlyph;')

function Add-StoreFont([string]$Name, [single]$Size, [System.Drawing.FontStyle]$Style) {
    $font = [System.Drawing.Font]::new($family, $Size, $Style, [System.Drawing.GraphicsUnit]::Pixel)
    $height = 6 + [int][Math]::Ceiling($Size * $family.GetLineSpacing($Style) / $family.GetEmHeight($Style))
    $canvasWidth = [int]($Size * 2)
    $canvas = [System.Drawing.Bitmap]::new($canvasWidth, $height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($canvas)
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $row = [byte[]]::new($canvasWidth * 4)
    $alpha = [byte[]]::new($canvasWidth * $height)
    $bounds = [System.Drawing.Rectangle]::new(0, 0, $canvasWidth, $height)
    $origin = [System.Drawing.PointF]::new(2, 1)
    $measure = [System.Drawing.SizeF]::new($canvasWidth, $height)
    $glyphs = [System.Collections.Generic.List[string]]::new()
    $pixels = [System.Collections.Generic.List[byte]]::new()
    try {
        foreach ($point in $codepoints) {
            $text = [string][char]$point
            $advance = [Math]::Max(1, [int][Math]::Round($graphics.MeasureString($text, $font, $measure, $format).Width))
            $width = 0
            if ($point -ne 32) {
                $graphics.Clear([System.Drawing.Color]::Transparent)
                $graphics.DrawString($text, $font, [System.Drawing.Brushes]::White, $origin, $format)
                $bits = $canvas.LockBits($bounds, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $canvas.PixelFormat)
                try {
                    for ($y = 0; $y -lt $height; $y++) {
                        $address = [IntPtr]::Add($bits.Scan0, $y * $bits.Stride)
                        [Runtime.InteropServices.Marshal]::Copy($address, $row, 0, $row.Length)
                        for ($x = 0; $x -lt $canvasWidth; $x++) {
                            $opacity = $row[4 * $x + 3]
                            $alpha[$y * $canvasWidth + $x] = $opacity
                            if ($opacity -ne 0 -and $x -ge $width) { $width = $x + 1 }
                        }
                    }
                } finally {
                    $canvas.UnlockBits($bits)
                }
            }

            $glyphs.Add(('    {{ {0}, {1}, {2} }},' -f $width, $advance, $pixels.Count))
            for ($y = 0; $y -lt $height; $y++) {
                for ($x = 0; $x -lt $width; $x++) { $pixels.Add($alpha[$y * $canvasWidth + $x]) }
            }
        }
    } finally {
        $graphics.Dispose()
        $canvas.Dispose()
        $font.Dispose()
    }

    [void]$header.AppendLine('')
    [void]$header.AppendLine(('#define STORE_{0}_HEIGHT {1}' -f $Name.ToUpperInvariant(), $height))
    [void]$header.AppendLine(('static const StoreGlyph store_{0}_glyphs[] = {{' -f $Name))
    foreach ($glyph in $glyphs) { [void]$header.AppendLine($glyph) }
    [void]$header.AppendLine('};')
    [void]$header.AppendLine(('static const unsigned char store_{0}_pixels[] = {{' -f $Name))
    for ($position = 0; $position -lt $pixels.Count; $position += 24) {
        $count = [Math]::Min(24, $pixels.Count - $position)
        [void]$header.AppendLine('    ' + [string]::Join(', ', $pixels.GetRange($position, $count)) + ',')
    }
    [void]$header.AppendLine('};')
}

try {
    Add-StoreFont 'small' 21 ([System.Drawing.FontStyle]::Regular)
    Add-StoreFont 'large' 28 ([System.Drawing.FontStyle]::Bold)
} finally {
    $format.Dispose()
    $family.Dispose()
    $fonts.Dispose()
}

[void]$header.AppendLine('')
[void]$header.AppendLine('#endif')
$directory = (New-Item -ItemType Directory -Force $OutputDirectory).FullName
[System.IO.File]::WriteAllText((Join-Path $directory 'store-font.h'), $header.ToString(), [System.Text.Encoding]::ASCII)
