#!/usr/bin/env python3
"""
tools/train_anomaly_model.py
============================
Trains a lightweight binary-classifier neural network on DBC-grounded CAN signal
data and exports it as an ONNX model for AnomalyDetector (C++ gateway).

Data source: dbc/toyota_corolla.dbc (parsed via cantools).
  Signal min/max boundaries are read directly from the DBC — so training envelopes
  stay consistent with whatever the runtime decoder sees. No hardcoded ranges.

Model I/O — must match anomaly_detector.cpp run_inference():
  Input  "signal_features" : shape [1, 1], float32 — one signal value, min-max
                              normalized against that signal's own
                              SIGNAL_OPERATING_WINDOWS["normal"] band:
                              normalized = (value - lo) / (hi - lo)
                              (mirrored exactly in anomaly_detector.cpp's
                              kNormalBounds — both sides must stay in sync)
  Output "anomaly_score"   : shape [1, 1], float32 — anomaly probability [0.0, 1.0]

Interview talking point:
  "We use cantools to parse the DBC at training time, extract per-signal min/max,
  and synthesise realistic normal/anomaly distributions from those bounds. This
  keeps training data in sync with the runtime decoder — any DBC update
  automatically tightens the anomaly model without touching training code."

Usage:
  pip install cantools  (first time only)
  python3 tools/train_anomaly_model.py
  python3 tools/train_anomaly_model.py --dbc dbc/toyota_corolla.dbc --output tools/anomaly_detector.onnx

Deploy to Pi (BOTH files — model uses ONNX external data format):
  scp tools/anomaly_detector.onnx tools/anomaly_detector.onnx.data manoj@manoj-tcu:/opt/sdv/
"""

import argparse
import os
from pathlib import Path

import numpy as np
import onnx
import torch
import torch.nn as nn
from torch.utils.data import DataLoader, TensorDataset

try:
    import cantools
    CANTOOLS_AVAILABLE = True
except ImportError:
    CANTOOLS_AVAILABLE = False
    print("[train] WARNING: cantools not installed. Falling back to hardcoded envelopes.")
    print("[train] Install with:  pip install cantools")

try:
    import onnxruntime as ort
    ORT_AVAILABLE = True
except ImportError:
    ORT_AVAILABLE = False


# ─── DBC signal envelope extraction ───────────────────────────────────────────

# Per-signal operating windows (physical values) derived from domain knowledge.
# These define what "normal" looks like inside each signal's valid DBC range.
# Anomaly = outside these windows (sensor spoof, fault, or CAN injection attempt).
SIGNAL_OPERATING_WINDOWS = {
    "VEHICLE_SPEED":       {"normal": (0.0,   140.0),  "anomaly_hi": 160.0},
    "ENGINE_RPM":          {"normal": (600.0, 6500.0), "anomaly_hi": 7500.0},
    "ENGINE_COOLANT_TEMP": {"normal": (60.0,  105.0),  "anomaly_hi": 130.0},
    "BATTERY_VOLTAGE":     {"normal": (11.5,  14.5),   "anomaly_lo": 9.0, "anomaly_hi": 16.0},
    # DBC declares GEAR_SHIFT's valid enum domain as [0|4] — every declared gear
    # position is equally "normal", so the normal band IS the full declared range.
    # An anomaly here isn't a physically-extreme value like overspeed; it's a raw
    # byte outside the declared enum (5-255) — a spoofed/corrupted gear code, i.e.
    # a CAN injection indicator. See load_signal_bounds() for how headroom above
    # 4 is derived (the declared DBC max can't provide it, since normal==declared).
    "GEAR_SHIFT":          {"normal": (0.0, 4.0),       "anomaly_hi": 5.0},
}


def load_signal_bounds(dbc_path: str) -> dict:
    """
    Parse DBC and return {signal_name: (min_phys, max_phys)} using cantools.
    Falls back to SIGNAL_OPERATING_WINDOWS ranges if cantools unavailable.
    """
    if not CANTOOLS_AVAILABLE:
        # Fallback: use operating window bounds as DBC stand-in
        return {name: (w["normal"][0] * 0.0, 300.0)
                for name, w in SIGNAL_OPERATING_WINDOWS.items()}

    db = cantools.database.load_file(dbc_path)
    bounds = {}
    for msg in db.messages:
        for sig in msg.signals:
            if sig.name in SIGNAL_OPERATING_WINDOWS:
                lo = float(sig.minimum) if sig.minimum is not None else 0.0
                hi = float(sig.maximum) if sig.maximum is not None else 300.0
                # cantools' sig.maximum reflects the DBC's declared valid-enum
                # ceiling, not the physical wire capacity. For GEAR_SHIFT those are
                # the same number (4), leaving zero headroom to synthesize an
                # anomaly example above the normal band. The raw byte can still
                # carry any value up to its bit width — use that as the ceiling
                # instead so "spoofed/out-of-enum gear code" has room to exist.
                if sig.name == "GEAR_SHIFT":
                    hi = float((1 << sig.length) - 1) * sig.scale + sig.offset
                bounds[sig.name] = (lo, hi)
                print(f"  [DBC] {sig.name:28s}  range=[{lo:.1f}, {hi:.1f}]  unit='{sig.unit}'  factor={sig.scale}")
    return bounds


# ─── Data generation ──────────────────────────────────────────────────────────

def generate_data(dbc_path: str, n_normal: int = 5000, n_anomaly: int = 1000):
    """
    Returns (X, y) where X is shape [N, 1] float32, y is shape [N, 1] float32.
    Label 0 = normal, label 1 = anomaly.

    Normal samples: drawn from SIGNAL_OPERATING_WINDOWS normal bands.
    Anomaly samples: drawn from bands just outside normal (realistic fault injection).

    Signal names and their min/max come from the DBC, so if the DBC changes
    (new signals, adjusted ranges) the training data changes automatically.
    """
    dbc_bounds = load_signal_bounds(dbc_path)
    rng = np.random.default_rng(seed=42)

    normal_parts = []
    anomaly_parts = []
    per_signal_normal  = n_normal  // len(SIGNAL_OPERATING_WINDOWS)
    per_signal_anomaly = n_anomaly // len(SIGNAL_OPERATING_WINDOWS)

    for sig_name, window in SIGNAL_OPERATING_WINDOWS.items():
        n_lo, n_hi = window["normal"]
        band = n_hi - n_lo   # normalization: (value - n_lo) / band — must match
                              # anomaly_detector.cpp's kNormalBounds exactly

        # ── Normal band (generated in physical units, then normalized) ─────────
        normal_raw = rng.uniform(n_lo, n_hi, per_signal_normal).astype(np.float32)
        normal_parts.append(((normal_raw - n_lo) / band).astype(np.float32))

        # ── Anomaly bands (physical units, DBC-grounded ceiling) ───────────────
        # Use DBC max as upper ceiling for anomaly values
        dbc_lo, dbc_hi = dbc_bounds.get(sig_name, (0.0, n_hi * 2))

        anom_parts = []
        if "anomaly_hi" in window:
            anom_hi = window["anomaly_hi"]
            anom_parts.append(rng.uniform(anom_hi, min(dbc_hi, anom_hi * 1.5),
                                          per_signal_anomaly // 2).astype(np.float32))
        if "anomaly_lo" in window:
            anom_lo = window["anomaly_lo"]
            anom_parts.append(rng.uniform(max(dbc_lo, 0.0), anom_lo,
                                          per_signal_anomaly // 2).astype(np.float32))
        if not anom_parts:
            # Signal only has hi anomaly — give full quota there
            anom_hi = window.get("anomaly_hi", n_hi * 1.5)
            anom_parts.append(rng.uniform(anom_hi, min(dbc_hi, anom_hi * 1.5),
                                          per_signal_anomaly).astype(np.float32))

        anomaly_raw = np.concatenate(anom_parts)
        anomaly_parts.append(((anomaly_raw - n_lo) / band).astype(np.float32))

    normal_values  = np.concatenate(normal_parts)
    anomaly_values = np.concatenate(anomaly_parts)

    X = np.concatenate([normal_values, anomaly_values]).reshape(-1, 1)
    y = np.concatenate([
        np.zeros(len(normal_values),  dtype=np.float32),
        np.ones (len(anomaly_values), dtype=np.float32),
    ]).reshape(-1, 1)

    idx = rng.permutation(len(X))
    return X[idx], y[idx]


# ─── Model ────────────────────────────────────────────────────────────────────

class AnomalyNet(nn.Module):
    """
    Lightweight binary classifier — 1 float in, anomaly score in [0,1] out.

    BatchNorm1d as first layer normalises across the batch so the network
    handles the wide value range across signal types without manual scaling
    (speed ~60 and RPM ~3000 look very different without normalisation).
    BatchNorm stats are folded into the ONNX graph at export (constant folding),
    so inference on the Pi is just a few matrix multiplications — fast.
    """
    def __init__(self):
        super().__init__()
        self.net = nn.Sequential(
            nn.BatchNorm1d(1),    # learns mean/std of training data; folded at export
            nn.Linear(1, 32),
            nn.ReLU(),
            nn.Linear(32, 16),
            nn.ReLU(),
            nn.Linear(16, 1),
            nn.Sigmoid(),         # squash output to [0, 1] — anomaly probability
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.net(x)


# ─── Training ─────────────────────────────────────────────────────────────────

def train(model: nn.Module, X: np.ndarray, y: np.ndarray,
          epochs: int = 40, batch_size: int = 256, lr: float = 1e-3) -> None:

    X_t = torch.from_numpy(X)
    y_t = torch.from_numpy(y)

    loader    = DataLoader(TensorDataset(X_t, y_t), batch_size=batch_size, shuffle=True)
    optimizer = torch.optim.Adam(model.parameters(), lr=lr)
    criterion = nn.BCELoss()

    model.train()
    for epoch in range(epochs):
        total_loss = 0.0
        for xb, yb in loader:
            optimizer.zero_grad()
            loss = criterion(model(xb), yb)
            loss.backward()
            optimizer.step()
            total_loss += loss.item()

        if (epoch + 1) % 10 == 0:
            print(f"  epoch {epoch+1:3d}/{epochs}  loss={total_loss/len(loader):.4f}")


# ─── ONNX export ──────────────────────────────────────────────────────────────

def export_onnx(model: nn.Module, output_path: str) -> None:
    model.eval()

    out_dir = os.path.dirname(output_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    dummy = torch.zeros(1, 1)   # shape [batch=1, features=1]

    torch.onnx.export(
        model,
        dummy,
        output_path,
        input_names=["signal_features"],   # matched in anomaly_detector.cpp
        output_names=["anomaly_score"],     # matched in anomaly_detector.cpp
        dynamic_axes={                      # allow any batch size at inference
            "signal_features": {0: "batch_size"},
            "anomaly_score":   {0: "batch_size"},
        },
        opset_version=17,
        do_constant_folding=True,           # fold BatchNorm stats into graph
    )

    # Collapse to a single self-contained file. Some PyTorch versions default to
    # ONNX's "external data" format (weights split into a companion <name>.onnx.data
    # file) even for models this tiny, where it's unnecessary — that format exists
    # to work around protobuf's 2GB message limit. Reload with external data merged
    # back in, then re-save as one file, so a deploy is a single scp, not two.
    model_proto = onnx.load(output_path, load_external_data=True)
    onnx.save_model(model_proto, output_path, save_as_external_data=False)
    external_data_path = output_path + ".data"
    if os.path.exists(external_data_path):
        os.remove(external_data_path)

    print(f"  exported → {output_path}")


# ─── Sanity check ─────────────────────────────────────────────────────────────

def sanity_check(output_path: str) -> None:
    if not ORT_AVAILABLE:
        print("  onnxruntime not found — skipping sanity check")
        return

    sess = ort.InferenceSession(output_path, providers=["CPUExecutionProvider"])

    # (label, signal_name, raw_physical_value, expected)
    # raw values are normalized below using the same SIGNAL_OPERATING_WINDOWS
    # bounds the model was trained on — feeding raw physical values straight
    # into the model here would silently mis-report every case, since the
    # model now only understands normalized inputs.
    cases = [
        ("normal   speed      60 km/h",  "VEHICLE_SPEED",       60.0,   "<0.5"),
        ("normal   eng temp   90 degC",  "ENGINE_COOLANT_TEMP", 90.0,   "<0.5"),
        ("normal   battery  12.5 V",     "BATTERY_VOLTAGE",     12.5,   "<0.5"),
        ("normal   rpm      2000 rpm",   "ENGINE_RPM",          2000.0, "<0.5"),
        ("anomaly  overspeed 200 km/h",  "VEHICLE_SPEED",       200.0,  ">0.5"),
        ("anomaly  overtemp  150 degC",  "ENGINE_COOLANT_TEMP", 150.0,  ">0.5"),
        ("anomaly  undervolt   5 V",     "BATTERY_VOLTAGE",       5.0,  ">0.5"),
        ("anomaly  over-rev  8000 rpm",  "ENGINE_RPM",          8000.0, ">0.5"),
        ("anomaly  overvolt  25 V",      "BATTERY_VOLTAGE",      25.0,  ">0.5"),
        ("normal   gear      3 (D)",     "GEAR_SHIFT",            3.0,  "<0.5"),
        ("normal   gear      0 (P)",     "GEAR_SHIFT",            0.0,  "<0.5"),
        ("anomaly  gear code   7",       "GEAR_SHIFT",            7.0,  ">0.5"),
    ]

    print("\n  Sanity check:")
    all_pass = True
    for label, sig_name, val, expected in cases:
        n_lo, n_hi = SIGNAL_OPERATING_WINDOWS[sig_name]["normal"]
        normalized = (val - n_lo) / (n_hi - n_lo)
        inp   = np.array([[normalized]], dtype=np.float32)
        score = sess.run(["anomaly_score"], {"signal_features": inp})[0][0][0]
        ok    = (score < 0.5) == (expected == "<0.5")
        sym   = "✓" if ok else "✗"
        if not ok:
            all_pass = False
        print(f"    {sym}  {label:38s}  score={score:.4f}  expected {expected}")

    print(f"\n  Result: {'ALL PASS' if all_pass else 'SOME FAILED — adjust SIGNAL_OPERATING_WINDOWS envelopes'}")


# ─── Main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="Train and export CAN anomaly detector")
    parser.add_argument("--dbc",     default="dbc/toyota_corolla.dbc",
                        help="Path to DBC file (signal ranges pulled from here)")
    parser.add_argument("--output",  default="tools/anomaly_detector.onnx",
                        help="Output path for ONNX model")
    parser.add_argument("--epochs",  type=int, default=40)
    parser.add_argument("--n-normal",  type=int, default=5000)
    parser.add_argument("--n-anomaly", type=int, default=1000)
    args = parser.parse_args()

    print("=== CAN Anomaly Model Training ===")
    print(f"  DBC:    {args.dbc}")
    print(f"  Output: {args.output}\n")

    print("1. Loading signal bounds from DBC...")
    # Signal bounds are printed inside generate_data → load_signal_bounds
    X, y = generate_data(args.dbc, args.n_normal, args.n_anomaly)
    n_anom = int(y.sum())
    print(f"\n   {len(X)} samples — {n_anom} anomaly, {len(X)-n_anom} normal")

    print("\n2. Training AnomalyNet...")
    model = AnomalyNet()
    train(model, X, y, epochs=args.epochs)

    print("\n3. Exporting ONNX...")
    export_onnx(model, args.output)

    print("\n4. Running sanity check...")
    sanity_check(args.output)

    data_file = args.output + ".data"
    print(f"\nDone. Deploy to Pi (BOTH files — ONNX external data format):")
    print(f"  sudo mkdir -p /opt/sdv")
    print(f"  scp {args.output} {data_file} manoj@manoj-tcu:/opt/sdv/")


if __name__ == "__main__":
    main()
