// Y24.50: I-mode (IPTV) controller — browse iptv-org (CC0) playlists by All/Region/Country/
//   Category/Language/Custom. m3u content is fetched live + cached (24h, INI [iptv] cache_hours),
//   so data stays current without bundling stale snapshots. Channels play via the existing mpv path
//   (vo=auto → video window; --quiet → audio-only).
#include "panicast/app/app.h"

#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sys/stat.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "panicast/config/ini_config.h"
#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/core/paths.h"
#include "panicast/net/network.h"
#include "panicast/parsers/m3u_parser.h"

namespace panicast
{
namespace fs = std::filesystem;
using json = nlohmann::json;

// ── Cache: <data_dir>/iptv-cache/<sanitized-url>.txt, TTL = cache_hours ──
static std::string iptv_cache_path(const std::string &url) {
    std::string key = url;
    for (char &c : key)
        if (!isalnum((unsigned char)c) && c != '.' && c != '-')
            c = '_';
    if (key.size() > 128)
        key = key.substr(key.size() - 128);
    return Paths::get_data_dir() + "/iptv-cache/" + key + ".txt";
}

static std::string iptv_fetch(const std::string &url) {
    std::string path = iptv_cache_path(url);
    int cache_sec = IniConfig::instance().get_iptv_cache_hours() * 3600;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        if (cache_sec <= 0 || (time(nullptr) - st.st_mtime) < cache_sec) {
            std::ifstream f(path);
            if (f) {
                std::string s((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
                if (!s.empty())
                    return s;
            }
        }
    }
    std::string data = Network::fetch(url, 10);
    if (!data.empty()) {
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        std::ofstream f(path);
        if (f)
            f << data;
    }
    return data;
}

// Build a playable channel leaf (Enter plays it; peers = siblings).
static TreeNodePtr make_channel_node(const IptvChannel &c) {
    auto n = std::make_shared<TreeNode>();
    n->title = c.name.empty() ? c.url : c.name;
    n->url = c.url;
    n->type = NodeType::PODCAST_EPISODE; // playable leaf; play() routes the stream URL via mpv
    n->is_iptv_channel = true;           // N04: render with the 📺 TV icon
    n->children_loaded = true;           // leaf — no children
    n->art_url = c.logo;                 // META-3: tvg-logo channel icon
    n->artist = c.group.empty() ? "IPTV" : c.group; // META-3: group = artist context
    n->album = c.group.empty() ? "IPTV" : c.group;  // META-3
    return n;
}

static std::string to_lower(std::string s) {
    for (char &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

// Y24.51: a channel URL whose path (before '?') ends in ".m3u" (but NOT ".m3u8") is a nested
//   sub-playlist → make it an expandable folder (recursively parsed on expand). ".m3u8" is HLS,
//   which mpv plays directly as a stream (playable leaf). .ts/.mp4/etc. are also playable leaves.
static bool is_sub_playlist(const std::string &url) {
    std::string p = url.substr(0, url.find('?'));
    if (p.size() >= 4 && p.compare(p.size() - 4, 4, ".m3u") == 0) {
        if (p.size() >= 5 && p.compare(p.size() - 5, 5, ".m3u8") == 0)
            return false; // HLS stream
        return true;
    }
    return false;
}

// Build a channel leaf OR a sub-playlist folder (recursive) from a parsed channel.
static TreeNodePtr make_iptv_node(const IptvChannel &c) {
    if (is_sub_playlist(c.url)) {
        auto n = std::make_shared<TreeNode>();
        n->title = (c.name.empty() ? c.url : c.name) + " ▸"; // ▸ hints it expands
        n->url = "iptv:subm3u:" + c.url; // expand_iptv_node fetches+parses this recursively
        n->type = NodeType::FOLDER;
        n->children_loaded = false; // lazy recursive parse on expand
        return n;
    }
    return make_channel_node(c);
}

// Group channels by group-title into subfolders (or flat if there's only one group).
static std::vector<TreeNodePtr> build_channel_tree(const std::vector<IptvChannel> &ch) {
    std::vector<TreeNodePtr> kids;
    if (ch.empty())
        return kids;
    std::map<std::string, std::vector<IptvChannel>> by_group;
    for (const auto &c : ch)
        by_group[c.group.empty() ? "Other" : c.group].push_back(c);
    if (by_group.size() == 1) {
        for (const auto &c : ch)
            kids.push_back(make_iptv_node(c));
    } else {
        for (auto &[group, list] : by_group) {
            auto folder = std::make_shared<TreeNode>();
            folder->title = group;
            folder->type = NodeType::FOLDER;
            for (const auto &c : list) {
                auto n = make_iptv_node(c);
                n->parent = folder;
                folder->children.push_back(n);
            }
            folder->children_loaded = true;
            kids.push_back(folder);
        }
    }
    return kids;
}

// Parse an iptv-org API JSON array (countries/categories/regions/languages) into child folder
// nodes. `code_key`/`name_key` select the fields; `kind` prefixes the child url (iptv:<kind>:<code>).
static std::vector<TreeNodePtr> build_catalog(const std::string &body, const std::string &kind,
                                              const std::string &code_key,
                                              const std::string &name_key) {
    std::vector<TreeNodePtr> kids;
    if (body.empty())
        return kids;
    try {
        auto j = json::parse(body);
        if (!j.is_array())
            return kids;
        for (const auto &e : j) {
            std::string code = e.value(code_key, "");
            std::string name = e.value(name_key, code);
            if (code.empty())
                continue;
            auto n = std::make_shared<TreeNode>();
            n->title = name.empty() ? code : name;
            n->type = NodeType::FOLDER;
            n->url = "iptv:" + kind + ":" + code;
            n->children_loaded = false; // lazy: expanding fetches the per-code .m3u
            kids.push_back(n);
        }
    } catch (const std::exception &e) {
        LOG(fmt::format("[IPTV] catalog parse error: {}", e.what()));
    }
    return kids;
}

// D11-3c: load_iptv_root relocated to LibraryService (the iptv_root_ owner).

// ── CACHE-1: serialize/deserialize an iptv child subtree (channel leaf, sub-playlist folder,
//   or group folder with nested children) to/from the iptv_cache table. ──
static json serialize_iptv_node(const TreeNodePtr &n) {
    json j;
    j["title"] = n->title;
    j["url"] = n->url;
    j["type"] = static_cast<int>(n->type);
    j["is_iptv_channel"] = n->is_iptv_channel;
    j["children_loaded"] = n->children_loaded;
    if (!n->children.empty()) {
        json kids = json::array();
        for (const auto &c : n->children)
            kids.push_back(serialize_iptv_node(c));
        j["children"] = kids;
    }
    return j;
}

static TreeNodePtr deserialize_iptv_node(const json &j) {
    auto n = std::make_shared<TreeNode>();
    n->title = j.value("title", std::string());
    n->url = j.value("url", std::string());
    n->type = static_cast<NodeType>(j.value("type", static_cast<int>(NodeType::FOLDER)));
    n->is_iptv_channel = j.value("is_iptv_channel", false);
    n->children_loaded = j.value("children_loaded", false);
    if (j.contains("children") && j["children"].is_array()) {
        for (const auto &cj : j["children"]) {
            auto c = deserialize_iptv_node(cj);
            c->parent = n;
            n->children.push_back(c);
        }
    }
    return n;
}

void App::expand_iptv_node(TreeNodePtr node) {
    if (!node)
        return;
    const std::string u = node->url;
    if (u.empty() || u.rfind("iptv:", 0) != 0)
        return;

    node->loading = true;
    node->parse_failed = false;
    node->error_msg.clear();
    node->children.clear();
    node->children_loaded = false;

    pool_.submit([this, node, u]() {
        // CACHE-1: serve a fresh DB cache entry (same TTL as iptv_fetch), skipping the network.
        const int cache_sec = IniConfig::instance().get_iptv_cache_hours() * 3600;
        const std::string cached = DatabaseManager::instance().load_iptv_cache(u);
        if (!cached.empty()) {
            const int64_t age = static_cast<int64_t>(time(nullptr)) -
                                DatabaseManager::instance().iptv_cache_updated_at(u);
            if (cache_sec <= 0 || age < cache_sec) {
                try {
                    json j = json::parse(cached);
                    if (j.is_array()) {
                        std::vector<TreeNodePtr> cached_kids;
                        for (const auto &kj : j) {
                            auto n = deserialize_iptv_node(kj);
                            n->parent = node;
                            cached_kids.push_back(n);
                        }
                        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                        node->children = cached_kids;
                        node->children_loaded = true;
                        node->expanded = true;
                        node->loading = false;
                        EVENT_LOG(fmt::format("[IPTV] loaded {} item(s) for {} (cached)",
                                              cached_kids.size(), u));
                        return;
                    }
                } catch (const std::exception &e) {
                    LOG(fmt::format("[IPTV] cache parse error: {}", e.what()));
                }
            }
        }

        const std::string base = IniConfig::instance().get_iptv_base_url();
        const std::string api = IniConfig::instance().get_iptv_api_url();
        std::vector<TreeNodePtr> kids;

        if (u == "iptv:all") {
            kids = build_channel_tree(parse_m3u(iptv_fetch(base + "/index.m3u")));
        } else if (u == "iptv:regions") {
            kids = build_catalog(iptv_fetch(api + "/regions.json"), "region", "code", "name");
        } else if (u.rfind("iptv:region:", 0) == 0) {
            // Y24.52: substr offset = len("iptv:region:") = 12 (was 11 → leaked ':' into the URL).
            //   regions.json code is UPPERCASE (AFR) but the m3u filename is lowercase (afr.m3u).
            kids = build_channel_tree(
                parse_m3u(iptv_fetch(base + "/regions/" + to_lower(u.substr(12)) + ".m3u")));
        } else if (u == "iptv:countries") {
            kids = build_catalog(iptv_fetch(api + "/countries.json"), "country", "code", "name");
        } else if (u.rfind("iptv:country:", 0) == 0) {
            // Y24.52: substr offset = len("iptv:country:") = 13 (was 12 → leaked ':').
            kids = build_channel_tree(
                parse_m3u(iptv_fetch(base + "/countries/" + to_lower(u.substr(13)) + ".m3u")));
        } else if (u == "iptv:categories") {
            kids = build_catalog(iptv_fetch(api + "/categories.json"), "category", "id", "name");
        } else if (u.rfind("iptv:category:", 0) == 0) {
            // Y24.52: substr offset = len("iptv:category:") = 14 (was 13 → leaked ':').
            kids = build_channel_tree(
                parse_m3u(iptv_fetch(base + "/categories/" + u.substr(14) + ".m3u")));
        } else if (u == "iptv:languages") {
            kids = build_catalog(iptv_fetch(api + "/languages.json"), "language", "code", "name");
        } else if (u.rfind("iptv:language:", 0) == 0) {
            // Y24.52: substr offset = len("iptv:language:") = 14 (was 13 → leaked ':').
            kids = build_channel_tree(
                parse_m3u(iptv_fetch(base + "/languages/" + to_lower(u.substr(14)) + ".m3u")));
        } else if (u.rfind("iptv:subm3u:", 0) == 0) {
            // Y24.51: recursive sub-playlist. Y24.52: substr offset = len("iptv:subm3u:") = 12.
            kids = build_channel_tree(parse_m3u(iptv_fetch(u.substr(12))));
        } else if (u == "iptv:custom") {
            // Parse INI custom_urls (comma-separated) into child folders.
            std::string raw = IniConfig::instance().get_iptv_custom_urls();
            std::regex sep("\\s*,\\s*");
            std::sregex_token_iterator it(raw.begin(), raw.end(), sep, -1), end;
            int idx = 0;
            for (; it != end; ++it) {
                std::string cu = it->str();
                if (cu.empty() || cu.rfind("http", 0) != 0)
                    continue;
                auto n = std::make_shared<TreeNode>();
                // Use the last URL path segment as a label, or the whole URL.
                size_t slash = cu.find_last_of('/');
                n->title = (slash != std::string::npos && slash + 1 < cu.size())
                               ? cu.substr(slash + 1)
                               : cu;
                n->url = "iptv:customm3u:" + std::to_string(idx); // index into a sidecar file
                n->type = NodeType::FOLDER;
                n->children_loaded = false;
                // Persist the actual URL in a tiny sidecar so the expand step can read it.
                std::string meta =
                    Paths::get_data_dir() + "/iptv-cache/custom_" + std::to_string(idx) + ".url";
                std::error_code ec;
                fs::create_directories(fs::path(meta).parent_path(), ec);
                std::ofstream f(meta);
                if (f)
                    f << cu;
                kids.push_back(n);
                ++idx;
            }
            if (kids.empty()) {
                auto hint = std::make_shared<TreeNode>();
                hint->title =
                    "(no custom m3u — set [iptv] custom_urls = url1,url2 in panicast.ini)";
                hint->type = NodeType::FOLDER;
                hint->children_loaded = true;
                kids.push_back(hint);
            }
        } else if (u.rfind("iptv:customm3u:", 0) == 0) {
            // Y24.52: substr offset = len("iptv:customm3u:") = 15 (was 14 → leaked ':').
            std::string meta =
                Paths::get_data_dir() + "/iptv-cache/custom_" + u.substr(15) + ".url";
            std::ifstream f(meta);
            std::string cu((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            // strip whitespace
            while (!cu.empty() && (cu.back() == '\n' || cu.back() == '\r' || cu.back() == ' '))
                cu.pop_back();
            if (!cu.empty())
                kids = build_channel_tree(parse_m3u(iptv_fetch(cu)));
        }

        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        node->children = kids;
        for (auto &c : node->children)
            c->parent = node;
        node->children_loaded = true;
        node->expanded = true;
        node->loading = false;
        if (kids.empty()) {
            node->parse_failed = true;
            node->error_msg = "No channels (network blocked, empty playlist, or geo-restricted)";
        }

        // CACHE-1: persist the parsed tree (outside the lock) so the next expand hits the DB.
        if (!kids.empty()) {
            json arr = json::array();
            for (const auto &k : kids)
                arr.push_back(serialize_iptv_node(k));
            DatabaseManager::instance().save_iptv_cache(u, arr.dump());
        }

        EVENT_LOG(fmt::format("[IPTV] loaded {} item(s) for {}", kids.size(), u));
    });
}

} // namespace panicast
