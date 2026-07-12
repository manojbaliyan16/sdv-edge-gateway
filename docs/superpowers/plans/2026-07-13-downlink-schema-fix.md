# Downlink Command Schema Fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite `cloud/iot_setup/send_command.py` so it speaks the exact wire protocol `CommandHandler.cpp`/`OtaAgent.cpp` actually implement, closing the currently-100%-broken downlink.

**Architecture:** One file changes. Payload-building functions produce `{"type", "correlation_id", "payload"}` JSON matching `command_handler.cpp:145-171` and `ota_agent.cpp:108-116` exactly. All five command types publish to the single topic `sdv/commands/to/uin/<uin>`.

**Tech Stack:** Python 3, boto3 (already a dependency, unchanged). No new dependencies — repo has no pytest/test framework anywhere, so verification uses plain `assert`-based scripts run directly with `python3`, matching existing project conventions (no test framework introduced for a single CLI tool).

## Global Constraints

- Scope is `cloud/iot_setup/send_command.py` only. No C++ file may be touched (per spec, "Decision" section).
- Wire format is exactly: `{"type": <one of DOOR_LOCK|DOOR_UNLOCK|ENGINE_START|HORN_LIGHT|OTA_TRIGGER>, "correlation_id": <uuid4 str>, "payload": {...}}`.
- OTA `payload` contains exactly `url`, `sha256`, `version` — no `signature`/`size_bytes`/`min_battery_v`/`require_stop`.
- `DIAGNOSTIC_QUERY` and `CAN_WRITE` are removed, not adapted.
- No `Co-Authored-By` trailer in any git commit (explicit standing user rule).
- Never force-push or rewrite existing history.

---

### Task 1: Rewrite payload builders and CLI in `send_command.py`

**Files:**
- Modify: `cloud/iot_setup/send_command.py` (full rewrite, same path)
- Create: `cloud/iot_setup/test_send_command.py` (plain-assert verification script, not pytest — no test framework exists in this repo)

**Interfaces:**
- Produces: `build_actuation_command(cmd_type: str, correlation_id: str) -> dict`, `build_ota_command(correlation_id: str, url: str, sha256: str, version: str) -> dict`, `ACTUATION_TYPES = ["DOOR_LOCK", "DOOR_UNLOCK", "ENGINE_START", "HORN_LIGHT"]`, `CMD_TYPES = ACTUATION_TYPES + ["OTA_TRIGGER"]`, `COMMAND_TOPIC = "sdv/commands/to/uin/{uin}"` (format template).

- [ ] **Step 1: Write the failing test script**

Create `cloud/iot_setup/test_send_command.py`:

```python
#!/usr/bin/env python3
"""
Plain-assert verification for send_command.py's payload builders.
Run directly: python3 cloud/iot_setup/test_send_command.py
No pytest — this repo has no test framework, and one script doesn't
justify introducing one.
"""

import json
import sys

from send_command import (
    build_actuation_command,
    build_ota_command,
    ACTUATION_TYPES,
    CMD_TYPES,
    COMMAND_TOPIC,
)


def test_actuation_shape():
    for cmd_type in ACTUATION_TYPES:
        cmd = build_actuation_command(cmd_type, "corr-1")
        assert set(cmd.keys()) == {"type", "correlation_id", "payload"}, cmd.keys()
        assert cmd["type"] == cmd_type
        assert cmd["correlation_id"] == "corr-1"
        assert cmd["payload"] == {}
        # Must round-trip through json.dumps/loads exactly like the wire does
        assert json.loads(json.dumps(cmd)) == cmd
    print("test_actuation_shape: PASS")


def test_ota_shape():
    cmd = build_ota_command("corr-2", "https://example.com/fw.bin", "abc123", "1.2.0")
    assert set(cmd.keys()) == {"type", "correlation_id", "payload"}, cmd.keys()
    assert cmd["type"] == "OTA_TRIGGER"
    assert cmd["correlation_id"] == "corr-2"
    # Exactly these 3 keys — matches ota_agent.cpp:108-116, no extras
    assert set(cmd["payload"].keys()) == {"url", "sha256", "version"}, cmd["payload"].keys()
    assert cmd["payload"]["url"] == "https://example.com/fw.bin"
    assert cmd["payload"]["sha256"] == "abc123"
    assert cmd["payload"]["version"] == "1.2.0"
    print("test_ota_shape: PASS")


def test_cmd_types_no_removed_values():
    # CAN_WRITE and DIAGNOSTIC_QUERY must never reappear — vehicle side
    # has no handler for them (command_handler.cpp type_map).
    assert "CAN_WRITE" not in CMD_TYPES
    assert "DIAGNOSTIC_QUERY" not in CMD_TYPES
    assert set(CMD_TYPES) == {
        "DOOR_LOCK", "DOOR_UNLOCK", "ENGINE_START", "HORN_LIGHT", "OTA_TRIGGER",
    }
    print("test_cmd_types_no_removed_values: PASS")


def test_single_topic():
    topic = COMMAND_TOPIC.format(uin="VIN123")
    assert topic == "sdv/commands/to/uin/VIN123"
    # No separate manifest topic constant should exist at all
    import send_command as sc
    assert not hasattr(sc, "OTA_MANIFEST_TOPIC")
    print("test_single_topic: PASS")


if __name__ == "__main__":
    test_actuation_shape()
    test_ota_shape()
    test_cmd_types_no_removed_values()
    test_single_topic()
    print("\nAll tests passed.")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd cloud/iot_setup && python3 test_send_command.py`
Expected: `ImportError` or `ModuleNotFoundError` — `build_actuation_command` etc. don't exist yet in the current `send_command.py`.

- [ ] **Step 3: Rewrite `send_command.py`**

Replace the entire file with:

```python
#!/usr/bin/env python3
"""
send_command.py — Publish remote ops commands to a vehicle via AWS IoT Core.

Closes the DOWNLINK path:
  Cloud Operator (this script)
        │
        └── boto3 iot-data publish()
              │
              ▼
        AWS IoT Core (routes by topic ACL per UIN)
              │
              └── sdv/commands/to/uin/<UIN>  → CommandHandler.cpp
                    ├── DOOR_LOCK/DOOR_UNLOCK/ENGINE_START/HORN_LIGHT → CanWriter
                    └── OTA_TRIGGER                                   → OtaAgent (ota_queue_)

Wire format (must match CommandHandler::parse_command, command_handler.cpp:145-171):
  {
    "type":           "DOOR_LOCK" | "DOOR_UNLOCK" | "ENGINE_START" | "HORN_LIGHT" | "OTA_TRIGGER",
    "correlation_id":  "<uuid4>",
    "payload":         {}            # actuation commands — CanWriter ignores this entirely
  }

For OTA_TRIGGER, "payload" carries the manifest — these are the ONLY three
keys OtaAgent::run() reads out of payload_json (ota_agent.cpp:108-116):
  {
    "type": "OTA_TRIGGER",
    "correlation_id": "<uuid4>",
    "payload": {
      "url":     "https://<s3-presigned-or-cdn-url>/firmware.bin",
      "sha256":  "<64-char hex digest>",
      "version": "<semver, optional — defaults to \"unknown\" on the vehicle>"
    }
  }

NOTE: there is no separate OTA manifest topic. An earlier version of this
script published OTA manifests to sdv/ota/to/uin/<UIN>/manifest — nothing in
the C++ ever subscribed to that topic (CommandHandler only subscribes to
sdv/commands/to/uin/<uin> and the diagnosis topic; OtaAgent never subscribes
to MQTT at all, only publishes status). Every OTA manifest sent that way was
silently lost. OTA now goes through the same command topic as everything
else, as an OTA_TRIGGER.

NOTE: CAN_WRITE and DIAGNOSTIC_QUERY do not exist as vehicle-side commands.
CanWriter can only send one fixed trigger byte to one of four fixed CAN IDs
per named command (can_writer.cpp) — it cannot write an arbitrary CAN ID +
data payload. DIAGNOSTIC_QUERY has no CommandType value and no handler at
all. Diagnosis is push-based instead: AnomalyDetector -> Lambda ->
sdv/Analytics/to/uin/<uin>/diagnosis -> CommandHandler::handle_diagnosis()
(already working, logs DTC/ASIL/driver_message).

NOTE: actuation commands have no status feedback topic. CanWriter::write()
returns a bool that's only logged locally via DLT on the vehicle — there is
nothing to subscribe to for DOOR_LOCK/etc. acknowledgement. OTA is the only
command type with real status feedback (sdv/ota/from/uin/<uin>/status,
published by OtaAgent::report_status()).

Usage:
  # Lock the doors
  python3 cloud/iot_setup/send_command.py \\
    --uin VIN_PLACEHOLDER --region eu-west-1 --cmd-type DOOR_LOCK

  # Start the engine
  python3 cloud/iot_setup/send_command.py \\
    --uin VIN_PLACEHOLDER --region eu-west-1 --cmd-type ENGINE_START

  # Trigger an OTA update
  python3 cloud/iot_setup/send_command.py \\
    --uin VIN_PLACEHOLDER --region eu-west-1 --cmd-type OTA_TRIGGER \\
    --ota-url https://s3.eu-west-1.amazonaws.com/my-bucket/fw_v1.2.0.bin \\
    --ota-sha256 abc123... --ota-version 1.2.0

  # Monitor OTA progress (separate terminal) — actuation commands have no
  # equivalent status topic, see NOTE above.
  aws iot-data subscribe --topic "sdv/ota/from/uin/VIN_PLACEHOLDER/status"
"""

import argparse
import boto3
import json
import uuid

# ─── Command types (must match command_handler.cpp's type_map exactly) ────────
ACTUATION_TYPES = ["DOOR_LOCK", "DOOR_UNLOCK", "ENGINE_START", "HORN_LIGHT"]
CMD_TYPES = ACTUATION_TYPES + ["OTA_TRIGGER"]

COMMAND_TOPIC = "sdv/commands/to/uin/{uin}"


def build_actuation_command(cmd_type: str, correlation_id: str) -> dict:
    """
    Build a DOOR_LOCK/DOOR_UNLOCK/ENGINE_START/HORN_LIGHT command.
    payload is empty — CanWriter::write() ignores it; the CAN ID and the
    single trigger byte are both fixed on the vehicle side per cmd_type.
    """
    return {
        "type": cmd_type,
        "correlation_id": correlation_id,
        "payload": {},
    }


def build_ota_command(correlation_id: str, url: str, sha256: str, version: str) -> dict:
    """
    Build an OTA_TRIGGER command. payload carries exactly the 3 fields
    OtaAgent::run() reads (ota_agent.cpp:108-116) — url, sha256, version.
    """
    return {
        "type": "OTA_TRIGGER",
        "correlation_id": correlation_id,
        "payload": {
            "url": url,
            "sha256": sha256,
            "version": version,
        },
    }


def publish_to_iot(region: str, topic: str, payload: dict, qos: int = 1):
    """Publish payload to AWS IoT Core data plane."""
    iot = boto3.client("iot-data", region_name=region)
    return iot.publish(topic=topic, qos=qos, payload=json.dumps(payload))


def main():
    parser = argparse.ArgumentParser(
        description="Send a remote ops command to a vehicle via AWS IoT Core"
    )
    parser.add_argument("--uin", required=True, help="Vehicle UIN / VIN")
    parser.add_argument("--region", default="eu-west-1")
    parser.add_argument("--cmd-type", required=True, choices=CMD_TYPES)

    # OTA_TRIGGER args
    parser.add_argument("--ota-url", default="", help="HTTPS URL for firmware binary")
    parser.add_argument("--ota-sha256", default="", help="64-char hex SHA256 of binary")
    parser.add_argument("--ota-version", default="unknown")

    args = parser.parse_args()
    correlation_id = str(uuid.uuid4())

    if args.cmd_type == "OTA_TRIGGER":
        if not args.ota_url or not args.ota_sha256:
            parser.error("OTA_TRIGGER requires --ota-url and --ota-sha256")
        payload = build_ota_command(correlation_id, args.ota_url, args.ota_sha256, args.ota_version)
    else:
        payload = build_actuation_command(args.cmd_type, correlation_id)

    topic = COMMAND_TOPIC.format(uin=args.uin)

    print(f"\n→ Publishing to: {topic}")
    print(f"  Payload:\n{json.dumps(payload, indent=4)}\n")

    try:
        publish_to_iot(args.region, topic, payload)
        print(f"✓ Published successfully (correlation_id={correlation_id})")
        if args.cmd_type == "OTA_TRIGGER":
            print(f"\nMonitor OTA progress:")
            print(f"  aws iot-data subscribe --topic \"sdv/ota/from/uin/{args.uin}/status\"")
        else:
            print(f"\n(No status topic exists for actuation commands — see module docstring.)")
    except Exception as e:
        print(f"✗ Publish failed: {e}")
        raise


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd cloud/iot_setup && python3 test_send_command.py`
Expected:
```
test_actuation_shape: PASS
test_ota_shape: PASS
test_cmd_types_no_removed_values: PASS
test_single_topic: PASS

All tests passed.
```

- [ ] **Step 5: CLI smoke test (argparse only, no publish)**

Run: `cd cloud/iot_setup && python3 send_command.py --help`
Expected: usage text lists `--cmd-type {DOOR_LOCK,DOOR_UNLOCK,ENGINE_START,HORN_LIGHT,OTA_TRIGGER}`, no `CAN_WRITE`/`DIAGNOSTIC_QUERY`, no `--can-id`/`--data`/`--priority`/`--ota-sig`/`--ota-size` flags.

Run: `python3 send_command.py --uin TEST --cmd-type OTA_TRIGGER` (missing required OTA args)
Expected: exits with `error: OTA_TRIGGER requires --ota-url and --ota-sha256`.

- [ ] **Step 6: Commit**

```bash
git add cloud/iot_setup/send_command.py cloud/iot_setup/test_send_command.py
git commit -m "Fix downlink: rewrite send_command.py to match real CommandHandler/OtaAgent wire protocol

send_command.py sent cmd_type/cmd_id fields and CAN_WRITE/OTA_UPDATE/
DIAGNOSTIC_QUERY values the C++ side never understood, plus an OTA manifest
to a topic nothing subscribed to -- every downlink command was silently
dropped. Rewritten to send type/correlation_id/payload matching
CommandHandler::parse_command exactly, all 5 real CommandType values, single
topic, OTA payload trimmed to the 3 fields OtaAgent actually reads."
```

---

### Task 2: Live smoke test against the real gateway + AWS IoT Core

**Files:** none modified — verification only, using the existing Mac build at `build/sdv_gateway` and existing `config/config.yaml` + `certs/`.

**Interfaces:**
- Consumes: `build_actuation_command`, `build_ota_command`, `COMMAND_TOPIC` from Task 1.

- [ ] **Step 1: Confirm the local build is current**

```bash
cd /Users/manojkumar/RoadMap/sdv-edge-gateway
cmake --build build -j4 2>&1 | tail -20
```
Expected: `[100%] Built target sdv_gateway` (or "up to date" if no C++ changed — Task 1 touched no C++, so this should be a no-op build, just confirming nothing else is broken).

- [ ] **Step 2: Run the gateway in the background, capturing output**

```bash
cd /Users/manojkumar/RoadMap/sdv-edge-gateway
./build/sdv_gateway config/config.yaml > /tmp/gateway_smoketest.log 2>&1 &
sleep 3
tail -30 /tmp/gateway_smoketest.log
```
Expected: MQTT connect success, `CommandHandler subscribed to sdv/commands/to/uin/2T1BURHE0JC123456 and sdv/Analytics/to/uin/2T1BURHE0JC123456/diagnosis` in the log. If MQTT connect fails (e.g. expired cert), stop here and report — do not proceed to Step 3 with a non-connected gateway.

- [ ] **Step 3: Send one actuation command**

```bash
cd cloud/iot_setup
python3 send_command.py --uin 2T1BURHE0JC123456 --region eu-west-1 --cmd-type DOOR_LOCK
sleep 2
tail -15 /tmp/gateway_smoketest.log
```
Expected: no "Malformed command JSON, dropped" line. Since this is the Mac build, `CanWriter::write()` is the Mac stub and returns `false` — expect a logged `CanWriter::write failed for correlation_id: <uuid>` line, which on Mac is the CORRECT outcome (stub always fails per its own code) and proves parsing + routing worked; it is not evidence of a bug in this fix.

- [ ] **Step 4: Send an OTA_TRIGGER with a real small HTTPS file**

Use a small, stable public HTTPS file as a stand-in firmware binary and its real SHA256:

```bash
FW_URL="https://raw.githubusercontent.com/manojbaliyan16/sdv-edge-gateway/main/README.md"
FW_SHA=$(curl -sL "$FW_URL" | shasum -a 256 | cut -d' ' -f1)
python3 send_command.py --uin 2T1BURHE0JC123456 --region eu-west-1 \
  --cmd-type OTA_TRIGGER --ota-url "$FW_URL" --ota-sha256 "$FW_SHA" --ota-version test-1.0
sleep 5
tail -30 /tmp/gateway_smoketest.log
```
Expected: `MANIFEST_RECEIVED` → `DOWNLOADING` → `VERIFYING` → `SHA256 verified OK`, then either `INSTALLING` followed by success, OR (expected on Mac, not a bug) a `FAILED — Backup failed` if `/opt/sdv/gateway` doesn't exist on this machine — `installed_path_` defaults to that Pi-only path (`ota_agent.hpp:39`) and `backup_current()` legitimately can't back up a file that isn't there on a dev Mac. Confirm the failure (if any) is exactly at that step, not earlier — anything failing at `MANIFEST_RECEIVED` or `DOWNLOADING` or `VERIFYING` would indicate this fix is still broken.

- [ ] **Step 5: Negative test — wrong SHA256**

```bash
python3 send_command.py --uin 2T1BURHE0JC123456 --region eu-west-1 \
  --cmd-type OTA_TRIGGER --ota-url "$FW_URL" --ota-sha256 "0000000000000000000000000000000000000000000000000000000000000000" --ota-version test-bad
sleep 5
tail -15 /tmp/gateway_smoketest.log
```
Expected: `FAILED — SHA256 mismatch — firmware rejected`.

- [ ] **Step 6: Stop the gateway cleanly**

```bash
GATEWAY_PID=$(pgrep -f "build/sdv_gateway")
kill -SIGINT "$GATEWAY_PID"
sleep 2
tail -10 /tmp/gateway_smoketest.log
```
Expected: clean reverse-order shutdown log lines, process exits (confirm with `pgrep -f "build/sdv_gateway"` returning nothing).

- [ ] **Step 7: Record results**

Append a dated entry to the session note and `CLAUDE.md` (see Task 3) stating exactly which steps passed, which showed the expected Mac-only limitation, and the log evidence (line excerpts), so this isn't asserted without a trail — consistent with how every other fix in this project has been verified this week.

---

### Task 3: Session bookkeeping and push (per CLAUDE.md standing rules)

**Files:**
- Modify: `/Users/manojkumar/RoadMap/CLAUDE.md` (dated entry)
- Modify: `/Users/manojkumar/RoadMap/manoj-roadmap-private/session_notes/2026-07-13_session_summary.md` (new)
- Modify: `/Users/manojkumar/RoadMap/manoj-roadmap-private/session_notes/interview_prep_QA.md` (append)
- Modify: `/Users/manojkumar/RoadMap/manoj-roadmap-private/Manoj_Principal_Architect_52Week_Roadmap_V5.xlsx` (Week 17 row)

- [ ] **Step 1:** Write the session note covering: the schema mismatch found, the fix, the live smoke-test results from Task 2 (exact log lines), and what remains (real Pi deploy/flash still untested from this Mac sandbox; DIAGNOSTIC_QUERY intentionally dropped, not built).

- [ ] **Step 2:** Add a dated entry to `CLAUDE.md`'s sdv-edge-gateway section documenting the bug, the fix, and the commit hash from Task 1.

- [ ] **Step 3:** Append 1-2 Q&A entries to `interview_prep_QA.md` on the topic "how a cross-language wire contract can drift silently between a Python cloud tool and C++ firmware, and how you'd prevent it recurring" — ties into the project's already-logged recurring topic-drift bug class.

- [ ] **Step 4:** Update Week 17 row in the Excel Weekly Tracker (Wins + any additional Actual Hrs).

- [ ] **Step 5: Commit and push both repos**

```bash
cd /Users/manojkumar/RoadMap/sdv-edge-gateway
git add -A
git commit -m "Session 13-Jul-26: downlink schema fix verified live, session docs"
git push

cd /Users/manojkumar/RoadMap/manoj-roadmap-private
git add Manoj_Principal_Architect_52Week_Roadmap_V5.xlsx session_notes/2026-07-13_session_summary.md session_notes/interview_prep_QA.md
git commit -m "Session 13-Jul-26: downlink schema fix, live-tested"
git push
```

No `Co-Authored-By` trailer in either commit (standing user rule).

---

## Self-Review

**Spec coverage:** wire format ✓ (Task 1), single topic ✓ (Task 1 + test), OTA payload trimmed to 3 fields ✓ (Task 1 + test), DIAGNOSTIC_QUERY/CAN_WRITE removed ✓ (test asserts absence), no C++ changes ✓ (Task 2 only runs the existing build, never edits C++), testing plan's 4 scenarios from the spec ✓ (Task 2 steps 3-5 cover actuation + OTA success path + OTA failure path; dedup/QoS-redelivery scenario from the spec's testing plan item 4 is not separately exercised here since it requires two rapid deliveries of the same correlation_id, which boto3's single publish call doesn't naturally trigger — flagged as a residual gap, not silently dropped).

**Placeholder scan:** none found — all code blocks are complete, all commands are exact.

**Type consistency:** `build_actuation_command`/`build_ota_command`/`ACTUATION_TYPES`/`CMD_TYPES`/`COMMAND_TOPIC` names are identical between Task 1's test file and implementation.
