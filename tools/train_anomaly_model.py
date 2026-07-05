#!/usr/bin/env python3
"""
tools/train_anomaly_model.py
============================
Trains a lightweight binary-classifier neural network on synthetic CAN signal
data and exports it as an ONNX model for AnomalyDetector (C++ gateway).

Model I/O — must match anomaly_detector.cpp run_inference():
  Input  "signal_features" : shape [1, 1], float32 — one decoded signal value
  Output "anomaly_score"   : shape [1, 1], float32 — anomaly probability [0.0, 1.0]

Synthetic data note:
  Normal/anomaly envelopes are derived from automotive domain knowledge.
  To use real data: replace generate_data() with a dataset loader
  (e.g. ROAD dataset — Oak Ridge National Lab, pre-decoded signal values).
  Training pipeline and ONNX export are unchanged.

Usage:
  python3 tools/train_anomaly_model.py
  python3 tools/train_anomaly_model.py --output tools/anomaly_detector.onnx

Deploy to Pi:
  scp tools/anomaly_detector.onnx manoj@manoj-tcu:/opt/sdv/anomaly_detector.onnx
"""

import argparse
import os

import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import DataLoader, TensorDataset

try:
    import onnxruntime as ort
    ORT_AVAILABLE = True
except ImportError:
    ORT_AVAILABLE = False   # sanity check skipped — inference still works on Pi


# ─── Synthetic data generation ────────────────────────────────────────────────

def generate_data(n_normal: int = 5000, n_anomaly: int = 1000):
    """
    Returns (X, y) where X is shape [N, 1] float32 and y is shape [N, 1] float32.
    Label 0 = normal, 1 = anomaly.

    Normal envelopes (healthy vehicle operating range):
      Speed:        0 – 140  km/h
      Engine temp: 60 – 105  °C
      Battery:     11.5 – 14.5 V
      Engine RPM:  600 – 6500 rpm
      Throttle:      0 – 100  %

    Anomaly envelopes (fault or spoofed signal):
      Speed:        > 160  km/h   — overspeed / sensor spoof
      Engine temp:  > 130  °C    — thermal runaway
      Battery:      < 9.0  V     — deep discharge / CAN injection
      Engine RPM:   > 7500 rpm   — over-rev / spoof
    """
    rng = np.random.default_rng(seed=42)

    normal_values = np.concatenate([
        rng.uniform(0,    140,  n_normal // 5),   # speed km/h
        rng.uniform(60,   105,  n_normal // 5),   # engine temp °C
        rng.uniform(11.5, 14.5, n_normal // 5),   # battery V
        rng.uniform(600,  6500, n_normal // 5),   # RPM
        rng.uniform(0,    100,  n_normal // 5),   # throttle %
    ]).astype(np.float32)

    anomaly_values = np.concatenate([
        rng.uniform(160, 260,   n_anomaly // 4),  # overspeed km/h
        rng.uniform(130, 200,   n_anomaly // 4),  # overtemp °C
        rng.uniform(0,   9.0,   n_anomaly // 4),  # undervoltage V
        rng.uniform(7500, 9000, n_anomaly // 4),  # over-rev RPM
    ]).astype(np.float32)

    X = np.concatenate([normal_values, anomaly_values]).reshape(-1, 1)
    y = np.concatenate([
        np.zeros(len(normal_values), dtype=np.float32),
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
    """
    def __init__(self):
        super().__init__()
        self.net = nn.Sequential(
            nn.BatchNorm1d(1),    # learns mean/std of training data at runtime
            nn.Linear(1, 32),
            nn.ReLU(),
            nn.Linear(32, 16),
            nn.ReLU(),
            nn.Linear(16, 1),
            nn.Sigmoid(),         # squash output to [0, 1]
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
        do_constant_folding=True,           # fold BatchNorm stats into graph at export
    )
    print(f"  exported → {output_path}")


# ─── Sanity check ─────────────────────────────────────────────────────────────

def sanity_check(output_path: str) -> None:
    if not ORT_AVAILABLE:
        print("  onnxruntime not found — skipping sanity check (model still valid)")
        return

    sess = ort.InferenceSession(output_path,
                                providers=["CPUExecutionProvider"])

    cases = [
        ("normal   speed      60 km/h",  60.0,   "<0.5"),
        ("normal   eng temp   90 °C",    90.0,   "<0.5"),
        ("normal   battery  12.5 V",     12.5,   "<0.5"),
        ("anomaly  overspeed 200 km/h",  200.0,  ">0.5"),
        ("anomaly  overtemp  150 °C",    150.0,  ">0.5"),
        ("anomaly  undervolt   5 V",       5.0,  ">0.5"),
        ("anomaly  over-rev  8000 rpm",  8000.0, ">0.5"),
    ]

    print("\n  Sanity check:")
    all_pass = True
    for label, val, expected in cases:
        inp   = np.array([[val]], dtype=np.float32)
        score = sess.run(["anomaly_score"], {"signal_features": inp})[0][0][0]
        ok    = (score < 0.5) == (expected == "<0.5")
        sym   = "✓" if ok else "✗"
        if not ok:
            all_pass = False
        print(f"    {sym}  {label:38s}  score={score:.4f}  expected {expected}")

    print(f"\n  Result: {'ALL PASS' if all_pass else 'SOME FAILED — retrain or adjust envelopes'}")


# ─── Main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="Train and export CAN anomaly detector")
    parser.add_argument("--output",  default="tools/anomaly_detector.onnx",
                        help="Output path for ONNX model (default: tools/anomaly_detector.onnx)")
    parser.add_argument("--epochs",  type=int, default=40)
    parser.add_argument("--n-normal",  type=int, default=5000)
    parser.add_argument("--n-anomaly", type=int, default=1000)
    args = parser.parse_args()

    print("=== CAN Anomaly Model Training ===\n")

    print("1. Generating synthetic signal data...")
    X, y = generate_data(args.n_normal, args.n_anomaly)
    n_anom = int(y.sum())
    print(f"   {len(X)} samples — {n_anom} anomaly, {len(X)-n_anom} normal")

    print("\n2. Training AnomalyNet...")
    model = AnomalyNet()
    train(model, X, y, epochs=args.epochs)

    print("\n3. Exporting ONNX...")
    export_onnx(model, args.output)

    print("\n4. Running sanity check...")
    sanity_check(args.output)

    print(f"\nDone. Deploy to Pi:")
    print(f"  sudo mkdir -p /opt/sdv")
    print(f"  scp {args.output} manoj@manoj-tcu:/opt/sdv/anomaly_detector.onnx")


if __name__ == "__main__":
    main()
