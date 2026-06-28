# sdv-edge-gateway — System Architecture
**Author:** Manoj Kumar  
**Design method:** 5-step system design framework (Clarify → Estimate → High-level → Deep dive → Trade-offs)

---

## 1. Clarify — What Problem Does This Solve?

A Software Defined Vehicle generates continuous telemetry (speed, RPM, temperature, battery),
responds to remote commands (lock, OTA), and must do this **securely, offline-resiliently,
and with on-device intelligence** — not just pipe bytes to the cloud.

**Functional requirements:**
- Ingest CAN signals from vehicle ECUs (using open DBC format)
- Detect anomalies on-device using INT8 ML inference (before sending to cloud)
- Publish telemetry to cloud over mutual TLS MQTT
- Receive and execute authenticated remote commands
- Apply signed OTA firmware updates with A/B rollback
- Cloud-side LLM diagnostic agent explains anomalies in plain language

**Non-functional requirements:**
- Latency: anomaly detection < 50ms on Pi 4 (on DRIVE Orin DLA: < 4ms)
- Availability: offline buffer sustains 10,000 messages without cloud connectivity
- Safety: OTA never applied while vehicle moving (ASIL-C requirement — see safety/ASIL_ANALYSIS.md)
- Security: mutual TLS + ECDSA firmware signing (see safety/TARA.md)

---

## 2. Estimate — Scale and Sizing

| Dimension | Value |
|-----------|-------|
| CAN messages/sec | ~500 (real vehicle); 10 (this demo) |
| Telemetry publishes/sec | 1 Hz (configurable) |
| MQTT payload size | ~200 bytes JSON per message |
| Daily telemetry | ~17 MB/day per vehicle |
| Offline buffer | 10,000 messages × 200B = 2 MB SQLite |
| ONNX model size (INT8) | ~1 MB (vs 3.8 MB FP32 static) |
| YOLOv8n ONNX size | 12.3 MB |

---

## 3. High-Level Design

```
┌──────────────────────────── Vehicle Side (Raspberry Pi) ────────────────────────────┐
│                                                                                       │
│  [ADAS Simulator]          [CAN Bus — can0/can1]                                     │
│  tools/adas_simulator.py   SocketCAN (MCP2515 @ 500kbps)                            │
│  YOLOv8n → CAN frames ─────────────────────────────────────────────────────┐        │
│                                                                             │        │
│  ┌──────────────────────────────────────────────────────────────────────┐  │        │
│  │                   sdv_gateway (C++ binary)                           │  │        │
│  │                                                                      │  │        │
│  │  CanReader ──SafeQueue<CanFrame>──► SignalDecoder                   │  │        │
│  │                                          │                           │  │        │
│  │                              SafeQueue<DecodedSignal>               │  │        │
│  │                                    │         │                      │  │        │
│  │                            AnomalyDetector  TelemetryPublisher      │  │        │
│  │                            (ONNX INT8)      (Paho MQTT / TLS)      │  │        │
│  │                                 │                  │                │  │        │
│  │                         SafeQueue<AnomalyEvent>  OfflineBuffer     │  │        │
│  │                                 │                 (SQLite3)        │  │        │
│  │                         CommandHandler ◄── MQTT subscribe          │  │        │
│  │                                 │                                  │  │        │
│  │                           OtaAgent (ECDSA + A/B)                  │  │        │
│  └──────────────────────────────────────────────────────────────────────┘  │        │
│                                       │ MQTT / mutual TLS / port 8883       │        │
└───────────────────────────────────────┼─────────────────────────────────────────────┘
                                        │
                         ┌──────────────▼──────────────┐
                         │       AWS IoT Core           │
                         │  Topic ACL per UIN           │
                         │  IoT Rules → Lambda          │
                         └──────────┬──────────┬────────┘
                                    │          │
                        ┌───────────▼──┐  ┌────▼──────────────────────┐
                        │  DynamoDB    │  │  diagnostic_agent Lambda   │
                        │  Telemetry   │  │  RAG (ISO 26262 DTC docs) │
                        │  time-series │  │  + Bedrock LLM             │
                        └─────────────┘  └────────────────────────────┘
```

---

## 4. Deep Dive — Key Design Decisions

### DD-01: Why C++ for vehicle-side, Python only for cloud?
Automotive software is C++. RTCU, DriveOS, AUTOSAR Adaptive ara:: — all C++.
Python interpretation overhead is incompatible with deterministic timing requirements.
ONNX Runtime C++ API runs on ARM Linux (Pi) with same model as Python onnxruntime.

### DD-02: Why ONNX Runtime instead of TensorRT for anomaly detection?
TensorRT (.plan) is device-specific — compiles for DRIVE Orin GPU/DLA only.
ONNX Runtime runs on any ARM Linux (Pi, SA525M, Snapdragon Ride).
In production on DRIVE Orin: export ONNX → TensorRT → INT8 DLA = <4ms.
Demo on Pi: ONNX Runtime CPU → ~50ms. Shows the portability layer correctly.
(See P1.3/P1.4 for the full PyTorch → ONNX → TensorRT pipeline.)

### DD-03: Why two-layer offline resilience?
Layer 1: MQTT Persistent Session (broker holds QoS1 messages during TCP disconnect)
Layer 2: SQLite offline buffer (handles broker-side capacity limits + long outages)
Pattern from RTCU DataCollect: assumes 30-min cellular dead zones in some regions.

### DD-04: Why A/B rollback instead of single-slot OTA?
Single-slot: brick risk if power fails mid-install. No recovery without physical access.
A/B: active slot unchanged until new slot validates. Power-fail safe. RTCU uses same pattern.
ISO 26262 SG-01 requires the vehicle to remain operational if OTA fails.

### DD-05: Human-in-the-loop for CRITICAL anomalies
LLM output for vehicle actuation decisions is not ASIL-certifiable.
CRITICAL severity → SNS alert to human operator before diagnosis published to vehicle.
This is the guardrails pattern from AGI.2: agents + oversight for safety-critical actions.

---

## 5. Trade-offs

| Decision | Chosen | Alternative | What we gave up |
|----------|--------|-------------|-----------------|
| MQTT vs DDS | MQTT | DDS | Sub-ms latency (DDS) for simpler cloud integration |
| SQLite vs Redis | SQLite | Redis | Query flexibility for zero dependency overhead |
| ONNX Runtime vs TensorRT | ONNX | TensorRT | 10× speed on DRIVE Orin for Pi portability |
| Bedrock Claude Haiku vs GPT-4 | Haiku | GPT-4 | Quality ceiling for 10× lower cost + AWS integration |
| Raspberry Pi vs DRIVE Orin | Pi | DRIVE Orin | ASIL-D ceiling for accessible demo hardware |

---

## Roadmap Phases Integrated in This Project

| Phase | Component |
|-------|-----------|
| P1.2 — INT8 quantization | `src/analytics/anomaly_detector` (model + inference) |
| P1.3 — ONNX export + DRIVE Orin arch | `models/anomaly_int8.onnx`, DD-02, this doc |
| P1.4 — YOLOv8 edge inference | `tools/adas_simulator.py` (ONNX inference + CAN encoding) |
| P2.1 — TARA | `safety/TARA.md` (5 threat scenarios for this exact project) |
| P2.2 — ISO 26262 | `safety/ASIL_ANALYSIS.md` (HARA + ASIL decomposition) |
| P2.3 — MQTT + CAN + AWS IoT + OTA | Core gateway (all `src/` modules) |
| AGI.1 — RAG | `cloud/lambda/diagnostic_agent.py` (DTC vector retrieval) |
| AGI.2 — Agentic AI | `cloud/lambda/diagnostic_agent.py` (detect→retrieve→diagnose→publish) |
| System Design | This document (5-step framework + 6 NFR dimensions) |
