# Downlink Command Schema Fix — Design

**Date:** 2026-07-13
**Status:** Approved
**Scope:** `cloud/iot_setup/send_command.py` only. No C++ changes.

## Problem

The downlink (cloud → vehicle command/OTA path) has never worked end to end,
because the cloud-side tool and the vehicle-side C++ describe two different,
incompatible protocols. Confirmed by reading both sides directly:

**`send_command.py` sends:**
```json
{"cmd_id": "...", "cmd_type": "CAN_WRITE", "can_id": 928, "data": "DEADBEEF...", "priority": 128}
```
to `sdv/commands/to/uin/<uin>`, or
```json
{"version": "...", "url": "...", "sha256": "...", "signature": "...", "size_bytes": 0, "min_battery_v": 11.5, "require_stop": true}
```
to `sdv/ota/to/uin/<uin>/manifest`.

**`CommandHandler.cpp` (`parse_command`, `command_handler.cpp:145-171`) actually expects:**
```json
{"type": "OTA_TRIGGER", "correlation_id": "...", "payload": {...}}
```
on a single topic, `sdv/commands/to/uin/<uin>` — the only topic it subscribes to
for commands (`command_handler.cpp:19,46`). It recognizes exactly five `type`
values: `DOOR_LOCK`, `DOOR_UNLOCK`, `ENGINE_START`, `HORN_LIGHT`, `OTA_TRIGGER`.

Consequences of the mismatch:
- `j.at("type")` (required key) throws for every message `send_command.py`
  sends, since it never sends a `type` key. `parse_command` returns `false`
  100% of the time; every command is logged "Malformed command JSON, dropped".
- The OTA manifest topic (`.../manifest`) has zero subscribers anywhere in the
  C++ code — `CommandHandler` doesn't subscribe to it, `OtaAgent` never
  subscribes to MQTT at all (`ota_agent.hpp`: "used here ONLY for status
  publishing, no subscribe"). An OTA manifest published there is silently
  lost, permanently.
- `CanWriter::write()` (`can_writer.cpp`) sends one hardcoded trigger byte
  (`0x01`) to one of four fixed CAN IDs per named command — it has no
  mechanism to write an arbitrary CAN ID + data payload, so even a
  schema-corrected `CAN_WRITE` couldn't do what its name implies.
- `DIAGNOSTIC_QUERY` has no vehicle-side representation at all: no
  `CommandType` enum value, no case in `route()`.

## Decision

The vehicle-side C++ (`CommandHandler`, `CanWriter`, `OtaAgent`, `types.hpp`)
is frozen and treated as the source of truth. `send_command.py` is rewritten
to speak that protocol exactly. This is the minimal-risk fix: it touches one
Python file instead of the C++ actuation/OTA/build-tested path, and it keeps
the vehicle's command surface to five named, bounded operations rather than
adding an arbitrary-CAN-ID write callable from the cloud — a materially
smaller attack surface for a project whose other half (`safety/TARA.md`) is
built around threat modeling this exact channel.

## Wire protocol (after fix)

All commands publish to a single topic: `sdv/commands/to/uin/<uin>`.

**Actuation** (`DOOR_LOCK`, `DOOR_UNLOCK`, `ENGINE_START`, `HORN_LIGHT`):
```json
{"type": "DOOR_LOCK", "correlation_id": "<uuid4>", "payload": {}}
```
`payload` is empty — `CanWriter::write()` ignores it entirely; the CAN ID and
the single trigger byte are both fixed on the vehicle side per command type.

**OTA** (`OTA_TRIGGER`):
```json
{
  "type": "OTA_TRIGGER",
  "correlation_id": "<uuid4>",
  "payload": {
    "url": "https://.../firmware.bin",
    "sha256": "<64-char hex>",
    "version": "1.2.0"
  }
}
```
These are the only three keys `OtaAgent::run()` reads out of `payload_json`
(`ota_agent.cpp:108-116`); `version` defaults to `"unknown"` if omitted.
`signature`, `size_bytes`, `min_battery_v`, `require_stop` are dropped from
the script — nothing on the vehicle consumes them today. ECDSA signature
verification is a separately-tracked future item (logged 2026-07-11), not
part of this fix; adding those fields back is a one-line change whenever that
work happens.

## Explicitly out of scope

- **`DIAGNOSTIC_QUERY`** is removed from the script, not implemented. There is
  no `CommandType` for it and no handler. A working push-based diagnosis path
  already exists (`AnomalyDetector` → Lambda → `sdv/Analytics/to/uin/<uin>/diagnosis`
  → `CommandHandler::handle_diagnosis()`) and covers the same need this
  session's brainstorm was scoped to fix bugs, not add an on-demand query
  feature. Can be proposed as its own future addition.
- **Status feedback for actuation commands.** The current script's help text
  claims you can `aws iot-data subscribe --topic ".../commands/from/uin/.../status"`
  to see a response — no such topic is ever published by `CanWriter` or
  `CommandHandler`; actuation is fire-and-forget (a bool return value, logged
  locally via DLT only). The rewritten script's usage text will say this
  plainly instead of implying a status topic that doesn't exist. OTA is the
  only downlink command type with real status feedback
  (`sdv/ota/from/uin/<uin>/status`, already correct).
- **No C++ changes of any kind.** `CommandHandler`, `CanWriter`, `OtaAgent`,
  `types.hpp` are untouched.

## File changes

`cloud/iot_setup/send_command.py` — full rewrite of the payload-building and
CLI logic:
- Module docstring rewritten to describe the real wire format and the real
  single topic.
- `CMD_CAN_WRITE`/`CMD_OTA_UPDATE`/`CMD_DIAGNOSTIC_QUERY` constants replaced
  with the five real `CommandType` strings.
- `build_can_command()` and `build_diagnostic_query()` deleted. Replaced with
  one `build_actuation_command(cmd_type, correlation_id)` returning
  `{"type": cmd_type, "correlation_id": ..., "payload": {}}`.
- `build_ota_manifest()` simplified to the three consumed fields, wrapped in
  `{"type": "OTA_TRIGGER", "correlation_id": ..., "payload": {...}}`.
- `--cmd-type` choices become the five real values; `--can-id`/`--data`/
  `--priority` args removed (no longer meaningful); `--ota-sig`/`--ota-size`
  removed; `--ota-url`/`--ota-sha256`/`--ota-version` kept.
- `main()`'s topic selection collapses to the one topic for all five types.
- Usage examples in the docstring updated to show `DOOR_LOCK`/`ENGINE_START`/
  `OTA_TRIGGER` invocations instead of `CAN_WRITE`/`DIAGNOSTIC_QUERY`.

## Testing plan

Run against a live gateway (Mac build or Pi) with the actual MQTT broker:
1. `send_command.py --cmd-type DOOR_LOCK` (and one more actuation type) —
   confirm via the gateway's debug/DLT output that `CommandHandler::route()`
   dispatches to `CanWriter::write()` and it returns `true`.
2. `send_command.py --cmd-type OTA_TRIGGER --ota-url <local HTTPS test file>
   --ota-sha256 <real sha256 of that file>` — confirm the full
   `MANIFEST_RECEIVED → DOWNLOADING → VERIFYING → INSTALLING → COMPLETE`
   sequence appears in `sdv/ota/from/uin/<uin>/status`, using a small local
   test binary so the download+hash+flash+rollback path is genuinely
   exercised, not just parsed.
3. Negative case: bad `--ota-sha256` (deliberately wrong) — confirm
   `FAILED` with "SHA256 mismatch" and that the tampered file gets deleted
   (`ota_agent.cpp:144`), not just logged.
4. Confirm a duplicate publish (same `correlation_id`) is dropped by
   `already_seen()` as a QoS-1-redelivery dedup, not double-actuated.
