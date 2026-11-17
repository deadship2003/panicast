// iTunes podcast search (search queries, not a feed parser, does not participate in ParserRegistry self-registration).
// Singleton; queries the iTunes Search API by region, results carry database caching.
#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "panicast/core/types.h"

namespace panicast
{

class ITunesSearch {
public:
    static ITunesSearch &instance() {
        static ITunesSearch is;
        return is;
    }

    // Supported region list
    static std::vector<std::string> get_regions();
    static std::string get_region_name(const std::string &region);

    // Search podcasts (with cache marker)
    std::vector<TreeNodePtr> search(const std::string &query, const std::string &region = "US",
                                    int limit = 50);

    // Public parse_result method for external callers
    TreeNodePtr parse_result(const nlohmann::json &item);

private:
    ITunesSearch() {}

    // Save podcast info to cache
    void save_podcast_to_cache(const nlohmann::json &item);

    // Fetch with completed SSL config
    std::string fetch(const std::string &url);
};

} // namespace panicast
