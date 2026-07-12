#include "gateway/offline_buffer.hpp"
#include "common/dlt_wrapper.hpp"
#include <sqlite3.h>
#include <iostream>                              // std::cerr for error logging (Mac only)
#include <rapidjson/writer.h>                   // Writer — streaming JSON serializer
#include <rapidjson/stringbuffer.h>             // StringBuffer — char buffer Writer writes into
#include <rapidjson/document.h>                 // Document — DOM parser for from_json
#include <chrono>                               // timestamp conversion

DLT_DECLARE_CONTEXT(ctx_offline_buffer);

// ─── Constructor / Destructor ─────────────────────────────────────────────────

OfflineBuffer::OfflineBuffer(const std::string& db_path, std::size_t max_rows)
    : db_path_(db_path)     // store path — SQLite file opened later in open()
    , max_rows_(max_rows)   // capacity ceiling — oldest row evicted when full
    , db_(nullptr)          // not open yet
{}

OfflineBuffer::~OfflineBuffer() {
    close();    // RAII — if caller forgot to call close(), destructor handles it
}

// ─── open() / close() ────────────────────────────────────────────────────────

bool OfflineBuffer::open() {
    DLT_REGISTER_CONTEXT(ctx_offline_buffer, "OFBF", "OfflineBuffer logs");

    // Step 1 — open (or create) the SQLite DB file at db_path_
    // SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE: create file if it doesn't exist
    int rc = sqlite3_open_v2(
        db_path_.c_str(),
        &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        nullptr   // use default VFS
    );

    if (rc != SQLITE_OK) {
        DLT_LOG(ctx_offline_buffer, DLT_LOG_ERROR,
                DLT_STRING("open() failed:"), DLT_STRING(sqlite3_errmsg(db_)));
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // Step 2 — set PRAGMAs
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;",    nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;",  nullptr, nullptr, nullptr);

    // Step 3 — create schema (idempotent — IF NOT EXISTS)
    return create_schema();
}

void OfflineBuffer::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

namespace {

// to_json — serializes TelemetryMsg directly into a char buffer via rapidjson Writer.
// No intermediate tree built. Writer writes field by field into StringBuffer.
std::string to_json(const TelemetryMsg& msg) {
    // Convert timestamp to Unix milliseconds — SQLite stores as INTEGER
    int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        msg.timestamp.time_since_epoch()).count();

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);

    writer.StartObject();
    writer.Key("uin");      writer.String(msg.uin.c_str());
    writer.Key("speed");    writer.Double(msg.vehicle_speed_kmh);
    writer.Key("rpm");      writer.Double(msg.engine_rpm);
    writer.Key("coolant");  writer.Double(msg.engine_coolant_temp_c);
    writer.Key("batt");     writer.Double(msg.battery_voltage_v);
    writer.Key("gear");     writer.String(msg.gear.c_str());
    writer.Key("ts_epoch"); writer.Int64(ts_ms);
    writer.EndObject();

    return sb.GetString();  // copies buffer into std::string
}

// from_json — parses JSON payload back into TelemetryMsg during drain().
// Uses DOM style — builds a tree so we can look up fields by name.
bool from_json(const char* payload, TelemetryMsg& msg) {
    rapidjson::Document doc;
    doc.Parse(payload);

    // Parse error check — malformed JSON in DB should not crash the process
    if (doc.HasParseError()) {
        return false;
    }

    // Validate all expected fields exist with correct types before reading
    if (!doc.HasMember("uin")     || !doc["uin"].IsString())  return false;
    if (!doc.HasMember("speed")   || !doc["speed"].IsNumber()) return false;
    if (!doc.HasMember("rpm")     || !doc["rpm"].IsNumber())   return false;
    if (!doc.HasMember("coolant") || !doc["coolant"].IsNumber()) return false;
    if (!doc.HasMember("batt")    || !doc["batt"].IsNumber())  return false;
    if (!doc.HasMember("gear")    || !doc["gear"].IsString())  return false;
    if (!doc.HasMember("ts_epoch")|| !doc["ts_epoch"].IsInt64()) return false;

    msg.uin                   = doc["uin"].GetString();
    msg.vehicle_speed_kmh     = doc["speed"].GetDouble();
    msg.engine_rpm            = doc["rpm"].GetDouble();
    msg.engine_coolant_temp_c = doc["coolant"].GetDouble();
    msg.battery_voltage_v     = doc["batt"].GetDouble();
    msg.gear                  = doc["gear"].GetString();

    // Restore timestamp from Unix milliseconds back to time_point
    int64_t ts_ms = doc["ts_epoch"].GetInt64();
    msg.timestamp = std::chrono::system_clock::time_point{
                        std::chrono::milliseconds{ts_ms}};

    return true;
}

} // namespace

// ─── store() ─────────────────────────────────────────────────────────────────

bool OfflineBuffer::store(const TelemetryMsg& msg) {
    if (!db_) return false;

    // Step 1 — enforce capacity ceiling
    // If full, delete oldest unsent row before inserting new one
    if (pending_count() >= max_rows_) {
        sqlite3_exec(db_,
            "DELETE FROM telemetry WHERE id = ("
            "  SELECT id FROM telemetry WHERE sent=0 ORDER BY id ASC LIMIT 1"
            ");",
            nullptr, nullptr, nullptr);
    }

    // Step 2 — build JSON payload from TelemetryMsg
    const std::string payload = to_json(msg);

    // Step 3 — prepared statement INSERT (never sqlite3_exec — payload is external data)
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO telemetry (payload, sent) VALUES (?, 0);";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DLT_LOG(ctx_offline_buffer, DLT_LOG_ERROR,
                DLT_STRING("store() prepare failed:"), DLT_STRING(sqlite3_errmsg(db_)));
        return false;
    }

    // Bind payload — SQLITE_TRANSIENT tells SQLite to copy the string internally
    // Safe when payload goes out of scope after this function returns
    sqlite3_bind_text(stmt, 1, payload.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);// 
    sqlite3_finalize(stmt);   // always finalize — even on error

    if (rc != SQLITE_DONE) {
        DLT_LOG(ctx_offline_buffer, DLT_LOG_ERROR,
                DLT_STRING("store() step failed:"), DLT_STRING(sqlite3_errmsg(db_)));
        return false;
    }

    return true;
}

// ─── drain() ─────────────────────────────────────────────────────────────────

std::vector<std::pair<int64_t, TelemetryMsg>> OfflineBuffer::drain(std::size_t limit) {
    std::vector<std::pair<int64_t, TelemetryMsg>> result;
    if (!db_) return result;

    const char* sql =
        "SELECT id, payload FROM telemetry "
        "WHERE sent = 0 "
        "ORDER BY id ASC "   // oldest first — FIFO replay order
        "LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DLT_LOG(ctx_offline_buffer, DLT_LOG_ERROR,
                DLT_STRING("drain() prepare failed:"), DLT_STRING(sqlite3_errmsg(db_)));
        return result;
    }

    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(limit));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t     row_id  = sqlite3_column_int64(stmt, 0);
        const char* payload = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        TelemetryMsg msg;
        msg.from_buffer = true;   // tells TelemetryPublisher this is replayed data

        if (payload && from_json(payload, msg)) {
            result.emplace_back(row_id, std::move(msg));
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

// ─── mark_sent() ─────────────────────────────────────────────────────────────

bool OfflineBuffer::mark_sent(int64_t row_id) {
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE telemetry SET sent=1 WHERE id=?;";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DLT_LOG(ctx_offline_buffer, DLT_LOG_ERROR,
                DLT_STRING("mark_sent() prepare failed:"), DLT_STRING(sqlite3_errmsg(db_)));
        return false;
    }

    sqlite3_bind_int64(stmt, 1, row_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        DLT_LOG(ctx_offline_buffer, DLT_LOG_ERROR,
                DLT_STRING("mark_sent() step failed:"), DLT_STRING(sqlite3_errmsg(db_)));
        return false;
    }
    return true;
}

// ─── pending_count() ─────────────────────────────────────────────────────────

std::size_t OfflineBuffer::pending_count() const {
    if (!db_) return 0;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM telemetry WHERE sent=0;";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    std::size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<std::size_t>(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return count;
}

// ─── create_schema() ──────────────────────────────────────────────────────────

bool OfflineBuffer::create_schema() {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS telemetry ("
        "  id      INTEGER PRIMARY KEY AUTOINCREMENT,"  // row_id used in mark_sent()
        "  payload TEXT    NOT NULL,"                   // JSON blob — all signals
        "  sent    INTEGER NOT NULL DEFAULT 0"          // 0=pending, 1=ACK'd by broker
        ");"
        // Index on sent — drain() and pending_count() both filter WHERE sent=0
        // Without this, every reconnect does a full table scan
        "CREATE INDEX IF NOT EXISTS idx_sent ON telemetry(sent);";

    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        DLT_LOG(ctx_offline_buffer, DLT_LOG_ERROR,
                DLT_STRING("create_schema() failed:"), DLT_STRING(errmsg));
        sqlite3_free(errmsg);   // errmsg allocated by SQLite — must free
        return false;
    }
    return true;
}
