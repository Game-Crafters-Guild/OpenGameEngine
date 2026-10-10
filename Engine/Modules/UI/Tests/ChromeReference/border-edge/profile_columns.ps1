# Per-column intensity profile across a horizontal span of a PNG.
# A border edge reads as a column ramp; integrating the ramp gives the edge's
# ink in device pixels, which is what "left thinner than right" means
# numerically. Usage:
#   profile_columns.ps1 -Path shot.png -X0 10 -X1 60 -Y0 200 -Y1 300
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][int]$X0,
    [Parameter(Mandatory = $true)][int]$X1,
    [Parameter(Mandatory = $true)][int]$Y0,
    [Parameter(Mandatory = $true)][int]$Y1
)

Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Path))
try {
    Write-Output ("image {0}x{1}  band y=[{2}..{3}]" -f $bmp.Width, $bmp.Height, $Y0, $Y1)
    Write-Output "   x      R      G      B"
    for ($x = $X0; $x -le $X1; $x++) {
        if ($x -lt 0 -or $x -ge $bmp.Width) { continue }
        $sr = 0.0; $sg = 0.0; $sb = 0.0; $n = 0
        for ($y = $Y0; $y -le $Y1; $y++) {
            if ($y -lt 0 -or $y -ge $bmp.Height) { continue }
            $c = $bmp.GetPixel($x, $y)
            $sr += $c.R; $sg += $c.G; $sb += $c.B; $n++
        }
        if ($n -eq 0) { continue }
        Write-Output ("{0,4} {1,6:N1} {2,6:N1} {3,6:N1}" -f $x, ($sr / $n), ($sg / $n), ($sb / $n))
    }
}
finally { $bmp.Dispose() }
