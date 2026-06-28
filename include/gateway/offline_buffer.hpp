#pragma once

// Forward declare sqlite3 — keeps sqlite3.h out of this header.
// TelemetryPublisher includes offline_buffer.hpp and should not need to know
// that SQLite is used internally.
struct sqlite3;

#include <string>
#include <vector>
#include <utility>        // std::pair
#include <cstdint>        // int64_t
#include <cstddef>        // std::size_t
#include "common/types.hpp"

class OfflineBuffer {
public:
    explicit OfflineBuffer(const std::string& db_path, std::size_t max_rows = 10000);
    ~OfflineBuffer();

    bool   open();
    void   close();

    bool                       store(const TelemetryMsg& msg);
    std::vector<std::pair<int64_t, TelemetryMsg>>  drain(std::size_t limit = 100);
    bool                       mark_sent(int64_t row_id);
    std::size_t                pending_count() const;

private:
    bool create_schema();

    std::string  db_path_;
    std::size_t  max_rows_;
    sqlite3*     db_{nullptr};
};

