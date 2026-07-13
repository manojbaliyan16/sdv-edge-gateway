// Regression test for CommandHandler::stop() shutdown latency.
//
// Bug: CommandHandler::stop() calls mqtt_client_.unsubscribe(topic)->wait()
// with no timeout. Paho's wait() blocks until the broker sends UNSUBACK --
// forever, if it never does. Same defect shape as the QoS 2 publish hang
// already fixed in telemetry_publisher.cpp (AWS IoT Core silently drops a
// connection it can't complete a handshake on; here, a broker that accepts
// the session but goes silent on UNSUBSCRIBE has the identical effect).
//
// This test drives the REAL CommandHandler against fake_broker.py, which
// ACKs CONNECT and SUBSCRIBE normally but deliberately never sends UNSUBACK.
// Before the fix: stop() never returns -- this process hangs until an
// external `timeout` wrapper kills it (see run_command_handler_shutdown_test.sh).
// After the fix: stop() must return within SHUTDOWN_BOUND_MS.
#include <mqtt/async_client.h>
#include <iostream>
#include <chrono>
#include <cstdlib>

#include "gateway/command_handler.hpp"
#include "can_layer/can_writer.hpp"
#include "common/safe_queue.hpp"
#include "common/types.hpp"

namespace {
constexpr int SHUTDOWN_BOUND_MS = 3000;
}

int main(int argc, char** argv) {
    std::string port = argc > 1 ? argv[1] : "18884";
    mqtt::async_client client("tcp://127.0.0.1:" + port, "test-command-handler");

    mqtt::connect_options conn_opts;
    conn_opts.set_clean_session(true);
    conn_opts.set_automatic_reconnect(false);
    conn_opts.set_connect_timeout(3);

    std::cout << "[test] connecting to fake broker...\n" << std::flush;
    client.connect(conn_opts)->wait();
    std::cout << "[test] connected.\n" << std::flush;

    SafeQueue<Command> ota_queue;
    CanWriter can_writer("can0");   // never start()-ed -- no real socket needed for this test

    CommandHandler handler(client, ota_queue, can_writer, "TESTUIN");

    std::cout << "[test] starting CommandHandler (subscribes, spawns thread)...\n" << std::flush;
    if (!handler.start()) {
        std::cout << "[test] FAIL: CommandHandler::start() returned false\n";
        return 1;
    }
    std::cout << "[test] started. now calling stop() -- broker will never "
                 "UNSUBACK -- timing it...\n" << std::flush;

    auto begin = std::chrono::steady_clock::now();
    handler.stop();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();

    std::cout << "[test] stop() returned after " << elapsed_ms << "ms\n" << std::flush;

    if (elapsed_ms > SHUTDOWN_BOUND_MS) {
        std::cout << "[test] FAIL: stop() took " << elapsed_ms
                  << "ms, expected <= " << SHUTDOWN_BOUND_MS << "ms\n";
        return 1;
    }

    std::cout << "[test] PASS\n";
    return 0;
}
