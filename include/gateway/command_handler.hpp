#pragma once
// command_handler.hpp — receives remote commands and cloud diagnostics over MQTT.
//
// Subscribes to TWO topics (both cloud→vehicle downlink):
//   sdv/commands/to/uin/<uin>            — remote ops (CAN_WRITE, OTA_UPDATE, DIAGNOSTIC_QUERY)
//   sdv/Analytics/to/uin/<uin>/diagnosis — LLM diagnosis from diagnostic_agent Lambda
//
// QoS: 1 (at-least-once). Dedup guard prevents double-execution on redelivery.

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <chrono>

#include <mqtt/async_client.h>      // Paho MQTT C++ async client

#include "common/types.hpp"         // Command, CommandType
#include "common/safe_queue.hpp"    // SafeQueue<T>
#include "common/dlt_wrapper.hpp"   // DLT logging stubs (no-op on Mac)
#include "can_layer/can_writer.hpp" // CanWriter — direct actuation path

class CommandHandler {
public:
    // mqtt_client — SHARED with TelemetryPublisher (same reasoning as there):
    //               one MQTT connection per vehicle, multiplexing publish and
    //               subscribe, same as the DataCollect/RemoteServices channels
    //               over one bridge in a real TCU. Already connect()-ed by
    //               whoever wires main.cpp — this class only subscribes on it.
    // ota_queue   — OTA_TRIGGER commands are pushed here. OtaAgent (not yet
    //               built) will pop from this queue and run the actual OTA flow.
    // can_writer  — DOOR_LOCK / DOOR_UNLOCK / ENGINE_START / HORN_LIGHT are
    //               executed immediately by writing a CAN frame through this.
    // uin         — vehicle UIN, used to build the subscribe topic.
    explicit CommandHandler(mqtt::async_client& mqtt_client,
                            SafeQueue<Command>& ota_queue,
                            CanWriter& can_writer,
                            const std::string& uin);
    ~CommandHandler();

    // Subscribes to sdv/commands/to/uin/<uin> and sdv/Analytics/to/uin/<uin>/diagnosis
    // (QoS 1), spawns the consumer thread.
    // Returns false if the client isn't connected yet or subscribe fails.
    bool start();

    // Signals the thread to stop, joins it, unsubscribes.
    void stop();

private:
    // Consume loop — runs on thread_. Pulls messages off mqtt_client_'s
    // internal consumer queue (start_consuming() must be called first).
    void run();

    // Parses the raw JSON payload into a Command. Returns false on malformed
    // JSON or a missing "type" field (out is left untouched in that case).
    bool parse_command(const std::string& payload, Command& out);

    // QoS 1 = at-least-once: the broker may redeliver the same message after
    // a reconnect. Without this guard, a redelivered ENGINE_START would
    // execute twice. Returns true (and does NOT re-mark it) if already seen.
    bool already_seen(const std::string& correlation_id);

    // Dispatches a parsed, de-duplicated Command to its executor.
    void route(const Command& cmd);

    // ── Shared resources — none of these are owned by this class ────────────
    mqtt::async_client&  mqtt_client_;
    SafeQueue<Command>&  ota_queue_;
    CanWriter&           can_writer_;

    // Parses a diagnosis JSON payload and logs it via DLT.
    // diagnostic_agent Lambda publishes:
    //   {"root_cause": "...", "recommended_action": "...", "driver_message": "...",
    //    "dtc": "P0217", "asil": "ASIL-B", "escalate_human": false}
    void handle_diagnosis(const std::string& payload);

    // ── Identity ─────────────────────────────────────────────────────────────
    std::string          uin_;
    std::string          topic_;           // sdv/commands/to/uin/<uin>
    std::string          diagnosis_topic_; // sdv/Analytics/to/uin/<uin>/diagnosis

    // ── Thread control ───────────────────────────────────────────────────────
    std::thread          thread_;
    std::atomic<bool>    running_{false};

    // ── QoS 1 dedup guard ────────────────────────────────────────────────────
    std::mutex                      seen_mutex_;
    std::unordered_set<std::string> seen_ids_;
    // Simple cap, not an LRU: once this many IDs are tracked, the whole set is
    // cleared rather than evicting the oldest individually. This project's
    // command volume is human-triggered remote ops, not high-frequency
    // telemetry, so the rare false-negative after a long session is an
    // acceptable trade-off against the complexity of a proper ring buffer.
    static constexpr size_t MAX_SEEN_IDS = 200;

    // ── Constants ────────────────────────────────────────────────────────────
    static constexpr int  QOS           = 1;
    static constexpr auto CONSUME_TIMEOUT = std::chrono::seconds(1);
};
