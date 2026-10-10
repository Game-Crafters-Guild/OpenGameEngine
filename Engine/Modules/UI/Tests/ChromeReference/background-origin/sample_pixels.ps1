# Samples the background-origin probe points from a Chrome screenshot of
# chrome_fixture.html. Fails loudly (NONUNIFORM) when a patch straddles an edge,
# so a layout drift between this mirror and the engine fixture
# (Engine/Modules/UI/Tests/BackgroundOriginPaddingBoxTests.cpp) cannot silently
# shift a mean.
#
# Render first:
#   chrome.exe --headless=new --force-device-scale-factor=1 --window-size=800,600
#              --screenshot=chrome.png chrome_fixture.html
#
# Probes sit in border-box coordinates (the fixture is box-sizing: border-box,
# matching what the engine's CSS resolves): #pad (20,20,200,120),
# #sq (260,20,100,100) r20, #cov (20,180,200,120), #opq (260,180,200,120),
# #tile (20,340,200,120). Each padding box is that inset by the 10px border.
param([string]$Png = "$PSScriptRoot\chrome.png",
      [int]$Radius = 2)
Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile($Png)

function Patch([int]$x, [int]$y, [string]$name, [int]$r = $Radius) {
    $c0 = $bmp.GetPixel($x, $y)
    for ($dy = -$r; $dy -le $r; $dy++) {
        for ($dx = -$r; $dx -le $r; $dx++) {
            $c = $bmp.GetPixel($x + $dx, $y + $dy)
            if ($c.R -ne $c0.R -or $c.G -ne $c0.G -or $c.B -ne $c0.B) {
                Write-Output ("{0}: NONUNIFORM at ({1},{2}) {3},{4},{5} vs {6},{7},{8}" -f $name, ($x+$dx), ($y+$dy), $c.R, $c.G, $c.B, $c0.R, $c0.G, $c0.B)
                return
            }
        }
    }
    Write-Output ("{0} ({1},{2}): {3},{4},{5}" -f $name, $x, $y, $c0.R, $c0.G, $c0.B)
}

Patch 400 10  "backdrop"
# contain in a non-square box: the image stops at the padding box, so the band
# reads border-over-FILL. That byte IS the padding-box positioning area.
Patch 120 25  "pad.top.band"
Patch 25  80  "pad.left.band"
Patch 120 80  "pad.interior"
Patch 120 32  "pad.inside.padtop"
# Square + rounded: the shipped thumbnail shape.
Patch 310 25  "sq.top.band.straight"
# Single-pixel: (270,30) is already the padding-box corner, where the value
# flips to border-over-image, so no patch fits outside it.
Patch 269 29  "sq.corner45.outer" 0
# The square image's corner reaches into the corner band, where CSS paints the
# border OVER it. Single-pixel sample: the band is ~2px wide on the diagonal.
Patch 272 32  "sq.corner45.inner" 0
Patch 310 70  "sq.interior"
# cover overflows the padding box, so the band legitimately has image behind it.
Patch 120 185 "cov.top.band"
Patch 25  240 "cov.left.band"
Patch 120 240 "cov.interior"
# Opaque border: model-invariant.
Patch 360 185 "opq.top.band"
Patch 360 240 "opq.interior"
# Tiling: phase anchored at the padding box (first tile's yellow quadrant
# starts at x=30, not x=20) and tiles continue under the border.
Patch 25  355 "tile.left.band" 1
Patch 35  355 "tile.first.yellow"
Patch 45  355 "tile.first.magenta"
$bmp.Dispose()
