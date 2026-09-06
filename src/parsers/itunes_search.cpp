// iTunes podcast search implementation.
#include "panicast/parsers/itunes_search.h"

#include <fmt/format.h>

#include <curl/curl.h>

#include "panicast/config/ini_config.h"
#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/core/utils.h"
#include "panicast/net/network.h"
#include "panicast/storage/database.h"

namespace panicast
{

using json = nlohmann::json;

std::vector<std::string> ITunesSearch::get_regions() {
    return {"US", "CN", "TW", "JP", "UK", "DE", "FR", "KR", "AU"};
}

std::string ITunesSearch::get_region_name(const std::string &region) {
    static std::map<std::string, std::string> names = {
        {"US", "United States"}, {"CN", "China"},          {"TW", "Taiwan"},
        {"JP", "Japan"},         {"UK", "United Kingdom"}, {"DE", "Germany"},
        {"FR", "France"},        {"KR", "South Korea"},    {"AU", "Australia"}};
    return names.count(region) ? names[region] : region;
}

std::vector<TreeNodePtr> ITunesSearch::search(const std::string &query, const std::string &region,
                                              int limit) {
    std::vector<TreeNodePtr> results;

    // First check the cache
    std::string cached = DatabaseManager::instance().load_search_cache(query, region);
    if (!cached.empty()) {
        try {
            json j = json::parse(cached);
            if (j.contains("results") && j["results"].is_array()) {
                for (const auto &item : j["results"]) {
                    auto node = parse_result(item);
                    if (node) {
                        //Check whether it is already in podcast_cache
                        node->is_db_cached =
                            DatabaseManager::instance().is_podcast_cached(node->url);
                        results.push_back(node);
                    }
                }
            }
            EVENT_LOG(fmt::format("[iTunes] Loaded from cache: {} results for '{}'", results.size(),
                                  query));
            return results;
        } catch (const std::exception &e) {
            LOG(fmt::format("[Exception] {}", e.what()));
        }
    }

    // Network request (region also needs URL encoding, to prevent INI injection of &parameters)
    std::string url =
        fmt::format("https://itunes.apple.com/search?term={}&media=podcast&country={}&limit={}",
                    Utils::url_encode(query), Utils::url_encode(region), limit);

    std::string response = fetch(url);
    if (response.empty()) {
        EVENT_LOG(fmt::format("[iTunes] Search failed for '{}'", query));
        return results;
    }

    // Parse and validate first, confirming it is a valid result before caching — avoid persisting
    //   HTML error pages / truncated bodies as valid results (cache poisoning, and no TTL never expires)
    bool parsed_ok = false;
    try {
        json j = json::parse(response);
        if (j.contains("results") && j["results"].is_array()) {
            parsed_ok = true;
            for (const auto &item : j["results"]) {
                auto node = parse_result(item);
                if (node) {
                    save_podcast_to_cache(item);
                    node->is_db_cached = DatabaseManager::instance().is_podcast_cached(node->url);
                    results.push_back(node);
                }
            }
        }
    } catch (const std::exception &e) {
        LOG(fmt::format("[iTunes] Parse error: {}", e.what()));
    }

    if (parsed_ok) {
        DatabaseManager::instance().save_search_cache(query, region, response);
    }

    EVENT_LOG(fmt::format("[iTunes] Search '{}': {} results", query, results.size()));
    return results;
}

TreeNodePtr ITunesSearch::parse_result(const json &item) {
    auto node = std::make_shared<TreeNode>();
    node->type = NodeType::PODCAST_FEED;

    // Use value()/type checks instead of get<>() to avoid a single malformed item throwing and discarding the whole batch
    auto get_str = [&](const char *key) -> std::string {
        if (!item.contains(key))
            return "";
        const json &v = item[key];
        if (v.is_string())
            return v.get<std::string>();
        if (v.is_number())
            return std::to_string(v.get<long long>());
        return "";
    };

    std::string name = get_str("collectionName");
    if (name.empty())
        name = get_str("trackName");
    if (name.empty())
        return nullptr;
    node->title = name;

    node->url = get_str("feedUrl");
    if (node->url.empty())
        return nullptr; // Skip items without a feed URL
    // META-3: iTunes carries full display metadata — use it (art gets the 600px variant).
    node->art_url = get_str("artworkUrl600");
    if (node->art_url.empty())
        node->art_url = get_str("artworkUrl100");
    node->artist = get_str("artistName");
    node->album = name; // a podcast feed's "album" is the show itself (META-1 semantics)

    // Add subtext to display more information
    std::string subtext;
    std::string artist = get_str("artistName");
    if (!artist.empty())
        subtext = artist;
    if (item.contains("trackCount") && item["trackCount"].is_number_integer()) {
        if (!subtext.empty())
            subtext += " · ";
        subtext += std::to_string(item["trackCount"].get<int>()) + " episodes";
    }
    std::string genre = get_str("primaryGenreName");
    if (!genre.empty()) {
        if (!subtext.empty())
            subtext += " · ";
        subtext += genre;
    }
    node->subtext = subtext;

    return node;
}

void ITunesSearch::save_podcast_to_cache(const json &item) {
    if (!item.contains("feedUrl") || !item["feedUrl"].is_string())
        return; // null/non-string feedUrl does not throw, to avoid discarding the whole batch

    std::string feed_url = item["feedUrl"].get<std::string>();
    std::string title = item.value("collectionName", "");
    std::string artist = item.value("artistName", "");
    std::string genre = item.value("primaryGenreName", "");
    int track_count = item.value("trackCount", 0);
    std::string artwork_url = item.value("artworkUrl600", item.value("artworkUrl100", ""));
    int collection_id = item.value("collectionId", 0);

    DatabaseManager::instance().save_podcast_cache(feed_url, title, artist, genre, track_count,
                                                   artwork_url, collection_id);
}

std::string ITunesSearch::fetch(const std::string &url) {
    // CurlRAII auto-releases
    CurlRAII curl_raii;
    CURL *curl = curl_raii.handle;
    if (!curl)
        return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION, +[](void *ptr, size_t size, size_t nmemb, void *data) {
            ((std::string *)data)->append((char *)ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT); // Unified browser UA
    apply_network_proxy(curl, url, "podcast");             // [network] proxy
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

    // TLS verification is enabled by default, disabled only when the user explicitly sets tls_verify=false
    bool tls_verify = IniConfig::instance().get_tls_verify();
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, tls_verify ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, tls_verify ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_DEFAULT);
    curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN,
                     1L); // Enable ALPN - iTunes (Akamai CDN) requires it
#if LIBCURL_VERSION_NUM < 0x075600
    curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_NPN, 1L);
#endif

    CURLcode res = curl_easy_perform(curl);
    // curl is auto-released by CurlRAII

    if (res != CURLE_OK)
        return "";
    // Validate the HTTP status code; 4xx/5xx with a body is not treated as success either
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 400 || http_code == 0) {
        LOG(fmt::format("[iTunes] HTTP {} for search", http_code));
        return "";
    }
    return response;
}

} // namespace panicast
