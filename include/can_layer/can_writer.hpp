#pragma once
// can_writer.hpp — writes remote-actuation commands out onto SocketCAN.
//
// Mirrors CanReader's socket open/bind exactly (same interface, same steps).
// No thread here: unlike CanReader (which blocks waiting for inbound frames),
// writes are synchronous and called directly from CommandHandler's consumer
// thread — there's nothing to poll, so a dedicated thread would just add
// complexity for no benefit.

#include <string>
#include <atomic>
#include <cstdint>
#include <unordered_map>

#include "common/types.hpp"   // CommandType

class CanWriter {
public:
    // interface_name: "can0" or "can1" — same convention as CanReader
    explicit CanWriter(const std::string& interface_name);

    ~CanWriter();

    // Opens the SocketCAN socket. Call once before write().
    bool start();

    // Closes the socket. Safe to call even if never started.
    void stop();

    // Encodes `type` into a CAN frame and writes it to the bus.
    // payload_json is accepted for future use (e.g. HORN_LIGHT duration) but
    // this minimal version only sends a single-byte "execute" trigger — see
    // can_writer.cpp for why that's an acceptable simplification here.
    // Returns false if the socket isn't open, the type has no CAN mapping,
    // or the write() syscall fails.
    bool write(CommandType type, const std::string& payload_json);

private:
    // Identical steps to CanReader::open_socket() — same interface, same bind.
    // No SO_RCVTIMEO here since this side never calls read().
    int open_socket();

    std::string       interface_;
    std::atomic<bool> open_{false};
    int               socket_fd_{-1};

    // Demo CAN IDs for this portfolio rig — NOT real OEM DBC assignments.
    // Arbitrary IDs chosen to be visibly non-proprietary, same spirit as the
    // rest of this public repo (see README: no real vehicle signal IDs used).
    static const std::unordered_map<CommandType, uint32_t> COMMAND_CAN_ID;
};
