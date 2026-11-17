// Playback events — the OUTPUT-side (Core→UI) playback messages on the EventBus (D9).
//   PlaybackService publishes these when its state changes; App subscribes today (updating the
//   runtime handles the UI still reads), and the UI / remote will subscribe directly in later
//   increments. These replace the D8b-2 attach() callback seam: the message bus is now the only
//   service→App channel for playback state (M1: "bus = the only channel").
//
//   EventBus dispatch is SYNCHRONOUS on the publisher's thread, so replacing the callbacks with
//   these events preserves threading & ordering exactly (play_current / on_playback_ended publish
//   on the UI thread; record_play_history publishes HistoryChanged on a pool thread).
#pragma once

#include "panicast/core/types.h" // AppMode, TreeNodePtr

namespace panicast
{

// A new track is the playing pointer: its source tree node + the App mode active when playback
//   started (saved so 'N' can jump back to that mode), + the per-track has_video flag. Replaces
//   set_playback_node_ + set_playback_mode_ (which always fired back-to-back).
//   D10-3 Step 2: has_video is the re-identified A/B flag (Method A mpv-render vs Method B LYRIC),
//   computed by PlaybackService as `is_youtube || URLClassifier::is_video(url)` — NOT node->is_video
//   (those two differ). SubtitleService subscribes → begin_track(node, has_video): the FIRST real
//   consumer of this reactor channel. Default-init so the {node,mode} aggregate inits still compile.
struct PlaybackTrackChanged {
    TreeNodePtr node;
    AppMode mode;
    bool has_video = false;
};

// A track is ending / being superseded — published at every track boundary where real-time ASR must
//   NOT carry across: on_playback_ended entry (a track ended) and play_current entry (the previous
//   track is replaced by a manual play). Carries no payload — the NEXT track, if any, arrives via
//   PlaybackTrackChanged. D11-1: SubtitleService subscribes → stop_realtime(), so the LAST direct
//   PlaybackService→SubtitleService call is gone — playback no longer knows about subtitles at all.
struct PlaybackTrackEnded {};

// BUFFERING pending flag toggled. pending=true → a track is loading (the subscriber stamps a fresh
//   start time, used by the app_run state machine to derive BUFFERING + timeout); pending=false →
//   error/stop, back to BROWSING. Replaces set_pending_(bool).
struct PlaybackBufferingChanged {
    bool pending;
};

// Playback history DB changed (a track was recorded). The subscriber rebuilds the HISTORY tree
//   async. Replaces on_history_changed_(). Published on a pool thread.
struct HistoryChanged {};

} // namespace panicast
