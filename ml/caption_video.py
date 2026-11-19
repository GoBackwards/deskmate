"""Burn bakeoff-style trial captions into a dashboard recording and encode it as H.264.

The captions go in the empty strip under the camera view (x 24-664, y 586-680 of the 1280x720
dashboard), so nothing the board reports is covered. Times come from the recording's own state
banner (see the timeline in the comments below).

  .venv\\Scripts\\python -X utf8 ml\\caption_video.py runs\\demo_20261118_184735.mp4 out.mp4
"""
import subprocess
import sys

import cv2
import imageio_ffmpeg
import numpy as np
from PIL import Image, ImageDraw, ImageFont

# (start s, end s, caption) for demo_20261118_184735.mp4; state segments measured from the banner:
# IDLE 0-2.9, FOCUS 3.0-9.6, GRACE 9.7-13.2, FOCUS 13.3-20.0, GRACE 20.1-23.6, NUDGE 23.7-42.1,
# FOCUS 42.2-48.8, BREAK 48.9-56.5, FOCUS 56.6-62.2.
CAPTIONS = [
    (0.0, 3.0, "Neutral: empty bay → idle"),
    (3.0, 9.7, "Phone parked → FOCUS, timer runs"),
    (9.7, 13.3, "Phone lifted → grace countdown"),
    (13.3, 20.1, "Second phone parked → FOCUS again"),
    (20.1, 23.7, "Phone lifted → grace countdown…"),
    (23.7, 27.0, "…5 s later: NUDGE (red LED, chime)"),
    (27.0, 42.2, "Decoys (earbuds case, pen) are not a phone:\nthe nudge stays"),
    (42.2, 48.9, "Phone back → FOCUS"),
    (48.9, 56.6, "BOOT button → BREAK: phone allowed, no nudge"),
    (56.6, 62.4, "Phone back → FOCUS"),
]
TAG = "Live run, real time · every decision is made on the ESP32-S3"
BOX = (24, 588, 664, 676)
FONT = r"C:\Windows\Fonts\seguisb.ttf"


def caption_at(t):
    for a, b, text in CAPTIONS:
        if a <= t < b:
            return text
    return None


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    cap = cv2.VideoCapture(src)
    fps = cap.get(cv2.CAP_PROP_FPS) or 10.0
    w, h = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)), int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    ff = imageio_ffmpeg.get_ffmpeg_exe()
    enc = subprocess.Popen([ff, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgr24",
                            "-s", f"{w}x{h}", "-r", str(fps), "-i", "-", "-c:v", "libx264", "-pix_fmt", "yuv420p",
                            "-crf", "22", "-preset", "slow", "-movflags", "+faststart", "-an", dst],
                           stdin=subprocess.PIPE)
    f_cap = ImageFont.truetype(FONT, 26)
    f_tag = ImageFont.truetype(FONT, 18)
    i = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        t = i / fps
        img = Image.fromarray(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
        d = ImageDraw.Draw(img, "RGBA")
        d.text((700, 22), TAG, font=f_tag, fill=(232, 163, 61))
        text = caption_at(t)
        if text:
            d.rounded_rectangle(BOX, radius=12, fill=(232, 163, 61, 235))
            lines = text.split("\n")
            y = BOX[1] + (BOX[3] - BOX[1] - 32 * len(lines)) / 2
            for line in lines:
                d.text((BOX[0] + 18, y), line, font=f_cap, fill=(20, 28, 36))
                y += 32
        enc.stdin.write(cv2.cvtColor(np.asarray(img), cv2.COLOR_RGB2BGR).tobytes())
        i += 1
    enc.stdin.close()
    enc.wait()
    print(f"wrote {dst}: {i} frames at {fps} fps")
    return enc.returncode


if __name__ == "__main__":
    sys.exit(main())
