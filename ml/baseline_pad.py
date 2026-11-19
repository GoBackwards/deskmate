"""Non-ML baseline for "phone on the pad": does the pad region differ from the empty pad?

This is what a simple presence sensor (or a pixel-change detector) can do: it notices that
SOMETHING is on the pad, not WHAT. Score = mean absolute difference of the pad crop (grey, 96x96,
blurred) from a reference = median of empty-pad frames.

Two ways to run it:
  .venv\\Scripts\\python -X utf8 ml\\baseline_pad.py e2              # reference + best threshold from e2 itself
                                                                     # (an upper bound: tuned on the test data)
  .venv\\Scripts\\python -X utf8 ml\\baseline_pad.py e2 --fit s3     # fair: reference and threshold from s3,
                                                                     # then scored on e2
"""
import argparse
import json
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from train_pad import crop, load_session  # noqa: E402


def grey(it):
    g = cv2.cvtColor(crop(cv2.imread(it["path"]), it["roi"], 96), cv2.COLOR_BGR2GRAY).astype(np.float32)
    return cv2.GaussianBlur(g, (5, 5), 0)


def scores(items, ref):
    return np.array([np.abs(grey(it) - ref).mean() for it in items])


def best_threshold(score, y):
    return max(((float(((score >= t) == y).mean()), float(t)) for t in np.unique(score)), key=lambda a: a[0])


def report(items, score, thr):
    y = np.array([it["y"] for it in items]).astype(bool)
    kinds = np.array([it["kind"] for it in items])
    pred = score >= thr
    return {"frames": int(len(y)), "threshold": round(thr, 2), "accuracy": round(float((pred == y).mean()), 4),
            "per_kind_called_phone": {k: f"{int(pred[kinds == k].sum())}/{int((kinds == k).sum())}"
                                      for k in ("phone", "empty", "other")}}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("test", nargs="+")
    ap.add_argument("--fit", nargs="*", default=None, help="sessions that give the reference and threshold")
    args = ap.parse_args()
    test = [it for s in args.test for it in load_session(s)]
    fit = [it for s in args.fit for it in load_session(s)] if args.fit else test
    ref = np.median(np.stack([grey(it) for it in fit if it["kind"] == "empty"]), axis=0)
    fit_score = scores(fit, ref)
    _, thr = best_threshold(fit_score, np.array([it["y"] for it in fit]).astype(bool))
    out = report(test, scores(test, ref) if fit is not test else fit_score, thr)
    out["threshold_from"] = args.fit or "the test sessions themselves (upper bound)"
    print(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
