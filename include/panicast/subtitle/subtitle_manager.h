// Y24.7: SubtitleManager — centralizes ALL subtitle handling (previously scattered across
//   rss_parser / app_download / app_playback / app_input). Owns the load status machine, the
//   z/Z sync offset, and the async fetch→parse→UI handoff. Emits a full diagnostic LOG chain
//   (detect → probe → fetch → parse → segments) so load vs parse vs stale-cache is visible.
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "panicast/core/types.h"               // TreeNodePtr, TranscriptStatus
#include "panicast/subtitle/subtitle_parser.h" // TranscriptSegment

namespace panicast
{

class IFrontend;
class ThreadPool;

class SubtitleManager {
public:
    // RSS detection — called by rss_parser on <podcast:transcript url= type=>. Sets the node's
    //   subtitle_url / has_subtitle / subtitle_type and logs recognition.
    static void detect_from_rss(TreeNodePtr node, const std::string &url, const std::string &type);

    // Probe for a same-name local sidecar (.json/.srt/.vtt/.lrc/.transcript) next to node->local_file.
    //   If found AND subtitle_url is empty, sets subtitle_url to the sidecar path (+ has_subtitle).
    //   Idempotent. Returns the sidecar path if one exists ("" otherwise).
    static std::string probe_sidecar(TreeNodePtr node);

    // D11-3a: unified local-subtitle finder — the ONE place that decides whether a track has a
    //   local subtitle file. ASR transcripts live under <data_dir>/transcripts/<djb2-hex(url)>.srt
    //   (app-data dir, not ~/Downloads), so the ASR SRT is checked FIRST, then a same-name sidecar
    //   next to local_file (any subtitle ext — for online 📜 sidecars). Supersedes the adjacent-only
    //   find_sidecar for all callers (probe_sidecar, load_async, SubtitleService::resolve_subtitle_source)
    //   so the "local subtitle first" policy has a single source of truth. Returns the path or "".
    static std::string find_local_subtitle(TreeNodePtr node);

    // Async load: probe sidecar → fetch+parse node->subtitle_url → set status + pending segments.
    //   Sets LOADING before the pool fetch; READY/FAILED on completion. Emits the diagnostic LOG.
    void load_async(TreeNodePtr node, ThreadPool &pool);

    // Y24.18: sync reset to NONE + clear pending (poll clears the UI transcript next frame). Used
    //   when switching to a track that doesn't use Method B (video Method A / no subtitle) so the
    //   LYRIC panel doesn't show the previous track's stale lyrics.
    void reset();

    // Y24.20: progressive transcript feed (real-time transcription) — set segments + READY so poll()
    //   picks them up next frame (set_transcript → LYRIC). Called repeatedly as whisper-cli emits
    //   segments. Thread-safe (worker thread → UI thread handoff via pending_).
    void set_pending(const std::vector<TranscriptSegment> &segs, const std::string &url);

    // UI-thread handoff: if a pending transcript is ready, push segments to the UI (set_transcript)
    //   and update L-mode active state (requested && READY) + offset. Returns true if handed off.
    bool poll(IFrontend &ui, bool lyric_bar_requested);

    // Download the transcript sidecar alongside an episode (format-preserving extension, async).
    //   Saved as <local_file>.<ext> where ext is inferred from subtitle_type / subtitle_url.
    void download_sidecar(TreeNodePtr node, const std::string &local_file, ThreadPool &pool);

    TranscriptStatus status() const;
    bool is_ready() const; // status == READY

    // Force a status re-log on the next poll (track change / L-toggle).
    void force_log();

private:
    mutable std::mutex mtx_;
    TranscriptStatus status_ = TranscriptStatus::NONE;
    TranscriptStatus last_logged_ = TranscriptStatus::NONE;
    bool force_log_ = false;
    std::vector<TranscriptSegment> pending_;
    std::string pending_url_;
    bool pending_ready_ = false;

    void set_status_locked(TranscriptStatus st); // requires mtx_ held; logs transitions (English)
};

} // namespace panicast
