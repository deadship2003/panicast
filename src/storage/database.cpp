#include "panicast/storage/database.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <sys/stat.h> // P1.4: chmod(0600) on the DB file

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "panicast/config/ini_config.h"
#include "panicast/core/logger.h"
#include "panicast/core/paths.h"
#include "panicast/net/url_classifier.h" // N06: classifyMediaType for media_type backfill

namespace panicast
{

namespace fs = std::filesystem;
using json = nlohmann::json;

DatabaseManager &DatabaseManager::instance() {
    static DatabaseManager db;
    return db;
}

DatabaseManager::DatabaseManager() : initialized_(false), db_(nullptr) {}

bool DatabaseManager::init() {
    // Fast path (lock-free) + double-checked locking; ensures one-time, reentrant init under multithreading
    if (initialized_.load())
        return true;
    static std::mutex init_mtx;
    std::lock_guard<std::mutex> lk(init_mtx);
    if (initialized_.load())
        return true;

    std::string db_path = Paths::get_db_file();
    if (db_path.empty())
        return false;

    fs::create_directories(fs::path(db_path).parent_path());

    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        LOG(fmt::format("[DB] Failed to open: {}", db_ ? sqlite3_errmsg(db_) : "null handle"));
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        } // close even on failure
        return false;
    }

    // P1.4 (Y23.4): restrict DB file to owner-only (0600). The DB holds plaintext history,
    //   favourites, subscriptions, search history, and player state; the 0644 default (from umask)
    //   let other local users read it. Also makes the encrypted-token guarantee meaningful.
    ::chmod(db_path.c_str(), 0600);

    // Enable WAL mode to allow concurrent read/write (download thread writes don't block UI reads)
    char *err = nullptr;
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err);
    if (err) {
        LOG(fmt::format("[DB] WAL pragma: {}", err));
        sqlite3_free(err);
    }
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr); // 5s lock wait
    LOG("[DB] WAL mode enabled, synchronous=NORMAL");

    // F38: schema version via PRAGMA user_version. On mismatch, drop the tables whose schema
    //   changed (no row-level migration — historical data is not important by design). The
    //   CREATE TABLE IF NOT EXISTS below then rebuilds them with the new schema.
    // Y01: SCHEMA_VERSION 38 -> 39. Adds the Y-mode account tables (accounts / youtube_subscriptions
    //   / youtube_history / account_sync_state) and an account_id column on youtube_cache (per-account
    //   isolation). Existing F42 tables are untouched (IF NOT EXISTS keeps them); the new tables are
    //   additive. youtube_cache is dropped+rebuilt only if its old schema lacks account_id.
    // Y03: SCHEMA_VERSION 39 -> 40. history.url was created as plain TEXT by older builds; the
    //   CREATE TABLE IF NOT EXISTS never migrated it, so add_history's ON CONFLICT(url) UPSERT failed
    //   with "ON CONFLICT clause does not match any PRIMARY KEY or UNIQUE constraint" and silently
    //   accumulated duplicate-url rows. Migration: dedupe (keep newest id per url) + add a UNIQUE
    //   INDEX on url so the UPSERT resolves. F42 user data otherwise preserved.
    //   Y08: SCHEMA_VERSION 40 -> 41. `is_youtube` (and several favourites columns) were added to the
    //   CREATE TABLE without a version bump, so old DBs already at v40 kept the old episode_cache/
    //   favourites schema → "table episode_cache has no column named is_youtube" → YouTube cache
    //   writes failed. Migration: idempotent ALTER TABLE ADD COLUMN for missing columns (after CREATE
    //   TABLE below); youtube_cache dropped+rebuilt (account_id). User data preserved.
    //   Y23.10: SCHEMA_VERSION 43 -> 44. episode_cache gains a `subtitle_url` column (RSS
    //   <podcast:transcript> URL) so cached episodes can reload transcripts on replay, not just the
    //   has_subtitle flag. Idempotent ALTER TABLE ADD COLUMN (below); episode rows preserved.
    //   Y24.11: SCHEMA_VERSION 44 -> 45. tiktok_accounts table for T-mode subscribed creators
    //   (TikTok/Douyin). CREATE TABLE IF NOT EXISTS (safe on existing DBs); no user data touched.
    //   N05: SCHEMA_VERSION 46 -> 47. history/favourites gain a `media_type` INTEGER column
    //   (DB-stored display category; replaces runtime URL→icon inference). Legacy rows are
    //   backfilled from their URL via classifyMediaType (see backfill below).
    //   D14-4: SCHEMA_VERSION 47 -> 48. progress/player_state current_url re-keyed from the played
    //     cache path to the canonical source URL (identity convergence w/ history/remote/UI).
    //   CACHE-1: SCHEMA_VERSION 48 -> 49. Four per-mode list-cache tables so every mode persists its
    //     L/ENTER expanded content to SQLite (bilibili_follow_cache / bilibili_history_cache /
    //     iptv_cache / local_folder_cache). All CREATE TABLE IF NOT EXISTS — additive, user data kept.
    //   ART-1: SCHEMA_VERSION 49 -> 50. tree_nodes gains `art_url` + `subtext` columns so the
    //     remote (Squeeze Client) browse rows and now-playing artwork survive a restart.
    //     Idempotent ALTER TABLE ADD COLUMN below; tree data preserved.
    //   META-1: SCHEMA_VERSION 50 -> 51. tree_nodes gains artist/album/track_num —
    //     structured LMS-standard display metadata for the remote (Squeeze Client)
    //     rows and now-playing. Idempotent ALTER TABLE ADD COLUMN; tree data kept.
    constexpr int SCHEMA_VERSION = 51;
    int stored_version = 0;
    {
        sqlite3_stmt *sv = nullptr;
        if (sqlite3_prepare_v2(db_, "PRAGMA user_version;", -1, &sv, nullptr) == SQLITE_OK) {
            if (sqlite3_step(sv) == SQLITE_ROW)
                stored_version = sqlite3_column_int(sv, 0);
            sqlite3_finalize(sv);
        }
    }
    if (stored_version != SCHEMA_VERSION) {
        // Y03 (39 -> 40): ensure history.url has a UNIQUE constraint (see comment above). Runs for
        //   any version mismatch; idempotent. Dedupe first so CREATE UNIQUE INDEX can't fail on dups.
        if (stored_version > 0) { // skip on a fresh DB (no rows yet)
            sqlite3_exec(
                db_,
                "DELETE FROM history WHERE id NOT IN (SELECT MAX(id) FROM history GROUP BY url);",
                nullptr, nullptr, nullptr);
        }
        sqlite3_exec(db_, "CREATE UNIQUE INDEX IF NOT EXISTS idx_history_url ON history(url);",
                     nullptr, nullptr, nullptr);
        LOG(fmt::format("[DB] Schema version {} -> {}: history.url UNIQUE index; youtube_cache "
                        "rebuilt (account_id); "
                        "F42 user data preserved",
                        stored_version, SCHEMA_VERSION));
        sqlite3_exec(db_, "DROP TABLE IF EXISTS youtube_cache;", nullptr, nullptr, nullptr);
    }

    // Create tables
    const char *create_tables = R"(
        -- F38: unified recursive tree (replaces nodes + radio_cache). root_type ∈ {podcast,radio}.
        --   children via parent_id recursion; node state as columns; no data_json.
        --   is_downloaded/local_file NOT here — use media_cache (single source of truth).
        CREATE TABLE IF NOT EXISTS tree_nodes (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            root_type TEXT NOT NULL,
            parent_id INTEGER DEFAULT 0,
            title TEXT,
            url TEXT,
            type INTEGER,
            expanded INTEGER DEFAULT 0,
            children_loaded INTEGER DEFAULT 0,
            is_youtube INTEGER DEFAULT 0,
            has_subtitle INTEGER DEFAULT 0,
            channel_name TEXT,
            is_cached INTEGER DEFAULT 0,
            sort_order INTEGER DEFAULT 0,
            art_url TEXT,     -- ART-1: remote browse / now-playing artwork (station logo, podcast cover)
            subtext TEXT,     -- ART-1: second display line for the remote browse rows
            artist TEXT,      -- META-1: LMS-standard display metadata
            album TEXT,
            track_num INTEGER DEFAULT 0,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS progress (
            url TEXT PRIMARY KEY,
            position REAL DEFAULT 0,
            completed INTEGER DEFAULT 0,
            last_played TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            url TEXT UNIQUE,
            title TEXT,
            timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            duration INTEGER DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS stats (
            key TEXT PRIMARY KEY,
            value TEXT
        );
        -- Media cache: per-URL local-media cache status. 0=none, 1=complete, 2=partial(.part)
        CREATE TABLE IF NOT EXISTS media_cache (
            url TEXT PRIMARY KEY,
            status INTEGER NOT NULL DEFAULT 0,
            local_file TEXT,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS search_cache (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            query TEXT NOT NULL,
            region TEXT DEFAULT 'US',
            result_json TEXT,
            timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS player_state (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            volume INTEGER DEFAULT 100,
            speed REAL DEFAULT 1.0,
            paused INTEGER DEFAULT 0,
            current_url TEXT,
            position REAL DEFAULT 0,
            scroll_mode INTEGER DEFAULT 0,
            show_tree_lines INTEGER DEFAULT 1,
            current_title TEXT,
            current_mode INTEGER DEFAULT 0,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS podcast_cache (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            feed_url TEXT UNIQUE NOT NULL,
            title TEXT,
            artist TEXT,
            genre TEXT,
            track_count INTEGER DEFAULT 0,
            artwork_url TEXT,
            collection_id INTEGER,
            timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS episode_cache (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            feed_url TEXT NOT NULL,
            episode_url TEXT,
            title TEXT,
            duration INTEGER DEFAULT 0,
            pub_date TEXT,
            is_youtube INTEGER DEFAULT 0,
            has_subtitle INTEGER DEFAULT 0,
            subtitle_url TEXT,                          -- Y23.10: RSS <podcast:transcript> URL (empty = none)
            has_asr_srt INTEGER DEFAULT 0,     -- Y24.23: local ASR SRT (📝)
            asr_srt_path TEXT,                -- Y24.23: path to local ASR SRT
            timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
        -- DB-12: prevent duplicate (feed, episode) rows. episode_url alone is NOT unique — the same
        --   episode/video can legitimately appear under multiple feeds (syndication, multi-sub).
        --   The correct key is (feed_url, episode_url). Partial index (non-empty episode_url); dedupe first.
        DELETE FROM episode_cache WHERE episode_url IS NOT NULL AND episode_url != ''
            AND id NOT IN (SELECT MIN(id) FROM episode_cache
                           WHERE episode_url IS NOT NULL AND episode_url != ''
                           GROUP BY feed_url, episode_url);
        CREATE UNIQUE INDEX IF NOT EXISTS idx_episode_feed_url ON episode_cache(feed_url, episode_url)
            WHERE episode_url IS NOT NULL AND episode_url != '';
        CREATE TABLE IF NOT EXISTS favourites (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT,
            url TEXT UNIQUE,
            type INTEGER DEFAULT 0,
            is_youtube INTEGER DEFAULT 0,
            has_subtitle INTEGER DEFAULT 0,
            channel_name TEXT,
            source_type TEXT,
            is_link INTEGER DEFAULT 0,
            link_target_url TEXT,
            is_local_folder INTEGER DEFAULT 0,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS youtube_cache (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            account_id INTEGER NOT NULL DEFAULT 0,   -- Y01: 0 = legacy/global; >0 = bound to a Google account
            channel_url TEXT NOT NULL,
            channel_name TEXT,
            videos_json TEXT,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
        CREATE INDEX IF NOT EXISTS idx_tree_parent ON tree_nodes(root_type, parent_id);
        CREATE INDEX IF NOT EXISTS idx_tree_url ON tree_nodes(url);
        CREATE INDEX IF NOT EXISTS idx_history_timestamp ON history(timestamp);
        CREATE INDEX IF NOT EXISTS idx_search_query ON search_cache(query, region);
        CREATE INDEX IF NOT EXISTS idx_podcast_feed ON podcast_cache(feed_url);
        CREATE INDEX IF NOT EXISTS idx_episode_feed ON episode_cache(feed_url);
        -- Review-fix (2026-08-16): lone index on episode_url. get_episode_transcript_meta looks an
        --   episode up by episode_url ALONE (synthetic history/search nodes carry no feed_url) and
        --   runs on EVERY begin_track — the only covering index was (feed_url, episode_url), so each
        --   lookup was a full-table scan (twice when the URL has a query string), on the UI thread.
        --   Predicate is IS NOT NULL ONLY: SQLite's partial-index implication check can prove
        --   `episode_url=? ⟹ IS NOT NULL` but NOT `⟹ != ''` (EXPLAIN QUERY PLAN verified — with the
        --   != '' term the planner fell back to SCAN even for a literal), so the leaner predicate is
        --   what actually makes the query SEARCH this index.
        CREATE INDEX IF NOT EXISTS idx_episode_url ON episode_cache(episode_url)
            WHERE episode_url IS NOT NULL;
        CREATE INDEX IF NOT EXISTS idx_favourites_url ON favourites(url);
        CREATE TABLE IF NOT EXISTS bilibili_accounts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uid TEXT NOT NULL,
            uname TEXT,
            sessdata TEXT NOT NULL,
            bili_jct TEXT,
            dedeuserid TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            last_login_at TIMESTAMP
        );
        -- DB-11: uid UNIQUE. Dedupe legacy duplicates first (keep oldest id per uid) so the unique
        --   index cannot fail; the app also upserts on uid, so no new duplicates are created.
        DELETE FROM bilibili_accounts WHERE id NOT IN (SELECT MIN(id) FROM bilibili_accounts GROUP BY uid);
        CREATE UNIQUE INDEX IF NOT EXISTS idx_bilibili_uid ON bilibili_accounts(uid);
        -- Y23: bilibili UP-master logo cache (avatar/cover URL stored for display by a future remote/sixel client).
        CREATE TABLE IF NOT EXISTS bilibili_up_cache (
            mid TEXT PRIMARY KEY,
            uname TEXT,
            upic TEXT,
            fans INTEGER DEFAULT 0,
            sign TEXT,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
        -- P2 (Y23.4): youtube_cache UNIQUE(account_id, channel_url). Dedup legacy duplicates first
        --   (keep newest by id) so the unique index can't fail; INSERT OR REPLACE now actually replaces.
        DELETE FROM youtube_cache WHERE id NOT IN (SELECT MAX(id) FROM youtube_cache GROUP BY account_id, channel_url);
        CREATE UNIQUE INDEX IF NOT EXISTS idx_youtube_cache_url ON youtube_cache(account_id, channel_url);
        -- Records built-in default podcast URLs deleted by the user, so load_default_podcasts won't re-add them on restart
        CREATE TABLE IF NOT EXISTS removed_defaults (
            url TEXT PRIMARY KEY
        );
        -- Y24.11: T-mode subscribed creators (TikTok/Douyin). platform='tiktok'|'douyin'.
        CREATE TABLE IF NOT EXISTS tiktok_accounts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            platform TEXT NOT NULL,
            handle TEXT NOT NULL,
            url TEXT NOT NULL,
            uname TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
        DELETE FROM tiktok_accounts WHERE id NOT IN (SELECT MIN(id) FROM tiktok_accounts GROUP BY platform, handle);
        CREATE UNIQUE INDEX IF NOT EXISTS idx_tiktok_handle ON tiktok_accounts(platform, handle);

        -- ============================================================
        -- Y01: Google account tables (multi-account, per-account isolation)
        --   Existing F42 data stays global; only YouTube-side data is per-account here.
        -- ============================================================

        -- Google accounts registered via Y-mode OAuth login. account_id=1 is NEVER a "local" account
        --   (Y01 has no local account) — rows here are always real Google accounts.
        --   Tokens are stored encrypted (see core/crypto.cpp + AccountsManager); the *_enc columns
        --   hold ciphertext. is_active=1 marks the currently active account (at most one).
        CREATE TABLE IF NOT EXISTS accounts (
            account_id      INTEGER PRIMARY KEY AUTOINCREMENT,
            type            TEXT    NOT NULL DEFAULT 'google',
            label           TEXT,
            google_email    TEXT,
            gaia_id         TEXT,
            channel_id      TEXT,
            access_token_enc  TEXT,
            refresh_token_enc TEXT,
            token_expires_at INTEGER,                 -- unix epoch (UTC) when access_token expires
            token_scope     TEXT,
            created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            last_login_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            last_sync_at    INTEGER DEFAULT 0,
            is_active       INTEGER NOT NULL DEFAULT 0
        );

        -- YouTube subscription list per account (synced via Data API subscriptions.list).
        CREATE TABLE IF NOT EXISTS youtube_subscriptions (
            account_id        INTEGER NOT NULL,
            channel_id        TEXT NOT NULL,
            channel_name      TEXT,
            channel_url       TEXT,
            subscription_order INTEGER DEFAULT 0,
            synced_at         TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (account_id, channel_id)
        );
        CREATE INDEX IF NOT EXISTS idx_yt_subs_account ON youtube_subscriptions(account_id);

        -- YouTube watch history per account. Pulled via authenticated InnerTube /browse (history);
        --   local plays under this account are also upserted here (source discriminates origin).
        CREATE TABLE IF NOT EXISTS youtube_history (
            account_id  INTEGER NOT NULL,
            video_id    TEXT NOT NULL,
            title       TEXT,
            channel_name TEXT,
            duration    INTEGER DEFAULT 0,
            watched_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            source      TEXT NOT NULL DEFAULT 'youtube',  -- 'youtube' (pulled) | 'local' (played in panicast)
            PRIMARY KEY (account_id, video_id)
        );
        CREATE INDEX IF NOT EXISTS idx_yt_hist_account_time ON youtube_history(account_id, watched_at);

        -- Per-account incremental sync state.
        CREATE TABLE IF NOT EXISTS account_sync_state (
            account_id   INTEGER NOT NULL,
            sync_type    TEXT NOT NULL,    -- 'subscriptions' | 'watch_history'
            last_sync_at INTEGER DEFAULT 0,
            sync_cursor  TEXT,
            PRIMARY KEY (account_id, sync_type)
        );

        -- CACHE-1 (SCHEMA 49): per-mode list caches so every mode persists L/ENTER expanded
        --   content to SQLite (not just the modes that already used episode_cache/search_cache).
        -- B mode: UP followings list (parsed WBI arc). key = account's uid.
        CREATE TABLE IF NOT EXISTS bilibili_follow_cache (
            account_id INTEGER NOT NULL,
            uid        TEXT    NOT NULL,
            data_json  TEXT    NOT NULL,
            updated_at INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (account_id, uid)
        );
        -- B mode: watch history list (parsed /x/v2/history). key = account's uid.
        CREATE TABLE IF NOT EXISTS bilibili_history_cache (
            account_id INTEGER NOT NULL,
            uid        TEXT    NOT NULL,
            data_json  TEXT    NOT NULL,
            updated_at INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (account_id, uid)
        );
        -- I mode: parsed iptv-org channel tree. TTL via updated_at (cache_hours).
        CREATE TABLE IF NOT EXISTS iptv_cache (
            url        TEXT PRIMARY KEY,
            data_json  TEXT    NOT NULL,
            updated_at INTEGER NOT NULL DEFAULT 0
        );
        -- F mode: local-directory scan result. path = absolute directory path.
        CREATE TABLE IF NOT EXISTS local_folder_cache (
            path       TEXT PRIMARY KEY,
            data_json  TEXT    NOT NULL,
            updated_at INTEGER NOT NULL DEFAULT 0
        );
    )";

    char *err_msg = nullptr;
    if (sqlite3_exec(db_, create_tables, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        LOG(fmt::format("[DB] Create tables error: {}", err_msg));
        sqlite3_free(err_msg);
        return false;
    }

    // Y08: idempotent column migration. CREATE TABLE IF NOT EXISTS does NOT add columns to an
    //   existing table, so an old DB (created before these columns existed) keeps the old schema
    //   and INSERTs fail with "no such column". Add any missing column via ALTER TABLE. Runs every
    //   init (cheap PRAGMA table_info check); safe on fresh DBs (columns already present).
    auto add_column_if_missing = [&](const char *table, const char *col, const char *decl) {
        sqlite3_stmt *st = nullptr;
        std::string q = fmt::format("PRAGMA table_info({});", table);
        bool found = false;
        if (sqlite3_prepare_v2(db_, q.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *name = reinterpret_cast<const char *>(sqlite3_column_text(st, 1));
                if (name && strcmp(name, col) == 0) {
                    found = true;
                    break;
                }
            }
            sqlite3_finalize(st);
        }
        if (!found) {
            std::string al = fmt::format("ALTER TABLE {} ADD COLUMN {} {};", table, col, decl);
            char *e = nullptr;
            if (sqlite3_exec(db_, al.c_str(), nullptr, nullptr, &e) == SQLITE_OK) {
                LOG(fmt::format("[DB] migrated {} + column {}", table, col));
            } else {
                LOG(fmt::format("[DB] ALTER {} ADD {} failed: {}", table, col, e ? e : "?"));
                sqlite3_free(e);
            }
        }
    };
    add_column_if_missing("episode_cache", "is_youtube", "INTEGER DEFAULT 0");
    add_column_if_missing("episode_cache", "has_subtitle", "INTEGER DEFAULT 0");
    add_column_if_missing("episode_cache", "subtitle_url", "TEXT"); // Y23.10
    add_column_if_missing("episode_cache", "has_asr_srt", "INTEGER DEFAULT 0");
    add_column_if_missing("episode_cache", "asr_srt_path", "TEXT");
    // ART-1 (49 -> 50): tree artwork + second-line text survive restarts.
    add_column_if_missing("tree_nodes", "art_url", "TEXT");
    add_column_if_missing("tree_nodes", "subtext", "TEXT");
    add_column_if_missing("tree_nodes", "artist", "TEXT");                 // META-1 (51)
    add_column_if_missing("tree_nodes", "album", "TEXT");                  // META-1 (51)
    add_column_if_missing("tree_nodes", "track_num", "INTEGER DEFAULT 0"); // META-1 (51)
    add_column_if_missing("favourites", "is_youtube", "INTEGER DEFAULT 0");
    add_column_if_missing("favourites", "channel_name", "TEXT");
    add_column_if_missing("favourites", "source_type", "TEXT");
    add_column_if_missing("favourites", "is_link", "INTEGER DEFAULT 0");
    add_column_if_missing("favourites", "link_target_url", "TEXT");
    add_column_if_missing("favourites", "is_local_folder", "INTEGER DEFAULT 0");
    // Y23.1: search_cache source + account_id — distinguish O-mode (itunes/0) from Y/B per-account
    //   searches (youtube/bilibili + account_id). Defaults preserve existing O-mode rows.
    add_column_if_missing("search_cache", "source", "TEXT DEFAULT 'itunes'");
    add_column_if_missing("search_cache", "account_id", "INTEGER DEFAULT 0");

    // N06: MediaType — DB-stored display category for history/favourites (replaces runtime
    //   URL→icon inference). INTEGER = (int)MediaType; default 0 = Radio.
    add_column_if_missing("history", "media_type", "INTEGER DEFAULT 0");
    add_column_if_missing("favourites", "media_type", "INTEGER DEFAULT 0");

    // N06: one-time backfill — compute media_type from the URL for legacy rows (pre-47 DBs had no
    //   such column → all default 0). New rows get the value at insert time. classifyMediaType is
    //   platform-priority (YouTube never Video, m3u8→IPTV, local vs online distinguished).
    if (stored_version > 0 && stored_version < 47) {
        auto backfill = [&](const char *table) {
            sqlite3_stmt *q = nullptr;
            std::string sel = fmt::format("SELECT rowid, url FROM {} WHERE media_type = 0;", table);
            if (sqlite3_prepare_v2(db_, sel.c_str(), -1, &q, nullptr) == SQLITE_OK) {
                sqlite3_stmt *upd = nullptr;
                std::string usql =
                    fmt::format("UPDATE {} SET media_type = ? WHERE rowid = ?;", table);
                sqlite3_prepare_v2(db_, usql.c_str(), -1, &upd, nullptr);
                int n = 0;
                while (upd && sqlite3_step(q) == SQLITE_ROW) {
                    sqlite3_int64 rowid = sqlite3_column_int64(q, 0);
                    const unsigned char *u = sqlite3_column_text(q, 1);
                    std::string url = u ? reinterpret_cast<const char *>(u) : "";
                    int mt = static_cast<int>(URLClassifier::classifyMediaType(url));
                    sqlite3_bind_int(upd, 1, mt);
                    sqlite3_bind_int64(upd, 2, rowid);
                    sqlite3_step(upd);
                    sqlite3_reset(upd);
                    sqlite3_clear_bindings(upd);
                    ++n;
                }
                if (upd)
                    sqlite3_finalize(upd);
                sqlite3_finalize(q);
                LOG(fmt::format("[DB] backfilled {} rows in {} with media_type", n, table));
            }
        };
        backfill("history");
        backfill("favourites");
    }

    // D14-4: progress/player_state identity convergence — re-key rows from the played cache path to
    //   the canonical source URL. Pre-D14-4 these tables stored mpv's current_url (the played path;
    //   for cached items a local cache path); D14-4 keys them on the source URL (aligning with
    //   history's orig_url, remote D14-2, UI D14-3). Reverse-map path→source via media_cache: only
    //   still-cached items can be re-keyed (a cleared cache has no resumable file anyway), so this
    //   is lossless for all recoverable data. Correlated UPDATE in SQL; runs once (gated < 48).
    if (stored_version > 0 && stored_version < 48) {
        const char *rekey_progress =
            "UPDATE progress "
            "SET url = (SELECT mc.url FROM media_cache mc WHERE mc.local_file = progress.url) "
            "WHERE url IN (SELECT local_file FROM media_cache WHERE local_file != '');";
        const char *rekey_player_state =
            "UPDATE player_state "
            "SET current_url = (SELECT mc.url FROM media_cache mc "
            "                   WHERE mc.local_file = player_state.current_url) "
            "WHERE current_url IN (SELECT local_file FROM media_cache WHERE local_file != '');";
        char *e = nullptr;
        if (sqlite3_exec(db_, rekey_progress, nullptr, nullptr, &e) == SQLITE_OK) {
            LOG(fmt::format("[DB] D14-4 re-keyed {} progress rows (cache path → source URL)",
                            sqlite3_changes(db_)));
        } else {
            LOG(fmt::format("[DB] D14-4 progress re-key failed: {}", e ? e : "?"));
            sqlite3_free(e);
        }
        e = nullptr;
        if (sqlite3_exec(db_, rekey_player_state, nullptr, nullptr, &e) == SQLITE_OK) {
            LOG(fmt::format("[DB] D14-4 re-keyed player_state (cache path → source URL)"));
        } else {
            LOG(fmt::format("[DB] D14-4 player_state re-key failed: {}", e ? e : "?"));
            sqlite3_free(e);
        }
    }

    // Record the schema version (PRAGMA user_version) so future schema changes can detect mismatch.
    sqlite3_exec(db_, fmt::format("PRAGMA user_version = {};", SCHEMA_VERSION).c_str(), nullptr,
                 nullptr, nullptr);

    initialized_ = true;
    LOG("[DB] Database initialized successfully");

    // Auto-clean expired history
    cleanup_old_history();

    return true;
}

DatabaseManager::~DatabaseManager() {
    if (db_)
        sqlite3_close_v2(db_); // close_v2 schedules final teardown even with outstanding statements
}

// Public is_ready method for external callers
bool DatabaseManager::is_ready() const {
    return initialized_ && db_ != nullptr;
}

// Transaction helpers — ensure atomicity of "delete then insert" style operations, avoiding data loss on midway failure
bool DatabaseManager::begin_txn() {
    return exec_sql("BEGIN;");
}
bool DatabaseManager::commit_txn() {
    return exec_sql("COMMIT;");
}
bool DatabaseManager::rollback_txn() {
    return exec_sql("ROLLBACK;");
}

bool DatabaseManager::exec_sql(const std::string &sql) {
    char *err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        LOG(fmt::format("[DB] SQL error: {}", err_msg ? err_msg : "unknown"));
        if (err_msg)
            sqlite3_free(err_msg);
        return false;
    }
    return true;
}

void DatabaseManager::run_locked(const std::function<void()> &fn) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    fn();
}
} // namespace panicast
