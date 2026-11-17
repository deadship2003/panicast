// Media cache (singleton): per-URL local-media cache status, persisted to the `media_cache` table
// (status: 0=none, 1=complete, 2=partial). In-memory sets are the fast read path (loaded once at
// startup via load()); every mutation writes through to the DB.
#include "panicast/storage/cache.h"

#include <filesystem>

#include <fmt/format.h>

#include "panicast/core/logger.h"
#include "panicast/storage/database.h"

namespace panicast
{

namespace fs = std::filesystem;

CacheManager &CacheManager::instance() {
    static CacheManager cm;
    return cm;
}

void CacheManager::mark_downloaded(const std::string &url, const std::string &local_file) {
    if (url.empty())
        return;
    std::lock_guard<std::mutex> lock(mtx_);
    downloaded_.insert(url);
    partial_.erase(url); // a successful download is no longer partial
    if (!local_file.empty())
        local_files_[url] = local_file;
    DatabaseManager::instance().media_cache_set(url, 1, local_file);
}

bool CacheManager::is_downloaded(const std::string &url) {
    std::lock_guard<std::mutex> lock(mtx_);
    return downloaded_.count(url) > 0;
}

void CacheManager::mark_partial(const std::string &url) {
    if (url.empty())
        return;
    std::lock_guard<std::mutex> lock(mtx_);
    if (downloaded_.count(url))
        return; // already complete — don't downgrade
    partial_.insert(url);
    // Persist status=2 (partial survives restart). Keep any known local_file (the .part path is
    //   derived from title, not stored, so pass empty — the row just marks "partial").
    std::string lf = local_files_.count(url) ? local_files_[url] : std::string();
    DatabaseManager::instance().media_cache_set(url, 2, lf);
}

bool CacheManager::is_partial(const std::string &url) {
    std::lock_guard<std::mutex> lock(mtx_);
    return !downloaded_.count(url) && partial_.count(url) > 0;
}

std::string CacheManager::get_local_file(const std::string &url) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = local_files_.find(url);
    return it != local_files_.end() ? it->second : std::string();
}

void CacheManager::clear_download(const std::string &url) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        downloaded_.erase(url);
        partial_.erase(url);
        auto it = local_files_.find(url);
        if (it != local_files_.end()) {
            path = it->second;
            local_files_.erase(it);
        }
        DatabaseManager::instance().media_cache_set(url, 0, ""); // delete the row
    }
    // Filesystem operations run outside the lock to avoid blocking while holding it
    if (!path.empty()) {
        try {
            if (fs::exists(path))
                fs::remove(path);
        } catch (const std::exception &e) {
            LOG(fmt::format("[Exception] {}", e.what()));
        }
    }
}

// Load all media_cache rows from the DB into the in-memory structures. Called once at startup
// (before any playback); afterwards is_downloaded/is_partial/get_local_file are pure in-memory reads.
void CacheManager::load() {
    std::vector<DatabaseManager::MediaCacheRow> rows;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        rows = DatabaseManager::instance().load_media_cache();
        for (const auto &r : rows) {
            if (r.status == 1) {
                downloaded_.insert(r.url);
                if (!r.local_file.empty())
                    local_files_[r.url] = r.local_file;
            } else if (r.status == 2) {
                partial_.insert(r.url);
                if (!r.local_file.empty())
                    local_files_[r.url] = r.local_file;
            }
        }
    }
    LOG(fmt::format("[CACHE] Loaded {} media-cache rows (complete={}, partial={})", rows.size(),
                    downloaded_.size(), partial_.size()));
}

} // namespace panicast
