#include "gateway/telemetry_publisher.hpp"
#include "common/dlt_wrapper.hpp"
#include "common/timestamp.hpp"

#include <nlohmann/json.hpp>   // JSON serialisation for TelemetryMsg
#include <sstream>             // std::ostringstream — build full lines before one atomic-ish write
#include "common/debug_log.hpp"  // mutex-protected std::cerr — see header for why

DLT_DECLARE_CONTEXT(tp_ctx);

// ─── Helper: GEAR_SHIFT raw code → label ──────────────────────────────────────
// Matches the DBC encoding in dbc/toyota_corolla.dbc: SG_ GEAR_SHIFT (0=P,1=R,2=N,3=D)

static std::string gear_code_to_string(double raw)
{
    switch (static_cast<int>(raw)) {
        case 0:  return "P";
        case 1:  return "R";
        case 2:  return "N";
        case 3:  return "D";
        default: return "unknown";
    }
}

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
            // GEAR_SHIFT decodes as a numeric DBC signal (0=P,1=R,2=N,3=D,4=unknown)
            // like every other signal — DecodedSignal has no label field, so the
            // raw code is mapped to a human-readable string here, not upstream.
            if (result->name == "GEAR_SHIFT") {
                gear_          = gear_code_to_string(result->value);
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

        // TEMPORARY 12-Jul-26: direct visibility into the publish decision.
        // Build the full line in one string first, then a SINGLE cerr write —
        // three threads (CanReader, main's decode loop, this one) all write to
        // std::cerr concurrently; chained << calls are NOT atomic and were
        // getting sliced mid-line by other threads' output, producing garbled,
        // untrustworthy readings (e.g. "elapsed= degC" instead of a number).
        {
            std::ostringstream dbg;
            dbg << "[DEBUG] TelemetryPublisher::run — numeric_.size()=" << numeric_.size()
                << " gear_received_=" << gear_received_
                << " all_signals=" << all_signals
                << " elapsed=" << elapsed << "s"
                << " timer_fired=" << timer_fired << "\n";
            debug_log(dbg.str());
        }

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
    // Keys here MUST match the exact signal names SignalDecoder emits, which
    // are the DBC's own SG_ names verbatim (e.g. "VEHICLE_SPEED", all-caps) —
    // NOT the snake_case TelemetryMsg/JSON field names. This was previously
    // looking up "vehicle_speed_kmh" etc., which never matched anything
    // numeric_ actually contained, so every field silently published as 0.0
    // (count()-before-at() made it defensive, not crashing, just wrong).
    msg.vehicle_speed_kmh      = numeric_.count("VEHICLE_SPEED")
                                     ? numeric_.at("VEHICLE_SPEED") : 0.0;
    msg.engine_rpm             = numeric_.count("ENGINE_RPM")
                                     ? numeric_.at("ENGINE_RPM")         : 0.0;
    msg.engine_coolant_temp_c  = numeric_.count("ENGINE_COOLANT_TEMP")
                                     ? numeric_.at("ENGINE_COOLANT_TEMP") : 0.0;
    msg.battery_voltage_v      = numeric_.count("BATTERY_VOLTAGE")
                                     ? numeric_.at("BATTERY_VOLTAGE")  : 0.0;
    msg.gear                   = gear_received_ ? gear_ : "unknown";
    msg.from_buffer            = false;

    // ── Serialise to JSON ────────────────────────────────────────────────────
    nlohmann::json j;
    j["uin"]                   = msg.uin;
    // sdv_telemetry's DynamoDB sort key "timestamp" is type String (S) — this
    // MUST be a JSON string, not epoch-millis as a number, or every PutItem
    // fails a silent type-mismatch inside the IoT Rule engine (0 rows ever
    // appear, nothing logged on the device side). See common/timestamp.hpp.
    j["timestamp"]             = iso8601_utc(msg.timestamp);
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
        // TEMPORARY 12-Jul-26: debug_log fallback, same reason as CanReader/main.
        {
            std::ostringstream dbg;
            dbg << "[INFO] TelemetryPublisher: published to " << topic
                << " — speed=" << msg.vehicle_speed_kmh
                << " rpm=" << msg.engine_rpm
                << " coolant=" << msg.engine_coolant_temp_c
                << " battery=" << msg.battery_voltage_v << "\n";
            debug_log(dbg.str());
        }
    } catch (const mqtt::exception& e) {
        DLT_LOG(tp_ctx, DLT_LOG_ERROR,
                DLT_STRING("MQTT publish failed:"), DLT_STRING(e.what()));
        std::ostringstream dbg;
        dbg << "[ERROR] TelemetryPublisher: MQTT publish failed: " << e.what() << "\n";
        debug_log(dbg.str());
        // TODO: push to offline_buffer_ when MQTT disconnected
    }

    // ── Reset accumulators and timer ─────────────────────────────────────────
    numeric_.clear();
    gear_.clear();
    gear_received_ = false;
    last_publish_  = std::chrono::steady_clock::now();
}
