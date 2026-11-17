// Media cache (singleton): local-media cache status per URL, persisted to the `media_cache` table
// (status: 0=none, 1=complete, 2=partial). In-memory sets are the fast read path (loaded once at
// startup via load()); every mutation writes through to the DB.
// NOTE: "feed parsed" (episode list cached) is NOT tracked here — that lives in episode_cache /
//   is_episode_cached, set on the node's is_cached flag at expand time (lazy DB query).
#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>

namespace panicast
{

class CacheManager {
public:
    static CacheManager &instance();

    // All accesses locked — concurrent mark by the download thread and read by the UI thread would corrupt the set/map (UB)
    void mark_downloaded(const std::string &url, const std::string &local_file = "");
    bool is_downloaded(const std::string &url);

    // Incomplete download (.part): status=2, persisted. Marked when a download fails/interrupts,
    //   cleared on successful download.
    void mark_partial(const std::string &url);
    bool is_partial(const std::string &url);

    std::string get_local_file(const std::string &url);

    void clear_download(const std::string &url);

    // Load all media_cache rows from the DB into the in-memory structures. Called once at startup.
    void load();

private:
    CacheManager() = default;
    std::set<std::string> downloaded_; // status == 1
    std::set<std::string> partial_;    // status == 2
    std::map<std::string, std::string> local_files_;
    std::mutex mtx_;
};

} // namespace panicast
