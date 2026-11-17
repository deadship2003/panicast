// DownloadService — Application Service (functional abstraction layer) for media downloads.
//   Owns the download EXECUTION ENGINE (D43 — App god-object service extraction, #73): the pending
//   queue (pending_downloads_), slot throttling (pump/start_one_download), the shared yt-dlp core
//   (ytdlp_download, Y24.49), and the curl path with retry/resume. App keeps download_node — the
//   thin orchestrator that gathers marked/selected items (via App's mark methods, which stay in App
//   because navigation/input also use them), enqueues them here, pumps, clears marks, and persists
//   the cache. The execution engine takes specific collaborator refs (library_/pool_/subtitle_) —
//   NO App& back-ref, matching the PlaybackService/LibraryService/SubtitleService pattern.
//
//   The download *verification* helpers (capture_exec / probe_media_duration / verify_downloaded_file
//   / VerifyResult) travelled here too as file-local free functions in download_service.cpp — they
//   are pure functions (no member state) consumed only by the download engine, so they need no place
//   in the class interface. Bodies of start_one_download/ytdlp_download are verbatim from App (same
//   member names: library_/pool_/subtitle_/pending_downloads_ — behaviour-equivalent).
//
//   Behaviour is UNCHANGED. The download path is not covered by the pty smoke test (no network);
//   it is verified by the user's end-to-end batch test ("user tests at the end in one pass").
#pragma once

#include <cstddef>
#include <deque>
#include <vector>

#include "panicast/core/types.h" // TreeNodePtr

namespace panicast
{

class LibraryService;
class ThreadPool;
class SubtitleService;

class DownloadService
{
public:
    DownloadService(LibraryService &library, ThreadPool &pool, SubtitleService &subtitle)
        : library_(library), pool_(pool), subtitle_(subtitle) {}

    // Enqueue items into the pending queue (main-thread-only). App::download_node calls this with
    //   the gathered marked/selected items; pump() then promotes them into free slots.
    void enqueue(const std::vector<TreeNodePtr> &items);

    // Promote pending downloads into free ProgressManager slots. current_slot_count = current
    //   number of ProgressManager entries (active + within their completion display window).
    //   Main-thread-only (called from the render loop's prepare_frame + from download_node).
    void pump(size_t current_slot_count);

    // Pending-queue access — App's prepare_frame reads it for the per-frame "··· +N pending" summary.
    //   Main-thread-only.
    std::deque<TreeNodePtr> &pending_downloads() { return pending_downloads_; }

private:
    // Start one download for node n. Returns true only if a NEW ProgressManager slot was created
    //   (NEW); REUSED_ACTIVE and RESET_EXISTING reuse an existing slot and return false, so the
    //   throttle counter in pump is not double-counted.
    bool start_one_download(TreeNodePtr n);
    // Y24.49: shared yt-dlp download core — run yt-dlp with progress parsing, verify, and cache the
    //   result. Used by the YouTube and Bilibili/TikTok/Douyin video branches (DRY). `site_args` is
    //   the site-specific prefix (cookies / player_client / js_runtime); the common -f / -o /
    //   --progress / url args are appended here.
    void ytdlp_download(const std::string &url, const std::vector<std::string> &site_args,
                        const std::string &dir, const std::string &base_name,
                        const std::string &title, const std::string &dl_id, TreeNodePtr n);

    LibraryService &library_;
    ThreadPool &pool_;
    SubtitleService &subtitle_;
    std::deque<TreeNodePtr> pending_downloads_;
};

} // namespace panicast
