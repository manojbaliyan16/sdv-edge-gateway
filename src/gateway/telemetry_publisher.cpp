#include "gateway/telemetry_publisher.hpp"
#include "common/dlt_wrapper.hpp"

#include <nlohmann/json.hpp>   // JSON serialisation for TelemetryMsg

DLT_DECLARE_CONTEXT(tp_ctx);

// ─── Constructor ─────────────────────────────────────────────────────────────
// mqtt_client_ has no default constructor — must be in initializer list
// last_publish_ set to now() so first forced publish fires after PUBLISH_INTERVAL_S

TelemetryPublisher::TelemetryPublisher(SafeQueue<DecodedSignal>& queue,
                                       const std::string& uin,
                                       mqtt::async_client& mqtt_client)
    : queue_(queue)
    , uin_(uin)
    , mqtt_client_(mqtt_client)                        // bind reference to the shared client
    , last_publish_(std::chrono::steady_clock::now())  // start the 5s clock now
{}

// ─── Destructor ──────────────────────────────────────────────────────────────

TelemetryPublisher::~TelemetryPublisher()
{
    stop();   // safe to call even if never started
}

// ─── start ───────────────────────────────────────────────────────────────────
// 1. Connect to AWS IoT Core with persistent session (cleanSession=false)
// 2. Spawn publisher thread
// Returns false if broker connection fails — caller should abort gateway startup

bool TelemetryPublisher::start()
{
    DLT_REGISTER_CONTEXT(tp_ctx, "TELP", "Telemetry Publisher - MQTT uplink");

    // Connection lifecycle (connect/disconnect) now lives OUTSIDE this class —
    // main.cpp connects the shared mqtt_client_ ONCE, before starting both
    // TelemetryPublisher and CommandHandler on top of it (see command_handler.hpp).
    // This class just verifies the client is usable and starts publishing.
    if (!mqtt_client_.is_connected()) {
        DLT_LOG(tp_ctx, DLT_LOG_ERROR,
                DLT_STRING("TelemetryPublisher::start — MQTT client not connected"));
        return false;
    }

    // running_ = true BEFORE thread spawn
    // If reversed: thread enters run(), sees running_=false, exits immediately
    running_ = true;
    thread_  = std::thread(&TelemetryPublisher::run, this);

    DLT_LOG(tp_ctx, DLT_LOG_INFO,
            DLT_STRING("TelemetryPublisher started"));
    return true;
}

// ─── stop ────────────────────────────────────────────────────────────────────
// 1. Signal thread to exit (running_ = false)
// 2. Wait for thread to finish its current pop_for() and exit (join)
// 3. Disconnect from broker

void TelemetryPublisher::stop()
{
    running_ = false;

    if (thread_.joinable()) {
        thread_.join();   // wait — thread may be mid-sleep in pop_for(1s)
    }

    // NOTE: does NOT disconnect mqtt_client_ — it's shared with CommandHandler.
    // Disconnect happens exactly once, wherever main.cpp owns the client.

    DLT_LOG(tp_ctx, DLT_LOG_INFO, DLT_STRING("TelemetryPublisher stopped"));
}

// ─── run ─────────────────────────────────────────────────────────────────────
// Runs on thread_ — the main accumulation + publish loop
//
// Two triggers to publish:
//   1. All 5 signals collected (numeric_.size()==4 && gear_received_)
//   2. PUBLISH_INTERVAL_S elapsed since last publish (ECU fault / missing signal)
//
// pop_for(1s) — blocks for 1 second waiting for a DecodedSignal
//   → if signal arrives: accumulate it
//   → if timeout (EAGAIN): check timer, publish if interval elapsed

void TelemetryPublisher::run()
{
    while (running_) {

        // ── Try to pop a decoded signal (blocks up to 1 second) ──────────────
        // pop_for returns std::optional<DecodedSignal> — empty optional on timeout
        auto result = queue_.pop_for(QUEUE_TIMEOUT);

        if (result) {
            // ── Accumulate into the right bucket ─────────────────────────────
            if (result->name == "gear") {
                gear_          = result->unit;   // gear stored as string in unit field
                gear_received_ = true;
            } else {
                numeric_[result->name] = result->value;
            }
        }

        // ── Check publish conditions ──────────────────────────────────────────
        bool all_signals = (numeric_.size() == 4 && gear_received_);

        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                           now - last_publish_).count();
        bool timer_fired = (elapsed >= PUBLISH_INTERVAL_S);

        // Publish if all signals collected OR timer expired (and we have something)
        if (all_signals || (timer_fired && (!numeric_.empty() || gear_received_))) {
            publish_now();
        }
    }
}

// ─── publish_now ─────────────────────────────────────────────────────────────
// Builds TelemetryMsg from accumulators, serialises to JSON, publishes to MQTT
// Resets accumulators and restarts the 5s timer

void TelemetryPublisher::publish_now()
{
    // ── Build TelemetryMsg ───────────────────────────────────────────────────
    TelemetryMsg msg;
    msg.uin                    = uin_;
    msg.timestamp              = std::chrono::system_clock::now();
    msg.vehicle_speed_kmh      = numeric_.count("vehicle_speed_kmh")
                                     ? numeric_.at("vehicle_speed_kmh") : 0.0;
    msg.engine_rpm             = numeric_.count("engine_rpm")
                                     ? numeric_.at("engine_rpm")         : 0.0;
    msg.engine_coolant_temp_c  = numeric_.count("engine_coolant_temp_c")
                                     ? numeric_.at("engine_coolant_temp_c") : 0.0;
    msg.battery_voltage_v      = numeric_.count("battery_voltage_v")
                                     ? numeric_.at("battery_voltage_v")  : 0.0;
    msg.gear                   = gear_received_ ? gear_ : "unknown";
    msg.from_buffer            = false;

    // ── Serialise to JSON ────────────────────────────────────────────────────
    nlohmann::json j;
    j["uin"]                   = msg.uin;
    j["timestamp"]             = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     msg.timestamp.time_since_epoch()).count();
    j["vehicle_speed_kmh"]     = msg.vehicle_speed_kmh;
    j["engine_rpm"]            = msg.engine_rpm;
    j["engine_coolant_temp_c"] = msg.engine_coolant_temp_c;
    j["battery_voltage_v"]     = msg.battery_voltage_v;
    j["gear"]                  = msg.gear;
    j["from_buffer"]           = msg.from_buffer;

    std::string payload = j.dump();

    // ── Publish to MQTT ──────────────────────────────────────────────────────
    // Topic: telemetry/<uin>/data  (vehicle→cloud direction)
    // QoS 2: exactly-once — no duplicate telemetry records in DynamoDB
    // Must match the IoT Rule's SQL topic filter in create_infrastructure.py
    // ("FROM 'sdv/telemetry/from/uin/+'") and config.yaml's topic_telemetry_up —
    // this was hardcoded to a different, unrelated topic ("telemetry/<uin>/data")
    // that matched nothing, so publishes silently never reached DynamoDB. Third
    // instance of this exact bug pattern in this project (see IAM policy fix,
    // command_handler fix) — topic strings keep drifting because they're
    // duplicated across files instead of defined once.
    std::string topic = "sdv/telemetry/from/uin/" + uin_;

    try {
        auto pub_msg = mqtt::make_message(topic, payload, QOS, false);
        mqtt_client_.publish(pub_msg)->wait();

        DLT_LOG(tp_ctx, DLT_LOG_INFO,
                DLT_STRING("Telemetry published, speed:"),
                DLT_STRING(std::to_string(msg.vehicle_speed_kmh).c_str()));
        // TEMPORARY 12-Jul-26: std::cerr fallback, same reason as CanReader/main.
        std::cerr << "[INFO] TelemetryPublisher: published to " << topic
                  << " — speed=" << msg.vehicle_speed_kmh
                  << " rpm=" << msg.engine_rpm
                  << " coolant=" << msg.engine_coolant_temp_c
                  << " battery=" << msg.battery_voltage_v << "\n";
    } catch (const mqtt::exception& e) {
        DLT_LOG(tp_ctx, DLT_LOG_ERROR,
                DLT_STRING("MQTT publish failed:"), DLT_STRING(e.what()));
        std::cerr << "[ERROR] TelemetryPublisher: MQTT publish failed: " << e.what() << "\n";
        // TODO: push to offline_buffer_ when MQTT disconnected
    }

    // ── Reset accumulators and timer ─────────────────────────────────────────
    numeric_.clear();
    gear_.clear();
    gear_received_ = false;
    last_publish_  = std::chrono::steady_clock::now();
}
