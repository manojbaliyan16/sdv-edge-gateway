# ISO 26262 Functional Safety Analysis
## sdv-edge-gateway — ASIL Assignment and Safety Architecture
**Standard:** ISO 26262:2018 (Road Vehicles — Functional Safety)  
**Author:** Manoj Kumar  
**Reference:** P2.2 functional safety study (applied to this project)

---

## Hazard Analysis and Risk Assessment (HARA)

| Hazard | Severity | Exposure | Controllability | ASIL |
|--------|----------|----------|----------------|------|
| OTA update applied while vehicle moving | S3 | E3 | C2 | **ASIL-C** |
| Spoofed remote door unlock at speed | S2 | E3 | C2 | **ASIL-B** |
| Telemetry loss causing fleet blind spot | S1 | E4 | C3 | **ASIL-A** |
| False anomaly alert causing driver panic | S2 | E3 | C1 | **QM** |

---

## Safety Goals

**SG-01 (ASIL-C):** The gateway shall not apply OTA firmware updates while the vehicle speed > 0 km/h.  
→ Implemented: `ota_agent.cpp` precondition check: `speed_kmh_ == 0.0`

**SG-02 (ASIL-B):** The gateway shall not execute vehicle commands without valid ECDSA-authenticated source.  
→ Implemented: `firmware_verifier.cpp` + `token_auth.cpp`

---

## ASIL Decomposition (SG-01)

ISO 26262 §5.9.1 — ASIL decomposition allows ASIL-C to be split into two independent ASIL-B channels:

```
              [ASIL-C Requirement]
               "No OTA while moving"
                      │
         ┌────────────┴────────────┐
         │                         │
   [ASIL-B Channel A]        [ASIL-B Channel B]
   Software precondition      Watchdog monitor
   speed check in             independent check
   ota_agent.cpp              (future: MCU watchdog)
         │                         │
         └────────────┬────────────┘
                      │
              ASIL-C satisfied
              (10⁻⁴ × 10⁻⁴ = 10⁻⁸/hr)
```

**Current state:** Channel A implemented. Channel B (MCU watchdog) is architectural note — would require real MCU hardware (e.g., Aurix TC397).

---

## Freedom from Interference (FFI)

**ISO 26262-6 §7.4.10:** Safety-critical functions must be isolated from QM functions.

| Function | Classification | Isolation |
|----------|---------------|-----------|
| OTA precondition check | ASIL-B | Runs in dedicated thread, no shared mutable state with telemetry |
| Speed signal reading | ASIL-B | Atomic `std::atomic<double> speed_kmh_` in OtaAgent |
| Telemetry publishing | QM | Separate thread, cannot corrupt OTA state |
| LLM diagnostic agent | QM | Cloud-only, no direct vehicle actuation |
| YOLOv8 ADAS simulator | QM | Tool only, not in production gateway binary |

**Linux OS note:** Linux (Raspberry Pi OS) does NOT provide FFI. This is a development/demo platform. Production deployment requires QNX or AUTOSAR OS for ASIL certification.

---

## Connection to DriveOS (P1.3)
DRIVE Orin STM static scheduler = deterministic timing = ASIL-D certifiable.  
This gateway on Pi = QM / ASIL-B demo platform.  
Same software architecture, different OS + hardware = different ASIL ceiling.  
The design decisions in this repo explicitly target production-deployable patterns.
