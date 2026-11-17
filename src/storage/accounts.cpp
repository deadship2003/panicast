// Y01: AccountsManager implementation. See header.
#include "panicast/storage/accounts.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <mutex>
#include <unordered_set>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "panicast/core/logger.h"
#include "panicast/net/google_oauth.h"
#include "panicast/storage/database.h"

namespace panicast
{

using json = nlohmann::json;

AccountsManager &AccountsManager::instance() {
    static AccountsManager a;
    return a;
}
AccountsManager::AccountsManager() {}

int64_t AccountsManager::now_epoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string AccountsManager::seal(const std::string &plaintext) {
    return token_seal(machine_key(), plaintext);
}
bool AccountsManager::open_sealed(const std::string &b64, std::string &out) {
    if (b64.empty())
        return false;
    return token_open(machine_key(), b64, out);
}

namespace
{
// Bind helpers executed under the caller's lock.
int exec_locked(sqlite3 *db, const std::string &sql) {
    char *err = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (err) {
        LOG(fmt::format("[Accounts] sql err: {}", err));
        sqlite3_free(err);
    }
    return rc;
}

// Parameterized exec for statements that take a single integer parameter (e.g. WHERE account_id=?).
// Y24.42: replaces fmt::format integer interpolation so no SQL is string-concatenated.
static int exec_locked_int(sqlite3 *db, const char *sql, int param) {
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG(fmt::format("[Accounts] sql err: prepare failed rc={}", rc));
        return rc;
    }
    sqlite3_bind_int(stmt, 1, param);
    int step_rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (step_rc == SQLITE_DONE || step_rc == SQLITE_ROW) ? SQLITE_OK : step_rc;
}

// P2-S1: per-account OAuth refresh single-flight. Without this, two threads refreshing the same
//   account concurrently can cause Google to revoke the refresh_token. Only one refresher runs;
//   others wait, then re-read the freshly-stored token.
std::mutex g_refresh_mtx;
std::condition_variable g_refresh_cv;
std::unordered_set<int> g_refreshing;
} // namespace

int AccountsManager::add_account(const std::string &google_email, const std::string &gaia_id,
                                 const std::string &channel_id, const std::string &access_token,
                                 const std::string &refresh_token, int64_t expires_at,
                                 const std::string &scope, const std::string &label) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return 0;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());

    // Deactivate all existing accounts; the new one becomes active.
    exec_locked(db, "UPDATE accounts SET is_active = 0;");

    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "INSERT INTO accounts (type, label, google_email, gaia_id, channel_id, "
        "access_token_enc, refresh_token_enc, token_expires_at, token_scope, "
        "created_at, last_login_at, is_active) "
        "VALUES ('google', ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, 1);";
    std::string lbl = label.empty() ? google_email : label;
    std::string at_enc = seal(access_token);
    std::string rt_enc = seal(refresh_token);
    int new_id = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, lbl.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, google_email.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, gaia_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, channel_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, at_enc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, rt_enc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 7, expires_at);
        sqlite3_bind_text(stmt, 8, scope.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_DONE)
            new_id = (int)sqlite3_last_insert_rowid(db);
        sqlite3_finalize(stmt);
    }
    // Persist active pointer.
    if (new_id > 0) {
        exec_locked(db, fmt::format("INSERT OR REPLACE INTO stats (key, value) VALUES "
                                    "('active_google_account_id', '{}');",
                                    new_id));
    }
    return new_id;
}

bool AccountsManager::update_tokens(int account_id, const std::string &access_token,
                                    const std::string &refresh_token, int64_t expires_at,
                                    const std::string &scope) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return false;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    sqlite3_stmt *stmt = nullptr;
    // refresh_token may be empty on a pure refresh; keep old if empty.
    std::string at_enc = seal(access_token);
    std::string rt_enc = refresh_token.empty() ? "" : seal(refresh_token);
    const char *sql =
        rt_enc.empty()
            ? "UPDATE accounts SET access_token_enc=?, token_expires_at=?, token_scope=?, "
              "last_login_at=CURRENT_TIMESTAMP WHERE account_id=?;"
            : "UPDATE accounts SET access_token_enc=?, refresh_token_enc=?, token_expires_at=?, "
              "token_scope=?, last_login_at=CURRENT_TIMESTAMP WHERE account_id=?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, at_enc.c_str(), -1, SQLITE_TRANSIENT);
    int idx = 2;
    if (!rt_enc.empty())
        sqlite3_bind_text(stmt, idx++, rt_enc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, idx++, expires_at);
    sqlite3_bind_text(stmt, idx++, scope.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, idx, account_id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool AccountsManager::update_profile(int account_id, const std::string &google_email,
                                     const std::string &gaia_id, const std::string &channel_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return false;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "UPDATE accounts SET google_email=?, gaia_id=?, channel_id=?, "
                      "label=COALESCE(NULLIF(label,''), google_email) WHERE account_id=?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, google_email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, gaia_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, channel_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, account_id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool AccountsManager::delete_account(int account_id) {
    if (account_id <= 0)
        return false;
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return false;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    // P2-C1: wrap the 5 deletes in a transaction so a partial failure can't orphan rows.
    exec_locked(db, "BEGIN;");
    bool ok = true;
    ok &= exec_locked_int(db, "DELETE FROM accounts WHERE account_id=?;", account_id) == SQLITE_OK;
    ok &= exec_locked_int(db, "DELETE FROM youtube_subscriptions WHERE account_id=?;",
                          account_id) == SQLITE_OK;
    ok &= exec_locked_int(db, "DELETE FROM youtube_history WHERE account_id=?;", account_id) ==
          SQLITE_OK;
    ok &= exec_locked_int(db, "DELETE FROM account_sync_state WHERE account_id=?;", account_id) ==
          SQLITE_OK;
    ok &= exec_locked_int(db, "DELETE FROM youtube_cache WHERE account_id=?;", account_id) ==
          SQLITE_OK;
    if (!ok) {
        exec_locked(db, "ROLLBACK;");
        return false;
    }
    exec_locked(db, "COMMIT;");
    // If we deleted the active account, clear the pointer (no active login state).
    // Read the active id directly under the already-held lock (active_account_id() would re-lock).
    int active = 0;
    {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT value FROM stats WHERE key='active_google_account_id';",
                               -1, &st, nullptr) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char *t = sqlite3_column_text(st, 0);
                if (t)
                    active = std::atoi(reinterpret_cast<const char *>(t));
            }
            sqlite3_finalize(st);
        }
    }
    if (active == account_id) {
        exec_locked(db, "UPDATE accounts SET is_active=0;");
        exec_locked(db, "DELETE FROM stats WHERE key='active_google_account_id';");
    }
    return true;
}

bool AccountsManager::set_label(int account_id, const std::string &label) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return false;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "UPDATE accounts SET label=? WHERE account_id=?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, account_id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<GoogleAccount> AccountsManager::list_accounts() {
    std::vector<GoogleAccount> out;
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return out;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT account_id, type, label, google_email, gaia_id, channel_id, "
        "token_expires_at, token_scope, created_at, last_login_at, last_sync_at, is_active "
        "FROM accounts ORDER BY account_id ASC;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            GoogleAccount a;
            auto col = [&](int i) -> std::string {
                const unsigned char *t = sqlite3_column_text(stmt, i);
                return t ? reinterpret_cast<const char *>(t) : "";
            };
            a.account_id = sqlite3_column_int(stmt, 0);
            a.type = col(1);
            a.label = col(2);
            a.google_email = col(3);
            a.gaia_id = col(4);
            a.channel_id = col(5);
            a.token_expires_at = sqlite3_column_int64(stmt, 6);
            a.token_scope = col(7);
            a.created_at = sqlite3_column_int64(stmt, 8);
            a.last_login_at = sqlite3_column_int64(stmt, 9);
            a.last_sync_at = sqlite3_column_int64(stmt, 10);
            a.is_active = sqlite3_column_int(stmt, 11) != 0;
            out.push_back(std::move(a));
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

void AccountsManager::touch_login(int account_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return;
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    exec_locked_int(dbm.raw_handle(),
                    "UPDATE accounts SET last_login_at=CURRENT_TIMESTAMP WHERE account_id=?;",
                    account_id);
}

void AccountsManager::touch_sync(int account_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return;
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    exec_locked_int(dbm.raw_handle(),
                    "UPDATE accounts SET last_sync_at=CURRENT_TIMESTAMP WHERE account_id=?;",
                    account_id);
}

int AccountsManager::active_account_id() {
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return 0;
    sqlite3 *db = dbm.raw_handle();
    // read-only; still take the mutex for consistency
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    sqlite3_stmt *stmt = nullptr;
    int id = 0;
    if (sqlite3_prepare_v2(db, "SELECT value FROM stats WHERE key='active_google_account_id';", -1,
                           &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *t = sqlite3_column_text(stmt, 0);
            if (t)
                id = std::atoi(reinterpret_cast<const char *>(t));
        }
        sqlite3_finalize(stmt);
    }
    return id;
}

void AccountsManager::set_active_account(int account_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    exec_locked(db, "UPDATE accounts SET is_active=0;");
    if (account_id > 0) {
        exec_locked_int(db, "UPDATE accounts SET is_active=1 WHERE account_id=?;", account_id);
        exec_locked(db, fmt::format("INSERT OR REPLACE INTO stats (key, value) VALUES "
                                    "('active_google_account_id', '{}');",
                                    account_id));
    } else {
        exec_locked(db, "DELETE FROM stats WHERE key='active_google_account_id';");
    }
}

bool AccountsManager::get_account(int account_id, GoogleAccount &out) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return false;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT account_id, label, google_email, gaia_id, channel_id, "
        "access_token_enc, refresh_token_enc, token_expires_at, token_scope, is_active "
        "FROM accounts WHERE account_id=?;";
    bool found = false;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, account_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            auto col = [&](int i) -> std::string {
                const unsigned char *t = sqlite3_column_text(stmt, i);
                return t ? reinterpret_cast<const char *>(t) : "";
            };
            out.account_id = sqlite3_column_int(stmt, 0);
            out.label = col(1);
            out.google_email = col(2);
            out.gaia_id = col(3);
            out.channel_id = col(4);
            std::string at_enc = col(5), rt_enc = col(6);
            open_sealed(at_enc, out.access_token);
            open_sealed(rt_enc, out.refresh_token);
            out.token_expires_at = sqlite3_column_int64(stmt, 7);
            out.token_scope = col(8);
            out.is_active = sqlite3_column_int(stmt, 9) != 0;
            found = true;
        }
        sqlite3_finalize(stmt);
    }
    return found;
}

bool AccountsManager::get_tokens(int account_id, GoogleAccount &out) {
    if (!get_account(account_id, out))
        return false;
    // Refresh if expired (or within 60s of expiry).
    if (out.token_expires_at - 60 > now_epoch())
        return !out.access_token.empty();
    if (out.refresh_token.empty())
        return false;

    // P2-S1: single-flight — only one thread refreshes this account; others wait then re-read.
    {
        std::unique_lock<std::mutex> rlk(g_refresh_mtx);
        if (g_refreshing.count(account_id)) {
            g_refresh_cv.wait(rlk, [&] { return !g_refreshing.count(account_id); });
            rlk.unlock();
            // Another thread just refreshed; re-read the stored token instead of refreshing again.
            GoogleAccount cur;
            if (get_account(account_id, cur))
                out = cur;
            return !out.access_token.empty() && (out.token_expires_at - 60 > now_epoch());
        }
        g_refreshing.insert(account_id);
    }

    GoogleOAuth::TokenResult tr = GoogleOAuth::refresh(out.refresh_token);
    bool ok = tr.ok;
    if (ok) {
        out.access_token = tr.access_token;
        out.token_expires_at = tr.obtained_at + tr.expires_in;
        out.token_scope = tr.scope.empty() ? out.token_scope : tr.scope;
        update_tokens(account_id, out.access_token, "", out.token_expires_at, out.token_scope);
    } else {
        LOG(fmt::format("[Accounts] token refresh failed for {}: {}", account_id, tr.error));
    }
    {
        std::lock_guard<std::mutex> rlk(g_refresh_mtx);
        g_refreshing.erase(account_id);
    }
    g_refresh_cv.notify_all();
    return ok && !out.access_token.empty();
}

void AccountsManager::replace_subscriptions(int account_id,
                                            const std::vector<YouTubeSubRow> &subs) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    // P2-C5: wrap delete+inserts in a transaction; check sqlite3_step == SQLITE_DONE.
    exec_locked(db, "BEGIN;");
    exec_locked_int(db, "DELETE FROM youtube_subscriptions WHERE account_id=?;", account_id);
    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "INSERT OR REPLACE INTO youtube_subscriptions "
        "(account_id, channel_id, channel_name, channel_url, subscription_order, synced_at) "
        "VALUES (?,?,?,?,?, CURRENT_TIMESTAMP);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        for (const auto &s : subs) {
            sqlite3_bind_int(stmt, 1, account_id);
            sqlite3_bind_text(stmt, 2, s.channel_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, s.channel_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, s.channel_url.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 5, s.subscription_order);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                LOG(fmt::format("[Accounts] replace_subscriptions step failed: {}",
                                sqlite3_errmsg(db)));
            }
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }
        sqlite3_finalize(stmt);
    }
    exec_locked(db, "COMMIT;");
}

std::vector<YouTubeSubRow> AccountsManager::load_subscriptions(int account_id) {
    std::vector<YouTubeSubRow> out;
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return out;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(
            db,
            "SELECT channel_id, channel_name, channel_url, subscription_order "
            "FROM youtube_subscriptions WHERE account_id=? ORDER BY subscription_order ASC;",
            -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, account_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            YouTubeSubRow r;
            auto col = [&](int i) -> std::string {
                const unsigned char *t = sqlite3_column_text(stmt, i);
                return t ? reinterpret_cast<const char *>(t) : "";
            };
            r.channel_id = col(0);
            r.channel_name = col(1);
            r.channel_url = col(2);
            r.subscription_order = sqlite3_column_int(stmt, 3);
            out.push_back(std::move(r));
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

void AccountsManager::upsert_history(int account_id, const YouTubeHistoryRow &row) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "INSERT OR REPLACE INTO youtube_history "
                      "(account_id, video_id, title, channel_name, duration, watched_at, source) "
                      "VALUES (?,?,?,?,?, CURRENT_TIMESTAMP, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, account_id);
        sqlite3_bind_text(stmt, 2, row.video_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, row.title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, row.channel_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, row.duration);
        sqlite3_bind_text(stmt, 6, row.source.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE)
            LOG(fmt::format("[Accounts] step failed: {}", sqlite3_errmsg(db)));
        sqlite3_finalize(stmt);
    }
}

std::vector<YouTubeHistoryRow> AccountsManager::load_history(int account_id, int limit) {
    std::vector<YouTubeHistoryRow> out;
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return out;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    sqlite3_stmt *stmt = nullptr;
    std::string sql = fmt::format(
        "SELECT video_id, title, channel_name, duration, watched_at, source FROM youtube_history "
        "WHERE account_id=? ORDER BY watched_at DESC LIMIT {};",
        limit);
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, account_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            YouTubeHistoryRow r;
            auto col = [&](int i) -> std::string {
                const unsigned char *t = sqlite3_column_text(stmt, i);
                return t ? reinterpret_cast<const char *>(t) : "";
            };
            r.video_id = col(0);
            r.title = col(1);
            r.channel_name = col(2);
            r.duration = sqlite3_column_int(stmt, 3);
            r.watched_at = col(4);
            r.source = col(5);
            out.push_back(std::move(r));
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

void AccountsManager::clear_history(int account_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return;
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    exec_locked_int(dbm.raw_handle(), "DELETE FROM youtube_history WHERE account_id=?;",
                    account_id);
}

void AccountsManager::set_sync_state(int account_id, const std::string &sync_type,
                                     const std::string &cursor) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    sqlite3_stmt *stmt = nullptr;
    // DB-3/FX-4: store last_sync_at as an INTEGER Unix epoch (was CURRENT_TIMESTAMP text, which
    //   get_sync_last parsed with atoll → always 0, breaking incremental sync).
    const char *sql = "INSERT OR REPLACE INTO account_sync_state (account_id, sync_type, "
                      "last_sync_at, sync_cursor) "
                      "VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, account_id);
        sqlite3_bind_text(stmt, 2, sync_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, now_epoch());
        sqlite3_bind_text(stmt, 4, cursor.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE)
            LOG(fmt::format("[Accounts] step failed: {}", sqlite3_errmsg(db)));
        sqlite3_finalize(stmt);
    }
}

int64_t AccountsManager::get_sync_last(int account_id, const std::string &sync_type) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &dbm = DatabaseManager::instance();
    if (!dbm.is_ready())
        return 0;
    sqlite3 *db = dbm.raw_handle();
    std::lock_guard<std::recursive_mutex> dlk(dbm.raw_mutex());
    sqlite3_stmt *stmt = nullptr;
    int64_t ts = 0;
    // DB-3/FX-4: last_sync_at is now an INTEGER epoch; read it directly (was text+atoll → 0).
    //   Bind sync_type (DB-4) instead of string interpolation.
    const char *sql =
        "SELECT last_sync_at FROM account_sync_state WHERE account_id=? AND sync_type=?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, account_id);
        sqlite3_bind_text(stmt, 2, sync_type.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            ts = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return ts;
}

} // namespace panicast
