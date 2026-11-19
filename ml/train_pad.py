"""Train the "phone on the pad?" classifier and export it to ESP-DL int8 for the ESP32-S3.

Data: sessions recorded by capture.py (data/pad/<session>/NN_<label>/*.jpg + session.json).
Each image is cropped to that session's pad box and resized to 96x96 (nearest neighbour, as
ESP-DL does on the board); phone = 1, empty/other = 0.
  * training sessions: the last 20 % of every step is held out for model selection;
  * test sessions (--test): never seen in training; the reported accuracy comes from them.
Output: firmware/models/pad_cls_96_s8.espdl (+ .json with the numbers), and misclassified test
crops in runs/pad_errors/ for a look.

  .venv\\Scripts\\python -X utf8 ml\\train_pad.py --train s1 --test t1
"""
import argparse
import glob
import json
import os
import random
import shutil
import sys

import cv2
import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import DataLoader, Dataset

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SIZE = 96


# ---------------------------------------------------------------- data
def load_session(name):
    d = os.path.join(ROOT, "data", "pad", name)
    meta = json.load(open(os.path.join(d, "session.json")))
    items = []
    for step in meta["steps"]:
        roi = step.get("roi") or meta["roi"]  # pad box at the time of this step
        files = sorted(glob.glob(os.path.join(d, step["dir"], "*.jpg")))
        y = 1 if step["label"] == "phone" else 0
        for i, f in enumerate(files):
            items.append({"path": f, "y": y, "roi": roi, "kind": step["label"], "pos": i / max(1, len(files))})
    return items


def crop(img, roi, size, jitter=0.0, interp=cv2.INTER_NEAREST, rng=None):
    x1, y1, x2, y2 = roi
    if jitter and rng is not None:
        w, h = x2 - x1, y2 - y1
        cx = (x1 + x2) / 2 + rng.uniform(-jitter, jitter) * w
        cy = (y1 + y2) / 2 + rng.uniform(-jitter, jitter) * h
        s = rng.uniform(1 - jitter, 1 + jitter)
        w, h = w * s, h * s
        x1, x2 = int(round(cx - w / 2)), int(round(cx + w / 2))
        y1, y2 = int(round(cy - h / 2)), int(round(cy + h / 2))
    H, W = img.shape[:2]
    x1, y1 = max(0, x1), max(0, y1)
    x2, y2 = min(W, max(x1 + 4, x2)), min(H, max(y1 + 4, y2))
    return cv2.resize(img[y1:y2, x1:x2], (size, size), interpolation=interp)


def augment(rgb, rng):
    img = rgb.astype(np.float32)
    # brightness, contrast, saturation, small hue shift
    img *= rng.uniform(0.55, 1.45)
    mean = img.mean()
    img = (img - mean) * rng.uniform(0.7, 1.3) + mean
    img = np.clip(img, 0, 255).astype(np.uint8)
    hsv = cv2.cvtColor(img, cv2.COLOR_RGB2HSV).astype(np.int16)
    hsv[..., 0] = (hsv[..., 0] + rng.integers(-6, 7)) % 180
    hsv[..., 1] = np.clip(hsv[..., 1] * rng.uniform(0.6, 1.4), 0, 255)
    img = cv2.cvtColor(hsv.astype(np.uint8), cv2.COLOR_HSV2RGB)
    if rng.random() < 0.5:
        img = img[:, ::-1]
    if rng.random() < 0.2:
        img = img[::-1, :]
    if rng.random() < 0.3:
        a = rng.uniform(-12, 12)
        m = cv2.getRotationMatrix2D((SIZE / 2, SIZE / 2), a, 1.0)
        img = cv2.warpAffine(np.ascontiguousarray(img), m, (SIZE, SIZE), borderMode=cv2.BORDER_REFLECT)
    if rng.random() < 0.2:
        img = cv2.GaussianBlur(np.ascontiguousarray(img), (3, 3), 0)
    if rng.random() < 0.3:
        img = np.clip(img.astype(np.int16) + rng.normal(0, 6, img.shape).astype(np.int16), 0, 255).astype(np.uint8)
    return np.ascontiguousarray(img)


class PadSet(Dataset):
    def __init__(self, items, train):
        self.items, self.train = items, train
        self.cache = {}

    def __len__(self):
        return len(self.items)

    def __getitem__(self, i):
        it = self.items[i]
        img = self.cache.get(it["path"])
        if img is None:
            img = cv2.imread(it["path"])
            self.cache[it["path"]] = img
        if self.train:
            rng = np.random.default_rng()
            interp = [cv2.INTER_NEAREST, cv2.INTER_LINEAR, cv2.INTER_AREA][rng.integers(0, 3)]
            c = crop(img, it["roi"], SIZE, jitter=0.1, interp=interp, rng=rng)
            rgb = augment(cv2.cvtColor(c, cv2.COLOR_BGR2RGB), rng)
        else:
            rgb = cv2.cvtColor(crop(img, it["roi"], SIZE), cv2.COLOR_BGR2RGB)
        x = torch.from_numpy(rgb.astype(np.float32).transpose(2, 0, 1) / 255.0)
        return x, it["y"]


# ---------------------------------------------------------------- model
def conv_bn(cin, cout, stride):
    return nn.Sequential(nn.Conv2d(cin, cout, 3, stride, 1, bias=False), nn.BatchNorm2d(cout), nn.ReLU(inplace=True))


class PadNet(nn.Module):
    """~120 k parameters, ~19 M multiply-adds at 96x96."""

    def __init__(self):
        super().__init__()
        self.features = nn.Sequential(conv_bn(3, 16, 2), conv_bn(16, 32, 2), conv_bn(32, 32, 1), conv_bn(32, 64, 2),
                                      conv_bn(64, 64, 1), conv_bn(64, 96, 2))
        self.pool = nn.AdaptiveAvgPool2d(1)
        self.fc = nn.Linear(96, 2)

    def forward(self, x):
        return self.fc(torch.flatten(self.pool(self.features(x)), 1))


def evaluate(fn, loader):
    ys, ps = [], []
    for x, y in loader:
        ps.append(fn(x))
        ys.append(y.numpy())
    p = np.concatenate(ps)
    y = np.concatenate(ys)
    return y, p


def summary(y, p, thr=0.5):
    pred = p >= thr
    yb = y.astype(bool)
    return {"n": int(len(y)), "accuracy": round(float((pred == yb).mean()), 4),
            "false_phone": int((pred & ~yb).sum()), "missed_phone": int((~pred & yb).sum()),
            "phone": int(yb.sum()), "no_phone": int((~yb).sum())}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--train", nargs="+", required=True)
    ap.add_argument("--test", nargs="*", default=[])
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--out", default=os.path.join(ROOT, "firmware", "models", "pad_cls_96_s8.espdl"))
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()
    sys.stdout.reconfigure(errors="replace")
    torch.manual_seed(args.seed)
    random.seed(args.seed)
    dev = "cuda" if torch.cuda.is_available() else "cpu"

    train_items, val_items = [], []
    for s in args.train:
        for it in load_session(s):
            (val_items if it["pos"] >= 0.8 else train_items).append(it)
    test_items = [it for s in args.test for it in load_session(s)]
    test_name = "test"
    if not test_items:  # no separate session yet: score on the held-out end of every step instead
        test_items, test_name = val_items, "val (last 20 % of each step, not trained on)"
    print(f"train {len(train_items)}  val {len(val_items)}  test {len(test_items)}  "
          f"(phone share train {np.mean([i['y'] for i in train_items]):.2f})", flush=True)

    n_pos = sum(i["y"] for i in train_items)
    w = torch.tensor([len(train_items) / max(1, 2 * (len(train_items) - n_pos)), len(train_items) / max(1, 2 * n_pos)])
    tl = DataLoader(PadSet(train_items, True), batch_size=64, shuffle=True, num_workers=0, drop_last=True)
    vl = DataLoader(PadSet(val_items, False), batch_size=128)
    model = PadNet().to(dev)
    print("parameters:", sum(p.numel() for p in model.parameters()), flush=True)
    opt = torch.optim.AdamW(model.parameters(), lr=3e-3, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=3e-3, total_steps=args.epochs * len(tl))
    loss_fn = nn.CrossEntropyLoss(weight=w.to(dev), label_smoothing=0.05)

    def torch_prob(m):
        def fn(x):
            with torch.no_grad():
                return torch.softmax(m(x.to(dev)), 1)[:, 1].cpu().numpy()
        return fn

    best, best_state = -1.0, None
    for ep in range(args.epochs):
        model.train()
        tot = 0.0
        for x, y in tl:
            opt.zero_grad()
            loss = loss_fn(model(x.to(dev)), y.to(dev))
            loss.backward()
            opt.step()
            sched.step()
            tot += loss.item()
        model.eval()
        y, p = evaluate(torch_prob(model), vl)
        acc = float(((p >= 0.5) == y.astype(bool)).mean())
        if acc >= best:
            best, best_state = acc, {k: v.detach().clone() for k, v in model.state_dict().items()}
        print(f"epoch {ep + 1:2d}  loss {tot / len(tl):.3f}  val acc {acc:.4f}", flush=True)
    model.load_state_dict(best_state)
    model.eval().cpu()
    dev_eval = "cpu"

    result = {"train_sessions": args.train, "test_sessions": args.test, "scored_on": test_name, "train": len(train_items),
              "val": len(val_items), "best_val_acc": best}
    test_loader = DataLoader(PadSet(test_items, False), batch_size=128) if test_items else None

    def cpu_prob(x):
        with torch.no_grad():
            return torch.softmax(model(x), 1)[:, 1].numpy()

    if test_loader:
        y, p = evaluate(cpu_prob, test_loader)
        result["test_float32"] = summary(y, p)
        print(f"{test_name} float32:", result["test_float32"], flush=True)

    # ONNX export
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    onnx_path = os.path.splitext(args.out)[0] + ".onnx"
    torch.onnx.export(model, torch.zeros(1, 3, SIZE, SIZE), onnx_path, opset_version=13, input_names=["input"],
                      output_names=["logits"], dynamo=False)
    import onnx
    import onnxsim
    m, ok = onnxsim.simplify(onnx.load(onnx_path))
    onnx.save(m, onnx_path)

    # int8 quantization for the ESP32-S3
    from esp_ppq import QuantizationSettingFactory
    from esp_ppq.api import espdl_quantize_onnx
    from esp_ppq.executor import TorchExecutor
    calib_items = random.Random(1).sample(train_items + val_items, min(512, len(train_items) + len(val_items)))
    cl = DataLoader(PadSet(calib_items, False), batch_size=32)
    calib = [x for x, _ in cl]
    setting = QuantizationSettingFactory.espdl_setting()
    graph = espdl_quantize_onnx(onnx_import_file=onnx_path, espdl_export_file=args.out, calib_dataloader=calib,
                                calib_steps=len(calib), input_shape=[1, 3, SIZE, SIZE], target="esp32s3",
                                num_of_bits=8, collate_fn=lambda b: b.to(dev_eval), setting=setting,
                                device=dev_eval, error_report=False, skip_export=False, export_test_values=False,
                                verbose=0)
    ex = TorchExecutor(graph=graph, device=dev_eval)

    def int8_prob(x):
        outs = []
        for i in range(x.shape[0]):
            o = ex.forward(inputs=x[i:i + 1])[0]
            outs.append(torch.softmax(o.float(), 1)[:, 1].numpy())
        return np.concatenate(outs)

    if test_loader:
        y, p = evaluate(int8_prob, test_loader)
        result["test_int8"] = summary(y, p)
        print(f"{test_name} int8   :", result["test_int8"], flush=True)
        err_dir = os.path.join(ROOT, "runs", "pad_errors")
        shutil.rmtree(err_dir, ignore_errors=True)
        os.makedirs(err_dir)
        for it, yy, pp in zip(test_items, y, p):
            if (pp >= 0.5) != bool(yy):
                img = cv2.imread(it["path"])
                c = cv2.resize(crop(img, it["roi"], SIZE), (192, 192), interpolation=cv2.INTER_NEAREST)
                name = f"{it['kind']}_p{pp:.2f}_{os.path.basename(os.path.dirname(it['path']))}_" \
                       f"{os.path.basename(it['path'])}"
                cv2.imwrite(os.path.join(err_dir, name), c)
    result["espdl_bytes"] = os.path.getsize(args.out)
    with open(os.path.splitext(args.out)[0] + ".json", "w") as f:
        json.dump(result, f, indent=1)
    print("wrote", args.out, result["espdl_bytes"], "bytes", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
