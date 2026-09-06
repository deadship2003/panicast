// YouTube channel cache (singleton): in-memory cache + database youtube_cache table persistence.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace panicast
{

struct YouTubeVideoInfo {
    std::string id, title, url;
    std::string thumbnail; // ART-2: video thumbnail (remote browse / now-playing art)
};
struct YouTubeChannelCache {
    std::string channel_name;
    std::vector<YouTubeVideoInfo> videos;
};

class YouTubeCache {
public:
    static YouTubeCache &instance();

    // Switched to database storage, no longer uses JSON files
    void load();
    void save();

    // All accesses are locked — concurrent reads of cache_ and update reallocation cause UB;
    //   serialization also eliminates competition between independent sqlite connections and DatabaseManager.
    bool has(const std::string &url);
    YouTubeChannelCache get(const std::string &url);
    void update(const std::string &url, const std::string &name,
                const std::vector<YouTubeVideoInfo> &videos);

private:
    YouTubeCache() = default;
    std::map<std::string, YouTubeChannelCache> cache_;
    std::mutex mtx_; // Protects cache_ and db access (caller holds lock)

    // Load YouTube cache from database
    bool load_from_db(const std::string &channel_url);
    // Save YouTube cache to database
    void save_to_db(const std::string &channel_url, const std::string &channel_name,
                    const std::vector<YouTubeVideoInfo> &videos);
};

} // namespace panicast
