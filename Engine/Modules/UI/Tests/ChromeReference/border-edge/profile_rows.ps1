# Per-row intensity profile down a vertical span of a PNG — the horizontal
# counterpart of profile_columns.ps1, for top/bottom border edges.
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
    Write-Output ("image {0}x{1}  band x=[{2}..{3}]" -f $bmp.Width, $bmp.Height, $X0, $X1)
    Write-Output "   y      R      G      B"
    for ($y = $Y0; $y -le $Y1; $y++) {
        if ($y -lt 0 -or $y -ge $bmp.Height) { continue }
        $sr = 0.0; $sg = 0.0; $sb = 0.0; $n = 0
        for ($x = $X0; $x -le $X1; $x++) {
            if ($x -lt 0 -or $x -ge $bmp.Width) { continue }
            $c = $bmp.GetPixel($x, $y)
            $sr += $c.R; $sg += $c.G; $sb += $c.B; $n++
        }
        if ($n -eq 0) { continue }
        Write-Output ("{0,4} {1,6:N1} {2,6:N1} {3,6:N1}" -f $y, ($sr / $n), ($sg / $n), ($sb / $n))
    }
}
finally { $bmp.Dispose() }
