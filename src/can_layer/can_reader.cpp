#include "can_layer/can_reader.hpp"
#include "common/dlt_wrapper.hpp"

#ifdef __linux__

#include <linux/can.h>        // struct can_frame, CAN_RAW
#include <linux/can/raw.h>    // CAN_RAW socket options
#include <net/if.h>           // struct ifreq, if_nametoindex
#include <sys/socket.h>       // socket(), bind(), setsockopt()
#include <sys/ioctl.h>        // ioctl() — get interface index by name
#include <unistd.h>           // close(), read()
#include <cstring>            // memset(), strncpy()

DLT_DECLARE_CONTEXT(can_reader_ctx);

// ─── Constructor / Destructor ────────────────────────────────────────────────

CanReader::CanReader(SafeQueue<CanFrame>& queue,
                     const std::string& interface_name)
    : queue_(queue)
    , interface_(interface_name)
{}

CanReader::~CanReader()
{
    stop();  // safe to call even if never started
}

// ─── Public: start ───────────────────────────────────────────────────────────

bool CanReader::start()
{
    socket_fd_ = open_socket();
    if (socket_fd_ < 0) {
        DLT_LOG(can_reader_ctx, DLT_LOG_ERROR,
                DLT_STRING("Failed to open SocketCAN on"), DLT_STRING(interface_.c_str()));
        return false;
    }

    running_ = true;
    thread_  = std::thread(&CanReader::run, this);  // spawn reader thread

    DLT_LOG(can_reader_ctx, DLT_LOG_INFO,
            DLT_STRING("CanReader started on"), DLT_STRING(interface_.c_str()));
    return true;
}

// ─── Public: stop ────────────────────────────────────────────────────────────

void CanReader::stop()
{
    running_ = false;          // signal the run() loop to exit

    if (thread_.joinable()) {
        thread_.join();        // wait for thread to finish cleanly
    }

    if (socket_fd_ >= 0) {
        ::close(socket_fd_);   // close SocketCAN socket
        socket_fd_ = -1;
    }

    DLT_LOG(can_reader_ctx, DLT_LOG_INFO,
            DLT_STRING("CanReader stopped on"), DLT_STRING(interface_.c_str()));
}

// ─── Private: open_socket ────────────────────────────────────────────────────

int CanReader::open_socket()
{
    // Step 1 — create a raw CAN socket (same as SOCK_RAW for Ethernet)
    int fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        return -1;
    }

    // Step 2 — look up the interface index by name ("can0" → integer)
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface_.c_str(), IFNAMSIZ - 1);

    if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        ::close(fd);
        return -1;
    }

    // Step 3 — bind socket to this CAN interface
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    // Step 4 — set 1-second receive timeout
    // read() will return EAGAIN after 1s if no frame arrives
    // This lets run() check running_ periodically instead of blocking forever
    struct timeval tv;
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return fd;
}

// ─── Private: run ────────────────────────────────────────────────────────────

void CanReader::run()
{
    struct can_frame raw;   // Linux kernel CAN frame struct (8 bytes data + header)

    while (running_) {

        // Blocking read — returns after a frame arrives OR after 1s timeout
        ssize_t nbytes = ::read(socket_fd_, &raw, sizeof(raw));

        if (nbytes < 0) {
            // EAGAIN / EWOULDBLOCK = timeout, no frame arrived — check running_ and loop
            // Any other error = real problem — log and continue
            continue;
        }

        if (nbytes < static_cast<ssize_t>(sizeof(raw))) {
            // Incomplete frame — discard
            continue;
        }

        // ── Translate kernel struct → our CanFrame type ──────────────────────
        CanFrame frame;
        frame.can_id    = raw.can_id & CAN_EFF_MASK;  // strip flags, keep ID
        frame.dlc       = raw.can_dlc;
        frame.timestamp = std::chrono::steady_clock::now();
        std::memcpy(frame.data, raw.data, raw.can_dlc);

        // ── Push into shared queue — signal_decoder will pop from here ────────
        queue_.push(frame);

        DLT_LOG(can_reader_ctx, DLT_LOG_INFO,
                DLT_STRING("CAN frame received, id:"),
                DLT_STRING(std::to_string(frame.can_id).c_str()));
    }
}

#endif // __linux__
