#pragma once
// ota_agent.hpp — Executes OTA firmware updates triggered by CommandHandler.
//
// Flow:  ota_queue (Command) → precondition check → HTTPS download
//        → SHA256 verify → backup current → flash new → report status
//        → rollback on any failure after backup
//
// Runs on its own thread. Publishes OtaStatus back to cloud on topic:
//   sdv/ota/from/uin/<uin>/status

#include <string>
#include <thread>
#include <atomic>

#include <mqtt/async_client.h>      // Paho MQTT C++ — for status reporting

#include "common/types.hpp"         // Command, OtaStatus, OtaState
#include "common/safe_queue.hpp"    // SafeQueue<Command> — input queue
#include "common/dlt_wrapper.hpp"   // DLT logging stubs (no-op on Mac)

class OtaAgent {
public:
    // ota_queue      — receives OTA_TRIGGER Commands from CommandHandler
    // mqtt_client    — shared with TelemetryPublisher + CommandHandler;
    //                  used here ONLY for status publishing (no subscribe)
    // uin            — vehicle UIN; used to build status topic + log context
    // firmware_path  — where downloaded firmware is written before verification
    //                  default: /tmp/sdv_firmware_new.bin
    // backup_path    — copy of current firmware saved before flash attempt
    //                  used for rollback if flash fails
    //                  default: /tmp/sdv_firmware_backup.bin
    // installed_path — path of the currently running firmware binary
    //                  on Pi: /opt/sdv/gateway (the actual executable)
    explicit OtaAgent(SafeQueue<Command>&  ota_queue,
                      mqtt::async_client&  mqtt_client,
                      const std::string&   uin,
                      const std::string&   firmware_path  = "/tmp/sdv_firmware_new.bin",
                      const std::string&   backup_path    = "/tmp/sdv_firmware_backup.bin",
                      const std::string&   installed_path = "/opt/sdv/gateway");

    ~OtaAgent();

    // Spawns the OTA worker thread. Must be called after MQTT client is connected.
    bool start();

    // Signals thread to stop and joins it.
    void stop();

private:
    // ── Main loop ────────────────────────────────────────────────────────────
    // Blocks on ota_queue_.pop() waiting for OTA_TRIGGER commands.
    // Runs the full OTA flow for each command.
    void run();

    // ── OTA flow steps — called in sequence from run() ───────────────────────

    // Checks battery voltage > 12.0V, vehicle speed == 0, free disk space.
    // Returns false if any condition not met — OTA is deferred, not aborted.
    bool check_preconditions();

    // Downloads firmware from url to firmware_path_ using libcurl over HTTPS.
    // Returns false on network error or HTTP status != 200.
    bool download_firmware(const std::string& url);

    // Computes SHA256 of file at firmware_path_, compares to expected_hash.
    // Returns false if mismatch — prevents flashing tampered firmware.
    bool verify_sha256(const std::string& expected_hash);

    // Copies installed_path_ → backup_path_ before overwriting.
    // Must succeed before flash is attempted — no backup = no rollback.
    bool backup_current();

    // Copies firmware_path_ → installed_path_.
    // On failure: calls rollback() immediately.
    bool flash_firmware();

    // Restores backup_path_ → installed_path_.
    // Called automatically if flash_firmware() fails.
    bool rollback();

    // Publishes OtaStatus to "sdv/ota/from/uin/<uin>/status" as JSON.
    // correlation_id is echoed back so cloud can match this status to its request.
    void report_status(const std::string& correlation_id, const OtaStatus& status);

    // ── Injected dependencies ─────────────────────────────────────────────────
    SafeQueue<Command>&  ota_queue_;     // input — OTA_TRIGGER commands from CommandHandler
    mqtt::async_client&  mqtt_client_;   // output — publish OtaStatus to cloud
    std::string          uin_;           // vehicle identifier

    // ── Paths ─────────────────────────────────────────────────────────────────
    std::string          firmware_path_;   // download target
    std::string          backup_path_;     // rollback source
    std::string          installed_path_;  // firmware currently running

    // ── Thread control ────────────────────────────────────────────────────────
    std::thread          thread_;
    std::atomic<bool>    running_{false};

    // ── Constants ─────────────────────────────────────────────────────────────
    static constexpr int    QOS               = 1;    // status publish QoS
    static constexpr double MIN_BATTERY_V     = 12.0; // precondition: volts
    static constexpr int    MAX_SPEED_KMH     = 0;    // precondition: parked only
};
