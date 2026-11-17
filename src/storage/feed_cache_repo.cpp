// Feed / episode / search-cache repository (podcast_cache, episode_cache, search_cache).
// Y24.37: domain split out of database.cpp. Methods remain DatabaseManager members
//   (they use db_/mtx_ and the infra helpers declared in database.h); only their
//   implementations live here. Declarations stay in database.h.
#include "panicast/storage/database.h"

#include <cmath>
#include <cstring>
#include <ctime>
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

// Purge cache tables only, preserve user data
void DatabaseManager::purge_cache_only() {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_exec(db_,
                 "DELETE FROM search_cache; "
                 "DELETE FROM podcast_cache; "
                 "DELETE FROM episode_cache; "
                 "DELETE FROM media_cache; "
                 "DELETE FROM stats; ",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "VACUUM;", nullptr, nullptr, nullptr);
    LOG("[DB] Cache purged (user data preserved)");
}

// Search cache management (uses INI config)
void DatabaseManager::save_search_cache(const std::string &query, const std::string &region,
                                        const std::string &result_json) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Read max cache count from INI
    int max_cache = IniConfig::instance().get_search_cache_max();

    // First check whether a record with the same query and region exists (parameterized)
    bool exists = false;
    {
        const char *check_sql = "SELECT id FROM search_cache WHERE query=? AND region=? LIMIT 1;";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, check_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, region.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW)
                exists = true;
            sqlite3_finalize(stmt);
        }
    }

    if (exists) {
        // Update timestamp and result of the existing record (parameterized)
        const char *update_sql =
            "UPDATE search_cache SET result_json=?, timestamp=CURRENT_TIMESTAMP "
            "WHERE query=? AND region=?;";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, update_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, result_json.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, query.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, region.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    } else {
        // Insert a new record (parameterized)
        const char *sql = "INSERT INTO search_cache (query, region, result_json) VALUES (?, ?, ?);";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, region.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, result_json.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    // Only clean by count, not by time (max_cache is a sanitized int from config; bound for consistency)
    {
        const char *cleanup = "DELETE FROM search_cache WHERE id NOT IN "
                              "(SELECT id FROM search_cache ORDER BY timestamp DESC LIMIT ?);";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, cleanup, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, max_cache);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
}

// Load all search history (for Online mode display)
std::vector<DatabaseManager::SearchHistoryItem> DatabaseManager::load_all_search_history() {
    std::vector<SearchHistoryItem> history;
    if (!is_ready())
        return history;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // P3-D7/DB-19: single query including result_json (was N+1 — one extra query per row to fetch
    //   the JSON it could have selected). Removed the dead `(SELECT COUNT(*) FROM podcast_cache) as dummy`
    //   subquery that computed a value never read.
    const char *sql = "SELECT id, query, region, timestamp, result_json "
                      "FROM search_cache ORDER BY timestamp DESC;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SearchHistoryItem item;
            item.id = sqlite3_column_int(stmt, 0);

            const char *q = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            item.query = q ? q : "";

            const char *r = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            item.region = r ? r : "US";

            const char *t = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            item.timestamp = t ? t : "";

            const char *json_data = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
            if (json_data) {
                try {
                    json j = json::parse(json_data);
                    if (j.contains("results") && j["results"].is_array()) {
                        item.result_count = j["results"].size();
                    }
                } catch (...) {
                    item.result_count = 0;
                }
            }

            history.push_back(item);
        }
        sqlite3_finalize(stmt);
    }
    return history;
}

// Get full data of a single search cache entry
std::string DatabaseManager::load_search_cache(const std::string &query,
                                               const std::string &region) {
    if (!is_ready())
        return "";
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Parameterized query (no string interpolation)
    const char *sql = "SELECT result_json FROM search_cache WHERE query=? AND region=? ORDER BY "
                      "timestamp DESC LIMIT 1;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, region.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *data = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            std::string result = data ? data : "";
            sqlite3_finalize(stmt);
            return result;
        }
        sqlite3_finalize(stmt);
    }
    return "";
}

// Delete a specific search history entry
void DatabaseManager::delete_search_history(const std::string &query, const std::string &region) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Parameterized (no string interpolation)
    const char *sql = "DELETE FROM search_cache WHERE query=? AND region=?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, region.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    LOG(fmt::format("[DB] Deleted search history: {} [{}]", query, region));
}

// Y23.1: per-source/per-account search cache (bind, no string interpolation).
void DatabaseManager::save_search_cache_src(const std::string &source, int account_id,
                                            const std::string &query,
                                            const std::string &result_json) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    int max_cache = IniConfig::instance().get_search_cache_max();
    sqlite3_stmt *stmt = nullptr;
    // INSERT OR REPLACE by (source, account_id, query) — there is no unique index on these, so do a
    //   manual check+update/insert to avoid duplicates (same as the O-mode path).
    const char *chk =
        "SELECT id FROM search_cache WHERE source=? AND account_id=? AND query=? LIMIT 1;";
    bool exists = false;
    if (sqlite3_prepare_v2(db_, chk, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, account_id);
        sqlite3_bind_text(stmt, 3, query.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            exists = true;
        sqlite3_finalize(stmt);
    }
    const char *sql = exists ? "UPDATE search_cache SET result_json=?, timestamp=CURRENT_TIMESTAMP "
                               "WHERE source=? AND account_id=? AND query=?;"
                             : "INSERT INTO search_cache (source, account_id, query, result_json) "
                               "VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (exists) {
            sqlite3_bind_text(stmt, 1, result_json.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, source.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, account_id);
            sqlite3_bind_text(stmt, 4, query.c_str(), -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_text(stmt, 1, source.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, account_id);
            sqlite3_bind_text(stmt, 3, query.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, result_json.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    // Bound the per-(source,account) history count (parameterized).
    {
        const char *cleanup =
            "DELETE FROM search_cache WHERE source=? AND account_id=? AND id NOT IN "
            "(SELECT id FROM search_cache WHERE source=? AND account_id=? ORDER BY timestamp DESC "
            "LIMIT ?);";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, cleanup, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, source.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, account_id);
            sqlite3_bind_text(stmt, 3, source.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 4, account_id);
            sqlite3_bind_int(stmt, 5, max_cache);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
}

std::vector<DatabaseManager::SearchHistoryItem>
DatabaseManager::load_search_history_src(const std::string &source, int account_id) {
    std::vector<SearchHistoryItem> out;
    if (!is_ready())
        return out;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "SELECT id, query, timestamp, result_json FROM search_cache "
                      "WHERE source=? AND account_id=? ORDER BY timestamp DESC;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, account_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SearchHistoryItem it;
            it.id = sqlite3_column_int(stmt, 0);
            const char *q = (const char *)sqlite3_column_text(stmt, 1);
            it.query = q ? q : "";
            it.region = source;
            const char *t = (const char *)sqlite3_column_text(stmt, 2);
            it.timestamp = t ? t : "";
            const char *j = (const char *)sqlite3_column_text(stmt, 3);
            it.result_count = 0;
            if (j) {
                try {
                    json jj = json::parse(j);
                    if (jj.contains("results") && jj["results"].is_array())
                        it.result_count = (int)jj["results"].size();
                } catch (...) {
                }
            }
            out.push_back(it);
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

std::string DatabaseManager::load_search_cache_src(const std::string &source, int account_id,
                                                   const std::string &query) {
    if (!is_ready())
        return "";
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *stmt = nullptr;
    std::string out;
    const char *sql =
        "SELECT result_json FROM search_cache WHERE source=? AND account_id=? AND query=? LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, account_id);
        sqlite3_bind_text(stmt, 3, query.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *j = (const char *)sqlite3_column_text(stmt, 0);
            if (j)
                out = j;
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

void DatabaseManager::delete_search_history_src(const std::string &source, int account_id,
                                                const std::string &query) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "DELETE FROM search_cache WHERE source=? AND account_id=? AND query=?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, account_id);
        sqlite3_bind_text(stmt, 3, query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    LOG(fmt::format("[DB] Deleted search history: {} [{}/{}]", query, source, account_id));
}

// Delete podcast cache
void DatabaseManager::delete_podcast_cache(const std::string &feed_url) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Parameterized (no string interpolation)
    const char *sql = "DELETE FROM podcast_cache WHERE feed_url=?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, feed_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    LOG(fmt::format("[DB] Deleted podcast cache: {}", feed_url));
}

// Delete episode cache (by feed_url)
void DatabaseManager::delete_episode_cache_by_feed(const std::string &feed_url) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Parameterized (no string interpolation)
    const char *sql = "DELETE FROM episode_cache WHERE feed_url=?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, feed_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    LOG(fmt::format("[DB] Deleted episode cache for feed: {}", feed_url));
}

// Podcast feed cache management
void DatabaseManager::save_podcast_cache(const std::string &feed_url, const std::string &title,
                                         const std::string &artist, const std::string &genre,
                                         int track_count, const std::string &artwork_url,
                                         int collection_id) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // F38: no data_json — columns are the single source. Parameterized (no string interpolation).
    const char *sql = "INSERT OR REPLACE INTO podcast_cache "
                      "(feed_url, title, artist, genre, track_count, artwork_url, collection_id) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, feed_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, artist.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, genre.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, track_count);
        sqlite3_bind_text(stmt, 6, artwork_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 7, collection_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// Check whether the podcast is cached
bool DatabaseManager::is_podcast_cached(const std::string &feed_url) {
    if (!is_ready())
        return false;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Parameterized (no string interpolation)
    const char *sql = "SELECT 1 FROM podcast_cache WHERE feed_url=? LIMIT 1;";

    sqlite3_stmt *stmt = nullptr;
    bool found = false;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, feed_url.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            found = true;
        }
        sqlite3_finalize(stmt);
    }
    return found;
}

// Save episode list cache
void DatabaseManager::save_episode_cache(const std::string &feed_url,
                                         const std::string &episodes_json) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Wrap in a transaction to avoid delete-then-insert-failure leaving the feed cache empty.
    //   Y03: only open our own txn if not already in one (see save_tree for rationale).
    bool own_txn = (db_ && sqlite3_get_autocommit(db_) != 0);
    if (own_txn)
        begin_txn();
    // Delete old cache for this feed first (parameterized)
    {
        const char *del_sql = "DELETE FROM episode_cache WHERE feed_url=?;";
        sqlite3_stmt *del_stmt = nullptr;
        if (sqlite3_prepare_v2(db_, del_sql, -1, &del_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(del_stmt, 1, feed_url.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(del_stmt);
            sqlite3_finalize(del_stmt);
        }
    }

    // Parse JSON and save each entry — prepare once, bind+step+reset per row (parameterized)
    try {
        json j = json::parse(episodes_json);
        if (j.is_array()) {
            const char *sql = "INSERT INTO episode_cache "
                              "(feed_url, episode_url, title, duration, pub_date, is_youtube, "
                              "has_subtitle, subtitle_url, has_asr_srt, asr_srt_path) "
                              "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
            sqlite3_stmt *stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                for (const auto &ep : j) {
                    std::string ep_url = ep.value("url", "");
                    std::string ep_title = ep.value("title", "");
                    int ep_duration = ep.value("duration", 0);
                    std::string ep_pub_date = ep.value("pubDate", "");
                    bool ep_is_youtube =
                        ep.value("is_youtube", false); // F38: column (was in data_json)
                    bool ep_has_sub = ep.value("has_subtitle", false);     // Y16: subtitle flag
                    std::string ep_sub_url = ep.value("subtitle_url", ""); // Y23.10: transcript URL
                    std::string ep_asr_path = ep.value("asr_srt_path", "");

                    sqlite3_bind_text(stmt, 1, feed_url.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, ep_url.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 3, ep_title.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(stmt, 4, ep_duration);
                    sqlite3_bind_text(stmt, 5, ep_pub_date.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(stmt, 6, ep_is_youtube ? 1 : 0);
                    sqlite3_bind_int(stmt, 7, ep_has_sub ? 1 : 0);
                    sqlite3_bind_text(stmt, 8, ep_sub_url.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(stmt, 9, ep.value("has_asr_srt", false) ? 1 : 0);
                    sqlite3_bind_text(stmt, 10, ep_asr_path.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                    sqlite3_reset(stmt);
                    sqlite3_clear_bindings(stmt);
                }
                sqlite3_finalize(stmt);
            }
        }
    } catch (const std::exception &e) {
        LOG(fmt::format("[Exception] {}", e.what()));
        if (own_txn)
            rollback_txn(); // Rollback on parse failure, otherwise DELETE commits while INSERT is empty -> cache cleared
        return;
    }
    if (own_txn)
        commit_txn();
}

// Check whether the episode list is cached
// Y24.22: persist 📜 marker after local ASR transcription — update episode_cache has_subtitle + subtitle_url.
void DatabaseManager::update_episode_subtitle(const std::string &feed_url,
                                              const std::string &episode_url, bool has_subtitle,
                                              const std::string &subtitle_url) {
    if (!is_ready() || feed_url.empty() || episode_url.empty())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "UPDATE episode_cache SET has_subtitle=?, subtitle_url=? WHERE "
                           "feed_url=? AND episode_url=?;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, has_subtitle ? 1 : 0);
        sqlite3_bind_text(stmt, 2, subtitle_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, feed_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, episode_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// Y24.23: persist ASR SRT marker to episode_cache.
void DatabaseManager::update_episode_asr(const std::string &feed_url,
                                         const std::string &episode_url,
                                         const std::string &asr_srt_path) {
    if (!is_ready() || feed_url.empty() || episode_url.empty())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "UPDATE episode_cache SET has_asr_srt=1, asr_srt_path=? WHERE "
                           "feed_url=? AND episode_url=?;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, asr_srt_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, feed_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, episode_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

bool DatabaseManager::is_episode_cached(const std::string &feed_url) {
    if (!is_ready())
        return false;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Parameterized (no string interpolation)
    const char *sql = "SELECT COUNT(*) FROM episode_cache WHERE feed_url=?;";

    sqlite3_stmt *stmt = nullptr;
    int count = 0;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, feed_url.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return count > 0;
}

// Load episode list cache (structured return)
// Used to load child nodes from the DB cache when expanding the favourites reference view
// F38: returns is_youtube column (was data_json parsed for is_youtube).
// Y23.10: also returns subtitle_url (6th element) so cached episodes can reload transcripts.
std::vector<std::tuple<std::string, std::string, int, bool, bool, std::string, bool, std::string>>
DatabaseManager::load_episodes_from_cache(const std::string &feed_url) {
    std::vector<
        std::tuple<std::string, std::string, int, bool, bool, std::string, bool, std::string>>
        episodes;
    if (!is_ready())
        return episodes;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Parameterized (no string interpolation)
    const char *sql =
        "SELECT episode_url, title, duration, is_youtube, has_subtitle, subtitle_url, has_asr_srt, "
        "asr_srt_path FROM episode_cache WHERE feed_url=? ORDER BY timestamp DESC;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, feed_url.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *ep_url = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            const char *ep_title = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            int ep_duration = sqlite3_column_int(stmt, 2);
            bool ep_is_youtube = sqlite3_column_int(stmt, 3) != 0;
            bool ep_has_sub = sqlite3_column_int(stmt, 4) != 0; // Y16: online transcript (📜)
            const char *sub_url =
                reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5)); // Y23.10
            bool ep_has_asr = sqlite3_column_int(stmt, 6) != 0; // Y24.25: ASR SRT (📝)
            const char *asr_path =
                reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7)); // Y24.25

            episodes.push_back({ep_url ? ep_url : "", ep_title ? ep_title : "", ep_duration,
                                ep_is_youtube, ep_has_sub, sub_url ? sub_url : "", ep_has_asr,
                                asr_path ? asr_path : ""});
        }
        sqlite3_finalize(stmt);
    }
    return episodes;
}

// ASR-fix (2026-08-15): single-episode transcript-metadata lookup by episode URL. episode_cache has
//   no lone index on episode_url (the unique index is (feed_url, episode_url)), so this is a scan —
//   fine at a few thousand rows / once per track begin. Tries the exact URL first, then the
//   query-stripped form (feeds sometimes append tracking params to the enclosure URL).
bool DatabaseManager::get_episode_transcript_meta(const std::string &episode_url,
                                                  bool &has_subtitle, std::string &subtitle_url,
                                                  bool &has_asr_srt, std::string &asr_srt_path) {
    has_subtitle = false;
    subtitle_url.clear();
    has_asr_srt = false;
    asr_srt_path.clear();
    if (!is_ready() || episode_url.empty())
        return false;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    auto lookup = [&](const std::string &key) -> bool {
        const char *sql =
            "SELECT has_subtitle, subtitle_url, has_asr_srt, asr_srt_path FROM episode_cache "
            "WHERE episode_url=? AND (has_subtitle=1 OR has_asr_srt=1) LIMIT 1;";
        sqlite3_stmt *stmt = nullptr;
        bool found = false;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                has_subtitle = sqlite3_column_int(stmt, 0) != 0;
                const char *su = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
                subtitle_url = su ? su : "";
                has_asr_srt = sqlite3_column_int(stmt, 2) != 0;
                const char *ap = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
                asr_srt_path = ap ? ap : "";
                found = true;
            }
            sqlite3_finalize(stmt);
        }
        return found;
    };
    if (lookup(episode_url))
        return true;
    size_t q = episode_url.find('?');
    if (q != std::string::npos)
        return lookup(episode_url.substr(0, q));
    return false;
}

// ── CACHE-1 (SCHEMA 49): per-mode list caches ────────────────────────────────
//   Every mode persists its L/ENTER expanded content to SQLite. data_json holds the serialized
//   child-node list; load returns "" on miss. INSERT OR REPLACE makes refresh re-writes idempotent
//   (no delete needed).

void DatabaseManager::save_bili_followings(int account_id, const std::string &uid,
                                           const std::string &data_json) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    const char *sql = "INSERT OR REPLACE INTO bilibili_follow_cache "
                      "(account_id, uid, data_json, updated_at) VALUES (?, ?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, account_id);
        sqlite3_bind_text(stmt, 2, uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, data_json.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, (sqlite3_int64)std::time(nullptr));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::string DatabaseManager::load_bili_followings(int account_id, const std::string &uid) {
    if (!is_ready())
        return "";
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::string out;
    const char *sql =
        "SELECT data_json FROM bilibili_follow_cache WHERE account_id=? AND uid=? LIMIT 1;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, account_id);
        sqlite3_bind_text(stmt, 2, uid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *j = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            if (j)
                out = j;
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

void DatabaseManager::save_bili_history(int account_id, const std::string &uid,
                                        const std::string &data_json) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    const char *sql = "INSERT OR REPLACE INTO bilibili_history_cache "
                      "(account_id, uid, data_json, updated_at) VALUES (?, ?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, account_id);
        sqlite3_bind_text(stmt, 2, uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, data_json.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, (sqlite3_int64)std::time(nullptr));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::string DatabaseManager::load_bili_history(int account_id, const std::string &uid) {
    if (!is_ready())
        return "";
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::string out;
    const char *sql =
        "SELECT data_json FROM bilibili_history_cache WHERE account_id=? AND uid=? LIMIT 1;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, account_id);
        sqlite3_bind_text(stmt, 2, uid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *j = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            if (j)
                out = j;
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

void DatabaseManager::save_iptv_cache(const std::string &url, const std::string &data_json) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    const char *sql = "INSERT OR REPLACE INTO iptv_cache (url, data_json, updated_at) "
                      "VALUES (?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, data_json.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)std::time(nullptr));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::string DatabaseManager::load_iptv_cache(const std::string &url) {
    if (!is_ready())
        return "";
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::string out;
    const char *sql = "SELECT data_json FROM iptv_cache WHERE url=? LIMIT 1;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *j = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            if (j)
                out = j;
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

int64_t DatabaseManager::iptv_cache_updated_at(const std::string &url) {
    if (!is_ready())
        return 0;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    int64_t out = 0;
    const char *sql = "SELECT updated_at FROM iptv_cache WHERE url=? LIMIT 1;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            out = (int64_t)sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return out;
}

void DatabaseManager::save_local_folder_cache(const std::string &path,
                                              const std::string &data_json) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    const char *sql = "INSERT OR REPLACE INTO local_folder_cache (path, data_json, updated_at) "
                      "VALUES (?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, data_json.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)std::time(nullptr));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::string DatabaseManager::load_local_folder_cache(const std::string &path) {
    if (!is_ready())
        return "";
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::string out;
    const char *sql = "SELECT data_json FROM local_folder_cache WHERE path=? LIMIT 1;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *j = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            if (j)
                out = j;
        }
        sqlite3_finalize(stmt);
    }
    return out;
}
} // namespace panicast
