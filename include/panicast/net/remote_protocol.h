// Remote protocol types + control/query interface (PRP — panicast Protocol, MPD-style).
//   RemoteStateSnapshot: a compact, copyable snapshot of App + player state, built on the UI
//   thread (App::update_remote_state_cache) under remote_state_mtx_ and read by any server thread
//   via RemoteControlInterface::snapshot_state(). This is the thread-safe read path for query
//   commands (status / currentsong) — distinct from the command bus (the write path).
//
//   RemoteControlInterface: abstract interface App implements, so RemoteServer/RemoteSession stay
//   decoupled from App internals (composable — the server depends on an interface, not App).
#pragma once

#include <string>
#include <vector>

namespace panicast
{

struct RemotePlaylistItem {
    std::string title;
    int duration = 0;
    bool is_video = false;
};

// One row of the CURRENT mode's display list (the flat tree the TUI renders). Served to
//   Squeeze Client as the browseable "panicast library" — remote taps map 1:1 onto the
//   TUI's cursor+Enter (nav_activate), so remote browsing and the TUI stay on the same
//   list by construction. Not a copy of the tree: only what a remote row needs.
struct RemoteBrowseItem {
    std::string title;
    std::string subtext;    // second line (episode duration/status, feed description)
    std::string art_url;    // cover/thumbnail when known
    int depth = 0;          // flatten depth (0 = mode root)
    bool is_branch = false; // FOLDER / PODCAST_FEED → descend; leaf → play
};

struct RemoteStateSnapshot {
    // ── player (subset of MPVController::State, already thread-safe via its own mutex) ──
    bool paused = true;
    bool has_media = false;
    int volume = 0;
    double speed = 1.0;
    double elapsed = 0.0;  // time_pos
    double duration = 0.0; // media_duration
    std::string title;
    std::string url; // current_url
    bool has_video = false;
    int playlist_pos = -1; // mpv playlist_pos (unused by app pointer model; kept for parity)
    int playlist_count = 0;
    double net_speed_bps = 0.0;
    int buffering_pct = 0;
    std::string audio_codec;
    // ── app (built under remote_state_mtx_) ──
    std::string
        mode; // "RADIO"/"PODCAST"/"FAVOURITE"/"HISTORY"/"ONLINE"/"ACCOUNT"/"BILIBILI"/"TIKTOK"/"IPTV"
    std::string play_mode; // "repeat"/"shuffle"/"cycle"
    int selected_idx = 0;
    int current_index = -1;                   // -1 = nothing in the implicit playlist
    std::vector<RemotePlaylistItem> playlist; // current peers (the implicit play queue)
    // Current-mode display list (see RemoteBrowseItem) + a cheap change signature the
    //   LMS server polls to detect navigation (browse open/back waits for a list change).
    std::vector<RemoteBrowseItem> browse;
    std::string browse_sig; // FNV over mode + depth + titles; differs → list changed
    std::string art_url;    // cover art (TreeNode::art_url of the playing node)
    // ART-2: display metadata for the remote's now-playing/playlist rows. The app
    //   renders "unknown artist/album" for BLANK fields — fill them with the source's
    //   name (parent feed/station/channel) and a mode context instead.
    std::string artist;       // source name (feed / station / channel)
    std::string album;        // "panicast · <MODE>"
    int sleep_remaining = -1; // -1 = sleep timer inactive
    bool subtitle_active = false;
};

class RemoteControlInterface {
public:
    virtual ~RemoteControlInterface() = default;
    // Returns a thread-safe copy of the current state. Called from server worker threads.
    virtual RemoteStateSnapshot snapshot_state() = 0;
};

} // namespace panicast
