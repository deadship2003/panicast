// MPV playback IPTV-diagnostics layer — extracted implementation unit (D20 god-object split).
//   IPTV off-air / audio-only / slow / load-failure detection: the case-insensitive log
//   keyword matcher (log_has, file-local static — used only here) + the -13 sub-classifier
//   + per-track detection reset + the IPTV-context toggle. They remain MPVController members
//   (they read private detection state: last_log_text_/iptv_context_/the one-shot flags);
//   only their implementations live here. Declarations stay in mpv_controller.h.
//   Mechanical verbatim move from mpv_controller.cpp.
#include "panicast/playback/mpv_controller.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>

#include "panicast/config/ini_config.h"
#include "panicast/core/event_log.h"

namespace panicast
{

// Y24.55: case-insensitive substring search over the last mpv warn/error log line, used to
//   sub-classify a -13 loading-failed into 404 / 403 / 5xx / unreachable for the IPTV context message.
static bool log_has(const std::string &text, const char *needle) {
    if (!needle || !*needle)
        return false;
    std::string hay = text, nd = needle;
    for (auto &c : hay)
        c = (char)std::tolower((unsigned char)c);
    for (auto &c : nd)
        c = (char)std::tolower((unsigned char)c);
    return hay.find(nd) != std::string::npos;
}

// Y24.55: map a -13 loading-failed to an IPTV context message using the last mpv log line's keywords.
//   mpv/ffmpeg log text usually carries the HTTP status or connection error, so we can tell the user
//   WHY the load failed rather than just "loading failed". Falls back to #1 (unreachable) when no
//   keyword matches — the most common -13 cause and reasonable generic advice.
std::string MPVController::classify_iptv_load_error_() const {
    const std::string &t = last_log_text_;
    // #2 channel not found
    if (log_has(t, "404") || log_has(t, "not found"))
        return "IPTV: channel not found — 404, address invalid or removed";
    // #3 access denied
    if (log_has(t, "403") || log_has(t, "forbidden") || log_has(t, "unauthorized"))
        return "IPTV: access denied — 403, region-restricted or authorization required";
    // #4 server error
    if (log_has(t, "500") || log_has(t, "502") || log_has(t, "503") || log_has(t, "504") ||
        log_has(t, "server error") || log_has(t, "service unavailable"))
        return "IPTV: server error — 5xx, source unavailable; retry later";
    // #1 unreachable (connection-level)
    if (log_has(t, "refused") || log_has(t, "timed out") || log_has(t, "timeout") ||
        log_has(t, "resolve") || log_has(t, "unreachable") || log_has(t, "connection reset") ||
        log_has(t, "no route"))
        return "IPTV: server unreachable — network, DNS, or timeout; check connection or switch "
               "source";
    // generic fallback (still #1 family — address/network/access)
    return "IPTV: server unreachable — network, DNS, or timeout; check connection or switch source";
}

// Y24.55: pick the IPTV context message for an END_FILE r=4 error code. Empty = no IPTV message
//   (for codes with no useful IPTV framing, e.g. -1/-2/-3). The MPV: behavior message is emitted
//   separately and unconditionally by the END_FILE handler; this only adds the IPTV explanation.
std::string MPVController::iptv_message_for_error_(int error) const {
    switch (error) {
    case -13:
        return classify_iptv_load_error_(); // #1/#2/#3/#4 by log keywords
    case -14:
        return "IPTV: audio output init failed — no audio device; check [mpv] ao, PulseAudio, WSLg";
    case -15:
        return "IPTV: video output init failed — no display, falling back to audio";
    case -16:
        return "IPTV: empty playlist — no playable stream for this channel";
    case -17:
        return "IPTV: cannot decode stream — missing decoder; ensure ffmpeg is installed";
    default:
        return std::string(); // no IPTV framing for other codes
    }
}

void MPVController::set_iptv_context(bool on) {
    iptv_context_ = on;
}

// Y24.55: reset per-track IPTV detection state. Called at every load entry point (play/play_list/
//   play_list_from) so one-shots re-arm for each new channel and stale timing from the previous
//   track doesn't carry over.
// D20: dropped the spurious `inline` from the definition (the header declaration was never
//   inline). It only compiled before because the definition + all callers lived in one TU;
//   moving the definition to this sibling TU broke the link until definition matched the
//   (non-inline) declaration.
void MPVController::reset_iptv_detection_() {
    last_log_text_.clear();
    file_loaded_time_set_ = false;
    stuck_timing_ = false;
    offair_reported_ = false;
    audio_only_reported_ = false;
    slow_reported_ = false;
    had_playback_started_ = false;
}

// Y24.55/D46: IPTV runtime diagnostics — extracted verbatim from update_state() (D46 god-object
//   split). Detects the three states mpv does NOT surface as an END_FILE error: off-air (#5),
//   audio-only channel (#7), sustained slow rebuffering (#11). One-shot per track (re-armed in
//   reset_iptv_detection_ / FILE_LOADED). update_state runs on the event-loop thread, so these
//   members need no lock. Inputs are the property-read snapshot passed from update_state.
void MPVController::detect_iptv_states_(bool has_media_now, bool has_audio,
                                        bool has_video_track, bool idle,
                                        int64_t cache_speed, int64_t buf_pct,
                                        double buf_dur) {
    if (!(iptv_context_.load() && file_loaded_time_set_ && !offair_reported_))
        return;
    auto now = std::chrono::steady_clock::now();
    auto since_loaded_s =
        std::chrono::duration_cast<std::chrono::seconds>(now - file_loaded_time_).count();

    // #5 off-air: loaded (address OK, HTTP 200) but no media data flows — core idle, no codec,
    //   no download, buffering stuck at 0. Held continuously for offair_detect_secs (INI, default
    //   12s) before reporting, so a slow-but-working stream's initial fill isn't misread. Any
    //   data/codec arrival cancels the timer.
    bool no_data = has_media_now && idle && !has_audio && !has_video_track &&
                   cache_speed == 0 && buf_pct == 0;
    if (no_data) {
        if (!stuck_timing_) {
            stuck_timing_ = true;
            stuck_since_ = now;
        } else if (since_loaded_s >= IniConfig::instance().get_iptv_offair_detect_secs()) {
            offair_reported_ = true;
            stuck_timing_ = false;
            EVENT_LOG("IPTV: connected, no stream data — channel may be off-air; retry later");
        }
    } else {
        stuck_timing_ = false; // data arrived or playing — cancel off-air timing
    }

    // #7 audio-only: an audio codec is present and actually playing, but no video track ever
    //   appeared (after an 8s grace so a slow video-track init isn't misread). Informational.
    if (!offair_reported_ && !audio_only_reported_ && has_audio && !has_video_track && !idle &&
        buf_pct == 0 && since_loaded_s >= 8) {
        audio_only_reported_ = true;
        EVENT_LOG("IPTV: audio-only channel — no video track, playing as audio");
    }

    // #11 slow: data is flowing but the core keeps stalling (buffering in progress, <1s buffered
    //   ahead) sustained past 20s. Throttled to once per track. Distinct from #5 (which has
    //   buf_pct==0 and no data at all).
    if (!offair_reported_ && !slow_reported_ && idle && buf_pct > 0 && buf_dur < 1.0 &&
        since_loaded_s >= 20) {
        slow_reported_ = true;
        EVENT_LOG(
            "IPTV: network too slow — sustained buffering, bandwidth insufficient or unstable");
    }
}
} // namespace panicast
