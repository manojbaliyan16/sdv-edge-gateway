# Threat Analysis and Risk Assessment (TARA)
## sdv-edge-gateway — Automotive Cybersecurity Analysis
**Standard:** ISO/SAE 21434 · UN R155  
**Author:** Manoj Kumar  
**Reference:** P2.1 TARA methodology (applied to this project's specific attack surface)

---

## Assets and Cybersecurity Goals

| Asset | CAL | Cybersecurity Goal |
|-------|-----|-------------------|
| MQTT TLS certificates (device_key, device_cert) | CAL-4 | Prevent private key exfiltration |
| OTA firmware package | CAL-4 | Prevent unsigned firmware installation |
| CAN bus (vehicle control commands) | CAL-3 | Prevent spoofed remote commands |
| Telemetry data (vehicle state) | CAL-2 | Prevent data manipulation/replay |
| Auth token (AWS IoT) | CAL-3 | Prevent unauthorized service access |

---

## Threat Scenarios

### THREAT-01: MQTT MITM (Man-in-the-Middle)
**Attack path:** Attacker intercepts MQTT traffic between Pi and AWS IoT Core  
**Impact:** Telemetry manipulation, command injection  
**MITIGATED by:**  
- Mutual TLS (`security/tls_config.cpp`) — both client and server must present valid certs  
- Certificate pinned to AWS IoT ATS root CA  
- AWS IoT Topic ACL policy (cloud/iot_setup/policy.json) — device cannot subscribe to other vehicles' topics  
**Residual risk:** Low ✅

### THREAT-02: Replay Attack
**Attack path:** Attacker captures valid MQTT command, replays it later  
**Impact:** Unauthorized door unlock / engine start  
**MITIGATED by:**  
- `correlation_id` in Command struct (UUID per command, single-use)  
- CommandHandler rejects duplicate correlation IDs (in-memory set, cleared on restart)  
**Residual risk:** Medium ⚠️ (ID set lost on restart — persistent nonce store needed for production)

### THREAT-03: Unsigned OTA Firmware
**Attack path:** Attacker delivers unsigned or tampered firmware via OTA channel  
**Impact:** Remote code execution on vehicle gateway  
**MITIGATED by:**  
- ECDSA-P256 signature verification (`security/firmware_verifier.cpp`) before ANY install  
- SHA-256 manifest hash check before download completes  
- A/B rollback if firmware fails boot validation  
**Residual risk:** Low ✅

### THREAT-04: CAN Bus Spoofing (from compromised ADAS simulator)
**Attack path:** Malicious CAN frame injected on bus, spoofing speed=0 to trigger OTA  
**Impact:** OTA triggered while vehicle is moving (safety violation)  
**MITIGATED by:**  
- OTA precondition check: speed from *multiple* CAN signals cross-validated  
- Anomaly detector flags implausible signal combinations  
**Residual risk:** Medium ⚠️ (hardware CAN firewall not implemented — would require Aurix/SafeAssure in production)

### THREAT-05: Token Lateral Movement
**Attack path:** Compromised gateway uses valid AWS IoT cert to subscribe to other vehicles' topics  
**Impact:** Fleet-wide data access  
**MITIGATED by:**  
- AWS IoT policy scope-locked to `sdv/*/uin/<THIS_UIN>/*` — verified in `cloud/iot_setup/policy.json`  
- Per-vehicle cert: one Thing = one cert = one policy  
**Residual risk:** Low ✅

---

## Connection to UN R155
- Cybersecurity Management System (CSMS) ← satisfied by this TARA document  
- Secure software update ← `security/firmware_verifier.cpp` + OTA preconditions  
- Communication security ← mutual TLS + topic ACL  
- Incident detection ← `src/analytics/anomaly_detector` (flags abnormal signal patterns)
