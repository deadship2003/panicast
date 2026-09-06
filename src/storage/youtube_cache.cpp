// YouTube channel cache (singleton): in-memory cache + `youtube_cache` DB table persistence.
// All DB access goes through DatabaseManager's single shared connection (no second sqlite
// connection, which used to race with the main one).
#include "panicast/storage/youtube_cache.h"

#include <nlohmann/json.hpp>

#include <fmt/format.h>

#include "panicast/core/logger.h"
#include "panicast/storage/database.h"

namespace panicast
{

using json = nlohmann::json;

YouTubeCache &YouTubeCache::instance() {
    static YouTubeCache yc;
    return yc;
}

// In-memory cache is lazy (loaded per-channel on first has()/get()). Nothing to preload.
void YouTubeCache::load() {
    std::lock_guard<std::mutex> lock(mtx_);
    cache_.clear();
}

void YouTubeCache::save() {
    // Per-channel saves happen in update(); nothing to do here.
}

bool YouTubeCache::has(const std::string &url) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (cache_.count(url) > 0)
        return true;
    return load_from_db(url);
}

YouTubeChannelCache YouTubeCache::get(const std::string &url) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (cache_.count(url))
        return cache_[url];
    if (load_from_db(url) && cache_.count(url)) {
        return cache_[url];
    }
    return YouTubeChannelCache();
}

void YouTubeCache::update(const std::string &url, const std::string &name,
                          const std::vector<YouTubeVideoInfo> &videos) {
    std::lock_guard<std::mutex> lock(mtx_);
    cache_[url] = {name, videos};
    save_to_db(url, name, videos);
}

// Load one channel from the DB (via DatabaseManager's shared connection) into the in-memory map.
bool YouTubeCache::load_from_db(const std::string &channel_url) {
    if (!DatabaseManager::instance().is_ready())
        return false;
    std::string name, videos_json;
    if (!DatabaseManager::instance().youtube_cache_load(channel_url, name, videos_json))
        return false;

    YouTubeChannelCache cache;
    cache.channel_name = name;
    if (!videos_json.empty()) {
        try {
            json j = json::parse(videos_json);
            if (j.is_array()) {
                for (const auto &vid : j) {
                    cache.videos.push_back({vid.value("id", ""), vid.value("title", ""),
                                            vid.value("url", ""), vid.value("thumbnail", "")});
                }
            }
        } catch (const std::exception &e) {
            LOG(fmt::format("[Exception] {}", e.what()));
        }
    }
    cache_[channel_url] = cache;
    return true;
}

// Save one channel to the DB (via DatabaseManager's shared connection).
void YouTubeCache::save_to_db(const std::string &channel_url, const std::string &channel_name,
                              const std::vector<YouTubeVideoInfo> &videos) {
    if (!DatabaseManager::instance().is_ready())
        return;
    json videos_json = json::array();
    for (const auto &vi : videos) {
        videos_json.push_back(
            {{"id", vi.id}, {"title", vi.title}, {"url", vi.url}, {"thumbnail", vi.thumbnail}});
    }
    DatabaseManager::instance().youtube_cache_save(channel_url, channel_name, videos_json.dump());
}

} // namespace panicast
