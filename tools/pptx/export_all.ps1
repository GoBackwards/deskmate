# Export every slide of a deck as PNG through PowerPoint (read-only), for a visual check.
param([string]$Path, [string]$OutDir)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$ppt = New-Object -ComObject PowerPoint.Application
$openBefore = $ppt.Presentations.Count
$pres = $ppt.Presentations.Open($Path, -1, 0, 0)
try {
    for ($i = 1; $i -le $pres.Slides.Count; $i++) {
        $pres.Slides.Item($i).Export((Join-Path $OutDir ("slide{0}.png" -f $i)), 'PNG', 1600, 900)
    }
    "exported {0} slides" -f $pres.Slides.Count
    $fonts = @()
    foreach ($f in $pres.Fonts) { $fonts += ("{0} (embedded={1})" -f $f.Name, $f.Embedded) }
    "fonts used: " + ($fonts -join ', ')
}
finally {
    $pres.Close()
    if ($openBefore -eq 0 -and $ppt.Presentations.Count -eq 0) { $ppt.Quit() }
}
