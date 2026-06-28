#!/usr/bin/env python3
"""
adas_simulator.py — Simulates an ADAS ECU sending object detection results as CAN frames.

CONNECTS TO:
  P1.4 — YOLOv8 edge inference pipeline (same ONNX model exported in P1.4)
  P1.3 — ONNX export (loads yolov8n.onnx via ONNX Runtime)
  P1.2 — INT8 quantization insight: in production this runs INT8 on DRIVE Orin DLA

Architecture:
  In a real SDV, the ADAS ECU (DRIVE Orin) runs perception and publishes results
  over SOME/IP or CAN to the TCU. Here we simulate that ECU:
    Pi Camera / video file → YOLOv8n ONNX → encode detections → CAN frame → can0

Usage:
  python3 tools/adas_simulator.py --source 0          # Pi Camera
  python3 tools/adas_simulator.py --source video.mp4  # Video file
  python3 tools/adas_simulator.py --source mock       # No camera, send mock frames

ADAS CAN encoding (custom, not in toyota.dbc — simulated ADAS ECU output):
  CAN ID 0x200: OBJECT_CLASS (byte 0: 0=none,1=person,2=car,3=truck,4=bus)
                CONFIDENCE   (byte 1: 0-100)
                COUNT        (byte 2: number of detected objects)
  CAN ID 0x201: COLLISION_WARN (byte 0: 0=safe, 1=warning, 2=critical)
"""

import can
import time
import argparse
import numpy as np

try:
    import onnxruntime as ort
    import cv2
    INFERENCE_AVAILABLE = True
except ImportError:
    INFERENCE_AVAILABLE = False
    print("[adas_sim] WARNING: onnxruntime or cv2 not installed. Running in mock mode.")

# ─── CAN encoding ──────────────────────────────────────────────────────────────
ADAS_CAN_ID_DETECTION = 0x200
ADAS_CAN_ID_COLLISION = 0x201

# YOLOv8 COCO class indices we care about
PERSON_CLASS = 0
CAR_CLASS    = 2
TRUCK_CLASS  = 7
BUS_CLASS    = 5

def encode_detection_frame(obj_class: int, confidence: float, count: int) -> bytes:
    data = bytearray(8)
    data[0] = obj_class & 0xFF
    data[1] = int(confidence * 100) & 0xFF
    data[2] = min(count, 255) & 0xFF
    return bytes(data)

def encode_collision_frame(warning_level: int) -> bytes:
    data = bytearray(8)
    data[0] = warning_level & 0xFF  # 0=safe, 1=warning, 2=critical
    return bytes(data)


# ─── Mock mode (no camera) ────────────────────────────────────────────────────
def mock_loop(bus: can.interface.Bus, interval: float):
    """Send realistic-looking ADAS CAN frames without real inference."""
    print("[adas_sim] Mock mode: sending synthetic ADAS frames")
    import math
    t = 0
    while True:
        # Oscillate between detecting 0 and 2 people
        count = int(abs(math.sin(t * 0.3)) * 2)
        conf  = 0.82 if count > 0 else 0.0
        warn  = 1 if count > 1 else 0

        bus.send(can.Message(arbitration_id=ADAS_CAN_ID_DETECTION,
                             data=encode_detection_frame(PERSON_CLASS, conf, count),
                             is_extended_id=False))
        bus.send(can.Message(arbitration_id=ADAS_CAN_ID_COLLISION,
                             data=encode_collision_frame(warn),
                             is_extended_id=False))
        t += interval
        time.sleep(interval)


# ─── Real ONNX inference mode ─────────────────────────────────────────────────
def inference_loop(source, model_path: str, bus: can.interface.Bus, interval: float):
    """
    Run YOLOv8n ONNX inference on camera/video and send CAN frames.
    This is the same model exported in P1.4 using model.export(format='onnx').

    Key insight from P1.2: on DRIVE Orin DLA this runs INT8 at <4ms.
    On Pi 4 CPU with FP32 ONNX Runtime: ~200ms. Shows why dedicated hardware matters.
    """
    session = ort.InferenceSession(model_path,
                                   providers=["CPUExecutionProvider"])
    input_name = session.get_inputs()[0].name

    cap = cv2.VideoCapture(0 if source == "0" else source)
    print(f"[adas_sim] Running YOLOv8n ONNX inference on source: {source}")

    while cap.isOpened():
        ret, frame = cap.read()
        if not ret:
            break

        # Preprocess (same as P1.4 pipeline)
        img = cv2.resize(frame, (640, 640))
        img = img.transpose(2, 0, 1)[np.newaxis].astype(np.float32) / 255.0

        outputs = session.run(None, {input_name: img})
        # outputs[0] shape: [1, 84, 8400] — 84 = 4 bbox + 80 classes
        preds = outputs[0][0].T  # [8400, 84]

        # Filter by confidence
        conf = preds[:, 4:].max(axis=1)
        cls  = preds[:, 4:].argmax(axis=1)
        mask = conf > 0.5
        detections = list(zip(cls[mask].tolist(), conf[mask].tolist()))

        # Find highest-priority object
        persons = [(c, v) for c, v in detections if c == PERSON_CLASS]
        cars    = [(c, v) for c, v in detections if c in [CAR_CLASS, TRUCK_CLASS, BUS_CLASS]]

        dominant_class = PERSON_CLASS if persons else (CAR_CLASS if cars else 0)
        dominant_conf  = max((v for _, v in persons + cars), default=0.0)
        obj_count      = len(detections)
        collision_warn = 2 if len(persons) > 2 else (1 if len(persons) > 0 else 0)

        bus.send(can.Message(arbitration_id=ADAS_CAN_ID_DETECTION,
                             data=encode_detection_frame(dominant_class, dominant_conf, obj_count),
                             is_extended_id=False))
        bus.send(can.Message(arbitration_id=ADAS_CAN_ID_COLLISION,
                             data=encode_collision_frame(collision_warn),
                             is_extended_id=False))

        print(f"  Detected {obj_count} objects, collision_warn={collision_warn}")
        time.sleep(interval)

    cap.release()


# ─── Entry point ─────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="ADAS ECU Simulator")
    parser.add_argument("--source",    default="mock",        help="'mock', '0' (camera), or video file path")
    parser.add_argument("--model",     default="models/yolov8n.onnx", help="YOLOv8n ONNX model path")
    parser.add_argument("--interface", default="can0",        help="CAN interface to send on")
    parser.add_argument("--interval",  type=float, default=0.1, help="Seconds between frames")
    args = parser.parse_args()

    bus = can.interface.Bus(channel=args.interface, bustype="socketcan")
    print(f"[adas_sim] Sending on {args.interface}. Ctrl+C to stop.")

    try:
        if args.source == "mock" or not INFERENCE_AVAILABLE:
            mock_loop(bus, args.interval)
        else:
            inference_loop(args.source, args.model, bus, args.interval)
    except KeyboardInterrupt:
        print("\n[adas_sim] Stopped.")
    finally:
        bus.shutdown()

if __name__ == "__main__":
    main()
