#pragma once
// debug_log.hpp — TEMPORARY thread-safe console output for the TEMPORARY
// std::cerr debug prints scattered across CanReader / TelemetryPublisher /
// AnomalyDetector (added 12-Jul-26 while DLT delivery is unverified).
//
// Without this, concurrent threads writing raw chained `<<` calls to
// std::cerr/std::cout interleave mid-line (each `<<` is a separate call, not
// an atomic unit) — e.g. one thread's "elapsed=3s" landing as "elapsed= degC"
// because another thread's write sliced in between. Build the full line into
// one string first, then call debug_log() once — this mutex is the only thing
// making that single write atomic *across* threads.
//
// Delete this header and all call sites once DLT delivery is confirmed
// working end-to-end and these TEMPORARY prints are removed.

#include <iostream>
#include <mutex>
#include <string>

inline std::mutex& debug_log_mutex()
{
    static std::mutex m;
    return m;
}

inline void debug_log(const std::string& line)
{
    std::lock_guard<std::mutex> lock(debug_log_mutex());
    std::cerr << line;
}
