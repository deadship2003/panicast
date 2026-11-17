#include "panicast/app/app.h"

namespace panicast
{

// Improved delete_node supporting all modes
// ONLINE mode delete enhanced: thoroughly cleans database records
// ONLINE/HISTORY mode supports multi-select delete
void App::delete_node(int marked_count) {
    if (library_.selected_idx() < 0 || library_.selected_idx() >= (int)library_.display_list().size())
        return;
    auto node = library_.display_list()[library_.selected_idx()].node;

    // ONLINE mode - supports multi-select delete
    if (mode == AppMode::ONLINE) {
        // Multi-select delete
        if (marked_count > 0) {
            std::string response =
                frontend_->dialog(fmt::format("Delete {} marked items? (Y/N)", marked_count));
            if (response != "Y" && response != "y")
                return;

            std::vector<TreeNodePtr> to_delete;
            {
                std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                collect_marked(OnlineState::instance().online_root, to_delete);
            }

            int deleted_count = 0;
            for (auto &n : to_delete) {
                // Execute different delete logic based on node type
                if (n->url.find("search:") == 0) {
                    // Search-record node
                    std::string search_id = n->url.substr(7);
                    size_t pos = search_id.find(':');
                    if (pos != std::string::npos) {
                        // Fix URL format parsing - search:region:query
                        std::string region = search_id.substr(0, pos);
                        std::string query = search_id.substr(pos + 1);
                        DatabaseManager::instance().delete_search_history(query, region);
                    }
                    // Delete child-node caches
                    for (auto &child : n->children) {
                        if (child->type == NodeType::PODCAST_FEED && !child->url.empty()) {
                            DatabaseManager::instance().delete_podcast_cache(child->url);
                            DatabaseManager::instance().delete_episode_cache_by_feed(child->url);
                        }
                    }
                } else if (n->type == NodeType::PODCAST_FEED && !n->url.empty()) {
                    // Podcast feed node
                    DatabaseManager::instance().delete_podcast_cache(n->url);
                    DatabaseManager::instance().delete_episode_cache_by_feed(n->url);
                }

                // Delete from the tree
                {
                    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                    remove_node(OnlineState::instance().online_root, n);
                }
                deleted_count++;
            }

            // Clear all marks
            {
                std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                clear_marks(OnlineState::instance().online_root);
            }

            EVENT_LOG(fmt::format("Deleted {} items from ONLINE", deleted_count));
            if (library_.selected_idx() > 0)
                library_.selected_idx()--;
            return;
        }

        // Single-select delete - decide by node type
        if (node->url.find("search:") == 0) {
            // Search-record node - delete the whole search record and its children's caches
            std::string response = frontend_->dialog("Delete this search record and all caches? (Y/N)");
            if (response == "Y" || response == "y") {
                // Fix URL format parsing - search:region:query
                std::string search_id = node->url.substr(7); // strip "search:"
                size_t pos = search_id.find(':');
                if (pos != std::string::npos) {
                    std::string region = search_id.substr(0, pos);
                    std::string query = search_id.substr(pos + 1);
                    DatabaseManager::instance().delete_search_history(query, region);
                }

                // Delete all child nodes' podcast caches and episode caches
                for (auto &child : node->children) {
                    if (child->type == NodeType::PODCAST_FEED && !child->url.empty()) {
                        DatabaseManager::instance().delete_podcast_cache(child->url);
                        DatabaseManager::instance().delete_episode_cache_by_feed(child->url);
                    }
                }

                // Delete the node from the tree
                {
                    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                    remove_node(OnlineState::instance().online_root, node);
                }
                EVENT_LOG(fmt::format("Deleted search record and caches: {}", node->title));
                if (library_.selected_idx() > 0)
                    library_.selected_idx()--;
            }
        } else if (node->type == NodeType::PODCAST_FEED) {
            // Podcast feed node - delete podcast cache and episode cache
            std::string response = frontend_->dialog("Delete this podcast cache? (Y/N)");
            if (response == "Y" || response == "y") {
                // Delete database records
                if (!node->url.empty()) {
                    DatabaseManager::instance().delete_podcast_cache(node->url);
                    DatabaseManager::instance().delete_episode_cache_by_feed(node->url);
                }
                // Delete the node from the tree
                {
                    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                    remove_node(OnlineState::instance().online_root, node);
                }
                EVENT_LOG(fmt::format("Deleted podcast cache: {}", node->title));
                if (library_.selected_idx() > 0)
                    library_.selected_idx()--;
            }
        } else {
            // Other nodes - delete from the tree only
            std::string response = frontend_->dialog("Delete this item? (Y/N)");
            if (response == "Y" || response == "y") {
                {
                    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                    remove_node(OnlineState::instance().online_root, node);
                }
                EVENT_LOG(fmt::format("Deleted: {}", node->title));
                if (library_.selected_idx() > 0)
                    library_.selected_idx()--;
            }
        }
        return;
    }

    // HISTORY mode - supports multi-select delete
    if (mode == AppMode::HISTORY) {
        // Multi-select delete
        if (marked_count > 0) {
            std::string response =
                frontend_->dialog(fmt::format("Delete {} marked history records? (Y/N)", marked_count));
            if (response != "Y" && response != "y")
                return;

            std::vector<TreeNodePtr> to_delete;
            {
                std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                collect_marked_current(to_delete);
            }

            int deleted_count = 0;
            for (auto &n : to_delete) {
                // Delete from database
                if (!n->url.empty()) {
                    DatabaseManager::instance().delete_history(n->url);
                }
                // Delete from tree
                {
                    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                    remove_from_current(n);
                }
                deleted_count++;
            }

            // Clear all marks
            {
                std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                clear_marks_current();
            }

            EVENT_LOG(fmt::format("Deleted {} history records", deleted_count));
            if (library_.selected_idx() > 0)
                library_.selected_idx()--;
            return;
        }

        // Single-select delete
        std::string response = frontend_->dialog("Delete this history record? (Y/N)");
        if (response == "Y" || response == "y") {
            // Delete from database
            if (!node->url
                     .empty()) { // empty URL would mistakenly delete all empty-string history rows
                DatabaseManager::instance().delete_history(node->url);
            }
            // Delete the node from the tree
            {
                std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                remove_from_current(node);
            }
            EVENT_LOG(fmt::format("Deleted history: {}", node->title));
            if (library_.selected_idx() > 0)
                library_.selected_idx()--;
        }
        return;
    }

    // PODCAST/FAVOURITE mode
    if (mode != AppMode::PODCAST && mode != AppMode::FAVOURITE)
        return;

    // FAVOURITE mode - handle bidirectional sync delete of LINK nodes
    if (mode == AppMode::FAVOURITE) {
        // Check whether operating under a LINK node
        TreeNodePtr parent_link = nullptr;
        for (auto &f : library_.fav_root()) {
            if (f->is_link) {
                // Check whether node is a child of f or f itself
                if (f.get() == node.get()) {
                    // Deleting the LINK node itself - remove the favourite directly
                    std::string response = frontend_->dialog("Remove this LINK from favourites? (Y/N)");
                    if (response == "Y" || response == "y") {
                        {
                            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                            auto it = std::remove_if(library_.fav_root().begin(), library_.fav_root().end(),
                                                     [&](auto &n) { return n == node; });
                            library_.fav_root().erase(it, library_.fav_root().end());
                        }
                        DatabaseManager::instance().delete_favourite(node->url);
                        EVENT_LOG(fmt::format("Removed LINK: {}", node->title));
                        if (library_.selected_idx() > 0)
                            library_.selected_idx()--;
                    }
                    return;
                }
                // Check whether node is a child of f
                for (auto &child : f->children) {
                    if (child.get() == node.get()) {
                        parent_link = f;
                        break;
                    }
                }
                if (parent_link)
                    break;
            }
        }

        // If deleting a child node under a LINK node, sync to ONLINE
        if (parent_link && parent_link->url == "online_root") {
            // This is online_root's LINK; deletion needs to sync
            std::string response = frontend_->dialog("Delete from both FAVOURITE and ONLINE? (Y/N)");
            if (response != "Y" && response != "y")
                return;

            // Delete from ONLINE's online_root
            {
                std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                remove_node(OnlineState::instance().online_root, node);
            }

            // If it is a search record, delete the database record
            if (node->url.find("search:") == 0) {
                std::string search_id = node->url.substr(7);
                size_t pos = search_id.find(':');
                if (pos != std::string::npos) {
                    std::string region = search_id.substr(0, pos);
                    std::string query = search_id.substr(pos + 1);
                    DatabaseManager::instance().delete_search_history(query, region);
                }
            }

            // Clear the LINK node's children; re-sync on next expand
            parent_link->children_loaded = false;
            parent_link->children.clear();

            EVENT_LOG(fmt::format("Deleted from both: {}", node->title));
            if (library_.selected_idx() > 0)
                library_.selected_idx()--;
            return;
        }
    }

    // Support multi-select batch delete of subscriptions
    if (marked_count > 0) {
        std::vector<TreeNodePtr> to_delete;
        {
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            collect_marked_current(to_delete);
        }

        if (to_delete.empty())
            return;

        std::string response =
            frontend_->dialog(fmt::format("Delete {} subscriptions? (Y/N)", to_delete.size()));
        if (response != "Y" && response != "y")
            return;

        int deleted_count = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            for (auto &n : to_delete) {
                // If PODCAST_FEED, delete subscription and cache
                if (n->type == NodeType::PODCAST_FEED && !n->url.empty()) {
                    DatabaseManager::instance().delete_podcast_cache(n->url);
                    DatabaseManager::instance().delete_episode_cache_by_feed(n->url);
                    // Record the deleted subscription to prevent built-in defaults from reviving on restart
                    DatabaseManager::instance().add_removed_default(n->url);
                }
                remove_from_current(n);
                deleted_count++;
            }
            clear_marks_current();
        }

        EVENT_LOG(fmt::format("Deleted {} subscriptions", deleted_count));
        save_persistent_data();
        Persistence::save_cache(library_.radio_root(), library_.podcast_root());
        if (library_.selected_idx() > 0)
            library_.selected_idx()--;
        return;
    }

    if (node->type == NodeType::PODCAST_FEED) {
        std::string response = frontend_->dialog("Delete: (S)ubscription / (C)ache / (N)o?");
        if (response == "S" || response == "s") {
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            if (!node->url.empty())
                DatabaseManager::instance().add_removed_default(node->url);
            remove_from_current(node);
            EVENT_LOG(fmt::format("Deleted subscription: {}", node->title));
        } else if (response == "C" || response == "c") {
            clear_feed_cache(node);
            EVENT_LOG(fmt::format("Cleared cache for: {}", node->title));
        }
    } else if (node->type == NodeType::PODCAST_EPISODE) {
        std::string response = frontend_->dialog("Clear cache? (Y/N)");
        if (response == "Y" || response == "y") {
            CacheManager::instance().clear_download(node->url);
            node->is_downloaded = false;
            node->local_file.clear();
            EVENT_LOG(fmt::format("Cleared cache: {}", node->title));
        }
    } else {
        std::vector<TreeNodePtr> to_delete;
        {
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            if (marked_count > 0)
                collect_marked_current(to_delete);
            else
                to_delete.push_back(node);
        }
        if (to_delete.empty())
            return;
        {
            std::string response =
                frontend_->dialog(fmt::format("Delete {} items? (Y/N)", to_delete.size()));
            if (response != "Y" && response != "y")
                return; // accept Y/y, consistent with the rest
        }
        {
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            for (auto &n : to_delete)
                remove_from_current(n);
            clear_marks_current();
        }
        EVENT_LOG(fmt::format("Deleted {} items", to_delete.size()));
    }

    save_persistent_data();
    Persistence::save_cache(library_.radio_root(), library_.podcast_root());
    if (library_.selected_idx() > 0)
        library_.selected_idx()--;
}

void App::clear_feed_cache(TreeNodePtr feed) {
    for (auto &child : feed->children) {
        DatabaseManager::instance().delete_episode_cache_by_feed(child->url);
        CacheManager::instance().clear_download(child->url);
        child->is_cached = false;
        child->is_downloaded = false;
        child->local_file.clear();
    }
    feed->is_cached = false;
    DatabaseManager::instance().delete_episode_cache_by_feed(feed->url);
}

// Fix multi-select marking - supports all node types
void App::collect_playable_marked(const TreeNodePtr &node, std::vector<TreeNodePtr> &list) {
    if (node->marked) {
        // Directly playable types
        if (node->type == NodeType::RADIO_STREAM || node->type == NodeType::PODCAST_EPISODE) {
            list.push_back(node);
            return; // collected; do not recurse (avoid duplication below)
        }
        // Folder/Feed types - recursively collect playable items among children
        else if (node->type == NodeType::FOLDER || node->type == NodeType::PODCAST_FEED) {
            for (auto &child : node->children) {
                collect_playable_items(child, list);
            }
            return; // collected all playable descendants; do not recurse collect_playable_marked
        }
    }
    // Unmarked node: keep looking for marked nodes in the subtree
    for (auto &child : node->children) {
        collect_playable_marked(child, list);
    }
}

// Helper - collect all playable items in a node (regardless of marked state)
void App::collect_playable_items(const TreeNodePtr &node, std::vector<TreeNodePtr> &list) {
    if (node->type == NodeType::RADIO_STREAM || node->type == NodeType::PODCAST_EPISODE) {
        list.push_back(node);
    }
    for (auto &child : node->children) {
        collect_playable_items(child, list);
    }
}

void App::clear_marks(const TreeNodePtr &node) {
    node->marked = false;
    for (auto &child : node->children)
        clear_marks(child);
}

void App::clear_all_marks() {
    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
    clear_marks_current();
    EVENT_LOG("All marks cleared");
    Persistence::save_cache(library_.radio_root(), library_.podcast_root());
}

// Enhanced Visual multi-select - supports all node types in all modes
void App::confirm_visual_selection() {
    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
    int start = std::min(visual_start_, library_.selected_idx());
    int end = std::max(visual_start_, library_.selected_idx());
    if (start < 0)
        start = 0; // guard against negative-index overflow (visual_start_/selected_idx may be -1)
    if (library_.display_list().empty()) {
        visual_mode_ = false;
        visual_start_ = -1;
        return;
    }
    int count = 0;

    for (int i = start; i <= end && i < (int)library_.display_list().size(); ++i) {
        auto node = library_.display_list()[i].node;
        if (!node)
            continue; // null guard
        // Support multi-select for all node types
        // RADIO_STREAM/PODCAST_EPISODE: for batch download/delete
        // FOLDER/PODCAST_FEED: for batch delete (ONLINE/FAVOURITE mode)
        // Do not mark folders with empty URLs (no practical operation)
        if (node->type == NodeType::RADIO_STREAM || node->type == NodeType::PODCAST_EPISODE ||
            node->type == NodeType::FOLDER || node->type == NodeType::PODCAST_FEED) {
            node->marked = true;
            count++;
        }
    }

    visual_mode_ = false;
    visual_start_ = -1;
    EVENT_LOG(fmt::format("Marked {} items", count));
    Persistence::save_cache(library_.radio_root(), library_.podcast_root());
}

bool App::remove_node(TreeNodePtr parent, TreeNodePtr target) {
    auto &children = parent->children;
    auto it =
        std::remove_if(children.begin(), children.end(), [&](auto &n) { return n == target; });
    if (it != children.end()) {
        children.erase(it, children.end());
        return true;
    }
    for (auto &child : parent->children)
        if (remove_node(child, target))
            return true;
    return false;
}

// ── E: mode-level helpers (operate on cur_items(); the root container is gone) ──
int App::count_marked_current() {
    int n = 0;
    for (auto &it : cur_items())
        n += count_marked_safe(it);
    return n;
}
void App::clear_marks_current() {
    for (auto &it : cur_items())
        clear_marks(it);
}
void App::collect_marked_current(std::vector<TreeNodePtr> &list) {
    for (auto &it : cur_items())
        collect_marked(it, list);
}
void App::collect_playable_marked_current(std::vector<TreeNodePtr> &list) {
    for (auto &it : cur_items())
        collect_playable_marked(it, list);
}
bool App::remove_from_current(TreeNodePtr target) {
    auto &items = cur_items();
    auto it = std::find(items.begin(), items.end(), target);
    if (it != items.end()) {
        items.erase(it);
        return true;
    }
    for (auto &top : items)
        if (remove_node(top, target))
            return true;
    return false;
}

// Edit node title and URL
void App::edit_node() {
    if (library_.selected_idx() < 0 || library_.selected_idx() >= (int)library_.display_list().size())
        return;
    auto node = library_.display_list()[library_.selected_idx()].node;

    // Only allow editing podcast feed sources and custom nodes
    if (node->type != NodeType::PODCAST_FEED && node->type != NodeType::RADIO_STREAM) {
        EVENT_LOG("Can only edit feed/stream nodes");
        return;
    }

    // Get the new title (show the current value as default)
    std::string new_title = frontend_->input_box("Title:", node->title);
    if (new_title.empty())
        new_title = node->title; // keep original value

    // Get the new URL (show the current value as default)
    std::string new_url = frontend_->input_box("URL:", node->url);
    if (new_url.empty())
        new_url = node->url; // keep original value

    // Update the node
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        node->title = new_title;
        node->url = new_url;
    }

    EVENT_LOG(fmt::format("Updated: {} -> {}", new_title, new_url.substr(0, 50)));
    Persistence::save_cache(library_.radio_root(), library_.podcast_root());
}

// Y24.27: Extracted from 7 duplicated sites in app_tree_expand.cpp + app_navigation.cpp.
//   Builds episode child nodes from a DB cache tuple vector. is_opml=true uses RADIO_STREAM
//   classification (OPML children may be streams); otherwise YouTube channel/playlist → feed,
//   everything else → episode.
void App::build_episode_children_from_cache(
    TreeNodePtr parent,
    const std::vector<std::tuple<std::string, std::string, int, bool, bool, std::string, bool,
                                 std::string>> &episodes,
    bool is_opml) {
    parent->children.clear();
    for (const auto &[ep_url, ep_title, ep_duration, ep_is_youtube, ep_has_sub, ep_sub_url,
                      ep_has_asr, ep_asr_path] : episodes) {
        auto child = std::make_shared<TreeNode>();
        child->title = ep_title.empty() ? "Untitled" : ep_title;
        child->url = ep_url;
        child->duration = ep_duration;
        if (is_opml) {
            URLType cut = URLClassifier::classify(ep_url);
            child->type = (cut == URLType::RADIO_STREAM || cut == URLType::VIDEO_FILE)
                              ? NodeType::RADIO_STREAM
                              : NodeType::PODCAST_EPISODE;
            child->children_loaded = true;
        } else {
            URLType cut = URLClassifier::classify(ep_url);
            if (cut == URLType::YOUTUBE_CHANNEL || cut == URLType::YOUTUBE_PLAYLIST) {
                child->type = NodeType::PODCAST_FEED;
                child->children_loaded = false;
            } else {
                child->type = NodeType::PODCAST_EPISODE;
                child->children_loaded = true;
            }
        }
        child->is_db_cached = true;
        child->parent = parent;
        child->is_youtube = ep_is_youtube;
        child->has_subtitle = ep_has_sub;
        child->subtitle_url = ep_sub_url;
        child->has_asr_srt = ep_has_asr;
        child->asr_srt_path = ep_asr_path;
        parent->children.push_back(child);
    }
}

void App::refresh_node() {
    if (library_.selected_idx() < 0 || library_.selected_idx() >= (int)library_.display_list().size())
        return;
    auto node = library_.display_list()[library_.selected_idx()].node;

    if (node->is_local_folder) {
        // CACHE-1: 'r' on a local folder → force re-scan the directory and rewrite the cache.
        node->children.clear();
        node->children_loaded = false;
        expand_local_folder(node, /*force=*/true);
        EVENT_LOG(fmt::format("Refreshing local folder: {}", node->url));
    } else if (node->type == NodeType::FOLDER && mode == AppMode::RADIO) {
        node->children.clear();
        node->children_loaded = false;
        node->loading = true;
        node->parse_failed = false;
        spawn_load_radio(node, true);
    } else if (node->type == NodeType::PODCAST_FEED) {
        node->children.clear();
        node->children_loaded = false;
        node->loading = false; // must set false first: spawn_load_feed's first-line guard
                               //   if(node->loading) return; would make refresh a no-op
        node->parse_failed = false;
        spawn_load_feed(node);
        // Print the full URL
        EVENT_LOG(fmt::format("Refreshing: {}", node->url));
    } else if (node->type == NodeType::PODCAST_EPISODE) {
        // Y24.26: 'r' on an episode is a no-op (episodes don't need refreshing; use 'r' on the feed).
        EVENT_LOG(fmt::format("Use 'r' on the feed to refresh, not on individual episodes: {}",
                              node->title));
    } else if (mode == AppMode::TIKTOK && node->type == NodeType::FOLDER && !node->url.empty()) {
        // T-fix: 'r' on a TikTok/Douyin creator (or #tag) node → re-fetch its video list online
        //   (yt-dlp --flat-playlist) and replace the in-memory children + the episode_cache entry.
        //   Local data may be stale; spawn_load_feed → parse_feed_by_type(TIKTOK_USER) always
        //   fetches online, and commit_feed_result replaces children + save_episode_cache (DEL+INS).
        node->children.clear();
        node->children_loaded = false;
        node->loading = false; // spawn_load_feed's first-line guard is `if (node->loading) return;`
        node->parse_failed = false;
        spawn_load_feed(node);
        EVENT_LOG(fmt::format("T: refreshing creator video list: {}", node->title));
    }
}

} // namespace panicast
