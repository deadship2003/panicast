// MPV playback init-options layer — extracted implementation unit (D47 god-object split).
//   apply_mpv_options_() applies all user-configurable mpv options (vo/vid/ao/ytdl-format/
//   keep-open/subtitle/slang/audio-display/tls/cache/user-agent) read from the [mpv]
//   section of IniConfig, with CLI overrides taking precedence. It remains an MPVController
//   member (takes the ctx handle as a parameter + reads the cli_*_override_ statics); only its
//   Declaration stays in mpv_controller.h. Mechanical verbatim move from initialize() (mpv_controller.cpp).
#include "panicast/playback/mpv_controller.h"

#include <cstdlib> // getenv, setenv (WSLg wayland remap)
#include <string>
#include <unistd.h> // access (WSLg wayland remap)

#include <fmt/format.h>

#include "panicast/config/ini_config.h"
#include "panicast/core/logger.h"

namespace panicast
{

// D50 (vo-fix): WSLg wayland-socket remap. On the Debian 13 (trixie) upgrade the systemd
//   user session owns XDG_RUNTIME_DIR (/run/user/<uid>) while WSLg's wayland socket stays in
//   /mnt/wslg/runtime-dir — nothing bridges it, so libwayland cannot resolve $WAYLAND_DISPLAY
//   and every wayland vo (wlshm/dmabuf-wayland/gpu-on-wayland) fails VO_INIT_FAILED: audio
//   plays, no video window (mpv 0.40 logs only "Error opening/initializing the selected
//   video_out (--vo) device"). libwayland accepts an absolute socket path in WAYLAND_DISPLAY,
//   so when the configured name is missing under XDG_RUNTIME_DIR but present under WSLg's
//   runtime dir, repoint it. No-op when the socket resolves normally (native wayland, fixed
//   WSLg bridging) or WAYLAND_DISPLAY is unset/absolute.
static void remap_wslg_wayland_socket_() {
    const char *wd = std::getenv("WAYLAND_DISPLAY");
    if (!wd || !wd[0] || wd[0] == '/')
        return;
    const char *xdg = std::getenv("XDG_RUNTIME_DIR");
    std::string dir = (xdg && xdg[0]) ? xdg : "";
    while (!dir.empty() && dir.back() == '/')
        dir.pop_back(); // trim trailing '/' — keep the log path clean ('/run/user/1000/wayland-0')
    std::string cur = dir.empty() ? std::string(wd) : dir + "/" + wd;
    if (::access(cur.c_str(), F_OK) == 0)
        return; // resolves fine — leave the session untouched
    std::string wslg = std::string("/mnt/wslg/runtime-dir/") + wd;
    if (::access(wslg.c_str(), F_OK) == 0) {
        LOG(fmt::format("[MPV] WSLg: wayland socket '{}' not found under XDG_RUNTIME_DIR; "
                        "remapping WAYLAND_DISPLAY to '{}'",
                        cur, wslg));
        ::setenv("WAYLAND_DISPLAY", wslg.c_str(), 1);
    }
}

// D47: apply all [mpv]-section IniConfig options to the context before mpv_initialize.
//   CLI overrides (--vo/--vid/--ao) take precedence over INI values. Extracted verbatim from
//   initialize() — single concern: "apply user-configurable mpv options". No control flow leaves
//   this method (no early return), so calling it once between the fixed-behavior flags and
//   mpv_initialize() is behavior-identical to the original inline block.
void MPVController::apply_mpv_options_(mpv_handle *ctx) {
    // D50: repair the WSLg wayland socket path before any vo is chosen (see helper above).
    remap_wslg_wayland_socket_();
    // F24: vo/vid/ytdl-format/cache/demuxer/tls/user-agent/keep-open from [mpv] section of IniConfig.
    //   CLI overrides (--vo, --vid, --ao, --quiet = --vo=null --vid=no) take precedence over INI values.
    std::string mpv_vo = IniConfig::instance().get_mpv_vo();
    std::string mpv_vid = IniConfig::instance().get_mpv_vid();
    // AO-AUTO: empty ao = mpv's own auto-detection (pipewire→pulse→alsa…). We only
    //   pass --ao when the user pinned something; pinning also survives F40-era configs
    //   that stored "pulse,alsa" explicitly (still honoured as-is).
    std::string mpv_ao = IniConfig::instance().get_mpv_ao();
    if (!cli_vo_override_.empty())
        mpv_vo = cli_vo_override_;
    if (!cli_vid_override_.empty())
        mpv_vid = cli_vid_override_;
    if (!cli_ao_override_.empty())
        mpv_ao = cli_ao_override_;
    // D50 (vo-fix): remember the effective vo/vid/ytdl-format — play_video() re-asserts them
    //   before every loadfile so the -15 audio-only fallback never latches across tracks.
    init_vo_ = mpv_vo;
    init_vid_ = mpv_vid;
    mpv_set_option_string(ctx, "vo", mpv_vo.c_str());
    mpv_set_option_string(ctx, "vid", mpv_vid.c_str());
    // Hardware decoding ([mpv] hwdec, default auto-safe; empty = leave mpv's
    // own default "no" — embedders that want GPU decode set it explicitly).
    {
        std::string mpv_hwdec = IniConfig::instance().get("mpv", "hwdec", "");
        if (!mpv_hwdec.empty())
            mpv_set_option_string(ctx, "hwdec", mpv_hwdec.c_str());
    }
    if (!mpv_ao.empty())
        if (!mpv_ao.empty())
            mpv_set_option_string(ctx, "ao", mpv_ao.c_str()); // empty = leave mpv default (auto)
    std::string mpv_ytdl_format = IniConfig::instance().get_mpv_ytdl_format();
    init_ytdl_format_ = mpv_ytdl_format; // D50: snapshot for play_video() re-assertion
    mpv_set_option_string(ctx, "ytdl-format", mpv_ytdl_format.c_str());
    mpv_set_option_string(ctx, "keep-open",
                          IniConfig::instance().get_mpv_keep_open() ? "yes" : "no");
    // Y14: subtitle settings now INI-configurable ([mpv] sub_align_x/y, sub_visibility, sub_ass_override).
    //   Defaults: center/bottom/yes/auto. For VIDEO: subtitles render in the video window (mpv native).
    //   For AUDIO (no video window): TUI lyric panel shows sub-text (gated by !has_video in draw_info).
    mpv_set_option_string(ctx, "sub-ass-override",
                          IniConfig::instance().get_mpv_sub_ass_override().c_str());
    mpv_set_option_string(ctx, "sub-align-x", IniConfig::instance().get_mpv_sub_align_x().c_str());
    mpv_set_option_string(ctx, "sub-align-y", IniConfig::instance().get_mpv_sub_align_y().c_str());
    mpv_set_option_string(ctx, "sub-visibility",
                          IniConfig::instance().get_mpv_sub_visibility().c_str());
    // Y24.43: prefer English among multiple embedded subtitle tracks (slang). mpv auto-selects the
    //   matching track → sub-text reflects the English subtitle. INI-configurable, default "en".
    mpv_set_option_string(ctx, "slang", IniConfig::instance().get_mpv_sub_lang().c_str());
    // Y12: audio-display=no — don't open a video window for embedded cover art / pictures when
    //   playing audio files (mp3 with album art). Keeps audio playback windowless (no art popup).
    mpv_set_option_string(ctx, "audio-display", "no");
    LOG(fmt::format("[MPV] Init: vo={}, vid={}, ao={}, ytdl-format={}, keep-open={}", mpv_vo,
                    mpv_vid, mpv_ao.empty() ? "auto" : mpv_ao, mpv_ytdl_format,
                    IniConfig::instance().get_mpv_keep_open() ? "yes" : "no"));

    // Proxy: NONE, by design. mpv is playback-only; the network is the user's concern (e.g. a
    //   transparent proxy). The app's own network front (curl downloads / yt-dlp resolves /
    //   parsers) routes through [network] proxy via ProxyManager; the URLs mpv receives are
    //   already proxy-resolved direct stream URLs. Setting http-proxy or http_proxy/https_proxy
    //   env vars here would leak the app proxy into mpv's stream fetches (Y24.53 behavior,
    //   removed 2026-08) — geo-blocked streams are the user's transparent-proxy problem, not mpv's.

    // F24: TLS verification from [mpv] section (default true, aligned with libcurl configuration)
    bool mpv_tls_verify = IniConfig::instance().get_mpv_tls_verify();
    mpv_set_option_string(ctx, "tls-verify", mpv_tls_verify ? "yes" : "no");
    LOG(fmt::format("[MPV] TLS verify: {} (streaming {})", mpv_tls_verify ? "enabled" : "disabled",
                    mpv_tls_verify ? "secure" : "compatibility mode"));

    // F24: Network buffer configuration from [mpv] section — optimize streaming playback under VPN/high-latency
    std::string mpv_cache = IniConfig::instance().get_mpv_cache();
    std::string mpv_demuxer_max_bytes = IniConfig::instance().get_mpv_demuxer_max_bytes();
    std::string mpv_demuxer_max_back_bytes = IniConfig::instance().get_mpv_demuxer_max_back_bytes();
    int mpv_cache_secs = IniConfig::instance().get_mpv_cache_secs();
    mpv_set_option_string(ctx, "cache", mpv_cache.c_str());
    mpv_set_option_string(ctx, "demuxer-max-bytes", mpv_demuxer_max_bytes.c_str());
    mpv_set_option_string(ctx, "demuxer-max-back-bytes", mpv_demuxer_max_back_bytes.c_str());
    mpv_set_option_string(ctx, "cache-secs", std::to_string(mpv_cache_secs).c_str());
    LOG(fmt::format("[MPV] Network buffer: cache={}, demuxer-max-bytes={}, "
                    "demuxer-max-back-bytes={}, cache-secs={}",
                    mpv_cache, mpv_demuxer_max_bytes, mpv_demuxer_max_back_bytes, mpv_cache_secs));

    // Wedge-fix (2026-08-16): bound stream-open/read stalls. A half-open CDN connection (proxy
    //   black-hole) under mpv's default 60s network-timeout holds the core hostage — measured
    //   2026-08-16 11:20: a dead megaphone stream blocked EVERY queued loadfile (local files
    //   included, 8-20s each) for exactly 60.1s until the timeout fired. 20s caps the wedge below
    //   the app's 30s BUFFERING timeout; it is per network operation (connect/response wait), so
    //   slow-but-alive streams are unaffected. Not INI-exposed — playback robustness, same class
    //   as the cache options above.
    mpv_set_option_string(ctx, "network-timeout", "20");
    LOG("[MPV] network-timeout: 20s (bounded stream-open stalls)");

    // F24: browser User-Agent from [mpv] section (some CDNs reject default mpv UA).
    //   Applied before mpv_initialize (option-string form). This complements the yt-dlp -g fallback
    //   in the event_loop for sites that still reject the player UA.
    std::string mpv_user_agent = IniConfig::instance().get_mpv_user_agent();
    mpv_set_option_string(ctx, "user-agent", mpv_user_agent.c_str());
    LOG(fmt::format("[MPV] user-agent: {}", mpv_user_agent));
}

} // namespace panicast
