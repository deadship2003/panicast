// SubtitleService — the Application Service (functional abstraction layer) for subtitles / transcription (ASR).
//   Owns the SubtitleManager (detect/probe/load/offset/status) and TranscriptionEngine
//   (whisper.cpp offline + real-time speech-to-text) that previously lived as bare App members.
//   Centralizes their LIFECYCLE wiring: init() wires the engine to its own SubtitleManager + the
//   shared pool + mpv (video ASR) — replacing App's transcription_engine_.init(&subtitle_mgr_,…)
//   so the inter-object wiring stays inside the service; shutdown()/poll() are the system-teardown
//   and per-frame handoff entry points. The remaining trigger sites (App input / remote / download
//   + PlaybackService) reach the engines through the accessors subtitle_mgr() /
//   transcription_engine() — transitional, like the D8b-1 queue-state accessors. D10-2 moves the
//   subtitle input keys (LYRIC toggle / offset / ASR) onto the message bus as SubtitleActions the
//   service subscribes to; D10-3 makes the service event-driven (subscribe PlaybackTrackChanged →
//   auto-load the new track's transcript; publish SubtitleStatusChanged for direct UI/remote subs).
//   This is the second Application Service (after PlaybackService) and the foundation of the M3
//   SubtitleController.   (D10-1 — UI-decoupling M1.)
#pragma once

#include <cstddef>
#include <vector>

#include "panicast/subtitle/subtitle_manager.h"     // Y24.7
#include "panicast/subtitle/transcription_engine.h" // Y24.19/20

namespace panicast
{

class ThreadPool;
class MPVController;
class IFrontend;

// D11-3a: the result of resolving the best AVAILABLE (non-ASR) subtitle source for a track.
//   resolve_subtitle_source returns the first that applies, in priority order, so every ASR entry
//   point (L-key / :asr / remote asr_start) applies the SAME "local subtitle first" chain instead of
//   each re-implementing it. ASR is the fallback the CALLER starts when kind == None.
struct ResolvedSubtitle {
    enum Kind { None, Embedded, LocalSrt, Online };
    Kind kind = None;
    std::string path; // LocalSrt: the file path; Online: the node's subtitle_url (also in path for convenience)
};

class SubtitleService {
public:
    // Wire the transcription engine to this service's own SubtitleManager + the shared pool + mpv
    //   (Y24.28: mpv for video ASR). Replaces App's transcription_engine_.init(&subtitle_mgr_,…).
    void init(ThreadPool &pool, MPVController &mpv);

    // Teardown — stop the transcription dispatcher. Call before the pool joins (as App's dtor did).
    void shutdown();

    // Per-frame UI-thread handoff: push any pending transcript to the UI + offset + logs.
    //   Forwards SubtitleManager::poll (called once per frame from App's run loop).
    void poll(IFrontend &ui, bool lyric_bar_requested);

    // ── Track-change orchestration (D10-3 relocated from PlaybackService → D11-1 fully event-driven) ──
    //   begin_track is triggered by the PlaybackTrackChanged subscription (manual play + auto-advance);
    //   stop_realtime by the PlaybackTrackEnded subscription (track end / supersede). init() wires both.
    //   PlaybackService no longer references SubtitleService at all — playback↔subtitle is 100% via the
    //   bus. Same objects throughout (pool_/mpv_/subtitle_mgr_ are init()'s, = the one MPVController).
    // Stop any running real-time ASR. Triggered on EVERY track end/switch (the PlaybackTrackEnded
    //   subscriber) — real-time ASR must not carry across tracks. Idempotent (no-op when nothing running).
    void stop_realtime();
    // Begin subtitle setup for a track about to play. Triggered by the PlaybackTrackChanged
    //   subscription for BOTH manual play and auto-advance (Option B, D10-3): auto-advance now
    //   re-identifies the media type from the event's has_video flag and follows the SAME A/B branch
    //   as a manual play (was a Method-B-only load_transcript). has_video → reset Method B, async
    //   sidecar probe, then mpv sub_add (Method A) for mpv formats or load_async (Method B) otherwise;
    //   !has_video → Method B load_async. Fully async — never blocks play().
    void begin_track(TreeNodePtr node, bool has_video);

    // D11-3a: resolve the best available NON-ASR subtitle source for a track, in priority order
    //   (Embedded [mpv has an active sub track] > LocalSrt [unified find_local_subtitle: download-dir
    //   + adjacent] > Online [node->has_subtitle + subtitle_url]). Returns {None} when nothing local/
    //   online exists → caller falls back to ASR. `:asr` intentionally does NOT call this (it forces
    //   ASR past all local sources). Centralizes "local subtitle first": remote asr_start + L-key both go
    //   through here, so ASR only runs when no cheaper source exists.
    ResolvedSubtitle resolve_subtitle_source(TreeNodePtr node);

    // ── Engine access (transitional — D10-2/3 replace direct use with Actions/events) ──
    //   PlaybackService.attach() and App's input/remote/download sites use these; they are the
    //   redirect seam for the D10-1 ownership move (no behaviour change).
    SubtitleManager &subtitle_mgr() {
        return subtitle_mgr_;
    }
    TranscriptionEngine &transcription_engine() {
        return transcription_engine_;
    }

private:
    SubtitleManager subtitle_mgr_;
    TranscriptionEngine transcription_engine_; // Y24.19: whisper.cpp offline (later realtime) ASR
    // D10-3 Step 1: retained from init() so the track-change orchestration methods can submit pool
    //   work + call mpv sub_add (video Method A) — previously PlaybackService reached the pool/mpv
    //   via its own pointers; that path now lives here.
    ThreadPool *pool_ = nullptr;
    MPVController *mpv_ = nullptr;
    // D49: play-path auto-ASR — after a track starts, if NO cheaper subtitle source exists (no
    //   local sidecar, no online 📜, no embedded track) and the media is a LOCAL file, kick off
    //   real-time whisper transcription in the background so the LYRIC panel lights up when
    //   segments arrive. Play is NEVER delayed — this runs entirely in the pool. Runs in a pool
    //   task (find_local_subtitle stats the WSL2 /mnt/e mount); NOT for streaming URLs (the
    //   realtime path would download the whole episode first — that stays a deliberate L press),
    //   and the engine's auto gate skips live/unknown-duration media.
    void maybe_auto_asr_(TreeNodePtr node, bool has_video);
    // D10-3 Step 2: EventBus subscription tokens (PlaybackTrackChanged → begin_track). Unsubscribed
    //   in shutdown() so the bus never invokes a callback on a destroyed service.
    std::vector<std::size_t> subs_;
};

} // namespace panicast
