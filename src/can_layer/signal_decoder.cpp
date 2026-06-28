#include "can_layer/signal_decoder.hpp"
#include "common/dlt_wrapper.hpp"
#include <fstream>        // std::ifstream — read DBC file line by line
#include <sstream>        // std::istringstream — parse tokens from each line
#include <regex>          // match BO_ and SG_ lines in DBC
#include <unordered_set>  // O(1) lookup of wanted signal names during load

DLT_DECLARE_CONTEXT(signal_decoder_ctx);

bool SignalDecoder::load(const std::string& dbc_path,
                         const std::vector<std::string>& signal_names)
{
    // ── 1. Open file ───────────────────────────────────────────────────────
    std::ifstream file(dbc_path);
    if (!file.is_open()) {
        DLT_LOG(signal_decoder_ctx, DLT_LOG_ERROR,
                DLT_STRING("Cannot open DBC file:"), DLT_STRING(dbc_path.c_str()));
        return false;
    }

    // ── 2. Build set for O(1) lookup ───────────────────────────────────────
    std::unordered_set<std::string> wanted(signal_names.begin(), signal_names.end());

    // ── 3. Regex patterns ──────────────────────────────────────────────────
    // BO_ 100 VEHICLE_INFO: 8 Vector__XXX
    const std::regex bo_re(R"(^BO_\s+(\d+))");

    // SG_ VEHICLE_SPEED : 5|12@1+ (0.01,0) [0|163.83] 'km/h' Vector__XXX
    const std::regex sg_re(
        R"(^\s+SG_\s+(\w+)\s*:\s*(\d+)\|(\d+)@([01])([+-])\s*\(([^,]+),([^)]+)\)[^']*'([^']*)')");
    //                 ^^^^         ^^^^   ^^^^   ^^^   ^^^        ^^^^     ^^^^            ^^^^
    //                 name      start  length endian signed     scale   offset            unit

    // ── 4. Parse line by line ──────────────────────────────────────────────
    uint32_t    current_can_id = 0;
    std::string line;
    std::smatch m;

    while (std::getline(file, line)) {

        if (std::regex_search(line, m, bo_re)) {
            // New message block — remember its CAN ID
            current_can_id = static_cast<uint32_t>(std::stoul(m[1].str()));
            continue;
        }

        if (std::regex_search(line, m, sg_re)) {
            const std::string name = m[1].str();

            // Not in our wanted set — skip this signal
            if (wanted.find(name) == wanted.end()) {
                continue;
            }

            SignalDef def;
            def.can_id          = current_can_id;
            def.start_bit       = static_cast<uint8_t>(std::stoul(m[2].str()));
            def.length          = static_cast<uint8_t>(std::stoul(m[3].str()));
            def.is_little_endian = (m[4].str() == "1");  // @1 = Intel LE, @0 = Motorola BE
            def.is_signed       = (m[5].str() == "-");   // + = unsigned, - = signed
            def.scale           = std::stod(m[6].str());
            def.offset          = std::stod(m[7].str());
            def.unit            = m[8].str();

            signals_[name] = def;

            DLT_LOG(signal_decoder_ctx, DLT_LOG_INFO,
                    DLT_STRING("Loaded signal:"), DLT_STRING(name.c_str()),
                    DLT_STRING("can_id:"), DLT_STRING(std::to_string(current_can_id).c_str()));
        }
    }

    DLT_LOG(signal_decoder_ctx, DLT_LOG_INFO,
            DLT_STRING("DBC load complete. Signals loaded:"),
            DLT_STRING(std::to_string(signals_.size()).c_str()));

    return true;
}

std::vector<DecodedSignal> SignalDecoder::decode(const CanFrame& frame) const
{
    std::vector<DecodedSignal> results;

    for (const auto& [name, def] : signals_) {

        // ── 1. Skip signals that don't belong to this CAN frame ───────────
        if (def.can_id != frame.can_id) {
            continue;
        }

        // ── 2. Extract raw bits ────────────────────────────────────────────
        // Intel little-endian: start_bit is the LSB position.
        // Bit N in the frame = byte[N/8], bit index (N%8).
        uint64_t raw = 0;
        for (uint8_t i = 0; i < def.length; ++i) {
            uint8_t bit_pos  = def.start_bit + i;   // absolute bit position in frame
            uint8_t byte_idx = bit_pos / 8;          // which byte
            uint8_t bit_idx  = bit_pos % 8;          // which bit inside that byte

            if (byte_idx >= frame.dlc) {
                break;   // signal extends beyond actual data length — truncate
            }

            if (frame.data[byte_idx] & (1u << bit_idx)) {
                raw |= (uint64_t(1) << i);           // set bit i in raw
            }
        }

        // ── 3. Sign-extend if signal is signed ────────────────────────────
        // If MSB of the raw value is 1, the physical value is negative.
        // We extend the sign bit across the remaining upper bits of int64_t.
        int64_t raw_signed = static_cast<int64_t>(raw);
        if (def.is_signed) {
            uint64_t sign_bit = uint64_t(1) << (def.length - 1);
            if (raw & sign_bit) {
                // Fill all bits above length with 1s → two's complement negative
                raw_signed = static_cast<int64_t>(raw | (~(sign_bit - 1)));
            }
        }

        // ── 4. Apply scale and offset → physical value ────────────────────
        // physical = raw * scale + offset  (DBC formula, same for all signals)
        double physical = static_cast<double>(raw_signed) * def.scale + def.offset;

        // ── 5. Build result ────────────────────────────────────────────────
        DecodedSignal sig;
        sig.name      = name;
        sig.value     = physical;
        sig.unit      = def.unit;
        sig.timestamp = std::chrono::system_clock::now();

        results.push_back(std::move(sig));
    }

    return results;
}
