#include "panicast/app/app.h"

namespace panicast
{

void App::mark_cached_nodes(TreeNodePtr node) {
    // Download status from in-memory media_cache (fast). "Feed parsed" (is_cached) is NOT
    //   set here — it's set lazily on expand (is_episode_cached) / on parse, onto node->is_cached.
    if (CacheManager::instance().is_downloaded(node->url)) {
        node->is_downloaded = true;
        std::string local = CacheManager::instance().get_local_file(node->url);
        if (!local.empty())
            node->local_file = local;
    }
    for (auto &child : node->children) {
        mark_cached_nodes(child);
    }
}

// Get the corresponding mode's items vector by source_mode string (E: was a TreeNodePtr root)
std::vector<TreeNodePtr> *App::get_root_by_mode_string(const std::string &mode_str) {
    if (mode_str == "RADIO")
        return &library_.radio_root();
    if (mode_str == "PODCAST")
        return &library_.podcast_root();
    if (mode_str == "ONLINE")
        return &OnlineState::instance().online_root->children;
    if (mode_str == "FAVOURITE")
        return &library_.fav_root();
    if (mode_str == "HISTORY")
        return &library_.history_root();
    return nullptr;
}

// Select the parse method based on source_mode and type
// Returns: true = use spawn_load_radio, false = use spawn_load_feed
bool App::should_use_radio_loader(const std::string &source_mode, NodeType node_type,
                                  const std::string &url) {
    // Priority 1: source_mode decides the main parser
    if (source_mode == "RADIO") {
        return true; // RADIO mode uniformly uses the radio parser
    }
    if (source_mode == "PODCAST") {
        return false; // PODCAST mode uniformly uses the feed parser
    }

    // Priority 2: ONLINE mode decides by node_type
    if (source_mode == "ONLINE") {
        // ONLINE search results may be radio folders or podcast feeds
        if (node_type == NodeType::FOLDER) {
            return true; // folder type uses the radio parser
        }
        return false; // other types use the feed parser
    }

    // Priority 3: decide by URL type (fallback)
    URLType url_type = URLClassifier::classify(url);
    if (url_type == URLType::OPML || url.find(".opml") != std::string::npos ||
        url.find("Browse.ashx") != std::string::npos ||
        url.find("Tune.ashx") != std::string::npos) {
        return true;
    }

    // Priority 4: decide by node type
    if (node_type == NodeType::FOLDER) {
        return true;
    }

    return false;
}

// Sync LINK node status (checks both library_.fav_root() and library_.podcast_root())
// When the target node finishes loading, sync all LINK nodes referencing it
// This function is called after spawn_load_feed/spawn_load_radio completes
void App::sync_link_node_status(TreeNodePtr target) {
    if (!target || !target->children_loaded)
        return;

    // Traverse favourite_root and library_.podcast_root() to find all LINK nodes referencing this target
    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());

    std::function<void(TreeNodePtr)> sync_children = [&](TreeNodePtr parent) {
        for (auto &child : parent->children) {
            if (child->is_link) {
                // D14-5: match the LINK→target relation by URL identity (link_target_url), not by
                //   pointer equality of the runtime weak_ptr cache. The weak_ptr expires when the
                //   target is destroyed + rebuilt by URL (expand_link_node Step 3); a pointer-only
                //   match would then miss the rebuild. Pointer equality kept as a fast path (e.g.
                //   online_root, whose target url may not equal the synthetic "online_root" key) so
                //   this is strictly a superset of the old condition — no regression.
                auto linked = child->linked_node.lock();
                std::string lt =
                    child->link_target_url.empty() ? child->url : child->link_target_url;
                if ((linked && linked.get() == target.get()) || lt == target->url) {
                    child->linked_node = target; // refresh runtime cache to the resolved target
                    // Found a LINK node referencing this target; sync status
                    child->loading = false;
                    child->children_loaded = true;
                    child->parse_failed = target->parse_failed;
                    child->error_msg = target->error_msg;

                    // Sync children
                    if (!target->children.empty()) {
                        child->children.clear();
                        for (auto &tc : target->children) {
                            // Do not reset tc->parent — these children are shared between target
                            //   and LINK; resetting would break target's parent-pointer invariant
                            child->children.push_back(tc);
                        }
                    }

                    EVENT_LOG(fmt::format("[LINK] Synced status from target: {}", child->title));
                }
            }
            // Recurse into children
            sync_children(child);
        }
    };

    // Check both library_.fav_root() and library_.podcast_root()
    // Because a YouTube channel may be subscribed directly in PODCAST mode or favourited in FAVOURITE mode
    for (auto &it : library_.fav_root())
        sync_children(it);
    for (auto &it : library_.podcast_root())
        sync_children(it);
}

bool App::expand_link_node(TreeNodePtr node) {
    if (!node || !node->is_link)
        return false;

    std::string target_url = node->link_target_url.empty() ? node->url : node->link_target_url;

    // Ensure loading state is correctly initialized
    // If target is already loaded, ensure the LINK node's loading is false
    TreeNodePtr early_target = node->linked_node.lock();
    if (early_target && early_target->children_loaded && !early_target->children.empty()) {
        node->loading = false;
    }

    // ─────────────────────────────────────────────────────────────────
    // Step 1: find the real node (target)
    // ─────────────────────────────────────────────────────────────────
    TreeNodePtr target = node->linked_node.lock();

    // Try an in-memory lookup
    if (!target) {
        std::vector<TreeNodePtr> *search_root = get_root_by_mode_string(node->source_mode);

        // Special case: online_root
        if (target_url == "online_root") {
            if (!OnlineState::instance().history_loaded) {
                OnlineState::instance().load_search_history();
            }
            target = OnlineState::instance().online_root;
        }
        // Special case: search-record node
        else if (target_url.find("search:") == 0) {
            // Search-record nodes load directly into the LINK node; no real node needed
            load_search_history_children(node);
            node->expanded = !node->children.empty();
            node->loading = false; // ensure loading state is updated
            EVENT_LOG(fmt::format("[LINK] Loaded search record: {}", target_url));
            return !node->children.empty();
        }
        // Normal node: look up in the corresponding root
        else if (search_root) {
            if (node->source_mode == "ONLINE") {
                // ONLINE mode must ensure history is loaded
                if (!OnlineState::instance().history_loaded) {
                    OnlineState::instance().load_search_history();
                }
            }
            for (auto &it : *search_root) {
                target = find_node_by_url(it, target_url);
                if (target)
                    break;
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Step 2: if the real node is found, check whether loading is needed
    // ─────────────────────────────────────────────────────────────────
    if (target) {
        // Update the runtime reference
        node->linked_node = target;

        // Check whether the real node has children loaded
        if (target->children_loaded && !target->children.empty()) {
            // Real node already loaded; sync children directly to the LINK
            {
                std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                node->children.clear();
                for (auto &child : target->children) {
                    node->children.push_back(
                        child); // do not reset parent: child is shared with target; resetting would break target's parent-pointer invariant
                }
            }
            node->children_loaded = true;
            node->expanded = true;
            node->loading = false; // ensure loading state is updated
            EVENT_LOG(fmt::format("[LINK] Synced from target: {} items", node->children.size()));
            return true;
        }

        // ─────────────────────────────────────────────────────────────────
        // Key fix: the real node is not loaded; trigger parsing on the real node
        // Data will be cached under the real node, not the LINK node
        // Use should_use_radio_loader to select the parse method
        // ─────────────────────────────────────────────────────────────────
        EVENT_LOG(
            fmt::format("[LINK] Target not loaded, loading on target: {} (source_mode={}, type={})",
                        target->title, node->source_mode, (int)target->type));

        // Select parse method based on source_mode and type
        if (should_use_radio_loader(node->source_mode, target->type, target_url)) {
            // Radio/folder type: load on the real node
            spawn_load_radio(target);
            node->loading = true;
            EVENT_LOG(fmt::format("[LINK] Loading RADIO on target: {}", target_url));
            return true;
        }

        // Podcast type: prefer loading from the database cache into the real node
        if (DatabaseManager::instance().is_episode_cached(target_url)) {
            auto episodes = DatabaseManager::instance().load_episodes_from_cache(target_url);
            if (!episodes.empty()) {
                {
                    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                    build_episode_children_from_cache(target, episodes); // Y24.29: was inline
                }
                target->children_loaded = true;
                target->is_db_cached = true;
                // Sync to the LINK node
                {
                    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                    node->children.clear();
                    for (auto &child : target->children) {
                        node->children.push_back(
                            child); // do not reset parent: see above, consistent with sync_link_node_status
                    }
                }
                node->children_loaded = true;
                node->expanded = true;
                node->loading = false; // ensure loading state is updated
                EVENT_LOG(
                    fmt::format("[LINK] Loaded {} episodes from cache to target", episodes.size()));
                return true;
            }
        }

        // Cache miss; trigger network loading on the real node
        spawn_load_feed(target);
        node->loading = true;
        EVENT_LOG(fmt::format("[LINK] Loading PODCAST on target: {}", target_url));
        return true;
    }

    // ─────────────────────────────────────────────────────────────────
    // Step 3: real node not found; try creating and loading at the original location
    // This happens when the real node existed at favourite time but was later deleted
    // ─────────────────────────────────────────────────────────────────

    // Special case: online_root
    if (target_url == "online_root") {
        if (!OnlineState::instance().history_loaded) {
            OnlineState::instance().load_search_history();
        }
        {
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            node->children.clear();
            for (auto &child : OnlineState::instance().online_root->children) {
                node->children.push_back(
                    child); // do not reset parent: online_root children are shared; resetting breaks their parent-pointer invariant
            }
        }
        node->children_loaded = true;
        node->expanded = true;
        node->loading = false; // ensure loading state is updated
        node->linked_node = OnlineState::instance().online_root;
        EVENT_LOG("[LINK] Loaded from online_root");
        return true;
    }

    // Create a new node under the original root and load it
    // The newly created node becomes the "real node"; the LINK references it
    TreeNodePtr new_target = std::make_shared<TreeNode>();
    new_target->title = node->title;
    new_target->url = target_url;
    new_target->type = node->type;
    new_target->is_youtube = node->is_youtube;
    new_target->channel_name = node->channel_name;

    // Add the new node to the corresponding root
    std::vector<TreeNodePtr> *target_root = get_root_by_mode_string(node->source_mode);
    if (target_root) {
        {
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            new_target->parent.reset(); // E: top-level (no root node)
            target_root->insert(target_root->begin(), new_target);
        }
        // Update the LINK reference
        node->linked_node = new_target;

        // Use should_use_radio_loader to select the parse method
        if (should_use_radio_loader(node->source_mode, node->type, target_url)) {
            spawn_load_radio(new_target);
            EVENT_LOG(fmt::format("[LINK] Created new RADIO target: {}", target_url));
        } else {
            spawn_load_feed(new_target);
            EVENT_LOG(fmt::format("[LINK] Created new PODCAST target: {}", target_url));
        }
        node->loading = true;
        return true;
    }

    // Last fallback: load directly on the LINK node (not recommended, but keeps functionality working)
    EVENT_LOG(fmt::format("[LINK] Fallback: loading directly on LINK node: {}", target_url));

    // Use should_use_radio_loader to select the parse method
    if (should_use_radio_loader(node->source_mode, node->type, target_url)) {
        spawn_load_radio(node);
    } else if (load_favourite_children_from_cache(node)) {
        node->expanded = true;
        return true;
    } else {
        spawn_load_feed(node);
    }
    return true;
}

// Recursively find a node whose URL matches
TreeNodePtr App::find_node_by_url(TreeNodePtr root, const std::string &url) {
    if (!root)
        return nullptr;
    if (root->url == url)
        return root;

    for (auto &child : root->children) {
        auto found = find_node_by_url(child, url);
        if (found)
            return found;
    }
    return nullptr;
}

// Load favourite-referenced child nodes from the database cache (reference mode)
// For ONLINE/RADIO/PODCAST/HISTORY mode favourites, expand loads child nodes from the database cache
// This makes the favourite a "soft link" that always points to the latest data
bool App::load_favourite_children_from_cache(TreeNodePtr node) {
    if (!node || node->url.empty())
        return false;

    std::string url = node->url;
    URLType url_type = URLClassifier::classify(url);

    // ═══════════════════════════════════════════════════════════════════════════
    // Includes: podcast feeds, YouTube channels, OPML directories, radio streams, etc.
    // ═══════════════════════════════════════════════════════════════════════════

    // Prefer loading from episode_cache (applies to podcast feeds, YouTube channels, etc.)
    if (url_type == URLType::RSS_PODCAST || url_type == URLType::YOUTUBE_RSS ||
        url_type == URLType::YOUTUBE_CHANNEL || url_type == URLType::APPLE_PODCAST ||
        url_type == URLType::YOUTUBE_PLAYLIST) {

        // Check whether podcast_cache exists
        if (!DatabaseManager::instance().is_podcast_cached(url)) {
            EVENT_LOG(fmt::format("[Favourite] Podcast cache not found: {}", url));
            return false; // return false so the caller tries online parsing
        }

        // Load episode list from episode_cache
        auto episodes = DatabaseManager::instance().load_episodes_from_cache(url);
        if (episodes.empty()) {
            EVENT_LOG(fmt::format("[Favourite] No episodes cached for: {}", url));
            return false;
        }

        // Build child nodes
        build_episode_children_from_cache(node, episodes); // Y24.29: was inline

        node->children_loaded = true;
        node->parse_failed = false;
        EVENT_LOG(fmt::format("[Favourite] Loaded {} episodes from cache", node->children.size()));
        return true;
    }

    // OPML/radio directory types: check whether there are cached child nodes
    if (url_type == URLType::OPML || url.find(".opml") != std::string::npos ||
        url.find("Browse.ashx") != std::string::npos ||
        url.find("Tune.ashx") != std::string::npos) {

        // OPML node tries to load from episode_cache (some OPMLs may already be cached)
        if (DatabaseManager::instance().is_episode_cached(url)) {
            auto episodes = DatabaseManager::instance().load_episodes_from_cache(url);
            if (!episodes.empty()) {
                build_episode_children_from_cache(node, episodes); // Y24.29: was inline

                node->children_loaded = true;
                node->parse_failed = false;
                EVENT_LOG(fmt::format("[Favourite] Loaded {} OPML items from cache",
                                      node->children.size()));
                return true;
            }
        }

        // OPML cache miss; return false so the caller parses online
        EVENT_LOG(fmt::format("[Favourite] OPML cache not found: {}", url));
        return false;
    }

    // Other types: try loading from episode_cache
    if (DatabaseManager::instance().is_episode_cached(url)) {
        auto episodes = DatabaseManager::instance().load_episodes_from_cache(url);
        if (!episodes.empty()) {
            build_episode_children_from_cache(node, episodes); // Y24.29: was inline

            node->children_loaded = true;
            node->parse_failed = false;
            EVENT_LOG(fmt::format("[Favourite] Loaded {} items from cache (generic)",
                                  node->children.size()));
            return true;
        }
    }

    return false;
}

// Expand a local-folder node: scan audio/video files + subfolders in the directory as child nodes.
// Subdirectories become FOLDER children with is_local_folder=true (lazy: expanding calls this function recursively again);
// media files become PODCAST_EPISODE children whose url is the absolute path, uniformly played via MPV, marked as cached (green).
// Skip symlink directories to prevent circular references; only scan one level at a time, deeper levels recurse on user expand.
void App::expand_local_folder(TreeNodePtr node, bool force) {
    if (!node)
        return;
    std::string dir = node->url;

    // CACHE-1: serve a cached scan (skip the disk walk) unless force re-scan.
    if (!force) {
        std::string cached = DatabaseManager::instance().load_local_folder_cache(dir);
        if (!cached.empty()) {
            try {
                json j = json::parse(cached);
                if (j.is_array()) {
                    std::vector<TreeNodePtr> kids;
                    for (const auto &e : j) {
                        auto n = std::make_shared<TreeNode>();
                        n->title = e.value("title", std::string());
                        n->url = e.value("url", std::string());
                        int t = e.value("type", static_cast<int>(NodeType::FOLDER));
                        n->type = static_cast<NodeType>(t);
                        n->parent = node;
                        if (t == static_cast<int>(NodeType::FOLDER)) {
                            n->is_local_folder = true;
                            n->source_mode = "LOCAL_FOLDER";
                            n->children_loaded = false;
                        } else {
                            n->local_file = e.value("local_file", n->url);
                            n->is_downloaded = true;
                            n->children_loaded = true;
                            CacheManager::instance().mark_downloaded(n->url, n->local_file);
                        }
                        kids.push_back(n);
                    }
                    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                    node->children = kids;
                    node->children_loaded = true;
                    node->expanded = true;
                    EVENT_LOG(fmt::format("[LocalFolder] Loaded {} items from cache: {}", kids.size(),
                                          dir));
                    return;
                }
            } catch (const std::exception &e) {
                LOG(fmt::format("[LocalFolder] cache parse error: {}", e.what()));
            }
        }
    }

    std::vector<TreeNodePtr> subdirs; // subfolder nodes (can recurse after expansion)
    std::vector<TreeNodePtr> files;   // audio/video file nodes
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::error_code sec;
        // Skip symlink directories to prevent circular references (e.g. self-referential/mutually-linked dirs causing infinite recursion)
        if (it->is_symlink(sec))
            continue;
        if (it->is_directory(sec)) {
            auto sub = std::make_shared<TreeNode>();
            sub->title = it->path().filename().string();
            sub->url = it->path().string();
            sub->type = NodeType::FOLDER;
            sub->is_local_folder = true; // mark; recursive scan on expand
            sub->source_mode = "LOCAL_FOLDER";
            sub->children_loaded = false; // lazy load
            sub->parent = node;
            subdirs.push_back(sub);
            continue;
        }
        if (!it->is_regular_file(sec))
            continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (!is_media_extension(ext))
            continue;
        auto child = std::make_shared<TreeNode>();
        child->title = it->path().filename().string();
        child->url =
            it->path().string(); // absolute path; classify and MPV can both handle it directly
        child->local_file = it->path().string(); // Y24: local file path for sidecar probe + resume
        child->type = NodeType::PODCAST_EPISODE;
        child->is_downloaded = true; // local file, mark as cached (green)
        child->children_loaded = true;
        // F40: cache the local file path in media_cache (local-first playback + resume)
        CacheManager::instance().mark_downloaded(child->url, child->url);
        // Y24.8: sidecar probe is done ASYNC below (batch) — not here, to avoid N×5 synchronous
        //   fs::exists on slow /mnt/e mounts blocking the folder expand.
        child->parent = node;
        files.push_back(child);
    }
    // Sort each by name lexicographically: folders first, then files (common file-manager behavior)
    std::sort(subdirs.begin(), subdirs.end(),
              [](const TreeNodePtr &a, const TreeNodePtr &b) { return a->title < b->title; });
    std::sort(files.begin(), files.end(),
              [](const TreeNodePtr &a, const TreeNodePtr &b) { return a->title < b->title; });
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        node->children.clear();
        for (auto &c : subdirs)
            node->children.push_back(c);
        for (auto &c : files)
            node->children.push_back(c);
        node->children_loaded = true;
        node->expanded = true;
    }
    // CACHE-1: persist the scan so the next expand hits the DB instead of re-scanning disk.
    {
        json arr = json::array();
        for (const auto &c : subdirs) {
            json o = json::object();
            o["title"] = c->title;
            o["url"] = c->url;
            o["type"] = static_cast<int>(NodeType::FOLDER);
            o["is_local_folder"] = true;
            arr.push_back(std::move(o));
        }
        for (const auto &c : files) {
            json o = json::object();
            o["title"] = c->title;
            o["url"] = c->url;
            o["type"] = static_cast<int>(NodeType::PODCAST_EPISODE);
            o["is_downloaded"] = true;
            o["local_file"] = c->local_file;
            arr.push_back(std::move(o));
        }
        DatabaseManager::instance().save_local_folder_cache(dir, arr.dump());
    }
    // Y24.8: probe sidecars (📜 detection) async as one batch — off the UI thread so a large
    //   folder with many files doesn't block. 📜 markers appear within a frame or two.
    if (!files.empty()) {
        pool_.submit([files]() {
            for (auto &c : files)
                SubtitleManager::probe_sidecar(c);
        });
    }
    size_t total = subdirs.size() + files.size();
    if (total == 0) {
        EVENT_LOG(fmt::format("[LocalFolder] Directory empty (no audio/video files/subfolders): {}",
                              dir));
    } else {
        EVENT_LOG(fmt::format("[LocalFolder] Loaded {} items ({} subfolders, {} files): {}", total,
                              subdirs.size(), files.size(), dir));
    }
}

} // namespace panicast
