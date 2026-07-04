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
    , topic_("ota/" + uin + "/command")   // built once — reused by subscribe/unsubscribe
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
    if (!mqtt_client_.is_connected()) {
        DLT_LOG(ch_ctx, DLT_LOG_ERROR,
                DLT_STRING("CommandHandler::start — MQTT client not connected"));
        return false;
    }

    try {
        mqtt_client_.subscribe(topic_, QOS)->wait();
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
            DLT_STRING("CommandHandler subscribed to"), DLT_STRING(topic_.c_str()));
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
