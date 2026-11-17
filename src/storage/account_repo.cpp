// Account repository (accounts, bilibili, tiktok, youtube_cache).
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

// ── YouTube channel cache (single shared connection) ──
bool DatabaseManager::youtube_cache_load(const std::string &channel_url, std::string &out_name,
                                         std::string &out_videos_json, int account_id) {
    out_name.clear();
    out_videos_json.clear();
    if (!is_ready() || channel_url.empty())
        return false;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *stmt = nullptr;
    bool found = false;
    const char *sql = "SELECT channel_name, videos_json FROM youtube_cache "
                      "WHERE channel_url=? AND account_id=? LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, channel_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, account_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            const char *vids = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            out_name = name ? name : "";
            out_videos_json = vids ? vids : "";
            found = true;
        }
        sqlite3_finalize(stmt);
    }
    return found;
}

void DatabaseManager::youtube_cache_save(const std::string &channel_url,
                                         const std::string &channel_name,
                                         const std::string &videos_json, int account_id) {
    if (!is_ready() || channel_url.empty())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "INSERT OR REPLACE INTO youtube_cache (account_id, channel_url, "
                      "channel_name, videos_json, updated_at) "
                      "VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, account_id);
        sqlite3_bind_text(stmt, 2, channel_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, channel_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, videos_json.c_str(), static_cast<int>(videos_json.size()),
                          SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            LOG(fmt::format("[DB] youtube_cache_save step failed: {}", sqlite3_errmsg(db_)));
        }
        sqlite3_finalize(stmt);
    }
}

// Y24.27: bilibili account CRUD (moved from app_bilibili.cpp — was direct sqlite3_* calls).
std::vector<BilibiliAccount> DatabaseManager::list_bilibili_accounts() {
    std::vector<BilibiliAccount> out;
    if (!is_ready())
        return out;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT id, uid, uname, sessdata, bili_jct, dedeuserid FROM "
                           "bilibili_accounts ORDER BY id",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            BilibiliAccount a;
            a.id = sqlite3_column_int(stmt, 0);
            a.uid = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            a.uname = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            a.sessdata = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            a.bili_jct = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
            a.dedeuserid = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
            out.push_back(a);
        }
        sqlite3_finalize(stmt);
    } else {
        LOG(fmt::format("[DB] list_bilibili_accounts prepare failed: {}", sqlite3_errmsg(db_)));
    }
    return out;
}

int DatabaseManager::upsert_bilibili_account(const BilibiliAccount &a) {
    if (!is_ready())
        return 0;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // Check for duplicate uid → update.
    int existing_id = 0;
    {
        sqlite3_stmt *chk = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT id FROM bilibili_accounts WHERE uid=?;", -1, &chk,
                               nullptr) == SQLITE_OK) {
            sqlite3_bind_text(chk, 1, a.uid.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(chk) == SQLITE_ROW)
                existing_id = sqlite3_column_int(chk, 0);
            sqlite3_finalize(chk);
        }
    }
    if (existing_id) {
        sqlite3_stmt *upd = nullptr;
        if (sqlite3_prepare_v2(db_,
                               "UPDATE bilibili_accounts SET sessdata=?, bili_jct=?, dedeuserid=?, "
                               "uname=?, last_login_at=datetime('now') WHERE id=?;",
                               -1, &upd, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(upd, 1, a.sessdata.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upd, 2, a.bili_jct.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upd, 3, a.dedeuserid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upd, 4, a.uname.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(upd, 5, existing_id);
            sqlite3_step(upd);
            sqlite3_finalize(upd);
        }
        return existing_id;
    }
    sqlite3_stmt *ins = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO bilibili_accounts (uid, uname, sessdata, bili_jct, "
                           "dedeuserid, last_login_at) VALUES (?, ?, ?, ?, ?, datetime('now'));",
                           -1, &ins, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(ins, 1, a.uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, a.uname.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, a.sessdata.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 4, a.bili_jct.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 5, a.dedeuserid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
        sqlite3_finalize(ins);
    }
    return (int)sqlite3_last_insert_rowid(db_);
}

bool DatabaseManager::delete_bilibili_account(int id) {
    if (!is_ready() || id <= 0)
        return false;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *del = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db_, "DELETE FROM bilibili_accounts WHERE id=?;", -1, &del, nullptr) ==
        SQLITE_OK) {
        sqlite3_bind_int(del, 1, id);
        ok = sqlite3_step(del) == SQLITE_DONE;
        sqlite3_finalize(del);
    }
    return ok;
}

// Y24.27: tiktok account CRUD (moved from app_tiktok.cpp).
std::vector<TiktokAccount> DatabaseManager::list_tiktok_accounts() {
    std::vector<TiktokAccount> out;
    if (!is_ready())
        return out;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_, "SELECT id, platform, handle, url, uname FROM tiktok_accounts ORDER BY id", -1,
            &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            TiktokAccount a;
            a.id = sqlite3_column_int(stmt, 0);
            a.platform = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            a.handle = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            a.url = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            a.uname = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
            out.push_back(a);
        }
        sqlite3_finalize(stmt);
    } else {
        LOG(fmt::format("[DB] list_tiktok_accounts prepare failed: {}", sqlite3_errmsg(db_)));
    }
    return out;
}

int DatabaseManager::upsert_tiktok_account(const TiktokAccount &a) {
    if (!is_ready())
        return 0;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    int existing_id = 0;
    {
        sqlite3_stmt *chk = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT id FROM tiktok_accounts WHERE platform=? AND handle=?;",
                               -1, &chk, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(chk, 1, a.platform.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(chk, 2, a.handle.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(chk) == SQLITE_ROW)
                existing_id = sqlite3_column_int(chk, 0);
            sqlite3_finalize(chk);
        }
    }
    if (existing_id) {
        sqlite3_stmt *upd = nullptr;
        if (sqlite3_prepare_v2(db_, "UPDATE tiktok_accounts SET url=?, uname=? WHERE id=?;", -1,
                               &upd, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(upd, 1, a.url.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upd, 2, a.uname.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(upd, 3, existing_id);
            sqlite3_step(upd);
            sqlite3_finalize(upd);
        }
        return existing_id;
    }
    sqlite3_stmt *ins = nullptr;
    if (sqlite3_prepare_v2(
            db_, "INSERT INTO tiktok_accounts (platform, handle, url, uname) VALUES (?, ?, ?, ?);",
            -1, &ins, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(ins, 1, a.platform.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, a.handle.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, a.url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 4, a.uname.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
        sqlite3_finalize(ins);
    }
    return (int)sqlite3_last_insert_rowid(db_);
}

bool DatabaseManager::delete_tiktok_account(int id) {
    if (!is_ready() || id <= 0)
        return false;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *del = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db_, "DELETE FROM tiktok_accounts WHERE id=?;", -1, &del, nullptr) ==
        SQLITE_OK) {
        sqlite3_bind_int(del, 1, id);
        ok = sqlite3_step(del) == SQLITE_DONE;
        sqlite3_finalize(del);
    }
    return ok;
}

// Y23: bilibili UP logo cache — UPSERT by mid.
void DatabaseManager::save_bili_up(const std::string &mid, const std::string &uname,
                                   const std::string &upic, int fans, const std::string &sign) {
    if (!is_ready() || mid.empty())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "INSERT OR REPLACE INTO bilibili_up_cache (mid, uname, upic, fans, sign, updated_at) "
        "VALUES (?, ?, ?, ?, ?, CURRENT_TIMESTAMP);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, mid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, uname.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, upic.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, fans);
        sqlite3_bind_text(stmt, 5, sign.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::string DatabaseManager::load_bili_up_pic(const std::string &mid) {
    if (!is_ready() || mid.empty())
        return "";
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *stmt = nullptr;
    std::string pic;
    if (sqlite3_prepare_v2(db_, "SELECT upic FROM bilibili_up_cache WHERE mid=?;", -1, &stmt,
                           nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, mid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *t = sqlite3_column_text(stmt, 0);
            if (t)
                pic = reinterpret_cast<const char *>(t);
        }
        sqlite3_finalize(stmt);
    }
    return pic;
}
} // namespace panicast
