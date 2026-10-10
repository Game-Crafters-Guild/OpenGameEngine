param(
    [Parameter(Mandatory=$true)][string]$Image,
    # Each case: name, scanline y (device px), left window x0..x1, right window x0..x1
    [Parameter(Mandatory=$true)][string]$CasesJson
)
Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile($Image)
$cases = ConvertFrom-Json (Get-Content $CasesJson -Raw)
foreach ($c in $cases) {
    $y = [int]$c.y
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($win in @('L','R')) {
        $x0 = if ($win -eq 'L') { [int]$c.lx0 } else { [int]$c.rx0 }
        $x1 = if ($win -eq 'L') { [int]$c.lx1 } else { [int]$c.rx1 }
        $cols = @()
        $sum = 0.0
        for ($x = $x0; $x -le $x1; $x++) {
            $px = $bmp.GetPixel($x, $y)
            # Border is white-ish over dark bg/black page: use max channel as intensity.
            $v = [Math]::Max($px.R, [Math]::Max($px.G, $px.B))
            $cols += ('{0}:{1}' -f $x, $v)
            $sum += $v
        }
        $out.Add(('{0} edge cols [{1}]' -f $win, ($cols -join ' ')))
    }
    Write-Output ('== {0} (y={1})' -f $c.name, $y)
    $out | ForEach-Object { Write-Output ('   ' + $_) }
}
$bmp.Dispose()
