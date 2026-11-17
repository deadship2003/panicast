#include "panicast/core/logger.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fmt/format.h>

#include "panicast/core/constants.h"
#include "panicast/core/paths.h"
#include "panicast/core/platform.h"

namespace panicast
{

namespace fs = std::filesystem;

// Y24.17: log retention — one file per day (panicast-YYYYMMDD.log), auto-deleted after 365 days.
//   The mpv log subscription (also Y24.17) increases log volume, so bounded retention is needed.
//   Daily files make "keep 365 days" precise; cleanup compares each file's mtime to now via the
//   file_time_type clock (portable — no clock conversion, no date parsing).
constexpr int LOG_RETENTION_DAYS = 365;

// Today's date as YYYYMMDD (for the daily log filename).
static std::string today_date() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[16];
    struct tm tm_local;
    struct tm *tmp = localtime_local(&t, &tm_local);
    if (tmp)
        std::strftime(buf, sizeof(buf), "%Y%m%d", tmp);
    else
        buf[0] = '\0';
    return buf;
}

// Delete panicast-*.log and legacy panicast.log files older than LOG_RETENTION_DAYS (by mtime).
static void cleanup_old_logs(const std::string &dir) {
    std::error_code ec;
    auto now = fs::file_time_type::clock::now();
    auto cutoff = now - std::chrono::hours(24 * LOG_RETENTION_DAYS);
    for (auto &e : fs::directory_iterator(dir, ec)) {
        if (ec)
            break;
        auto name = e.path().filename().string();
        // Daily files "panicast-YYYYMMDD.log" + legacy "panicast.log". Don't touch other files.
        bool is_log = (name.rfind("panicast-", 0) == 0 && name.size() > 10 &&
                       name.compare(name.size() - 4, 4, ".log") == 0) ||
                      name == "panicast.log";
        if (!is_log)
            continue;
        auto mt = fs::last_write_time(e, ec);
        if (ec)
            continue;
        if (mt < cutoff) {
            fs::remove(e, ec);
            if (!ec)
                fmt::print(""); // no-op (can't LOG yet — Logger not open)
        }
    }
}

Logger &Logger::instance() {
    static Logger l;
    return l;
}

void Logger::init() {
    std::string dir = Paths::get_data_dir();
    if (dir.empty())
        return;
    fs::create_directories(dir);
    cleanup_old_logs(dir); // Y24.17: delete logs older than 365 days
    std::string path = dir + "/panicast-" + today_date() + ".log";
    file_.open(path, std::ios::app);
    if (file_.is_open()) {
        file_ << "========================================\n";
        file_ << fmt::format("{} {} by {}\n", APP_NAME, VERSION, AUTHOR);
        file_ << "========================================\n";
        file_.flush();
    }
}

void Logger::log(const std::string &msg) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (file_.is_open()) {
        // Add a timestamp to all log entries
        auto now = std::chrono::system_clock::now();
        auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char buf[32];
        struct tm tm_local;
        struct tm *tmp = localtime_local(&t, &tm_local);
        // P2-C10: localtime_local may return nullptr on failure — guard strftime (else UB).
        if (tmp)
            std::strftime(buf, sizeof(buf), "%H:%M:%S", tmp);
        else
            buf[0] = '\0';
        std::string line = fmt::format("[{}.{:03d}] {}", buf, ms.count(), msg);
        file_ << line << std::endl;
        file_.flush();
        if (echo_stderr_) // LIF-002 --debug: console mirror of the log stream
            fmt::print(stderr, "{}\n", line);
    }
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (file_.is_open()) {
        file_ << "========================================\n";
        file_.close();
    }
}

} // namespace panicast
