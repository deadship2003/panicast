#include <unistd.h> // _exit (skip destructors on clean exit)
#include "panicast/app/app.h"
#include "panicast/app/actions.h"
#include "panicast/app/playback_events.h"
#include "panicast/core/event_bus.h"
#include "panicast/net/bilibili_api.h"
#include "panicast/net/lms_server.h"   // N08: mini-LMS (Squeezer) — guarded use; ON builds only
#include "panicast/ui/null_frontend.h" // N09/S1: daemon frontend
#include "panicast/net/network.h"      // D45-fix: Network::init_proxy_routing after IniConfig load
#include "panicast/net/tiktok_region.h"
#include "panicast/parsers/bilibili_parser.h"
#include <cstdio>
#include <iostream>

namespace panicast
{

App::App() {
    Logger::instance().init();
    IniConfig::instance().load();
    // D45-fix: wire proxy routing AFTER the INI is loaded (was a network.cpp static initializer
    //   running before load() → bilibili_direct=false in the INI was silently ignored).
    Network::init_proxy_routing();
    // Initialize SQLite3 database
    DatabaseManager::instance().init();
    CacheManager::instance()
        .load(); // load media_cache into memory (else get_local_file empty -> downloaded items buffer)
    YouTubeCache::instance().load();

    // E: the 8 per-mode roots are now std::vector<TreeNodePtr> (default-constructed empty);
    //   no TreeNode root nodes to create. (online_root lives in OnlineState.)
    // Y24.xx: region grouping removed — T mode is fixed (TikTok=US; Douyin detected by URL).
    tiktok_region_ = "US";
    TikTokRegion::set_current(tiktok_region_);

    // Load the global play mode at startup (no persisted playlist anymore)
    play_mode = IniConfig::instance().get_play_mode();
    LOG(fmt::format("[INIT] play_mode={}", static_cast<int>(play_mode)));
    subtitle_.init(pool_,
                   player); // D10-1: SubtitleService wires its own SubtitleManager + engine
                            //   (Y24.28: mpv for video ASR; Y24.19: whisper.cpp transcription)
}

// P1-8: explicit destructor — stop the mpv event thread and join the pool BEFORE any member
//   is destroyed. The playlist queue (current_playlist / playlist_mutex_) now lives in
//   PlaybackService (D8b-1), declared right after player_; without this explicit stop the mpv
//   event thread could still fire on_playback_ended (touching the queue) during stack unwinding
//   → use-after-free. Order: player.stop() joins the mpv event thread, then pool_.shutdown()
//   joins workers.
App::~App() {
    running = false;
    // N01: stop the remote control server FIRST (joins accept + worker threads) and disable
    //   the command bus so any in-flight push becomes a no-op, before tearing down the rest.
    try {
        remote_server_.stop();
    } catch (...) {
    }
#ifdef PANICAST_REMOTE_LMS
    // N08: singleton (not a member) so OFF builds need no #ifdef in app.h — stop it here,
    //   next to the remote server, before tearing down the rest.
    try {
        LmsServer::instance().stop();
    } catch (...) {
    }
#endif
    remote_bus_.shutdown();
    try {
        player.stop();
    } catch (...) {
    }
    subtitle_.shutdown(); // D10-1: SubtitleService teardown (Y24.19: stop transcription dispatcher)
    Utils::
        kill_all_child_processes(); // N04-fix: kill tracked yt-dlp/whisper children before joining pool
    pool_.shutdown();
}

void App::set_headless() {
    frontend_ = std::make_unique<NullFrontend>();
    headless_ = true;
}

void App::run() {
    startup();

    while (running) {
        if (headless_) {
            // N09/S1 daemon frame: same per-frame crossings as the TUI loop (exit checks +
            //   remote-command drain + playback-ended queue + remote state cache), just
            //   paced by sleep instead of the 30ms ncurses input poll. No draw, no input.
            if (check_exit_requests())
                break;
            // N10.3: the display list is ENGINE state in the daemon, not just a draw
            //   source — the remote browse mirror (RemoteStateSnapshot::browse) and the
            //   nav/nav_activate actions read display_list(). Rebuild it every frame
            //   like the TUI does (flatten + async select consume + subtitle poll all
            //   ride along; NullFrontend swallows the UI bits).
            build_frame_display();
            drain_frame_events();
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            continue;
        }

        const auto frame_start_ = std::chrono::steady_clock::now();
        // D40: SIGINT (CTRL+C) / termination-signal / sleep-timer exit checks extracted to
        //   check_exit_requests() (sets running=false + returns true when the loop should break).
        if (check_exit_requests())
            break;

        // Unified window-size detection and scroll-offset reset
        // LayoutMetrics auto-detects size changes and resets all scroll offsets
        if (LayoutMetrics::instance().check_resize()) {
            // Use double buffering to avoid clear+refresh single-frame white flicker
            // Old code: clear(); refresh();  ← immediate refresh, white frame
            resizeterm(LINES, COLS);
            werase(stdscr);
            wnoutrefresh(stdscr);
            doupdate();
            EVENT_LOG(fmt::format("Terminal resized: {}x{}", COLS, LINES));
        }

        // D40: per-frame cross-thread drain (remote commands + queued playback-ended + remote
        //   state cache) extracted to drain_frame_events().
        drain_frame_events();

        FrameCtx f = prepare_frame();

        int vh = LINES - 5;
        if (vh < 1)
            vh = 1;
        if (library_.selected_idx() < library_.view_start())
            library_.view_start() = library_.selected_idx();
        else if (library_.selected_idx() >= library_.view_start() + vh)
            library_.view_start() = library_.selected_idx() - vh + 1;
        if (library_.view_start() < 0)
            library_.view_start() = 0;

        draw_frame(f);

        // Wide-char input: wget_wch cleanly distinguishes special keys (KEY_CODE_YES) from
        //   committed characters (OK). In browsing mode only ASCII (<128) + special keys are
        //   dispatched to handle_input; non-ASCII (IME-committed CJK etc.) is silently dropped
        //   so it neither triggers commands nor pollutes the screen. Text-input popups
        //   (input_box/search/`:`) read their own input and still accept full UTF-8.
        wint_t wch;
        int wrc = wget_wch(stdscr, &wch);
        if (wrc == KEY_CODE_YES) {
            handle_input(static_cast<int>(wch), f.marked); // special key / mouse
        } else if (wrc == OK && wch < 128) {
            handle_input(static_cast<int>(wch), f.marked); // ASCII only
        }
        // wrc == ERR (no input) or non-ASCII char → drop silently
        const long _frame_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - frame_start_)
                                   .count();
        if (_frame_ms > 150)
            LOG(fmt::format("[WATCHDOG] slow frame: {}ms (tree/pl wait logged above if >80ms)",
                            _frame_ms));
    }

    shutdown();
}

// D40: per-frame exit-request check (Extract Method from run()'s loop). Handles the three
//   loop-terminating conditions — CTRL+C/SIGINT (with a confirm popup), termination signals
//   (SIGHUP/SIGTERM/SIGQUIT: flush & exit immediately WITHOUT a popup — there may be no
//   terminal to interact with; pool_.shutdown() runs in shutdown() so in-flight YouTube parses
//   finish writing episode_cache, avoiding the "abnormal exit loses the YouTube channel" bug),
//   and the sleep timer. Sets running=false and returns true when the loop should break; the
//   caller does `if (check_exit_requests()) break;`.
bool App::check_exit_requests() {
    if (g_exit_requested) {
        g_exit_requested = false; // reset flag
        if (g_crash_sig != 0) {
            g_crash_sig = 0;
            EVENT_LOG(fmt::format("Termination signal received, flushing cache before exit..."));
            running = false;
            return true;
        }
        if (frontend_->confirm_box("Quit panicast? (CTRL+C)")) {
            running = false;
            return true;
        }
    }
    if (SleepTimer::instance().is_active() && SleepTimer::instance().check_expired()) {
        EVENT_LOG("Sleep timer expired, exiting...");
        running = false;
        return true;
    }
    return false;
}

// D40: per-frame cross-thread drain (Extract Method from run()'s loop). Three UI-thread
//   drains that must run each frame:
//   - N01: remote commands (server→bus→here), non-blocking no-op when empty/disabled.
//   - D4: the playback-ended handler. The mpv event thread only QUEUES END_FILE here —
//     previously it ran on_playback_ended on the mpv thread under playlist_mutex_, contending
//     with this loop's draw lock and freezing the TUI after pause (a paused live stream drops →
//     END_FILE → the mpv thread blocks the UI's draw). Draining here runs it on the UI thread.
//   - N02: refresh the remote state-snapshot cache for query commands (status/currentsong).
void App::drain_frame_events() {
    drain_remote_commands();
    int _end_reason = pending_end_reason_.exchange(-1);
    if (_end_reason != -1)
        playback_.on_playback_ended(_end_reason, mode, play_mode);
    update_remote_state_cache();
}

// D38: per-frame render-context preparation (Extract Method + Method Object from run()'s loop).
//   Pulls the previously-inline compute+gather phase — player state, the derived AppState
//   (BUFFERING/PAUSED/PLAYING/BROWSING, merged with the node-loading flag), and the
//   selection/download snapshots — into one named method returning FrameCtx. run()'s loop
//   shrinks to a flat prepare→scroll→draw→input→watchdog skeleton; the draw phase reads the
//   same values back from the struct instead of ~5 interleaved loop locals.
App::FrameCtx App::prepare_frame() {
    FrameCtx f;
    f.state = player.get_state();

    // D9-3: the buffering lifecycle (pending + 30s timeout + has_media clear + the one-time
    //   buffering-duration log) is owned by PlaybackService::advance_buffering (App holds no
    //   playback-state member). It returns true while a just-started track is still pending mpv
    //   load (<30s); the PLAYING/PAUSED vs mpv-idle-BUFFERING distinction still derives from mpv
    //   here (core_idle/paused). Runs on the UI thread (D4 invariant unaffected).
    if (playback_.advance_buffering(f.state.has_media)) {
        f.app_state = AppState::BUFFERING; // playback initiated, mpv still loading (<30s)
    } else if (f.state.has_media) {
        if (f.state.core_idle && !f.state.paused) {
            f.app_state = AppState::BUFFERING; // has media, idle and not paused
        } else if (f.state.paused) {
            f.app_state = AppState::PAUSED;
        } else {
            f.app_state = AppState::PLAYING;
        }
    } else {
        f.app_state = AppState::BROWSING; // no media, navigating
    }

    const bool is_loading = build_frame_display();
    // Node-loading state has higher priority than browsing but lower than playback states.
    if (is_loading && f.app_state == AppState::BROWSING)
        f.app_state = AppState::LOADING;

    f.marked = count_marked_current();
    f.sel_node = (library_.selected_idx() >= 0 &&
                  library_.selected_idx() < (int)library_.display_list().size())
                     ? library_.display_list()[library_.selected_idx()].node
                     : nullptr;
    f.downloads = ProgressManager::instance().get_all();
    // Free slots (completed entries just cleared by get_all) → promote pending downloads,
    //   capping active+visible entries at MAX_CONCURRENT_DOWNLOADS.
    download_.pump(f.downloads.size());
    // Collapsed summary for queued items (keeps the INFO panel from being overrun).
    if (!download_.pending_downloads().empty()) {
        DownloadProgress syn;
        syn.title = fmt::format("··· +{} pending download", download_.pending_downloads().size());
        syn.active = false;
        syn.complete = false;
        syn.failed = false;
        syn.is_youtube = false;
        f.downloads.push_back(syn);
    }
    return f;
}

// D39: per-frame draw (Extract Method from run()'s loop). Snapshots the playing pointer
//   + INFO play-context (history 3 + next 3) under playlist_mutex_, builds the DisplayContext
//   view-model bag, and hands everything to the frontend. P1.2 (Y23.5): the lock is held for
//   draw ONLY — this method returns before run()'s input phase (handle_input → play_current →
//   locks playlist_mutex_ → would deadlock if still held). All inputs come from FrameCtx +
//   members; current_index/next/history are computed here and consumed only by the draw call.
void App::draw_frame(const FrameCtx &f) {
    // Snapshot the playing pointer + the INFO play-context (history 3 + next 3)
    //   under the lock. P1.2 (Y23.5): hold the lock during draw ONLY (not during input —
    //   handle_input → play_current → locks playlist_mutex_ → would deadlock if held).
    // Pointer-driven model: current_index is app-owned (set by play_current /
    //   on_playback_ended), NOT derived from mpv's playlist_pos. No per-frame sync from
    //   mpv is needed.
    int current_index_snap = playback_.current_index();
    std::vector<int> next_snap;
    std::string cur_url_snap;
    const auto _pw0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> pl_draw_lock(playback_.playlist_mutex()); // released before input
    const long _pl_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - _pw0)
                                 .count();
    if (_pl_wait_ms > 80)
        LOG(fmt::format("[WATCHDOG] waited {}ms for playlist_mutex_", _pl_wait_ms));
    {
        current_index_snap = playback_.current_index();
        int n = static_cast<int>(playback_.playlist().size());
        if (current_index_snap >= 0 && current_index_snap < n) {
            cur_url_snap = playback_.playlist()[current_index_snap].url;
            if (play_mode == PlayMode::REPEAT) {
                for (int k = 0; k < 3; ++k)
                    next_snap.push_back(current_index_snap);
            } else if (play_mode == PlayMode::CYCLE) {
                for (int k = 1; k <= 3; ++k)
                    next_snap.push_back((current_index_snap + k) % n);
            } else { // SHUFFLE — pre-generated lookahead
                int cnt = 0;
                for (int idx : playback_.shuffle_queue()) {
                    next_snap.push_back(idx);
                    if (++cnt == 3)
                        break;
                }
            }
        }
    }

    // Global play history (last 3, excluding the currently playing track)
    // P2 (Y23.7): throttle get_history(8) DB query to ~1/sec (was every frame ~33fps).
    //   record_play_history (on track change) invalidates immediately.
    static int hist_frame_cnt = 0;
    static std::vector<std::string> cached_hist_titles;
    static std::string cached_hist_url;
    if (++hist_frame_cnt >= 30 || (!cur_url_snap.empty() && cur_url_snap != cached_hist_url)) {
        hist_frame_cnt = 0;
        cached_hist_url = cur_url_snap;
        cached_hist_titles.clear();
        auto hist = DatabaseManager::instance().get_history(8);
        for (auto &[u, t, ts, mt, h_a, h_al, h_art] : hist) { // META-3 tail unused here
            if (!cur_url_snap.empty() && u == cur_url_snap)
                continue;
            cached_hist_titles.push_back(t);
            if (cached_hist_titles.size() >= 3)
                break;
        }
    }
    auto &hist_titles = cached_hist_titles;

    // INFO area renders the 7-line play context from playlist/current_index/
    //   play_mode + hist_titles + next_snap.
    // Y24.7: L-mode poll already ran above (handoff + activation); no separate call needed.
    // D12-1: push runtime display state into the UI — it must not query these singletons
    //   itself (runtime/business state; see docs/ARCHITECTURE.md §2.1). Region codes are
    //   resolved to display names here so the UI renders plain values. URLClassifier stays
    //   a direct call inside the UI (stateless pure function, cross-cutting infra).
    DisplayContext dctx;
    dctx.sleep_active = SleepTimer::instance().is_active();
    dctx.sleep_remaining = dctx.sleep_active ? SleepTimer::instance().remaining_seconds() : 0;
    dctx.online_region_name = ITunesSearch::get_region_name(OnlineState::instance().current_region);
    dctx.tiktok_region = TikTokRegion::current();
    // D14-3/D15: canonical now-playing identity from PlaybackService. The UI reads the
    //   playing track's url+title from this view-model bag instead of a domain TreeNodePtr,
    //   so cached items identify by source URL (not the played path) and the render
    //   contract stays view-model-only for the playing track.
    const Media np = playback_.now_playing();
    dctx.now_playing_url = np.id.url();
    dctx.now_playing_title = np.title;

    frontend_->draw(mode, library_.display_list(), library_.selected_idx(), f.state,
                    library_.view_start(), f.app_state, f.marked, search_.search_query(),
                    search_.current_match_idx(), search_.total_matches(), f.sel_node, f.downloads,
                    visual_mode_, visual_start_, playback_.playlist(), current_index_snap,
                    play_mode, hist_titles, next_snap, dctx);
}

// D35: run() startup bookend — one-time init before the frame loop.

// D37: per-frame tree-locked display build (Extract Method from run()'s loop).
//   Acquires tree_mutex (watchdog-timed), rebuilds the display list (flatten), consumes a
//   pending async select, polls subtitles, refreshes lyric history, resolves lyric-bar
//   activation. Returns whether any displayed node is still loading (drives LOADING state).
bool App::build_frame_display() {
    bool is_loading = false;
    const auto _tw0 = std::chrono::steady_clock::now();
    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
    const long _tree_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - _tw0)
                                   .count();
    if (_tree_wait_ms > 80)
        LOG(fmt::format("[WATCHDOG] waited {}ms for tree_mutex", _tree_wait_ms));
    library_.display_list().clear();
    flatten_items(cur_items());
    for (const auto &item : library_.display_list()) {
        if (item.node->loading) {
            is_loading = true;
            break;
        }
    }
    // Y11: consume a pending async selection (set by a pool task that built a new node,
    //   e.g. YouTube search results, or D12-2's eventized jump_to_match) — move the cursor
    //   to it once, then clear. D12-2: also center the view (was jump_to_match's inline
    //   scroll) so search-jump, jump_to_playing, and async selects all center consistently.
    if (library_.pending_select()) {
        for (size_t i = 0; i < library_.display_list().size(); ++i) {
            if (library_.display_list()[i].node == library_.pending_select()) {
                library_.selected_idx() = (int)i;
                library_.view_start() = std::max(0, (int)i - (LINES - 5) / 2);
                break;
            }
        }
        library_.pending_select().reset();
    }
    // Y24.7: SubtitleManager poll — handoff pending transcript to UI + offset + logs.
    subtitle_.poll(*frontend_, frontend_->is_lyric_bar_requested());
    // Y24.48: refresh lyric history EVERY frame (even when the bar is inactive) so an
    //   embedded sub cue (sub_text) is detected and can auto-open the bar.
    frontend_->update_lyric_history(player.get_state());
    // Y24.48: LYRIC bar activation — default CLOSED. Auto-open only when a displayable
    //   subtitle source exists (transcript READY / ASR running / embedded cue seen this
    //   track), OR the user manually opened (L). Manual=Closed suppresses auto until track
    //   change. VO open (video window) → always closed (subs render in mpv's window).
    bool lyric_active = false;
    if (!player.is_video_window_open() && frontend_->is_lyric_bar_requested()) {
        switch (frontend_->lyric_manual()) {
        case LyricManual::Open:
            lyric_active = true;
            break;
        case LyricManual::Closed:
            lyric_active = false;
            break;
        default: // Auto
            lyric_active = subtitle_.subtitle_mgr().status() == TranscriptStatus::READY ||
                           subtitle_.transcription_engine().realtime_running() ||
                           frontend_->embedded_sub_confirmed();
            break;
        }
    }
    frontend_->set_lyric_bar_active(lyric_active);
    return is_loading;
}
void App::startup() {
    frontend_->init();
    // Confirm XML error handler is set (already set in main; this is a secondary confirmation)
    // Redirect errors to LOG/EVENT_LOG to avoid terminal output causing screen corruption
    xmlSetGenericErrorFunc(NULL, xml_error_handler);
    // libxml2 2.12+ callback signature uses const xmlError*; 2.9.x uses non-const.
    //   The handler uses the const version (for newer releases); this cast compatibilizes older libxml2 (e.g. Debian 12).
    xmlSetStructuredErrorFunc(NULL, (xmlStructuredErrorFunc)xml_structured_error_handler);
    if (!player.initialize())
        LOG("MPV initialization failed");

    // Register playback-ended callback to auto-play the next track
    player.set_end_file_callback([this](int reason) { pending_end_reason_.store(reason); });
    LOG("[AUTOPLAY] End-file callback registered");

    // D8: PlaybackService (first Application Service) owns the playback Action handlers
    //   (pause/volume) — subscribed via playback_.init(). Nav Actions stay here for now.
    playback_.init();
    // D8b-2: inject pool_/subtitle (declared after playback_ in App, so they can't be
    //   construction-time references). play_current / on_playback_ended live in PlaybackService
    //   now and own the track handles (playback_node_ / playback_mode_) directly (D9-2) — App reads
    //   them via playback_.playback_node() / playback_.playback_mode(). Must run before the loop.
    //   D11-1: subtitle is fully event-driven (SubtitleService subscribes the playback events), so
    //   attach() now takes only the pool — PlaybackService no longer references SubtitleService.
    playback_.attach(pool_);
    // D9: subscribe to the playback state events App still consumes locally. The bus is
    //   synchronous, so these run on the publisher's thread (pool thread for history). D9-2/D9-3:
    //   PlaybackTrackChanged / PlaybackBufferingChanged no longer need App subscribers — the track
    //   and buffering state moved into the service (App reads accessors / calls advance_buffering);
    //   both events stay published as the reactor channel for future direct UI/remote subscribers
    //   (D10+). Only HistoryChanged is still consumed here (rebuild the history tree).
    action_subs_.push_back(EventBus::instance().subscribe<HistoryChanged>(
        [this](const HistoryChanged &) { library_.load_history_to_root(); }));
    // D7: Keymap (key→Action) + nav input Actions.
    build_keymap();
    action_subs_.push_back(
        EventBus::instance().subscribe<NavUpAction>([this](const NavUpAction &) { nav_up(); }));
    action_subs_.push_back(EventBus::instance().subscribe<NavDownAction>(
        [this](const NavDownAction &) { nav_down(); }));
    // D42: mode-switch keys (R/P/F/H/O/Y/B/I) → SwitchModeAction.
    action_subs_.push_back(
        EventBus::instance().subscribe<SwitchModeAction>([this](const SwitchModeAction &a) {
            switch_mode(a.target);
            if (!a.hint.empty())
                EVENT_LOG(a.hint);
        }));

    // Y07: startup runtime-dependency pre-check for YouTube playback. Warn once in the LOG
    //   panel so a missing dependency is obvious immediately, instead of a cryptic
    //   "YouTube resolve failed (no yt-dlp output)" discovered only when the user tries to play.
    {
        std::string ytdlp = Utils::which_binary("yt-dlp");
        if (ytdlp.empty()) {
            EVENT_LOG("Warning: yt-dlp not installed / not in PATH — YouTube playback and parsing "
                      "unavailable. Install: pip install -U \"yt-dlp[default]\"");
        } else {
            LOG(fmt::format("[INIT] yt-dlp: {}", ytdlp));
            std::string qjs = Utils::which_binary("qjs");
            if (qjs.empty())
                qjs = Utils::which_binary("qjsng");
            if (qjs.empty() && Utils::which_binary("deno").empty()) {
                EVENT_LOG("Warning: no qjs/qjsng/deno detected — YouTube playback needs a JS "
                          "runtime to solve nsig. Install quickjs-ng (binary qjs) or deno");
            } else if (!qjs.empty()) {
                LOG(fmt::format("[INIT] JS runtime: {}", qjs));
                // quickjs can't fetch the EJS solver from npm (deno can); needs yt-dlp[default]
                //   (yt-dlp-ejs). Probe via yt-dlp's OWN environment (yt-dlp -v lists Optional
                //   libraries incl. yt_dlp_ejs) — reliable, no false negative from system python3
                //   differing from yt-dlp's install env. Best-effort, non-fatal.
                FILE *p = popen("yt-dlp -v 2>&1 | grep -q yt_dlp_ejs", "r");
                if (p) {
                    if (pclose(p) != 0)
                        EVENT_LOG("Warning: quickjs missing EJS solver — pip install -U "
                                  "\"yt-dlp[default]\" (includes yt-dlp-ejs)");
                }
            }
        }
    }

    Persistence::load_cache(library_.radio_root(), library_.podcast_root());
    load_persistent_data();

    // Load history into library_.history_root()
    library_.load_history_to_root();

    // Y01: load Google accounts into library_.account_root() (Y mode)
    library_.load_accounts_root();
    library_.load_bilibili_root(); // Y15: B-mode
    library_.load_tiktok_root();   // Y24.11: T-mode

    // Restore last playback state
    restore_player_state();

    for (auto &it : library_.radio_root())
        mark_cached_nodes(it);
    for (auto &it : library_.podcast_root())
        mark_cached_nodes(it);

    if (library_.radio_root().empty()) {
        // Use thread pool; App destructor safely joins (old code: .detach() had use-after-free)
        pool_.submit([this]() { library_.load_radio_root(); });
    }
    // Always load built-in podcasts, merging with cached data
    load_default_podcasts();

    // Initialize layout manager
    LayoutMetrics::instance().check_resize();

    // N01: start the remote control server if [remote] enable=true (opt-in). Default off →
    //   zero impact on the local TUI. The server thread pushes RemoteCommands into remote_bus_;
    //   the main loop drains them below each frame.
    if (IniConfig::instance().get_remote_enabled()) {
        if (remote_server_.start(IniConfig::instance().get_remote_bind(),
                                 IniConfig::instance().get_remote_port(), this)) {
            EVENT_LOG(fmt::format("Remote control on — PIN {} (or {}). Open http://127.0.0.1:{}/ "
                                  "in a browser; ':pin' to show.",
                                  remote_server_.dynamic_pin(), remote_server_.universal_pin(),
                                  IniConfig::instance().get_remote_port()));
        } else {
            EVENT_LOG("Remote control server failed to start (see log); continuing without remote "
                      "control");
        }
    }

    // N08: mini-LMS (Squeezer control plane) — runtime layer of the dual gate. Compile layer
    //   is the CMake PANICAST_REMOTE_LMS option (default ON); even compiled in, nothing
    //   listens until [remote] lms_enable=true (default false → zero local-TUI impact).
#ifdef PANICAST_REMOTE_LMS
    if (IniConfig::instance().get_remote_lms_enabled()) {
        if (LmsServer::instance().start(IniConfig::instance().get_remote_bind(),
                                        IniConfig::instance().get_remote_lms_port(), this,
                                        &remote_bus_)) {
            EVENT_LOG(fmt::format("mini-LMS (Squeezer cometd) on :{} — point Squeezer at "
                                  "<this-host>:{}",
                                  IniConfig::instance().get_remote_lms_port(),
                                  IniConfig::instance().get_remote_lms_port()));
        } else {
            EVENT_LOG("mini-LMS server failed to start (see log); continuing without it");
        }
    }
#else
    // OFF build still honors the user's intent with an actionable hint instead of silence.
    if (IniConfig::instance().get_remote_lms_enabled()) {
        EVENT_LOG("mini-LMS requested ([remote] lms_enable=true) but this build was compiled "
                  "without PANICAST_REMOTE_LMS — reconfigure with cmake -DPANICAST_REMOTE_LMS=ON "
                  ".. and rebuild");
    }
#endif
}

// D35: run() shutdown bookend — post-loop teardown, persist state, then _exit(0)
//   (skips ~App so detached pool workers can't UAF members during unwinding).
void App::shutdown() {

    // Shut down the thread pool and join all background load tasks before exiting,
    //   to avoid concurrent mutations causing torn reads/crashes while save_cache serializes the tree
    // N04-fix: kill in-flight yt-dlp/whisper children FIRST so worker threads blocked in
    //   YtdlpRunner::run's poll()/waitpid() unblock and pool_.shutdown() can join promptly.
    //   Without this, a worker stuck in a 600s yt-dlp call makes the process unkillable by SIGTERM.
    Utils::kill_all_child_processes();
    pool_.shutdown();

    save_persistent_data();
    Persistence::save_cache(library_.radio_root(), library_.podcast_root());

    // Save player state
    auto player_state = player.get_state();
    int mode_int = static_cast<int>(mode);
    // D14-4/D16: canonical now-playing identity (url + title) from ONE PlaybackService::now_playing()
    //   call. D14-4 keyed the url on the source URL (not the played path); D16 also routes title
    //   through now_playing() (was a separate playback_node() indirection) so the SAVE path — like
    //   the RENDER path (D15) — reads now-playing identity from a single channel. now_playing()
    //   derives url+title from playback_node_ (both empty when nothing is playing).
    const Media np = playback_.now_playing();
    std::string canonical_url = np.id.url();
    std::string current_title = np.title;
    DatabaseManager::instance().save_player_state(
        player_state.volume, player_state.speed, player_state.paused, canonical_url,
        player_state.time_pos, frontend_->is_scroll_mode(),
        frontend_->is_show_tree_lines(), // persist user T-key preference (no longer hardcoded true)
        current_title, mode_int);
    // Also save to the progress table (dedicated to resume playback)
    if (!canonical_url.empty() && player_state.time_pos > 5.0) {
        // Align the save guard with the read-side resume guard (playback_service.cpp
        //   `ut != RADIO_STREAM || duration > 0`). RADIO_STREAM covers radio live streams, online
        //   podcast .mp3, AND local audio files (classify() folds local files into RADIO_STREAM).
        //   review-fix (2026-08-16): the read side's `duration > 0` is the NODE's duration (feed
        //   metadata; 0 for feeds without itunes:duration, search/local nodes), NOT mpv's measured
        //   media_duration — a save guard on the latter wrote rows the read side still refused to
        //   resume (exactly the dead-row accumulation this guard exists to prevent). Use the same
        //   source: playback_node() is the node now_playing() derived canonical_url from.
        //   Open-ended live streams (duration 0) stay skipped.
        TreeNodePtr pn = playback_.playback_node();
        bool finite = pn && pn->duration > 0;
        if (URLClassifier::classify(canonical_url) != URLType::RADIO_STREAM || finite) {
            bool completed = (player_state.media_duration > 0 &&
                              player_state.time_pos >= player_state.media_duration - 5.0);
            DatabaseManager::instance().save_progress(canonical_url, player_state.time_pos,
                                                      completed);
            LOG(fmt::format("[Progress] Saved: {} at {:.1f}s (completed={})", canonical_url,
                            player_state.time_pos, completed));
        } else {
            LOG(fmt::format("[Progress] Skip (live RADIO_STREAM, no duration — no resume): {}",
                            canonical_url));
        }
    }
    EVENT_LOG("Player state saved");

    // Stop the mpv event thread before exit — ensures the on_playback_ended callback
    //   no longer fires, preventing it from accessing current_playlist/playlist_mutex_
    //   (destroyed before player) during App destruction
    player.stop();

    frontend_->cleanup();
    // Kill EVERY tracked child subtree before exiting. _exit(0) below skips ~App (where
    //   kill_all_child_processes normally runs), so without this only DIRECT children die —
    //   via the kernel's PR_SET_PDEATHSIG — while GRANDCHILDREN (e.g. yt-dlp's ffmpeg merge
    //   child, or ffmpeg spawned by a download) get reparented to init and keep running. The
    //   tracked children are each process-group leaders (pgid==pid), so kill(-pgid) takes down
    //   the whole subtree. Terminal is already restored above; the ~200ms SIGTERM→SIGKILL grace
    //   is invisible. After this, _exit(0) returns to the shell prompt immediately.
    Utils::kill_all_child_processes();
    // Exit IMMEDIATELY — skip ~App destructors (the pool workers are detached and could
    //   access App members during destruction → use-after-free). The terminal is already
    //   restored by frontend_->cleanup; the OS reclaims all resources (threads, mpv, DB handles).
    if (exit_hook_)
        exit_hook_(); // N09/S1: daemon cleanup (pid file) — _exit skips main's epilogue
    _exit(0);
}

} // namespace panicast
