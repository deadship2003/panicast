#include "panicast/app/subtitle_service.h"

#include <cctype>
#include <filesystem> // review-fix: fs::exists staleness guard (maybe_auto_asr_)
#include <string>

#include <fmt/format.h>

#include "panicast/app/playback_events.h" // D10-3 Step 2: PlaybackTrackChanged → begin_track
#include "panicast/config/ini_config.h"   // D49: [transcription] auto_asr gate
#include "panicast/core/event_bus.h"      // D10-3 Step 2: subscribe the reactor channel
#include "panicast/core/logger.h"
#include "panicast/core/thread_pool.h"
#include "panicast/net/url_classifier.h"  // D49: is_local_file — auto-ASR is local-media only
#include "panicast/playback/mpv_controller.h"
#include "panicast/storage/cache.h"      // ASR-fix: CacheManager local-cache lookup (maybe_auto_asr_)
#include "panicast/storage/database.h"   // ASR-fix: episode_cache transcript reattach (begin_track)
#include "panicast/subtitle/subtitle_parser.h" // ASR-fix: SubtitleParserRegistry::hint_for_url
#include "panicast/ui/frontend.h" // D12-3b: poll(IFrontend&) — forwards to SubtitleManager::poll

namespace panicast
{

namespace {
// Y24.17 helpers shared by begin_track's subtitle handling (sync reset + async pool probe path).
//   Relocated verbatim from playback_service.cpp (D10-3 Step 1) — behaviour-equivalent.
bool is_mpv_sub_url(const std::string &url) {
    if (url.empty())
        return false;
    auto ew = [](const std::string &s, const char *suf) {
        size_t n = 0;
        while (suf[n])
            ++n;
        if (s.size() < n)
            return false;
        for (size_t i = 0; i < n; ++i)
            if ((char)std::tolower((unsigned char)s[s.size() - n + i]) !=
                (char)std::tolower((unsigned char)suf[i]))
                return false;
        return true;
    };
    return ew(url, ".vtt") || ew(url, ".srt") || ew(url, ".ass") || ew(url, ".ssa");
}
std::string basename_of(const std::string &p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}
} // namespace

void SubtitleService::init(ThreadPool &pool, MPVController &mpv) {
    // Y24.28: pass mpv for video ASR. Y24.19: whisper.cpp transcription. The engine is wired to
    //   this service's own SubtitleManager — the inter-object dependency stays internal.
    transcription_engine_.init(&subtitle_mgr_, &pool, &mpv);
    pool_ = &pool;
    mpv_ = &mpv;
    // D10-3 Step 2: subscribe to track changes → auto-load the new track's subtitle (the reactor
    //   pattern — the FIRST real consumer of the D9 PlaybackTrackChanged channel). PlaybackService
    //   publishes {node, mode, has_video} on BOTH manual play and auto-advance; begin_track
    //   re-identifies the media type from has_video and runs the A/B branch (Option B: auto-advance
    //   now follows the same path as a manual play). EventBus dispatch is synchronous on the
    //   publisher's (UI) thread, so this matches the old imperative call's threading + ordering.
    subs_.push_back(EventBus::instance().subscribe<PlaybackTrackChanged>(
        [this](const PlaybackTrackChanged &e) { begin_track(e.node, e.has_video); }));
    // D11-1: track boundaries (a track ends / is superseded) → stop any running real-time ASR so it
    //   never carries across tracks. Replaces the two direct stop_realtime() calls that were the last
    //   PlaybackService→SubtitleService coupling (stop_realtime is idempotent, so the advance path —
    //   which also fires begin_track — invoking it again is a harmless no-op).
    subs_.push_back(EventBus::instance().subscribe<PlaybackTrackEnded>(
        [this](const PlaybackTrackEnded &) { stop_realtime(); }));
}

void SubtitleService::shutdown() {
    // D10-3 Step 2: drop the EventBus subscription before the engines tear down, so the bus never
    //   invokes begin_track on a half-destroyed service.
    for (std::size_t t : subs_)
        EventBus::instance().unsubscribe(t);
    subs_.clear();
    transcription_engine_.shutdown(); // Y24.19: stop transcription dispatcher
}

void SubtitleService::poll(IFrontend &ui, bool lyric_bar_requested) {
    // Y24.7: SubtitleManager poll — handoff pending transcript to UI + offset + logs.
    subtitle_mgr_.poll(ui, lyric_bar_requested);
}

// D10-3: the methods below are the subtitle orchestration, relocated from PlaybackService. Step 2
//   made begin_track event-driven — it is now called by the PlaybackTrackChanged subscriber (not by
//   PlaybackService directly). stop_realtime stays a direct call from PlaybackService's track-end
//   sites (the residual coupling; D11 will cut it). Same objects throughout: pool_/mpv_/subtitle_mgr_
//   are the very ones App passed to init — and the same mpv PlaybackService used via player_ (there
//   is one MPVController in the system).

void SubtitleService::stop_realtime() {
    transcription_engine_.stop_realtime(); // Y24.20: realtime transcription doesn't carry across tracks
}

void SubtitleService::begin_track(TreeNodePtr node, bool has_video) {
    // ASR-fix (2026-08-15): playback nodes can lose the 📜 transcript URL while keeping the flag.
    //   Two producers of the broken state: (a) synthetic nodes (history root, search) built with
    //   only url/title/duration; (b) tree_nodes DB restore — that table persists has_subtitle but
    //   has NO subtitle_url column, so a restart-restored episode shows has_subtitle=1 with an
    //   empty URL and load_async reports "no transcript" despite episode_cache holding the correct
    //   URL. Reattach whenever the URL is missing (flag state irrelevant) — the online transcript
    //   (cheapest source) then wins over ASR without re-parsing the feed. Single row lookup
    //   (record_play_history already does DB writes on this thread).
    if (node && node->subtitle_url.empty() && !node->has_asr_srt &&
        node->url.rfind("http", 0) == 0) {
        bool db_sub = false, db_asr = false;
        std::string db_sub_url, db_asr_path;
        if (DatabaseManager::instance().get_episode_transcript_meta(node->url, db_sub, db_sub_url,
                                                                    db_asr, db_asr_path)) {
            if (db_sub && !db_sub_url.empty()) {
                node->has_subtitle = true;
                node->subtitle_url = db_sub_url;
                node->subtitle_type = SubtitleParserRegistry::hint_for_url(db_sub_url);
                LOG(fmt::format("[Subtitle] reattached 📜 from episode_cache: {}",
                                basename_of(db_sub_url)));
            } else if (db_asr && !db_asr_path.empty()) {
                node->has_asr_srt = true;
                node->asr_srt_path = db_asr_path;
                node->subtitle_url = db_asr_path;
                node->subtitle_type = "srt";
                LOG(fmt::format("[Subtitle] reattached 📝 ASR SRT from episode_cache: {}",
                                basename_of(db_asr_path)));
            }
        }
    }
    // Y24.8: subtitle handling is FULLY ASYNC for audio — play() is called immediately by the caller
    //   with no synchronous fs::exists probe (slow on /mnt/e WSL2 mounts). Method A (mpv sub-file) is
    //   only used for VIDEO (mpv renders in the video window); AUDIO always uses Method B
    //   (SubtitleManager fetches+parses async, drives LYRIC via time_pos).
    // Y24.17: VIDEO sidecar probe is ASYNC too. The sub-file URL isn't known at loadfile time, so
    //   it's added via mpv sub-add after the async probe finds it. AUDIO was already async.
    if (has_video) {
        // Y24.18: reset Method B first — video uses Method A (mpv renders) or no subtitle; either
        //   way the LYRIC panel must drop the previous (audio) track's lyrics. JSON → Method B fill
        //   below. Done sync (calling thread) so the panel clears immediately.
        subtitle_mgr_.reset();
        TreeNodePtr pn_cap = node;
        pool_->submit([this, pn_cap]() {
            LOG("[Subtitle] video sidecar probe (async)...");
            subtitle_mgr_.probe_sidecar(pn_cap);
            if (pn_cap && pn_cap->has_subtitle && !pn_cap->subtitle_url.empty()) {
                if (is_mpv_sub_url(pn_cap->subtitle_url)) {
                    mpv_->sub_add(pn_cap->subtitle_url);
                    LOG(fmt::format("[Subtitle] mpv sub-add: {} (Method A — mpv renders)",
                                    basename_of(pn_cap->subtitle_url)));
                } else {
                    LOG(fmt::format(
                        "[Subtitle] panicast resolves: {} (Method B — video, non-mpv format)",
                        basename_of(pn_cap->subtitle_url)));
                    subtitle_mgr_.load_async(pn_cap, *pool_); // fills Method B (LYRIC for JSON)
                }
            } else {
                LOG("[Subtitle] video: no local sidecar found (async)");
                maybe_auto_asr_(pn_cap, /*has_video=*/true); // D49: local file + no sub → whisper
            }
        });
    } else {
        // AUDIO: always Method B, fully async (probe+fetch+parse in pool). Never blocks play.
        //   load_async probes for a local sidecar async; if none and no transcript URL → NONE.
        subtitle_mgr_.load_async(node, *pool_);
        if (node && node->has_subtitle && !node->subtitle_url.empty())
            LOG(fmt::format("[Subtitle] panicast resolves: {} (Method B — audio, async)",
                            basename_of(node->subtitle_url)));
        // D49: play-path auto-ASR — pool task (stats the mount); starts whisper only when no
        //   cheaper source exists and the media is local. Play has already been issued above.
        TreeNodePtr pn_cap = node;
        pool_->submit([this, pn_cap]() { maybe_auto_asr_(pn_cap, /*has_video=*/false); });
    }
}

// D49: decide + start the play-path auto-ASR. MUST run in a pool task (find_local_subtitle stats
//   the WSL2 /mnt/e mount). Order mirrors resolve_subtitle_source's "local subtitle first": any cheaper
//   source (embedded track / local sidecar / online 📜) suppresses ASR. Local-file-only — the
//   realtime path fetches remote media whole before transcribing, which is a deliberate user
//   action (L), not something play should trigger implicitly.
void SubtitleService::maybe_auto_asr_(TreeNodePtr node, bool has_video) {
    if (!node)
        return;
    if (!IniConfig::instance().get_bool("transcription", "auto_asr", true))
        return;
    if (transcription_engine_.realtime_running())
        return;
    // Review-fix (2026-08-16): cheapest-first ordering. The online-📜 check is a PURE memory
    //   read (node fields) and is the most common suppressor, so it runs BEFORE any filesystem
    //   work — the old order paid the whisper PATH walk + model stat + /mnt/e sidecar probe on
    //   every track start of an episode that already had a transcript, only to bail here.
    if (node->has_subtitle && !node->subtitle_url.empty())
        return; // online 📜 transcript — the cheapest source, decided without I/O
    // Availability pre-check (quiet LOG, no EVENT_LOG popup per track — see resolve_* in the header).
    if (TranscriptionEngine::resolve_whisper_bin().empty() ||
        TranscriptionEngine::resolve_model().empty())
        return;
    // Review-fix (2026-08-16): cheaper sources win via the SAME central resolver the L key uses
    //   (D11-3a) — the previous inline copy of the embedded>sidecar>online chain could drift from
    //   resolve_subtitle_source (a new source kind or a priority change here would keep firing
    //   background whisper on tracks that already have subtitles). has_video only gates whether
    //   an embedded sub can render; the resolver's embedded check is a cached state read either way.
    if (resolve_subtitle_source(node).kind != ResolvedSubtitle::None)
        return; // embedded track / local sidecar (incl. a previous ASR SRT) — load_async picks it up
    std::string url = node->local_file.empty() ? node->url : node->local_file;
    // review-fix (2026-08-16): a node local_file / cached path can be STALE (the file was deleted
    //   outside the app — media_cache keeps the row; play_current guards its lookups with
    //   fs::exists). Passing a dead path on would make the worker attempt a download of a
    //   schemeless path (fails fast but logs a misleading "check [network] proxy" EVENT and writes
    //   a bogus mark_partial row keyed by the dead path). Mirror the play path: verify existence.
    if (URLClassifier::is_local_file(url) && !std::filesystem::exists(url))
        url = node->url; // dead local path — fall back to the source URL (re-gated below)
    if (!url.empty() && !URLClassifier::is_local_file(url)) {
        // ASR-fix (2026-08-15): play_current resolves the local cache by SOURCE url (CacheManager),
        //   but a synthetic node's local_file may be empty even though the played media IS the
        //   cached file. Mirror the play path so cached/downloaded episodes auto-ASR too.
        std::string cached = CacheManager::instance().get_local_file(node->url);
        if (!cached.empty() && std::filesystem::exists(cached))
            url = cached;
    }
    if (url.empty() || !URLClassifier::is_local_file(url))
        return; // streaming URL — stays a deliberate L press (would download the whole episode)
    transcription_engine_.start_realtime(node, url, /*is_streaming=*/false, has_video,
                                         /*auto_started=*/true);
    LOG(fmt::format("[Subtitle] auto-ASR started (local, no cheaper source): '{}'",
                    node->title));
}

// D11-3a: central "local subtitle first" resolver. Returns the first available non-ASR source so ASR is
//   only started when nothing cheaper exists. Embedded (mpv active sub) implies video; LocalSrt uses
//   the unified find_local_subtitle (download-dir + adjacent); Online is the RSS 📜 transcript.
ResolvedSubtitle SubtitleService::resolve_subtitle_source(TreeNodePtr node) {
    ResolvedSubtitle r;
    if (!node)
        return r;
    if (mpv_ && mpv_->has_active_subtitle()) {
        r.kind = ResolvedSubtitle::Embedded;
        return r;
    }
    std::string local = subtitle_mgr_.find_local_subtitle(node);
    if (!local.empty()) {
        r.kind = ResolvedSubtitle::LocalSrt;
        r.path = local;
        return r;
    }
    if (node->has_subtitle && !node->subtitle_url.empty()) {
        r.kind = ResolvedSubtitle::Online;
        r.path = node->subtitle_url;
        return r;
    }
    return r; // None → caller falls back to ASR
}

} // namespace panicast
