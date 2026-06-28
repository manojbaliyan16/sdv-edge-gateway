#pragma once
// types.hpp — Shared data structures used across all gateway modules.
// Passed through SafeQueue<T> between threads.

#include <string>
#include <cstdint>
#include <chrono>

// ─── CAN Layer ────────────────────────────────────────────────────────────────
 
// Raw CAN frame as read from SocketCAN
struct CanFrame {
    uint32_t can_id;          // 11-bit or 29-bit CAN ID
    uint8_t  dlc;             // Data Length Code (0–8 bytes)
    uint8_t  data[8];
    std::chrono::steady_clock::time_point timestamp;
};

// Decoded signal after DBC parsing
struct DecodedSignal {
    std::string name;         // e.g. "VEHICLE_SPEED"
    double      value;        // Physical value (km/h, RPM, °C, V)
    std::string unit;         // e.g. "km/h"
    std::chrono::system_clock::time_point timestamp;
};

// ─── Telemetry ────────────────────────────────────────────────────────────────

// Telemetry snapshot — one publish per interval
struct TelemetryMsg {
    std::string uin;
    double vehicle_speed_kmh;
    double engine_rpm;
    double engine_coolant_temp_c;
    double battery_voltage_v;
    std::string gear;
    std::chrono::system_clock::time_point timestamp;
    bool from_buffer = false;     // true = replayed from offline SQLite buffer
};

// ─── Remote Commands ──────────────────────────────────────────────────────────

enum class CommandType {
    DOOR_LOCK,
    DOOR_UNLOCK,
    ENGINE_START,
    HORN_LIGHT,
    OTA_TRIGGER,
    UNKNOWN
};

struct Command {
    CommandType type;
    std::string correlation_id;   // Echo back in status for cloud to correlate
    std::string payload_json;     // Full JSON payload (for OTA: manifest URL etc.)
};

// ─── OTA ──────────────────────────────────────────────────────────────────────

enum class OtaState {
    IDLE,
    MANIFEST_RECEIVED,
    DOWNLOADING,
    VERIFYING,
    INSTALLING,
    COMPLETE,
    FAILED,
    ROLLED_BACK
};

struct OtaStatus {
    OtaState    state;
    std::string fw_version;
    std::string error_msg;         // Empty if no error
    int         progress_pct = 0;  // 0–100
};
