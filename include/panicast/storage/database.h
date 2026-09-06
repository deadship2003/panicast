// SQLite3 database manager (singleton) + sqlite3_stmt RAII wrapper.
// Handles table creation and persistence of progress/history/favourites/subscriptions/search cache.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <functional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <sqlite3.h>

#include "panicast/core/types.h"

namespace panicast
{

// sqlite3_stmt RAII wrapper: prepare on construct, finalize on destruct, non-copyable.
class StmtRAII {
public:
    sqlite3_stmt *stmt;
    explicit StmtRAII(sqlite3 *db, const char *sql) : stmt(nullptr) {
        sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    }
    ~StmtRAII() {
        if (stmt)
            sqlite3_finalize(stmt);
    }
    StmtRAII(const StmtRAII &) = delete;
    StmtRAII &operator=(const StmtRAII &) = delete;
    operator bool() const {
        return stmt != nullptr;
    }
    operator sqlite3_stmt *() const {
        return stmt;
    }
};

// Y24.27: account structs (were anonymous in app_bilibili.cpp / app_tiktok.cpp).
struct BilibiliAccount {
    int id = 0;
    std::string uid, uname, sessdata, bili_jct, dedeuserid;
};
struct TiktokAccount {
    int id = 0;
    std::string platform, handle, url, uname;
};

// SQLite3 database manager - singleton pattern, auto-cleans history
class DatabaseManager {
public:
    static DatabaseManager &instance();

    bool init();
    ~DatabaseManager();
    // Y15: expose raw sqlite3 handle for B-mode account CRUD (bilibili_accounts table).
    sqlite3 *raw_db() {
        return db_;
    }

    // Clean up expired history records (uses INI config)
    void cleanup_old_history();

    // Progress management
    void save_progress(const std::string &url, double position, bool completed);
    std::pair<double, bool> get_progress(const std::string &url);

    // History records
    void add_history(const std::string &url, const std::string &title, int duration,
                     const std::string &artist = "", const std::string &album = "",
                     const std::string &art_url = ""); // META-3
    // tuple: url, title, timestamp, media_type, artist, album, art_url (META-3 tail)
    std::vector<std::tuple<std::string, std::string, std::string, int, std::string,
                           std::string, std::string>>
    get_history(int limit = 50);

    // ── Unified tree (tree_nodes table, recursive parent_id, root_type discriminator) ──
    // F38: replaces the old nodes (nested-JSON children) + radio_cache (recursive) tables.
    //   root_type ∈ {"podcast","radio"}. TreeNode in-memory model unchanged.
    //   Node state (expanded/children_loaded/is_youtube/channel_name/is_cached) is stored as
    //   columns; is_downloaded/local_file are NOT stored (use media_cache, single source of truth).
    //   children are recursive parent_id rows (no data_json).
    void save_tree(const std::string &root_type, const std::vector<TreeNodePtr> &top_nodes);
    void load_tree(const std::string &root_type, const TreeNodePtr &parent);

    // Clear cache tables only, preserve user data
    void purge_cache_only();

    // Search cache management (uses INI config)
    void save_search_cache(const std::string &query, const std::string &region,
                           const std::string &result_json);

    // Load all search history (for Online mode display)
    struct SearchHistoryItem {
        int id;
        std::string query;
        std::string region;
        std::string timestamp;
        int result_count;
    };

    std::vector<SearchHistoryItem> load_all_search_history();

    // Get full data of a single search cache
    std::string load_search_cache(const std::string &query, const std::string &region);

    // Y23.1: per-source/per-account search cache (Y/B mode mirror of O-mode). source ∈ {youtube,bilibili}.
    void save_search_cache_src(const std::string &source, int account_id, const std::string &query,
                               const std::string &result_json);
    std::vector<SearchHistoryItem> load_search_history_src(const std::string &source,
                                                           int account_id);
    std::string load_search_cache_src(const std::string &source, int account_id,
                                      const std::string &query);
    void delete_search_history_src(const std::string &source, int account_id,
                                   const std::string &query);

    // Favourites management (F38: link/local-folder metadata as columns, no data_json)
    void save_favourite(const std::string &title, const std::string &url, int type,
                        bool is_youtube = false, const std::string &channel_name = "",
                        const std::string &source_type = "", bool is_link = false,
                        const std::string &link_target_url = "", bool is_local_folder = false,
                        const std::string &art_url = "", const std::string &artist = "",
                        const std::string &album = ""); // META-3
    // tuple: title,url,type,is_youtube,channel_name,source_type,is_link,link_target_url,
    //   is_local_folder,media_type,art_url,artist,album (META-3 tail)
    std::vector<std::tuple<std::string, std::string, int, bool, std::string, std::string, bool,
                           std::string, bool, int, std::string, std::string, std::string>>
    load_favourites();
    void delete_favourite(const std::string &url);

    // Favourites sync step: DELETE rows so save_data can rewrite the full set in one txn (sync, not drop)
    void clear_favourites();

    // Delete a specific search history record
    void delete_search_history(const std::string &query, const std::string &region);
    // Delete a specific playback history record
    void delete_history(const std::string &url);
    // Delete podcast cache
    void delete_podcast_cache(const std::string &feed_url);
    // Delete episode cache (by feed_url)
    void delete_episode_cache_by_feed(const std::string &feed_url);

    // ═════════════════════════════════════════════════════════════════════════
    // Media cache: per-URL local-media cache status (single status column).
    //   status: 0 = no cache, 1 = complete download, 2 = partial download (.part)
    //   (the "feed parsed" concept is NOT here — it lives in episode_cache / is_episode_cached)
    // ═════════════════════════════════════════════════════════════════════════
    struct MediaCacheRow {
        std::string url;
        int status = 0; // 0=none, 1=complete, 2=partial
        std::string local_file;
    };
    // Load all media-cache rows (status + local_file)
    std::vector<MediaCacheRow> load_media_cache();
    // Set a URL's cache status (0=none/delete row, 1=complete, 2=partial); local_file for status 1.
    void media_cache_set(const std::string &url, int status, const std::string &local_file);
    // Purge all media-cache rows (--purge)
    void clear_media_cache();

    // ── YouTube channel cache (youtube_cache table) ──
    // Single-connection access via DatabaseManager (replaces the second sqlite connection that
    //   YouTubeCache used to open on its own, which raced with the main connection).
    // Y01: account_id scopes the row (0 = legacy/global anonymous browse; >0 = bound to a Google
    //   account, so different accounts' channel listings don't leak across each other).
    // Returns true if a row was found; fills out_name/out_videos_json.
    bool youtube_cache_load(const std::string &channel_url, std::string &out_name,
                            std::string &out_videos_json, int account_id = 0);
    void youtube_cache_save(const std::string &channel_url, const std::string &channel_name,
                            const std::string &videos_json, int account_id = 0);

    // Podcast feed cache management (F38: no data_json — columns are the single source)
    void save_podcast_cache(const std::string &feed_url, const std::string &title,
                            const std::string &artist, const std::string &genre, int track_count,
                            const std::string &artwork_url, int collection_id);
    bool is_podcast_cached(const std::string &feed_url);

    // Save episode list cache (F38: is_youtube extracted per-episode into a column, no data_json)
    void save_episode_cache(const std::string &feed_url, const std::string &episodes_json);
    bool is_episode_cached(const std::string &feed_url);
    // Load episode list cache. tuple: episode_url, title, duration, is_youtube, has_subtitle, subtitle_url
    // Y24.25: tuple: episode_url, title, duration, is_youtube, has_subtitle, subtitle_url, has_asr_srt, asr_srt_path
    std::vector<
        std::tuple<std::string, std::string, int, bool, bool, std::string, bool, std::string>>
    load_episodes_from_cache(const std::string &feed_url);
    // Y24.22: persist 📜 marker after local ASR transcription — update episode_cache has_subtitle + subtitle_url.
    void update_episode_subtitle(const std::string &feed_url, const std::string &episode_url,
                                 bool has_subtitle, const std::string &subtitle_url);
    // Y24.23: persist ASR SRT marker (has_asr_srt + asr_srt_path) to episode_cache.
    void update_episode_asr(const std::string &feed_url, const std::string &episode_url,
                            const std::string &asr_srt_path);
    // ASR-fix (2026-08-15): look up one episode's persisted transcript metadata by episode URL
    //   (no feed_url needed — synthetic playback nodes only know their own URL). Returns false
    //   when the row is missing or carries no transcript. Used by SubtitleService::begin_track to
    //   reattach 📜/📝 info to history/search nodes that lost it at construction.
    bool get_episode_transcript_meta(const std::string &episode_url, bool &has_subtitle,
                                     std::string &subtitle_url, bool &has_asr_srt,
                                     std::string &asr_srt_path);

    // Y24.27: bilibili/tiktok account CRUD — moved from app_bilibili.cpp / app_tiktok.cpp
    //   (was direct sqlite3_* calls bypassing DatabaseManager).
    std::vector<BilibiliAccount> list_bilibili_accounts();
    int upsert_bilibili_account(const BilibiliAccount &a); // returns id
    bool delete_bilibili_account(int id);
    std::vector<TiktokAccount> list_tiktok_accounts();
    int upsert_tiktok_account(const TiktokAccount &a); // returns id
    bool delete_tiktok_account(int id);

    // Y23: bilibili UP-master logo cache (avatar URL stored for future remote/sixel display).
    void save_bili_up(const std::string &mid, const std::string &uname, const std::string &upic,
                      int fans, const std::string &sign);
    std::string load_bili_up_pic(const std::string &mid); // empty if not cached

    // ── CACHE-1 (SCHEMA 49): per-mode list caches — every mode persists its L/ENTER expanded
    //   content to SQLite. data_json holds the serialized child nodes; load returns "" on miss.
    // B mode: UP followings list / watch-history list (keyed by the account's uid).
    void save_bili_followings(int account_id, const std::string &uid, const std::string &data_json);
    std::string load_bili_followings(int account_id, const std::string &uid);
    void save_bili_history(int account_id, const std::string &uid, const std::string &data_json);
    std::string load_bili_history(int account_id, const std::string &uid);
    // I mode: parsed iptv-org channel tree (TTL via updated_at, honoring [iptv] cache_hours).
    void save_iptv_cache(const std::string &url, const std::string &data_json);
    std::string load_iptv_cache(const std::string &url);
    int64_t iptv_cache_updated_at(const std::string &url); // unix seconds, 0 if absent
    // F mode: local-directory scan result.
    void save_local_folder_cache(const std::string &path, const std::string &data_json);
    std::string load_local_folder_cache(const std::string &path);

    // Extended player state save
    void save_player_state(int volume, double speed, bool paused, const std::string &url = "",
                           double position = 0, bool scroll_mode = false,
                           bool show_tree_lines = true, const std::string &current_title = "",
                           int current_mode = 0);

    // Extended state data
    struct PlayerStateData {
        int volume = 100;
        double speed = 1.0;
        bool paused = false;
        std::string current_url;
        double position = 0;
        bool scroll_mode = false;
        bool show_tree_lines = true;
        std::string current_title;
        int current_mode = 0;
    };

    PlayerStateData load_player_state();

    // Public is_ready method for external callers
    bool is_ready() const;

    // Y01: raw accessors for AccountsManager — it shares DatabaseManager's single sqlite connection
    //   (opening a second connection raced with the main one, per F38). AccountsManager owns all
    //   account / youtube_subscriptions / youtube_history / account_sync_state SQL; it must hold
    //   raw_mutex() around every raw_handle() statement.
    sqlite3 *raw_handle() {
        return db_;
    }
    std::recursive_mutex &raw_mutex() {
        return mtx_;
    }
    void run_locked(const std::function<void()> &
                        fn); // Y23.5: hold mtx_ (recursive) for a transaction-like critical section

    // Transaction helpers - ensure atomicity of "delete-then-insert" operations, preventing data loss on mid-failure
    bool begin_txn();
    bool commit_txn();
    bool rollback_txn();

    // Built-in default podcast records that have been removed
    void add_removed_default(const std::string &url);
    std::set<std::string> load_removed_defaults();

private:
    // F38: recursively save/load tree_nodes (unified, root_type-discriminated)
    void save_tree_node_recursive(const TreeNodePtr &node, const std::string &root_type,
                                  int parent_id, int &order);
    void load_tree_node_recursive(const TreeNodePtr &parent, const std::string &root_type,
                                  int parent_id);

    DatabaseManager();
    std::atomic<bool> initialized_{
        false}; // Singleton init flag (atomic, for double-checked locking)
    sqlite3 *db_ = nullptr;
    std::recursive_mutex mtx_;

    bool exec_sql(const std::string &sql);
};

} // namespace panicast
