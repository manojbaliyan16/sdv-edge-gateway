// main.cpp — SDV Edge Gateway entry point
// Wires all modules together, owns all queues, runs the decode fan-out loop.
//
// Data flow (uplink):
//   can1 → CanReader → SafeQueue<CanFrame>
//                         ↓ decode loop (this thread)
//                      SignalDecoder::decode()
//                         ↓ fan-out push
//              telemetry_q → TelemetryPublisher → MQTT
//              anomaly_q   → AnomalyDetector    → MQTT alerts
//
// Data flow (downlink):
//   MQTT → CommandHandler → SafeQueue<Command> → OtaAgent
//                        └→ CanWriter → can0 (DOOR_LOCK / UNLOCK / ENGINE_START)

#include <yaml-cpp/yaml.h>
#include <mqtt/async_client.h>

#include <csignal>
#include <atomic>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>

#include "common/types.hpp"
#include "common/safe_queue.hpp"
#include "can_layer/can_reader.hpp"
#include "can_layer/can_writer.hpp"
#include "can_layer/signal_decoder.hpp"
#include "gateway/telemetry_publisher.hpp"
#include "gateway/command_handler.hpp"
#include "gateway/ota_agent.hpp"
#include "gateway/anomaly_detector.hpp"

// ─── Shutdown flag ────────────────────────────────────────────────────────────
// Set by SIGINT/SIGTERM handler — checked by the decode loop in main().
// atomic so the signal handler (different OS context) writes it safely.

static std::atomic<bool> g_running{true};

static void signal_handler(int /*sig*/)
{
    g_running = false;
}

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    // ── 1. Load config ────────────────────────────────────────────────────────
    const std::string config_path = (argc > 1) ? argv[1] : "config/config.yaml";

    YAML::Node cfg;
    try {
        cfg = YAML::LoadFile(config_path);
    } catch (const YAML::Exception& e) {
        std::cerr << "[FATAL] Cannot load config: " << e.what() << "\n";
        return 1;
    }

    const std::string uin          = cfg["device"]["uin"].as<std::string>();
    const std::string can_iface_rx = cfg["can"]["interface_rx"].as<std::string>();  // read CAN frames
    const std::string can_iface_tx = cfg["can"]["interface_tx"].as<std::string>();  // write CAN frames
    const std::string dbc_path     = cfg["can"]["dbc_path"].as<std::string>();
    const auto        signal_names = cfg["can"]["signals"].as<std::vector<std::string>>();

    const std::string broker      = "ssl://" + cfg["mqtt"]["broker"].as<std::string>()
                                    + ":" + cfg["mqtt"]["port"].as<std::string>();
    const std::string client_id   = cfg["mqtt"]["client_id"].as<std::string>();
    const int         keep_alive  = cfg["mqtt"]["keep_alive_sec"].as<int>();

    const std::string ca_cert     = cfg["tls"]["ca_cert"].as<std::string>();
    const std::string device_cert = cfg["tls"]["device_cert"].as<std::string>();
    const std::string device_key  = cfg["tls"]["device_key"].as<std::string>();

    // Anomaly config — optional section, sane defaults if absent
    const std::string model_path  = cfg["anomaly"] && cfg["anomaly"]["model_path"]
                                    ? cfg["anomaly"]["model_path"].as<std::string>()
                                    : "/opt/sdv/anomaly_detector.onnx";
    const float anomaly_threshold = cfg["anomaly"] && cfg["anomaly"]["threshold"]
                                    ? cfg["anomaly"]["threshold"].as<float>()
                                    : 0.85f;

    std::cout << "[INFO] SDV Edge Gateway starting — UIN: " << uin << "\n";

    // ── 2. MQTT client — shared by TelemetryPublisher, CommandHandler, AnomalyDetector
    //       One connection, one persistent session — same pattern as a real TCU
    //       where DataCollect and RemoteServices share one MQTT bridge. ─────────
    mqtt::async_client mqtt_client(broker, client_id);

    mqtt::ssl_options ssl_opts;
    ssl_opts.set_trust_store(ca_cert);
    ssl_opts.set_key_store(device_cert);
    ssl_opts.set_private_key(device_key);
    ssl_opts.set_verify(true);   // reject self-signed / expired certs (TARA TR-03)

    mqtt::connect_options conn_opts;
    conn_opts.set_ssl(ssl_opts);
    conn_opts.set_clean_session(false);          // persistent session — broker queues for us offline
    conn_opts.set_keep_alive_interval(keep_alive);
    conn_opts.set_automatic_reconnect(true);     // Paho reconnects on drop, no manual retry loop

    std::cout << "[INFO] Connecting to MQTT broker: " << broker << "\n";
    try {
        mqtt_client.connect(conn_opts)->wait();
        std::cout << "[INFO] MQTT connected\n";
    } catch (const mqtt::exception& e) {
        std::cerr << "[FATAL] MQTT connect failed: " << e.what() << "\n";
        return 1;
    }

    // ── 3. Queues — owned here, passed by reference to each module
    //       SafeQueue is single-consumer, so TelemetryPublisher and AnomalyDetector
    //       each get their own SafeQueue<DecodedSignal>. The decode loop below
    //       fan-outs each decoded signal into both queues. ─────────────────────
    SafeQueue<CanFrame>      can_frame_q;    // CanReader → decode loop
    SafeQueue<DecodedSignal> telemetry_q;   // decode loop → TelemetryPublisher
    SafeQueue<DecodedSignal> anomaly_q;     // decode loop → AnomalyDetector
    SafeQueue<Command>       ota_q;         // CommandHandler → OtaAgent

    // ── 4. Signal decoder — stateless utility, load DBC once ──────────────────
    SignalDecoder decoder;
    if (!decoder.load(dbc_path, signal_names)) {
        std::cerr << "[FATAL] Failed to load DBC: " << dbc_path << "\n";
        return 1;
    }
    std::cout << "[INFO] DBC loaded: " << dbc_path << "\n";

    // ── 5. Create modules ─────────────────────────────────────────────────────
    CanReader          can_reader(can_frame_q, can_iface_rx);
    CanWriter          can_writer(can_iface_tx);
    TelemetryPublisher telemetry_pub(telemetry_q, uin, mqtt_client);
    CommandHandler     cmd_handler(mqtt_client, ota_q, can_writer, uin);
    OtaAgent           ota_agent(ota_q, mqtt_client, uin);
    AnomalyDetector    anomaly_det(anomaly_q, mqtt_client, uin, model_path, anomaly_threshold);

    // ── 6. Start modules — order matters:
    //       CanWriter before CommandHandler (handler calls writer)
    //       MQTT must be connected (step 2) before any publish/subscribe ────────
    if (!can_writer.start()) {
        std::cerr << "[FATAL] CanWriter failed to start on " << can_iface_tx << "\n";
        return 1;
    }
    if (!can_reader.start()) {
        std::cerr << "[FATAL] CanReader failed to start on " << can_iface_rx << "\n";
        return 1;
    }
    if (!telemetry_pub.start()) {
        std::cerr << "[FATAL] TelemetryPublisher failed to start\n";
        return 1;
    }
    if (!cmd_handler.start()) {
        std::cerr << "[FATAL] CommandHandler failed to start\n";
        return 1;
    }
    if (!ota_agent.start()) {
        std::cerr << "[FATAL] OtaAgent failed to start\n";
        return 1;
    }
    if (!anomaly_det.start()) {
        // Non-fatal — gateway runs without anomaly detection if model is missing
        std::cerr << "[WARN]  AnomalyDetector failed to start (model missing?). "
                     "Continuing without anomaly detection.\n";
    }

    std::cout << "[INFO] All modules started. Press Ctrl+C to stop.\n";

    // ── 7. Signal handler — Ctrl+C or systemd SIGTERM sets g_running = false ──
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── 8. Decode loop — this thread is the decode stage ─────────────────────
    //       Pops raw CanFrame, decodes to signals via DBC, fan-outs to both
    //       telemetry_q and anomaly_q.
    //       pop_for(100ms): short timeout so we respond to g_running=false within
    //       100ms rather than blocking indefinitely. ────────────────────────────
    while (g_running) {
        auto frame = can_frame_q.pop_for(std::chrono::milliseconds(100));
        if (!frame) continue;   // timeout — loop back and check g_running

        const auto decoded = decoder.decode(*frame);

        for (const auto& sig : decoded) {
            telemetry_q.push(sig);   // TelemetryPublisher consumer
            anomaly_q.push(sig);     // AnomalyDetector consumer
        }
    }

    std::cout << "\n[INFO] Shutdown signal received — stopping modules...\n";

    // ── 9. Graceful shutdown — reverse dependency order ───────────────────────
    //       Consumers first (they pop from queues)
    //       Producers last  (CanReader pushes into can_frame_q)
    anomaly_det.stop();
    ota_agent.stop();
    cmd_handler.stop();
    telemetry_pub.stop();
    can_reader.stop();   // stop producer after all consumers are done
    can_writer.stop();

    std::cout << "[INFO] Disconnecting MQTT...\n";
    try {
        mqtt_client.disconnect()->wait();
    } catch (const mqtt::exception& e) {
        std::cerr << "[WARN] MQTT disconnect: " << e.what() << "\n";
    }

    std::cout << "[INFO] Gateway stopped cleanly.\n";
    return 0;
}
