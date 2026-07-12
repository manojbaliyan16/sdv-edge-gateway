#include "gateway/anomaly_detector.hpp"
#include "common/dlt_wrapper.hpp"
#include "common/timestamp.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include "common/debug_log.hpp"  // mutex-protected std::cerr — see header for why

DLT_DECLARE_CONTEXT(anomaly_ctx);

// ─── Constructor / Destructor ─────────────────────────────────────────────────

AnomalyDetector::AnomalyDetector(SafeQueue<DecodedSignal>& signal_queue,
                                 mqtt::async_client&        mqtt_client,
                                 const std::string&         uin,
                                 const std::string&         model_path,
                                 float                      anomaly_threshold)
    : signal_queue_(signal_queue)
    , mqtt_client_(mqtt_client)
    , uin_(uin)
    , model_path_(model_path)
    , anomaly_threshold_(anomaly_threshold)
    , env_(ORT_LOGGING_LEVEL_WARNING, "sdv_anomaly")
    // session_ intentionally left empty — loaded in start()
{}

AnomalyDetector::~AnomalyDetector()
{
    stop();
}

// ─── start ────────────────────────────────────────────────────────────────────
// Loads the ONNX model, then spawns the inference thread.
// Returns false if the model file cannot be opened — caller should not proceed.

bool AnomalyDetector::start()
{
    DLT_REGISTER_CONTEXT(anomaly_ctx, "ANOM", "Anomaly Detector - ONNX inference on CAN signals");

    try {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);   // Pi has 4 cores; 1 thread avoids starving
                                        // CAN reader and MQTT publisher threads
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        session_.emplace(env_, model_path_.c_str(), opts);

        DLT_LOG(anomaly_ctx, DLT_LOG_INFO,
                DLT_STRING("ONNX model loaded:"), DLT_STRING(model_path_.c_str()));
    } catch (const Ort::Exception& e) {
        DLT_LOG(anomaly_ctx, DLT_LOG_ERROR,
                DLT_STRING("Failed to load ONNX model:"), DLT_STRING(e.what()));
        // TEMPORARY 12-Jul-26: DLT delivery to the daemon is still unverified
        // (FIFO /tmp/dlt has been failing to open all session), so the real
        // reason for this failure was invisible. debug_log guarantees we see
        // it regardless of DLT's transport state. Remove once DLT is confirmed
        // working end-to-end and this stops being the only way to see errors.
        debug_log(std::string("[ERROR] AnomalyDetector: ONNX load failed: ") + e.what() + "\n");
        return false;
    }

    running_ = true;
    thread_  = std::thread(&AnomalyDetector::run, this);
    DLT_LOG(anomaly_ctx, DLT_LOG_INFO, DLT_STRING("AnomalyDetector started"));
    return true;
}

// ─── stop ─────────────────────────────────────────────────────────────────────

void AnomalyDetector::stop()
{
    running_ = false;
    if (thread_.joinable()) thread_.join();
    DLT_LOG(anomaly_ctx, DLT_LOG_INFO, DLT_STRING("AnomalyDetector stopped"));
}

// ─── run ─────────────────────────────────────────────────────────────────────
// Drains signal_queue_ via pop_for(1s) — wakes every second to check running_.
// For each signal: runs inference, publishes alert if score > threshold.

void AnomalyDetector::run()
{
    while (running_) {
        auto result = signal_queue_.pop_for(std::chrono::seconds(1));
        if (!result) continue;   // timeout — no signal, loop back

        const DecodedSignal& signal = *result;

        float score = run_inference(signal);

        DLT_LOG(anomaly_ctx, DLT_LOG_VERBOSE,
                DLT_STRING("signal:"), DLT_STRING(signal.name.c_str()),
                DLT_STRING("score:"), DLT_FLOAT32(score));

        if (score > anomaly_threshold_) {
            DLT_LOG(anomaly_ctx, DLT_LOG_WARN,
                    DLT_STRING("ANOMALY detected — score:"), DLT_FLOAT32(score),
                    DLT_STRING("signal:"), DLT_STRING(signal.name.c_str()));
            std::ostringstream dbg;
            dbg << "[DEBUG] AnomalyDetector: ANOMALY detected — signal=" << signal.name
                << " value=" << signal.value << " score=" << score
                << " threshold=" << anomaly_threshold_ << "\n";
            debug_log(dbg.str());
            publish_alert(signal, score);
        }
    }
}

// ─── run_inference ────────────────────────────────────────────────────────────
// Step N: normalize the raw physical value against its own signal's normal band
// Step D: pack the normalized value into a float array
// Step B: create MemoryInfo (tells ONNX "data is on CPU RAM")
// Step E: wrap data as input Ort::Value (tensor)
// Step A: run the model
// Step C: unpack score from output tensor → return
//
// Input shape: [1, 1] — batch=1, features=1 (min-max normalized signal value)
// Output shape: [1, 1] — batch=1, anomaly score in [0.0, 1.0]

float AnomalyDetector::run_inference(const DecodedSignal& signal)
{
    // ── N: Per-signal min-max normalization ───────────────────────────────────
    // Raw physical values live on wildly different scales (RPM ~600-6500 vs
    // battery ~11.5-14.5V). Feeding raw values into one shared model meant the
    // network effectively learned "large absolute number = anomaly", which only
    // worked for RPM by luck and never fired for speed/temp/battery — a real
    // 25V battery-voltage anomaly injection (cansend can0 3B3#FA00000000000000,
    // 12-Jul-26) scored 0.108, far under the 0.85 threshold, despite the value
    // itself being nonsensical for a 12V vehicle battery.
    //
    // Normalizing each value against its own signal's normal band — see
    // SIGNAL_OPERATING_WINDOWS["<signal>"]["normal"] in
    // tools/train_anomaly_model.py, which this table must mirror exactly —
    // puts every signal's normal case near [0,1] and its degree of anomaly in
    // comparable, unit-free terms, so one shared model can generalize across
    // all four signal types.
    static const std::unordered_map<std::string, std::pair<float, float>> kNormalBounds = {
        {"VEHICLE_SPEED",       {0.0f,   140.0f}},
        {"ENGINE_RPM",          {600.0f, 6500.0f}},
        {"ENGINE_COOLANT_TEMP", {60.0f,  105.0f}},
        {"BATTERY_VOLTAGE",     {11.5f,  14.5f}},
    };

    float raw = static_cast<float>(signal.value);
    float normalized = raw;   // unknown signal name — pass raw value through
    auto bounds_it = kNormalBounds.find(signal.name);
    if (bounds_it != kNormalBounds.end()) {
        float lo = bounds_it->second.first;
        float hi = bounds_it->second.second;
        normalized = (raw - lo) / (hi - lo);
    }

    // ── D: Pack normalized value into float array ─────────────────────────────
    // Model input shape: [1, 1] — batch=1, features=1
    std::array<float, 1> input_data = { normalized };
    std::array<int64_t, 2> input_shape = {1, 1};  // batch=1, features=1

    // ── B: MemoryInfo — tells ONNX where input data lives ────────────────────
    // OrtArenaAllocator = use ONNX's internal arena allocator
    // OrtMemTypeDefault = CPU RAM (as opposed to GPU VRAM)
    auto memory_info = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    // ── E: Wrap data as Ort::Value (input tensor) ─────────────────────────────
    // CreateTensor<float>: template param = element type
    // args: memory_info, data pointer, element count, shape pointer, rank
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info,
        input_data.data(),
        input_data.size(),
        input_shape.data(),
        input_shape.size());

    // ── A: Run inference ──────────────────────────────────────────────────────
    // Input/output node names must match what the model was exported with.
    // These are set at export time (tools/train_anomaly_model.py).
    const char* input_names[]  = {"signal_features"};
    const char* output_names[] = {"anomaly_score"};

    auto output_tensors = session_->Run(
        Ort::RunOptions{nullptr},
        input_names,  &input_tensor,  1,   // 1 input — pointer to Ort::Value
        output_names, 1);                   // 1 output

    // ── C: Unpack score from output tensor ────────────────────────────────────
    // GetTensorMutableData<float>() returns float* pointing to output data.
    // [0] = first (and only) element — the anomaly score.
    float* output_data = output_tensors[0].GetTensorMutableData<float>();
    return output_data[0];
}

// ─── publish_alert ────────────────────────────────────────────────────────────
// Publishes anomaly alert JSON to "sdv/Analytics/from/uin/<uin>/anomaly" (QoS 1).
// Topic matches the AWS IoT Rule trigger in create_infrastructure.py which routes
// to diagnostic_agent Lambda (RAG + Bedrock diagnosis + human-in-the-loop for CRITICAL).

void AnomalyDetector::publish_alert(const DecodedSignal& signal, float score)
{
    // Determine severity band — used by diagnostic_agent.py to decide
    // whether to auto-publish diagnosis (WARNING) or escalate to human (CRITICAL).
    std::string severity = (score > 0.95f) ? "CRITICAL" : "WARNING";

    nlohmann::json j;
    j["uin"]            = uin_;                 // required by diagnostic_agent.py
    // sdv_anomalies' DynamoDB sort key "timestamp" is type String (S) and is a
    // REQUIRED key attribute — this payload previously had no "timestamp" field
    // at all, so every PutItem failed inside the IoT Rule engine (missing key
    // element), silently, with 0 rows ever landing. See common/timestamp.hpp.
    j["timestamp"]      = iso8601_utc(std::chrono::system_clock::now());
    j["signal_name"]    = signal.name;
    j["value"]          = signal.value;
    j["unit"]           = signal.unit;
    j["anomaly_score"]  = score;
    j["anomaly_prob"]   = score;                // alias — diagnostic_agent.py uses this key
    j["threshold"]      = anomaly_threshold_;
    j["severity"]       = severity;
    j["vehicle_speed_kmh"] = 0.0;               // populated by future context; 0 = safe assumption

    // Topic aligned with diagnostic_agent.py IoT Rule trigger:
    // sdv/Analytics/from/uin/+/anomaly
    std::string topic = "sdv/Analytics/from/uin/" + uin_ + "/anomaly";

    try {
        auto msg = mqtt::make_message(topic, j.dump(), QOS, false);
        mqtt_client_.publish(msg)->wait();
        // TEMPORARY 12-Jul-26: debug_log fallback, same reason as CanReader/
        // TelemetryPublisher — DLT delivery is unverified, this is the only
        // reliable way to see confirmation of a successful publish.
        std::ostringstream dbg;
        dbg << "[INFO] AnomalyDetector: published to " << topic
            << " — signal=" << signal.name << " value=" << signal.value
            << " score=" << score << " severity=" << severity << "\n";
        debug_log(dbg.str());
    } catch (const mqtt::exception& e) {
        DLT_LOG(anomaly_ctx, DLT_LOG_ERROR,
                DLT_STRING("publish_alert failed:"), DLT_STRING(e.what()));
        debug_log(std::string("[ERROR] AnomalyDetector: MQTT publish failed: ") + e.what() + "\n");
    }
}
