#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "common/types.hpp"

class SignalDecoder {
public:
    // Load DBC file and filter to only the signals we care about.
    // signal_names: list from config.yaml — e.g. ["VEHICLE_SPEED", "ENGINE_RPM"]
    // Returns false if file cannot be opened or parsed.
    bool load(const std::string& dbc_path,
              const std::vector<std::string>& signal_names);

    // Decode a CAN frame — returns all signals found in this frame.
    // Returns empty vector if no matching signals in this frame.
    std::vector<DecodedSignal> decode(const CanFrame& frame) const;

private:
    struct SignalDef {
        uint32_t    can_id;           // which CAN frame this signal belongs to
        uint8_t     start_bit;        // bit position in the frame
        uint8_t     length;           // number of bits
        bool        is_little_endian; // true=Intel, false=Motorola
        bool        is_signed;        // signed or unsigned raw value
        double      scale;            // physical = raw * scale + offset
        double      offset;
        std::string unit;             // "km/h", "RPM", "°C" etc.
    };

    // signal name → definition. Populated by load(), read by decode().
    std::unordered_map<std::string, SignalDef> signals_;
};
