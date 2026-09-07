// Y24.7: SubtitleManager implementation.
#include "panicast/subtitle/subtitle_manager.h"

#include <filesystem>
#include <fstream>

#include <fmt/format.h>

#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/core/thread_pool.h"
#include "panicast/config/ini_config.h"
#include "panicast/net/network.h"
#include "panicast/parsers/transcript_parser.h" // TranscriptParser::load (facade → registry)
#include "panicast/subtitle/transcription_engine.h" // D11-3a: TranscriptionEngine::transcript_path (find_local_subtitle)
#include "panicast/ui/frontend.h" // D12-3b: poll(IFrontend&) — set_transcript/set_lyric_bar_active are on the contract

namespace panicast
{
namespace fs = std::filesystem;

// Infer the subtitle type ("json"/"srt"/"vtt"/"lrc") from a <podcast:transcript type="..."> MIME
//   ("application/json" → json, "text/vtt" → vtt, "application/srt" → srt), else by URL extension.
static std::string infer_type(const std::string &mime_or_type, const std::string &url) {
    std::string t = mime_or_type;
    // normalize to lowercase
    for (auto &c : t)
        c = (char)std::tolower((unsigned char)c);
    if (t.find("json") != std::string::npos)
        return "json";
    if (t.find("vtt") != std::string::npos)
        return "vtt";
    if (t.find("srt") != std::string::npos || t.find("subrip") != std::string::npos)
        return "srt";
    if (t.find("lrc") != std::string::npos)
        return "lrc";
    // Y24.9: detect omny "TextWithTimestamps" by the URL query param (no MIME type / extension).
    std::string ul = url;
    for (auto &c : ul)
        c = (char)std::tolower((unsigned char)c);
    if (ul.find("format=textwithtimestamps") != std::string::npos)
        return "twt";
    return SubtitleParserRegistry::hint_for_url(url); // fall back to extension
}

// Y24.9: format priority — prefer versatile formats (VTT/SRT work for both Method A video and
//   Method B audio; JSON/LRC/twt only Method B; unknown = unparseable). Used to pick the best
//   <podcast:transcript> URL when a feed offers several.
static int type_rank(const std::string &t) {
    if (t == "vtt")
        return 4;
    if (t == "srt")
        return 3;
    if (t == "json")
        return 2;
    if (t == "lrc")
        return 1;
    if (t == "twt")
        return 1; // parseable (omny fallback) but not mpv-native
    return 0;     // unknown
}

// File extension for a sidecar of the given type ("json"→".json", …; default ".transcript").
static const char *ext_for_type(const std::string &type) {
    if (type == "json")
        return ".json";
    if (type == "srt")
        return ".srt";
    if (type == "vtt")
        return ".vtt";
    if (type == "lrc")
        return ".lrc";
    return ".transcript";
}

// First existing sidecar path next to `local_file` (tries known extensions), or "".
static std::string find_sidecar(const std::string &local_file) {
    if (local_file.empty())
        return "";
    std::string base = local_file;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos)
        base = base.substr(0, dot);
    std::error_code ec;
    for (const char *ext : {".json", ".srt", ".vtt", ".lrc", ".transcript"}) {
        std::string p = base + ext;
        if (fs::exists(p, ec))
            return p;
    }
    return "";
}

// D11-3a: unified local-subtitle finder. ASR transcripts live under
//   <data_dir>/transcripts/<djb2-hex(url)>.srt (the XDG app-data dir — no longer next to the media
//   in ~/Downloads), so the ASR SRT is checked there FIRST, then a same-name sidecar next to
//   local_file (any ext, via find_sidecar — for online 📜 sidecars). Single source of truth for
//   probe_sidecar/load_async/resolve_subtitle_source, so the "local subtitle first" policy has one lookup.
std::string SubtitleManager::find_local_subtitle(TreeNodePtr node) {
    if (!node)
        return "";
    std::string srt = TranscriptionEngine::transcript_path(node->url);
    std::error_code ec;
    if (fs::exists(srt, ec))
        return srt;
    return find_sidecar(node->local_file);
}

// ── RSS detection ──
// Y24.9: a feed may offer several <podcast:transcript> tags (e.g. omny: srt + vtt + TextWithTimestamps).
//   Keep the HIGHEST-priority one (vtt>srt>json>lrc>twt>unknown), don't blindly overwrite — otherwise
//   the last tag (often an unparseable format) wins and the episode shows 📜 but loads 0 segments.
void SubtitleManager::detect_from_rss(TreeNodePtr node, const std::string &url,
                                      const std::string &type) {
    if (!node || url.empty())
        return;
    std::string new_type = infer_type(type, url);
    int new_rank = type_rank(new_type);
    if (node->has_subtitle && !node->subtitle_url.empty()) {
        int cur_rank = type_rank(node->subtitle_type);
        if (new_rank <= cur_rank) {
            // Keep the current (higher-or-equal priority) URL; skip this lower-priority one.
            LOG(fmt::format(
                "[Subtitle] RSS skip: {} (type={}, rank={}) — keep {} (type={}, rank={})", url,
                new_type.empty() ? "auto" : new_type, new_rank, node->subtitle_url,
                node->subtitle_type.empty() ? "auto" : node->subtitle_type, cur_rank));
            return;
        }
    }
    node->subtitle_url = url;
    node->has_subtitle = true;
    node->subtitle_type = new_type;
    LOG(fmt::format("[Subtitle] RSS detected: {} (type={}, kept)", url,
                    new_type.empty() ? "auto" : new_type));
}

// ── Local sidecar probe ──
std::string SubtitleManager::probe_sidecar(TreeNodePtr node) {
    // Y24.25: probe_sidecar does NOT set has_subtitle (that's only for online 📜 from detect_from_rss).
    //   If a local sidecar is found and subtitle_url is empty → set has_asr_srt (📝) + subtitle_url
    //   (for loading). has_subtitle stays false (no online transcript).
    if (!node)
        return "";
    std::string sc = find_local_subtitle(node); // D11-3a: unified (download-dir + adjacent)
    if (!sc.empty() && node->subtitle_url.empty()) {
        node->subtitle_url = sc;  // for load_async to pick up
        node->has_asr_srt = true; // 📝 (ASR source)
        node->asr_srt_path = sc;
        node->subtitle_type = SubtitleParserRegistry::hint_for_url(sc);
        LOG(fmt::format("[Subtitle] local sidecar detected for '{}': {} (source=asr)", node->title,
                        sc));
    }
    return sc;
}

// Y24.8: fully-async subtitle load. The whole chain (probe sidecar → fetch → parse → status) runs
//   in the pool so the caller (play_current) can invoke player.play() IMMEDIATELY — no synchronous
//   fs::exists / network in the play path. Emits clear Method-B wording + the result (with failure
//   reason) to both the log file and the LOG area (EVENT_LOG).
void SubtitleManager::load_async(TreeNodePtr node, ThreadPool &pool) {
    // Y24.18: SYNC reset before the pool task — clear the previous track's transcript IMMEDIATELY
    //   so the LYRIC panel doesn't show stale lyrics (old segments matched to the new track's
    //   time_pos) during the LOADING gap. poll() picks up pending_ready_ next frame → set_transcript
    //   (empty, new url) → clears current_transcript_ + lyric_history_. The pool task later fills
    //   pending_ with the real segments (or NONE) + pending_ready_ again.
    force_log_ = true; // per-track: re-announce status on next poll
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_.clear();
        pending_url_ = node ? node->url : std::string();
        pending_ready_ = true;
        set_status_locked(TranscriptStatus::LOADING);
    }
    pool.submit([this, node]() {
        probe_sidecar(node); // picks up local ASR SRT if present (sets subtitle_url + has_asr_srt)
        // Y24.25: load if has_subtitle (online 📜) OR has_asr_srt (local ASR 📝) + subtitle_url set.
        if (!node || (!node->has_subtitle && !node->has_asr_srt) || node->subtitle_url.empty()) {
            LOG(fmt::format("[Subtitle] panicast: no transcript for '{}'",
                            node ? node->title : std::string("<null>")));
            EVENT_LOG("No subtitle for this track");
            std::lock_guard<std::mutex> lk(mtx_);
            pending_.clear();
            pending_url_ = node ? node->url : std::string();
            pending_ready_ = true;
            set_status_locked(TranscriptStatus::NONE);
            return;
        }
        // Prefer a local sidecar (fast, no network) over the online URL.
        std::string src = node->subtitle_url;
        std::string sidecar = find_local_subtitle(node); // D11-3a: unified (download-dir + adjacent)
        bool is_local = false;
        if (!sidecar.empty()) {
            src = sidecar;
            is_local = true;
        }
        std::string fname = src;
        size_t slash = fname.find_last_of("/\\");
        if (slash != std::string::npos)
            fname = fname.substr(slash + 1);

        LOG(fmt::format("[Subtitle] panicast resolves: {} (Method B — {})", fname,
                        is_local ? "local sidecar" : "fetching online"));
        EVENT_LOG(fmt::format("Loading subtitle: {}", fname));
        {
            std::lock_guard<std::mutex> lk(mtx_);
            set_status_locked(TranscriptStatus::LOADING);
        }

        // Fetch (online via curl, or local file read) — capture failure reason at each step.
        std::string content, fail_reason;
        if (src.rfind("http", 0) == 0) {
            content = Network::fetch(src);
            if (content.empty())
                fail_reason = "online fetch returned empty (network/HTTP/proxy?)";
        } else {
            std::ifstream f(src, std::ios::binary);
            if (!f)
                fail_reason = "sidecar file not readable";
            else
                content.assign((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        }
        if (fail_reason.empty() && content.empty())
            fail_reason = "empty content";

        // Parse via the registry (Json/Srt/Vtt/Lrc/TextWithTimestamps).
        std::vector<TranscriptSegment> segs;
        if (fail_reason.empty()) {
            // Y24.9: use the RSS-captured subtitle_type as the parse hint (e.g. "twt"); for a local
            //   sidecar, infer from its extension instead (the sidecar may differ from the RSS type).
            std::string hint =
                is_local ? SubtitleParserRegistry::hint_for_url(src)
                         : (node->subtitle_type.empty() ? SubtitleParserRegistry::hint_for_url(src)
                                                        : node->subtitle_type);
            segs = SubtitleParserRegistry::instance().parse(content, hint);
            LOG(fmt::format("[Subtitle] parsed {} segments ({} bytes, type={})", segs.size(),
                            content.size(), hint.empty() ? "auto" : hint));
            if (segs.empty())
                fail_reason = "parsed 0 segments (format unrecognized or empty transcript)";
        }

        if (!fail_reason.empty()) {
            LOG(fmt::format("[Subtitle] load failed: {} ({})", fail_reason, src));
            EVENT_LOG(fmt::format("Subtitle load failed: {}", fail_reason));
            std::lock_guard<std::mutex> lk(mtx_);
            pending_.clear();
            pending_url_ = src;
            pending_ready_ = true;
            set_status_locked(TranscriptStatus::FAILED);
            return;
        }
        LOG(fmt::format("[Subtitle] ready: {} segments from '{}'", segs.size(), src));
        EVENT_LOG(fmt::format("Subtitle loaded: {} segments", segs.size()));
        std::lock_guard<std::mutex> lk(mtx_);
        pending_ = std::move(segs);
        pending_url_ = src;
        pending_ready_ = true;
        set_status_locked(TranscriptStatus::READY);
    });
}

// Y24.18: sync reset — clear to NONE so the LYRIC panel drops the previous track's lyrics. poll()
//   picks up pending_ready_ next frame and clears current_transcript_ + lyric_history_ via
//   set_transcript(empty). Used for video Method A (mpv renders subs) / no-subtitle tracks.
void SubtitleManager::reset() {
    force_log_ = true;
    std::lock_guard<std::mutex> lk(mtx_);
    pending_.clear();
    pending_url_.clear();
    pending_ready_ = true;
    set_status_locked(TranscriptStatus::NONE);
}

// Y24.20: progressive feed (real-time transcription) — worker sets accumulated segments; poll()
//   hands them to the UI. status=READY so the LYRIC bar activates (if requested).
void SubtitleManager::set_pending(const std::vector<TranscriptSegment> &segs,
                                  const std::string &url) {
    std::lock_guard<std::mutex> lk(mtx_);
    pending_ = segs;
    pending_url_ = url;
    pending_ready_ = true;
    set_status_locked(TranscriptStatus::READY);
}

// ── UI-thread handoff + L-mode activation ──
bool SubtitleManager::poll(IFrontend &ui, bool lyric_bar_requested) {
    bool handed = false;
    TranscriptStatus st;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        st = status_;
        if (pending_ready_) {
            ui.set_transcript(pending_, pending_url_);
            pending_ready_ = false;
            handed = true;
        }
    }
    // L-mode activation: active = requested && READY. Log transitions (English, deduped).
    if (lyric_bar_requested) {
        bool should_log = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            should_log = force_log_ || (st != last_logged_);
        }
        if (should_log) {
            switch (st) {
            case TranscriptStatus::READY:
                LOG("[LYRIC] Transcript ready, LYRIC bar activated");
                EVENT_LOG("LYRIC bar activated");
                break;
            case TranscriptStatus::LOADING:
                LOG("[LYRIC] Subtitle loading, LYRIC bar will activate on completion");
                break;
            case TranscriptStatus::NONE:
                LOG("[LYRIC] No subtitle for this track");
                break;
            case TranscriptStatus::FAILED:
                LOG("[LYRIC] Subtitle load failed, LYRIC bar standing by");
                break;
            }
            std::lock_guard<std::mutex> lk(mtx_);
            last_logged_ = st;
            force_log_ = false;
        }
    }
    ui.set_lyric_bar_active(lyric_bar_requested && (st == TranscriptStatus::READY));
    return handed;
}

// ── Download sidecar (format-preserving) ──
void SubtitleManager::download_sidecar(TreeNodePtr node, const std::string &local_file,
                                       ThreadPool &pool) {
    if (!node || !node->has_subtitle || node->subtitle_url.empty())
        return;
    if (node->subtitle_url.rfind("http", 0) != 0)
        return; // only fetch remote URLs
    std::string sub_url = node->subtitle_url;
    std::string type = node->subtitle_type;
    if (type.empty())
        type = SubtitleParserRegistry::hint_for_url(sub_url);
    std::string sidecar = local_file + ext_for_type(type);
    pool.submit([sub_url, sidecar, type]() {
        std::string content = Network::fetch(sub_url);
        if (content.empty()) {
            LOG(fmt::format("[Subtitle] sidecar download empty: {}", sub_url));
            return;
        }
        std::ofstream f(sidecar, std::ios::binary);
        if (f) {
            f << content;
            f.close();
            LOG(fmt::format("[Subtitle] sidecar saved: {} (type={})", sidecar,
                            type.empty() ? "auto" : type));
        }
    });
}

// N04-fix: z/Z offset methods removed (subtitle delay now via :z/:Z mpv sub-delay only).

TranscriptStatus SubtitleManager::status() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return status_;
}

bool SubtitleManager::is_ready() const {
    return status() == TranscriptStatus::READY;
}

void SubtitleManager::force_log() {
    std::lock_guard<std::mutex> lk(mtx_);
    force_log_ = true;
}

// Requires mtx_ held. Logs only on transition (or when force_log_ was set — but force_log_ is
//   consumed in poll; here we just update last_logged_ so poll dedupes).
void SubtitleManager::set_status_locked(TranscriptStatus st) {
    status_ = st;
}

} // namespace panicast
