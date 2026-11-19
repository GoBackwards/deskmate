# Deskmate: park your phone to focus

Team final_2608 · EC ENGR 209AS final project (UCLA, fall 2026)

Deskmate is a desk study companion built on an ESP32-S3 camera board. You park your phone in a
bay under the camera to start a focus session. A small CNN running on the ESP32-S3 itself (int8,
ESP-DL) checks whether a phone is in the bay; a state machine turns that into focus, grace, nudge
and break; the board shows the state on its RGB LED and asks for a chime when it nudges.

Nothing is inferred on a laptop or in the cloud. The PC tools only record training data, train
and export the model, and display what the board reports.

| Measured on the board, 18 Nov 2026 | |
|---|---|
| Accuracy on a held-out live test round (755 frames: 2 phones, hands, shadows, decoys) | **97.4 %** |
| Non-ML baseline (pixel change from an empty-bay picture), same frames | 92.5 %; 17 false starts on an empty bay (ours: 0) |
| Model | 6-layer CNN, 125 k parameters, int8, 135 KB |
| Inference | 15 ms per frame (96 × 96 input) |

## Hardware

- Freenove ESP32-S3-WROOM CAM board (FNK0085: 8 MB flash, 8 MB PSRAM, GC0308 camera). Use the
  USB-C port next to the BOOT button (CH343 UART).
- A rigid stand that holds the camera straight above the bay. The model learns that exact view:
  if the camera moves, record new data and retrain (about 10 minutes).
- The BOOT button gives a break (phone allowed, no nudge).

## Repository layout

```
firmware/          ESP-IDF project (C++)
  main/            app_main.cpp (loop, serial protocol), pad_classifier.* (CNN), focus_fsm.* (states),
                   cam.*, led.*, settings.* (pad box and timings in NVS), yolo_detector.* (optional)
  models/          pad_cls_96_s8.espdl (our int8 model), .onnx (float), .json (training numbers)
  host_test/       PC unit tests for the focus state machine
ml/                PC tools (Python)
  capture.py       guided recording of training/test frames from the board's camera; --eval scores the board
  train_pad.py     trains the CNN and exports int8 .espdl with ESP-PPQ
  baseline_pad.py  the non-ML baseline
  dashboard.py     live demo window (display only), records demo videos
  board.py         serial link used by the tools
  caption_video.py adds trial captions to a recorded demo video
tools/pptx/        PowerPoint helpers used for the midterm slides (embed and autoplay the demo video)
```

Not in this repository: `data/` (recorded frames), `runs/` (logs and videos), build output, the
Python virtual environment and the optional YOLO model.

## Build and flash the firmware

Needs ESP-IDF v5.5 (tested with v5.5.6). From an ESP-IDF terminal:

```
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COM4 flash
```

`idf.py flash` also writes every `.espdl` in `firmware/models/` to the `models` partition
(`idf.py app-flash` leaves the models alone). After adding or removing a model file, run
`idf.py reconfigure` first. Replace `COM4` with your port.

**Optional person detector.** The firmware can also run Espressif's YOLO11n COCO model
(`coco_detect_yolo11n_320_s8_v1.espdl` from the esp-dl model zoo) to check that someone is at the
desk. It is not included here because it is a third-party model under its own licence. Copy it into
`firmware/models/`, reconfigure and flash. Without it the firmware logs `WARN no person model` and
assumes someone is there; the demo does not need it.

## PC tools

Python 3.12 on Windows (the dashboard's chime uses `winsound`; `caption_video.py` uses a Windows
font).

```
python -m venv .venv
.venv\Scripts\pip install -r ml\requirements.txt
```

The usual loop, from the repository root:

1. Record training and test data from the board's camera:
   `.venv\Scripts\python -X utf8 ml\capture.py --session s1` and `... --session t1 --short`
2. Train and export the int8 model:
   `.venv\Scripts\python -X utf8 ml\train_pad.py --train s1 --test t1`
   (writes `firmware/models/pad_cls_96_s8.espdl`)
3. Flash it (see above).
4. Score the flashed model live on the board, on a new round it has never seen:
   `.venv\Scripts\python -X utf8 ml\capture.py --session e1 --short --eval`
5. Compare with the non-ML baseline: `.venv\Scripts\python -X utf8 ml\baseline_pad.py e1 --fit s1`
6. Demo: `.venv\Scripts\python -X utf8 ml\dashboard.py --team final_2608`
   (drag a box on the camera view to mark the bay; `v` records a video)

All tools take `--port` (default `COM4`).

## Serial protocol

921600 baud on UART0, one line per message.

| Board → PC | |
|---|---|
| `ST <frame> <STATE> focus=… pickups=… phone=<0/1> p=<p> … pad_ms=… cycle_ms=… led=…` | state every frame |
| `EV <frame> <EVENT> focus=… pickups=…` | events such as `NUDGE`, `PHONE_OUT`, `SESSION_START` |
| `IMG <frame> <base64 JPEG>` | camera frame for the dashboard |
| `DET …` | person detections (only with the optional YOLO model) |
| `CFG …` | settings, after boot and after every change |

PC → board: `r x1 y1 x2 y2` (bay box), `s session grace away [renudge]` (timings in seconds),
`b` (break), `n` (new session), `c0/c1` (capture mode), `i0/i1` (images), `q<quality>`,
`p0/p1` (person detector), `d` (defaults), `?`.

## Data

The recorded frames are not public: they show phone lock screens and our desks. Teammates can get
them from the team's shared folder. The format is `data/pad/<session>/<NN>_<label>/*.jpg` plus
`session.json` (bay box and steps); labels are `phone` = 1, `empty` and `other` = 0.

## License

Not chosen yet. Until a license is added, others may view the code but not reuse it.
