#include "can_layer/can_writer.hpp"
#include "common/dlt_wrapper.hpp"

#ifdef __linux__

#include <linux/can.h>        // struct can_frame, CAN_RAW
#include <linux/can/raw.h>    // CAN_RAW socket options
#include <net/if.h>           // struct ifreq, if_nametoindex
#include <sys/socket.h>       // socket(), bind(), setsockopt()
#include <sys/ioctl.h>        // ioctl() — get interface index by name
#include <unistd.h>           // close()
#include <cstring>            // memset(), strncpy()

DLT_DECLARE_CONTEXT(can_writer_ctx);

// ─── Demo CAN ID table ────────────────────────────────────────────────────────
// Static member — one definition, shared across all CanWriter instances.

const std::unordered_map<CommandType, uint32_t> CanWriter::COMMAND_CAN_ID = {
    {CommandType::DOOR_LOCK,    0x300},
    {CommandType::DOOR_UNLOCK,  0x301},
    {CommandType::ENGINE_START, 0x302},
    {CommandType::HORN_LIGHT,   0x303},
    // OTA_TRIGGER and UNKNOWN intentionally absent — they never reach this
    // class. CommandHandler routes OTA_TRIGGER to OtaAgent's queue instead;
    // see command_handler.cpp::route().
};

// ─── Constructor / Destructor ────────────────────────────────────────────────

CanWriter::CanWriter(const std::string& interface_name)
    : interface_(interface_name)
{}

CanWriter::~CanWriter()
{
    stop();   // safe to call even if never started
}

// ─── Public: start ───────────────────────────────────────────────────────────

bool CanWriter::start()
{
    socket_fd_ = open_socket();
    if (socket_fd_ < 0) {
        DLT_LOG(can_writer_ctx, DLT_LOG_ERROR,
                DLT_STRING("Failed to open SocketCAN on"), DLT_STRING(interface_.c_str()));
        return false;
    }

    open_ = true;
    DLT_LOG(can_writer_ctx, DLT_LOG_INFO,
            DLT_STRING("CanWriter ready on"), DLT_STRING(interface_.c_str()));
    return true;
}

// ─── Public: stop ────────────────────────────────────────────────────────────

void CanWriter::stop()
{
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);   // close SocketCAN socket
        socket_fd_ = -1;
    }
    open_ = false;
}

// ─── Private: open_socket ────────────────────────────────────────────────────
// Same 3 steps as CanReader::open_socket(): create raw CAN socket, resolve
// interface name to index, bind. No recv timeout — this side only writes.

int CanWriter::open_socket()
{
    int fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface_.c_str(), IFNAMSIZ - 1);

    if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        ::close(fd);
        return -1;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

// ─── Public: write ───────────────────────────────────────────────────────────
//
// NOTE the "::write" below — this method is ALSO named write(). Inside a
// member function, an unqualified call resolves against the class's own
// members first, so a bare `write(...)` here would try to call
// CanWriter::write() recursively instead of the POSIX write() syscall.
// The "::" forces global-namespace lookup. CanReader avoids this collision
// by naming its method run(), not read() — worth knowing either pattern.

bool CanWriter::write(CommandType type, const std::string& /*payload_json*/)
{
    if (!open_ || socket_fd_ < 0) {
        return false;
    }

    auto it = COMMAND_CAN_ID.find(type);
    if (it == COMMAND_CAN_ID.end()) {
        DLT_LOG(can_writer_ctx, DLT_LOG_WARN,
                DLT_STRING("No CAN mapping for this CommandType — dropped"));
        return false;
    }

    // Minimal encoding: 1 byte, value 0x01 = "execute". payload_json isn't
    // wire-encoded here — a real implementation would bit-pack it per a DBC
    // signal definition (like SignalDecoder does in reverse for inbound
    // frames), but a single trigger byte is enough to prove the write path
    // for this project's scope (interview talking point: "encoding follows
    // the same DBC pattern as signal_decoder, just not built out yet").
    struct can_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.can_id  = it->second;
    frame.can_dlc = 1;
    frame.data[0] = 0x01;

    ssize_t nbytes = ::write(socket_fd_, &frame, sizeof(frame));

    DLT_LOG(can_writer_ctx, DLT_LOG_INFO,
            DLT_STRING("CAN command written, id:"),
            DLT_STRING(std::to_string(frame.can_id).c_str()));

    return nbytes == static_cast<ssize_t>(sizeof(frame));
}

#endif // __linux__
