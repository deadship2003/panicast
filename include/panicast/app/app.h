// Application controller (business layer): the App class integrates UI / MPVController / Parsers / Storage / Network,
//   driving the TUI main loop, command-line mode (import/export/subscribe), play queue, and downloads.
//   Method bodies are split across 9 out-of-line .cpp files in src/app/; this header holds the class
//   declaration (members + method declarations + short one-liner inline helpers + static helpers).
#pragma once

#include "panicast/app/keymap.h"
#include "panicast/app/playback_service.h"

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <map>
#include <set>
#include <queue>
#include <deque>
#include <chrono>
#include <regex>
#include <random>
#include <array>
#include <optional>
#include <ctime>
#include <cmath>

#include <cerrno>
#include <csignal>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mpv/client.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <fmt/format.h>
#include <fmt/chrono.h>

#include "panicast/core/types.h"
#include "panicast/core/paths.h"
#include "panicast/core/constants.h"
#include "panicast/core/platform.h"
#include "panicast/core/logger.h"
#include "panicast/core/event_log.h"
#include "panicast/core/thread_pool.h"
#include "panicast/core/safe_tmp.h"
#include "panicast/core/terminal.h"
#include "panicast/core/utils.h"
#include "panicast/ui/win_raii.h"
#include "panicast/net/url_classifier.h"
#include "panicast/config/ini_config.h"
#include "panicast/net/url_guard.h"
#include "panicast/net/ytdlp_runner.h"
#include "panicast/net/network.h"
#include "panicast/net/google_oauth.h"
#include "panicast/net/remote_command_bus.h"
#include "panicast/net/remote_server.h"
#include "panicast/net/remote_protocol.h"
#include "panicast/net/bilibili_api.h"
#include "panicast/storage/accounts.h"
#include "panicast/ui/qr.h"
#include "panicast/ui/icons.h"
#include "panicast/ui/art.h"
#include "panicast/ui/layout_metrics.h"
#include "panicast/ui/layout_guard.h"
#include "panicast/storage/database.h"
#include "panicast/storage/cache.h"
#include "panicast/storage/youtube_cache.h"
#include "panicast/storage/persistence.h"
#include "panicast/app/subtitle_service.h" // D10-1: subtitles/ASR Application Service (owns SubtitleManager + TranscriptionEngine)
#include "panicast/app/search_service.h" // D10-2: in-tree search Application Service (owns search state)
#include "panicast/app/library_service.h" // D10-4: library Application Service (owns per-mode tree data model)
#include "panicast/app/download_service.h" // D43: download Application Service (owns download execution engine)
#include "panicast/ui/border.h"
#include "panicast/ui/ui.h"
#include "panicast/theme/colors.h"
#include "panicast/theme/pairs.h"
#include "panicast/parsers/feed_parser.h"
#include "panicast/parsers/xml_helpers.h"
#include "panicast/parsers/itunes_search.h"
#include "panicast/parsers/opml_parser.h"
#include "panicast/parsers/rss_parser.h"
#include "panicast/parsers/youtube_channel_parser.h"
#include "panicast/playback/mpv_controller.h"
#include "panicast/playback/sleep_timer.h"
#include "panicast/app/progress.h"
#include "panicast/app/online_state.h"

namespace panicast
{

namespace fs = std::filesystem;
using json = nlohmann::json;

class App : public RemoteControlInterface {
public:
    App();
    ~App();
    void run();

    // N09/S1: swap the ncurses frontend for the no-op NullFrontend and mark the App
    //   headless — `panicast -d` runs the full engine without a terminal;
    //   the run loop then paces on sleeps instead of draw/input. Must be called before run().
    void set_headless();

    // N09/S1: run `hook` right before the shutdown bookend's _exit(0) (which skips the
    //   rest of main) — the daemon uses it to remove its pid file.
    void set_exit_hook(std::function<void()> hook) {
        exit_hook_ = std::move(hook);
    }

    // Use std::cout instead of EVENT_LOG for command-line mode compatibility
    void import_feed(const std::string &url);
    // Import from an OPML file; use std::cout instead of EVENT_LOG
    void import_opml(const std::string &filepath);
    // Export podcast subscriptions
    void export_podcasts(const std::string &filename);
    // Public method for loading persistent data in command-line mode
    void load_data();

    // N02: RemoteControlInterface — thread-safe state snapshot for remote query commands.
    RemoteStateSnapshot snapshot_state() override;

private:
    std::unique_ptr<IFrontend> frontend_ = std::make_unique<UI>();
    bool headless_ = false; // N09/S1: daemon mode — no draw/input, engine-only frame loop
    std::function<void()>
        exit_hook_; // N09/S1: called just before _exit(0) in shutdown() // D12-3c: App owns the ncurses UI through the IFrontend contract (UI swappable — Qt could implement the same interface). Concrete UI is named only at this construction point + the static UI::is_input_cancelled input-marker check.
    MPVController player;
    PlaybackService playback_{player}; // D8: first Application Service (owns playback Actions)
    // D10-4: the per-mode tree DATA MODEL (8 root item lists + 6 "loaded" flags) moved into
    //   LibraryService. D11-2: the VIEW STATE (display_list/selected_idx/view_start) + pending_select_
    //   + its guard tree_mutex ALSO moved into LibraryService (co-located with the data it protects).
    //   app_*.cpp read/write via library_. accessors (radio_root()/…, *_loaded(), display_list(),
    //   selected_idx(), view_start(), tree_mutex(), pending_select()). The tree-building methods
    //   (load_*_root) still stay here (parser/storage/UI-coupled → relocate in D11-3c).
    LibraryService library_;
    std::string
        tiktok_region_; // Y24.11: current T-mode region code (US/JP/...), persisted in INI [tiktok] region
    // Y11: async interaction selection — pending_select_ now lives in library_ (D11-2). A pool task
    //   that builds a new node (e.g. YouTube search results) sets library_.pending_select() under
    //   library_.tree_mutex(); the UI thread consumes it next frame (sets selected_idx to that node)
    //   so interactions stay fluid (network/parse off the UI thread).
    // D4: END_FILE reason queued by the mpv event thread; drained on the UI thread each frame
    //   (see run()). Keeps on_playback_ended OFF the mpv thread so it can't contend with the UI's
    //   playlist_mutex_ draw lock — the root cause of "TUI freezes a while after pausing".
    std::atomic<int> pending_end_reason_{-1};
    // D6: EventBus action-subscription tokens (UI→Core). Kept for the App's lifetime (the app
    //   exits via _exit, so no explicit unsubscribe needed).
    std::vector<size_t> action_subs_;
    // D7: key → Action map. Bound keys go through the message bus (UI→Action→handler), not
    //   direct Core calls. Built by build_keymap() from a defaults table, overridable via INI
    //   [keys] (D44 — rebindable hotkeys, D7's full goal); absent [keys] = legacy defaults.
    Keymap keymap_;
    void build_keymap();
    // D11-2: display_list / selected_idx / view_start moved into library_ (view state + its guard
    //   tree_mutex co-located with the tree data). cur_sort_reversed stays here (sort toggle, not
    //   lock-guarded view state).
    bool cur_sort_reversed =
        false; // E: reverse state for top-level (cur_items) sort — was on the root node
    bool running = true;
    AppMode mode = AppMode::RADIO;
    // D11-2: tree_mutex moved into library_ (guards tree data + view state together).
    // Thread pool replaces detached threads; App destructor safely joins.
    // Sized to MAX_CONCURRENT_DOWNLOADS so up to 10 downloads can truly run at once.
    ThreadPool pool_{MAX_CONCURRENT_DOWNLOADS};
    // N01: remote control. remote_bus_ is the network→UI command queue (the single crossing
    //   point between the server thread and the UI thread); remote_server_ owns the TCP accept
    //   thread. Declared in this order so remote_server_ can bind a reference to remote_bus_.
    //   Only started when [remote] enable=true; otherwise the local TUI is unaffected.
    RemoteCommandBus remote_bus_;
    RemoteServer remote_server_{remote_bus_};
    // N02: state snapshot cache. Built once per frame on the UI thread
    //   (update_remote_state_cache) under remote_state_mtx_; read by server threads via
    //   snapshot_state(). Avoids cross-locking tree_mutex/playlist_mutex from server threads.
    std::mutex remote_state_mtx_;
    RemoteStateSnapshot remote_state_cache_;
    void update_remote_state_cache();
    // (D43) pending_downloads_ moved to DownloadService (download_ below). prepare_frame reads it
    //   via download_.pending_downloads().
    // D10-2: in-tree search state (search_query/matches/idx/total) moved into SearchService.
    //   app_search.cpp reads/writes via search_. accessors; the search METHODS stay here for now
    //   (they reach library_.tree_mutex()/display_list()/selected_idx() via accessors — D11-2 made
    //   those accessors; the method BODIES relocate in D11-3b).
    SearchService search_;
    bool visual_mode_ = false;
    int visual_start_ = -1;

    // D8b-1: the implicit-playlist QUEUE STATE (current_playlist / current_index /
    //   shuffle_queue_ / playlist_mutex_) moved into PlaybackService. D9-2: the "what's playing"
    //   track handles (playback_node / playback_mode_) moved in too. D9-3: the BUFFERING handle
    //   playback_pending_(_start_) + its state machine moved in (advance_buffering). App now holds
    //   only the global play_mode (a setting written from several input sites) — NO playback-state
    //   member remains here.
    PlayMode play_mode = PlayMode::CYCLE; // global play mode (persisted in INI [playback] mode)

    // ═════════════════════════════════════════════════════════════════════════
    // Pointer-driven playback (on END_FILE advance current_index)
    // ═════════════════════════════════════════════════════════════════════════

    // ── app_playback.cpp ───────────────────────────────────────────────────────
    bool is_playable_node(TreeNodePtr node);
    // D8b-2: on_playback_ended / play_current / record_play_history / resolve_youtube_url moved to
    //   PlaybackService (it now owns playback + autoplay logic); it reaches App's runtime handles
    //   via the attach() callback seam. build_peer_list stays here (tree logic); play_episode calls
    //   playback_.play_current(idx, mode, play_mode).
    int build_peer_list(TreeNodePtr node);
    // Y24.27: build episode child nodes from DB cache — extracted from 7 duplicated sites.
    // Y24.27: unified mode switch (was duplicated 4x — R/P/F/H/O/Y inline + M cycle + B + T).
    //   B/T were missing reset_search() (fixed).
    void switch_mode(AppMode new_mode);
    void build_episode_children_from_cache(
        TreeNodePtr parent,
        const std::vector<std::tuple<std::string, std::string, int, bool, bool, std::string, bool,
                                     std::string>> &episodes,
        bool is_opml = false);
    void play_episode(TreeNodePtr node);

    // ── app_download.cpp ───────────────────────────────────────────────────────
    // D43: the download EXECUTION engine (start_one_download / ytdlp_download / pump + the verify
    //   helpers capture_exec/probe_media_duration/verify_downloaded_file + pending_downloads_)
    //   moved to DownloadService (download_service.h/.cpp). App keeps download_node — the thin
    //   orchestrator (gather marks → download_.enqueue/pump → clear marks → save_cache).
    void download_node(int marked_count);

    // ── app_search.cpp ─────────────────────────────────────────────────────────
    void perform_online_search();
    // META-4: query-taking core behind perform_online_search — also the remote
    //   (Squeezer input box) entry for O-mode searches.
    void run_online_search(const std::string &query);
    // META-6: Douyin signed keyword search (T-mode bare keyword / remote 🔍).
    void run_tiktok_keyword_search(const std::string &query);
    // META-6: remote-admin surface (login flows + shared post-auth bodies).
    //   start_remote_login returns the authorization URL immediately and finishes
    //   on the pool; "tiktok" is a reserved slot (err="reserved").
    bool start_remote_login(const std::string &mode, std::string &url_out, std::string &code_out,
                            std::string &err_out);
    void finish_google_login(const GoogleOAuth::TokenResult &tr);
    void finish_bilibili_login(const BilibiliAPI::LoginResult &login);
    void perform_online_search_from_favourite();
    void load_search_history_children(TreeNodePtr node);
    // Y23.1: B/Y search-record cache (mirror O-mode online_root).
    TreeNodePtr build_search_result_node(const std::string &source, const nlohmann::json &r);
    // D11-3c: make_search_history_child relocated to LibraryService (pure node construction).
    // Y23.2: shared finalizer (mode-agnostic; mode is just `source`).
    void finalize_search(const std::string &source, int account_id, TreeNodePtr account_node,
                         const std::string &query, const nlohmann::json &results_json);
    void load_search_record_children(TreeNodePtr node, const std::string &source);
    void expand_search_history(TreeNodePtr node);
    void add_search_record(const std::string &source, int account_id, TreeNodePtr account_node,
                           const std::string &query, const std::string &results_json,
                           std::vector<TreeNodePtr> result_nodes);
    void reset_search();
    void perform_search();
    // D11-3b: search_recursive + the context-aware collection moved into SearchService
    //   (search_::search_recursive / collect_context_matches / cycle_match). D12-2: reveal_node also
    //   moved there (pure tree expand), and jump_to_match's cursor jump is eventized via
    //   pending_select (deferred to the run loop) — App keeps only perform_search (entry).
    void jump_search(int dir);
    void jump_to_match(int idx);

    // ── app_subscriptions.cpp ──────────────────────────────────────────────────
    void add_feed();
    void subscribe_online_podcast();
    void subscribe_online_podcasts_batch(int marked_count);
    void subscribe_favourites_batch(int marked_count);
    void add_favourites_batch(int marked_count);
    void add_favourite();
    // F42: add_local_folder removed (dead code — never called; 'a' uses add_local_files).
    // BUG/new: 'a' in FAVOURITE mode — recursively scan a local folder for audio/video files
    //   and add them as playable children of a new folder node under fav_root.
    void add_local_files();

    // ── app_navigation.cpp ─────────────────────────────────────────────────────
    void nav_up() {
        if (library_.selected_idx() > 0)
            library_.selected_idx()--;
    }
    void nav_down() {
        if (library_.selected_idx() < (int)library_.display_list().size() - 1)
            library_.selected_idx()++;
    }
    void nav_top() {
        library_.selected_idx() = 0;
        library_.view_start() = 0;
    }
    // When the list is empty, size()-1 underflows to SIZE_MAX, casts to -1 as int, then out-of-bounds access
    void nav_bottom() {
        library_.selected_idx() =
            library_.display_list().empty() ? 0 : (int)library_.display_list().size() - 1;
    }
    // Y24.54: jump to the currently playing node — switch to its mode, expand ancestors, select + scroll.
    void jump_to_playing();
    void nav_page_up() {
        library_.selected_idx() -= PAGE_SCROLL_LINES;
        if (library_.selected_idx() < 0)
            library_.selected_idx() = 0;
    }
    void nav_page_down();
    void go_back();
    void enter_node(int marked_count);
    // Y24.39: enter_node split into focused sub-functions (was a ~175-line god function).
    void enter_marked(int marked_count);        // play the first marked episode, then clear marks
    void enter_folder_expand(TreeNodePtr node); // FOLDER / PODCAST_FEED expand-or-collapse dispatch
    void enter_favourite_folder(
        TreeNodePtr node);             // FAVOURITE-mode expansion (local/link/search/feed/folder)
    void enter_leaf(TreeNodePtr node); // playable leaf node → play_episode
    void toggle_mark();
    void toggle_sort_order();

    // ── app_tree_expand.cpp ────────────────────────────────────────────────────
    void mark_cached_nodes(TreeNodePtr node);
    std::vector<TreeNodePtr> *get_root_by_mode_string(
        const std::string &mode_str); // E: returns the mode's items vector (was TreeNodePtr root)
    bool should_use_radio_loader(const std::string &source_mode, NodeType node_type,
                                 const std::string &url);
    void sync_link_node_status(TreeNodePtr target);
    bool expand_link_node(TreeNodePtr node);
    TreeNodePtr find_node_by_url(TreeNodePtr root, const std::string &url);
    bool load_favourite_children_from_cache(TreeNodePtr node);
    void expand_local_folder(TreeNodePtr node, bool force = false);

    // ── app_input.cpp ──────────────────────────────────────────────────────────
    void handle_mouse_event();
    void configure_proxy();
    // Y02: set the YouTube cookies.txt path (latest yt-dlp requires cookies; oauth2 removed).
    void configure_cookies();
    void open_command_window();
    void handle_input(int ch, int marked_count);

    // ── app_remote.cpp (N line: network control) ───────────────────────────────
    // Drain the remote command queue and dispatch each command on the UI thread. Called once
    //   per frame from run(). N01: logs the action only; the real action mapping lands in N02+.
    void drain_remote_commands();
    void dispatch_remote(const RemoteCommand &cmd);

    // ── app_nodes.cpp ──────────────────────────────────────────────────────────
    void delete_node(int marked_count);
    void clear_feed_cache(TreeNodePtr feed);
    void collect_playable_marked(const TreeNodePtr &node, std::vector<TreeNodePtr> &list);
    void collect_playable_items(const TreeNodePtr &node, std::vector<TreeNodePtr> &list);
    void clear_marks(const TreeNodePtr &node);
    void clear_all_marks();
    void confirm_visual_selection();
    bool remove_node(TreeNodePtr parent, TreeNodePtr target);
    void edit_node();
    void refresh_node();

    // ── app_run.cpp ─────────────────────────────────────────────────────────────
    // D35: Extract Method — run()'s startup/shutdown bookends. run() is now the pure
    //   frame loop; the one-time init (frontend/mpv/services/data load/remote server)
    //   and the post-loop teardown (persist state + _exit) are named for readability.
    void startup();
    void shutdown();
    bool
    check_exit_requests(); // D40: SIGINT/termination/sleep-timer → sets running=false + returns true to break
    void drain_frame_events(); // D40: per-frame remote/playback/state drain (UI thread)
    // D37: Extract Method — the per-frame tree-locked display-build phase of run()'s loop.
    bool build_frame_display();
    // D38: per-frame render context (Replace Method with Method Object — Fowler). The frame
    //   loop computes a handful of temporaries each frame — player state, the derived
    //   AppState, the selection/download snapshots — that the prepare and draw phases both
    //   touch. Bundling them into one struct lets the loop delegate those phases to named
    //   methods without long parameter lists (the plain-Extract-Method alternative would
    //   need a 5-arg draw(state, app_state, marked, sel_node, downloads)).
    struct FrameCtx {
        MPVController::State state;
        AppState app_state = AppState::BROWSING;
        int marked = 0;
        TreeNodePtr sel_node;
        std::vector<DownloadProgress> downloads;
    };
    FrameCtx prepare_frame(); // D38: state + derived AppState + selection/download snapshots
    void draw_frame(
        const FrameCtx
            &f); // D39: per-frame draw under playlist_mutex_ (snapshot + dctx + frontend_->draw)
    // D11-3c: load_history_to_root relocated to LibraryService (the history_root_ owner).
    void load_persistent_data();
    void save_persistent_data();
    void restore_player_state();
    // D11-3c: load_radio_root relocated to LibraryService (the radio_root_ owner).
    void spawn_load_radio(TreeNodePtr node, bool force = false);
    void spawn_load_feed(TreeNodePtr node);
    // Y24.40: spawn_load_feed's worker lambda split into focused helpers (was a ~230-line god lambda).
    //   parse_feed_by_type: Apple lookup + YouTube cache + ParserRegistry dispatch → result tree.
    //     cur_url_out = (possibly Apple-rewritten) URL; abort_out=true ⇒ Apple lookup failed,
    //     node state already set, caller must skip commit.
    TreeNodePtr parse_feed_by_type(TreeNodePtr node, const std::string &url, URLType url_type,
                                   std::string &cur_url_out, bool &abort_out);
    void cache_youtube_videos(TreeNodePtr node, URLType cur_type, const std::string &cur_url,
                              TreeNodePtr &result); // cache miss: parse + backfill YouTubeCache
    void commit_feed_result(TreeNodePtr node, TreeNodePtr result,
                            const std::string &cur_url); // tree write + episode_cache + save_cache
    void load_default_podcasts();
    void flatten(const TreeNodePtr &node, int depth, bool is_last, int parent_idx);
    // E refactor: display iterates the current mode's TOP-LEVEL items directly — the per-mode
    //   root node is never in the display domain (not "hidden", just not part of the input).
    //   items_for_mode returns the active mode's top-level item list; flatten_items flattens it.
    std::vector<TreeNodePtr> &items_for_mode(AppMode m);
    std::vector<TreeNodePtr> &cur_items();                    // items_for_mode(mode)
    void flatten_items(const std::vector<TreeNodePtr> &tops); // flatten each top at depth 0
    // E: mode-level helpers — operate on cur_items() (looping the subtree-recursion fns per
    //   top-level item), replacing the old "pass current_root as the container" pattern.
    int count_marked_current();
    void clear_marks_current();
    void collect_marked_current(std::vector<TreeNodePtr> &list);
    void collect_playable_marked_current(std::vector<TreeNodePtr> &list);
    bool
    remove_from_current(TreeNodePtr target); // erase a top-level item, else recurse to its parent

    // ── app_account.cpp (Y01: Y mode + Google account login) ───────────────────
    // D11-3c: load_accounts_root (build/refresh account_root) relocated to LibraryService
    //   (the account_root_ owner). UI-coupled login/activate/delete ops stay here.
    // a (another=false) / A (another=true): run SmartTube-style OAuth device flow with a QR
    //   popup, fetch identity, persist the account, set it active, refresh the tree.
    void start_account_login(bool another);
    // Y02: subscribe the selected YouTube search-result channel to the active account.
    void subscribe_youtube_channel(TreeNodePtr node);
    // Y02: `/` in Y mode — search YouTube (mixed C/V/P, optional c/v/p/m prefix) and build a
    //   tagged result list under the active account.
    // Y23.4-1: LYRIC Method B + TranscriptParser module
    // Y24.7: subtitle handling centralized in SubtitleManager (detect/probe/load/download/offset/status).
    // D10-1: SubtitleManager + TranscriptionEngine moved into SubtitleService (owns both; lifecycle
    //   init/shutdown/poll + accessor redirect). App reaches the engines via
    //   subtitle_.subtitle_mgr() / subtitle_.transcription_engine(); PlaybackService still gets them
    //   via attach() (App passes the accessors at the call site). D10-2/3 will move the subtitle
    //   input onto the bus and make the service event-driven.
    SubtitleService subtitle_;
    // D43: download execution engine (owns pending_downloads_/start_one/ytdlp/pump + verify helpers).
    //   App keeps download_node as the orchestrator (enqueue/pump here). Declared after
    //   library_/pool_/subtitle_ so its ref ctor {library_, pool_, subtitle_} sees them initialized.
    DownloadService download_{library_, pool_, subtitle_};
    // Y23.9/D9-3: BUFFERING state (playback_pending_(_start_) + the 30s timeout / has_media logic)
    //   moved into PlaybackService (advance_buffering). App owns NO playback-state member now.
    void perform_youtube_search(const std::string &preset = "");
    // Y15: B-mode (Bilibili). D11-3c: load_bilibili_root relocated to LibraryService.
    // DB-11: delete the Bilibili account under the cursor (wired to 'd' in B mode).
    void delete_bilibili_account_node(TreeNodePtr node);
    void start_bilibili_login();
    void expand_bilibili_account(TreeNodePtr node);
    // Y22: lazy-expand the B-account's Subscriptions / History children.
    // CACHE-1: force=true bypasses the list_cache read (used by 'r' to re-fetch online).
    void expand_bili_followings(TreeNodePtr node, bool force = false);
    void expand_bili_history(TreeNodePtr node, bool force = false);
    // Y-fix: 'r' on a B-account's Subscriptions / History / account node → force re-fetch
    //   (B mode previously had no refresh path at all — 'r' was a silent no-op).
    void refresh_bili_followings(TreeNodePtr node);
    void refresh_bili_history(TreeNodePtr node);
    void refresh_bilibili_account(TreeNodePtr node);
    // Y23: subscribe to a Bilibili UP from a 👤 search result ('a').
    void subscribe_bilibili_up(TreeNodePtr node);
    void perform_bilibili_search(const std::string &preset = "");
    // Y24.11/16: T-mode (TikTok/Douyin) — anonymous, no login. Subscribed items listed from DB;
    //   'a' adds @user/video URL, '/' opens @user/#tag/URL, Enter loads a creator's videos via
    //   yt-dlp --flat-playlist (+--geo-bypass-country), 'b' cycles region (13: 12 TikTok + CN=Douyin).
    //   D11-3c: load_tiktok_root relocated to LibraryService (the tiktok_root_ owner).
    void start_douyin_login(); // Y24.xx: 'a' in T mode — Douyin QR scan login (terminal QR)
    void
    tiktok_subscribe(const std::string &input); // shared core: TikTok creator / Douyin video leaf
    void tiktok_direct_input();                 // '/' handler: @user / #tag / URL dispatch
    void tag_browse(const std::string &tag); // '#' → yt-dlp tag list (transient; dormant upstream)
    void delete_tiktok_user_node(TreeNodePtr node);

    // ── app_iptv.cpp (Y24.50: I-mode, iptv-org playlists) ──────────────────────────
    // D11-3c: load_iptv_root (catalog top-level) relocated to LibraryService (the iptv_root_ owner).
    void expand_iptv_node(TreeNodePtr node); // lazy fetch+parse m3u/json on expand (async, cached)
    // yt-dlp --flat-playlist listing (TikTok creator / tag; Douyin user unsupported — no DouyinUserIE).
    //   region = TikTok geo-bypass country code (Douyin: "CN"). cookies_file optional. Retries on
    //   transient empty output.
    static TreeNodePtr parse_tiktok_user_videos(const std::string &url, const std::string &region,
                                                const std::string &cookies_file,
                                                const std::string &title);
    // Y23.1: r/d on a 🔍 search record (rerun / delete); no-op on the container.
    void rerun_search_record(TreeNodePtr node);
    void delete_search_record(TreeNodePtr node);
    // Enter/activate a Y-mode node: account node → set active; history/subscriptions/channel →
    //   lazy-load children from the per-account tables / YouTube API.
    void enter_account_node(TreeNodePtr node);
    // Lazy-expand a "History" / "Subscriptions" / channel child (populate children).
    void expand_account_child(TreeNodePtr node);
    // d on a Y-mode account node: delete the account (after confirm).
    void delete_account_node(TreeNodePtr node);
    // r on a Y-mode account node: re-sync subscriptions + watch history.
    void resync_account_node(TreeNodePtr node);
    // Y-fix: 'r' on the Subscriptions / History container → re-sync only that subtree from
    //   Google and reload it (preserves the rest of the tree + other accounts' expansion).
    void refresh_account_subs(TreeNodePtr node);
    void refresh_account_history(TreeNodePtr node);

    // ── app_sync.cpp (Y01: YouTube sync, bidirectional) ─────────────────────────
    void sync_account_subscriptions(int account_id);

    void sync_account_history(int account_id);
    // Record a YouTube play (under the active account) into youtube_history (local side).
    void record_youtube_play(const std::string &video_id, const std::string &title,
                             const std::string &channel_name = "");

    // ── Inline helpers (short, used across multiple translation units) ─────────
    int count_marked_safe(const TreeNodePtr &node) {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        return count_marked(node);
    }

    int count_marked(const TreeNodePtr &node) {
        int count = node->marked ? 1 : 0;
        for (auto &child : node->children)
            count += count_marked(child);
        return count;
    }

    void collect_marked(const TreeNodePtr &node, std::vector<TreeNodePtr> &list) {
        if (node->marked)
            list.push_back(node);
        for (auto &child : node->children)
            collect_marked(child, list);
    }

    // ─── Local folder support (added via A in F mode) ───────────────────────────
    // Determine whether an extension is an audio/video format playable by MPV/ffmpeg.
    static bool is_media_extension(const std::string &ext) {
        static const std::set<std::string> exts = {
            // ── Lossy audio ──
            ".mp3", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".wma", ".amr", ".awb", ".mpc", ".ofr",
            ".ofs", ".spx", ".qcp", ".ra", ".rm", ".m4r",
            // ── Lossless audio ──
            ".flac", ".alac", ".ape", ".wv", ".tta", ".tak", ".wav", ".aiff", ".aif", ".aifc",
            ".caf",
            // ── DSD (Direct Stream Digital) ──
            ".dsf", ".dff", ".dsd",
            // ── Cinema / multi-channel audio ──
            ".ac3", ".dts", ".dtshd", ".eac3",
            // ── Audiobooks / container audio ──
            ".m4b", ".mka",
            // ── Video ──
            ".mp4", ".m4v", ".webm", ".mkv", ".avi", ".mov", ".flv", ".ogv", ".ogm", ".ts", ".m2ts",
            ".mts", ".mpg", ".mpeg", ".vob", ".wmv", ".asf", ".rmvb", ".3gp", ".3g2", ".f4v",
            ".mxf", ".y4m", ".divx", ".nut"};
        return exts.count(ext) > 0;
    }

    // Sort comparison function: supports digit-prefixed titles
    // Digit-prefixed (0-9) < letter-prefixed (A-Z)
    static bool title_compare_asc(const std::string &a, const std::string &b) {
        bool a_starts_digit = !a.empty() && std::isdigit(static_cast<unsigned char>(a[0]));
        bool b_starts_digit = !b.empty() && std::isdigit(static_cast<unsigned char>(b[0]));

        // Digit-prefixed sorts first
        if (a_starts_digit && !b_starts_digit)
            return true;
        if (!a_starts_digit && b_starts_digit)
            return false;

        // Same type: alphabetical order
        return a < b;
    }

    static bool title_compare_desc(const std::string &a, const std::string &b) {
        bool a_starts_digit = !a.empty() && std::isdigit(static_cast<unsigned char>(a[0]));
        bool b_starts_digit = !b.empty() && std::isdigit(static_cast<unsigned char>(b[0]));

        // Reverse: letters first, digits last
        if (a_starts_digit && !b_starts_digit)
            return false;
        if (!a_starts_digit && b_starts_digit)
            return true;

        // Same type: reverse alphabetical order
        return a > b;
    }
};

// app_bilibili.cpp, shared with app_remote_admin.cpp (META-6)
int save_bilibili_account(const BilibiliAccount &a);
void write_bilibili_cookies(const BilibiliAccount &a);

} // namespace panicast
