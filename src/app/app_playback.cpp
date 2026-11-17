#include "panicast/app/app.h"

namespace panicast
{

// Check whether a node is playable
bool App::is_playable_node(TreeNodePtr node) {
    if (!node)
        return false;
    // Only leaf playable types can be played — the original code returned true for any non-empty URL,
    //   handing PODCAST_FEED's feed URL to mpv as playable (which always fails)
    if (node->url.empty())
        return false;
    return node->type == NodeType::RADIO_STREAM || node->type == NodeType::PODCAST_EPISODE;
}

// ═════════════════════════════════════════════════════════════════════════
// Peer-list construction (the implicit playlist = siblings of the played episode)
// ═════════════════════════════════════════════════════════════════════════

// Build the peer playlist from the playable siblings (peers) of `node` under its parent, honoring
//   the parent's sort_reversed order. current_index is set to the position of `node` itself (so
//   playback starts there). If `node` has no parent / no siblings, the list contains just `node`.
// Returns the resulting current_index, or -1 on failure. (D8b-1: writes the queue via playback_.)
int App::build_peer_list(TreeNodePtr node) {
    if (!node)
        return -1;
    std::vector<PlaylistItem> peers;
    int target_idx = -1;

    auto parent = node->parent.lock();
    if (!parent || parent->children.empty()) {
        PlaylistItem it;
        it.title = node->title;
        it.url = node->url;
        it.duration = node->duration;
        it.artist = node->artist; // META-1
        it.album = node->album;
        it.art_url = node->art_url; // META-7i
        URLType ut = URLClassifier::classify(node->url);
        it.is_video = node->is_youtube || URLClassifier::is_video(ut);
        it.node = node;
        peers.push_back(it);
        target_idx = 0;
    } else {
        auto &siblings = parent->children;
        bool reversed = parent->sort_reversed;
        auto push_peer = [&](TreeNodePtr sib) {
            if (sib->url.empty())
                return; // skip folders/titles
            PlaylistItem it;
            it.title = sib->title;
            it.url = sib->url;
            it.duration = sib->duration;
            it.artist = sib->artist; // META-1
            it.album = sib->album;
            it.art_url = sib->art_url; // META-7i
            URLType ut = URLClassifier::classify(sib->url);
            it.is_video = sib->is_youtube || URLClassifier::is_video(ut);
            it.node = sib;
            if (sib.get() == node.get())
                target_idx = static_cast<int>(peers.size());
            peers.push_back(it);
        };
        if (!reversed) {
            for (auto &s : siblings)
                push_peer(s);
        } else {
            for (int i = static_cast<int>(siblings.size()) - 1; i >= 0; --i)
                push_peer(siblings[i]);
        }
        if (peers.empty()) {
            // No playable siblings (e.g. parent is a flat folder of subfolders). Scan the parent
            //   folder recursively for playable files to form the play list, so playback still has a
            //   queue. Final fallback: single item.
            auto parent = node->parent.lock();
            if (parent) {
                std::vector<TreeNodePtr> items;
                collect_playable_items(parent, items);
                for (auto &it : items) {
                    PlaylistItem pi;
                    pi.title = it->title;
                    pi.url = it->url;
                    pi.duration = it->duration;
                    pi.artist = it->artist; // META-1
                    pi.album = it->album;
                    pi.art_url = it->art_url; // META-7i
                    URLType ut = URLClassifier::classify(it->url);
                    pi.is_video = it->is_youtube || URLClassifier::is_video(ut);
                    pi.node = it;
                    if (it.get() == node.get())
                        target_idx = static_cast<int>(peers.size());
                    peers.push_back(pi);
                }
            }
            if (peers.empty()) {
                LOG(fmt::format(
                    "[PEERS] No playable files in parent folder, single-item fallback: {}",
                    node->title));
                // Final fallback: single item
                PlaylistItem it;
                it.title = node->title;
                it.url = node->url;
                it.duration = node->duration;
                URLType ut = URLClassifier::classify(node->url);
                it.is_video = node->is_youtube || URLClassifier::is_video(ut);
                peers.push_back(it);
                target_idx = 0;
            }
        }
    }

    std::lock_guard<std::mutex> pl_lock(playback_.playlist_mutex());
    playback_.playlist() = std::move(peers);
    playback_.set_current_index(target_idx);
    playback_.shuffle_queue().clear();
    if (play_mode == PlayMode::SHUFFLE)
        playback_.refill_shuffle_queue();
    LOG(fmt::format("[PEERS] Built {} peers, current at idx {}", playback_.playlist().size(),
                    playback_.current_index()));
    return playback_.current_index();
}

// Play an episode node: snapshot its peers into the playlist and play it. Used by Enter/l.
// (D8b-2: play_current lives in PlaybackService now; mode/play_mode are App settings passed in.)
void App::play_episode(TreeNodePtr node) {
    if (!is_playable_node(node))
        return;
    int idx = build_peer_list(node);
    if (idx >= 0)
        playback_.play_current(idx, mode, play_mode);
}

} // namespace panicast
