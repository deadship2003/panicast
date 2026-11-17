// Persistence layer: DB read/write for subscriptions/favourites/RADIO tree (unified tree_nodes),
//   OPML import/export.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "panicast/core/types.h"

namespace panicast
{

class OnlineState; // Forward declaration (full definition of get_online_root is used only in .cpp)

class Persistence {
public:
    // Save RADIO tree to database (cache.json no longer used). E: radio is the items vector.
    static void save_cache(const std::vector<TreeNodePtr> &radio,
                           const std::vector<TreeNodePtr> &podcast);
    // Load RADIO tree from database into the items vector.
    static void load_cache(std::vector<TreeNodePtr> &radio, std::vector<TreeNodePtr> &podcast);

    // Save subscriptions and favourites to database
    // Key fix: clear subscription data in the nodes table first, then save current data
    // so that deleting a subscription before saving actually takes effect
    static void save_data(const std::vector<TreeNodePtr> &podcasts,
                          const std::vector<TreeNodePtr> &favs);
    // Load subscriptions and favourites from database (nodes/favourites tables — the only source of truth)
    static void load_data(std::vector<TreeNodePtr> &podcasts, std::vector<TreeNodePtr> &favs);

    // Export OPML (compatible with command-line mode, outputs via std::cout)
    static void export_opml(const std::string &filename, const std::vector<TreeNodePtr> &podcasts);

private:
    // F39: removed dead legacy json helpers (save_tree/load_tree/save_node/load_node) —
    //   pre-DB nested-JSON persistence, unused since F38's recursive tree_nodes storage.

    // Bridges OnlineState::instance().online_root (defined in persistence.cpp).
    static TreeNodePtr get_online_root();
};

} // namespace panicast
