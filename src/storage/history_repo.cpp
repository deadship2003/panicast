// History + playback-progress repository (progress, play history).
// Y24.37: domain split out of database.cpp. Methods remain DatabaseManager members
//   (they use db_/mtx_ and the infra helpers declared in database.h); only their
//   implementations live here. Declarations stay in database.h.
#include "panicast/storage/database.h"

#include <cmath>
#include <cstring>
#include <filesystem>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "panicast/net/url_classifier.h" // N06: classifyMediaType for media_type

#include "panicast/config/ini_config.h"
#include "panicast/core/logger.h"
#include "panicast/core/paths.h"

namespace panicast
{

namespace fs = std::filesystem;
using json = nlohmann::json;

void DatabaseManager::cleanup_old_history() {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Read config from INI
    int max_days = IniConfig::instance().get_history_max_days();
    int max_records = IniConfig::instance().get_history_max_records();

    // Clean by days (if configured > 0) — parameterized modifier string
    if (max_days > 0) {
        const char *sql = "DELETE FROM history WHERE timestamp < datetime('now', ?);";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            std::string mod = fmt::format("-{} days", max_days);
            sqlite3_bind_text(stmt, 1, mod.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    // Clean by record count (if configured > 0) — parameterized LIMIT
    if (max_records > 0) {
        const char *sql = "DELETE FROM history WHERE id NOT IN "
                          "(SELECT id FROM history ORDER BY timestamp DESC LIMIT ?);";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, max_records);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    LOG(fmt::format("[DB] History cleanup: max {} days, {} records", max_days, max_records));
}

// Progress management
void DatabaseManager::save_progress(const std::string &url, double position, bool completed) {
    if (!is_ready())
        return;
    // NaN/Inf would be formatted by fmt as "nan"/"inf", producing invalid SQL
    if (std::isnan(position) || std::isinf(position))
        position = 0.0;
    if (position < 0)
        position = 0.0;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // Parameterized (no string interpolation)
    const char *sql = "INSERT OR REPLACE INTO progress (url, position, completed, last_played) "
                      "VALUES (?, ?, ?, datetime('now'));";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, position);
        sqlite3_bind_int(stmt, 3, completed ? 1 : 0);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::pair<double, bool> DatabaseManager::get_progress(const std::string &url) {
    if (!is_ready())
        return {0.0, false};
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // Parameterized (no string interpolation)
    const char *sql = "SELECT position, completed FROM progress WHERE url = ?;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return {0.0, false};
    }
    sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);

    double pos = 0.0;
    bool completed = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        pos = sqlite3_column_double(stmt, 0);
        completed = sqlite3_column_int(stmt, 1) != 0;
    }
    sqlite3_finalize(stmt);
    return {pos, completed};
}

// History
void DatabaseManager::add_history(const std::string &url, const std::string &title, int duration,
                                  const std::string &artist, const std::string &album,
                                  const std::string &art_url) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // F38: UPSERT on url — one row per URL; re-playing refreshes timestamp → moves to top of history.
    // N06: also persist media_type (display category) so the icon no longer needs URL inference.
    //   Parameterized (no string interpolation)
    const char *sql =
        "INSERT INTO history (url, title, duration, media_type, timestamp) "
        "VALUES (?, ?, ?, ?, datetime('now')) "
        "ON CONFLICT(url) DO UPDATE SET title=excluded.title, duration=excluded.duration, "
        "media_type=excluded.media_type, timestamp=datetime('now');";
    int mt = static_cast<int>(URLClassifier::classifyMediaType(url));
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, duration);
        sqlite3_bind_int(stmt, 4, mt);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::vector<
    std::tuple<std::string, std::string, std::string, int, std::string, std::string, std::string>>
DatabaseManager::get_history(int limit) {
    if (!is_ready())
        return {};
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::vector<std::tuple<std::string, std::string, std::string, int, std::string, std::string,
                           std::string>>
        result;
    // Parameterized LIMIT (no string interpolation). N05: media_type; META-3: display meta.
    const char *sql =
        "SELECT url, title, timestamp, media_type, artist, album, art_url FROM history "
        "ORDER BY timestamp DESC LIMIT ?;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            // sqlite3_column_text returns NULL for SQL NULL,
            //   and constructing std::string from NULL is UB — so NULL-check here
            auto col = [&](int i) -> std::string {
                const unsigned char *t = sqlite3_column_text(stmt, i);
                return t ? reinterpret_cast<const char *>(t) : "";
            };
            result.push_back({col(0), col(1), col(2), sqlite3_column_int(stmt, 3), col(4), col(5),
                              col(6)}); // META-3
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

// Delete a specific playback history entry
void DatabaseManager::delete_history(const std::string &url) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Parameterized (no string interpolation)
    const char *sql = "DELETE FROM history WHERE url=?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    LOG(fmt::format("[DB] Deleted history: {}", url));
}
} // namespace panicast
