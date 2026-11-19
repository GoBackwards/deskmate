"""Deskmate midterm demo dashboard.

Everything that senses, infers and decides runs on the ESP32-S3. This window only draws what
the board reports (camera frame, pad box, person boxes, state, timer), mirrors its LED, plays
the chime the board's events ask for, and sends setup commands.

Run (from the repository root):   .venv\\Scripts\\python -X utf8 ml\\dashboard.py
Mouse: drag a box on the camera view to mark the phone pad.
Keys:  n new session   b break (same as the board's BOOT button)   d demo timings (2 min, 5 s grace)
       f full timings (25 min, 10 s grace)   v start/stop recording a video   s snapshot   q quit
"""
import argparse
import math
import os
import struct
import sys
import tempfile
import time
import wave

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from board import Board  # noqa: E402

W, H = 1280, 720
CAM_X, CAM_Y, CAM_S = 24, 96, 2  # camera view position and scale (320x240 -> 640x480)
PANEL_X = 700

STATE_TEXT = {
    "IDLE": ("PARK YOUR PHONE", "to start a focus session", (200, 200, 200)),
    "FOCUS": ("FOCUS", "phone parked, timer running", (80, 200, 80)),
    "GRACE": ("PHONE OFF THE PAD", "put it back to keep focusing", (40, 200, 230)),
    "NUDGE": ("PUT THE PHONE BACK", "timer paused", (60, 60, 235)),
    "AWAY": ("AWAY", "timer paused until you are back", (230, 150, 60)),
    "BREAK": ("BREAK", "phone allowed, timer paused", (200, 90, 190)),
    "DONE": ("SESSION DONE", "take your phone, enjoy the break", (80, 220, 120)),
}
LED_BGR = {
    "off": (40, 40, 40), "white": (220, 220, 220), "white-breathe": (170, 170, 170), "green": (60, 220, 60),
    "yellow-blink": (40, 210, 240), "red-fast-blink": (40, 40, 240), "blue": (240, 120, 40),
    "purple": (220, 80, 180), "rainbow": (60, 200, 240), "cyan": (230, 220, 40), "red": (40, 40, 240),
}


def tone_wav(path, notes, rate=22050):
    """notes: list of (freq_hz, seconds); a soft bell envelope on each."""
    frames = []
    for f, dur in notes:
        n = int(rate * dur)
        for i in range(n):
            t = i / rate
            env = math.exp(-4.0 * t / dur) * min(1.0, t * 200)
            v = 0.45 * env * (math.sin(2 * math.pi * f * t) + 0.3 * math.sin(4 * math.pi * f * t))
            frames.append(struct.pack("<h", int(max(-1, min(1, v)) * 32767)))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(frames))


class Sounds:
    def __init__(self):
        self.ok = sys.platform == "win32"
        if not self.ok:
            return
        import winsound
        self.ws = winsound
        d = tempfile.mkdtemp(prefix="deskmate_snd_")
        self.files = {
            "SESSION_START": [(523, 0.18), (784, 0.35)],
            "PHONE_BACK": [(659, 0.3)],
            "NUDGE": [(880, 0.15), (880, 0.15), (880, 0.3)],
            "DONE": [(523, 0.15), (659, 0.15), (784, 0.15), (1047, 0.5)],
            "BREAK_START": [(440, 0.3)],
            "AWAY": [(392, 0.3)],
        }
        for k, notes in list(self.files.items()):
            p = os.path.join(d, k + ".wav")
            tone_wav(p, notes)
            self.files[k] = p

    def play(self, event):
        if self.ok and event in self.files:
            self.ws.PlaySound(self.files[event], self.ws.SND_FILENAME | self.ws.SND_ASYNC)


def put(img, text, org, scale=0.6, color=(230, 230, 230), thick=1):
    cv2.putText(img, text, org, cv2.FONT_HERSHEY_SIMPLEX, scale, color, thick, cv2.LINE_AA)


def fmt_ms(ms):
    s = max(0, int(ms // 1000))
    return f"{s // 60:02d}:{s % 60:02d}"


class Dashboard:
    def __init__(self, board, team, record_dir):
        self.b = board
        self.team = team
        self.record_dir = record_dir
        self.cam = np.zeros((240, 320, 3), np.uint8)
        self.st = {}
        self.st_time = time.time()
        self.det = {}
        self.cfg = {}
        self.events = []
        self.log = []
        self.sounds = Sounds()
        self.drag = None
        self.writer = None
        self.last_write = 0.0

    # ---------------------------------------------------------------- input
    def on_mouse(self, ev, x, y, flags, _):
        cx = (x - CAM_X) / CAM_S
        cy = (y - CAM_Y) / CAM_S
        inside = 0 <= cx <= 320 and 0 <= cy <= 240
        if ev == cv2.EVENT_LBUTTONDOWN and inside:
            self.drag = [cx, cy, cx, cy]
        elif ev == cv2.EVENT_MOUSEMOVE and self.drag:
            self.drag[2], self.drag[3] = min(max(cx, 0), 320), min(max(cy, 0), 240)
        elif ev == cv2.EVENT_LBUTTONUP and self.drag:
            x1, y1, x2, y2 = self.drag
            x1, x2 = sorted((x1, x2))
            y1, y2 = sorted((y1, y2))
            if x2 - x1 >= 16 and y2 - y1 >= 16:
                self.b.send(f"r {int(x1)} {int(y1)} {int(x2)} {int(y2)}")
            self.drag = None

    def poll(self):
        for m in self.b.messages():
            k = m["kind"]
            if k == "IMG":
                img = cv2.imdecode(np.frombuffer(m["jpg"], np.uint8), cv2.IMREAD_COLOR)
                if img is not None:
                    self.cam = img
            elif k == "ST":
                self.st = m
                self.st_time = time.time()
            elif k == "DET":
                self.det = m
            elif k == "CFG":
                self.cfg = m
            elif k == "EV":
                self.events.insert(0, (time.strftime("%H:%M:%S"), m["event"]))
                self.events = self.events[:7]
                self.sounds.play(m["event"])
            elif k == "TEXT":
                self.log.insert(0, m["text"][:90])
                self.log = self.log[:3]
                print(m["text"], flush=True)

    # ---------------------------------------------------------------- drawing
    def draw(self):
        img = np.full((H, W, 3), 24, np.uint8)
        cv2.rectangle(img, (0, 0), (W, 64), (45, 45, 45), -1)
        put(img, "Deskmate", (24, 42), 1.1, (255, 255, 255), 2)
        put(img, "park your phone to focus", (210, 42), 0.7, (190, 190, 190))
        if self.team:
            put(img, f"Team {self.team}", (W - 170, 42), 0.7, (190, 190, 190))

        # Camera view with the board's boxes
        view = cv2.resize(self.cam, (320 * CAM_S, 240 * CAM_S), interpolation=cv2.INTER_NEAREST)
        for c, s, x1, y1, x2, y2 in self.det.get("dets", []):
            if c != 0:
                continue
            color = (230, 200, 60) if s >= self.cfg.get("person_thr", 0.5) else (120, 120, 120)
            cv2.rectangle(view, (x1 * CAM_S, y1 * CAM_S), (x2 * CAM_S, y2 * CAM_S), color, 2)
            put(view, f"person {s:.2f}", (x1 * CAM_S + 4, max(16, y1 * CAM_S - 6)), 0.55, color)
        roi = self.cfg.get("roi")
        if roi:
            on = self.st.get("phone", 0) == 1
            p = float(self.st.get("p", 0.0))
            color = (60, 220, 60) if on else ((40, 200, 230) if p >= 0.5 else (200, 200, 200))
            x1, y1, x2, y2 = (v * CAM_S for v in roi)
            cv2.rectangle(view, (x1, y1), (x2, y2), color, 3)
            label = ("PHONE ON PAD" if on else "pad empty") + f"  p={p:.2f}"
            put(view, label, (x1 + 4, y2 - 8 if y2 > 40 else y2 + 20), 0.6, color, 2)
        if self.drag:
            x1, y1, x2, y2 = (int(v * CAM_S) for v in self.drag)
            cv2.rectangle(view, (x1, y1), (x2, y2), (0, 255, 255), 1)
        img[CAM_Y:CAM_Y + 240 * CAM_S, CAM_X:CAM_X + 320 * CAM_S] = view
        cv2.rectangle(img, (CAM_X - 1, CAM_Y - 1), (CAM_X + 320 * CAM_S, CAM_Y + 240 * CAM_S), (90, 90, 90), 1)
        put(img, "ESP32-S3 camera view (drag to mark the phone pad)", (CAM_X, CAM_Y - 12), 0.55, (170, 170, 170))

        # State, timer, LED
        state = self.st.get("state", "IDLE")
        title, sub, color = STATE_TEXT.get(state, (state, "", (220, 220, 220)))
        if state == "GRACE":
            sub = f"nudge in {int(self.st.get('grace', 0)) // 1000 + 1} s - put it back to keep focusing"
        put(img, title, (PANEL_X, 140), 1.3, color, 3)
        put(img, sub, (PANEL_X, 178), 0.65, (200, 200, 200))
        focus = float(self.st.get("focus", 0))
        if state == "FOCUS":  # tick smoothly between the board's updates
            focus += min(2.0, max(0.0, time.time() - self.st_time)) * 1000
        target = float(self.st.get("target", 1500000)) or 1.0
        put(img, fmt_ms(focus), (PANEL_X, 300), 3.2, (255, 255, 255), 6)
        put(img, f"of {fmt_ms(target)} focus", (PANEL_X + 330, 300), 0.7, (180, 180, 180))
        bx, by, bw = PANEL_X, 330, 540
        cv2.rectangle(img, (bx, by), (bx + bw, by + 16), (70, 70, 70), -1)
        done = min(1.0, max(0.0, focus / target))
        if done > 0:
            cv2.rectangle(img, (bx, by), (bx + int(bw * done), by + 16), color, -1)

        led = self.st.get("led", "off")
        blink_off = (led == "yellow-blink" and int(time.time() * 2) % 2) or (
            led == "red-fast-blink" and int(time.time() * 8) % 2)
        lc = (40, 40, 40) if blink_off else LED_BGR.get(led, (80, 80, 80))
        cv2.circle(img, (PANEL_X + 22, 392), 18, lc, -1)
        cv2.circle(img, (PANEL_X + 22, 392), 19, (120, 120, 120), 1)
        put(img, f"board LED: {led}", (PANEL_X + 52, 399), 0.6, (200, 200, 200))

        # Numbers
        y = 448
        person_on = self.cfg.get("person", 1) == 1
        rows = [
            f"phone pickups this session: {self.st.get('pickups', 0)}",
            f"p(phone on pad) {float(self.st.get('p', 0)):.2f}" +
            (f"   person score {float(self.st.get('ps', 0)):.2f}" if person_on else ""),
            "on the ESP32-S3 (int8):",
            f"  pad CNN {float(self.st.get('pad_ms', 0)):.0f} ms   " +
            (f"person YOLO11n {float(self.st.get('det_ms', 0)):.0f} ms   " if person_on else "person detector off   ") +
            f"loop {float(self.st.get('cycle_ms', 0)):.0f} ms",
        ]
        for r in rows:
            put(img, r, (PANEL_X, y), 0.6, (215, 215, 215))
            y += 30
        put(img, "events", (PANEL_X, y + 14), 0.55, (150, 150, 150))
        y += 40
        for t, e in self.events:
            put(img, f"{t}  {e}", (PANEL_X, y), 0.55, (200, 200, 200))
            y += 24

        # Footer
        cv2.rectangle(img, (0, H - 34), (W, H), (45, 45, 45), -1)
        rec = "  REC" if self.writer else ""
        put(img, "drag: mark pad   n: new   b: break   d: demo 2 min   f: full 25 min   v: video   s: snapshot   "
                 "q: quit" + rec, (24, H - 12), 0.5, (200, 200, 200) if not rec else (80, 80, 255))
        return img

    # ---------------------------------------------------------------- main loop
    def run(self):
        win = "Deskmate demo"
        cv2.namedWindow(win, cv2.WINDOW_AUTOSIZE)
        cv2.setMouseCallback(win, self.on_mouse)
        self.b.send("?")
        while True:
            self.poll()
            frame = self.draw()
            cv2.imshow(win, frame)
            now = time.time()
            if self.writer is not None and now - self.last_write >= 0.1:
                self.writer.write(frame)
                self.last_write = now
            k = cv2.waitKey(30) & 0xFF
            if k == ord("q") or cv2.getWindowProperty(win, cv2.WND_PROP_VISIBLE) < 1:
                break
            if k == ord("n"):
                self.b.send("n")
            elif k == ord("b"):
                self.b.send("b")
            elif k == ord("d"):
                self.b.send("s 120 5 5 15")
            elif k == ord("f"):
                self.b.send("s 1500 10 5 30")
            elif k == ord("s"):
                os.makedirs(self.record_dir, exist_ok=True)
                p = os.path.join(self.record_dir, time.strftime("snap_%Y%m%d_%H%M%S.png"))
                cv2.imwrite(p, frame)
                print("snapshot", p, flush=True)
            elif k == ord("v"):
                if self.writer is None:
                    os.makedirs(self.record_dir, exist_ok=True)
                    p = os.path.join(self.record_dir, time.strftime("demo_%Y%m%d_%H%M%S.mp4"))
                    self.writer = cv2.VideoWriter(p, cv2.VideoWriter_fourcc(*"mp4v"), 10, (W, H))
                    print("recording", p, flush=True)
                else:
                    self.writer.release()
                    self.writer = None
                    print("recording stopped", flush=True)
        if self.writer is not None:
            self.writer.release()
        cv2.destroyAllWindows()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--team", default="[ID]")
    ap.add_argument("--reset", action="store_true", help="reset the board when connecting")
    ap.add_argument("--record-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "runs"))
    args = ap.parse_args()
    sys.stdout.reconfigure(errors="replace")
    board = Board(args.port, reset=args.reset)
    try:
        Dashboard(board, args.team, os.path.abspath(args.record_dir)).run()
    finally:
        board.close()


if __name__ == "__main__":
    main()
