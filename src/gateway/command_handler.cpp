#include "gateway/command_handler.hpp"
#include "common/dlt_wrapper.hpp"

#include <nlohmann/json.hpp>
#include <unordered_map>

DLT_DECLARE_CONTEXT(ch_ctx);

// ─── Constructor / Destructor ────────────────────────────────────────────────

CommandHandler::CommandHandler(mqtt::async_client& mqtt_client,
                               SafeQueue<Command>& ota_queue,
                               CanWriter& can_writer,
                               const std::string& uin)
    : mqtt_client_(mqtt_client)
    , ota_queue_(ota_queue)
    , can_writer_(can_writer)
    , uin_(uin)
    , topic_("sdv/commands/to/uin/" + uin)                      // remote ops downlink
    , diagnosis_topic_("sdv/Analytics/to/uin/" + uin + "/diagnosis") // LLM diagnosis downlink
{}

CommandHandler::~CommandHandler()
{
    stop();   // safe to call even if never started
}

// ─── Public: start ───────────────────────────────────────────────────────────
// Connection lifecycle lives outside this class — see telemetry_publisher.cpp
// for the identical reasoning. This just verifies the shared client is usable,
// subscribes, and spawns the consumer thread.

bool CommandHandler::start()
{
    DLT_REGISTER_CONTEXT(ch_ctx, "CMDH", "Command Handler - MQTT downlink");

    if (!mqtt_client_.is_connected()) {
        DLT_LOG(ch_ctx, DLT_LOG_ERROR,
                DLT_STRING("CommandHandler::start — MQTT client not connected"));
        return false;
    }

    try {
        // Subscribe to BOTH downlink topics.
        // Paho subscribes are independent calls; either can throw independently.
        mqtt_client_.subscribe(topic_,           QOS)->wait();
        mqtt_client_.subscribe(diagnosis_topic_, QOS)->wait();
    } catch (const mqtt::exception& e) {
        DLT_LOG(ch_ctx, DLT_LOG_ERROR,
                DLT_STRING("Subscribe failed:"), DLT_STRING(e.what()));
        return false;
    }

    // running_ = true BEFORE thread spawn — same ordering reason as
    // TelemetryPublisher: if reversed, the thread could see running_=false
    // and exit immediately before ever consuming a message.
    running_ = true;
    thread_  = std::thread(&CommandHandler::run, this);

    DLT_LOG(ch_ctx, DLT_LOG_INFO,
            DLT_STRING("CommandHandler subscribed to"),
            DLT_STRING(topic_.c_str()),
            DLT_STRING("and"),
            DLT_STRING(diagnosis_topic_.c_str()));
    return true;
}

// ─── Public: stop ────────────────────────────────────────────────────────────

void CommandHandler::stop()
{
    running_ = false;

    if (thread_.joinable()) {
        thread_.join();   // wait — thread may be mid-wait in try_consume_message_for(1s)
    }

    try {
        mqtt_client_.unsubscribe(topic_)->wait();
        mqtt_client_.unsubscribe(diagnosis_topic_)->wait();
    } catch (const mqtt::exception& e) {
        DLT_LOG(ch_ctx, DLT_LOG_WARN,
                DLT_STRING("Unsubscribe error:"), DLT_STRING(e.what()));
    }

    // NOTE: does NOT disconnect mqtt_client_ — shared with TelemetryPublisher.
}

// ─── Private: run ────────────────────────────────────────────────────────────
// Paho's async_client has a built-in consumer queue: start_consuming() turns
// it on, try_consume_message_for() blocks up to a timeout waiting for the next
// message (returns false on timeout, mirroring SafeQueue::pop_for's pattern
// used elsewhere in this codebase), stop_consuming() turns it back off.

void CommandHandler::run()
{
    mqtt_client_.start_consuming();

    while (running_) {
        mqtt::const_message_ptr msg;

        if (!mqtt_client_.try_consume_message_for(&msg, CONSUME_TIMEOUT)) {
            continue;   // timeout — no message arrived, loop back and check running_
        }
        if (!msg) {
            continue;   // defensive — shouldn't happen if try_consume_message_for returned true
        }

        // Route by topic — two downlink channels share the same consumer loop.
        if (msg->get_topic() == diagnosis_topic_) {
            // Diagnosis from diagnostic_agent Lambda: log and display to driver.
            // Does NOT go through dedup guard — diagnoses are informational, not
            // actuating, so redelivery just logs the same message twice (harmless).
            handle_diagnosis(msg->get_payload_str());
            continue;
        }

        // Remote ops command path — parse + dedup + route.
        Command cmd;
        if (!parse_command(msg->get_payload_str(), cmd)) {
            DLT_LOG(ch_ctx, DLT_LOG_ERROR,
                    DLT_STRING("Malformed command JSON, dropped"));
            continue;
        }

        if (already_seen(cmd.correlation_id)) {
            DLT_LOG(ch_ctx, DLT_LOG_WARN,
                    DLT_STRING("Duplicate command (QoS1 redelivery), ignored:"),
                    DLT_STRING(cmd.correlation_id.c_str()));
            continue;
        }

        route(cmd);
    }

    mqtt_client_.stop_consuming();
}

// ─── Private: parse_command ──────────────────────────────────────────────────
// Expected wire format:
//   {"type": "OTA_TRIGGER", "correlation_id": "abc-123", "payload": {...}}
// "type" is required; correlation_id/payload default to empty if absent
// (nlohmann's j.value() takes a default instead of throwing, unlike j.at()).

bool CommandHandler::parse_command(const std::string& payload, Command& out)
{
    static const std::unordered_map<std::string, CommandType> type_map = {
        {"DOOR_LOCK",    CommandType::DOOR_LOCK},
        {"DOOR_UNLOCK",  CommandType::DOOR_UNLOCK},
        {"ENGINE_START", CommandType::ENGINE_START},
        {"HORN_LIGHT",   CommandType::HORN_LIGHT},
        {"OTA_TRIGGER",  CommandType::OTA_TRIGGER},
    };

    try {
        auto j = nlohmann::json::parse(payload);

        std::string type_str = j.at("type").get<std::string>();  // required — throws if absent
        auto it = type_map.find(type_str);

        Command cmd;
        cmd.type           = (it != type_map.end()) ? it->second : CommandType::UNKNOWN;
        cmd.correlation_id = j.value("correlation_id", std::string(""));
        cmd.payload_json   = j.value("payload", nlohmann::json::object()).dump();

        out = cmd;
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;   // malformed JSON or missing required "type" field
    }
}

// ─── Private: already_seen ────────────────────────────────────────────────────

bool CommandHandler::already_seen(const std::string& correlation_id)
{
    std::lock_guard<std::mutex> lock(seen_mutex_);

    if (seen_ids_.count(correlation_id)) {
        return true;
    }

    if (seen_ids_.size() >= MAX_SEEN_IDS) {
        seen_ids_.clear();   // simple cap — see header comment for the trade-off
    }
    seen_ids_.insert(correlation_id);
    return false;
}

// ─── Private: route ───────────────────────────────────────────────────────────

void CommandHandler::route(const Command& cmd)
{
    switch (cmd.type) {
        case CommandType::OTA_TRIGGER:
            ota_queue_.push(cmd);
            DLT_LOG(ch_ctx, DLT_LOG_INFO,
                    DLT_STRING("Routed OTA_TRIGGER to OtaAgent queue"));
            break;

        case CommandType::DOOR_LOCK:
        case CommandType::DOOR_UNLOCK:
        case CommandType::ENGINE_START:
        case CommandType::HORN_LIGHT:
            if (!can_writer_.write(cmd.type, cmd.payload_json)) {
                DLT_LOG(ch_ctx, DLT_LOG_ERROR,
                        DLT_STRING("CanWriter::write failed for correlation_id:"),
                        DLT_STRING(cmd.correlation_id.c_str()));
            }
            break;

        case CommandType::UNKNOWN:
        default:
            DLT_LOG(ch_ctx, DLT_LOG_WARN,
                    DLT_STRING("Unknown command type, dropped"));
            break;
    }
}

// ─── Private: handle_diagnosis ────────────────────────────────────────────────
// Receives LLM diagnosis from diagnostic_agent Lambda (cloud→vehicle downlink).
// This CLOSES the full loop:
//   AnomalyDetector → MQTT → IoT Core → Lambda → RAG+LLM → MQTT → here
//
// Payload from diagnostic_agent.py:
//   {
//     "root_cause":        "Engine over-temperature — thermal runaway risk",
//     "recommended_action":"Reduce engine load, pull over safely if sustained",
//     "driver_message":    "Engine too hot. Reduce speed and pull over soon.",
//     "dtc":               "P0217",
//     "asil":              "ASIL-B",
//     "escalate_human":    false,
//     "timestamp":         "2026-07-10T12:34:56"
//   }
//
// In production (AUTOSAR Adaptive): route to ara::com service that drives the
// instrument cluster DTC display. Here: DLT_LOG for Pi demo.

void CommandHandler::handle_diagnosis(const std::string& payload)
{
    try {
        auto j = nlohmann::json::parse(payload);

        std::string driver_msg = j.value("driver_message", std::string(""));
        std::string dtc        = j.value("dtc",            std::string(""));
        std::string asil       = j.value("asil",           std::string(""));
        std::string root_cause = j.value("root_cause",     std::string(""));

        // DLT_LOG_WARN so the diagnosis stands out from routine INFO telemetry.
        DLT_LOG(ch_ctx, DLT_LOG_WARN,
                DLT_STRING("[DIAGNOSIS]"),
                DLT_STRING("DTC:"),       DLT_STRING(dtc.c_str()),
                DLT_STRING("ASIL:"),      DLT_STRING(asil.c_str()),
                DLT_STRING("Driver:"),    DLT_STRING(driver_msg.c_str()),
                DLT_STRING("RootCause:"), DLT_STRING(root_cause.c_str()));

    } catch (const nlohmann::json::exception& e) {
        DLT_LOG(ch_ctx, DLT_LOG_ERROR,
                DLT_STRING("handle_diagnosis — bad JSON:"), DLT_STRING(e.what()));
    }
}
