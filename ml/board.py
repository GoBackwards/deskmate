"""Serial link to the Deskmate demo firmware (firmware/main/app_main.cpp).

Opens the CH343 port with DTR/RTS off (opening must not reset the board), reads lines on a
background thread and turns them into dicts:
  {"kind": "ST", "frame": 12, "state": "FOCUS", "focus": 61000, ...}
  {"kind": "DET", "frame": 12, "dets": [(cls, score, x1, y1, x2, y2), ...], "model_ms": 794.0}
  {"kind": "EV", "frame": 12, "event": "NUDGE", "focus": 61000, "pickups": 1}
  {"kind": "IMG", "frame": 12, "jpg": b"..."}
  {"kind": "CFG", "roi": (x1, y1, x2, y2), "session": 1500, ...}
  {"kind": "TEXT", "text": "..."}            (anything else, e.g. boot log)
"""
import base64
import queue
import threading
import time

import serial


def _kv(tokens):
    out = {}
    for t in tokens:
        if "=" in t:
            k, v = t.split("=", 1)
            try:
                out[k] = float(v) if "." in v else int(v)
            except ValueError:
                out[k] = v
    return out


def parse_line(line: str):
    if line.startswith("ST "):
        tok = line.split()
        msg = {"kind": "ST", "frame": int(tok[1]), "state": tok[2]}
        msg.update(_kv(tok[3:]))
        return msg
    if line.startswith("DET "):
        parts = line.split(" | ")
        head = parts[0].split()
        msg = {"kind": "DET", "frame": int(head[1])}
        msg.update(_kv(head[3:]))
        dets = []
        for p in parts[1:]:
            c, s, x1, y1, x2, y2 = p.split()
            dets.append((int(c), float(s), int(x1), int(y1), int(x2), int(y2)))
        msg["dets"] = dets
        return msg
    if line.startswith("EV "):
        tok = line.split()
        msg = {"kind": "EV", "frame": int(tok[1]), "event": tok[2]}
        msg.update(_kv(tok[3:]))
        return msg
    if line.startswith("IMG "):
        _, fr, b64 = line.split(" ", 2)
        return {"kind": "IMG", "frame": int(fr), "jpg": base64.b64decode(b64)}
    if line.startswith("CFG "):
        msg = {"kind": "CFG"}
        msg.update(_kv(line.split()[1:]))
        roi = msg.get("roi")
        if isinstance(roi, str):
            msg["roi"] = tuple(int(v) for v in roi.split(","))
        return msg
    return {"kind": "TEXT", "text": line}


class Board:
    def __init__(self, port="COM4", baud=921600, reset=False):
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, baud, 0.1
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        try:  # Windows: the default 4 KB driver buffer overflows in ~45 ms at 921600 baud
            self.ser.set_buffer_size(rx_size=4 * 1024 * 1024, tx_size=64 * 1024)
        except (AttributeError, serial.SerialException):
            pass
        if reset:  # RTS pulls EN low on this board's auto-reset circuit
            self.ser.rts = True
            time.sleep(0.1)
            self.ser.rts = False
        self.q = queue.Queue()
        self._stop = False
        self._t = threading.Thread(target=self._reader, daemon=True)
        self._t.start()

    def _reader(self):
        buf = b""
        while not self._stop:
            try:
                chunk = self.ser.read(65536)
            except serial.SerialException as e:
                self.q.put({"kind": "TEXT", "text": f"serial error: {e}"})
                return
            if not chunk:
                continue
            buf += chunk
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                line = raw.decode("ascii", "replace").strip()
                if not line:
                    continue
                try:
                    self.q.put(parse_line(line))
                except (ValueError, IndexError):
                    self.q.put({"kind": "TEXT", "text": line[:200]})

    def send(self, cmd: str):
        self.ser.write((cmd.strip() + "\n").encode())

    def messages(self, timeout=0.0):
        """All messages waiting now (blocks up to `timeout` for the first one)."""
        out = []
        try:
            out.append(self.q.get(timeout=timeout) if timeout else self.q.get_nowait())
            while True:
                out.append(self.q.get_nowait())
        except queue.Empty:
            pass
        return out

    def close(self):
        self._stop = True
        self._t.join(timeout=1)
        self.ser.close()
