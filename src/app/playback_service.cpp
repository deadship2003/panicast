#include "panicast/app/playback_service.h"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <random>
#include <unordered_map> // stream-fix: resolve_cache
#include <sstream>

#include <fmt/format.h>

#include "panicast/app/actions.h"
#include "panicast/app/playback_events.h"
#include "panicast/config/ini_config.h"
#include "panicast/core/constants.h"
#include "panicast/core/event_bus.h"
#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/core/paths.h"
#include "panicast/core/thread_pool.h"
#include "panicast/net/url_classifier.h"
#include "panicast/net/network.h" // stream-fix: resolve_redirects (play-path CDN URL)
#include "panicast/net/ytdlp_runner.h"
#include "panicast/parsers/youtube_channel_parser.h"
#include "panicast/storage/cache.h"
#include "panicast/storage/database.h"
#include "panicast/subtitle/subtitle_manager.h"
#include "panicast/subtitle/transcription_engine.h"

namespace panicast
{

namespace
{
// stream-fix (2026-08-16): per-session map of episode source URL → resolved CDN URL. Every hit is
//   re-validated with a 1-byte GET before use (signed CDN URLs expire; a stale entry re-resolves
//   the chain), so the TTL only bounds memory, not correctness. One entry per distinct episode
//   played this session — trivially small.
std::mutex resolve_cache_mtx;
std::unordered_map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>>
    resolve_cache;
constexpr int RESOLVE_CACHE_TTL_MIN = 120;
} // namespace

// ── Action handling (D8a) ─────────────────────────────────────────────────────
void PlaybackService::init() {
    subs_.push_back(EventBus::instance().subscribe<PlayPauseAction>(
        [this](const PlayPauseAction &) { on_play_pause(); }));
    subs_.push_back(EventBus::instance().subscribe<VolumeUpAction>(
        [this](const VolumeUpAction &) { on_volume_up(); }));
    subs_.push_back(EventBus::instance().subscribe<VolumeDownAction>(
        [this](const VolumeDownAction &) { on_volume_down(); }));
}

void PlaybackService::on_play_pause() {
    player_.toggle_pause();
}

void PlaybackService::on_volume_up() {
    player_.set_volume(player_.get_state().volume + VOLUME_STEP);
}

void PlaybackService::on_volume_down() {
    player_.set_volume(player_.get_state().volume - VOLUME_STEP);
}

void PlaybackService::shutdown() {
    for (std::size_t t : subs_)
        EventBus::instance().unsubscribe(t);
    subs_.clear();
}

// ── Queue logic (D8b-1, moved from app_playback.cpp) ──────────────────────────

// N04-fix: clear the implicit peer playlist while keeping the current track playing.
//   The mpv handle is untouched (the current track keeps playing); on_playback_ended()
//   sees an empty current_playlist and returns without advancing, so auto-advance stops.
void PlaybackService::clear_playlist() {
    std::lock_guard<std::mutex> pl_lock(playlist_mutex_);
    current_playlist_.clear();
    shuffle_queue_.clear();
    current_index_ = -1;
    EVENT_LOG("Playlist cleared (keep playing)");
}

// Keep shuffle_queue_ at 3 entries (pre-generated upcoming random indices).
// Called under playlist_mutex_ (NOT self-locking — original contract preserved).
void PlaybackService::refill_shuffle_queue() {
    while (shuffle_queue_.size() < 3) {
        int last = shuffle_queue_.empty() ? current_index_ : shuffle_queue_.back();
        int idx = random_peer_index(last);
        if (idx < 0)
            break;
        shuffle_queue_.push_back(idx);
    }
}

// Pick one random index in [0, size) avoiding `avoid` when possible.
int PlaybackService::random_peer_index(int avoid) const {
    int size = static_cast<int>(current_playlist_.size());
    if (size <= 0)
        return -1;
    if (size == 1)
        return 0;
    static thread_local std::mt19937 gen(std::random_device{}());

    std::uniform_int_distribution<int> dist(0, size - 1);
    int idx = dist(gen);
    if (idx == avoid)
        idx = (idx + 1) % size;
    return idx;
}

// ── Playback / autoplay logic (D8b-2, moved from app_playback.cpp) ────────────
// D10-3 Step 1: the is_mpv_sub_url / basename_of subtitle helpers that lived in this file's
//   anonymous namespace moved to subtitle_service.cpp (they serve begin_track's subtitle block,
//   which relocated to SubtitleService).

// D8b-2 late injection — wire the ThreadPool declared after playback_ in App (it can't be a
//   construction-time ref). State changes go on the EventBus (D9), so no callbacks here.
//   Called once from App::run right after playback_.init().
//   D11-1: the SubtitleService param is gone — subtitle is fully event-driven (SubtitleService
//   subscribes PlaybackTrackChanged + PlaybackTrackEnded), so PlaybackService references
//   SubtitleService nowhere now.
void PlaybackService::attach(ThreadPool &pool) {
    pool_ = &pool;
}

// D9-3: single funnel for a buffering-state change — write playback_pending_(_start_) AND publish
//   PlaybackBufferingChanged (the reactor channel for future remote/UI subscribers). Replaces the
//   D9-1 publish-only sites in play_current / on_playback_ended: the state is now service-owned, so
//   the write lives here (not in an App subscription).
void PlaybackService::set_buffering_(bool pending) {
    playback_pending_ = pending;
    if (pending)
        playback_pending_start_ = std::chrono::steady_clock::now();
    EventBus::instance().publish(PlaybackBufferingChanged{pending});
}

// D9-3: per-frame buffering-lifecycle tick (moved verbatim from app_run's app_state state machine,
//   which used to read/write App's playback_pending_(_start_) directly). Called once per frame from
//   App's run loop with mpv's has_media, on the UI thread (D4 invariant unaffected — same thread the
//   inline logic ran on). Returns true while a just-started track is still pending mpv load (<30s);
//   false once loaded (App derives PLAYING/PAUSED from mpv) or idle/timed-out (App shows BROWSING).
//   Logs the buffering duration on the pending→loaded transition (Y24.17, once) and on 30s timeout.
bool PlaybackService::advance_buffering(bool mpv_has_media) {
    if (mpv_has_media) {
        if (playback_pending_) {
            auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - playback_pending_start_)
                                .count();
            LOG(fmt::format("[PLAY] BUFFERING cleared: total {}ms (sync steps above + mpv load)",
                            total_ms));
            if (total_ms > 2000)
                EVENT_LOG(fmt::format("BUFFERING {}ms (see panicast.log for breakdown)", total_ms));
            set_buffering_(false); // Y23.9: mpv has media → clear pending
        }
        return false; // media loaded → not pending (App derives PLAYING/PAUSED from mpv)
    }
    if (!playback_pending_)
        return false; // idle → not pending
    // Y23.9: playback initiated but mpv hasn't loaded yet → BUFFERING. Timeout 30s → clear + give up.
    if (std::chrono::steady_clock::now() - playback_pending_start_ > std::chrono::seconds(30)) {
        set_buffering_(false);
        EVENT_LOG("Playback pending timeout (30s) — stream may have failed");
        return false; // timed out → App shows BROWSING
    }
    return true; // still pending/buffering
}

// Playback-ended handler. Runs ONLY on the UI thread (D4 invariant): the mpv event thread merely
//   queues the END_FILE reason in App, which drains it here each frame (running it on the mpv
//   thread under playlist_mutex_ contended the UI draw lock and froze the TUI after pause).
//   Pointer-driven model: a single track is loaded into mpv at a time; when it ends normally
//   (reason=0), the service advances current_index_ per play_mode and plays the next track. REPEAT
//   relies on mpv loop_file (no action here). SHUFFLE consumes the pre-generated shuffle_queue_
//   front and refills it.
void PlaybackService::on_playback_ended(int reason, AppMode mode, PlayMode play_mode) {
    // D11-1: track ended → SubtitleService stop_realtime (was a direct call — playback no longer
    //   touches subtitles). review-fix (2026-08-16): the supersede reason is 2 (STOP — "stopped
    //   by an external action", i.e. our loadfile/loadlist replace; there is no other in-session
    //   "stop" producer — remote stop is pause, exit is covered by shutdown), and play_current
    //   already published Ended on supersede. Republishing there would race and kill the newborn
    //   track's auto-ASR (D49): the pool task's start_realtime can beat the END_FILE drain by a
    //   frame. reason=5 (REDIRECT — the file was a playlist; playback continues with its entries)
    //   is a continuation, not an end. Skip both; every other reason is a real end.
    if (reason != 2 && reason != 5)
        EventBus::instance().publish(PlaybackTrackEnded{});
    std::lock_guard<std::mutex> pl_lock(playlist_mutex_);
    if (reason != 0) {
        set_buffering_(false); // Y23.9: error/stop → clear pending (back to BROWSING)
        // Y24.8: human-readable reason (was raw "reason=X, ignored"). reason 0=EOF (advances),
        //   2=stop, 3=quit, 4=error, 5=redirect — none advance the queue.
        LOG(fmt::format("[AUTOPLAY] End file: {} — not advancing",
                        MPVController::end_file_reason_str(reason)));
        return;
    }

    int size = static_cast<int>(current_playlist_.size());
    if (size == 0) {
        LOG("[AUTOPLAY] Peer list empty, nothing to advance");
        return;
    }

    // REPEAT: mpv loop_file replays the same track; no intervention.
    if (play_mode == PlayMode::REPEAT) {
        LOG("[AUTOPLAY] REPEAT: loop_file replays current track");
        return;
    }

    // Compute the next pointer
    int next = current_index_;
    if (play_mode == PlayMode::CYCLE) {
        next = (current_index_ < 0) ? 0 : (current_index_ + 1) % size;
    } else { // PlayMode::SHUFFLE — consume the lookahead queue
        if (shuffle_queue_.empty())
            refill_shuffle_queue();
        if (!shuffle_queue_.empty()) {
            next = shuffle_queue_.front();
            shuffle_queue_.pop_front();
        }
        // ensure valid + not stuck on the same item when there's a choice
        if (size > 1 && next == current_index_) {
            next = (next + 1) % size;
        }
        // keep 3 ahead
        refill_shuffle_queue();
    }

    current_index_ = next;

    // Y11 bugfix: update playback_node to the NEXT track's source node, else the INFO area keeps
    //   showing the PREVIOUS track's title/info after auto-advance (on_playback_ended plays inline
    //   without play_current, the only other place playback_node is set). Set on this (UI) thread
    //   the same way current_index_ is; the TreeNode is retained by the tree, so the shared_ptr
    //   reassignment is safe in practice (matches existing pattern).
    TreeNodePtr next_node = current_playlist_[next].node;
    playback_node_ = next_node; // D9-2: authoritative track state (queried via playback_node())
    playback_mode_ = mode;
    bool has_video =
        current_playlist_[next].is_video; // snapshot under the lock (event + play below)
    now_playing_is_video_ = has_video;    // D14-2: per-track is_video for now_playing()
    // D9 + D10-3 Step 2: publish the track change WITH its re-identified has_video flag. SubtitleService
    //   subscribes → begin_track(next_node, has_video): auto-advance now runs the SAME full A/B branch
    //   as a manual play (Option B — was a Method-B-only load_transcript). Dispatch is synchronous on
    //   this (UI) thread, so subtitle setup starts before play() below, exactly as the old call did.
    EventBus::instance().publish(PlaybackTrackChanged{next_node, mode, has_video});
    // Y24.55: keep IPTV context flag in sync on auto-advance (same reasoning as play_current).
    player_.set_iptv_context(mode == AppMode::IPTV);
    set_buffering_(true); // Y23.9: BUFFERING until mpv loads the next track

    // Play the next track inline (must NOT call play_current — it also locks playlist_mutex_
    //   (non-recursive) and would deadlock).
    std::string orig_url = current_playlist_[next].url;

    // F23: async YouTube resolve — don't block the UI thread
    std::string local = CacheManager::instance().get_local_file(orig_url);
    if (!local.empty() && fs::exists(local)) {
        // Local file — play immediately
        std::string url = local; // Y23.9: raw path (no file://) — mpv handles special chars
        player_.play(url, has_video);
        player_.set_keep_open(false);
        player_.set_pause(false);
        player_.set_loop_file(false);
    } else {
        URLType ut = URLClassifier::classify(orig_url);
        // P1-4: snapshot title/duration here (under playlist_mutex_) — the pool lambda must not
        //   read current_playlist_[next] later, by which time the UI thread may have reset it.
        std::string title = current_playlist_[next].title;
        int duration = current_playlist_[next].duration;
        if (URLClassifier::is_youtube(ut)) {
            EVENT_LOG("Resolving YouTube stream via yt-dlp -g (async)...");
            pool_->submit([this, orig_url, has_video, next, title, duration]() {
                auto yt_urls = resolve_youtube_url(orig_url, has_video);
                if (yt_urls.empty()) {
                    EVENT_LOG("YouTube resolve failed — skipping playback");
                    return;
                }
                std::string audio = (yt_urls.size() >= 2) ? yt_urls[1] : std::string();
                std::string sub = (yt_urls.size() >= 3) ? yt_urls[2] : std::string();
                player_.play(yt_urls[0], has_video, audio, sub);
                player_.set_keep_open(false);
                player_.set_pause(false);
                player_.set_loop_file(false);
                record_play_history(orig_url, title, duration);
                EVENT_LOG(fmt::format("[AUTOPLAY] -> idx {} '{}'", next, title));
            });
            return; // don't proceed synchronously
        } else if (ut == URLType::BILIBILI_VIDEO || ut == URLType::DOUYIN_VIDEO ||
                   ut == URLType::TIKTOK_VIDEO) {
            // Y21 (issue 3): play the watch URL via mpv ytdl_hook (Referer from yt-dlp extractor).
            player_.play(orig_url, true);
            player_.set_keep_open(false);
            player_.set_pause(false);
            player_.set_loop_file(false);
            record_play_history(orig_url, title, duration);
            EVENT_LOG(fmt::format("[AUTOPLAY] -> idx {} '{}'", next, title));
            return;
        } else {
            // Non-YouTube — play directly
            player_.play(orig_url, has_video);
            player_.set_keep_open(false);
            player_.set_pause(false);
            player_.set_loop_file(false);
        }
    }

    record_play_history(orig_url, current_playlist_[next].title, current_playlist_[next].duration);

    EVENT_LOG(fmt::format("[AUTOPLAY] {} -> idx {} '{}'",
                          play_mode == PlayMode::SHUFFLE ? "SHUFFLE" : "CYCLE", next,
                          current_playlist_[next].title));
}

// F23: Extract YouTube URL resolution into a standalone method (callable from pool threads).
//   Y09 (1A DASH): picks the yt-dlp `-f` format from [youtube] play_format_video/play_format_audio
//   by has_video, and returns ALL stream URLs yt-dlp prints (1 for muxed/HLS/audio-only,
//   2 for DASH bestvideo+bestaudio). The caller feeds video URL + audio-file to mpv so it can
//   merge DASH streams → 1080p. Empty vector = resolve failed (caller skips, no fallback).
//   Y24.47: in audio-only mode (--quiet / vo=null / vid=no) pick the AUDIO format even for video
//   items, so yt-dlp returns an audio-only stream URL → the video stream is never downloaded

// stream-fix (2026-08-16): episode source URL → final CDN URL, session-cached + revalidated.
//   Why: tracker chains (pdst.fm → pscrb.fm → … → CDN, 3-6 hops) cost ~1s/hop through the
//   transparent proxy, and mpv's ffmpeg re-walks the ENTIRE chain on every open/probe/duration-
//   seek — measured 9-77s just to reach FILE_LOADED (curl one-shot: 6.4s for the whole chain,
//   0.8s TTFB on the final URL). Resolving once here and handing mpv the direct URL restores
//   "buffer a little, play while buffering". Never blocks the UI thread (pool task) and never
//   blocks playback on failure — "" / validation miss falls back to the source URL, which is
//   exactly the old mpv-native path. Also caches the no-redirect case so replays skip the probe.
std::string PlaybackService::resolve_stream_url_(const std::string &orig_url) {
    auto host_of = [](const std::string &u) {
        size_t s = u.find("://");
        size_t b = (s == std::string::npos) ? 0 : s + 3;
        size_t e = u.find('/', b);
        return u.substr(b, (e == std::string::npos) ? std::string::npos : e - b);
    };
    auto t0 = std::chrono::steady_clock::now();
    auto ms = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
            .count();
    };

    // 1) Fresh cache hit → validate it's still servable (signed CDN URLs expire); a redirect on
    //    validation is followed to the new final (chains can grow shorter/different over time).
    {
        std::string cached;
        {
            std::lock_guard<std::mutex> lock(resolve_cache_mtx);
            auto it = resolve_cache.find(orig_url);
            if (it != resolve_cache.end() && std::chrono::steady_clock::now() - it->second.second <
                                                 std::chrono::minutes(RESOLVE_CACHE_TTL_MIN))
                cached = it->second.first;
        }
        if (!cached.empty()) { // no lock held across the network probe
            std::string again = Network::resolve_redirects(cached);
            if (!again.empty()) {
                if (again != cached) { // moved again — keep the cache current
                    std::lock_guard<std::mutex> lock(resolve_cache_mtx);
                    resolve_cache[orig_url] = {again, std::chrono::steady_clock::now()};
                }
                LOG(fmt::format("[PLAY] stream URL cache hit ({}ms): {} -> {}", ms(),
                                host_of(orig_url), host_of(again)));
                return again;
            }
            std::lock_guard<std::mutex> lock(resolve_cache_mtx);
            resolve_cache.erase(orig_url); // stale (403/404/expired) — re-resolve below
            LOG(fmt::format("[PLAY] cached stream URL stale ({}ms) — re-resolving", ms()));
        }
    }

    // 2) Full chain resolve. Failure ("") or a no-redirect answer both return the source URL —
    //    mpv then does its own (old) thing; the cache entry saves the probe on the next replay.
    //    Timeout 25s: the 6-hop pdst.fm chain measures 6-11s through the transparent proxy
    //    (varies by load) and must stay under the 30s BUFFERING timeout so the fallback play
    //    still starts within the pending window. Validation above (1 direct hop) keeps 15s.
    std::string final_url = Network::resolve_redirects(orig_url, /*timeout=*/25);
    if (final_url.empty()) {
        LOG(fmt::format("[PLAY] redirect resolve failed ({}ms) — playing source URL", ms()));
        return orig_url;
    }
    {
        std::lock_guard<std::mutex> lock(resolve_cache_mtx);
        resolve_cache[orig_url] = {final_url, std::chrono::steady_clock::now()};
    }
    if (final_url == orig_url)
        LOG(fmt::format("[PLAY] no redirect chain ({}ms): {}", ms(), host_of(orig_url)));
    else
        LOG(fmt::format("[PLAY] redirect chain resolved ({}ms): {} -> {}", ms(), host_of(orig_url),
                        host_of(final_url)));
    return final_url;
}

//   items, so yt-dlp returns an audio-only stream URL → the video stream is never downloaded
//   (saves bandwidth). Played via play_video with vo=null (no window) so sub_file (lyrics) still
//   loads. For direct muxed URLs (non-YouTube) this can't apply — see mpv/container limitation.
std::vector<std::string> PlaybackService::resolve_youtube_url(const std::string &url,
                                                              bool has_video) const {
    std::vector<std::string> base_args = YouTubeChannelParser::ytdlp_youtube_args();
    bool audio_only = MPVController::is_audio_only_mode();
    bool want_video = has_video && !audio_only;
    std::string fmt = want_video ? IniConfig::instance().get_youtube_play_format_video()
                                 : IniConfig::instance().get_youtube_play_format_audio();

    // YT-fix: retry loop with a generous, configurable timeout. yt-dlp YouTube resolution is flaky
    //   — through a SOCKS proxy + quickjs/ejs nsig solving a single `-g` can take 30-60s, and the
    //   old fixed 30s cap caused intermittent "YouTube resolve failed" (exact-30s YtdlpRunner
    //   timeouts in the log). Re-resolving is cheap and usually succeeds on retry.
    int timeout_sec = IniConfig::instance().get_youtube_resolve_timeout_sec();
    int attempts = IniConfig::instance().get_youtube_resolve_retries();
    if (attempts < 1)
        attempts = 1;
    std::vector<std::string> urls;
    YtdlpRunner::Result result;
    int attempt = 0;
    for (attempt = 1; attempt <= attempts && urls.empty(); ++attempt) {
        std::vector<std::string> args = base_args;
        args.push_back("-f");
        args.push_back(fmt);
        args.push_back("-g");
        args.push_back("--no-warnings");
        args.push_back(url);
        result = YtdlpRunner::run(args, nullptr, timeout_sec, url);
        urls.clear();
        if (result.launched && result.exit_code == 0 && !result.stdout_output.empty()) {
            std::istringstream ss(result.stdout_output);
            std::string line;
            while (std::getline(ss, line)) {
                while (!line.empty() &&
                       (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
                    line.pop_back();
                if (!line.empty() && line.rfind("http", 0) == 0) {
                    urls.push_back(line);
                }
            }
        }
        if (urls.empty()) {
            LOG(fmt::format(
                "[YouTube] resolve attempt {}/{} failed (launched={}, exit={}, timeout={}s)",
                attempt, attempts, result.launched, result.exit_code, timeout_sec));
            // META-2: yt-dlp's own error is the diagnosis — a fast exit=1 is a solver/
            //   config problem (e.g. missing yt_dlp_ejs on 2026.07+), NOT slowness.
            //   Surface its last stderr line so the log says why, not just "failed".
            std::string tail;
            {
                std::istringstream es(result.stderr_output);
                std::string l;
                while (std::getline(es, l)) {
                    while (!l.empty() && (l.back() == '\r' || l.back() == ' '))
                        l.pop_back();
                    if (!l.empty())
                        tail = l; // keep the last non-empty line
                }
            }
            if (!tail.empty())
                LOG(fmt::format("[YouTube] yt-dlp: {}", tail.substr(0, 300)));
            static bool ejs_hinted = false;
            if (!ejs_hinted && result.stderr_output.find("ejs") != std::string::npos) {
                ejs_hinted = true;
                EVENT_LOG("YouTube nsig solver missing: pip install -U \"yt-dlp[default]\" "
                          "(or pip install yt-dlp-ejs) — every resolve fails fast without it");
            }
            if (attempt < attempts)
                EVENT_LOG(fmt::format("YouTube resolve retry {}/{}...", attempt, attempts));
        }
    }
    // Y11: fetch soft subtitle (.vtt) when [youtube] sub_lang is set; append its path as urls[2].
    //   mpv loads it via sub-file (per-file option) so F/G size, z/Z sync, r/R pos, v visibility
    //   all work (vtt subs are scalable; mpv centers by default). sub_auto=true falls back to
    //   auto-generated captions when no manual subtitle exists for sub_lang.
    std::string sub_lang = IniConfig::instance().get_youtube_sub_lang();
    if (!urls.empty() && !sub_lang.empty()) {
        std::string tmp_dir = Paths::get_tmp_dir();
        if (!tmp_dir.empty()) {
            std::string prefix = tmp_dir + "/pod_sub";
            std::vector<std::string> sargs = YouTubeChannelParser::ytdlp_youtube_args();
            sargs.push_back("--write-subs");
            if (IniConfig::instance().get_youtube_sub_auto())
                sargs.push_back("--write-auto-sub");
            sargs.push_back("--sub-langs");
            sargs.push_back(sub_lang);
            sargs.push_back("--sub-format");
            sargs.push_back("vtt");
            sargs.push_back("--skip-download");
            sargs.push_back("-o");
            sargs.push_back(prefix);
            sargs.push_back("--no-warnings");
            sargs.push_back(url);
            YtdlpRunner::run(sargs, nullptr, 20, url);
            // yt-dlp writes <prefix>.<lang>.vtt (or <prefix>.<lang>-auto.vtt); pick the first .vtt.
            std::error_code ec;
            for (auto &e : fs::directory_iterator(tmp_dir, ec)) {
                std::string n = e.path().filename().string();
                if (n.rfind("pod_sub", 0) == 0 && e.path().extension() == ".vtt") {
                    urls.push_back(e.path().string());
                    LOG(fmt::format("[YouTube] subtitle loaded: {}", n));
                    break;
                }
            }
        }
    }
    if (!urls.empty()) {
        LOG(fmt::format("[YouTube] Resolved {} stream URL(s) (fmt={}, has_video={}, audio_only={}, "
                        "attempts={}): {}",
                        urls.size(), fmt, has_video, audio_only,
                        attempt > attempts ? attempts : attempt, urls[0].substr(0, 70)));
        return urls;
    }
    LOG(fmt::format("[YouTube] yt-dlp -g failed after {} attempt(s) (launched={}, exit={}, fmt={})",
                    attempt > attempts ? attempts : attempt, result.launched, result.exit_code,
                    fmt));
    if (!result.stderr_output.empty()) {
        std::string err = result.stderr_output;
        size_t pos = err.find_last_of('\n');
        if (pos != std::string::npos && pos > 0) {
            size_t prev = err.find_last_of('\n', pos - 1);
            err = err.substr(prev == std::string::npos ? 0 : prev + 1);
            while (!err.empty() && err.back() == '\n')
                err.pop_back();
        }
        LOG(fmt::format("[YouTube] yt-dlp error: {}", err));
        EVENT_LOG(fmt::format("YouTube resolve failed: {}", err));
        // D51: signature hints — the two dominant YouTube-side failures (both observed
        //   2026-08-16) map to different user remedies, so name them explicitly instead
        //   of leaving a raw yt-dlp error line.
        if (err.find("Sign in to confirm") != std::string::npos) {
            EVENT_LOG("Hint: YouTube bot-wall — cookies missing or expired. Re-export "
                      "youtube_cookie.txt (see [youtube] comments in config.ini) and retry");
        } else if (err.find("Requested format is not available") != std::string::npos) {
            EVENT_LOG("Hint: YouTube served a degraded (storyboard-only) response — exit-IP "
                      "risk control. Retry later or re-export cookies; playback formats are "
                      "temporarily unavailable from this network");
        }
    } else {
        EVENT_LOG(fmt::format("YouTube resolve failed after {} attempt(s) (no yt-dlp output) — "
                              "cookies/proxy/JS-runtime?",
                              attempt > attempts ? attempts : attempt));
    }
    return {}; // empty → caller skips playback (single resolve path, no fallback)
}

// D14-2: canonical now-playing Media — the single identity+view the read/persist side converges on
//   (replacing the scattered current_url string = mpv's played path). Derives from the authoritative
//   playback_node_ (source url/title/art_url) + the per-track is_video flag.
Media PlaybackService::now_playing() const {
    Media m = media_from_node(playback_node_);
    m.is_video = now_playing_is_video_;
    return m;
}

// Play a single item by index (pointer-driven model).
// F23: YouTube URLs resolved async in pool_ (non-blocking); local/non-YouTube play immediately.
void PlaybackService::play_current(int idx, AppMode mode, PlayMode play_mode) {
    EventBus::instance().publish(
        PlaybackTrackEnded{}); // D11-1: previous track superseded → SubtitleService stop_realtime (realtime ASR must not carry across tracks)
    std::string orig_url;
    bool has_video = false;
    std::string title; // P1-4: snapshot under the lock; lambdas capture by value
    int duration = 0;
    TreeNodePtr pn; // F35: source node of the playing item (for INFO title)
    {
        std::lock_guard<std::mutex> pl_lock(playlist_mutex_);
        if (idx < 0 || idx >= static_cast<int>(current_playlist_.size()))
            return;
        current_index_ = idx;
        orig_url = current_playlist_[idx].url;
        has_video = current_playlist_[idx].is_video;
        title = current_playlist_[idx].title;
        duration = current_playlist_[idx].duration;
        pn = current_playlist_[idx].node;
        if (play_mode == PlayMode::SHUFFLE) {
            shuffle_queue_.clear();
            refill_shuffle_queue();
        }
    }
    // F35: track the playing node on the calling thread (for all 3 play paths, incl. YouTube which
    //   resolves the stream async — the TITLE is the node title, known immediately). Previously
    //   playback_node was always reset to nullptr, so INFO Title fell back to mpv media-title
    //   (stream URL/ICY for radio, not the station name).
    playback_node_ = pn; // D9-2: authoritative track state (queried via playback_node())
    playback_mode_ = mode;
    now_playing_is_video_ = has_video; // D14-2: per-track is_video for now_playing()
    // D9 + D10-3 Step 2: publish the track change WITH its has_video flag. SubtitleService subscribes
    //   → begin_track(pn, has_video) — subtitle A/B setup is now event-driven (was a direct call
    //   here). Dispatch is synchronous on this (UI) thread, so begin_track runs before play() below,
    //   exactly as the old imperative call did. Method A (mpv sub-add, video) vs Method B
    //   (SubtitleManager async fetch+parse → LYRIC, audio + non-mpv video formats) is decided inside
    //   begin_track; fully async — play() below is not blocked (no sync fs::exists; slow on WSL2).
    EventBus::instance().publish(PlaybackTrackChanged{pn, mode, has_video});
    // Y24.55: flag IPTV context so mpv_controller emits IPTV: messages alongside MPV: behavior for
    //   the same event (off-air, audio-only, slow, load/AO/VO/decode failures).
    player_.set_iptv_context(mode == AppMode::IPTV);

    // Y23.9: BUFFERING state — set pending before play; cleared when mpv reports has_media.
    // Y24.17: timestamp each step so panicast.log shows WHERE the wait goes (sync fs::exists/DB vs
    //   mpv load). >5s on a local file is abnormal — this pinpoints it.
    set_buffering_(
        true); // Y23.9: BUFFERING state — set pending before play; cleared when mpv reports has_media
    auto play_t0 = std::chrono::steady_clock::now();
    auto ms_since = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - play_t0)
            .count();
    };
    LOG(fmt::format("[PLAY] play_current start: idx={} '{}' (is_video={})", idx, title, has_video));

    std::string local = CacheManager::instance().get_local_file(orig_url);
    std::string local_url;
    // Y23.9: pass the RAW path to mpv (no file:// prefix) — mpv handles special chars (Chinese,
    //   spaces, : ! etc.) natively in raw paths, but file:// URLs require encoding which was
    //   missing → MPV_ERROR_LOADING_FAILED (-14) on paths with non-ASCII/special chars.
    if (!local.empty() && fs::exists(local))
        local_url = local;
    LOG(fmt::format("[PLAY] get_local_file+fs::exists: local='{}' ({}ms)", local_url, ms_since()));
    URLType ut = URLClassifier::classify(orig_url);

    if (!local_url.empty()) {
        auto [saved_pos, completed] = DatabaseManager::instance().get_progress(
            orig_url); // D14-4: key on source URL (not played cache path)
        // ASR-fix (2026-08-15): RADIO_STREAM classification also folds in direct podcast .mp3 URLs
        //   (extension-based) — resume those too when the item carries a finite duration (RSS
        //   enclosure). True live streams have duration==0 and stay non-resumable.
        if (saved_pos > 5.0 && !completed && (ut != URLType::RADIO_STREAM || duration > 0)) {
            player_.set_resume_position(local_url, saved_pos);
            EVENT_LOG(fmt::format("Resume from {:02d}:{:02d}", static_cast<int>(saved_pos) / 60,
                                  static_cast<int>(saved_pos) % 60));
        }
        // Y24.17: video subtitle is probed+added async (mpv sub-add); pass "" here.
        player_.play(local_url, has_video, "", ""); // Y24.17: video sub added async via sub-add
        player_.set_keep_open(play_mode == PlayMode::REPEAT);
        player_.set_loop_file(play_mode == PlayMode::REPEAT);
        player_.set_pause(false);
        record_play_history(orig_url, title, duration);
        EVENT_LOG(fmt::format("Play local file: idx {} '{}'", idx, orig_url));
    } else if (URLClassifier::is_youtube(ut)) {
        EVENT_LOG("Resolving YouTube stream via yt-dlp -g (async)...");
        // play_mode is a parameter here (not a member), so capture it for the async lambda.
        pool_->submit([this, orig_url, has_video, idx, title, duration, play_mode]() {
            auto yt_urls = resolve_youtube_url(orig_url, has_video);
            if (yt_urls.empty()) {
                EVENT_LOG("YouTube resolve failed — skipping playback");
                return;
            }
            std::string audio = (yt_urls.size() >= 2) ? yt_urls[1] : std::string();
            std::string sub = (yt_urls.size() >= 3) ? yt_urls[2] : std::string();
            player_.play(yt_urls[0], has_video, audio, sub);
            player_.set_keep_open(play_mode == PlayMode::REPEAT);
            player_.set_loop_file(play_mode == PlayMode::REPEAT);
            player_.set_pause(false);
            record_play_history(orig_url, title, duration);
            EVENT_LOG(fmt::format("Play online streaming: idx {} '{}'", idx, orig_url));
        });
    } else if (ut == URLType::BILIBILI_VIDEO || ut == URLType::DOUYIN_VIDEO ||
               ut == URLType::TIKTOK_VIDEO) {
        // Y21 (issue 3): play the watch URL directly via mpv's ytdl_hook. yt-dlp's bilibili
        //   extractor supplies the required Referer/http_headers to mpv for ALL streams (video+audio
        //   DASH), which a pre-resolve `yt-dlp -g` + manual audio-file approach cannot do (CDN 403
        //   on the audio stream). Correct single path — no pre-resolve.
        player_.play(orig_url, true);
        player_.set_keep_open(play_mode == PlayMode::REPEAT);
        player_.set_loop_file(play_mode == PlayMode::REPEAT);
        player_.set_pause(false);
        record_play_history(orig_url, title, duration);
        EVENT_LOG(fmt::format("Play Bilibili (ytdl_hook): idx {} '{}'", idx, orig_url));
    } else {
        // stream-fix (2026-08-16): finite http(s) episodes (RSS enclosure, duration > 0) resolve
        //   their redirect chain first via resolve_stream_url_ — see its doc comment for the
        //   9-77s mpv chain cost this removes. Live streams (duration == 0, radio/IPTV) keep the
        //   direct path: their redirect targets can be one-time tokens, and a Range probe can
        //   hang stream endpoints. The resolve runs in a pool task (curl I/O never on the UI
        //   thread — same shape as the YouTube branch above); BUFFERING stays pending until mpv
        //   reports media, which is exactly the "buffering…" state the user should see meanwhile.
        bool resolve_chain = duration > 0 && orig_url.rfind("http", 0) == 0;
        // Y24.17: video subtitle probed+added async; pass "" here.
        auto play_resolved = [this, orig_url, has_video, idx, title, duration, play_mode,
                              resolve_chain]() {
            std::string play_url = resolve_chain ? resolve_stream_url_(orig_url) : orig_url;
            player_.play(play_url, has_video, "", ""); // Y24.17: video sub added async via sub-add
            player_.set_keep_open(play_mode == PlayMode::REPEAT);
            player_.set_loop_file(play_mode == PlayMode::REPEAT);
            player_.set_pause(false);
            record_play_history(orig_url, title, duration);
            // F41: file:// URLs are local files (incl. WSL2 /mnt/ mounts), not "online streaming".
            const char *play_kind =
                (orig_url.rfind("file://", 0) == 0) ? "local file" : "online streaming";
            EVENT_LOG(fmt::format("Play {}: idx {} '{}'", play_kind, idx, orig_url));
        };
        if (resolve_chain) {
            EVENT_LOG("Resolving stream URL (async)...");
            pool_->submit(std::move(play_resolved));
        } else {
            play_resolved();
        }
    }
}

// Record playback history (called from play_current/on_playback_ended, on the UI thread or a pool
//   thread). on_history_changed_ lets App rebuild the history tree async — was a direct
//   load_history_to_root() call, now decoupled via the callback seam (D8b-2).
void PlaybackService::record_play_history(const std::string &url, const std::string &title,
                                          int duration) {
    if (url.empty())
        return;
    DatabaseManager::instance().add_history(url, title, duration);
    // Y24.26: rebuild history tree async (was sync — caused UI stutter on every track switch).
    //   D9: notify via the bus — App's HistoryChanged subscriber calls load_history_to_root().
    pool_->submit([this]() { EventBus::instance().publish(HistoryChanged{}); });
}

} // namespace panicast
