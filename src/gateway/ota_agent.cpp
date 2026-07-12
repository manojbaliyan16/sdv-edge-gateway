#include "gateway/ota_agent.hpp"
#include "common/dlt_wrapper.hpp"

#include <nlohmann/json.hpp>

#include <curl/curl.h>          // HTTPS firmware download
#include <openssl/evp.h>        // SHA256 verification

#include <filesystem>           // backup / flash file operations (C++17)
#include <fstream>              // reading file for SHA256
#include <cstdio>               // fopen/fwrite for curl write callback
#include <iomanip>              // std::hex, std::setw for hash hex string
#include <sstream>

DLT_DECLARE_CONTEXT(ota_ctx);

// ─── Helper: OtaState → string ───────────────────────────────────────────────
// Needed for JSON status publish. Static so it can be used before any OtaAgent
// object is constructed (e.g. in unit tests).

static const char* state_to_str(OtaState s)
{
    switch (s) {
        case OtaState::IDLE:              return "IDLE";
        case OtaState::MANIFEST_RECEIVED: return "MANIFEST_RECEIVED";
        case OtaState::DOWNLOADING:       return "DOWNLOADING";
        case OtaState::VERIFYING:         return "VERIFYING";
        case OtaState::INSTALLING:        return "INSTALLING";
        case OtaState::COMPLETE:          return "COMPLETE";
        case OtaState::FAILED:            return "FAILED";
        case OtaState::ROLLED_BACK:       return "ROLLED_BACK";
        default:                          return "UNKNOWN";
    }
}

// ─── libcurl write callback ───────────────────────────────────────────────────
// curl calls this each time it receives data.
// ptr  = chunk of data just received
// size = always 1 (curl API design quirk)
// nmemb= number of bytes in this chunk
// stream = our FILE* passed via CURLOPT_WRITEDATA
// Returns: bytes consumed — if != size*nmemb, curl aborts with write error

static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, FILE* stream)
{
    return fwrite(ptr, size, nmemb, stream);
}

// ─── Constructor / Destructor ────────────────────────────────────────────────

OtaAgent::OtaAgent(SafeQueue<Command>&  ota_queue,
                   mqtt::async_client&  mqtt_client,
                   const std::string&   uin,
                   const std::string&   firmware_path,
                   const std::string&   backup_path,
                   const std::string&   installed_path)
    : ota_queue_(ota_queue)
    , mqtt_client_(mqtt_client)
    , uin_(uin)
    , firmware_path_(firmware_path)
    , backup_path_(backup_path)
    , installed_path_(installed_path)
{}

OtaAgent::~OtaAgent()
{
    stop();
}

// ─── start / stop ─────────────────────────────────────────────────────────────

bool OtaAgent::start()
{
    DLT_REGISTER_CONTEXT(ota_ctx, "OTAA", "OTA Agent - firmware download and flash");

    running_ = true;
    thread_  = std::thread(&OtaAgent::run, this);
    DLT_LOG(ota_ctx, DLT_LOG_INFO, DLT_STRING("OtaAgent started"));
    return true;
}

void OtaAgent::stop()
{
    running_ = false;
    if (thread_.joinable()) thread_.join();
    DLT_LOG(ota_ctx, DLT_LOG_INFO, DLT_STRING("OtaAgent stopped"));
}

// ─── run ─────────────────────────────────────────────────────────────────────
// Main OTA flow. Blocks on ota_queue_ waiting for OTA_TRIGGER commands.
// Uses pop_for(1s) so running_ is checked every second — clean shutdown.

void OtaAgent::run()
{
    while (running_) {
        // Block up to 1s waiting for an OTA command
        auto result = ota_queue_.pop_for(std::chrono::seconds(1));
        if (!result) continue;   // timeout — no command yet, loop back

        const Command& cmd = *result;

        DLT_LOG(ota_ctx, DLT_LOG_INFO,
                DLT_STRING("OTA_TRIGGER received, correlation_id:"),
                DLT_STRING(cmd.correlation_id.c_str()));

        // ── Parse manifest from payload_json ─────────────────────────────────
        // Cloud sends: {"url": "https://...", "sha256": "abc123...", "version": "1.2.3"}
        std::string firmware_url;
        std::string expected_sha256;
        std::string fw_version;

        try {
            auto j       = nlohmann::json::parse(cmd.payload_json);
            firmware_url    = j.at("url").get<std::string>();
            expected_sha256 = j.at("sha256").get<std::string>();
            fw_version      = j.value("version", std::string("unknown"));
        } catch (const nlohmann::json::exception& e) {
            DLT_LOG(ota_ctx, DLT_LOG_ERROR,
                    DLT_STRING("Invalid OTA manifest:"), DLT_STRING(e.what()));
            report_status(cmd.correlation_id, {OtaState::FAILED, "", "Invalid manifest JSON"});
            continue;
        }

        report_status(cmd.correlation_id, {OtaState::MANIFEST_RECEIVED, fw_version, "", 0});

        // ── Step 1: Preconditions ─────────────────────────────────────────────
        if (!check_preconditions()) {
            report_status(cmd.correlation_id, {OtaState::FAILED, fw_version, "Preconditions not met"});
            continue;
        }

        // ── Step 2: Download ──────────────────────────────────────────────────
        report_status(cmd.correlation_id, {OtaState::DOWNLOADING, fw_version, "", 10});
        if (!download_firmware(firmware_url)) {
            report_status(cmd.correlation_id, {OtaState::FAILED, fw_version, "Download failed"});
            continue;
        }

        // ── Step 3: Verify SHA256 ─────────────────────────────────────────────
        // Do this BEFORE touching the installed firmware — fail fast on tampered files
        report_status(cmd.correlation_id, {OtaState::VERIFYING, fw_version, "", 50});
        if (!verify_sha256(expected_sha256)) {
            report_status(cmd.correlation_id, {OtaState::FAILED, fw_version, "SHA256 mismatch — firmware rejected"});
            std::filesystem::remove(firmware_path_);   // delete tampered file
            continue;
        }

        // ── Step 4: Backup current firmware ──────────────────────────────────
        // MUST succeed before we touch installed_path_ — no backup = no rollback
        if (!backup_current()) {
            report_status(cmd.correlation_id, {OtaState::FAILED, fw_version, "Backup failed — OTA aborted"});
            continue;
        }

        // ── Step 5: Flash ─────────────────────────────────────────────────────
        // flash_firmware() calls rollback() internally if the copy fails
        report_status(cmd.correlation_id, {OtaState::INSTALLING, fw_version, "", 75});
        if (!flash_firmware()) {
            report_status(cmd.correlation_id, {OtaState::ROLLED_BACK, fw_version, "Flash failed — rolled back to previous firmware"});
            continue;
        }

        // ── Done ──────────────────────────────────────────────────────────────
        report_status(cmd.correlation_id, {OtaState::COMPLETE, fw_version, "", 100});
        DLT_LOG(ota_ctx, DLT_LOG_INFO,
                DLT_STRING("OTA complete, version:"), DLT_STRING(fw_version.c_str()));
    }
}

// ─── check_preconditions ─────────────────────────────────────────────────────
// For Pi demo: checks available disk space (real check).
// Battery + speed TODO: in production these come from the latest CAN telemetry
// snapshot — OtaAgent would need a shared atomic<TelemetryMsg> injected.

bool OtaAgent::check_preconditions()
{
    // Disk space: need at least 100MB free at /tmp
    auto space = std::filesystem::space("/tmp");
    const uint64_t MIN_FREE_BYTES = 100ULL * 1024 * 1024;  // 100MB
    if (space.available < MIN_FREE_BYTES) {
        DLT_LOG(ota_ctx, DLT_LOG_ERROR,
                DLT_STRING("Precondition FAIL: insufficient disk space"));
        return false;
    }

    // TODO (production): check battery_voltage > MIN_BATTERY_V from telemetry snapshot
    // TODO (production): check vehicle_speed == 0 from telemetry snapshot

    DLT_LOG(ota_ctx, DLT_LOG_INFO, DLT_STRING("Preconditions OK"));
    return true;
}

// ─── download_firmware ────────────────────────────────────────────────────────
// HTTPS GET via libcurl → writes to firmware_path_
// SSL_VERIFYPEER=1: rejects self-signed or expired certs (TARA TR-03)

bool OtaAgent::download_firmware(const std::string& url)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        DLT_LOG(ota_ctx, DLT_LOG_ERROR, DLT_STRING("curl_easy_init failed"));
        return false;
    }

    FILE* fp = fopen(firmware_path_.c_str(), "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        DLT_LOG(ota_ctx, DLT_LOG_ERROR,
                DLT_STRING("Cannot open firmware_path for writing:"),
                DLT_STRING(firmware_path_.c_str()));
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);     // follow HTTP redirects
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);     // verify server cert
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);     // verify hostname in cert
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        300L);   // 5-minute timeout for large bins

    CURLcode res      = curl_easy_perform(curl);
    long     http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    // Must get http_code BEFORE cleanup — handle becomes invalid after cleanup
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res != CURLE_OK) {
        DLT_LOG(ota_ctx, DLT_LOG_ERROR,
                DLT_STRING("curl download error:"), DLT_STRING(curl_easy_strerror(res)));
        return false;
    }
    if (http_code != 200) {
        DLT_LOG(ota_ctx, DLT_LOG_ERROR,
                DLT_STRING("HTTP error code:"), DLT_INT(static_cast<int>(http_code)));
        return false;
    }

    DLT_LOG(ota_ctx, DLT_LOG_INFO,
            DLT_STRING("Firmware downloaded to"), DLT_STRING(firmware_path_.c_str()));
    return true;
}

// ─── verify_sha256 ────────────────────────────────────────────────────────────
// Reads firmware_path_ in 64KB chunks, feeds into OpenSSL EVP SHA256 context.
// Compares hex digest against expected_hash from the OTA manifest.

bool OtaAgent::verify_sha256(const std::string& expected_hash)
{
    std::ifstream file(firmware_path_, std::ios::binary);
    if (!file) {
        DLT_LOG(ota_ctx, DLT_LOG_ERROR, DLT_STRING("Cannot open firmware for SHA256 check"));
        return false;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);

    char buf[65536];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
        EVP_DigestUpdate(ctx, buf, static_cast<size_t>(file.gcount()));
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int  hash_len = 0;
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    // Convert binary hash to lowercase hex string
    std::ostringstream hex;
    for (unsigned int i = 0; i < hash_len; ++i) {
        hex << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(hash[i]);
    }

    bool match = (hex.str() == expected_hash);
    if (!match) {
        DLT_LOG(ota_ctx, DLT_LOG_ERROR,
                DLT_STRING("SHA256 MISMATCH — expected:"),
                DLT_STRING(expected_hash.c_str()),
                DLT_STRING("got:"), DLT_STRING(hex.str().c_str()));
    } else {
        DLT_LOG(ota_ctx, DLT_LOG_INFO, DLT_STRING("SHA256 verified OK"));
    }
    return match;
}

// ─── backup_current ───────────────────────────────────────────────────────────

bool OtaAgent::backup_current()
{
    try {
        std::filesystem::copy_file(installed_path_, backup_path_,
                                   std::filesystem::copy_options::overwrite_existing);
        DLT_LOG(ota_ctx, DLT_LOG_INFO,
                DLT_STRING("Current firmware backed up to"), DLT_STRING(backup_path_.c_str()));
        return true;
    } catch (const std::filesystem::filesystem_error& e) {
        DLT_LOG(ota_ctx, DLT_LOG_ERROR,
                DLT_STRING("Backup failed:"), DLT_STRING(e.what()));
        return false;
    }
}

// ─── flash_firmware ───────────────────────────────────────────────────────────
// Copies firmware_path_ → installed_path_.
// On failure: calls rollback() immediately — vehicle must not be left with a
// partial write at installed_path_.

bool OtaAgent::flash_firmware()
{
    try {
        std::filesystem::copy_file(firmware_path_, installed_path_,
                                   std::filesystem::copy_options::overwrite_existing);
        DLT_LOG(ota_ctx, DLT_LOG_INFO, DLT_STRING("Firmware flashed successfully"));
        return true;
    } catch (const std::filesystem::filesystem_error& e) {
        DLT_LOG(ota_ctx, DLT_LOG_ERROR,
                DLT_STRING("Flash failed:"), DLT_STRING(e.what()));
        rollback();   // restore backup immediately — don't leave corrupted state
        return false;
    }
}

// ─── rollback ─────────────────────────────────────────────────────────────────

bool OtaAgent::rollback()
{
    try {
        std::filesystem::copy_file(backup_path_, installed_path_,
                                   std::filesystem::copy_options::overwrite_existing);
        DLT_LOG(ota_ctx, DLT_LOG_WARN, DLT_STRING("Rolled back to previous firmware"));
        return true;
    } catch (const std::filesystem::filesystem_error& e) {
        DLT_LOG(ota_ctx, DLT_LOG_FATAL,
                DLT_STRING("ROLLBACK FAILED — vehicle may be in inconsistent state:"),
                DLT_STRING(e.what()));
        return false;
    }
}

// ─── report_status ────────────────────────────────────────────────────────────
// Publishes OtaStatus as JSON to "sdv/ota/from/uin/<uin>/status" (QoS 1).
// correlation_id echoed back so cloud matches this status to its original request.
// Must match the device's IAM policy (create_thing.py) and config.yaml's
// topic_ota_status — this was hardcoded to "ota/<uin>/status", which the IAM
// policy silently denies (visible only in CloudWatch, never in gateway logs).
// Fourth instance of this exact topic-drift bug pattern in this project.

void OtaAgent::report_status(const std::string& correlation_id, const OtaStatus& status)
{
    nlohmann::json j;
    j["correlation_id"] = correlation_id;
    j["state"]          = state_to_str(status.state);
    j["fw_version"]     = status.fw_version;
    j["error_msg"]      = status.error_msg;
    j["progress_pct"]   = status.progress_pct;

    std::string topic = "sdv/ota/from/uin/" + uin_ + "/status";

    try {
        auto msg = mqtt::make_message(topic, j.dump(), QOS, false);
        mqtt_client_.publish(msg)->wait();
    } catch (const mqtt::exception& e) {
        DLT_LOG(ota_ctx, DLT_LOG_ERROR,
                DLT_STRING("report_status publish failed:"), DLT_STRING(e.what()));
    }
}
