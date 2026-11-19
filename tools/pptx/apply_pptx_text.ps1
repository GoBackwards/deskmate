# Replace a placeholder (the team ID) in every slide shape, set each slide's speaker notes from
# notes_N.txt, and save. PowerPoint COM keeps the embedded video and all formatting intact.
param([string]$Path, [string]$NotesDir, [string]$Find, [string]$Repl)
$ErrorActionPreference = 'Stop'
$ppt = New-Object -ComObject PowerPoint.Application
$openBefore = $ppt.Presentations.Count
$pres = $ppt.Presentations.Open($Path, 0, 0, 0)   # read-write, no window
$script:count = 0

function Fix-Range($tr) {
    $guard = 0
    while ($tr.Text.Contains($Find) -and $guard -lt 20) {
        $null = $tr.Replace($Find, $Repl)
        $script:count++; $guard++
    }
}

function Fix-Shape($sh) {
    if ($sh.Type -eq 6) { foreach ($g in $sh.GroupItems) { Fix-Shape $g }; return }   # msoGroup
    if ($sh.HasTable -eq -1) {
        $t = $sh.Table
        for ($r = 1; $r -le $t.Rows.Count; $r++) {
            for ($c = 1; $c -le $t.Columns.Count; $c++) { Fix-Range $t.Cell($r, $c).Shape.TextFrame.TextRange }
        }
        return
    }
    if ($sh.HasTextFrame -eq -1) { Fix-Range $sh.TextFrame.TextRange }
}

try {
    for ($i = 1; $i -le $pres.Slides.Count; $i++) {
        $slide = $pres.Slides.Item($i)
        foreach ($sh in $slide.Shapes) { Fix-Shape $sh }
        $f = Join-Path $NotesDir ("notes_{0}.txt" -f $i)
        if (Test-Path -LiteralPath $f) {
            $text = (Get-Content -LiteralPath $f -Raw -Encoding UTF8).Trim()
            $set = $false
            foreach ($sh in $slide.NotesPage.Shapes) {
                if ($sh.Type -eq 14 -and $sh.PlaceholderFormat.Type -eq 2) {   # notes body placeholder
                    $sh.TextFrame.TextRange.Text = $text; $set = $true
                }
            }
            "slide {0}: notes set {1}" -f $i, $set
        }
    }
    "replaced '{0}' {1} times" -f $Find, $script:count
    $pres.Save()
    "saved"
}
finally {
    $pres.Close()
    if ($openBefore -eq 0 -and $ppt.Presentations.Count -eq 0) { $ppt.Quit() }   # never close the user's own decks
}
