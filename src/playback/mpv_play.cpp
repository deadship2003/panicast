#include "panicast/playback/mpv_controller.h"
#include <clocale> // setlocale
#include <chrono>  // steady_clock (bounded join in stop())
#include <cstdint> // int64_t (D48-prof: was relying on the transitive mpv/client.h include)
#include <cstring> // strlen
#include <thread>  // std::this_thread::sleep_for (bounded VO-teardown wait in stop())
#include <fcntl.h> // open, O_WRONLY (Y24.55: stderr redirect)
#include <fstream>
#include <sstream>
#include <string>   // std::to_string
#include <unistd.h> // dup2, close, STDERR_FILENO (Y24.55)
#include <fmt/format.h>
#include "panicast/config/ini_config.h"
#include "panicast/core/constants.h"
#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/core/safe_tmp.h"
#include "panicast/core/utils.h"
#include "panicast/net/url_classifier.h"
#include "panicast/net/ytdlp_runner.h"
#include "panicast/storage/accounts.h"
#include "panicast/parsers/youtube_channel_parser.h"

namespace panicast
{

// Freeze-fix review (2026-08-16): the "ensure playback starts (pause=no)" epilogue was
//   copy-pasted inside the play_audio / play_video / play_list_from cmd-worker lambdas (3
//   copies). One helper keeps them in lockstep and mirrors set_pause()'s optimistic state_
//   update so the last-known-good cache doesn't briefly disagree with what was just commanded.
//   CMD-WORKER ONLY (runs between loadfile and the next enqueued command, preserving FIFO order).
// D51: takes the caller's ctx snapshot — never reads the member (jam-recovery handoff).
void MPVController::ensure_playing_(mpv_handle *h) {
    { std::lock_guard<std::mutex> lock(mtx_); state_.paused = false; } // optimistic UI update
    int pause_val = 0;
    int rc_pause = mpv_set_property(h, "pause", MPV_FORMAT_FLAG, &pause_val);
    if (rc_pause < 0)
        LOG(fmt::format("[MPV] WARNING: set pause failed (rc={})", rc_pause));
    LOG("[MPV] Ensured playing (pause=no)");
}

void MPVController::play_audio(const std::string &url) {
    if (url.empty()) {
        LOG("[MPV] Error: Empty URL");
        return;
    }
    LOG(fmt::format("[MPV] Playing audio: {}", url));

    if (!ctx_) {
        LOG("[MPV] Error: ctx_ is null");
        return;
    }

    // Y24.12: audio fast-start profile — a smaller buffer so long podcast episodes start playing
    //   after ~5s (not 10s) and stream while buffering. Local files fill this near-instantly (disk
    //   throughput >> realtime), so cache=yes doesn't cause long BUFFERING for them — the >5s cases
    //   are mpv's open/probe/index per-op latency on slow mounts (WSL2 /mnt/e), diagnosed via the
    //   mpv log subscription + play_current timestamps added in Y24.17. Options are process-global
    //   last-set-wins, so play_video() restores the larger buffer.
    // Freeze-fix (2026-08-15): option strings are snapshotted here, ALL mpv calls run on the cmd
    //   worker — loadfile on a wedged core (dropped paused stream / reconnect black-hole) must
    //   never block the caller, which for play_current is the UI thread.
    auto &ini = IniConfig::instance();
    std::string cache_secs = std::to_string(ini.get_mpv_audio_cache_secs());
    std::string demux_max = ini.get_mpv_audio_demuxer_max_bytes();
    std::string demux_back = ini.get_mpv_audio_demuxer_max_back_bytes();
    std::string pause_wait = std::to_string(ini.get_mpv_audio_cache_pause_wait());
    LOG(fmt::format(
        "[MPV] Audio fast-start: cache-secs={}, demuxer-max-bytes={}, cache-pause-wait={}",
        cache_secs, demux_max, pause_wait));

    // F20: keep-open is NOT set here — play_current controls it (keep-open=no for CYCLE/SHUFFLE
    //   so EOF fires → auto-advance; keep-open=yes for REPEAT). Setting it here would override
    //   play_current's choice and prevent auto-advance (the PAUSE bug).

    // F23: no force-window setting — mpv default (no) handles it: no window for audio-only.
    // Don't change vo back to null: see vo_gpu_ note, avoids segfault on video->audio switch.

    {
        std::lock_guard<std::mutex> lock(cb_mtx_);
        last_load_url_ = url; // record for END_FILE -15 VO fallback
        vo_fallback_done_ = false;
        video_load_ = false; // D50: audio load — no window expected, skip the vo check
    }

    last_loadfile_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count(); // Y24.8: loadfile→File loaded timing
    enqueue_cmd_([this, url, cache_secs, demux_max, demux_back, pause_wait](mpv_handle *h) {
        mpv_set_option_string(h, "cache-secs", cache_secs.c_str());
        mpv_set_option_string(h, "demuxer-max-bytes", demux_max.c_str());
        mpv_set_option_string(h, "demuxer-max-back-bytes", demux_back.c_str());
        mpv_set_option_string(h, "cache-pause-wait", pause_wait.c_str());

        const char *cmd[] = {"loadfile", url.c_str(), "replace", nullptr};
        int result = mpv_command(h, cmd);
        LOG(fmt::format("[MPV] loadfile result: {}", result));

        ensure_playing_(h);
    }, "play_audio");
}

void MPVController::play_video(const std::string &url, const std::string &audio_file,
                               const std::string &sub_file) {
    if (url.empty()) {
        LOG("[MPV] Error: Empty URL");
        return;
    }
    LOG(fmt::format("[MPV] Playing video: {}", url));

    if (!ctx_) {
        LOG("[MPV] Error: ctx_ is null");
        return;
    }

    // F20: keep-open NOT set here — play_current controls it (same as play_audio).

    // F24 → D50: mpv activates the VO from the init config automatically — BUT the -15
    //   VO_INIT_FAILED fallback (handle_playback_error_) rewrites vo=null + vid=no +
    //   ytdl-format=bestaudio/best and nothing restored them, so ONE failure silently
    //   downgraded every later video of the session to audio ("vo=wlshm configured but
    //   playback shows vo=null"). The cmd worker now re-asserts the init values before
    //   each loadfile (idempotent no-op when nothing latched).
    LOG("[MPV] Video mode: loading (vo/vid re-asserted from init config)");

    // Y24.12: restore the larger [mpv] video cache (play_audio() shrinks it for podcast fast-start;
    //   options are process-global last-set-wins, so video must reset them to avoid rebuffers on
    //   YouTube/TikTok 1080p). Local video fills it fast (disk throughput); long BUFFERING on local
    //   video is mpv open/probe latency, not cache — diagnosed via Y24.17 mpv log + timestamps.
    // Freeze-fix (2026-08-15): option strings snapshotted here; ALL mpv calls run on the cmd
    //   worker (same rationale as play_audio).
    auto &ini = IniConfig::instance();
    std::string cache_secs = std::to_string(ini.get_mpv_cache_secs());
    std::string demux_max = ini.get_mpv_demuxer_max_bytes();
    std::string demux_back = ini.get_mpv_demuxer_max_back_bytes();
    LOG(fmt::format("[MPV] Video cache restored: cache-secs={}, demuxer-max-bytes={}",
                    cache_secs, demux_max));

    {
        std::lock_guard<std::mutex> lock(cb_mtx_);
        last_load_url_ = url; // record for END_FILE -15 VO fallback
        vo_fallback_done_ = false;
        video_load_ = true; // D50: a window is expected → arm the PLAYBACK_RESTART vo check
    }

    // Build the per-file options string: audio-file=<a>,sub-file=<s> (whichever are present).
    // Y09/Y10 (1A DASH) + Y11 (subtitles): attach external audio (DASH) and/or subtitle streams as
    //   per-file options via loadfile's <options> arg (comma-separated `key=value`). mpv has no
    //   `audio-file`/`sub-file` runtime PROPERTY (only the list forms) — the per-file OPTIONS string
    //   (loadfile 4th arg; 3rd arg = index "-1" since mpv 0.38) is the correct mechanism. vtt subs
    //   loaded this way respond to sub-scale/sub-pos/sub-delay and are centered by default.
    std::string opts;
    if (!audio_file.empty())
        opts += "audio-file=" + audio_file;
    if (!sub_file.empty())
        opts += (opts.empty() ? "" : ",") + std::string("sub-file=") + sub_file;

    // Y24.xx: Douyin CDN direct links (play_addr.url_list[0], from DouyinApi::fetchUserVideos)
    //   403 without a Referer. Inject it as a per-file option so mpv sends the header on the HTTP
    //   request. http-header-fields takes `field:value` pairs; a single Referer has no comma, so it
    //   can't clash with the option-list separator. Douyin-only (TikTok streams go through yt-dlp).
    if (url.find("douyinvod.com") != std::string::npos ||
        url.find("douyin.com") != std::string::npos ||
        url.find("aweme.snssdk.com") != std::string::npos) {
        opts += (opts.empty() ? "" : ",") +
                std::string("http-header-fields=Referer: https://www.douyin.com/");
    }

    last_loadfile_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count(); // Y24.8: loadfile→File loaded timing
    enqueue_cmd_([this, url, opts, cache_secs, demux_max, demux_back](mpv_handle *h) {
        // D50 (vo-fix): undo any -15 audio-only latch (vo=null/vid=no/bestaudio) from a
        //   previous track BEFORE this loadfile — see the F24→D50 note above. Idempotent
        //   no-ops when the options are already at their init values.
        if (!init_vo_.empty())
            mpv_set_property_string(h, "vo", init_vo_.c_str());
        if (!init_vid_.empty())
            mpv_set_property_string(h, "vid", init_vid_.c_str());
        if (!init_ytdl_format_.empty())
            mpv_set_property_string(h, "ytdl-format", init_ytdl_format_.c_str());

        mpv_set_option_string(h, "cache-secs", cache_secs.c_str());
        mpv_set_option_string(h, "demuxer-max-bytes", demux_max.c_str());
        mpv_set_option_string(h, "demuxer-max-back-bytes", demux_back.c_str());
        mpv_set_option_string(h, "cache-pause-wait", "10");

        int result;
        if (!opts.empty()) {
            const char *cmd[] = {"loadfile", url.c_str(), "replace", "-1", opts.c_str(), nullptr};
            result = mpv_command(h, cmd);
            LOG(fmt::format("[MPV] loadfile (+{}) result: {}", opts, result));
        } else {
            const char *cmd[] = {"loadfile", url.c_str(), "replace", nullptr};
            result = mpv_command(h, cmd);
            LOG(fmt::format("[MPV] loadfile result: {}", result));
        }

        ensure_playing_(h);
    }, "play_video");
}

void MPVController::play(const std::string &url, bool force_video, const std::string &audio_file,
                         const std::string &sub_file) {
    URLType type = URLClassifier::classify(url);
    logging_load_ = true; // Y24.17: start the load window — log mpv INFO events until FILE_LOADED
    reset_iptv_detection_(); // Y24.55: re-arm per-track IPTV diagnostics for the new channel

    // F23: no display detection — always route video to play_video. mpv's vo=auto handles
    //   VO selection (wlshm on WSL2, gpu on native Linux, null if nothing). If no display,
    //   mpv falls back to audio-only automatically.
    bool should_play_video =
        force_video || type == URLType::VIDEO_FILE || type == URLType::YOUTUBE_VIDEO;

    if (URLClassifier::is_youtube(type)) {
        should_play_video = true;
    }

    if (should_play_video) {
        play_video(url, audio_file, sub_file);
    } else {
        play_audio(url);
    }
}

void MPVController::play_list(const std::vector<std::string> &urls, bool is_video) {
    // D48-prof: was a ~50-line near-duplicate of play_list_from — the two differed ONLY by the
    //   start index (and one log line). Delegating keeps one implementation; behavior is
    //   identical (play_list_from clamps index 0 and start_idx=0 loadlist is the same command).
    play_list_from(urls, 0, is_video);
}

void MPVController::play_list_from(const std::vector<std::string> &urls, int start_idx,
                                   bool is_video) {
    if (urls.empty())
        return;
    if (!ctx_)
        return; // Guard against NULL handle segfault after initialize failure
    if (start_idx < 0)
        start_idx = 0;
    if (start_idx >= static_cast<int>(urls.size()))
        start_idx = static_cast<int>(urls.size()) - 1;
    logging_load_ = true; // Y24.17: start the load window (play() sets it too; loadlist path missed it)
    reset_iptv_detection_(); // Y24.55: re-arm per-track IPTV diagnostics
    video_load_ = is_video; // D50: arm the vo check only when a window is expected

    // Playlist mode settings
    // Freeze-fix (2026-08-15): all mpv interaction on the cmd worker (same rationale as
    //   play_audio) — keep-open/vo/loadlist/playlist-pos/pause enqueue in order. The temp m3u is
    //   written on the caller thread but removed INSIDE the worker, after loadlist has consumed it.
    LOG(fmt::format("[MPV] Playlist mode: keep-open=no, start from idx {}", start_idx));

    // Freeze-fix review (2026-08-16): vo=auto + the vo_gpu_ flag are decided INSIDE the cmd
    //   worker. Marking vo_gpu_ on the caller thread but issuing vo=auto in the worker split
    //   check/act/mark across threads — if the m3u write below failed, the fallback play() ran
    //   with vo_gpu_ already true but vo never set, and every later video playlist then skipped
    //   the vo switch (no video window). The worker is a single FIFO thread, so read+act+mark
    //   there is atomic. Don't change vo back to null otherwise (see vo_gpu_ note) — avoids
    //   segfault on video->audio switch.

    std::string tmp = SafeTmpFile::create(".m3u");
    std::ofstream f(tmp);
    if (f.is_open()) {
        for (const auto &url : urls)
            f << url << "\n";
        f.close();

        enqueue_cmd_([this, tmp, start_idx, is_video](mpv_handle *h) {
            int rc_keep_open = mpv_set_property_string(h, "keep-open", "no");
            if (rc_keep_open < 0)
                LOG(fmt::format("[MPV] WARNING: set property keep-open failed (rc={})", rc_keep_open));

            // D50 (vo-fix): honor the configured vo (CLI/INI via init_vo_) instead of hardcoded
            //   "auto" — auto probes gpu/x11 first and silently overrides an explicit software
            //   vo choice like wlshm. Also re-assert when the -15 fallback latched vo=null
            //   mid-playlist (its audio-only retry advances the list without play_video()).
            bool vo_null_latched = false;
            if (char *cur = mpv_get_property_string(h, "vo")) {
                vo_null_latched = (std::string(cur) == "null");
                mpv_free(cur);
            }
            if (is_video && (!vo_gpu_ || vo_null_latched)) {
                mpv_set_property_string(h, "vo",
                                        init_vo_.empty() ? "auto" : init_vo_.c_str());
                vo_gpu_ = true;
            }

            // Load the playlist
            const char *cmd[] = {"loadlist", tmp.c_str(), "replace", nullptr};
            int rc_loadlist = mpv_command(h, cmd); // P3-C5: check return (was ignored)
            if (rc_loadlist < 0)
                LOG(fmt::format("[MPV] WARNING: loadlist failed (rc={})", rc_loadlist));
            SafeTmpFile::remove(tmp); // Clean up temp file after loading

            // Set playback position to the specified start index
            int64_t pos = start_idx;
            if (rc_loadlist >= 0)
                mpv_set_property(h, "playlist-pos", MPV_FORMAT_INT64, &pos);
            LOG(fmt::format("[MPV] play_list_from: Set playlist-pos to {}", start_idx));

            ensure_playing_(h);
        }, "play_list_from");
    } else
        play(urls[static_cast<size_t>(start_idx)], is_video);
}

} // namespace panicast
