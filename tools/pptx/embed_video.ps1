# Embed the demo video into slide 2 of the exported deck with PowerPoint itself (COM automation).
# Replaces the still picture "Image 0" at the same position/size; video starts automatically,
# loops until stopped, and shows the still as its poster frame. Saves a NEW file.
param(
    [string]$Src,
    [string]$Out,
    [string]$Mp4,
    [string]$Still
)
$ErrorActionPreference = 'Stop'
$msoTrue = -1; $msoFalse = 0
$ppt = New-Object -ComObject PowerPoint.Application
$openBefore = $ppt.Presentations.Count
$pres = $ppt.Presentations.Open($Src, $msoTrue, $msoFalse, $msoFalse)   # read-only, no window
try {
    $slide = $pres.Slides.Item(2)
    $pic = $slide.Shapes.Item('Image 0')
    "picture: {0} x {1} at ({2}, {3}), z {4}" -f $pic.Width, $pic.Height, $pic.Left, $pic.Top, $pic.ZOrderPosition
    $z = $pic.ZOrderPosition
    $vid = $slide.Shapes.AddMediaObject2($Mp4, $msoFalse, $msoTrue, $pic.Left, $pic.Top, $pic.Width, $pic.Height)
    $vid.Name = 'Demo video'
    $vid.AlternativeText = 'One-minute recording of a live Deskmate run in real time: empty bay, phone parked (focus, green), phone lifted (grace, then a red nudge), decoys that do not stop the nudge, a second phone, and a break with the BOOT button (purple).'
    $vid.MediaFormat.SetDisplayPictureFromFile($Still)
    $ps = $vid.AnimationSettings.PlaySettings
    $ps.PlayOnEntry = $msoTrue          # start automatically with the slide
    $ps.LoopUntilStopped = $msoTrue
    $ps.HideWhileNotPlaying = $msoFalse
    $ps.RewindMovie = $msoTrue
    while ($vid.ZOrderPosition -gt $z) { $vid.ZOrder(3) }   # msoSendBackward: same layer as the old picture
    $pic.Delete()
    $pres.SaveAs($Out)
    "saved: $Out"
}
finally {
    $pres.Close()
    if ($openBefore -eq 0 -and $ppt.Presentations.Count -eq 0) { $ppt.Quit() }   # never close the user's own decks
}
