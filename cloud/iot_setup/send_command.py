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
