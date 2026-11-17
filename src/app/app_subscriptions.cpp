#include "panicast/app/app.h"
#include <iostream>

namespace panicast
{

// Use std::cout instead of EVENT_LOG for command-line mode compatibility
void App::import_feed(const std::string &url) {
    std::cout << "Adding feed: " << url << std::endl;

    auto node = std::make_shared<TreeNode>();
    node->url = url;
    node->type = NodeType::PODCAST_FEED;
    // YouTube channels get their @handle as the title; everything else shows "Loading..."
    //   until the parser fills in the real feed title.
    {
        URLType ut = URLClassifier::classify(url);
        node->title = (ut == URLType::YOUTUBE_CHANNEL) ? URLClassifier::extract_channel_name(url)
                                                       : "Loading...";
    }

    {
        std::lock_guard<std::recursive_mutex> lock(
            library_.tree_mutex()); // mutual exclusion with background load thread
        library_.podcast_root().insert(library_.podcast_root().begin(), node);
    }
    spawn_load_feed(node);

    // Wait for background loading to finish before serializing to avoid torn reads/crashes with the load thread
    pool_.wait_idle();
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        Persistence::save_cache(library_.radio_root(), library_.podcast_root());
        Persistence::save_data(library_.podcast_root(), library_.fav_root());
    }
    std::cout << "Feed added successfully: " << node->title << std::endl;
}

// Import from an OPML file; use std::cout instead of EVENT_LOG
void App::import_opml(const std::string &filepath) {
    std::cout << "Importing from: " << filepath << std::endl;

    auto feeds = OPMLParser::import_opml_file(filepath);
    if (feeds.empty()) {
        std::cout << "No feeds found in OPML file" << std::endl;
        return;
    }

    std::cout << "Found " << feeds.size() << " feeds in OPML" << std::endl;

    int count = 0;
    for (auto &feed : feeds) {
        // Check whether it already exists
        bool exists = false;
        {
            std::lock_guard<std::recursive_mutex> lock(
                library_.tree_mutex()); // mutual exclusion with background load thread
            for (const auto &existing : library_.podcast_root()) {
                if (existing->url == feed->url) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                library_.podcast_root().insert(library_.podcast_root().begin(), feed);
            }
        }
        if (exists) {
            std::cout << "  Skipping duplicate: " << feed->title << std::endl;
            continue;
        }
        spawn_load_feed(feed);
        count++;
        std::cout << "  Added: " << (feed->title.empty() ? feed->url : feed->title) << std::endl;
    }

    std::cout << "Imported " << count << " new feeds" << std::endl;
    // Wait for all background loading to finish before serializing to avoid torn reads/crashes
    pool_.wait_idle();
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        Persistence::save_cache(library_.radio_root(), library_.podcast_root());
        Persistence::save_data(library_.podcast_root(), library_.fav_root());
    }
}

// Export podcast subscriptions
void App::export_podcasts(const std::string &filename) {
    pool_
        .wait_idle(); // wait for background loading to finish, avoiding concurrent mutation of children during export
    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
    Persistence::export_opml(filename, library_.podcast_root());
}

void App::add_feed() {
    std::string url = frontend_->input_box("Enter URL (RSS/YouTube @Channel):");
    if (url.empty())
        return;

    URLType url_type = URLClassifier::classify(url);
    EVENT_LOG(fmt::format("Adding: {} [{}]", url, URLClassifier::type_name(url_type)));

    auto node = std::make_shared<TreeNode>();
    if (url_type == URLType::YOUTUBE_CHANNEL) {
        node->title = URLClassifier::extract_channel_name(url);
    } else {
        node->title = "Loading...";
    }
    node->url = url;
    node->type = NodeType::PODCAST_FEED;
    // Do not set loading=true; let the node show its normal type icon
    // spawn_load_feed will set the loading state when parsing starts
    node->parse_failed = false;

    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        node->parent.reset(); // set parent pointer
        library_.podcast_root().insert(library_.podcast_root().begin(), node);
    }

    // Save the subscription to the database immediately (before parsing, to ensure it loads after restart)
    Persistence::save_data(library_.podcast_root(), library_.fav_root());
    EVENT_LOG(fmt::format("Subscription saved: {}", url));

    spawn_load_feed(node);
}

// Subscribe to a podcast from Online search results
void App::subscribe_online_podcast() {
    if (library_.selected_idx() < 0 ||
        library_.selected_idx() >= (int)library_.display_list().size())
        return;
    auto node = library_.display_list()[library_.selected_idx()].node;
    if (!node || node->url.empty())
        return;

    // Check existence + add + persist (reading/writing library_.podcast_root() requires holding tree_mutex)
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        for (const auto &child : library_.podcast_root()) {
            if (child->url == node->url) {
                EVENT_LOG("Already subscribed");
                return;
            }
        }

        // Add to the PODCAST list
        auto new_node = std::make_shared<TreeNode>();
        new_node->title = node->title;
        new_node->url = node->url;
        new_node->type = NodeType::PODCAST_FEED;
        new_node->subtext = node->subtext;

        library_.podcast_root().insert(library_.podcast_root().begin(), new_node);
        Persistence::save_cache(library_.radio_root(), library_.podcast_root());
        Persistence::save_data(library_.podcast_root(), library_.fav_root());
    }

    EVENT_LOG(fmt::format("Subscribed: {}", node->title));
}

// ONLINE mode multi-select batch subscribe
void App::subscribe_online_podcasts_batch(int marked_count) {
    (void)marked_count; // parameter reserved for future use
    std::vector<TreeNodePtr> to_subscribe;
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        collect_marked(OnlineState::instance().online_root, to_subscribe);
    }

    if (to_subscribe.empty()) {
        EVENT_LOG("No items to subscribe");
        return;
    }

    // Filter out subscribable nodes (PODCAST_FEED type)
    std::vector<TreeNodePtr> feed_nodes;
    {
        std::lock_guard<std::recursive_mutex> lock(
            library_.tree_mutex()); // reading children must be mutually exclusive
        for (auto &n : to_subscribe) {
            if (n->type == NodeType::PODCAST_FEED && !n->url.empty()) {
                // Check whether already subscribed
                bool already_subscribed = false;
                for (const auto &child : library_.podcast_root()) {
                    if (child->url == n->url) {
                        already_subscribed = true;
                        break;
                    }
                }
                if (!already_subscribed) {
                    feed_nodes.push_back(n);
                }
            }
        }
    }

    if (feed_nodes.empty()) {
        EVENT_LOG("All selected items already subscribed");
        return;
    }

    // Batch subscribe
    int count = 0;
    for (auto &n : feed_nodes) {
        auto new_node = std::make_shared<TreeNode>();
        new_node->title = n->title;
        new_node->url = n->url;
        new_node->type = NodeType::PODCAST_FEED;
        new_node->subtext = n->subtext;
        new_node->is_youtube =
            n->is_youtube; // missing copy would cause YouTube podcasts to be played as audio
        new_node->channel_name = n->channel_name; //   and use the wrong loader

        std::lock_guard<std::recursive_mutex> lock(
            library_.tree_mutex()); // mutual exclusion with background load thread
        library_.podcast_root().insert(library_.podcast_root().begin(), new_node);
        count++;
    }

    // Clear marks
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        clear_marks(OnlineState::instance().online_root);
    }

    Persistence::save_cache(library_.radio_root(), library_.podcast_root());
    Persistence::save_data(library_.podcast_root(), library_.fav_root());

    EVENT_LOG(fmt::format("Subscribed {} podcasts", count));
}
// Y24.29: subscribe_favourite_single removed (dead code, 0 callers).

// FAVOURITE mode multi-select batch subscribe
void App::subscribe_favourites_batch(int marked_count) {
    (void)marked_count; // parameter reserved for future use
    std::vector<TreeNodePtr> to_subscribe;
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        for (auto &it : library_.fav_root())
            collect_marked(it, to_subscribe);
    }

    if (to_subscribe.empty()) {
        EVENT_LOG("No items to subscribe");
        return;
    }

    // Filter out subscribable nodes (reading library_.podcast_root() requires holding tree_mutex)
    std::vector<TreeNodePtr> feed_nodes;
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        for (auto &n : to_subscribe) {
            // Support PODCAST_FEED and nodes with a URL
            if (!n->url.empty() && n->url != "online_root") {
                // Check whether already subscribed
                bool already_subscribed = false;
                for (const auto &child : library_.podcast_root()) {
                    if (child->url == n->url) {
                        already_subscribed = true;
                        break;
                    }
                }
                if (!already_subscribed) {
                    feed_nodes.push_back(n);
                }
            }
        }
    }

    if (feed_nodes.empty()) {
        EVENT_LOG("All selected items already subscribed");
        return;
    }

    // Batch subscribe (reading/writing library_.podcast_root() requires holding tree_mutex)
    int count = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        for (auto &n : feed_nodes) {
            auto new_node = std::make_shared<TreeNode>();
            new_node->title = n->title;
            new_node->url = n->url;
            new_node->type = NodeType::PODCAST_FEED;
            new_node->is_youtube = n->is_youtube;

            library_.podcast_root().insert(library_.podcast_root().begin(), new_node);
            count++;
        }

        // Clear marks
        for (auto &it : library_.fav_root())
            clear_marks(it);

        Persistence::save_cache(library_.radio_root(), library_.podcast_root());
        Persistence::save_data(library_.podcast_root(), library_.fav_root());
    }

    EVENT_LOG(fmt::format("Subscribed {} podcasts from favourites", count));
}

// Multi-select batch favourite
// ONLINE mode batch favourite is also converted to a LINK
void App::add_favourites_batch(int marked_count) {
    (void)
        marked_count; // F38: was used only for the removed ref_json (batch metadata no longer persisted)
    // ONLINE mode special handling
    if (mode == AppMode::ONLINE) {
        // Check whether an online_root favourite already exists
        for (auto &f : library_.fav_root()) {
            if (f->url == "online_root") {
                EVENT_LOG("★ Online Search already in favourites");
                // Clear marks
                std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                clear_marks_current();
                return;
            }
        }

        // ONLINE mode batch favourite also creates only a single LINK
        auto fn = std::make_shared<TreeNode>();
        fn->title = "Online Search";
        fn->url = "online_root";
        fn->type = NodeType::FOLDER;
        fn->children_loaded = false;

        std::string source_type = "online_root_reference";
        fn->is_link = true; // F38: link flag persisted as a column (was inferred at load)
        fn->link_target_url = "online_root";

        {
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            library_.fav_root().insert(library_.fav_root().begin(), fn);
            clear_marks_current();
        }

        DatabaseManager::instance().save_favourite(
            fn->title, fn->url, (int)fn->type, fn->is_youtube, fn->channel_name, source_type,
            fn->is_link, fn->link_target_url, fn->is_local_folder, fn->art_url, fn->artist,
            fn->album); // META-3

        EVENT_LOG("★ Favourite: Online Search (LINK to online_root)");
        return;
    }

    // Non-ONLINE mode keeps the original batch-favourite logic
    std::vector<TreeNodePtr> to_fav;
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        collect_marked_current(to_fav);
    }

    if (to_fav.empty()) {
        EVENT_LOG("No items to favourite");
        return;
    }

    // Filter out favouritable nodes
    std::vector<TreeNodePtr> fav_nodes;
    std::set<std::string> existing_urls;
    for (const auto &f : library_.fav_root()) {
        if (!f->url.empty())
            existing_urls.insert(f->url);
    }

    for (auto &n : to_fav) {
        // Support favourites for all node types
        if (n->type == NodeType::PODCAST_FEED || n->type == NodeType::FOLDER ||
            n->type == NodeType::RADIO_STREAM || n->type == NodeType::PODCAST_EPISODE) {
            // Check whether already favourited (URL match or title match)
            bool already_fav = false;
            if (!n->url.empty()) {
                already_fav = existing_urls.count(n->url) > 0;
            } else {
                for (const auto &f : library_.fav_root()) {
                    if (f->title == n->title && f->type == n->type) {
                        already_fav = true;
                        break;
                    }
                }
            }
            if (!already_fav) {
                fav_nodes.push_back(n);
            }
        }
    }

    if (fav_nodes.empty()) {
        EVENT_LOG("All selected items already favourited");
        return;
    }

    // Batch favourite also uses the LINK mechanism
    // Get the source mode name
    std::string source_mode_name;
    switch (mode) {
    case AppMode::RADIO:
        source_mode_name = "RADIO";
        break;
    case AppMode::PODCAST:
        source_mode_name = "PODCAST";
        break;
    case AppMode::ONLINE:
        source_mode_name = "ONLINE";
        break;
    case AppMode::FAVOURITE:
        source_mode_name = "FAVOURITE";
        break;
    case AppMode::HISTORY:
        source_mode_name = "HISTORY";
        break;
    case AppMode::ACCOUNT:
        source_mode_name = "YOUTUBE";
        break;
    case AppMode::BILIBILI:
        source_mode_name = "BILIBILI";
        break;
    case AppMode::TIKTOK:
        source_mode_name = "TIKTOK";
        break;
    case AppMode::IPTV:
        source_mode_name = "IPTV";
        break;
    }

    // Batch favourite (writing library_.fav_root() requires holding tree_mutex)
    int count = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        for (auto &n : fav_nodes) {
            // Create LINK node
            auto fn = std::make_shared<TreeNode>();
            fn->title = n->title;
            fn->url = n->url;                   // target URL
            fn->type = n->type;                 // target type
            fn->is_link = true;                 // mark as LINK
            fn->link_target_url = n->url;       // persist target URL
            fn->source_mode = source_mode_name; // source mode
            fn->linked_node = n;                // runtime reference
            fn->is_youtube = n->is_youtube;
            fn->channel_name = n->channel_name;
            fn->children_loaded = false; // LINK node lazy-load

            fn->parent.reset();
            library_.fav_root().insert(library_.fav_root().begin(), fn);

            // Save to database (including LINK info)
            fn->is_link = true; // F38: link flag persisted as a column (was in data_json)
            DatabaseManager::instance().save_favourite(
                fn->title, fn->url, (int)fn->type, fn->is_youtube, fn->channel_name,
                source_mode_name, fn->is_link, fn->link_target_url, fn->is_local_folder);

            count++;
        }
    }

    // Clear marks
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        clear_marks_current();
    }

    EVENT_LOG(fmt::format("★ Added {} favourites", count));
}

// Extended favourite feature, supports all node types (including folders)
// ONLINE mode distinguishes two favourite types, with bidirectional sync
void App::add_favourite() {
    if (library_.selected_idx() < 0 ||
        library_.selected_idx() >= (int)library_.display_list().size())
        return;
    auto node = library_.display_list()[library_.selected_idx()].node;
    if (!node)
        return; // null guard

    // ═══════════════════════════════════════════════════════════════════
    // Unified LINK mechanism - all favourites are LINKs
    // Favourite node storage: target URL + target type + source mode + runtime reference
    // On expand: runtime reference -> memory lookup -> database cache -> network parsing
    // ═══════════════════════════════════════════════════════════════════

    // Check whether a favourite with the same URL already exists (reading library_.fav_root() requires holding tree_mutex)
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        for (auto &f : library_.fav_root()) {
            if (!node->url.empty() && f->url == node->url) {
                EVENT_LOG(fmt::format("★ Already in favourites: {}", node->title));
                return;
            }
            if (node->url.empty() && f->title == node->title && f->type == node->type) {
                EVENT_LOG(fmt::format("★ Already in favourites: {}", node->title));
                return;
            }
        }
    }

    // Get the source mode name
    std::string source_mode_name;
    switch (mode) {
    case AppMode::RADIO:
        source_mode_name = "RADIO";
        break;
    case AppMode::PODCAST:
        source_mode_name = "PODCAST";
        break;
    case AppMode::ONLINE:
        source_mode_name = "ONLINE";
        break;
    case AppMode::FAVOURITE:
        source_mode_name = "FAVOURITE";
        break;
    case AppMode::HISTORY:
        source_mode_name = "HISTORY";
        break;
    case AppMode::ACCOUNT:
        source_mode_name = "YOUTUBE";
        break;
    case AppMode::BILIBILI:
        source_mode_name = "BILIBILI";
        break;
    case AppMode::TIKTOK:
        source_mode_name = "TIKTOK";
        break;
    case AppMode::IPTV:
        source_mode_name = "IPTV";
        break;
    }

    // Create LINK node
    auto fn = std::make_shared<TreeNode>();
    fn->title = node->title;
    fn->url = node->url;                // target URL (for locating and rebuilding)
    fn->type = node->type;              // target type
    fn->is_link = true;                 // mark as LINK
    fn->link_target_url = node->url;    // persist target URL
    fn->source_mode = source_mode_name; // source mode
    fn->linked_node = node;             // runtime reference (may expire)
    fn->is_youtube = node->is_youtube;
    fn->channel_name = node->channel_name;
    fn->children_loaded = false; // LINK node initially unloaded

    // Special case: Online Search root node
    if (node->url == "online_root" || node.get() == OnlineState::instance().online_root.get()) {
        fn->url = "online_root";
        fn->link_target_url = "online_root";
        fn->linked_node = OnlineState::instance().online_root;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        fn->parent.reset();
        library_.fav_root().insert(library_.fav_root().begin(), fn);
    }

    // Save to database (including LINK info)
    fn->is_link = true; // F38: link flag persisted as a column (was in data_json)
    DatabaseManager::instance().save_favourite(fn->title, fn->url, (int)fn->type, fn->is_youtube,
                                               fn->channel_name, source_mode_name, fn->is_link,
                                               fn->link_target_url, fn->is_local_folder,
                                               fn->art_url, fn->artist, fn->album); // META-3

    EVENT_LOG(fmt::format("★ Favourite LINK: {} [{}]", node->title, source_mode_name));
}

// F mode: 'a' key — scan a local folder for audio/video files and add as a playable subtree.
//   F42: uses expand_local_folder for recursive subfolder structure (consistent with restart).
void App::add_local_files() {
    std::string path = frontend_->input_box("Folder path:");
    if (UI::is_input_cancelled(path)) {
        EVENT_LOG("Add local files cancelled");
        return;
    }
    if (path.empty())
        return;
    // Expand ~ to $HOME
    if (path[0] == '~') {
        const char *home = std::getenv("HOME");
        if (home)
            path = std::string(home) + path.substr(1);
    }
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_directory(path, ec)) {
        EVENT_LOG(fmt::format("[LocalFiles] Not a valid directory: {}", path));
        return;
    }
    // Tag name: default to the folder name, user may override.
    std::string default_tag = fs::path(path).filename().string();
    if (default_tag.empty())
        default_tag = path;

    // F42: build folder node, then populate recursively via expand_local_folder — gives
    //   subfolder hierarchy (consistent with post-restart expand). Replaces the former flat
    //   recursive_directory_iterator that listed all files without subfolder structure.
    auto folder = std::make_shared<TreeNode>();
    folder->title = default_tag;
    folder->url = path;
    folder->type = NodeType::FOLDER;
    folder->is_local_folder = true;
    folder->source_mode = "LOCAL_FOLDER";
    folder->expanded = true;
    expand_local_folder(folder); // recursive scan: subfolders + file leaves + mark_downloaded

    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        folder->parent.reset();
        library_.fav_root().insert(library_.fav_root().begin(), folder);
    }

    // Persist (record as a local folder favourite so it survives restart).
    DatabaseManager::instance().save_favourite(folder->title, folder->url, (int)folder->type, false,
                                               "", "LOCAL_FOLDER", folder->is_link,
                                               folder->link_target_url, folder->is_local_folder);
    Persistence::save_data(library_.podcast_root(), library_.fav_root());

    EVENT_LOG(fmt::format("★ Added local folder [{}]: {}", default_tag, path));
    LOG(fmt::format("[LocalFolder] added: {}", path));
}

} // namespace panicast
