// INI config management: read/write ~/.config/panicast/config.ini, includes status bar color config types.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <vector>

#include "panicast/core/constants.h"
#include "panicast/core/paths.h"
#include "panicast/core/types.h"

namespace panicast
{

namespace fs = std::filesystem;

enum class StatusBarColorMode { RAINBOW, RANDOM, TIME_BRIGHTNESS, FIXED, CUSTOM };

struct StatusBarColorConfig {
    StatusBarColorMode mode = StatusBarColorMode::RAINBOW;
    int update_interval_ms = 100;
    float brightness_min = 0.5f;
    float brightness_max = 1.0f;
    bool time_adjust = true;
    std::string fixed_color = "cyan";
    int rainbow_speed = 1;
    // CUSTOM mode - user-defined color sequence
    // ncurses color code cycle, e.g. "9,11,10,14,13,15" means red/yellow/green/blue/magenta/white
    std::vector<int> custom_colors;
    // CUSTOM mode switch speed: change color every N characters
    int custom_speed = 2;
};

class IniConfig {
public:
    static IniConfig &instance();

    void load();

    std::string get(const std::string &section, const std::string &key,
                    const std::string &default_val = "") const {
        std::shared_lock<std::shared_mutex> lk(cfg_mtx_); // P2 (Y23.7): shared read
        if (data_.count(section) && data_.at(section).count(key)) {
            return data_.at(section).at(key);
        }
        return default_val;
    }

    int get_int(const std::string &section, const std::string &key, int default_val = 0) const;

    float get_float(const std::string &section, const std::string &key,
                    float default_val = 0.0f) const {
        std::string val = get(section, key);
        if (!val.empty()) {
            try {
                return std::stof(val);
            } catch (...) {
            }
        }
        return default_val;
    }

    bool get_bool(const std::string &section, const std::string &key,
                  bool default_val = false) const {
        std::string val = get(section, key);
        if (val == "true" || val == "yes" || val == "1")
            return true;
        if (val == "false" || val == "no" || val == "0")
            return false;
        return default_val;
    }

    // set + save methods (for scenarios that need to write back to INI, e.g. region persistence)
    void set(const std::string &section, const std::string &key, const std::string &value);
    // set(int)/set(bool) overloads removed (zero call sites). When needed, use set(..., std::to_string(v)).

    // Write current config back to the INI file
    // Preserve original comments/blank lines/order, only replace values of existing keys; new keys are appended at the end
    void save();

    StatusBarColorConfig get_statusbar_color_config();

    // Get search cache max count (default 1024)
    int get_search_cache_max();

    // Get podcast cache expiry in days

    // Get playback history max records (default 2048)
    int get_history_max_records();

    // Get playback history max days (default 1080 days ≈ 3 years)
    int get_history_max_days();

    // Get default search region
    std::string get_default_region();

    // Newly added config items

    // P0-2/P0-5: network security config accessors
    bool get_tls_verify() const;
    bool get_reject_unsafe_url() const;
    int get_network_timeout() const;
    // Y24.12: emit OSC 8 terminal hyperlinks for URLs in the INFO/LOG areas (so a wrapped URL
    //   is recognized as ONE link, not just the first line). Off on terminals without OSC 8 support.
    bool get_url_hyperlink() const;

    // ─── Playback config ([playback] section) ───────────────────────────────
    // Play mode persistence ([playback] mode = repeat|shuffle|cycle).
    // Single global mode, loaded at startup and written whenever the user switches mode.
    PlayMode get_play_mode() const;
    void set_play_mode(PlayMode m);

    // Right-panel INFO/LOG height split: fraction of the right panel reserved for the LOG area
    //   (the rest goes to INFO). 0.3 = LOG 30% / INFO 70% (the default). [display] section.
    float get_log_height_ratio() const;
    // F36: terminal height (incl. 3-row status bar) at/above which the INFO:LOG ratio holds.
    //   Below it, LOG is compressed 1 row per 1 row of height loss (INFO prioritized), then
    //   hidden when < 2 rows. Configurable so a different display can adjust. The app cannot
    //   lock the terminal window size — this is a degradation threshold, not a hard limit.
    int get_log_compress_height() const;
    // Y11: unified playback-state refresh interval (ms). update_state() polls codec/bitrate/
    //   network/position/... at this cadence; the INFO "Network: | Buffering:" line refreshes
    //   here too. Default 100ms (10fps); raise to lower CPU/refresh, lower for smoother INFO.
    int get_display_state_refresh_ms() const;
    // Y12: lyric panel (right panel, between INFO and LOG). Shows the current subtitle/lyric line
    //   (mpv sub-text) + recent lines, auto-scrolling. lyric=on/off; lyric_lines=rows shown.
    bool get_display_lyric() const;
    int get_display_lyric_lines() const;
    // Y24: L-mode — full-width LYRIC bar at the bottom (replaces status bar when active).
    //   lyric_bar = on/off (default off); lyric_bar_height = total rows incl. borders (default 5 → 3 lyric lines).
    bool get_display_lyric_bar() const;
    int get_display_lyric_bar_height() const;

    // ── MPV playback config ([mpv] section) ───────────────────────────────────
    std::string get_mpv_vo() const;
    std::string get_mpv_vid() const;
    std::string get_mpv_ao() const;
    std::string get_mpv_ytdl_format() const;
    std::string get_mpv_user_agent() const;
    std::string get_mpv_cache() const;
    std::string get_mpv_demuxer_max_bytes() const;
    std::string get_mpv_demuxer_max_back_bytes() const;
    int get_mpv_cache_secs() const;
    // Y24.12: audio (podcast/radio) fast-start profile — small initial buffer so long episodes
    //   start playing ASAP and stream while buffering, instead of waiting to fill cache_secs.
    //   Applied per-file in play_audio(); video restores the larger [mpv] cache in play_video().
    int get_mpv_audio_cache_secs() const;
    std::string get_mpv_audio_demuxer_max_bytes() const;
    std::string get_mpv_audio_demuxer_max_back_bytes() const;
    int get_mpv_audio_cache_pause_wait() const;
    bool get_mpv_tls_verify() const;
    bool get_mpv_keep_open() const;
    // Y14: mpv subtitle settings (INI-configurable, was hardcoded in Y13).
    //   For VIDEO: subtitles render in the video window (mpv native). For AUDIO: TUI lyric panel.
    std::string get_mpv_sub_align_x() const;
    std::string get_mpv_sub_align_y() const;
    std::string get_mpv_sub_visibility() const;
    std::string get_mpv_sub_ass_override() const;
    // Y24.43: preferred subtitle language for selecting among multiple embedded tracks (mpv slang).
    std::string get_mpv_sub_lang() const;
    // ─── YouTube config ([youtube] section) ─────────────────────────────────
    // cookies_file: the SINGLE cookies source for yt-dlp playback (no browser fallback).
    //   Resolved to an absolute path:
    //     - empty / bare "youtube_cookie.txt" → <data_dir>/youtube_cookie.txt
    //     - "~/..." → expanded with $HOME
    //     - "/abs/path" → as-is
    //     - other relative → <data_dir>/<input>
    //   The args builder injects `--cookies <path>` only when the file exists (precondition, not a
    //   fallback method). Default bare name is youtube_cookie.txt (Ctrl+B writes this key).
    std::string get_youtube_cookies_file() const;
    // player_client: yt-dlp YouTube client (comma-separated, e.g. tv_downgraded,web).
    //   tv_downgraded,web is yt-dlp's own default for cookie-authenticated requests and is the
    //   least likely to trip the "Sign in to confirm you're not a bot" check (Y05: was android,web).
    std::string get_youtube_player_client() const;
    // js_runtime: JS runtime yt-dlp uses to solve YouTube's nsig "n challenge".
    //   "quickjs" → inject `--js-runtimes quickjs` (bundled qjs ~2MB, ~10× faster cold-start
    //     than deno; RECOMMENDED). Needs the EJS solver available: install `yt-dlp[default]`
    //     (pip) which brings yt-dlp-ejs, since quickjs can't fetch EJS from npm.
    //   "deno"    → inject `--js-runtimes deno` (yt-dlp default; ~106MB binary, slower cold-start).
    //   "" (empty, key present but blank) → don't inject; yt-dlp picks its default (deno if on PATH).
    //   Y08: DEFAULT is "quickjs" when the key is ABSENT (old configs auto-use quickjs, so removing
    //     deno doesn't break nsig). Optional path form e.g. `quickjs:/opt/qjs/bin/qjs`.
    std::string get_youtube_js_runtime() const;
    // play_format_video: yt-dlp `-f` format for VIDEO playback (Y09, 1A DASH pre-resolve).
    //   Default bestvideo[height<=1080]+bestaudio = up to 1080p DASH (video+audio
    //   separate streams, mpv merges via audio-file). Needs ffmpeg. YouTube has no 1080p
    //   single-file, so 1080p REQUIRES this DASH form (2 URLs).
    std::string get_youtube_play_format_video() const;
    // play_format_audio: yt-dlp `-f` format for AUDIO-only playback. Default bestaudio
    //   (highest-quality audio stream, single URL).
    std::string get_youtube_play_format_audio() const;
    // resolve_timeout_sec: per-attempt yt-dlp `-g` timeout for playback resolution. The old fixed
    //   30s was too short: through a SOCKS proxy + quickjs/ejs nsig solving, a single resolve can
    //   legitimately take 30-60s, and the 30s cap caused intermittent "YouTube resolve failed"
    //   (seen as exact-30s YtdlpRunner timeouts in the log). Default 90s.
    int get_youtube_resolve_timeout_sec() const;
    // resolve_retries: how many times playback resolution is retried on timeout/failure before
    //   giving up. yt-dlp YouTube resolution is flaky (nsig + bot check); a retry usually
    //   succeeds. Default 3 (1 initial + 2 retries).
    int get_youtube_resolve_retries() const;
    // sub_lang: YouTube subtitle language code to load for playback (Y11). Empty = no subtitles
    //   (opt-in; soft subs are not loaded unless this is set). e.g. "en", "zh-Hans", "ja".
    //   When set, resolve_youtube_url fetches the .vtt via yt-dlp and mpv loads it as sub-file
    //   (then F/G size, z/Z sync, r/R pos, v visibility all work; mpv centers by default).
    std::string get_youtube_sub_lang() const;
    // sub_auto: when sub_lang is set but no manual subtitle exists, use auto-generated captions.
    bool get_youtube_sub_auto() const;
    // Y15: Bilibili cookies file (Netscape format, for yt-dlp --cookies). QR login writes here;
    //   user can also import a browser-exported cookies.txt. Resolved to absolute path (same
    //   logic as get_youtube_cookies_file).
    std::string get_bilibili_cookies_file() const;
    // Y24.11: T-mode — Douyin cookies file (Netscape format, for yt-dlp --cookies). TikTok user
    //   listings work anonymously; Douyin usually requires a logged-in cookies.txt (CN exit too).
    std::string get_tiktok_douyin_cookies_file() const;
    // Y24.13: T-mode — optional TikTok cookies.txt (logged-in TikTok; anonymous works without it).
    std::string get_tiktok_cookies_file() const;

    // ─── IPTV config ([iptv], Y24.50) ─────────────────────────────────────────
    // iptv-org (CC0) playlists, fetched live + cached. base_url/api_url can be pointed at a mirror.
    std::string get_iptv_base_url() const;
    std::string get_iptv_api_url() const;
    int get_iptv_cache_hours() const;
    // Custom user m3u URLs (comma-separated) shown under I-mode → Custom.
    std::string get_iptv_custom_urls() const;
    // Y24.55: seconds a connected IPTV stream may stay data-less (core-idle, no codec, no
    //   download) before panicast reports it as likely off-air. Tunable so slow links can
    //   raise it. See MPVController IPTV detection in mpv_controller.cpp.
    int get_iptv_offair_detect_secs() const;

    // ─── Remote control config ([remote] section) — N line ──────────────────
    // N01: opt-in network control. Default off so the local TUI is unaffected unless the
    //   user explicitly enables it. bind=127.0.0.1 keeps it localhost; set 0.0.0.0 for LAN.
    //   auth_token empty = no auth (LAN-only); set a token to require it on every connection.
    bool get_remote_enabled() const;
    int get_remote_port() const;
    std::string get_remote_bind() const;
    std::string get_remote_auth_token() const;
    // N04: the universal pairing PIN (default 6696) — always valid, for headless/no-display
    //   pairing. Configurable so the user can change it from the INI.
    std::string get_remote_universal_pin() const;
    // N05: UDP discovery port the APK broadcasts to in order to auto-find panicast instances.
    int get_remote_discovery_port() const;
    // N08: mini-LMS (Squeezer remote control, LMS CLI control plane) — runtime layer of
    //   the dual gate. Compile layer is the CMake PANICAST_REMOTE_LMS option (default ON);
    //   the runtime layer also defaults ON (home-LAN allowlist + optional login below
    //   bound the exposure).
    bool get_remote_lms_enabled() const;
    int get_remote_lms_port() const; // LMS CLI convention: 9090
    // Allowed source IPs, comma-separated CIDRs (bare IP = /32). Empty string = allow all.
    //   Default: home LAN + loopback only (192.168.0.0/16,127.0.0.0/8).
    std::string get_remote_lms_allow() const;
    // Login auth (LMS CLI `login <user> <pass>` / HTTP Basic): factory default
    //   panicast/panicast — auth ON out of the box. Explicitly empty lms_pass disables it.
    std::string get_remote_lms_user() const;
    std::string get_remote_player_name() const;
    std::string get_remote_lms_pass() const;

    // ─── Network proxy config ([network] proxy) ──────────────────────────────
    // Read the normalized proxy URL (empty string = disabled, use direct/transparent proxy).
    std::string get_proxy() const;
    // Normalize + validate proxy URL. Empty return means "disabled"; original string return means "invalid".
    //   Rules: lowercase the scheme; socks:// → socks5h:// (proxy resolves DNS, preventing DNS pollution/leak);
    //   accept http/https/socks4/socks4a/socks5/socks5h; must be scheme://host[:port] with no path.
    //   is_valid outputs validation result (only meaningful when input is non-empty).
    static std::string normalize_proxy(const std::string &input, bool *is_valid = nullptr);

    // ─── Node tree status color config ([colors] section) ───────────────────────────
    // Parse a color name or 0-255 numeric code into an ncurses color number (short).
    //   Names (case-insensitive): black red green yellow blue magenta cyan white default
    //   Numbers: -1=default background passthrough; 0-7=standard ANSI; 8-15=bright; 16-255=xterm256 extended colors
    //   Returns fallback for unrecognized input, so a config typo won't cause a black screen.
    static short resolve_color(const std::string &s, short fallback);
    // Read a key from the [colors] section, returning fallback if missing
    short get_node_color(const std::string &key, short fallback) const;
    // ──────────────────────────────────────────────────────────────

    // Unified URL safety check entry point (honors config toggle)
    // Inlined to avoid a forward dependency on UrlGuard
    bool is_url_safe(const std::string &url) const;

    static std::string get_config_file();

private:
    IniConfig() {}
    // Y24.16: shared cookies_file path resolution — ~ → $HOME, /abs → as-is, bare/relative →
    //   <data_dir>/<value>. Used by the youtube/bilibili/tiktok cookies getters (was 4× duplicated).
    std::string resolve_cookies_path(const std::string &section, const std::string &key,
                                     const std::string &def) const {
        std::string v = get(section, key, def);
        if (!v.empty() && v[0] == '~') {
            const char *h = std::getenv("HOME");
            if (h)
                v = std::string(h) + v.substr(1);
        }
        if (!v.empty() && v[0] == '/')
            return v;
        std::string base = Paths::get_data_dir();
        return base.empty() ? v : (base + "/" + v);
    }
    std::map<std::string, std::map<std::string, std::string>> data_;
    std::vector<std::string>
        raw_lines_; // original file lines (incl. comments/blank lines/order), preserved for save()
    mutable std::shared_mutex
        cfg_mtx_; // P2 (Y23.7): protect data_ (mpv thread reads, UI thread writes)

    void create_default(const std::string &path);
};

} // namespace panicast
