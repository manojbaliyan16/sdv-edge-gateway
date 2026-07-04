#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <unordered_map>

#include <mqtt/async_client.h>      // Paho MQTT C++ async client

#include "common/types.hpp"         // DecodedSignal, TelemetryMsg
#include "common/safe_queue.hpp"    // SafeQueue<T>
#include "common/dlt_wrapper.hpp"   // DLT logging stubs (no-op on Mac)

class TelemetryPublisher {
public:
    // queue       — decoded signals popped from SignalDecoder output (shared, not owned)
    // uin         — vehicle UIN (e.g. "VIN_12345") — used to build the publish topic
    // mqtt_client — SHARED connection (see command_handler.hpp for why one MQTT client is
    //               used for both publish and subscribe). Owned + connect()-ed by whoever
    //               wires main.cpp; this class only publishes on it, never connects/disconnects.
    explicit TelemetryPublisher(SafeQueue<DecodedSignal>& queue,
                                const std::string& uin,
                                mqtt::async_client& mqtt_client);
    ~TelemetryPublisher();

    // Connect to MQTT broker (TLS), spawn publisher thread
    // Returns false if broker connection fails
    bool start();

    // Signal thread to stop, join it, disconnect from broker
    void stop();

private:
    // Pop loop — runs on thread_
    // Collects DecodedSignals into accumulators
    // Publishes when: all 5 signals collected OR PUBLISH_INTERVAL_S elapsed
    void run();

    // Build TelemetryMsg from numeric_ + gear_, publish to MQTT, reset accumulators
    void publish_now();

    // ── Identity / connectivity ──────────────────────────────────────────────
    SafeQueue<DecodedSignal>&              queue_;       // shared input queue
    std::string                            uin_;         // vehicle UIN
    mqtt::async_client&                    mqtt_client_; // SHARED, not owned — see ctor comment

    // ── Thread control ───────────────────────────────────────────────────────
    std::thread                            thread_;
    std::atomic<bool>                      running_{false};

    // ── Timer — forces publish every PUBLISH_INTERVAL_S even if < 5 signals ─
    std::chrono::steady_clock::time_point  last_publish_;

    // ── Signal accumulators — reset after every publish ──────────────────────
    // 4 numeric signals: vehicle_speed_kmh, engine_rpm,
    //                    engine_coolant_temp_c, battery_voltage_v
    std::unordered_map<std::string, double> numeric_;

    // gear is a string signal ("P","R","N","D","1".."6") — separate accumulator
    std::string                             gear_;
    bool                                    gear_received_{false};

    // ── Constants ────────────────────────────────────────────────────────────
    static constexpr int  PUBLISH_INTERVAL_S = 5;   // max wait before forced publish
    static constexpr int  QOS               = 2;    // exactly-once for telemetry
    static constexpr auto QUEUE_TIMEOUT     = std::chrono::seconds(1); // pop_for duration
};
