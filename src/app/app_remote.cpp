// Network control: state snapshot + command dispatch (N line, N02).
//
//   update_remote_state_cache() — runs on the UI thread once per frame; copies player + app state
//     into remote_state_cache_ under remote_state_mtx_. This is the thread-safe READ path for
//     remote query commands (status / currentsong / playlistinfo).
//
//   snapshot_state() — RemoteControlInterface; server threads call this to get a copy.
//
//   dispatch_remote() — runs on the UI thread (drained from the bus each frame); maps a remote
//     command to the EXISTING local control methods (player.* / nav_* / switch_mode / ...). This is
//     the 1:1 "remote terminal replicates the local keyboard" mapping. N04 covers playback / volume /
//     speed / seek / play-mode / sleep / mode-switch / navigation / mpv passthrough, plus (N04-fix)
//     search / mark / visual / favourite / edit / download / refresh / subtitle / ASR / playlist-clear.
//     add_node/delete remain TUI-only (context-dependent inline flows).
#include "panicast/app/app.h"

#include "panicast/core/constants.h"
#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/playback/sleep_timer.h"
#include "panicast/net/url_classifier.h" // D14-3b: is_local_file() for remote asr_start streaming/local

#include <mpv/client.h>

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace panicast
{

namespace
{

const char *mode_str(AppMode m) {
    switch (m) {
    case AppMode::RADIO:
        return "RADIO";
    case AppMode::PODCAST:
        return "PODCAST";
    case AppMode::FAVOURITE:
        return "FAVOURITE";
    case AppMode::HISTORY:
        return "HISTORY";
    case AppMode::ONLINE:
        return "ONLINE";
    case AppMode::ACCOUNT:
        return "ACCOUNT";
    case AppMode::BILIBILI:
        return "BILIBILI";
    case AppMode::TIKTOK:
        return "TIKTOK";
    case AppMode::IPTV:
        return "IPTV";
    }
    return "UNKNOWN";
}

bool parse_mode(const std::string &s, AppMode &out) {
    std::string u = s;
    std::transform(u.begin(), u.end(), u.begin(),
                   [](unsigned char c) { return (char)std::toupper(c); });
    if (u == "RADIO") {
        out = AppMode::RADIO;
        return true;
    }
    if (u == "PODCAST") {
        out = AppMode::PODCAST;
        return true;
    }
    if (u == "FAVOURITE") {
        out = AppMode::FAVOURITE;
        return true;
    }
    if (u == "HISTORY") {
        out = AppMode::HISTORY;
        return true;
    }
    if (u == "ONLINE") {
        out = AppMode::ONLINE;
        return true;
    }
    if (u == "ACCOUNT") {
        out = AppMode::ACCOUNT;
        return true;
    }
    if (u == "BILIBILI") {
        out = AppMode::BILIBILI;
        return true;
    }
    if (u == "TIKTOK") {
        out = AppMode::TIKTOK;
        return true;
    }
    if (u == "IPTV") {
        out = AppMode::IPTV;
        return true;
    }
    return false;
}

const char *play_mode_str(PlayMode m) {
    switch (m) {
    case PlayMode::REPEAT:
        return "repeat";
    case PlayMode::SHUFFLE:
        return "shuffle";
    case PlayMode::CYCLE:
        return "cycle";
    }
    return "cycle";
}

// Apply a play-mode change the same way the `:` command window does: set the global + persist.
void apply_play_mode(App &app, PlayMode m, PlayMode &cur) {
    cur = m;
    IniConfig::instance().set_play_mode(cur);
    EVENT_LOG(fmt::format("Remote: play_mode={}", play_mode_str(cur)));
    (void)app;
}

} // namespace

// ── State snapshot ───────────────────────────────────────────────────────────

void App::update_remote_state_cache() {
    MPVController::State ps = player.get_state();

    RemoteStateSnapshot s;
    s.paused = ps.paused;
    s.has_media = ps.has_media;
    s.volume = ps.volume;
    s.speed = ps.speed;
    s.elapsed = ps.time_pos;
    s.duration = ps.media_duration;
    // D14-2: now-playing identity + view from the canonical source (PlaybackService::now_playing(),
    //   built from the authoritative playback_node_) — canonical SOURCE url + node title, not mpv's
    //   played path (a local filesystem path for cached items) / media-title. Falls back to mpv
    //   state when no source node is set (e.g. a direct-URL play not routed through the service).
    if (Media np = playback_.now_playing(); np.id.valid()) {
        s.url = np.id.url();           // canonical source url (was ps.current_url = played path)
        s.title = std::move(np.title); // authoritative node title (was ps.title = mpv media-title)
        s.has_video = np.is_video;
        s.art_url = std::move(np.art_url);
    } else {
        s.title = ps.title;
        s.url = ps.current_url;
        s.has_video = ps.has_video;
    }
    s.icy_title = ps.icy_title; // META-1: radio now-playing "Artist - Title"
    // META-1: display metadata — structured fields first (RSS itunes:author / feed
    //   title / channel name), then the parent/channel heuristics, so the remote
    //   never has to show "unknown artist/album".
    {
        std::string artist, album;
        if (TreeNodePtr pn = playback_.playback_node()) {
            artist = pn->artist;
            album = pn->album;
            if (artist.empty()) {
                if (TreeNodePtr par = pn->parent.lock())
                    artist = par->title;
            }
            if (artist.empty())
                artist = pn->channel_name;
            if (album.empty()) {
                if (TreeNodePtr par = pn->parent.lock())
                    album = par->title;
            }
        }
        s.artist = artist.empty() ? "panicast" : artist;
        s.album = album.empty() ? "panicast · " + s.mode : album;
    }
    s.playlist_pos = ps.playlist_pos;
    s.playlist_count = ps.playlist_count;
    s.net_speed_bps = ps.net_speed_bps;
    s.buffering_pct = ps.buffering_pct;
    s.audio_codec = ps.audio_codec;

    s.mode = mode_str(mode);
    s.play_mode = play_mode_str(play_mode);
    s.selected_idx = library_.selected_idx();
    s.current_index = playback_.current_index();

    {
        std::lock_guard<std::mutex> lk(playback_.playlist_mutex());
        s.playlist.reserve(playback_.playlist().size());
        for (const auto &it : playback_.playlist()) {
            s.playlist.push_back({it.title, it.duration, it.is_video, it.artist, it.album});
        }
    }

    // Current-mode browse list (Squeeze Client's "panicast library"): the flat display
    //   list the TUI renders. REBUILT only when its signature changes (hashing titles
    //   every frame is cheap; re-materializing row strings is not — episode lists run
    //   long); the row copy into the snapshot is bounded by the cap below.
    {
        static std::string last_sig;
        static std::vector<RemoteBrowseItem> cached_rows;
        auto &dl = library_.display_list();
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const std::string &v) {
            for (unsigned char c : v) {
                h ^= c;
                h *= 1099511628211ull;
            }
        };
        mix(s.mode);
        for (const auto &d : dl) {
            h ^= (uint64_t)d.depth;
            h *= 1099511628211ull;
            mix(d.node ? d.node->title : std::string());
        }
        s.browse_sig = std::to_string(h);
        if (s.browse_sig != last_sig) {
            last_sig = s.browse_sig;
            cached_rows.clear();
            size_t n = 0;
            for (const auto &d : dl) {
                if (!d.node || n >= 400) // remote page cap; TUI keeps the full list
                    break;
                bool branch =
                    d.node->type == NodeType::FOLDER || d.node->type == NodeType::PODCAST_FEED;
                // ART-1: rows without their own artwork inherit the nearest ancestor's
                //   (episode → feed cover); DB-loaded and OPML trees carry parent links.
                std::string art = d.node->art_url;
                for (TreeNodePtr p = d.node->parent.lock(); art.empty() && p; p = p->parent.lock())
                    art = p->art_url;
                cached_rows.push_back({d.node->title, d.node->subtext, art, d.depth, branch});
                ++n;
            }
        }
        s.browse = cached_rows;
    }

    if (SleepTimer::instance().is_active()) {
        s.sleep_remaining = SleepTimer::instance().remaining_seconds();
    } else {
        s.sleep_remaining = -1;
    }
    s.subtitle_active = player.has_active_subtitle();

    {
        std::lock_guard<std::mutex> lk(remote_state_mtx_);
        remote_state_cache_ = std::move(s);
    }
}

RemoteStateSnapshot App::snapshot_state() {
    std::lock_guard<std::mutex> lk(remote_state_mtx_);
    return remote_state_cache_;
}

// ── Command dispatch (UI thread) ─────────────────────────────────────────────

void App::drain_remote_commands() {
    std::vector<RemoteCommand> cmds = remote_bus_.drain_all();
    for (const auto &c : cmds) {
        dispatch_remote(c);
    }
}

void App::dispatch_remote(const RemoteCommand &cmd) {
    const std::string &a = cmd.action;
    const auto &args = cmd.args;
    auto arg0 = [&]() -> std::string { return args.empty() ? std::string{} : args[0]; };

    // N04: internal — a remote client connected off-host; surface the pairing PIN in the LOG area.
    if (a == "_pin_log") {
        if (remote_server_.is_running()) {
            EVENT_LOG(fmt::format("Remote pairing request from {} — PIN {} (or {})",
                                  arg0().empty() ? std::string{"remote"} : arg0(),
                                  remote_server_.dynamic_pin(), remote_server_.universal_pin()));
        }
        return;
    }

    // ── Playback ──
    if (a == "play_pause") {
        player.toggle_pause();
        EVENT_LOG("Remote: play/pause");
        return;
    }
    if (a == "pause") {
        player.set_pause(true);
        EVENT_LOG("Remote: pause");
        return;
    }
    if (a == "resume") {
        player.set_pause(false);
        EVENT_LOG("Remote: resume");
        return;
    }
    if (a == "stop") {
        player.set_pause(true);
        EVENT_LOG("Remote: stop (pause)");
        return;
    }
    if (a == "play") {
        enter_node(count_marked_current());
        return;
    } // play selected (Enter)
    if (a == "next" || a == "previous") {
        int size = static_cast<int>(playback_.playlist().size());
        if (playback_.current_index() >= 0 && size > 0) {
            int idx = a == "next" ? (playback_.current_index() + 1) % size
                                  : (playback_.current_index() - 1 + size) % size;
            playback_.play_current(idx, mode, play_mode);
            EVENT_LOG(fmt::format("Remote: {}", a));
        } else {
            EVENT_LOG(fmt::format("Remote: {} — empty playlist", a));
        }
        return;
    }
    // ── Queue jumps / edits (Squeezer playlist view: tap row, swipe-remove, drag-reorder) ──
    if (a == "jump") { // play queue entry <idx> ("playlist index <N>" on the LMS plane)
        int idx = std::atoi(arg0().c_str());
        int size = static_cast<int>(playback_.playlist().size());
        if (idx >= 0 && idx < size) {
            playback_.play_current(idx, mode, play_mode);
            EVENT_LOG(fmt::format("Remote: jump {}", idx));
        } else {
            EVENT_LOG(fmt::format("Remote: jump {} — out of range (queue {})", idx, size));
        }
        return;
    }
    if (a == "playlist_remove") { // "playlist delete <N>"
        int idx = std::atoi(arg0().c_str());
        std::lock_guard<std::mutex> lk(playback_.playlist_mutex());
        auto &pl = playback_.playlist();
        if (idx >= 0 && idx < static_cast<int>(pl.size())) {
            pl.erase(pl.begin() + idx);
            int cur = playback_.current_index();
            if (cur > idx)
                playback_.set_current_index(cur - 1);
            else if (cur >= static_cast<int>(pl.size()))
                playback_.set_current_index(static_cast<int>(pl.size()) - 1);
            // Indices shifted → regenerate the SHUFFLE lookahead under the held lock.
            playback_.shuffle_queue().clear();
            playback_.refill_shuffle_queue();
            EVENT_LOG(fmt::format("Remote: playlist remove {} ({} left)", idx, pl.size()));
        }
        return;
    }
    if (a == "playlist_move" && args.size() >= 2) { // "playlist move <from> <to>"
        int from = std::atoi(args[0].c_str());
        int to = std::atoi(args[1].c_str());
        std::lock_guard<std::mutex> lk(playback_.playlist_mutex());
        auto &pl = playback_.playlist();
        if (from >= 0 && from < static_cast<int>(pl.size()) && to >= 0 &&
            to < static_cast<int>(pl.size()) && from != to) {
            PlaylistItem it = pl[from];
            pl.erase(pl.begin() + from);
            pl.insert(pl.begin() + to, it);
            // Keep the playing pointer on the same ENTRY as it slides.
            int cur = playback_.current_index();
            auto reindex = [&](int i) {
                if (i == from)
                    return to;
                if (from < to && i > from && i <= to)
                    return i - 1;
                if (from > to && i >= to && i < from)
                    return i + 1;
                return i;
            };
            playback_.set_current_index(reindex(cur));
            playback_.shuffle_queue().clear();
            playback_.refill_shuffle_queue();
            EVENT_LOG(fmt::format("Remote: playlist move {} -> {}", from, to));
        }
        return;
    }
    // ── Seek (forwarded to mpv) ──
    if (a == "seek" || a == "seekto" || a == "seek_percent") {
        if (args.empty()) {
            EVENT_LOG("Remote: seek needs <seconds>");
            return;
        }
        std::string mcmd;
        if (a == "seek")
            mcmd = fmt::format("seek {}", args[0]);
        else if (a == "seekto")
            mcmd = fmt::format("seek {} absolute", args[0]);
        else
            mcmd = fmt::format("seek {} absolute-percent", args[0]);
        if (mpv_handle *h = player.get_handle())
            mpv_command_string(h, mcmd.c_str());
        EVENT_LOG(fmt::format("Remote: {}", mcmd));
        return;
    }
    // ── Volume ──
    if (a == "volume") {
        if (args.empty()) {
            EVENT_LOG("Remote: volume needs <0-100>");
            return;
        }
        player.set_volume(std::atoi(args[0].c_str()));
        EVENT_LOG(fmt::format("Remote: volume={}", args[0]));
        return;
    }
    if (a == "volume_rel") { // Squeezer's hardware-volume keys ("mixer volume +5/-5")
        if (args.empty()) {
            EVENT_LOG("Remote: volume_rel needs <+/-delta>");
            return;
        }
        player.set_volume(player.get_state().volume + std::atoi(args[0].c_str()));
        EVENT_LOG(fmt::format("Remote: volume {}", args[0]));
        return;
    }
    if (a == "volume_up") {
        player.set_volume(player.get_state().volume + VOLUME_STEP);
        return;
    }
    if (a == "volume_down") {
        player.set_volume(player.get_state().volume - VOLUME_STEP);
        return;
    }
    // ── Speed ──
    if (a == "speed") {
        if (args.empty()) {
            EVENT_LOG("Remote: speed needs <0.25-4>");
            return;
        }
        player.set_speed(std::atof(args[0].c_str()));
        return;
    }
    if (a == "speed_up") {
        player.adjust_speed(true);
        return;
    }
    if (a == "speed_down") {
        player.adjust_speed(false);
        return;
    }
    if (a == "speed_reset") {
        player.reset_speed();
        return;
    }
    // ── Play mode (repeat/shuffle/cycle) ──
    if (a == "repeat") {
        apply_play_mode(*this, PlayMode::REPEAT, play_mode);
        return;
    }
    if (a == "shuffle") {
        apply_play_mode(*this, PlayMode::SHUFFLE, play_mode);
        return;
    }
    if (a == "cycle") {
        apply_play_mode(*this, PlayMode::CYCLE, play_mode);
        return;
    }
    if (a == "set_mode") {
        std::string v = arg0();
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (v.rfind("rep", 0) == 0) {
            apply_play_mode(*this, PlayMode::REPEAT, play_mode);
            return;
        }
        if (v.rfind("shu", 0) == 0) {
            apply_play_mode(*this, PlayMode::SHUFFLE, play_mode);
            return;
        }
        if (v.rfind("cyc", 0) == 0) {
            apply_play_mode(*this, PlayMode::CYCLE, play_mode);
            return;
        }
        EVENT_LOG(fmt::format("Remote: unknown play_mode '{}'", v));
        return;
    }
    // ── Sleep timer ──
    if (a == "sleep") {
        if (args.empty()) {
            EVENT_LOG("Remote: sleep needs <duration>");
            return;
        }
        SleepTimer::instance().set_duration(args[0]); // accepts "5h"/"30m"/"90"
        EVENT_LOG(fmt::format("Remote: sleep {}", args[0]));
        return;
    }
    if (a == "sleep_cancel") {
        SleepTimer::instance().cancel();
        EVENT_LOG("Remote: sleep cancel");
        return;
    }
    // ── AppMode ──
    if (a == "mode") {
        AppMode m;
        if (parse_mode(arg0(), m)) {
            switch_mode(m);
            EVENT_LOG(fmt::format("Remote: mode {}", arg0()));
        } else {
            EVENT_LOG(fmt::format("Remote: unknown mode '{}'", arg0()));
        }
        return;
    }
    if (a == "mode_next" || a == "mode_prev") {
        static const AppMode order[] = {AppMode::RADIO,    AppMode::PODCAST, AppMode::FAVOURITE,
                                        AppMode::HISTORY,  AppMode::ONLINE,  AppMode::ACCOUNT,
                                        AppMode::BILIBILI, AppMode::TIKTOK,  AppMode::IPTV};
        int idx = 0;
        for (int i = 0; i < 9; ++i)
            if (order[i] == mode) {
                idx = i;
                break;
            }
        idx = a == "mode_next" ? (idx + 1) % 9 : (idx - 1 + 9) % 9;
        switch_mode(order[idx]);
        return;
    }
    // ── Navigation ──
    if (a == "nav_up") {
        nav_up();
        return;
    }
    if (a == "nav_down") {
        nav_down();
        return;
    }
    if (a == "nav_top") {
        nav_top();
        return;
    }
    if (a == "nav_bottom") {
        nav_bottom();
        return;
    }
    if (a == "nav_page_up") {
        nav_page_up();
        return;
    }
    if (a == "nav_page_down") {
        nav_page_down();
        return;
    }
    if (a == "nav_back") {
        go_back();
        return;
    }
    if (a == "nav_enter") {
        enter_node(count_marked_current());
        return;
    }
    if (a == "nav_select") {
        if (!args.empty()) {
            int idx = std::atoi(args[0].c_str());
            int n = static_cast<int>(library_.display_list().size());
            if (n > 0) {
                library_.selected_idx() = std::clamp(idx, 0, n - 1);
            }
        }
        return;
    }
    // Remote browse (Squeeze Client's library view): select row <idx> + press Enter in
    //   one atomic dispatch — branch rows descend the tree, leaf rows play. The bus
    //   preserves order, so pushing nav_select + nav_enter separately would also work,
    //   but one action keeps the pair from straddling a frame boundary.
    if (a == "nav_activate") {
        if (!args.empty()) {
            int idx = std::atoi(args[0].c_str());
            int n = static_cast<int>(library_.display_list().size());
            if (n > 0) {
                library_.selected_idx() = std::clamp(idx, 0, n - 1);
                enter_node(count_marked_current());
                EVENT_LOG(fmt::format("Remote: browse activate row {}", idx));
            }
        }
        return;
    }
    if (a == "sort_toggle") {
        toggle_sort_order();
        return;
    }
    // ── mpv native passthrough (the `:` command window) ──
    if (a == "mpv") {
        if (args.empty()) {
            EVENT_LOG("Remote: mpv needs <command>");
            return;
        }
        // Rejoin args into one mpv command string.
        std::string mcmd = args[0];
        for (size_t i = 1; i < args.size(); ++i) {
            mcmd += " ";
            mcmd += args[i];
        }
        if (mpv_handle *h = player.get_handle()) {
            mpv_command_string(h, mcmd.c_str());
            EVENT_LOG(fmt::format("Remote: mpv → {}", mcmd));
        } else {
            EVENT_LOG("Remote: mpv not available");
        }
        return;
    }

    // ── Search (mode-appropriate; opens the input box on the host, replicating '/') ──
    if (a == "search") {
        if (mode == AppMode::ONLINE)
            perform_online_search();
        else if (mode == AppMode::BILIBILI)
            perform_bilibili_search(arg0());
        else if (mode == AppMode::ACCOUNT)
            perform_youtube_search(arg0());
        else
            perform_search();
        EVENT_LOG("Remote: search");
        return;
    }
    if (a == "search_next") {
        jump_search(1);
        return;
    }
    if (a == "search_prev") {
        jump_search(-1);
        return;
    }
    // ── Mark / Visual ──
    if (a == "mark_toggle") {
        toggle_mark();
        return;
    }
    if (a == "visual_on") {
        visual_mode_ = true;
        visual_start_ = library_.selected_idx();
        return;
    }
    if (a == "visual_off") {
        visual_mode_ = false;
        return;
    }
    if (a == "mark_clear") {
        clear_all_marks();
        return;
    }
    // ── Favourite / Edit / Download / Refresh ──
    if (a == "favorite_toggle" || a == "favourite_toggle") {
        add_favourite();
        return;
    }
    if (a == "edit_node") {
        edit_node();
        return;
    }
    if (a == "download" || a == "download_marked") {
        download_node(count_marked_current());
        return;
    }
    if (a == "refresh") {
        reset_search();
        refresh_node();
        return;
    }
    // ── Playlist ──
    if (a == "queue_clear" || a == "clear_playlist") {
        playback_.clear_playlist();
        return;
    }
    // ── Subtitle / ASR ──
    if (a == "subtitle_toggle") {
        frontend_->toggle_lyric_bar();
        EVENT_LOG("Remote: subtitle toggle");
        return;
    }
    // N04-fix: subtitle_offset removed (z/Z direct keys + INI offset removed; use :z/:Z mpv sub-delay).
    if (a == "asr_start") {
        auto pst = player.get_state();
        auto pn = playback_.playback_node();
        if (pst.has_media && pn && !subtitle_.transcription_engine().realtime_running()) {
            // D11-3a: respect "本地字幕文件优先". If a cheaper source exists, load it instead of
            //   burning ASR — this was the missing-local-check gap (remote always force-ASR'd).
            //   :asr remains the only force-bypass path.
            auto src = subtitle_.resolve_subtitle_source(pn);
            if (src.kind == ResolvedSubtitle::Embedded) {
                EVENT_LOG("Remote: embedded subtitle already active — no ASR needed");
            } else if (src.kind != ResolvedSubtitle::None) {
                // LocalSrt or Online — load via the standard track orchestration.
                subtitle_.begin_track(pn, pst.has_video);
                EVENT_LOG("Remote: local/online subtitle found — loading instead of ASR");
            } else {
                const std::string &url = pst.current_url;
                bool is_streaming = !URLClassifier::is_local_file(url);
                subtitle_.transcription_engine().start_realtime(pn, url, is_streaming);
                EVENT_LOG("Remote: ASR start");
            }
        } else {
            EVENT_LOG("Remote: ASR start — nothing playing or already running");
        }
        return;
    }
    if (a == "asr_stop") {
        subtitle_.transcription_engine().stop_realtime();
        EVENT_LOG("Remote: ASR stop");
        return;
    }

    // ── Not yet mapped: add_node/delete are context-dependent inline flows (TUI-only) ──
    LOG(fmt::format("[REMOTE] unmapped action='{}'", a));
    EVENT_LOG(fmt::format("Remote: '{}' not mapped (add/delete are TUI-only context flows)", a));
}

} // namespace panicast
