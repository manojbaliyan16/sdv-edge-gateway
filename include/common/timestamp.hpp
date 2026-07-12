#pragma once
// timestamp.hpp — shared ISO8601 UTC string formatter.
//
// sdv_telemetry and sdv_anomalies (DynamoDB) both declare `timestamp` as a
// String (S) sort key (see cloud/iot_setup/create_infrastructure.py). Any
// PutItem whose `timestamp` field isn't a JSON string — or is missing
// entirely — fails inside the IoT Rule engine with no signal on the device
// side and no row in the table: the console just shows "0 items". Both
// publishers must go through this helper rather than hand-rolling their own
// timestamp field, which is exactly how this drifted apart in the first
// place (telemetry_publisher sent a raw epoch-millis number, anomaly_detector
// sent no timestamp field at all).

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

inline std::string iso8601_utc(std::chrono::system_clock::time_point tp)
{
    using namespace std::chrono;

    auto ms = duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(tp);
    std::tm tm_utc;
    gmtime_r(&t, &tm_utc);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                  tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec,
                  static_cast<int>(ms.count()));
    return std::string(buf);
}
