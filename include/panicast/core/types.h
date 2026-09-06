// Core data types: node tree, playlist items, URL type enums.
// Globally shared fundamental types, no business dependencies. Widely referenced by modules.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace panicast
{

// Node types
enum class NodeType { FOLDER, RADIO_STREAM, PODCAST_FEED, PODCAST_EPISODE };

// App modes
enum class AppMode { RADIO, PODCAST, FAVOURITE, HISTORY, ONLINE, ACCOUNT, BILIBILI, TIKTOK, IPTV };

// Y24: transcript load status for the current track — drives L-mode (LYRIC bar) activation.
//   NONE = no subtitle / video (subs render in video window); LOADING = async fetch in flight;
//   READY = segments loaded (or mpv sub-text active); FAILED = fetch returned empty/0 segments.
enum class TranscriptStatus { NONE, LOADING, READY, FAILED };

// App states
enum class AppState { BROWSING, LOADING, BUFFERING, PLAYING, PAUSED };

// Play modes (single global mode; persisted in INI [playback] mode)
// R = REPEAT  : single-track loop (replays the current item)
// S = SHUFFLE : random next item
// C = CYCLE   : sequential list loop (wraps to start after the last item)
enum class PlayMode {
    REPEAT,  // R: single-track loop
    SHUFFLE, // S: random next item
    CYCLE    // C: sequential list loop (default)
};

// Coarse media category for display (DB-stored on history/favourites so the icon no longer has
//   to be re-inferred from the URL every render). Platform-specific types win over generic ones
//   (YouTube is never "OnlineVideo", a radio stream is never "OnlineAudio") because URLClassifier
//   matches platform patterns before generic extensions. Local-vs-online and m3u8/IPTV are NOT
//   distinguishable from URLType alone, so classifyMediaType layers those two checks on top.
//   DOUYIN_* fold into Tiktok (Douyin is the CN counterpart; rarely listable, placeholder).
enum class MediaType {
    Radio,
    Youtube,
    Bilibili,
    Tiktok,
    Iptv,
    OnlineAudio,
    OnlineVideo,
    LocalAudio,
    LocalVideo
};

struct TreeNode;
using TreeNodePtr = std::shared_ptr<TreeNode>;
using TreeNodeWeakPtr = std::weak_ptr<TreeNode>;

// Tree node: unified carrier for subscription tree / favorites tree / playlist
struct TreeNode {
    std::string title;
    std::string url;
    NodeType type;
    bool expanded = false;
    bool children_loaded = false;
    bool loading = false;
    bool marked = false;
    bool parse_failed = false;
    std::string error_msg;
    bool is_youtube = false;
    std::string channel_name;
    std::string subtext;
    // META-1 (LMS-standard display metadata): structured artist/album/track so the
    //   remote (Squeeze Client / web) shows real values instead of placeholders.
    //   Filled by parsers (RSS itunes:*), persisted in tree_nodes (schema 51);
    //   snapshot/status fall back to parent-title heuristics when empty.
    std::string artist; // itunes:author / channel name
    std::string album;  // podcast feed title / channel or collection name
    int track_num = 0;  // itunes:episode (stored for future clients; not shown by
                        //   Squeeze Client's current Item model)
    int duration = 0;
    bool is_cached = false;
    bool is_downloaded = false;
    bool is_db_cached = false; // Database cache flag (from podcast_cache/episode_cache)
    std::string local_file;
    double play_position = 0.0;  // Last playback position (seconds)
    bool play_completed = false; // Whether playback has completed
    std::vector<std::shared_ptr<TreeNode>> children;

    // LINK: reference to a target node. When is_link=true, expand/display uses linked_node's children
    bool is_link = false;
    // Local folder node (added via A in F mode): url = absolute folder path;
    // when expanded, scans audio/video files in the directory as child nodes, all played via MPV.
    bool is_local_folder = false;

    // N04: an IPTV channel leaf (from m3u parsing). Rendered with the 📺 TV icon (IPTV = TV).
    bool is_iptv_channel = false;
    // Media category for DISPLAY (history / favourites DB rows). media_type_set=true means the
    //   renderer should use media_type_icon(media_type) instead of inferring from URL/flags.
    //   Live-tree nodes (radio/podcast feeds etc.) leave this false and use their flag-based icons.
    MediaType media_type = MediaType::Radio;
    bool media_type_set = false;
    TreeNodeWeakPtr
        linked_node; // weak_ptr to avoid circular references (runtime reference, may expire)
    std::string
        link_target_url;     // URL identifier of the link target (used for persistence and rebuild)
    std::string source_mode; // Source mode: "RADIO"/"PODCAST"/"ONLINE"/"FAVOURITE"/"HISTORY"

    bool sort_reversed = false; // true=descending (new→old), false=ascending (old→new)

    TreeNodeWeakPtr parent; // Parent node pointer (used for auto-playing the next track)

    // ── Y01: Y-mode (Google account) tree nodes ──────────────────────────────────
    // account nodes and their YouTube-content children live under account_root.
    int account_id = 0;         // >0 = bound to a Google account row in `accounts`
    bool is_account = false;    // true = this node IS an account (top-level under account_root)
    bool is_yt_history = false; // true = "History" child (YouTube watch history)
    bool is_yt_subscriptions = false; // true = "Subscriptions" child (YouTube subscriptions)
    bool is_yt_channel = false;       // true = a subscribed channel node (under "Subscriptions")
    // ── Y02: YouTube search + per-channel account binding ────────────────────────
    bool is_yt_search = false; // true = a "🔍 <query>" results folder under an account
    bool is_yt_search_result =
        false; // true = a YouTube search-result node (channel/video/playlist)
    bool is_yt_playlist =
        false; // Y23.1: a YouTube playlist search-result (distinct icon from channel)
    bool is_yt_music = false; // Y23.1: a YouTube music-category video search-result (🎵 icon)
    bool is_search_parent =
        false; // Y23.1: a "Search History" container node (r/d no-op, l/enter expands)
    bool is_bili_up =
        false; // Y23.2: a Bilibili UP-master node (followings/subscribed/search) → 👤 icon
    // ── Y22: Bilibili B-mode account children (mirror Y-mode History / Subscriptions) ─────────
    bool is_bili_followings =
        false; // true = "Subscriptions" child (Bilibili followings) under a B account
    bool is_bili_history =
        false; // true = "History" child (Bilibili watch history) under a B account
    // Y23: art/thumbnail URL for search results & cached nodes — UP avatar (upic) or video cover
    //   (pic) or YouTube thumbnail. Stored (not rendered in TUI; terminals can't inline images) for
    //   future use (sixel/kitty terminal or the Android remote's cover-art display).
    std::string art_url;
    std::string channel_id; // YouTube channel id (subscribe / expand by id)
    // cached account display info (populated when account_root is built; rendered by draw_info)
    std::string account_email;
    int64_t account_token_expires = 0; // unix epoch
    int account_sub_count = 0;
    int64_t account_last_sync = 0;

    // ── Y16: subtitle/transcript detection ──────────────────────────────────────
    // has_subtitle=true → render 📜 emoji after type emoji, before title (📻 📜 Title).
    // subtitle_url = transcript URL from RSS <podcast:transcript> (downloaded to tmp at play time).
    bool has_subtitle = false; // online transcript from RSS (📜)
    std::string subtitle_url;  // RSS <podcast:transcript> URL (empty = no external transcript)
    std::string subtitle_type; // Y24.7: "json"/"srt"/"vtt"/"lrc" (parse hint + sidecar ext)
    // Y24.23: local ASR SRT (📝) — independent from online transcript. Set after whisper.cpp ASR.
    // probe_sidecar: if has_subtitle is false and a local .srt is found → set has_asr_srt (ASR origin).
    // has_subtitle + has_asr_srt both true → 📝 (ASR takes emoji precedence over 📜 for display).
    bool has_asr_srt = false; // local ASR SRT exists (📝)
    std::string asr_srt_path; // path to the local ASR SRT file
};

// Playlist item
struct PlaylistItem {
    std::string title;
    std::string url;
    int duration = 0;
    bool is_video = false;
    std::string artist;    // META-1: carried to the remote playlist rows
    std::string album;     // META-1
    std::string node_path; // Node path (SoftLink reference)
    TreeNodePtr node;      // F35: source tree node (so playback_node can track it for INFO title)
};

// URL types
enum class URLType {
    UNKNOWN,
    OPML,
    RSS_PODCAST,
    YOUTUBE_RSS,
    YOUTUBE_CHANNEL,
    YOUTUBE_VIDEO,
    YOUTUBE_PLAYLIST,
    APPLE_PODCAST,
    RADIO_STREAM,
    VIDEO_FILE,
    BILIBILI_CHANNEL,
    BILIBILI_VIDEO,
    DOUYIN_USER,
    DOUYIN_VIDEO, // Y16: B/T mode URLs
    TIKTOK_USER,
    TIKTOK_VIDEO // Y24.11: T mode (tiktok.com/@user, /video/<id>, vm.tiktok.com shortlink)
};

} // namespace panicast
