#include "panicast/storage/persistence.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#include <fmt/format.h>

#include "panicast/core/logger.h"
#include "panicast/core/paths.h"
#include "panicast/storage/database.h"
#include "panicast/app/online_state.h"

namespace panicast
{

namespace fs = std::filesystem;

// Save the RADIO tree to the database (F38: unified tree_nodes, root_type='radio'). E: radio is the items vector.
void Persistence::save_cache(const std::vector<TreeNodePtr> &radio,
                             const std::vector<TreeNodePtr> &podcast) {
    (void)podcast; // PODCAST tree is saved to tree_nodes via save_data()
    DatabaseManager::instance().save_tree("radio", radio);
}

// Load the RADIO tree from the database into the items vector.
void Persistence::load_cache(std::vector<TreeNodePtr> &radio, std::vector<TreeNodePtr> &podcast) {
    (void)podcast; // PODCAST tree is loaded from tree_nodes via load_data()
    // load_tree fills a node's children; load into a temp node then move the items out (E: no root node).
    auto tmp = std::make_shared<TreeNode>();
    tmp->title = "Radio";
    tmp->type = NodeType::FOLDER;
    tmp->expanded = true;
    DatabaseManager::instance().load_tree("radio", tmp);
    radio = std::move(tmp->children);
}

// Save subscriptions and favourites to the database (F38: unified tree_nodes + favourites link columns)
void Persistence::save_data(const std::vector<TreeNodePtr> &podcasts,
                            const std::vector<TreeNodePtr> &favs) {
    // Wrap the entire "clear + rewrite" in a transaction to avoid a crash/midway
    //   failure leaving old data deleted and new data half-written → user loses all subscriptions/favourites
    // P1.3 (Y23.5): hold mtx_ (recursive) across the entire BEGIN-COMMIT so background writes
    //   (save_episode_cache) can't interleave into the open transaction and get silently rolled back.
    auto &db = DatabaseManager::instance();
    db.run_locked([&]() {
        db.begin_txn();
        try {
            // F38: podcast tree → tree_nodes (recursive, root_type='podcast'). save_tree clears+recurses.
            //   Only top-level subscriptions with a url are meaningful as roots.
            std::vector<TreeNodePtr> pod_roots;
            for (const auto &p : podcasts)
                if (p && !p->url.empty())
                    pod_roots.push_back(p);
            // Y23.10: log this as a SYNC (clear+rewrite inside one txn), not a table deletion — the old
            //   "Cleared favourites table" wording misled users into thinking data was being erased.
            LOG(fmt::format("[DB] Syncing subscriptions ({} feeds) + favourites ({} entries) to DB",
                            pod_roots.size(), favs.size()));
            db.save_tree("podcast", pod_roots);

            // Favourites (link/local-folder metadata as columns, no data_json)
            db.clear_favourites(); // sync step: clear-then-rewrite within the open transaction
            for (const auto &f : favs) {
                if (f) {
                    db.save_favourite(f->title, f->url, (int)f->type, f->is_youtube,
                                      f->channel_name,
                                      f->source_mode, // source_type (LOCAL_FOLDER / RADIO / ...)
                                      f->is_link, f->link_target_url, f->is_local_folder);
                }
            }
            db.commit_txn();
        } catch (...) {
            db.rollback_txn();
            LOG("[Persistence] save_data failed, transaction rolled back");
            throw;
        }
    }); // close run_locked
}

// Load subscriptions and favourites from the database
void Persistence::load_data(std::vector<TreeNodePtr> &podcasts, std::vector<TreeNodePtr> &favs) {
    auto &db = DatabaseManager::instance();
    // F38: podcast tree from tree_nodes (recursive, root_type='podcast'). load_tree fills
    //   pod_root->children (top-level subscriptions) + their recursive children. The app sets
    //   the top-level parent pointer to podcast_root afterwards.
    auto pod_root = std::make_shared<TreeNode>();
    db.load_tree("podcast", pod_root);
    podcasts = pod_root->children;
    // Y23.2: is_bili_up isn't persisted in tree_nodes — re-mark Bilibili UP-master subscriptions
    //   (space.bilibili.com/<mid>/video) so they show the 👤 icon after restart.
    for (auto &c : podcasts) {
        if (!c->url.empty() && c->url.find("space.bilibili.com/") != std::string::npos)
            c->is_bili_up = true;
    }

    // Favourites (F38: link/local-folder metadata as columns, no data_json)
    auto db_favs = db.load_favourites();
    for (const auto &[title, url, type, is_youtube, channel_name, source_type, is_link,
                      link_target_url, is_local_folder, mt, art_url, artist, album] : db_favs) {
        auto node = std::make_shared<TreeNode>();
        node->title = title;
        node->url = url;
        node->type = (NodeType)type;
        // N06: display category from the DB. The renderer gates this on leaf node types, so
        //   folder/feed/link favourites still get a folder icon.
        node->media_type = static_cast<MediaType>(mt);
        node->media_type_set = true;
        node->is_youtube = is_youtube;
        node->channel_name = channel_name;
        node->source_mode = source_type;
        node->is_link = is_link;
        node->link_target_url = link_target_url;
        node->is_local_folder = is_local_folder;
        node->art_url = art_url; // META-3: thumbnails survive restarts
        node->artist = artist;
        node->album = album;
        node->children_loaded = true;
        if (is_local_folder) {
            node->source_mode = "LOCAL_FOLDER";
            node->children_loaded = false; // scan on expand
        } else if (is_link) {
            node->children_loaded = false; // LINK lazy-loads
            if (url == "online_root" || link_target_url == "online_root") {
                node->linked_node = get_online_root();
            }
        }
        favs.push_back(node);
    }
}

// Export OPML, using std::cout instead of EVENT_LOG (compatible with CLI mode)
void Persistence::export_opml(const std::string &filename,
                              const std::vector<TreeNodePtr> &podcasts) {
    std::ofstream f(filename);
    if (!f.is_open()) {
        std::cout << "Error: Cannot open file for writing: " << filename << std::endl;
        return;
    }
    // XML attribute escaping, to avoid titles containing &<>" producing invalid OPML
    auto xml_escape = [](const std::string &s) -> std::string {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out += c;
            }
        }
        return out;
    };
    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    f << "<opml version=\"1.0\">\n";
    f << "<head><title>panicast Export</title></head>\n";
    f << "<body>\n";
    int count = 0;
    for (const auto &p : podcasts) {
        if (p && !p->url.empty()) { // Null pointer check
            f << fmt::format("  <outline text=\"{}\" title=\"{}\" type=\"rss\" xmlUrl=\"{}\"/>\n",
                             xml_escape(p->title), xml_escape(p->title), xml_escape(p->url));
            count++;
        }
    }
    f << "</body>\n</opml>\n";
    f.close();
    std::cout << "Exported " << count << " podcasts to " << filename << std::endl;
}

// Bridge: returns the online_root of the OnlineState singleton.
TreeNodePtr Persistence::get_online_root() {
    return OnlineState::instance().online_root;
}

} // namespace panicast
