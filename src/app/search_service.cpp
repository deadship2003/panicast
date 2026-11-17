#include "panicast/app/search_service.h"

#include <functional> // std::function (reveal_node's recursive lambda)
#include <utility>    // std::move

namespace panicast
{

void SearchService::reset() {
    search_query_.clear();
    search_matches_.clear();
    current_match_idx_ = -1;
    total_matches_ = 0;
}

// D11-3b: relocated verbatim from App::search_recursive (app_search.cpp). Pure subtree title-match.
void SearchService::search_recursive(const TreeNodePtr &node, const std::string &query,
                                     std::vector<TreeNodePtr> &results) {
    if (Utils::to_lower(node->title).find(query) != std::string::npos)
        results.push_back(node);
    for (auto &child : node->children)
        search_recursive(child, query, results);
}

// D11-3b: the F20 context-aware core of perform_search, extracted verbatim (order + dedup
//   unchanged). Caller holds tree_mutex and passes the mode's top-level items + the cursor node;
//   this method never touches display_list / selected_idx / view_start / LINES.
void SearchService::collect_context_matches(const std::vector<TreeNodePtr> &roots,
                                            TreeNodePtr cursor, const std::string &ql) {
    search_matches_.clear(); // reset() already cleared it; defensive for direct callers
    if (cursor) {
        // 1. Search peers (same parent's children, same type priority)
        auto parent = cursor->parent.lock();
        if (parent) {
            // Same-type peers first (e.g., if cursor is on an episode, search episodes first)
            for (auto &sib : parent->children) {
                if (sib->type == cursor->type &&
                    Utils::to_lower(sib->title).find(ql) != std::string::npos)
                    search_matches_.push_back(sib);
            }
            // Different-type siblings (e.g., feed nodes if cursor is on an episode)
            for (auto &sib : parent->children) {
                if (sib->type != cursor->type &&
                    Utils::to_lower(sib->title).find(ql) != std::string::npos)
                    search_matches_.push_back(sib);
            }
            // 2. Search the parent's subtree (children of siblings)
            for (auto &sib : parent->children) {
                if (sib != cursor) {
                    for (auto &child : sib->children)
                        search_recursive(child, ql, search_matches_);
                }
            }
        }
        // 3. Search the current node's own subtree
        for (auto &child : cursor->children)
            search_recursive(child, ql, search_matches_);
    }

    // 4. Global search (everything not yet covered) — but skip the subtree already searched
    for (auto &it : roots)
        search_recursive(it, ql, search_matches_);

    // Deduplicate (peers/subtree may overlap with global)
    std::set<TreeNodePtr> seen;
    std::vector<TreeNodePtr> unique;
    for (auto &m : search_matches_) {
        if (seen.insert(m).second)
            unique.push_back(m);
    }
    search_matches_ = std::move(unique);
    total_matches_ = (int)search_matches_.size();
}

// D11-3b: the jump_search cursor math (extracted). Wraps ±1 over the match count.
bool SearchService::cycle_match(int dir) {
    if (total_matches_ == 0)
        return false;
    current_match_idx_ = (current_match_idx_ + dir + total_matches_) % total_matches_;
    return true;
}

// D12-2: relocated from App::reveal_node (app_search.cpp). Walks each root's subtree recursively;
//   on the path that reaches `node`, sets expanded=true so the node is visible when flattened.
//   Caller holds tree_mutex (traversal + mutation); this method never locks nor touches display.
void SearchService::reveal_node(const std::vector<TreeNodePtr> &roots, const TreeNodePtr &node) {
    std::function<bool(TreeNodePtr)> reveal = [&](TreeNodePtr curr) -> bool {
        if (curr == node)
            return true;
        for (auto &child : curr->children) {
            if (reveal(child)) {
                curr->expanded = true;
                return true;
            }
        }
        return false;
    };
    for (auto &it : roots)
        reveal(it);
}

} // namespace panicast
