#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <optional>

#include <onnxruntime_cxx_api.h>   // Ort::Env, Ort::Session — ONNX Runtime C++ API
#include <mqtt/async_client.h>

#include "common/types.hpp"        // DecodedSignal
#include "common/safe_queue.hpp"   // SafeQueue<T>
#include "common/dlt_wrapper.hpp"

// ─── AnomalyDetector ──────────────────────────────────────────────────────────
// Reads DecodedSignal telemetry from signal_queue_, runs each signal through a
// pre-trained INT8 ONNX model (P1.2 + P1.3 integration), and publishes an alert
// to "alerts/<uin>/anomaly" when the model's anomaly score exceeds the threshold.
//
// Thread model: one background thread (run()) drains signal_queue_ via pop_for(1s)
// so stop() wakes within 1 second cleanly.
//
// ONNX Runtime notes:
//   Ort::Env   — global runtime state; one per process, created once in start()
//   Ort::Session — loaded model; holds the inference engine for the .onnx file
//   session_ is std::optional<> because Ort::Session has no default constructor —
//   we initialise it inside start() so the constructor cannot throw on bad paths.

class AnomalyDetector {
public:
    explicit AnomalyDetector(SafeQueue<DecodedSignal>& signal_queue,
                             mqtt::async_client&        mqtt_client,
                             const std::string&         uin,
                             const std::string&         model_path,
                             float                      anomaly_threshold = 0.85f);
    ~AnomalyDetector();

    bool start();   // loads ONNX model, spawns thread — returns false if model not found
    void stop();

private:
    void  run();
    float run_inference(const DecodedSignal& signal);   // returns score in [0.0, 1.0]
    void  publish_alert(const DecodedSignal& signal, float score);

    // ── Injected dependencies ─────────────────────────────────────────────────
    SafeQueue<DecodedSignal>&  signal_queue_;   // shared with TelemetryPublisher fan-out
    mqtt::async_client&        mqtt_client_;
    std::string                uin_;            // topic: alerts/<uin>/anomaly

    // ── Config ────────────────────────────────────────────────────────────────
    std::string                model_path_;         // path to .onnx file on Pi
    float                      anomaly_threshold_;  // score > this → alert fired

    // ── ONNX Runtime ──────────────────────────────────────────────────────────
    Ort::Env                    env_;       // must be declared BEFORE session_
    std::optional<Ort::Session> session_;   // emplace()'d in start()

    // ── Thread control ────────────────────────────────────────────────────────
    std::thread      thread_;
    std::atomic<bool> running_{false};

    static constexpr int QOS = 1;   // at-least-once delivery for alerts
};
