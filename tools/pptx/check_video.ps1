# Reopen the saved deck in PowerPoint and report slide 2's media shape and playback settings,
# then export slide 2 as a PNG.
param([string]$Path, [string]$Png)
$ErrorActionPreference = 'Stop'
$ppt = New-Object -ComObject PowerPoint.Application
$openBefore = $ppt.Presentations.Count
$pres = $ppt.Presentations.Open($Path, -1, 0, 0)
try {
    "slides: {0}" -f $pres.Slides.Count
    $slide = $pres.Slides.Item(2)
    foreach ($sh in $slide.Shapes) {
        if ($sh.Type -eq 16) {   # msoMedia
            $ps = $sh.AnimationSettings.PlaySettings
            "media shape '{0}': {1} x {2} at ({3}, {4}); length {5} ms; embedded {6}" -f $sh.Name, $sh.Width, $sh.Height, $sh.Left, $sh.Top, $sh.MediaFormat.Length, ($sh.MediaFormat.IsEmbedded)
            "playback: PlayOnEntry {0}, LoopUntilStopped {1}, HideWhileNotPlaying {2}" -f $ps.PlayOnEntry, $ps.LoopUntilStopped, $ps.HideWhileNotPlaying
        }
    }
    $seq = $slide.TimeLine.MainSequence
    for ($i = 1; $i -le $seq.Count; $i++) {
        $e = $seq.Item($i)
        "effect {0}: type {1}, trigger {2} (3 = after previous), shape '{3}'" -f $i, $e.EffectType, $e.Timing.TriggerType, $e.Shape.Name
    }
    $slide.Export($Png, 'PNG', 1600, 900)
    "exported: $Png"
}
finally {
    $pres.Close()
    if ($openBefore -eq 0 -and $ppt.Presentations.Count -eq 0) { $ppt.Quit() }
}
