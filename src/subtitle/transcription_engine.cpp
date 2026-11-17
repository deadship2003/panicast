// Y24.19/20: TranscriptionEngine — whisper.cpp offline + real-time transcription.
#include "panicast/subtitle/transcription_engine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <thread>

#include <fmt/format.h>

#include "panicast/config/ini_config.h"
#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/core/paths.h"
#include "panicast/core/utils.h"
#include "panicast/core/thread_pool.h"
#include "panicast/playback/mpv_controller.h" // Y24.28: for video ASR (sub_add + show_osd)
#include "panicast/net/network.h"   // ASR: Network::download_to_file (proxy-aware source fetch)
#include "panicast/storage/cache.h" // ASR: CacheManager (reuse/register the episode's local cache)
#include "panicast/storage/database.h"
#include "panicast/subtitle/subtitle_manager.h"

namespace panicast
{
namespace fs = std::filesystem;

namespace
{
std::string temp_basename() {
    static std::atomic<unsigned> n{0};
    return "/tmp/panicast_wh_" + std::to_string(++n);
}
std::string expand_home(const std::string &p) {
    if (!p.empty() && p[0] == '~') {
        const char *h = std::getenv("HOME");
        if (h)
            return std::string(h) + p.substr(1);
    }
    return p;
}
bool cpu_has_room(int active, int max_concurrent) {
    if (active >= max_concurrent)
        return false;
    double load[1] = {0.0};
    int n = getloadavg(load, 1);
    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0)
        cores = 4;
    double threshold = (cores > 1) ? (static_cast<double>(cores) - 1.0) : 1.0;
    if (n < 1)
        return true;
    return load[0] < threshold;
}
// Thread count for whisper-cli (-t). whisper with -t=$(nproc) pegs EVERY core and starves the
//   ncurses UI thread (and mpv playback) → the TUI becomes unresponsive while transcribing. Reserve
//   cores for the UI + mpv: auto = hardware_concurrency()-2 (min 1). [transcription] threads > 0
//   overrides (power users with dedicated transcode boxes can set it higher).
unsigned whisper_threads() {
    int cfg = IniConfig::instance().get_int("transcription", "threads", 0);
    if (cfg > 0)
        return static_cast<unsigned>(cfg);
    unsigned t = std::thread::hardware_concurrency();
    if (t == 0)
        t = 4;
    if (t > 2)
        t -= 2;
    else
        t = 1;
    return t;
}
// Parse a whisper-cli stdout line "[HH:MM:SS.mmm --> HH:MM:SS.mmm]  text" into a segment.
bool parse_whisper_line(const std::string &line, TranscriptSegment &seg) {
    static const std::regex re(
        R"(\[(\d+):(\d+):(\d+\.\d+)\s*-->\s*(\d+):(\d+):(\d+\.\d+)\]\s*(.*))");
    std::smatch m;
    if (!std::regex_match(line, m, re))
        return false;
    auto to_sec = [](const std::string &h, const std::string &mm, const std::string &ss) {
        return std::stod(h) * 3600.0 + std::stod(mm) * 60.0 + std::stod(ss);
    };
    seg.start = to_sec(m[1].str(), m[2].str(), m[3].str());
    seg.end = to_sec(m[4].str(), m[5].str(), m[6].str());
    seg.text = m[7].str();
    return true;
}
// djb2 string hash → hex (for streaming sidecar filenames; no Math/random).
std::string url_hash(const std::string &s) {
    unsigned long h = 5381;
    for (char c : s)
        h = ((h << 5) + h) + static_cast<unsigned char>(c);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lx", h);
    return buf;
}
// Format seconds → SRT timestamp HH:MM:SS,mmm.
std::string srt_ts(double t) {
    if (t < 0)
        t = 0;
    int ms = static_cast<int>(t * 1000) % 1000;
    int s = static_cast<int>(t);
    int h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d,%03d", h, m, sec, ms);
    return buf;
}
// Write segments to an SRT file.
void write_srt_file(const std::string &path, const std::vector<TranscriptSegment> &segs) {
    std::ofstream f(path);
    if (!f)
        return;
    for (size_t i = 0; i < segs.size(); ++i) {
        f << (i + 1) << "\n"
          << srt_ts(segs[i].start) << " --> " << srt_ts(segs[i].end) << "\n"
          << segs[i].text << "\n\n";
    }
}
// Parse an existing SRT file into segments (via the subtitle parser registry).
std::vector<TranscriptSegment> parse_srt_file(const std::string &path) {
    std::ifstream f(path);
    if (!f)
        return {};
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return SubtitleParserRegistry::instance().parse(content, "srt");
}
// Get audio duration in seconds (ffmpeg -i → parse stderr "Duration: HH:MM:SS.xx").
double get_audio_duration(const std::string &file) {
    auto r = Utils::run_process("ffmpeg", {"-i", file});
    // ffmpeg without output exits 1; stderr has "Duration: 00:30:15.23,"
    size_t pos = r.stderr_out.find("Duration: ");
    if (pos == std::string::npos)
        return -1.0;
    pos += 10; // skip "Duration: "
    // Parse HH:MM:SS.xx
    int h = 0, m = 0;
    double s = 0.0;
    if (std::sscanf(r.stderr_out.c_str() + pos, "%d:%d:%lf", &h, &m, &s) < 3)
        return -1.0;

    return h * 3600.0 + m * 60.0 + s;
}

// Y24.22/23: persist the ASR subtitle marker (has_asr_srt + asr_srt_path) to episode_cache.
void persist_subtitle_marker(TreeNodePtr node) {
    if (!node)
        return;
    auto parent = node->parent.lock();
    if (!parent || parent->type != NodeType::PODCAST_FEED)
        return;
    // Y24.23: ASR SRT -> update has_asr_srt + asr_srt_path (NOT has_subtitle, which is for online 📜).
    node->has_asr_srt = true;
    node->asr_srt_path =
        node->subtitle_url; // subtitle_url was set by probe_sidecar to the SRT path
    DatabaseManager::instance().update_episode_asr(parent->url, node->url, node->asr_srt_path);
    LOG(fmt::format("[Transcribe] persisted ASR marker: feed={} ep={} srt={}",
                    parent->url.substr(0, 40), node->url.substr(0, 40), node->asr_srt_path));
}
} // namespace

// Canonical ASR transcript path (see header). All ASR SRT output — offline batch and real-time
//   (streaming + local) — lands under <data_dir>/transcripts/, keyed by the djb2 hash of the
//   episode URL. This keeps user-facing downloads (~/Downloads) clean and makes the path
//   deterministic for SubtitleManager to probe on load/replay.
std::string TranscriptionEngine::transcript_path(const std::string &url) {
    return Paths::get_data_dir() + "/transcripts/" + url_hash(url) + ".srt";
}

// ── Path resolution (BTW feedback) ──
std::string TranscriptionEngine::resolve_whisper_bin() {
    std::string v = IniConfig::instance().get("transcription", "whisper_bin", "whisper-cli");
    v = expand_home(v);
    if (v.empty())
        return "";
    if (v[0] == '/')
        return fs::exists(v) ? v : ""; // absolute → fs::exists
    return Utils::which_binary(v);     // bare → PATH search
}
std::string TranscriptionEngine::resolve_model() {
    std::string v = IniConfig::instance().get(
        "transcription", "model", "~/.local/share/panicast/models/ggml-small.en-q5_1.bin");
    v = expand_home(v);
    if (v.empty())
        return "";
    if (v[0] == '/')
        return fs::exists(v) ? v : ""; // absolute → fs::exists
    if (v.find('/') != std::string::npos)
        return fs::exists(v) ? v : ""; // relative path
    // bare filename → <data_dir>/models/<file>
    std::string p = Paths::get_data_dir() + "/models/" + v;
    return fs::exists(p) ? p : "";
}

// ── Offline (Y24.19) ──
bool TranscriptionEngine::queue_empty() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return queue_.empty();
}
int TranscriptionEngine::queue_remaining() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return static_cast<int>(queue_.size());
}

void TranscriptionEngine::enqueue_offline(const std::vector<TreeNodePtr> &nodes) {
    stop_offline_ = false; // Y24.21: reset stop flag for a new batch
    int added = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto &n : nodes) {
            if (!n)
                continue;
            std::string file = n->local_file.empty() ? n->url : n->local_file;
            if (file.empty())
                continue;
            queue_.push_back(n);
            ++added;
            ++total_;
        }
    }
    if (added > 0) {
        EVENT_LOG(fmt::format("Transcribe: queued {} file(s) (whisper.cpp offline)", added));
        start_dispatcher();
    } else {
        EVENT_LOG("Transcribe: no playable local files selected");
    }
}

void TranscriptionEngine::stop_offline() {
    stop_offline_ = true;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.clear();
    }
    EVENT_LOG("Transcribe: offline stopped (partial SRT saved for resume)");
}

void TranscriptionEngine::start_dispatcher() {
    if (dispatcher_started_)
        return;
    dispatcher_started_ = true;
    stop_ = false;
    if (dispatcher_.joinable())
        dispatcher_.join();
    dispatcher_ = std::thread([this]() { dispatcher_loop(); });
}

void TranscriptionEngine::dispatcher_loop() {
    int max_concurrent = IniConfig::instance().get_int("transcription", "max_concurrent", 3);
    if (max_concurrent < 1)
        max_concurrent = 1;
    while (!stop_ && !stop_offline_.load()) {
        if (!cpu_has_room(active_.load(), max_concurrent)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        TreeNodePtr node;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (queue_.empty())
                break;
            node = queue_.front();
            queue_.pop_front();
        }
        active_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            workers_.emplace_back([this, node]() {
                transcribe_one(node);
                active_.fetch_sub(1);
                done_.fetch_add(1);
            });
        }
    }
    dispatcher_started_ = false;
    LOG(fmt::format("[Transcribe] dispatcher idle: {}/{} done", done_.load(), total_.load()));
}

void TranscriptionEngine::transcribe_one(TreeNodePtr node) {
    std::string file = node->local_file.empty() ? node->url : node->local_file;
    if (node->local_file.empty())
        node->local_file = file;
    std::string whisper_bin = resolve_whisper_bin();
    std::string model = resolve_model();
    if (whisper_bin.empty()) {
        EVENT_LOG("Transcribe: whisper-cli not found — install whisper-cpp");
        return;
    }
    if (model.empty()) {
        EVENT_LOG("Transcribe: model not found — set [transcription] model");
        return;
    }

    // Y24.20: skip/resume check \u2014 don\u2019t waste CPU re-transcribing files that already have subtitles.
    std::string srt_dst = transcript_path(node->url);
    std::error_code ec;
    fs::create_directories(Paths::get_data_dir() + "/transcripts", ec);

    std::vector<TranscriptSegment> existing_segs;
    int offset_ms = 0; // 0 = full transcribe; >0 = resume from this offset
    if (fs::exists(srt_dst)) {
        existing_segs = parse_srt_file(srt_dst);
        if (!existing_segs.empty()) {
            double last_end = existing_segs.back().end;
            double duration = get_audio_duration(file);
            if (duration > 0 && last_end >= duration - 5.0) {
                EVENT_LOG(fmt::format(
                    "Transcribe skip: \u2018{}\u2019 (already has {} segs, {:.0f}s/{:.0f}s)",
                    node->title, existing_segs.size(), last_end, duration));
                LOG(fmt::format("[Transcribe] skip (complete): {} ({} segs, {:.1}s/{:.1}s)",
                                srt_dst, existing_segs.size(), last_end, duration));
                SubtitleManager::probe_sidecar(node);
                persist_subtitle_marker(node);
                return;
            }
            offset_ms = static_cast<int>(last_end * 1000);
            EVENT_LOG(
                fmt::format("Transcribe resume: \u2018{}\u2019 ({} segs to {:.0f}s, resuming)",
                            node->title, existing_segs.size(), last_end));
            LOG(fmt::format("[Transcribe] resume from {:.1}s (offset_ms={}): {}", last_end,
                            offset_ms, srt_dst));
        }
    }

    std::string tmp_base = temp_basename();
    std::string tmp_wav = tmp_base + ".wav";
    std::string out_base = tmp_base + "_out";
    LOG(fmt::format("[Transcribe] offline start: \u2018{}\u2019 (model={}, offset_ms={})",
                    node->title, model, offset_ms));
    auto r1 = Utils::run_process(
        "ffmpeg", {"-y", "-i", file, "-ar", "16000", "-ac", "1", "-f", "wav", tmp_wav});
    if (!r1.launched || r1.exit_code != 0) {
        LOG(fmt::format("[Transcribe] ffmpeg failed (exit={}): {}", r1.exit_code,
                        r1.stderr_out.substr(0, 200)));
        fs::remove(tmp_wav);
        return;
    }
    unsigned threads =
        whisper_threads(); // capped: leave cores for the UI + mpv (see whisper_threads)
    // Y24.21: streaming whisper-cli (stop_pred enables L-toggle stop; segments captured from stdout).
    std::vector<std::string> wargs = {"-m", model, "-f", tmp_wav, "-t", std::to_string(threads)};
    if (offset_ms > 0) {
        wargs.push_back("-ot");
        wargs.push_back(std::to_string(offset_ms));
    }
    std::vector<TranscriptSegment> new_segs;
    int rc = Utils::run_process_streaming(
        whisper_bin, wargs,
        [&new_segs](const std::string &line) {
            TranscriptSegment seg;
            if (parse_whisper_line(line, seg))
                new_segs.push_back(seg);
        },
        [this]() { return stop_offline_.load(); } // stop_pred: L-toggle kills whisper-cli
    );
    fs::remove(tmp_wav);
    bool stopped = stop_offline_.load();
    if (rc != 0 && !stopped && new_segs.empty()) {
        LOG(fmt::format("[Transcribe] whisper-cli failed (exit={})", rc));
        return;
    }
    // Combine existing + new, save SRT (full or partial for resume).
    if (!new_segs.empty())
        existing_segs.insert(existing_segs.end(), new_segs.begin(), new_segs.end());
    if (!existing_segs.empty())
        write_srt_file(srt_dst, existing_segs);
    if (stopped) {
        EVENT_LOG(fmt::format("Transcribe stopped: {} ({} segs saved, resume with L)", node->title,
                              existing_segs.size()));
        LOG(fmt::format("[Transcribe] stopped (partial): {} ({} segs)", srt_dst,
                        existing_segs.size()));
    } else {
        EVENT_LOG(fmt::format("Transcribe done: {} -> {} ({} segs)", node->title, srt_dst,
                              existing_segs.size()));
    }
    SubtitleManager::probe_sidecar(node);
    persist_subtitle_marker(node);
}

// ── Real-time (Y24.20) ──
void TranscriptionEngine::start_realtime(TreeNodePtr node, const std::string &url,
                                         bool is_streaming, bool is_video, bool auto_started) {
    unsigned gen = ++realtime_gen_;
    realtime_active_ = true;
    EVENT_LOG(fmt::format("Transcribe: real-time start for '{}' (video={}, auto={})",
                          node ? node->title : url, is_video, auto_started));
    // DETACH (don't join) any previous worker. start_realtime() is called from the L key handler on
    //   the UI thread; a .join() here would block the UI until the previous chunk's ffmpeg/whisper
    //   finishes (up to ~chunk+whisper time). The previous worker carries an old gen — it sees the
    //   gen mismatch and self-terminates (killable ffmpeg via stop_pred), and a detached thread is
    //   reaped by the OS / dies with the process at shutdown (_exit). Safe: this app exits via
    //   _exit(0), so detached workers never outlive a live engine.
    if (realtime_thread_.joinable())
        realtime_thread_.detach();
    realtime_thread_ = std::thread([this, node, url, is_streaming, is_video, gen, auto_started]() {
        realtime_worker(node, url, is_streaming, is_video, gen, auto_started);
    });
}

void TranscriptionEngine::stop_realtime() {
    if (!realtime_active_.load())
        return;
    ++realtime_gen_; // invalidate the running worker (stop_pred → kill whisper-cli)
    realtime_active_ = false;
    EVENT_LOG("Transcribe: real-time stopped");
}

// review-fix (2026-08-16): worker teardown — clear realtime_active_ ONLY while this worker still
//   owns the generation. A superseded worker's exit used to store false unconditionally, landing
//   AFTER the newborn start's `= true` (the old worker only exits BECAUSE it saw the gen bump), so
//   realtime_running() went false while worker N+1 was alive — the L-key "already running" check
//   and maybe_auto_asr_'s guard then let a SECOND worker start next to the live one, and
//   stop_realtime()'s `if (!realtime_active_) return;` early-exit made the clobbered worker
//   unkillable (it transcribed the old file to end-of-media). D49 auto-ASR made supersede
//   overlaps routine (a worker runs per local track), so every exit path goes through here now.
void TranscriptionEngine::finish_realtime_(unsigned gen) {
    if (realtime_gen_.load() == gen)
        realtime_active_ = false;
}

// Y24.20 / ASR-fix: real-time transcription. The OLD design decoded the ENTIRE source via a single
//   blocking, non-killable `ffmpeg -i <url>` call BEFORE invoking whisper-cli. For live radio streams
//   that ffmpeg call never returns (infinite capture) → whisper-cli was never reached; for finite
//   remote podcasts it downloaded the whole file first (long stall). It also had no stop hook, so
//   stop_realtime()/track-change/shutdown could not interrupt ffmpeg → the worker thread leaked and
//   start_realtime()'s realtime_thread_.join() (and shutdown()) blocked the UI thread.
//
// NEW design: capture the source in short BOUNDED chunks and transcribe each progressively.
//   - Each chunk is captured with `ffmpeg -ss <start> -t <chunk_sec>` (finite/seekable media) or just
//     `-t <chunk_sec>` (live streams, no seek), so ffmpeg SELF-TERMINATES per chunk instead of
//     capturing forever.
//   - ffmpeg is made interruptible by writing its progress to stdout (`-progress pipe:1
//     -stats_period 1`): run_process_streaming() polls its stop_pred once per stdout line (≈1/s),
//     so stop_realtime() (which bumps realtime_gen_) kills the in-flight ffmpeg within ~1s. The UI
//     thread never blocks on the join again.
//   - Each chunk is transcribed with whisper-cli (which already prints segment lines to stdout, so it
//     is killable the same way); segment timestamps are offset by the chunk start so they map onto the
//     source timeline, accumulated, and fed to the LYRIC panel progressively (audio) / OSD (video).
//   - Finite media: loop until chunk start ≥ duration. Live media: loop until stopped.
void TranscriptionEngine::realtime_worker(TreeNodePtr node, std::string url, bool is_streaming,
                                          bool is_video, unsigned gen, bool auto_started) {
    std::string whisper_bin = resolve_whisper_bin();
    std::string model = resolve_model();
    if (whisper_bin.empty()) {
        EVENT_LOG("Transcribe: whisper-cli not found — install whisper-cpp");
        finish_realtime_(gen);
        return;
    }
    if (model.empty()) {
        EVENT_LOG("Transcribe: model not found — set [transcription] model");
        finish_realtime_(gen);
        return;
    }

    // Y24.22: skip/resume check — load any existing partial/complete SRT and resume from its end.
    //   Keyed on the episode's canonical URL (node->url), so it matches save_srt + find_local_subtitle.
    std::string asr_key = node ? (node->url.empty() ? url : node->url) : url;
    std::string srt_path = transcript_path(asr_key);
    std::vector<TranscriptSegment>
        segs;           // accumulated (pre-filled with existing partial, then grown)
    double start = 0.0; // chunk start offset on the source timeline (seconds)
    // ASR-fix: read the duration from mpv (already loaded) — NOT via `ffmpeg -i <url>`. That probe has
    //   no timeout and HANGS on throttled/slow streaming URLs (YouTube googlevideo via a proxy, slow
    //   podcasts), blocking the worker before the chunk loop ever starts — the "Shift+L does nothing"
    //   regression (log showed "real-time start" then silence). mpv reports media_duration once the
    //   file is loaded; 0/unavailable ⇒ treat as a live stream (no seek, rolling capture).
    double duration = -1.0;
    if (mpv_) {
        auto s = mpv_->get_state();
        duration = s.has_media ? s.media_duration : -1.0;
    }
    // D49 (auto-ASR gate): an AUTO start must never rolling-capture a live stream (endless CPU).
    //   review-fix (2026-08-16): the initial state read above can describe the PREVIOUS track —
    //   begin_track (which spawns this worker) runs BEFORE play() issues the loadfile, so on a
    //   fast mount the worker can win the race against mpv's load + the update_state refresh. A
    //   stale positive duration would then truncate the chunk loop at the OLD track's length (and
    //   wrongly judge the new partial SRT complete below). Discard it and ALWAYS wait (≤10s) for
    //   the NEW media's duration; still unknown ⇒ treat as live and bow out quietly — the user
    //   can still force ASR with L.
    if (auto_started) {
        duration = -1.0;
        for (int i = 0; i < 20 && realtime_gen_.load() == gen; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!mpv_)
                break;
            auto s = mpv_->get_state();
            if (s.has_media && s.media_duration > 0.0) {
                duration = s.media_duration;
                break;
            }
        }
        if (duration <= 0.0) {
            LOG(fmt::format(
                "[Transcribe] auto ASR skipped (live/unknown-duration media): {}", url.substr(0, 80)));
            finish_realtime_(gen);
            return;
        }
    }
    bool seekable = (duration > 0.0);
    if (fs::exists(srt_path)) {
        segs = parse_srt_file(srt_path);
        if (!segs.empty()) {
            double last_end = segs.back().end;
            if (duration > 0 && last_end >= duration - 5.0) {
                // Complete — just load, don't transcribe.
                EVENT_LOG(
                    fmt::format("Transcribe skip (realtime): already has {} segs ({:.0f}s/{:.0f}s)",
                                segs.size(), last_end, duration));
                if (sm_)
                    sm_->set_pending(segs, url);
                if (node) {
                    node->has_asr_srt = true;
                    node->subtitle_url = srt_path;
                    node->subtitle_type = "srt";
                    node->asr_srt_path = srt_path;
                }
                persist_subtitle_marker(node);
                finish_realtime_(gen);
                return;
            }
            start = last_end;
            EVENT_LOG(fmt::format("Transcribe resume (realtime): {} segs to {:.0f}s, resuming",
                                  segs.size(), last_end));
            // Show existing segments immediately while new chunks transcribe.
            if (sm_)
                sm_->set_pending(segs, url);
        }
    }

    unsigned threads =
        whisper_threads(); // capped: leave cores for the UI + mpv (see whisper_threads)
    int chunk_sec = IniConfig::instance().get_int("transcription", "realtime_chunk_sec", 30);
    if (chunk_sec < 5)
        chunk_sec = 5;
    if (chunk_sec > 120)
        chunk_sec = 120;
    auto stop_pred = [this, gen]() { return realtime_gen_.load() != gen; };

    // ASR-fix (2026-08-15): align the first chunk with where playback actually IS. A track resumed
    //   deep into the episode (progress resume) would otherwise chunk from 0s/last-SRT-end and the
    //   LYRIC panel stays blank for many chunks until capture catches up. One chunk of backfill
    //   gives a little context. Never rewinds below an existing SRT's end (no re-transcribing).
    if (seekable && mpv_) {
        auto s = mpv_->get_state();
        double pos = s.has_media ? s.time_pos : 0.0;
        if (pos > chunk_sec * 2.0) {
            double target = pos - chunk_sec; // backfill one chunk
            if (target > start) {
                LOG(fmt::format(
                    "[Transcribe] realtime: aligning start {:.0f}s -> {:.0f}s (playback at {:.0f}s)",
                    start, target, pos));
                start = target;
            }
        }
    }

    EVENT_LOG(fmt::format("Transcribe (realtime): chunked {}s from {:.0f}s{}", chunk_sec, start,
                          seekable ? "" : " (live stream)"));
    LOG(fmt::format("[Transcribe] realtime chunked: chunk={}s start={:.1f}s dur={} url='{}'",
                    chunk_sec, start,
                    seekable ? fmt::format("{:.0f}s", duration) : std::string("live"), url));

    // Source for chunk capture. FINITE media (duration known) needs a LOCAL file: ffmpeg's HTTP input
    //   CANNOT tunnel over the configured SOCKS proxy, so a direct ffmpeg capture stalls on CDN URLs
    //   that need the proxy — the "Shift+L does nothing" bug (ffmpeg hangs on the network read, whisper
    //   never runs, no CPU load, no caption). curl handles SOCKS, so we fetch via curl.
    //   The fetch is a PERSISTENT cache of the episode (not a throwaway temp): if the episode is already
    //   cached (D-key download or a prior ASR), reuse it; otherwise download it ONCE into the normal
    //   download dir and register it via CacheManager (mark_downloaded → DB-persisted, is_downloaded
    //   highlight, and reused by playback + future ASR — no re-fetching). Then ffmpeg seeks the LOCAL
    //   file (instant, no per-chunk network) and chunk transcription stays progressive. LIVE streams
    //   have no finite file → capture directly with ffmpeg (best-effort; -timeout makes a stall fail fast).
    std::error_code ec;
    std::string src = url;
    if (seekable) {
        std::string cached = CacheManager::instance().get_local_file(url);
        if (!cached.empty() && fs::exists(cached, ec)) {
            src = cached;
            LOG(fmt::format("[Transcribe] realtime: using cached local file: {}", cached));
        } else if (fs::exists(url, ec)) {
            // Playing URL is itself a local file (D49 auto-ASR path): use it directly,
            // never route a local path into Network::download_to_file.
            src = url;
            LOG(fmt::format("[Transcribe] realtime: playing URL is a local file: {}", url));
        } else {
            std::string dir = Utils::get_download_dir();
            fs::create_directories(dir, ec);
            std::string ext = ".mp3";
            size_t dot = url.find_last_of('.'), slash = url.find_last_of('/');
            if (dot != std::string::npos && dot > slash) {
                std::string ue = url.substr(dot);
                size_t q = ue.find('?');
                if (q != std::string::npos)
                    ue = ue.substr(0, q);
                if (ue.length() <= 5 && !ue.empty())
                    ext = ue;
            }
            std::string path =
                dir + "/" + Utils::sanitize_filename(node ? node->title : std::string("asr")) + ext;
            EVENT_LOG(fmt::format("Transcribe: caching episode via proxy ({:.0f}s; highlighted + "
                                  "reused next time)...",
                                  duration));
            if (!Network::download_to_file(url, path, 600)) {
                LOG(fmt::format("[Transcribe] realtime: source download failed: {}",
                                url.substr(0, 80)));
                EVENT_LOG(
                    "Transcribe: source download failed — check [network] proxy / connectivity");
                CacheManager::instance().mark_partial(url);
                finish_realtime_(gen);
                return;
            }
            if (realtime_gen_.load() != gen) {
                finish_realtime_(gen);
                return;
            } // stopped during fetch
            CacheManager::instance().mark_downloaded(url, path); // persist + reuse (+ highlight)
            if (node) {
                node->local_file = path;
                node->is_downloaded = true;
            } // highlight this session
            src = path;
        }
    }

    bool stopped = false;
    while (realtime_gen_.load() == gen) {
        // Finite media stop condition.
        if (seekable && start >= duration - 0.5)
            break;

        // ── capture one bounded chunk → wav (killable via -progress pipe:1) ──
        std::string tmp_wav = temp_basename() + ".wav";
        std::vector<std::string> fargs = {"-y"};
        // ASR-fix (2026-08-15): -timeout is an http-protocol option — on a LOCAL input file ffmpeg
        //   rejects it outright ("Option timeout not found", rc=8), so every local-media capture
        //   failed at step 0. Only live (non-seekable) media actually reads the network URL here;
        //   finite media's src is always a local file by then (cache/download above).
        if (!seekable) {
            fargs.push_back("-timeout");
            fargs.push_back("20000000"); // 20s net read timeout (live/network input)
        }
        if (seekable && start > 0.5) {
            fargs.push_back("-ss");
            fargs.push_back(fmt::format("{:.3f}", start));
        }
        fargs.push_back("-i");
        fargs.push_back(src);
        fargs.push_back("-t");
        fargs.push_back(std::to_string(chunk_sec));
        fargs.push_back("-ar");
        fargs.push_back("16000");
        fargs.push_back("-ac");
        fargs.push_back("1");
        fargs.push_back("-f");
        fargs.push_back("wav");
        fargs.push_back(tmp_wav);
        fargs.push_back("-progress");
        fargs.push_back("pipe:1"); // progress→stdout so stop_pred is polled ~1/s
        fargs.push_back("-stats_period");
        fargs.push_back("1");
        LOG(fmt::format("[Transcribe] realtime: capture chunk [{:.1f},{:.1f}]s", start,
                        start + chunk_sec));
        int frc = Utils::run_process_streaming("ffmpeg", fargs, nullptr, stop_pred);
        if (realtime_gen_.load() != gen) {
            stopped = true;
            fs::remove(tmp_wav);
            break;
        }
        // ffmpeg returns non-zero at end-of-media for the last partial chunk, or on a decode error;
        // either way we stop after saving whatever segments we have. A too-small/missing wav also means
        // nothing useful was captured for this chunk.
        if (frc != 0 || !fs::exists(tmp_wav, ec) || fs::file_size(tmp_wav, ec) < 1000) {
            LOG(fmt::format("[Transcribe] realtime capture end/failed (rc={}, start={:.1f}s)", frc,
                            start));
            fs::remove(tmp_wav, ec);
            break;
        }

        // ── transcribe the chunk; offset segment times onto the source timeline ──
        std::vector<std::string> wargs = {"-m",    model, "-f",
                                          tmp_wav, "-t",  std::to_string(threads)};
        size_t before = segs.size();
        Utils::run_process_streaming(
            whisper_bin, wargs,
            [this, &segs, &url, &start, gen, is_video](const std::string &line) {
                if (realtime_gen_.load() != gen)
                    return;
                TranscriptSegment seg;
                if (parse_whisper_line(line, seg)) {
                    seg.start += start;
                    seg.end += start; // map chunk-local times → source timeline
                    segs.push_back(seg);
                    if (!is_video && sm_)
                        sm_->set_pending(segs, url); // audio: progressive LYRIC feed
                    if (is_video && mpv_ && (segs.size() % 5 == 0))
                        mpv_->show_osd(fmt::format("Transcribing... {} segments", segs.size()),
                                       1500);
                }
            },
            stop_pred // whisper-cli prints segment lines to stdout → killable on stop
        );
        fs::remove(tmp_wav, ec);
        if (realtime_gen_.load() != gen) {
            stopped = true;
            break;
        }
        if (segs.size() == before)
            LOG(fmt::format(
                "[Transcribe] realtime: chunk at {:.1f}s produced no segments (silence?)", start));

        start += chunk_sec; // advance to the next window
    }
    // NOTE: src is the episode's persistent cache (dl_dir) — do NOT delete it; it's reused by
    //   playback + future ASR (registered via CacheManager). Only tmp_wav chunks are removed above.

    finish_realtime_(gen);
    if (segs.empty()) {
        if (!stopped)
            EVENT_LOG("Transcribe: real-time produced no segments");
        return;
    }
    save_srt(segs, node, asr_key);
    if (node && !is_streaming)
        SubtitleManager::probe_sidecar(node);
    // Y24.28: video → load the SRT into mpv (renders in video window, bottom center).
    if (is_video && mpv_) {
        mpv_->sub_add(transcript_path(asr_key));
        if (!stopped)
            mpv_->show_osd(fmt::format("Transcription complete: {} segments", segs.size()), 3000);
    }
    persist_subtitle_marker(node);
    EVENT_LOG(fmt::format("Transcribe done (real-time): {} segments → saved{}", segs.size(),
                          stopped ? " (stopped)" : ""));
}

void TranscriptionEngine::save_srt(const std::vector<TranscriptSegment> &segs, TreeNodePtr node,
                                   const std::string &url) {
    std::string dst = transcript_path(url);
    std::error_code ec;
    fs::create_directories(Paths::get_data_dir() + "/transcripts", ec);
    std::ofstream f(dst);
    if (!f) {
        LOG(fmt::format("[Transcribe] save_srt: cannot write {}", dst));
        return;
    }
    for (size_t i = 0; i < segs.size(); ++i) {
        f << (i + 1) << "\n"
          << srt_ts(segs[i].start) << " --> " << srt_ts(segs[i].end) << "\n"
          << segs[i].text << "\n\n";
    }
    LOG(fmt::format("[Transcribe] SRT saved: {} ({} segments)", dst, segs.size()));
    if (node) {
        // Record the sidecar path on the node so probe/replay can find it (streaming AND local).
        node->subtitle_url = dst;
        node->has_asr_srt = true;
        node->asr_srt_path = dst;
        node->subtitle_type = "srt";
    }
}

void TranscriptionEngine::poll(
    IFrontend & /*ui*/) { /* progress via EVENT_LOG; reserved for a progress bar */ }

void TranscriptionEngine::shutdown() {
    stop_ = true;
    stop_offline_ = true;
    ++realtime_gen_; // invalidate any running realtime job
    realtime_active_ = false;
    if (realtime_thread_.joinable())
        realtime_thread_.join();
    if (dispatcher_.joinable())
        dispatcher_.join();
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto &w : workers_)
        if (w.joinable())
            w.join();
    workers_.clear();
}

} // namespace panicast
