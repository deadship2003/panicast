// Online mode state management implementation.
#include "panicast/app/online_state.h"

#include <algorithm>
#include <ctime>

#include <fmt/format.h>

#include "panicast/config/ini_config.h"
#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/core/platform.h"
#include "panicast/parsers/itunes_search.h"

namespace panicast
{

OnlineState &OnlineState::instance() {
    static OnlineState os;
    return os;
}

OnlineState::OnlineState() {
    online_root->title = "Online Search";
    online_root->type = NodeType::FOLDER;
}

void OnlineState::set_region(const std::string &region) {
    current_region = region;
    // Persist to INI (best-effort, failures are silent)
    try {
        IniConfig::instance().set("search", "default_region", region);
    } catch (const std::exception &e) {
        LOG(fmt::format("[Exception] {}", e.what()));
    }
    EVENT_LOG(fmt::format("Region set to: {}", region));
}

void OnlineState::load_region_from_config() {
    try {
        std::string saved = IniConfig::instance().get("search", "default_region", "US");
        if (!saved.empty()) {
            current_region = saved;
            LOG(fmt::format("[OnlineState] Loaded region from config: {}", saved));
        }
    } catch (const std::exception &e) {
        LOG(fmt::format("[Exception] {}", e.what()));
    }
}

std::string OnlineState::get_next_region() {
    auto regions = ITunesSearch::get_regions();
    auto it = std::find(regions.begin(), regions.end(), current_region);
    if (it != regions.end() && ++it != regions.end()) {
        set_region(*it);
    } else {
        set_region(regions[0]);
    }
    return current_region;
}

void OnlineState::load_search_history() {
    std::lock_guard<std::mutex> lock(mtx_); // Protect the online_root tree
    if (history_loaded)
        return;

    online_root->children.clear();
    auto history = DatabaseManager::instance().load_all_search_history();

    for (const auto &item : history) {
        auto node = create_search_node(item);
        if (node) {
            node->parent = online_root; //Set parent node pointer
            online_root->children.push_back(node);
        }
    }

    history_loaded = true;
    EVENT_LOG(fmt::format("[Online] Loaded {} search history items", history.size()));
}

void OnlineState::add_or_update_search_node(const std::string &query, const std::string &region,
                                            const std::vector<TreeNodePtr> &results) {
    std::lock_guard<std::mutex> lock(mtx_); // Protect the online_root tree
    // Look for an existing node with the same query and region
    TreeNodePtr existing_node = nullptr;
    for (auto &child : online_root->children) {
        if (child->url == make_search_id(query, region)) {
            existing_node = child;
            break;
        }
    }

    if (existing_node) {
        //Update existing node - move to front, refresh children
        existing_node->children.clear();
        for (const auto &result : results) {
            result->parent = existing_node; //Set parent node pointer
            existing_node->children.push_back(result);
        }
        // Update result count and time in the title
        update_search_node_title(existing_node, query, region, results.size());

        // Move to front (descending by time)
        auto it =
            std::find(online_root->children.begin(), online_root->children.end(), existing_node);
        if (it != online_root->children.begin()) {
            online_root->children.erase(it);
            online_root->children.insert(online_root->children.begin(), existing_node);
        }
    } else {
        // Create a new node and insert at the front
        auto node = std::make_shared<TreeNode>();
        node->type = NodeType::FOLDER;
        node->url = make_search_id(query, region);
        update_search_node_title(node, query, region, results.size());

        for (const auto &result : results) {
            result->parent = node; //Set parent node pointer
            node->children.push_back(result);
        }

        node->parent = online_root; //Set parent node pointer
        online_root->children.insert(online_root->children.begin(), node);
    }
}

TreeNodePtr OnlineState::create_search_node(const DatabaseManager::SearchHistoryItem &item) {
    auto node = std::make_shared<TreeNode>();
    node->type = NodeType::FOLDER;
    node->url = make_search_id(item.query, item.region);

    std::string region_name = ITunesSearch::get_region_name(item.region);
    std::string time_str = format_timestamp(item.timestamp);

    node->title = fmt::format("🔍 [{}][{}][{:4d}][\"{}\"]", time_str, region_name,
                              item.result_count, item.query);

    // Children are loaded from cache when expanded
    node->children_loaded = false;

    return node;
}

void OnlineState::update_search_node_title(TreeNodePtr node, const std::string &query,
                                           const std::string &region, int count) {
    std::string region_name = ITunesSearch::get_region_name(region);
    std::string time_str = get_current_time_str();

    // Display-friendly: the query IS the title; details (count/region/time) go to the
    //   second line (subtext) instead of a bracket wall that reads as garbage on the
    //   phone. The search id (url) still carries query+region for dedup.
    node->title = fmt::format("🔍 {}", query);
    node->subtext = fmt::format("{} results · {} · {}", count, region_name, time_str);
}

std::string OnlineState::make_search_id(const std::string &query, const std::string &region) {
    return fmt::format("search:{}:{}", region, query);
}

std::string OnlineState::format_timestamp(const std::string &timestamp) {
    // Input format: "2026-03-05 19:33:36"
    // Output format: "2026-03-05 19:33:36" (unchanged)
    if (timestamp.empty())
        return "";

    // Try to parse and format
    // SQLite CURRENT_TIMESTAMP format: YYYY-MM-DD HH:MM:SS
    return timestamp;
}

std::string OnlineState::get_current_time_str() {
    auto now = std::time(nullptr);
    char buf[64];
    struct tm tm_local;
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime_local(&now, &tm_local));
    return std::string(buf);
}

} // namespace panicast
