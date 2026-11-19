# Make slide 2's video start by itself (After Previous) instead of on the first click, then save.
param([string]$Path)
$ErrorActionPreference = 'Stop'
$ppt = New-Object -ComObject PowerPoint.Application
$openBefore = $ppt.Presentations.Count
$pres = $ppt.Presentations.Open($Path, 0, 0, 0)   # read-write, no window
try {
    $seq = $pres.Slides.Item(2).TimeLine.MainSequence
    $e = $seq.Item(1)
    "before: effect type {0}, trigger {1}" -f $e.EffectType, $e.Timing.TriggerType
    $e.Timing.TriggerType = 3          # msoAnimTriggerAfterPrevious: starts when the slide appears
    $e.Timing.TriggerDelayTime = 0
    "after: effect type {0}, trigger {1}" -f $e.EffectType, $e.Timing.TriggerType
    $pres.Save()
    "saved"
}
finally {
    $pres.Close()
    if ($openBefore -eq 0 -and $ppt.Presentations.Count -eq 0) { $ppt.Quit() }
}
