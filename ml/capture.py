"""Guided capture of phone-pad images from the board's own camera (training and test data).

The board streams frames (capture mode, no inference); this tool shows them with the pad box,
tells you what to put on the pad, and saves every frame of a step under that step's label:
  data/pad/<session>/<NN>_<label>/<frame>.jpg   and   data/pad/<session>/session.json (pad box, steps)

  .venv\\Scripts\\python -X utf8 ml\\capture.py --session s1            # training set, ~4 min
  .venv\\Scripts\\python -X utf8 ml\\capture.py --session t1 --short    # held-out test set, ~2 min
  .venv\\Scripts\\python -X utf8 ml\\capture.py --session e1 --short --eval
      # on-device accuracy: the board runs its pad model (person detector off for speed) and the
      # tool scores the board's p(phone) against the step labels.
SPACE starts each step, ESC quits. Labels: phone = 1; empty and other = 0.
"""
import argparse
import json
import os
import sys
import time

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from board import Board  # noqa: E402

STEPS = [
    ("phone", 35, "Phone in the bay, BACK facing up (screen down). Slide and turn it to different "
                  "spots and angles. Two phones? Swap them halfway."),
    ("phone", 30, "Phone in the bay, SCREEN facing up. Different spots and angles; touch it, lift "
                  "your hand away. Two phones? Swap them halfway."),
    ("empty", 25, "Bay EMPTY (no phone). Move your hands over the bay and in and out of it."),
    ("other", 40, "NO phone: put other things in the bay, one at a time: wallet, keys, earbuds "
                  "case, card, pen..."),
    ("phone", 20, "Phone in the bay (either side). Lean over it so your head and hands shade it."),
    ("empty", 20, "Bay EMPTY again. Lean over it and move around."),
    ("phone", 20, "Phone in the bay with clutter next to it (keys, pen, earbuds), or both phones "
                  "together."),
]
SHORT = [(lab, max(12, sec // 2), txt) for lab, sec, txt in STEPS]


def wrap(text, n=58):
    words, lines, cur = text.split(), [], ""
    for w in words:
        if len(cur) + len(w) + 1 > n:
            lines.append(cur)
            cur = w
        else:
            cur = (cur + " " + w).strip()
    return lines + [cur]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--session", required=True)
    ap.add_argument("--short", action="store_true")
    ap.add_argument("--eval", action="store_true", help="score the board's own pad model instead of capturing")
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "pad"))
    args = ap.parse_args()
    sys.stdout.reconfigure(errors="replace")

    out = os.path.abspath(os.path.join(args.root, args.session))
    if os.path.exists(os.path.join(out, "session.json")):
        print(f"{out} already has a session; pick another --session name")
        return 1
    os.makedirs(out, exist_ok=True)
    steps = SHORT if args.short else STEPS

    b = Board(args.port)
    cfg = {}
    b.send("?")
    person_before = 1
    t_wait = time.time()
    while time.time() - t_wait < 3 and not cfg:  # the board's settings before we change anything
        for m in b.messages(timeout=0.2):
            if m["kind"] == "CFG":
                cfg = m
                person_before = int(m.get("person", 1))
    if args.eval:
        b.send("c0")
        b.send("p0")   # person detector off: the pad model alone runs at several frames per second
        b.send("i1")
        b.send("q70")
    else:
        b.send("c1")
    b.send("?")

    win = "Deskmate capture"
    cv2.namedWindow(win, cv2.WINDOW_AUTOSIZE)
    drag = {"box": None, "recording": False}

    def on_mouse(ev, x, y, flags, _):  # drag on the camera view = mark the pad (not while recording)
        cx, cy = min(max(x / 2, 0), 320), min(max(y / 2, 0), 240)
        if drag["recording"] or x > 640:
            return
        if ev == cv2.EVENT_LBUTTONDOWN:
            drag["box"] = [cx, cy, cx, cy]
        elif ev == cv2.EVENT_MOUSEMOVE and drag["box"]:
            drag["box"][2], drag["box"][3] = cx, cy
        elif ev == cv2.EVENT_LBUTTONUP and drag["box"]:
            x1, y1, x2, y2 = drag["box"]
            x1, x2 = sorted((x1, x2))
            y1, y2 = sorted((y1, y2))
            if x2 - x1 >= 16 and y2 - y1 >= 16:
                b.send(f"r {int(x1)} {int(y1)} {int(x2)} {int(y2)}")
            drag["box"] = None

    cv2.setMouseCallback(win, on_mouse)
    frame_img = np.zeros((240, 320, 3), np.uint8)
    record = []  # per step: {"label","dir","n", ...}
    eval_rows = []
    step_i, recording, t_end = 0, False, 0.0
    step_dir, n_saved = None, 0
    last_st = None
    try:
        while step_i < len(steps):
            for m in b.messages(timeout=0.05):
                if m["kind"] == "CFG":
                    cfg = m
                elif m["kind"] == "ST":
                    last_st = m
                    if recording and args.eval:
                        lab = steps[step_i][0]
                        eval_rows.append({"step": step_i, "label": lab, "p": float(m.get("p", 0)),
                                          "phone": int(m.get("phone", 0)), "pad_ms": float(m.get("pad_ms", 0))})
                elif m["kind"] == "IMG":
                    img = cv2.imdecode(np.frombuffer(m["jpg"], np.uint8), cv2.IMREAD_COLOR)
                    if img is not None:
                        frame_img = img
                        if recording:
                            with open(os.path.join(step_dir, f"{m['frame']:06d}.jpg"), "wb") as f:
                                f.write(m["jpg"])
                            n_saved += 1
                elif m["kind"] == "TEXT":
                    print(m["text"], flush=True)

            label, secs, prompt = steps[step_i]
            view = cv2.resize(frame_img, (640, 480), interpolation=cv2.INTER_NEAREST)
            roi = cfg.get("roi")
            if roi:
                x1, y1, x2, y2 = (v * 2 for v in roi)
                cv2.rectangle(view, (x1, y1), (x2, y2), (60, 220, 60) if label == "phone" else (200, 200, 200), 2)
            if drag["box"]:
                x1, y1, x2, y2 = (int(v * 2) for v in drag["box"])
                cv2.rectangle(view, (x1, y1), (x2, y2), (0, 255, 255), 1)
            panel = np.full((480, 520, 3), 30, np.uint8)
            cv2.putText(panel, f"step {step_i + 1}/{len(steps)}   label: {label.upper()}", (16, 40),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.75, (255, 255, 255), 2, cv2.LINE_AA)
            for j, line in enumerate(wrap(prompt)):
                cv2.putText(panel, line, (16, 90 + 30 * j), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (220, 220, 220), 1,
                            cv2.LINE_AA)
            if recording:
                left = max(0.0, t_end - time.time())
                cv2.putText(panel, f"REC  {left:4.1f} s   frames {n_saved}", (16, 300),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.9, (60, 60, 255), 2, cv2.LINE_AA)
                if args.eval and last_st is not None:
                    cv2.putText(panel, f"board p(phone) = {float(last_st.get('p', 0)):.2f}", (16, 345),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (200, 255, 200), 2, cv2.LINE_AA)
                if left <= 0:
                    recording = False
                    drag["recording"] = False
                    record.append({"label": label, "dir": os.path.basename(step_dir), "n": n_saved,
                                   "seconds": secs, "prompt": prompt, "roi": list(cfg.get("roi", []))})
                    print(f"step {step_i + 1}: {label} {n_saved} frames", flush=True)
                    step_i += 1
            else:
                cv2.putText(panel, "arrange the pad, then press SPACE", (16, 300), cv2.FONT_HERSHEY_SIMPLEX, 0.7,
                            (80, 220, 255), 2, cv2.LINE_AA)
                cv2.putText(panel, f"records {secs} s", (16, 335), cv2.FONT_HERSHEY_SIMPLEX, 0.6,
                            (200, 200, 200), 1, cv2.LINE_AA)
                if step_i == 0:
                    cv2.putText(panel, "first: drag a box around the pad", (16, 380), cv2.FONT_HERSHEY_SIMPLEX,
                                0.6, (0, 255, 255), 1, cv2.LINE_AA)
                    cv2.putText(panel, "on the camera view (left)", (16, 405), cv2.FONT_HERSHEY_SIMPLEX, 0.6,
                                (0, 255, 255), 1, cv2.LINE_AA)
            cv2.imshow(win, np.hstack([view, panel]))
            k = cv2.waitKey(1) & 0xFF
            if k == 27:
                print("aborted", flush=True)
                break
            if k == ord(" ") and not recording and step_i < len(steps) and cfg.get("roi"):
                step_dir = os.path.join(out, f"{step_i:02d}_{label}")
                os.makedirs(step_dir, exist_ok=True)
                n_saved, recording, t_end = 0, True, time.time() + secs
                drag["recording"] = True
    finally:
        b.send("c0")
        if args.eval:
            b.send(f"p{person_before}")  # back to the setting found at the start
            b.send("q80")
        time.sleep(0.2)
        b.close()
        cv2.destroyAllWindows()

    meta = {"session": args.session, "created": time.strftime("%Y-%m-%d %H:%M:%S"), "roi": list(cfg.get("roi", [])),
            "eval": args.eval, "steps": record, "board_cfg": {k: v for k, v in cfg.items() if k != "kind"}}
    if args.eval and eval_rows:
        y = np.array([r["label"] == "phone" for r in eval_rows])
        p = np.array([r["p"] for r in eval_rows])
        deb = np.array([r["phone"] for r in eval_rows]).astype(bool)
        thr = float(cfg.get("phone_thr", 0.5))
        raw = p >= thr
        meta["eval_result"] = {
            "frames": int(len(y)), "phone_frames": int(y.sum()), "threshold": thr,
            "raw_accuracy": float((raw == y).mean()), "debounced_accuracy": float((deb == y).mean()),
            "raw_false_phone": int((raw & ~y).sum()), "raw_missed_phone": int((~raw & y).sum()),
            "pad_ms_median": float(np.median([r["pad_ms"] for r in eval_rows])),
        }
        with open(os.path.join(out, "eval_rows.json"), "w") as f:
            json.dump(eval_rows, f)
        print("on-device result:", json.dumps(meta["eval_result"], indent=1))
    with open(os.path.join(out, "session.json"), "w") as f:
        json.dump(meta, f, indent=1)
    print("saved", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
