// PlaybackService — the Application Service (functional abstraction layer) for playback. Owns:
//   • playback ACTION handling (UI→Core via the message bus): play/pause, volume (D8a).
//   • the implicit "playlist" QUEUE STATE + pure queue logic (D8b-1): current_playlist,
//     current_index, shuffle_queue_ and the playlist_mutex_ that guards them, plus
//     clear_playlist / refill_shuffle_queue / random_peer_index.
//   • playback / autoplay LOGIC (D8b-2): play_current / on_playback_ended / record_play_history /
//     resolve_youtube_url. pool_ / subtitle / transcription deps are injected late via attach()
//     (declared AFTER playback_ in App, so they can't be construction-time references).
// The mpv command worker (in MPVController) executes player calls off the UI thread.
// State ownership (D9-2/D9-3): ALL playback runtime state lives HERE as private members —
//   playback_node_ / playback_mode_ (the "what's playing" track handles, D9-2, queried via
//   playback_node() / playback_mode()) and playback_pending_(_start_) (the BUFFERING handle, D9-3,
//   driven per-frame by advance_buffering()). App owns NO playback-state member. State CHANGES are
//   announced on the EventBus (D9): PlaybackTrackChanged / PlaybackBufferingChanged / HistoryChanged
//   (playback_events.h) — the track/buffering events are the reactor channel for future direct
//   UI/remote subscribers (D10+); App no longer subscribes to them (it reads accessors / calls
//   advance_buffering from its frame loop). HistoryChanged is consumed by App to rebuild the history
//   tree. play_mode is a setting kept in App, passed into each call.
//   (D8/D9 — UI-decoupling M1.)
#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

#include "panicast/core/types.h" // AppMode, PlaylistItem, PlayMode, TreeNodePtr
#include "panicast/domain/media.h" // D14-2: now_playing() returns Media
#include "panicast/playback/mpv_controller.h"

namespace panicast
{

// Forward declarations — injected late via attach() (pointers only, to avoid heavy includes here).
class ThreadPool;

class PlaybackService {
public:
    explicit PlaybackService(MPVController &player) : player_(player) {}

    // ── Action handling (D8a) ────────────────────────────────────────────────
    // Subscribe to playback Actions on the EventBus (PlayPause / VolumeUp / VolumeDown).
    void init();
    // Unsubscribe (optional — the app exits via _exit, so this is mainly for completeness/tests).
    void shutdown();

    // ── Queue state access (D8b-1) ───────────────────────────────────────────
    // The implicit playlist = siblings (peers) of the playing episode. Guarded by
    // playlist_mutex() — callers lock it before touching playlist()/current_index()/
    // shuffle_queue(), exactly as the pre-refactor code locked playlist_mutex_.
    std::mutex &playlist_mutex() {
        return playlist_mutex_;
    }
    std::vector<PlaylistItem> &playlist() {
        return current_playlist_;
    }
    const std::vector<PlaylistItem> &playlist() const {
        return current_playlist_;
    }
    int current_index() const {
        return current_index_;
    }
    void set_current_index(int idx) {
        current_index_ = idx;
    }
    std::deque<int> &shuffle_queue() {
        return shuffle_queue_;
    }

    // N04-fix: clear the implicit peer playlist while keeping the current track playing.
    //   The mpv handle is untouched (the current track keeps playing); on_playback_ended()
    //   sees an empty playlist and returns without advancing, so auto-advance stops.
    // Locks playlist_mutex_ internally.
    void clear_playlist();
    // Keep shuffle_queue_ at 3 entries (pre-generated upcoming random indices).
    // NOT self-locking — caller must hold playlist_mutex_ (matches original contract).
    void refill_shuffle_queue();

    // ── Track state (D9-2, moved out of App) ─────────────────────────────────
    // The "what's playing" runtime handles, now service-owned (single writer = this service, in
    //   play_current / on_playback_ended). Read-only accessors for the UI draw loop, 'N' jump-back,
    //   remote state cache and ASR. playback_node() may return nullptr (nothing playing yet).
    TreeNodePtr playback_node() const {
        return playback_node_;
    }
    AppMode playback_mode() const {
        return playback_mode_;
    }

    // D14-2: canonical now-playing identity + view. Derives from playback_node_ (the authoritative
    //   source node) so consumers read the canonical SOURCE url (node->url), NOT mpv's played path
    //   (a local filesystem path for cached items — the bug D14-3 fixes across the read side).
    //   is_video = the per-track flag (current_playlist_[idx].is_video). Returns an invalid Media
    //   (empty url) when nothing is playing. UI-thread-only state (same writer/thread as playback_node_).
    Media now_playing() const;

    // ── Buffering state (D9-3, moved out of App) ──────────────────────────────
    // Per-frame buffering-lifecycle tick (called from App's run loop with mpv's has_media). Owns
    //   playback_pending_(_start_): a just-started track is "pending" until mpv reports it loaded
    //   (or 30s timeout). Returns true while still pending (→ App shows BUFFERING); false once loaded
    //   (→ App derives PLAYING/PAUSED from mpv) or idle/timed-out (→ BROWSING). Logs the buffering
    //   duration on the pending→loaded transition and on the 30s timeout.
    bool advance_buffering(bool mpv_has_media);

    // ── Playback / autoplay logic (D8b-2, moved from app_playback.cpp) ────────
    // Inject the ThreadPool declared after playback_ in App (it can't be a construction-time ref).
    //   Must be called once before any playback (App::run wires it right after playback_.init()).
    //   D11-1: the SubtitleService param is GONE — subtitle is fully event-driven now (SubtitleService
    //   subscribes PlaybackTrackChanged → begin_track + PlaybackTrackEnded → stop_realtime), so
    //   PlaybackService no longer references SubtitleService at all.
    void attach(ThreadPool &pool);
    // Play a single item by index. mode = active App mode (IPTV flag); play_mode = loop setting.
    void play_current(int idx, AppMode mode, PlayMode play_mode);
    // Pointer-driven auto-advance (runs on the UI thread — D4 invariant). Advances current_index_
    //   per play_mode and plays the next track inline (must NOT call play_current: playlist_mutex_
    //   is non-recursive, and this method already holds it).
    void on_playback_ended(int reason, AppMode mode, PlayMode play_mode);

private:
    MPVController &player_;
    std::vector<std::size_t> subs_;

    // ── Action handlers (D8a) ────────────────────────────────────────────────
    void on_play_pause();
    void on_volume_up();
    void on_volume_down();

    // ── Queue state (D8b-1) ──────────────────────────────────────────────────
    std::mutex playlist_mutex_;
    std::vector<PlaylistItem> current_playlist_;
    int current_index_ = -1;        // playing pointer (index into current_playlist_)
    std::deque<int> shuffle_queue_; // SHUFFLE lookahead (pre-generated upcoming indices)

    // ── Track state (D9-2, moved out of App) ─────────────────────────────────
    TreeNodePtr playback_node_;              // source node of the playing item (INFO title, ASR)
    AppMode playback_mode_ = AppMode::RADIO; // mode when playback started (for N = jump-to-playing)
    bool now_playing_is_video_ = false;      // D14-2: is_video for now_playing() (TreeNode has no such field)

    // ── Buffering state (D9-3, moved out of App) ─────────────────────────────
    bool playback_pending_ = false;
    std::chrono::steady_clock::time_point playback_pending_start_;
    // Single funnel for a buffering-state change: write playback_pending_(_start_) + publish
    //   PlaybackBufferingChanged (the reactor channel). Called from play_current / on_playback_ended
    //   / advance_buffering.
    void set_buffering_(bool pending);

    // Pick one random index in [0, size) avoiding `avoid` when possible.
    int random_peer_index(int avoid) const;

    // ── Playback-logic helpers (D8b-2) ───────────────────────────────────────
    void record_play_history(const std::string &url, const std::string &title, int duration);
    // F23: standalone YouTube `-g` resolve (pool-safe). Y09 1A: returns 1-2 stream URLs (DASH=2).
    // Stateless (uses IniConfig / yt-dlp / Paths only) → const.
    std::vector<std::string> resolve_youtube_url(const std::string &url, bool has_video) const;
    // stream-fix (2026-08-16): map an episode source URL to its final CDN URL (resolve the
    //   tracker redirect chain once via Network::resolve_redirects, with a validated session
    //   cache) so mpv never re-walks the chain. Falls back to the source URL on any failure —
    //   never worse than the old direct play. Pool-thread only (does network I/O).
    std::string resolve_stream_url_(const std::string &orig_url);

    // ── Injected deps (D8b-2) ────────────────────────────────────────────────
    // Null until attach(); dereferenced only after App::run has wired it.
    // D11-1: subtitle coupling is fully event-driven (SubtitleService subscribes the playback
    //   events), so the SubtitleService pointer is GONE — only the ThreadPool remains (record_play_
    //   history + the async YouTube resolve submit to it).
    ThreadPool *pool_ = nullptr;
};

} // namespace panicast
