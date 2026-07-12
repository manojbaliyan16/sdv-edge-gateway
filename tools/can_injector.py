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
    # CAN ID 0x0B4: GEAR_SHIFT (byte 0) + ENGINE_RPM (bytes 3-4, factor 0.25) + VEHICLE_SPEED (bytes 5-6, factor 0.01)
    {"can_id": 0x0B4, "speed_kmh": 60.0, "rpm": 2000.0, "gear": 3},
    # CAN ID 0x1C4: ENGINE_COOLANT_TEMP (byte 0, factor 1, offset -40)
    {"can_id": 0x1C4, "coolant_c": 85.0},
    # CAN ID 0x3B3: BATTERY_VOLTAGE (byte 0, factor 0.1)
    {"can_id": 0x3B3, "battery_v": 12.4},
]

# GEAR_SHIFT raw codes — must match dbc/toyota_corolla.dbc and
# gear_code_to_string() in src/gateway/telemetry_publisher.cpp
GEAR_PARK, GEAR_REVERSE, GEAR_NEUTRAL, GEAR_DRIVE = 0, 1, 2, 3

# ── Drive cycle — a repeating park/accelerate/cruise/decelerate/park loop ──────
# Gear is derived from the phase, not injected independently, so speed and gear
# never disagree (e.g. never GEAR_DRIVE at speed_kmh == 0).
PARK_S       = 3.0    # seconds stopped in Park before pulling away
ACCEL_S      = 5.0    # seconds ramping 0 -> CRUISE_KMH
CRUISE_S     = 5.0    # seconds holding CRUISE_KMH
DECEL_S      = 5.0    # seconds ramping CRUISE_KMH -> 0
CRUISE_KMH   = 60.0
IDLE_RPM     = 800.0
CRUISE_RPM   = 2000.0
CYCLE_S      = PARK_S + ACCEL_S + CRUISE_S + DECEL_S

def drive_cycle(t: float):
    """Given elapsed seconds, return (speed_kmh, rpm, gear) for the current
    phase of the repeating park -> accelerate -> cruise -> decelerate loop."""
    phase = t % CYCLE_S

    if phase < PARK_S:
        return 0.0, IDLE_RPM, GEAR_PARK

    phase -= PARK_S
    if phase < ACCEL_S:
        frac = phase / ACCEL_S
        return CRUISE_KMH * frac, IDLE_RPM + (CRUISE_RPM - IDLE_RPM) * frac, GEAR_DRIVE

    phase -= ACCEL_S
    if phase < CRUISE_S:
        return CRUISE_KMH, CRUISE_RPM, GEAR_DRIVE

    phase -= CRUISE_S
    frac = phase / DECEL_S
    return CRUISE_KMH * (1.0 - frac), CRUISE_RPM - (CRUISE_RPM - IDLE_RPM) * frac, GEAR_DRIVE

def build_engine_data_frame(speed_kmh: float, rpm: float, gear: int) -> bytes:
    """Encode GEAR_SHIFT, ENGINE_RPM, VEHICLE_SPEED into CAN ID 0x0B4 payload.
    Intel LE (little-endian): LSB at lower byte address, MSB at higher.
    Matches DBC: GEAR_SHIFT start_bit=0|8@1+, ENGINE_RPM start_bit=24|16@1+,
    VEHICLE_SPEED start_bit=40|16@1+
    """
    speed_raw = int(speed_kmh / 0.01)
    rpm_raw   = int(rpm / 0.25)
    data = bytearray(8)
    data[0] = gear & 0xFF                # GEAR_SHIFT         (start_bit 0 = byte 0)
    # Intel LE: low byte at lower index — must match SignalDecoder bit extraction
    data[5] = speed_raw & 0xFF           # VEHICLE_SPEED LSB  (start_bit 40 = byte 5)
    data[6] = (speed_raw >> 8) & 0xFF   # VEHICLE_SPEED MSB  (byte 6)
    data[3] = rpm_raw & 0xFF             # ENGINE_RPM LSB     (start_bit 24 = byte 3)
    data[4] = (rpm_raw >> 8) & 0xFF     # ENGINE_RPM MSB     (byte 4)
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
    print(f"[can_injector] Sending on {args.interface}, interval={args.interval}s. "
          f"Drive cycle: Park {PARK_S:.0f}s -> Accel {ACCEL_S:.0f}s -> Cruise {CRUISE_S:.0f}s "
          f"-> Decel {DECEL_S:.0f}s (repeats). Ctrl+C to stop.")

    start = time.monotonic()
    try:
        while True:
            speed_kmh, rpm, gear = drive_cycle(time.monotonic() - start)
            bus.send(can.Message(arbitration_id=0x0B4, data=build_engine_data_frame(speed_kmh, rpm, gear), is_extended_id=False))
            bus.send(can.Message(arbitration_id=0x1C4, data=build_coolant_frame(85.0),           is_extended_id=False))
            bus.send(can.Message(arbitration_id=0x3B3, data=build_battery_frame(12.4),           is_extended_id=False))
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n[can_injector] Stopped.")
    finally:
        bus.shutdown()

if __name__ == "__main__":
    main()
