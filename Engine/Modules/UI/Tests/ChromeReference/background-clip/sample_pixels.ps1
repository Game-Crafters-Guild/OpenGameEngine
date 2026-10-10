# Samples 5x5-uniform patches from a screenshot at the #805 probe points.
# Fails loudly (NONUNIFORM) if any patch straddles an edge, so a layout drift
# between the HTML mirror and the engine fixture cannot silently shift a mean.
# -Png defaults to chrome.png beside this script: a Chrome screenshot of chrome_fixture.html.
param([string]$Png = (Join-Path $PSScriptRoot "chrome.png"))
Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile($Png)

function Patch([int]$x, [int]$y, [string]$name) {
    $c0 = $bmp.GetPixel($x, $y)
    for ($dy = -2; $dy -le 2; $dy++) {
        for ($dx = -2; $dx -le 2; $dx++) {
            $c = $bmp.GetPixel($x + $dx, $y + $dy)
            if ($c.R -ne $c0.R -or $c.G -ne $c0.G -or $c.B -ne $c0.B) {
                Write-Output ("{0}: NONUNIFORM at ({1},{2}) {3},{4},{5} vs {6},{7},{8}" -f $name, ($x+$dx), ($y+$dy), $c.R, $c.G, $c.B, $c0.R, $c0.G, $c0.B)
                return
            }
        }
    }
    Write-Output ("{0} ({1},{2}): {3},{4},{5}" -f $name, $x, $y, $c0.R, $c0.G, $c0.B)
}

Patch 120 10  "backdrop"
Patch 25  80  "hud.band"
Patch 120 80  "hud.interior"
Patch 265 80  "tt.band"
Patch 360 80  "tt.interior"
Patch 505 80  "rad.band.straight"
Patch 509 29  "rad.band.corner45"
Patch 600 80  "rad.interior"
Patch 25  240 "op.band"
Patch 120 240 "op.interior"
Patch 360 240 "nb.centre"
$bmp.Dispose()
