#!/usr/bin/env python3
"""
can_injector.py — Test utility: inject simulated CAN frames on can1.
The C++ gateway reads can1 and processes them.
Usage: python3 tools/can_injector.py --interface can1
"""
import can
import time
import argparse
import struct

# Toyota Corolla DBC signal definitions (subset we care about)
# These match the CAN IDs in dbc/toyota_corolla.dbc
FRAMES = [
    # CAN ID 0x0B4: VEHICLE_SPEED (bytes 5-6, factor 0.01) + ENGINE_RPM (bytes 3-4, factor 0.25)
    {"can_id": 0x0B4, "speed_kmh": 60.0, "rpm": 2000.0},
    # CAN ID 0x1C4: ENGINE_COOLANT_TEMP (byte 0, factor 1, offset -40)
    {"can_id": 0x1C4, "coolant_c": 85.0},
    # CAN ID 0x3B3: BATTERY_VOLTAGE (byte 0, factor 0.1)
    {"can_id": 0x3B3, "battery_v": 12.4},
]

def build_speed_rpm_frame(speed_kmh: float, rpm: float) -> bytes:
    """Encode VEHICLE_SPEED and ENGINE_RPM into CAN ID 0x0B4 payload."""
    speed_raw = int(speed_kmh / 0.01)
    rpm_raw   = int(rpm / 0.25)
    data = bytearray(8)
    # Big-endian encoding per Toyota DBC
    data[5] = (speed_raw >> 8) & 0xFF
    data[6] = speed_raw & 0xFF
    data[3] = (rpm_raw >> 8) & 0xFF
    data[4] = rpm_raw & 0xFF
    return bytes(data)

def build_coolant_frame(temp_c: float) -> bytes:
    data = bytearray(8)
    data[0] = int(temp_c + 40) & 0xFF   # offset -40
    return bytes(data)

def build_battery_frame(voltage_v: float) -> bytes:
    data = bytearray(8)
    data[0] = int(voltage_v / 0.1) & 0xFF
    return bytes(data)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--interface", default="can1")
    parser.add_argument("--interval", type=float, default=0.1, help="Seconds between frames")
    args = parser.parse_args()

    bus = can.interface.Bus(channel=args.interface, bustype="socketcan")
    print(f"[can_injector] Sending on {args.interface}, interval={args.interval}s. Ctrl+C to stop.")

    try:
        while True:
            bus.send(can.Message(arbitration_id=0x0B4, data=build_speed_rpm_frame(60.0, 2000.0), is_extended_id=False))
            bus.send(can.Message(arbitration_id=0x1C4, data=build_coolant_frame(85.0),           is_extended_id=False))
            bus.send(can.Message(arbitration_id=0x3B3, data=build_battery_frame(12.4),           is_extended_id=False))
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n[can_injector] Stopped.")
    finally:
        bus.shutdown()

if __name__ == "__main__":
    main()
