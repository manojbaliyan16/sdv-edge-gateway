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
