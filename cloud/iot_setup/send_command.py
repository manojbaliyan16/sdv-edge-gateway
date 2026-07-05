#!/usr/bin/env python3
"""
send_command.py — Publish remote ops commands or OTA manifests to a vehicle via AWS IoT Core.

This closes the DOWNLINK path:
  Cloud Operator (this script)
        │
        └── boto3 iot-data publish()
              │
              ▼
        AWS IoT Core (routes by topic ACL per UIN)
              │
              ├── sdv/commands/to/uin/<UIN>         → CommandHandler.cpp → CanWriter
              └── sdv/ota/to/uin/<UIN>/manifest     → OtaAgent.cpp → HTTPS download → A/B flash

Vehicle-side command payload format (CommandHandler.cpp expects):
  {
    "cmd_id":   "<unique ID for dedup — use UUID>",
    "cmd_type": "CAN_WRITE" | "OTA_UPDATE" | "DIAGNOSTIC_QUERY",
    "can_id":   <hex int>    (only for CAN_WRITE),
    "data":     "<hex bytes>" (only for CAN_WRITE, max 8 bytes),
    "priority": 0-255        (optional, lower = higher priority)
  }

OTA manifest format (OtaAgent.cpp expects):
  {
    "version":       "<semver>",
    "url":           "https://<s3-presigned-or-cdn-url>/firmware.bin",
    "sha256":        "<64-char hex digest>",
    "signature":     "<ECDSA base64 signature of sha256>",
    "size_bytes":    <int>,
    "min_battery_v": 11.5,
    "require_stop":  true
  }

Usage:
  # Send a CAN_WRITE command (e.g. activate hazard lights — CAN ID 0x3A0, byte 0 bit 0)
  python3 cloud/iot_setup/send_command.py \\
    --uin VIN_PLACEHOLDER --region eu-west-1 \\
    --cmd-type CAN_WRITE --can-id 0x3A0 --data DEADBEEF00000000

  # Send a diagnostic query (no actuation — just request a DTC snapshot)
  python3 cloud/iot_setup/send_command.py \\
    --uin VIN_PLACEHOLDER --region eu-west-1 \\
    --cmd-type DIAGNOSTIC_QUERY

  # Send an OTA manifest
  python3 cloud/iot_setup/send_command.py \\
    --uin VIN_PLACEHOLDER --region eu-west-1 \\
    --cmd-type OTA_UPDATE \\
    --ota-url https://s3.eu-west-1.amazonaws.com/my-bucket/fw_v1.2.0.bin \\
    --ota-sha256 abc123... \\
    --ota-sig base64sig... \\
    --ota-version 1.2.0

  # Monitor vehicle responses in real time (separate terminal):
  aws iot-data subscribe --topic "sdv/commands/from/uin/VIN_PLACEHOLDER/status"
  aws iot-data subscribe --topic "sdv/ota/from/uin/VIN_PLACEHOLDER/status"
"""

import argparse
import boto3
import json
import uuid
from datetime import datetime

# ─── Command types (must match command_handler.cpp enum) ──────────────────────
CMD_CAN_WRITE        = "CAN_WRITE"
CMD_OTA_UPDATE       = "OTA_UPDATE"
CMD_DIAGNOSTIC_QUERY = "DIAGNOSTIC_QUERY"


def build_can_command(cmd_id: str, can_id_hex: str, data_hex: str, priority: int = 128) -> dict:
    """
    Build a CAN_WRITE command payload.
    can_id_hex: e.g. "0x3A0" or "3A0"
    data_hex:   e.g. "DEADBEEF00000000" (up to 16 hex chars = 8 bytes)
    """
    # Validate
    can_id = int(can_id_hex, 16)
    data_bytes = bytes.fromhex(data_hex.replace("0x", "").replace("0X", ""))
    if len(data_bytes) > 8:
        raise ValueError(f"CAN data too long: {len(data_bytes)} bytes (max 8)")

    return {
        "cmd_id":    cmd_id,
        "cmd_type":  CMD_CAN_WRITE,
        "can_id":    can_id,
        "data":      data_hex.upper()[:16].zfill(16),   # always 8 bytes, zero-padded
        "priority":  priority,
        "timestamp": datetime.utcnow().isoformat(),
    }


def build_diagnostic_query(cmd_id: str) -> dict:
    """
    Build a DIAGNOSTIC_QUERY command.
    Vehicle responds with current DTC snapshot on status topic.
    """
    return {
        "cmd_id":    cmd_id,
        "cmd_type":  CMD_DIAGNOSTIC_QUERY,
        "timestamp": datetime.utcnow().isoformat(),
    }


def build_ota_manifest(version: str, url: str, sha256: str,
                        signature: str, size_bytes: int) -> dict:
    """
    Build OTA manifest. Published to sdv/ota/to/uin/<UIN>/manifest.
    OtaAgent.cpp downloads firmware, verifies SHA256 and ECDSA sig, A/B flashes.
    Preconditions enforced on-device: vehicle must be stopped, battery ≥ 11.5V.
    """
    return {
        "version":       version,
        "url":           url,
        "sha256":        sha256,
        "signature":     signature,
        "size_bytes":    size_bytes,
        "min_battery_v": 11.5,
        "require_stop":  True,   # OtaAgent enforces this — ASIL-C requirement
        "timestamp":     datetime.utcnow().isoformat(),
    }


def publish_to_iot(region: str, topic: str, payload: dict, qos: int = 1):
    """Publish payload to AWS IoT Core data plane. Uses device-side endpoint."""
    iot = boto3.client("iot-data", region_name=region)
    resp = iot.publish(
        topic=topic,
        qos=qos,
        payload=json.dumps(payload),
    )
    return resp


def main():
    parser = argparse.ArgumentParser(
        description="Send remote ops command or OTA manifest to a vehicle via AWS IoT Core"
    )
    parser.add_argument("--uin",      required=True, help="Vehicle UIN / VIN")
    parser.add_argument("--region",   default="eu-west-1")
    parser.add_argument("--cmd-type", required=True,
                        choices=[CMD_CAN_WRITE, CMD_OTA_UPDATE, CMD_DIAGNOSTIC_QUERY])

    # CAN_WRITE args
    parser.add_argument("--can-id",  default="0x3A0", help="CAN ID hex (for CAN_WRITE)")
    parser.add_argument("--data",    default="0000000000000000",
                        help="8-byte hex payload (for CAN_WRITE)")
    parser.add_argument("--priority", type=int, default=128)

    # OTA_UPDATE args
    parser.add_argument("--ota-url",     default="", help="HTTPS URL for firmware binary")
    parser.add_argument("--ota-sha256",  default="", help="64-char hex SHA256 of binary")
    parser.add_argument("--ota-sig",     default="", help="Base64 ECDSA signature")
    parser.add_argument("--ota-version", default="1.0.0")
    parser.add_argument("--ota-size",    type=int, default=0)

    args = parser.parse_args()

    cmd_id = str(uuid.uuid4())

    # ── Build payload based on command type ──────────────────────────────────
    if args.cmd_type == CMD_CAN_WRITE:
        payload = build_can_command(cmd_id, args.can_id, args.data, args.priority)
        topic   = f"sdv/commands/to/uin/{args.uin}"

    elif args.cmd_type == CMD_DIAGNOSTIC_QUERY:
        payload = build_diagnostic_query(cmd_id)
        topic   = f"sdv/commands/to/uin/{args.uin}"

    elif args.cmd_type == CMD_OTA_UPDATE:
        if not args.ota_url or not args.ota_sha256 or not args.ota_sig:
            parser.error("OTA_UPDATE requires --ota-url, --ota-sha256, --ota-sig")
        payload = build_ota_manifest(
            args.ota_version, args.ota_url, args.ota_sha256,
            args.ota_sig, args.ota_size
        )
        topic = f"sdv/ota/to/uin/{args.uin}/manifest"

    # ── Publish ──────────────────────────────────────────────────────────────
    print(f"\n→ Publishing to: {topic}")
    print(f"  Payload:\n{json.dumps(payload, indent=4)}\n")

    try:
        publish_to_iot(args.region, topic, payload)
        print(f"✓ Published successfully (cmd_id={cmd_id})")
        print(f"\nMonitor vehicle response:")
        if args.cmd_type == CMD_OTA_UPDATE:
            print(f"  aws iot-data subscribe --topic \"sdv/ota/from/uin/{args.uin}/status\"")
        else:
            print(f"  aws iot-data subscribe --topic \"sdv/commands/from/uin/{args.uin}/status\"")
    except Exception as e:
        print(f"✗ Publish failed: {e}")
        raise


if __name__ == "__main__":
    main()
