// Online mode state management (singleton): node-based search history, region persistence.
//   - online_root is the root of the search history node tree; add_or_update_search_node updates it after a search
//   - Region persists to INI (search/default_region); load_region_from_config on startup
//   - load_search_history loads history from the database and builds the node tree
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "panicast/core/types.h"
#include "panicast/storage/database.h"

namespace panicast
{

class OnlineState {
public:
    static OnlineState &instance();

    std::string current_region = "US";
    std::string last_query;
    std::vector<TreeNodePtr> search_results;
    TreeNodePtr online_root = std::make_shared<TreeNode>();
    bool history_loaded = false; // Marks whether history has been loaded

    // Region persists to INI
    void set_region(const std::string &region);
    // Load region from INI on startup
    void load_region_from_config();
    std::string get_next_region();

    // Load search history from the database and build the node tree
    void load_search_history();
    // Add or update a search node (called after a search)
    void add_or_update_search_node(const std::string &query, const std::string &region,
                                   const std::vector<TreeNodePtr> &results);

private:
    OnlineState();

    // Create a search history node
    // Format: 🔍 [time][region][count(4 digits)]["keyword"]
    TreeNodePtr create_search_node(const DatabaseManager::SearchHistoryItem &item);
    // Update a search node title
    void update_search_node_title(TreeNodePtr node, const std::string &query,
                                  const std::string &region, int count);
    // Generate a unique search identifier
    std::string make_search_id(const std::string &query, const std::string &region);
    // Format a database timestamp
    std::string format_timestamp(const std::string &timestamp);
    // Get the current time string
    std::string get_current_time_str();

    std::mutex mtx_; // Protects mutations of online_root->children
};

} // namespace panicast
