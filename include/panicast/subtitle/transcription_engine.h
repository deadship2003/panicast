// Y24.19/20: TranscriptionEngine — whisper.cpp speech-to-text.
//   Offline (Y24.19): queue local-file nodes → ffmpeg + whisper-cli → <file>.srt sidecar.
//   Real-time (Y24.20): playing + no subtitle → L → ffmpeg (→wav) + whisper-cli (progressive stdout)
//   → segments fed to SubtitleManager → LYRIC shows them; on completion save SRT.
//   Dynamic concurrency for offline (≤ max_concurrent, getloadavg CPU-aware). No shell (posix_spawnp).
#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "panicast/core/types.h"               // TreeNodePtr
#include "panicast/subtitle/subtitle_parser.h" // TranscriptSegment

namespace panicast
{

class SubtitleManager;
class ThreadPool;
class IFrontend;
class MPVController; // Y24.28: for video ASR

class TranscriptionEngine {
public:
    void init(SubtitleManager *sm, ThreadPool *pool, MPVController *mpv = nullptr) {
        sm_ = sm;
        pool_ = pool;
        mpv_ = mpv;
    }

    // Offline (Y24.19): queue local-file nodes for transcription → <file>.srt sidecar.
    void enqueue_offline(const std::vector<TreeNodePtr> &nodes);
    void stop_offline(); // Y24.21: stop the offline batch (kill current + clear queue)

    // Real-time (Y24.20): transcribe the playing track progressively → LYRIC; save SRT on done.
    //   url = the playing media URL/path; is_streaming = true if not a local file (SRT → data_dir).
    //   auto_started (D49): this call came from the play-path auto-ASR (not a user L / :asr) — the
    //   worker then SKIPS non-seekable (live / unknown-duration) media instead of rolling-capturing
    //   it forever, so auto-ASR can never burn CPU endlessly on a radio stream.
    void start_realtime(TreeNodePtr node, const std::string &url, bool is_streaming,
                        bool is_video = false, bool auto_started = false);
    void stop_realtime(); // kill + invalidate the running realtime job
    bool realtime_running() const {
        return realtime_gen_.load() > 0 && realtime_active_.load();
    }

    bool busy() const {
        return active_.load() > 0 || !queue_empty();
    }
    int queue_remaining() const;
    int active_count() const {
        return active_.load();
    }
    void poll(IFrontend &ui);
    void shutdown();

    // Y24.20 path resolution (BTW feedback): bare→PATH/which, abs→fs::exists, ~→$HOME.
    //   Public (D49): the play-path auto-ASR pre-checks availability so users without whisper.cpp
    //   installed get one quiet LOG line, not an EVENT_LOG popup on every track.
    static std::string resolve_whisper_bin();
    static std::string resolve_model();

    // Canonical ASR transcript path: <data_dir>/transcripts/<djb2-hex(url)>.srt. Both offline
    //   (batch) and real-time (streaming + local) ASR write here — never next to the media in
    //   ~/Downloads (user-facing downloads stay clean; app-generated data lives under the XDG
    //   data dir). Public so SubtitleManager can probe the same path on load/replay.
    static std::string transcript_path(const std::string &url);

private:
    SubtitleManager *sm_ = nullptr;
    ThreadPool *pool_ = nullptr;
    MPVController *mpv_ = nullptr; // Y24.28: for video ASR (sub_add + OSD)
    mutable std::mutex mtx_;
    std::deque<TreeNodePtr> queue_;
    std::atomic<int> active_{0};
    std::atomic<int> done_{0};
    std::atomic<int> total_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> stop_offline_{
        false}; // Y24.21: stop the offline batch (kill whisper-cli + clear queue)
    std::thread dispatcher_;
    std::atomic<bool> dispatcher_started_{false}; // Y24.27: atomic (was plain bool — race)
    std::vector<std::thread> workers_;            // Y24.27: track offline workers (was detached)
    std::thread realtime_thread_;                 // Y24.27: track realtime worker (was detached)

    // Real-time state.
    std::atomic<unsigned> realtime_gen_{0}; // bumped on stop/track-change to invalidate workers
    std::atomic<bool> realtime_active_{false};

    bool queue_empty() const;
    void start_dispatcher();
    void dispatcher_loop();
    void transcribe_one(TreeNodePtr node); // offline: ffmpeg + whisper-cli → <file>.srt
    void realtime_worker(TreeNodePtr node, std::string url, bool is_streaming, bool is_video,
                         unsigned gen, bool auto_started);
    // review-fix (2026-08-16): gen-guarded worker teardown — a superseded worker must NOT clear
    //   realtime_active_ (the newborn worker owns it now); see impl for the failure it fixes.
    void finish_realtime_(unsigned gen);

    // Y24.20 path resolution moved to public (D49 — see above).
    // Save segments as SRT into transcript_path(url).
    static void save_srt(const std::vector<TranscriptSegment> &segs, TreeNodePtr node,
                         const std::string &url);
};

} // namespace panicast
