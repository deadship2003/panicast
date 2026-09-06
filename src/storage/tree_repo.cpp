// Tree / favourites / media-cache repository (tree_nodes, favourites, media_cache).
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
#include "panicast/net/url_classifier.h" // N06: classifyMediaType for media_type
#include "panicast/core/logger.h"
#include "panicast/core/paths.h"

namespace panicast
{

namespace fs = std::filesystem;
using json = nlohmann::json;

// F38: unified tree save (replaces save_node + save_radio_cache). root_type ∈ {"podcast","radio"}.

void DatabaseManager::save_tree(const std::string &root_type,
                                const std::vector<TreeNodePtr> &top_nodes) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // Y03: only open our own transaction when the connection is not already in one. When called from
    //   Persistence::save_data (which wraps save_tree + favourites clear/rewrite in an outer txn for
    //   atomicity), a nested BEGIN would fail "cannot start a transaction within a transaction" and
    //   save_tree's COMMIT would prematurely close the outer txn (leaving save_data's commit with
    //   "cannot commit - no transaction is active"). sqlite3_get_autocommit!=0 means idle/autocommit.
    bool own_txn = (db_ && sqlite3_get_autocommit(db_) != 0);
    if (own_txn)
        begin_txn();
    try {
        // Parameterized (no string interpolation)
        const char *del_sql = "DELETE FROM tree_nodes WHERE root_type=?;";
        sqlite3_stmt *del_stmt = nullptr;
        if (sqlite3_prepare_v2(db_, del_sql, -1, &del_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(del_stmt, 1, root_type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(del_stmt);
            sqlite3_finalize(del_stmt);
        }
        int order = 0;
        for (const auto &n : top_nodes) {
            if (n)
                save_tree_node_recursive(n, root_type, 0, order);
        }
        if (own_txn)
            commit_txn();
    } catch (...) {
        if (own_txn)
            rollback_txn();
        LOG(fmt::format("[DB] save_tree({}) failed{}", root_type,
                        own_txn ? ", rolled back" : " (within outer txn)"));
    }
}

void DatabaseManager::save_tree_node_recursive(const TreeNodePtr &node,
                                               const std::string &root_type, int parent_id,
                                               int &order) {
    if (!node)
        return;
    // Parameterized (no string interpolation). order is post-incremented (preserve original semantics).
    int cur_order = order++;
    const char *sql = "INSERT INTO tree_nodes (root_type, parent_id, title, url, type, expanded, "
                      "children_loaded, "
                      "is_youtube, has_subtitle, channel_name, is_cached, sort_order, art_url, "
                      "subtext, artist, album, track_num) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, root_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, parent_id);
        sqlite3_bind_text(stmt, 3, node->title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, node->url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, (int)node->type);
        sqlite3_bind_int(stmt, 6, node->expanded ? 1 : 0);
        sqlite3_bind_int(stmt, 7, node->children_loaded ? 1 : 0);
        sqlite3_bind_int(stmt, 8, node->is_youtube ? 1 : 0);
        sqlite3_bind_int(stmt, 9, node->has_subtitle ? 1 : 0);
        sqlite3_bind_text(stmt, 10, node->channel_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 11, node->is_cached ? 1 : 0);
        sqlite3_bind_int(stmt, 12, cur_order);
        sqlite3_bind_text(stmt, 13, node->art_url.c_str(), -1, SQLITE_TRANSIENT); // ART-1
        sqlite3_bind_text(stmt, 14, node->subtext.c_str(), -1, SQLITE_TRANSIENT); // ART-1
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    int64_t new_id = sqlite3_last_insert_rowid(db_);
    for (const auto &child : node->children) {
        save_tree_node_recursive(child, root_type, (int)new_id, order);
    }
}

// ─── Media cache (single status column: 0=none, 1=complete, 2=partial) ──────
std::vector<DatabaseManager::MediaCacheRow> DatabaseManager::load_media_cache() {
    std::vector<MediaCacheRow> rows;
    if (!is_ready())
        return rows;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, "SELECT url, status, local_file FROM media_cache;", -1, &stmt,
                           nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MediaCacheRow r;
            const unsigned char *t = sqlite3_column_text(stmt, 0);
            r.url = t ? reinterpret_cast<const char *>(t) : "";
            r.status = sqlite3_column_int(stmt, 1);
            t = sqlite3_column_text(stmt, 2);
            r.local_file = t ? reinterpret_cast<const char *>(t) : "";
            rows.push_back(std::move(r));
        }
        sqlite3_finalize(stmt);
    }
    return rows;
}

// status 0 → delete the row (no cache); 1/2 → INSERT OR REPLACE.
void DatabaseManager::media_cache_set(const std::string &url, int status,
                                      const std::string &local_file) {
    if (!is_ready() || url.empty())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (status == 0) {
        // Parameterized (no string interpolation)
        const char *del = "DELETE FROM media_cache WHERE url = ?;";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, del, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        return;
    }
    // Parameterized (no string interpolation)
    const char *sql = "INSERT OR REPLACE INTO media_cache(url, status, local_file, updated_at) "
                      "VALUES(?, ?, ?, CURRENT_TIMESTAMP);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, status);
        sqlite3_bind_text(stmt, 3, local_file.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void DatabaseManager::clear_media_cache() {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    exec_sql("DELETE FROM media_cache;");
    LOG("[DB] media_cache cleared");
}

// Favourites management - save a favourite item
void DatabaseManager::save_favourite(const std::string &title, const std::string &url, int type,
                                     bool is_youtube, const std::string &channel_name,
                                     const std::string &source_type, bool is_link,
                                     const std::string &link_target_url, bool is_local_folder) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // F38: link/local-folder metadata as columns (no data_json). Parameterized (no string interpolation).
    // N06: also persist media_type (display category) computed from url.
    const char *sql = "INSERT OR REPLACE INTO favourites (title, url, type, is_youtube, "
                      "channel_name, source_type, "
                      "is_link, link_target_url, is_local_folder, media_type) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    int mt = static_cast<int>(URLClassifier::classifyMediaType(url));
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, type);
        sqlite3_bind_int(stmt, 4, is_youtube ? 1 : 0);
        sqlite3_bind_text(stmt, 5, channel_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, source_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 7, is_link ? 1 : 0);
        sqlite3_bind_text(stmt, 8, link_target_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 9, is_local_folder ? 1 : 0);
        sqlite3_bind_int(stmt, 10, mt);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// Favourites management - load all favourites
std::vector<std::tuple<std::string, std::string, int, bool, std::string, std::string, bool,
                       std::string, bool, int>>
DatabaseManager::load_favourites() {
    std::vector<std::tuple<std::string, std::string, int, bool, std::string, std::string, bool,
                           std::string, bool, int>>
        favs;
    if (!is_ready())
        return favs;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    const char *sql =
        "SELECT title, url, type, is_youtube, channel_name, source_type, is_link, link_target_url, "
        "is_local_folder, media_type FROM favourites ORDER BY created_at DESC;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto col = [&](int i) -> std::string {
                const unsigned char *t = sqlite3_column_text(stmt, i);
                return t ? reinterpret_cast<const char *>(t) : "";
            };
            favs.push_back({col(0), col(1), sqlite3_column_int(stmt, 2),
                            sqlite3_column_int(stmt, 3) != 0, col(4), col(5),
                            sqlite3_column_int(stmt, 6) != 0, col(7),
                            sqlite3_column_int(stmt, 8) != 0, sqlite3_column_int(stmt, 9)});
        }
        sqlite3_finalize(stmt);
    }
    return favs;
}

// Favourites management - delete a favourite item
void DatabaseManager::delete_favourite(const std::string &url) {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Parameterized (no string interpolation)
    const char *sql = "DELETE FROM favourites WHERE url=?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// F38: unified tree load (replaces load_nodes + load_radio_cache). root_type ∈ {"podcast","radio"}.
void DatabaseManager::load_tree(const std::string &root_type, const TreeNodePtr &parent) {
    if (!is_ready() || !parent)
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    load_tree_node_recursive(parent, root_type, 0);
}

void DatabaseManager::load_tree_node_recursive(const TreeNodePtr &parent,
                                               const std::string &root_type, int parent_id) {
    // Parameterized (no string interpolation)
    const char *sql = "SELECT id, title, url, type, expanded, children_loaded, is_youtube, "
                      "has_subtitle, channel_name, is_cached, art_url, subtext, artist, album, "
                      "track_num "
                      "FROM tree_nodes WHERE root_type=? AND parent_id=? ORDER BY sort_order;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, root_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, parent_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto node = std::make_shared<TreeNode>();
        int node_id = sqlite3_column_int(stmt, 0);
        auto col = [&](int i) -> std::string {
            const unsigned char *t = sqlite3_column_text(stmt, i);
            return t ? reinterpret_cast<const char *>(t) : "";
        };
        node->title = col(1);
        node->url = col(2);
        node->type = (NodeType)sqlite3_column_int(stmt, 3);
        node->expanded = sqlite3_column_int(stmt, 4) != 0;
        node->children_loaded = sqlite3_column_int(stmt, 5) != 0;
        node->is_youtube = sqlite3_column_int(stmt, 6) != 0;
        node->has_subtitle = sqlite3_column_int(stmt, 7) != 0; // Y23.10: feed-level 📜 flag
        node->channel_name = col(8);
        node->is_cached = sqlite3_column_int(stmt, 9) != 0;
        node->art_url = col(10); // ART-1: remote artwork (station logo / podcast cover)
        node->subtext = col(11); // ART-1: remote browse second line
        node->artist = col(12);  // META-1: display metadata
        node->album = col(13);   // META-1
        node->track_num = sqlite3_column_int(stmt, 14); // META-1
        // F38 (#1): is_downloaded/local_file are NOT stored in tree_nodes (single source = media_cache).
        //   Left as default here; draw_line queries CacheManager::is_downloaded(url) live for coloring,
        //   so no information is lost. (Avoids a db-mtx → cache-mtx lock ordering inversion vs
        //   CacheManager::load's cache-mtx → db-mtx.)

        load_tree_node_recursive(node, root_type, node_id);
        node->parent = parent;
        parent->children.push_back(node);
    }
    sqlite3_finalize(stmt);
}

// Favourites sync step: DELETE existing rows so save_data can rewrite the full set inside one
//   transaction (clear+rewrite = sync, NOT a table drop). No log here — the sync is logged by the
//   caller (Persistence::save_data) with correct wording.
void DatabaseManager::clear_favourites() {
    if (!is_ready())
        return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    exec_sql("DELETE FROM favourites;");
}
} // namespace panicast
