# Keep the footer "Team <id> · Deskmate" on one line: no word wrap, box sized to its text. Then save.
param([string]$Path, [string]$Prefix)
$ErrorActionPreference = 'Stop'
$ppt = New-Object -ComObject PowerPoint.Application
$openBefore = $ppt.Presentations.Count
$pres = $ppt.Presentations.Open($Path, 0, 0, 0)   # read-write, no window
try {
    foreach ($slide in $pres.Slides) {
        foreach ($sh in $slide.Shapes) {
            if ($sh.HasTextFrame -eq -1 -and $sh.TextFrame.TextRange.Text.StartsWith($Prefix)) {
                $w0 = $sh.Width
                $sh.TextFrame.WordWrap = 0              # msoFalse
                $sh.TextFrame.AutoSize = 1              # ppAutoSizeShapeToFitText
                "slide {0}: '{1}' width {2:N1} -> {3:N1} pt, left {4:N1}, lines {5}" -f $slide.SlideIndex, $sh.TextFrame.TextRange.Text, $w0, $sh.Width, $sh.Left, $sh.TextFrame.TextRange.Lines().Count
            }
        }
    }
    $pres.Save()
    "saved"
}
finally {
    $pres.Close()
    if ($openBefore -eq 0 -and $ppt.Presentations.Count -eq 0) { $ppt.Quit() }
}
