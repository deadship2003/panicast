#include "panicast/app/app.h"

#include "panicast/app/online_state.h"
#include "panicast/core/event_log.h"

namespace panicast
{

// Y24.54: jump to the currently playing node — switch to its mode (without rebuilding the tree,
//   so node pointers stay valid), expand ancestors so it's visible, and let the run loop's
//   pending_select_ mechanism select + scroll to it.
void App::jump_to_playing() {
    auto pn = playback_.playback_node();
    if (!pn) {
        EVENT_LOG("Nothing playing — press N while a track is playing");
        return;
    }
    const auto pm = playback_.playback_mode();
    // Switch mode + root directly (skip switch_mode's load_*_root to preserve the tree state
    //   and node pointers — the tree is already built from the user's last visit).
    mode = pm;
    switch (pm) {
    case AppMode::RADIO:
        break;
    case AppMode::PODCAST:
        break;
    case AppMode::FAVOURITE:
        break;
    case AppMode::HISTORY:
        break;
    case AppMode::ONLINE:
        break;
    case AppMode::ACCOUNT:
        break;
    case AppMode::BILIBILI:
        break;
    case AppMode::TIKTOK:
        break;
    case AppMode::IPTV:
        break;
    }
    reset_search();
    library_.selected_idx() = 0;
    // Expand all ancestors so the node appears in the flattened display_list.
    {
        auto p = pn->parent.lock();
        while (p) {
            p->expanded = true;
            p = p->parent.lock();
        }
    }
    // pending_select_ is consumed by the run loop (app_run.cpp): it searches display_list for
    //   this node, sets selected_idx, and the existing view_start scroll makes it visible.
    library_.pending_select() = pn;
    EVENT_LOG(fmt::format("Jumped to playing: '{}'", pn->title));
}

void App::nav_page_down() {
    library_.selected_idx() += PAGE_SCROLL_LINES;
    if (library_.display_list().empty()) {
        library_.selected_idx() = 0;
        return;
    }
    if (library_.selected_idx() >= (int)library_.display_list().size())
        library_.selected_idx() = (int)library_.display_list().size() - 1;
}

void App::go_back() {
    if (library_.selected_idx() < 0 || library_.selected_idx() >= (int)library_.display_list().size())
        return;
    auto node = library_.display_list()[library_.selected_idx()].node;

    // Improved 'h' key behavior
    // If the current node is expandable and expanded, collapse first
    if ((node->type == NodeType::FOLDER || node->type == NodeType::PODCAST_FEED) &&
        node->expanded) {
        node->expanded = false;
        return;
    }

    // If the node is not expanded, try jumping to the parent node
    int parent = library_.display_list()[library_.selected_idx()].parent_idx;
    if (parent != -1) {
        library_.selected_idx() = parent;
    } else {
        // At the top-level node, check whether there are collapsible top-level nodes
        // Try to collapse all top-level children
        bool collapsed = false;
        for (auto &child : cur_items()) {
            if (child->expanded) {
                child->expanded = false;
                collapsed = true;
            }
        }
        if (!collapsed) {
            EVENT_LOG("Already at top level");
        }
    }
}

void App::enter_node(int marked_count) {
    // Y24.39: dispatch only — the per-case logic lives in enter_marked /
    //   enter_folder_expand / enter_favourite_folder / enter_leaf.
    if (marked_count > 0) {
        enter_marked(marked_count);
        return;
    }

    if (library_.selected_idx() < 0 || library_.selected_idx() >= (int)library_.display_list().size())
        return;
    auto node = library_.display_list()[library_.selected_idx()].node;
    if (!node)
        return;

    // P1.1 (Y23.5): hold tree_mutex for the entire node activation — enter_node mutates
    //   node->children/expanded/children_loaded (incl. cache-build blocks) which races with
    //   background spawn_load_feed callbacks + per-frame flatten. Recursive mutex → safe with
    //   spawn_load_feed/load_search_history_children which re-enter.
    std::lock_guard<std::recursive_mutex> enter_lock(library_.tree_mutex());

    // Y01: Y-mode node activation (account / history / subscriptions / channel).
    if (mode == AppMode::ACCOUNT) {
        enter_account_node(node);
        return;
    }

    if (node->type == NodeType::FOLDER || node->type == NodeType::PODCAST_FEED) {
        enter_folder_expand(node);
    } else if (!node->url.empty()) {
        enter_leaf(node);
    }
}

// Play the first marked episode (its peers become the list), then clear all marks.
void App::enter_marked(int marked_count) {
    (void)marked_count; // precondition: caller ensures marked_count > 0
    std::vector<TreeNodePtr> items;
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        collect_playable_marked_current(items);
    }
    if (!items.empty()) {
        play_episode(items[0]);
        EVENT_LOG(fmt::format("Play marked: '{}'", items[0]->title));
    }
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        clear_marks_current();
    }
    Persistence::save_cache(library_.radio_root(), library_.podcast_root());
}

// Expand or collapse a FOLDER / PODCAST_FEED node, dispatching by mode and cache state.
void App::enter_folder_expand(TreeNodePtr node) {
    if (!node->expanded) {
        // Detect whether this is a search-history node
        bool is_search_history_node = (node->url.find("search:") == 0);

        // Fix: check not only children_loaded but also whether children is empty
        // If children is empty but URL is non-empty (loadable), reload
        bool need_load = (!node->children_loaded || node->children.empty()) && !node->url.empty();

        if (need_load) {
            if (node->is_search_parent) {
                // Y23.1: Search History container — load past search records from cache.
                expand_search_history(node);
                node->expanded = true;
            } else if (is_search_history_node) {
                // Load search-history node children from cache
                load_search_history_children(node);
                node->expanded = true;
            } else if (mode == AppMode::RADIO) {
                spawn_load_radio(node);
            } else if (mode == AppMode::ONLINE) {
                // Online mode: prefer loading from the database cache
                if (node->type == NodeType::PODCAST_FEED &&
                    DatabaseManager::instance().is_episode_cached(node->url)) {
                    // Load episode list from the database cache
                    auto episodes = DatabaseManager::instance().load_episodes_from_cache(node->url);
                    if (!episodes.empty()) {
                        build_episode_children_from_cache(
                            node, episodes); // Y24.29: was inline (missing has_asr_srt)
                        node->children_loaded = true;
                        node->expanded = true;
                        EVENT_LOG(fmt::format("[Online] Loaded {} episodes from cache",
                                              node->children.size()));
                    } else {
                        spawn_load_feed(node);
                    }
                } else {
                    spawn_load_feed(node);
                }
            } else if (mode == AppMode::FAVOURITE) {
                enter_favourite_folder(node);
            } else if (mode == AppMode::IPTV) {
                // Y24.50: I-mode — lazy fetch+parse iptv-org m3u/json (async, cached).
                expand_iptv_node(node);
                node->expanded = true;
            } else if (mode == AppMode::PODCAST) {
                // ═══════════════════════════════════════════════════════════
                // PODCAST mode: prefer loading from the database cache
                // ═══════════════════════════════════════════════════════════
                if (node->type == NodeType::PODCAST_FEED &&
                    DatabaseManager::instance().is_episode_cached(node->url)) {
                    // Load episode list from the database cache
                    auto episodes = DatabaseManager::instance().load_episodes_from_cache(node->url);
                    if (!episodes.empty()) {
                        build_episode_children_from_cache(
                            node, episodes); // Y24.29: was inline (missing has_asr_srt)
                        node->children_loaded = true;
                        node->expanded = true;
                        EVENT_LOG(fmt::format("[Podcast] Loaded {} episodes from cache",
                                              node->children.size()));
                    } else {
                        spawn_load_feed(node);
                    }
                } else {
                    spawn_load_feed(node);
                }
            } else {
                // Other modes load from the network
                spawn_load_feed(node);
            }
        } else {
            node->expanded = true;
        }
    } else {
        // When the LINK node is collapsed, reset state so it reloads on next expand
        if (node->is_link) {
            node->children_loaded = false;
            node->children.clear();
        }
        node->expanded = false;
    }
}

// FAVOURITE-mode expansion: unified LINK mechanism (local folder / link / search / feed / folder).
void App::enter_favourite_folder(TreeNodePtr node) {
    // ═══════════════════════════════════════════════════════════
    // Unified LINK expansion mechanism
    // All favourite nodes are LINKs; use the unified expansion function
    // ═══════════════════════════════════════════════════════════
    if (node->is_local_folder) {
        // Local folder: scan audio/video files in the directory (node added via A key)
        expand_local_folder(node);
    } else if (node->is_link) {
        expand_link_node(node);
    } else if (node->url.find("search:") == 0) {
        // Search-record node: load from the database cache
        load_search_history_children(node);
        node->expanded = !node->children.empty();
    } else if (node->type == NodeType::PODCAST_FEED) {
        // Podcast feed: prefer loading from the database cache
        if (load_favourite_children_from_cache(node)) {
            node->expanded = true;
        } else {
            spawn_load_feed(node);
        }
    } else if (node->type == NodeType::FOLDER) {
        // Folder: prefer loading from local data, then online parsing
        URLType url_type = URLClassifier::classify(node->url);

        // First try loading from episode_cache (applies to podcast directories)
        if (DatabaseManager::instance().is_episode_cached(node->url)) {
            auto episodes = DatabaseManager::instance().load_episodes_from_cache(node->url);
            if (!episodes.empty()) {
                build_episode_children_from_cache(
                    node, episodes); // Y24.29: was inline (missing has_asr_srt)
                node->children_loaded = true;
                node->expanded = true;
                EVENT_LOG(
                    fmt::format("[Favourite] Loaded {} items from cache", node->children.size()));
            } else {
                // Cache empty; parse online
                if (url_type == URLType::OPML || node->url.find(".opml") != std::string::npos ||
                    node->url.find("Browse.ashx") != std::string::npos) {
                    spawn_load_radio(node);
                } else {
                    spawn_load_feed(node);
                }
            }
        } else {
            // No cache; parse online
            if (url_type == URLType::OPML || node->url.find(".opml") != std::string::npos ||
                node->url.find("Browse.ashx") != std::string::npos) {
                spawn_load_radio(node);
            } else {
                spawn_load_feed(node);
            }
        }
    } else {
        // Other types: prefer loading from cache
        if (load_favourite_children_from_cache(node)) {
            node->expanded = true;
        } else {
            spawn_load_feed(node);
        }
    }
}

// Playable leaf (episode / radio stream): play it. Its peers (siblings) become the
//   implicit playlist; playback advances per play_mode. (Caller guards url.empty().)
void App::enter_leaf(TreeNodePtr node) {
    if (is_playable_node(node)) {
        play_episode(node);
    }
}

void App::toggle_mark() {
    if (library_.selected_idx() < 0 || library_.selected_idx() >= (int)library_.display_list().size())
        return;
    auto node = library_.display_list()[library_.selected_idx()].node;
    if (!node)
        return; // null guard
    node->marked = !node->marked;
    EVENT_LOG(fmt::format("Mark: {}", node->marked ? "ON" : "OFF"));
    Persistence::save_cache(library_.radio_root(), library_.podcast_root());
}

// Supports two sorting scenarios:
// 1. Inside the L popup: sort current_playlist
// 2. Node tree: sort sibling nodes
// Sort rule: first time A->Z (digits first), second time Z->A (letters first)
// Digit-prefixed entries always sort first (at the head when forward, at the tail when reversed)

// Node tree sort
void App::toggle_sort_order() {
    if (library_.selected_idx() < 0 || library_.selected_idx() >= (int)library_.display_list().size())
        return;
    auto node = library_.display_list()[library_.selected_idx()].node;
    if (!node)
        return;

    // Find the parent; sort its children. Top-level items (no parent) sort the whole current
    //   mode list (cur_items()); nested items sort their parent's children. (E: no root node.)
    auto parent = node->parent.lock();
    bool sort_top = !parent;

    bool reversed = false;
    int n = 0;
    std::string scope_desc;
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        std::vector<TreeNodePtr> *targets = sort_top ? &cur_items() : &parent->children;
        n = (int)targets->size();
        if (n == 0) {
            EVENT_LOG("Sort: No siblings to sort");
            return;
        }
        if (sort_top) {
            cur_sort_reversed = !cur_sort_reversed;
            reversed = cur_sort_reversed;
        } else {
            parent->sort_reversed = !parent->sort_reversed;
            reversed = parent->sort_reversed;
        }
        if (reversed) {
            std::sort(targets->begin(), targets->end(),
                      [](const TreeNodePtr &a, const TreeNodePtr &b) {
                          return title_compare_desc(a->title, b->title);
                      });
        } else {
            std::sort(targets->begin(), targets->end(),
                      [](const TreeNodePtr &a, const TreeNodePtr &b) {
                          return title_compare_asc(a->title, b->title);
                      });
        }
        library_.display_list().clear();
        flatten_items(cur_items());
        scope_desc = sort_top ? (mode == AppMode::RADIO     ? "Radio List"
                                 : mode == AppMode::PODCAST ? "Podcast List"
                                                            : "Current List")
                              : (parent->title.empty() ? "current list" : parent->title);
    }

    EVENT_LOG(fmt::format("Sort [{}]: {} ({} items)", scope_desc, reversed ? "Z→A" : "A→Z", n));

    Persistence::save_cache(library_.radio_root(), library_.podcast_root());
}

} // namespace panicast
