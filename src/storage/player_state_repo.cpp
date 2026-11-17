// Player-state repository (player_state, removed_defaults).
// Y24.37: domain split out of database.cpp. Methods remain DatabaseManager members
//   (they use db_/mtx_ and the infra helpers declared in database.h); only their
//   implementations live here. Declarations stay in database.h.
#include "panicast/storage/database.h"

#include <cmath>
#include <cstring>
#include <filesystem>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "panicast/config/ini_config.h"
#include "panicast/core/logger.h"
#include "panicast/core/paths.h"

namespace panicast
{

namespace fs = std::filesystem;
using json = nlohmann::json;

// Extended player state save
void DatabaseManager::save_player_state(int volume, double speed, bool paused,
                                        const std::string &url, double position, bool scroll_mode,
                                        bool show_tree_lines, const std::string &current_title,
                                        int current_mode) {
    if (!is_ready())
        return;
    // Clamp doubles to prevent NaN/Inf from polluting SQL
    if (std::isnan(speed) || std::isinf(speed))
        speed = 1.0;
    if (std::isnan(position) || std::isinf(position))
        position = 0.0;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Parameterized (no string interpolation)
    const char *sql = "INSERT OR REPLACE INTO player_state "
                      "(id, volume, speed, paused, current_url, position, scroll_mode, "
                      "show_tree_lines, current_title, current_mode) "
                      "VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, volume);
        sqlite3_bind_double(stmt, 2, speed);
        sqlite3_bind_int(stmt, 3, paused ? 1 : 0);
        sqlite3_bind_text(stmt, 4, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 5, position);
        sqlite3_bind_int(stmt, 6, scroll_mode ? 1 : 0);
        sqlite3_bind_int(stmt, 7, show_tree_lines ? 1 : 0);
        sqlite3_bind_text(stmt, 8, current_title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 9, current_mode);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// Player state load
DatabaseManager::PlayerStateData DatabaseManager::load_player_state() {
    PlayerStateData state;
    if (!is_ready())
        return state;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    sqlite3_stmt *stmt;
    const char *sql = "SELECT volume, speed, paused, current_url, position, scroll_mode, "
                      "show_tree_lines, current_title, current_mode FROM player_state WHERE id=1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            state.volume = sqlite3_column_int(stmt, 0);
            state.speed = sqlite3_column_double(stmt, 1);
            state.paused = sqlite3_column_int(stmt, 2) != 0;
            const char *url = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            state.current_url = url ? url : "";
            state.position = sqlite3_column_double(stmt, 4);
            state.scroll_mode = sqlite3_column_int(stmt, 5) != 0;
            state.show_tree_lines = sqlite3_column_int(stmt, 6) != 0;
            const char *title = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
            state.current_title = title ? title : "";
            state.current_mode = sqlite3_column_int(stmt, 8);
        }
        sqlite3_finalize(stmt);
    }
    return state;
}

// Removed built-in default podcasts
void DatabaseManager::add_removed_default(const std::string &url) {
    if (!is_ready() || url.empty())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // Parameterized (no string interpolation)
    const char *sql = "INSERT OR IGNORE INTO removed_defaults (url) VALUES (?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::set<std::string> DatabaseManager::load_removed_defaults() {
    std::set<std::string> result;
    if (!is_ready())
        return result;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT url FROM removed_defaults;", -1, &stmt, nullptr) ==
        SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *t = sqlite3_column_text(stmt, 0);
            if (t)
                result.insert(reinterpret_cast<const char *>(t));
        }
        sqlite3_finalize(stmt);
    }
    return result;
}
} // namespace panicast
