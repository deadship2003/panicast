// SearchService — the Application Service (functional abstraction layer) for the in-tree search.
//   Owns the search STATE (query, match list, match cursor) AND the search ALGORITHM — both are
//   display-decoupled (operate on tree nodes + a cursor passed in, never touch display_list /
//   selected_idx / view_start / LINES). D10-2 moved the state out of App; D11-3b moves the
//   algorithm: search_recursive (subtree title-match), collect_context_matches (the F20
//   context-aware collection: peers-same-type → peers-diff-type → cursor subtree → global, +
//   dedup), and cycle_match (the jump_search cursor math). Principle ("search does only search's job"):
//   search produces a match list + cursor (model); flatten/scroll/select is the VIEW's job —
//   jump_to_match now only expands ancestors (SearchService::reveal_node) + hands the node to
//   pending_select (the deferred cursor the run loop resolves); App no longer touches display.
//   perform_search stays the App entry.
//   The online search-record cache (perform_online_search*, build_search_result_node,
//   add_search_record, …) is account-coupled and stays in App (AccountService territory).
//   (D10-2 state-holder cut — UI-decoupling M1; D11-3b algorithm cut.)
#pragma once

#include <set>
#include <string>
#include <vector>

#include "panicast/core/types.h" // TreeNodePtr
#include "panicast/core/utils.h" // D11-3b: Utils::to_lower (search_recursive / collect_context_matches)

namespace panicast
{

class SearchService {
public:
    // ── Search state (D10-2, moved out of App) ───────────────────────────────
    // Mutable refs for the query / match list (reassigned / push_back / cleared / indexed in place,
    //   like D8b-1's playlist()); const + setter for the two ints (like D8b-1's current_index()).
    std::string &search_query() {
        return search_query_;
    }
    const std::string &search_query() const {
        return search_query_;
    }
    std::vector<TreeNodePtr> &search_matches() {
        return search_matches_;
    }
    const std::vector<TreeNodePtr> &search_matches() const {
        return search_matches_;
    }
    int current_match_idx() const {
        return current_match_idx_;
    }
    void set_current_match_idx(int idx) {
        current_match_idx_ = idx;
    }
    int total_matches() const {
        return total_matches_;
    }
    void set_total_matches(int n) {
        total_matches_ = n;
    }

    // Clear all search state (new search / mode switch). The one pure piece of search logic.
    void reset();

    // ── Search algorithm (D11-3b — display-decoupled; operates on tree nodes + a cursor) ──
    // Recursive title-match: append every node in `node`'s subtree whose lowercased title
    //   contains `query` to `results`. Pure (no state, no display).
    static void search_recursive(const TreeNodePtr &node, const std::string &query,
                                 std::vector<TreeNodePtr> &results);
    // F20 context-aware match collection — the core of perform_search, verbatim. Given the
    //   current mode's top-level items (`roots`) and the cursor node, fills search_matches_ in
    //   priority order (peers same-type → peers diff-type → cursor subtree → global) and dedups,
    //   then sets total_matches_. Does NOT touch current_match_idx_ (the caller sets it) and does
    //   NOT lock (the caller holds tree_mutex — both `roots` and `cursor` are tree state).
    void collect_context_matches(const std::vector<TreeNodePtr> &roots, TreeNodePtr cursor,
                                 const std::string &ql);
    // jump_search cursor math: advance the match index by `dir` (±1), wrapping. Returns false
    //   when there are no matches (caller skips). Updates current_match_idx_.
    bool cycle_match(int dir);

    // D12-2: expand the ancestor chain of `node` so it appears in the flattened display_list.
    //   Pure tree mutation (sets expanded=true on the path roots→node); caller holds tree_mutex,
    //   same convention as collect_context_matches — this method never locks nor touches display.
    static void reveal_node(const std::vector<TreeNodePtr> &roots, const TreeNodePtr &node);

private:
    std::string search_query_;
    std::vector<TreeNodePtr> search_matches_;
    int current_match_idx_ = -1;
    int total_matches_ = 0;
};

} // namespace panicast
