#pragma once

#include <string>
#include <thread>
#include <atomic>
#include "common/types.hpp"
#include "common/safe_queue.hpp"

class CanReader {
public:
    // interface_name: "can0" or "can1" — read from config.yaml
    // queue: shared SafeQueue owned by main(), pushed into by this reader
    explicit CanReader(SafeQueue<CanFrame>& queue,
                       const std::string& interface_name);

    ~CanReader();

    // Spawns the reader thread, opens SocketCAN socket
    bool start();

    // Signals the thread to stop, waits for it to exit (join)
    void stop();

private:
    // The read loop — runs on thread_
    void run();

    // Opens SocketCAN socket for interface_
    // Sets 1-second recv timeout so run() can check running_ periodically
    // Returns socket fd on success, -1 on failure
    int open_socket();

    SafeQueue<CanFrame>& queue_;          // shared queue — not owned
    std::string          interface_;      // "can0" / "can1"
    std::atomic<bool>    running_{false}; // stop flag — checked every loop
    std::thread          thread_;         // reader thread
    int                  socket_fd_{-1}; // SocketCAN socket descriptor
};
