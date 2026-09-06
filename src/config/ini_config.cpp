#include "panicast/config/ini_config.h"

namespace panicast
{

IniConfig &IniConfig::instance() {
    static IniConfig ic;
    return ic;
}

namespace
{
// Atomic config write: stream to <path>.tmp, flush, then rename(2) over <path>.
//   A direct ofstream on <path> truncates it in place — a process killed mid-write
//   leaves a 0-byte file. With tmp+rename the replacement is atomic; on write
//   failure (disk full) the previous file survives untouched.
template <typename Writer> bool write_atomic(const std::string &path, Writer &&writer) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open())
            return false;
        writer(f);
        f.flush();
        if (!f.good()) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, path, ec); // same-dir rename(2) — atomic replacement
    return !ec;
}
} // namespace

// ── mpv-config getters (D24: moved out-of-line from ini_config.h) ──
std::string IniConfig::get_mpv_vo() const {
    return get("mpv", "vo", "auto");
}
std::string IniConfig::get_mpv_vid() const {
    return get("mpv", "vid", "auto");
}
std::string IniConfig::get_mpv_ao() const {
    // AO-AUTO: empty = mpv auto-detect (compiled-in order pipewire→pulse→alsa…).
    //   libpulse finds the socket via the standard $XDG_RUNTIME_DIR/pulse/native path
    //   on native hosts AND under WSLg (symlink) — no platform detection, no pinning.
    //   Pin explicitly (e.g. "pulse" / "alsa") only to override diagnosis.
    return get("mpv", "ao", "");
}
std::string IniConfig::get_mpv_ytdl_format() const {
    return get("mpv", "ytdl_format", "bestvideo+bestaudio");
}
std::string IniConfig::get_mpv_user_agent() const {
    return get("mpv", "user_agent",
               "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
}
std::string IniConfig::get_mpv_cache() const {
    return get("mpv", "cache", "yes");
}
std::string IniConfig::get_mpv_demuxer_max_bytes() const {
    return get("mpv", "demuxer_max_bytes", "50MiB");
}
std::string IniConfig::get_mpv_demuxer_max_back_bytes() const {
    return get("mpv", "demuxer_max_back_bytes", "25MiB");
}
int IniConfig::get_mpv_cache_secs() const {
    return get_int("mpv", "cache_secs", 10);
}
int IniConfig::get_mpv_audio_cache_secs() const {
    return get_int("mpv", "audio_cache_secs", 5);
}
std::string IniConfig::get_mpv_audio_demuxer_max_bytes() const {
    return get("mpv", "audio_demuxer_max_bytes", "5MiB");
}
std::string IniConfig::get_mpv_audio_demuxer_max_back_bytes() const {
    return get("mpv", "audio_demuxer_max_back_bytes", "2MiB");
}
int IniConfig::get_mpv_audio_cache_pause_wait() const {
    return get_int("mpv", "audio_cache_pause_wait", 1);
}
bool IniConfig::get_mpv_tls_verify() const {
    return get_bool("mpv", "tls_verify", true);
}
bool IniConfig::get_mpv_keep_open() const {
    return get_bool("mpv", "keep_open", true);
}
std::string IniConfig::get_mpv_sub_align_x() const {
    return get("mpv", "sub_align_x", "center");
}
std::string IniConfig::get_mpv_sub_align_y() const {
    return get("mpv", "sub_align_y", "bottom");
}
std::string IniConfig::get_mpv_sub_visibility() const {
    return get("mpv", "sub_visibility", "yes");
}
std::string IniConfig::get_mpv_sub_ass_override() const {
    return get("mpv", "sub_ass_override", "auto");
}
std::string IniConfig::get_mpv_sub_lang() const {
    return get("mpv", "sub_lang", "en");
}

// ── YouTube-config getters (D25: moved out-of-line from ini_config.h) ──
std::string IniConfig::get_youtube_cookies_file() const {
    return resolve_cookies_path("youtube", "cookies_file", "youtube_cookie.txt");
}
std::string IniConfig::get_youtube_player_client() const {
    std::string v = get("youtube", "player_client", "tv_downgraded,web");
    return v.empty() ? "tv_downgraded,web" : v;
}
std::string IniConfig::get_youtube_js_runtime() const {
    return get("youtube", "js_runtime", "quickjs");
}
std::string IniConfig::get_youtube_play_format_video() const {
    return get("youtube", "play_format_video", "bestvideo[height<=1080]+bestaudio");
}
std::string IniConfig::get_youtube_play_format_audio() const {
    return get("youtube", "play_format_audio", "bestaudio");
}
int IniConfig::get_youtube_resolve_timeout_sec() const {
    return get_int("youtube", "resolve_timeout_sec", 90);
}
int IniConfig::get_youtube_resolve_retries() const {
    return get_int("youtube", "resolve_retries", 3);
}
std::string IniConfig::get_youtube_sub_lang() const {
    return get("youtube", "sub_lang", "");
}
bool IniConfig::get_youtube_sub_auto() const {
    return get_bool("youtube", "sub_auto", true);
}

// ── cookies / IPTV / remote-control getters (D26: moved out-of-line) ──
std::string IniConfig::get_bilibili_cookies_file() const {
    return resolve_cookies_path("bilibili", "cookies_file", "bilibili_cookie.txt");
}
std::string IniConfig::get_tiktok_douyin_cookies_file() const {
    return resolve_cookies_path("tiktok", "douyin_cookies_file", "douyin_cookie.txt");
}
std::string IniConfig::get_tiktok_cookies_file() const {
    return resolve_cookies_path("tiktok", "cookies_file", "tiktok_cookie.txt");
}
std::string IniConfig::get_iptv_base_url() const {
    return get("iptv", "base_url", "https://iptv-org.github.io/iptv");
}
std::string IniConfig::get_iptv_api_url() const {
    return get("iptv", "api_url", "https://iptv-org.github.io/api");
}
int IniConfig::get_iptv_cache_hours() const {
    return get_int("iptv", "cache_hours", 24);
}
std::string IniConfig::get_iptv_custom_urls() const {
    return get("iptv", "custom_urls", "");
}
int IniConfig::get_iptv_offair_detect_secs() const {
    return get_int("iptv", "offair_detect_secs", 12);
}
bool IniConfig::get_remote_enabled() const {
    return get_bool("remote", "enable", false);
}
int IniConfig::get_remote_port() const {
    return get_int("remote", "port", 8421);
}
std::string IniConfig::get_remote_bind() const {
    return get("remote", "bind", "0.0.0.0");
}
std::string IniConfig::get_remote_auth_token() const {
    return get("remote", "auth_token", "");
}
std::string IniConfig::get_remote_universal_pin() const {
    return get("remote", "universal_pin", "6696");
}
int IniConfig::get_remote_discovery_port() const {
    return get_int("remote", "discovery_port", 18430);
}
bool IniConfig::get_remote_lms_enabled() const {
    return get_bool("remote", "lms_enable", true);
}
int IniConfig::get_remote_lms_port() const {
    return get_int("remote", "lms_port", 9090);
}
std::string IniConfig::get_remote_lms_allow() const {
    return get("remote", "lms_allow", "192.168.0.0/16,127.0.0.0/8");
}
std::string IniConfig::get_remote_lms_user() const {
    return get("remote", "lms_user", "panicast");
}
std::string IniConfig::get_remote_lms_pass() const {
    // Factory default panicast/panicast = auth ON out of the box; an explicitly EMPTY
    //   lms_pass key in the INI disables auth (key-present-but-empty wins over this default).
    return get("remote", "lms_pass", "panicast");
}

std::string IniConfig::get_remote_player_name() const {
    // Display name of the virtual player Squeezer sees. Falls back to server_name
    //   (the discovery display name) for consistency, then "panicast".
    std::string v = get("remote", "player_name", "");
    if (v.empty())
        v = get("remote", "server_name", "");
    return v.empty() ? "panicast" : v;
}

// ── misc getters: search/history/region/network/display (D27: moved out-of-line) ──
int IniConfig::get_search_cache_max() {
    return get_int("storage", "search_cache_max", 1024);
}
int IniConfig::get_history_max_records() {
    return get_int("storage", "history_max_records", 2048);
}
int IniConfig::get_history_max_days() {
    return get_int("storage", "history_max_days", 1080);
}
std::string IniConfig::get_default_region() {
    return get("search", "default_region", "US");
}
bool IniConfig::get_tls_verify() const {
    return get_bool("network", "tls_verify", true);
}
bool IniConfig::get_reject_unsafe_url() const {
    return get_bool("network", "reject_unsafe_url", true);
}
int IniConfig::get_network_timeout() const {
    return get_int("network", "timeout", 30);
}
bool IniConfig::get_url_hyperlink() const {
    return get_bool("display", "url_hyperlink", true);
}

// ── display getters ([display] section) (D28: moved out-of-line from ini_config.h) ──
float IniConfig::get_log_height_ratio() const {
    return get_float("display", "log_height_ratio", 0.3f);
}
int IniConfig::get_log_compress_height() const {
    return get_int("display", "log_compress_height", 23);
}
int IniConfig::get_display_state_refresh_ms() const {
    return get_int("display", "state_refresh_ms", 100);
}
bool IniConfig::get_display_lyric() const {
    return get_bool("display", "lyric", true);
}
int IniConfig::get_display_lyric_lines() const {
    return get_int("display", "lyric_lines", 3);
}
bool IniConfig::get_display_lyric_bar() const {
    return get_bool("display", "lyric_bar", false);
}
int IniConfig::get_display_lyric_bar_height() const {
    return get_int("display", "lyric_bar_height", 5);
}

// ── core accessors: load/save/get_int/set (D29: moved out-of-line from ini_config.h) ──
void IniConfig::load() {
    std::string path = get_config_file();
    std::error_code ec;
    // Missing OR empty → (re)generate the default template. The empty case matters:
    //   an existing-but-0-byte file (a save() interrupted by a kill) must not block
    //   regeneration, or the user is left with a key-less stub forever.
    if (!fs::exists(path, ec) || fs::is_empty(path, ec)) {
        create_default(path);
    }

    std::ifstream f(path);
    std::string line;
    std::string current_section;
    raw_lines_.clear();

    while (std::getline(f, line)) {
        // Keep original lines for writeback in save() (including comments, blank lines, original order)
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        raw_lines_.push_back(line);

        // UTF-8 BOM (generated file carries one so Windows editors auto-detect the encoding
        //   and show the bilingual comments correctly): strip it from the PARSED copy only —
        //   raw_lines_ keeps it verbatim so save() round-trips it.
        if (raw_lines_.size() == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
            line = line.substr(3);
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
        trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
            continue;

        if (trimmed[0] == '[' && trimmed.back() == ']') {
            current_section = trimmed.substr(1, trimmed.length() - 2);
            continue;
        }

        size_t pos = trimmed.find('=');
        if (pos != std::string::npos) {
            std::string key = trimmed.substr(0, pos);
            std::string value = trimmed.substr(pos + 1);

            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            if (value.length() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.length() - 2);
            }

            data_[current_section][key] = value;
        }
    }
}
int IniConfig::get_int(const std::string &section, const std::string &key, int default_val) const {
    std::string val = get(section, key);
    if (!val.empty()) {
        try {
            return std::stoi(val);
        } catch (...) {
        }
    }
    return default_val;
}
void IniConfig::set(const std::string &section, const std::string &key, const std::string &value) {
    std::unique_lock<std::shared_mutex> lk(cfg_mtx_); // P2 (Y23.7): exclusive write
    data_[section][key] = value;
    lk.unlock();
    save();
}
void IniConfig::save() {
    std::string path = get_config_file();
    if (path.empty())
        return;
    write_atomic(path, [&](std::ostream &f) {
        std::set<std::string> written; // "section\x1fkey"
        std::string current_section;
        for (const auto &raw : raw_lines_) {
            std::string line = raw;
            size_t a = line.find_first_not_of(" \t");
            if (a == std::string::npos) {
                f << raw << "\n";
                continue;
            }
            if (line[a] == '#' || line[a] == ';') {
                f << raw << "\n";
                continue;
            }
            if (line[a] == '[') {
                size_t rb = line.find(']', a);
                if (rb != std::string::npos)
                    current_section = line.substr(a + 1, rb - a - 1);
                f << raw << "\n";
                continue;
            }
            size_t eq = line.find('=', a);
            if (eq == std::string::npos) {
                f << raw << "\n";
                continue;
            }
            std::string key = line.substr(a, eq - a);
            key.erase(key.find_last_not_of(" \t") + 1);
            std::string id = current_section + "\x1f" + key;
            if (data_.count(current_section) && data_[current_section].count(key)) {
                f << key << " = " << data_[current_section][key] << "\n";
                written.insert(id);
            } else {
                f << raw << "\n";
            }
        }
        // Append new keys not present in the file
        for (const auto &[section, kv] : data_) {
            for (const auto &[k, v] : kv) {
                std::string id = section + "\x1f" + k;
                if (!written.count(id)) {
                    f << "[" << section << "]\n" << k << " = " << v << "\n";
                    written.insert(id);
                }
            }
        }
    });
}

// ── logic + static helpers: statusbar/play-mode/proxy/color/url/config-file (D30: moved out-of-line) ──
StatusBarColorConfig IniConfig::get_statusbar_color_config() {
    StatusBarColorConfig cfg;

    std::string mode_str = get("statusbar_color", "mode", "rainbow");
    if (mode_str == "random")
        cfg.mode = StatusBarColorMode::RANDOM;
    else if (mode_str == "time_brightness")
        cfg.mode = StatusBarColorMode::TIME_BRIGHTNESS;
    else if (mode_str == "fixed")
        cfg.mode = StatusBarColorMode::FIXED;
    else if (mode_str == "custom")
        cfg.mode = StatusBarColorMode::CUSTOM;
    else
        cfg.mode = StatusBarColorMode::RAINBOW;

    cfg.update_interval_ms = get_int("statusbar_color", "update_interval_ms", 100);
    cfg.brightness_min = get_float("statusbar_color", "brightness_min", 0.5f);
    cfg.brightness_max = get_float("statusbar_color", "brightness_max", 1.0f);
    cfg.time_adjust = get_bool("statusbar_color", "time_adjust", true);
    cfg.fixed_color = get("statusbar_color", "fixed_color", "cyan");
    cfg.rainbow_speed = get_int("statusbar_color", "rainbow_speed", 1);
    // Parse CUSTOM mode configuration
    std::string colors_str = get("statusbar_color", "custom_colors", "");
    if (!colors_str.empty()) {
        std::stringstream ss(colors_str);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try {
                cfg.custom_colors.push_back(std::stoi(token));
            } catch (...) {
            }
        }
    }
    cfg.custom_speed = get_int("statusbar_color", "custom_speed", 2);
    if (cfg.rainbow_speed < 1)
        cfg.rainbow_speed = 1;
    if (cfg.rainbow_speed > 10)
        cfg.rainbow_speed = 10;

    return cfg;
}
PlayMode IniConfig::get_play_mode() const {
    std::string v = get("playback", "mode", "cycle");
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (v == "repeat" || v == "r")
        return PlayMode::REPEAT;
    if (v == "shuffle" || v == "s" || v == "random")
        return PlayMode::SHUFFLE;
    return PlayMode::CYCLE; // "cycle"/"c"/default
}
void IniConfig::set_play_mode(PlayMode m) {
    const char *s = (m == PlayMode::REPEAT)    ? "repeat"
                    : (m == PlayMode::SHUFFLE) ? "shuffle"
                                               : "cycle";
    set("playback", "mode", s);
}
std::string IniConfig::get_proxy() const {
    return normalize_proxy(get("network", "proxy", ""));
}
std::string IniConfig::normalize_proxy(const std::string &input, bool *is_valid) {
    if (is_valid)
        *is_valid = true;
    std::string s = input;
    // Trim leading/trailing whitespace
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    if (s.empty())
        return ""; // proxy disabled
    // Lowercase the scheme (before "://")
    size_t pos = s.find("://");
    if (pos == std::string::npos) {
        if (is_valid)
            *is_valid = false;
        return s;
    }
    std::string scheme = s.substr(0, pos);
    for (auto &c : scheme)
        c = (char)std::tolower((unsigned char)c);
    std::string rest = s.substr(pos + 3);
    // socks:// → socks5h://
    if (scheme == "socks")
        scheme = "socks5h";
    // Validate scheme whitelist
    static const std::set<std::string> ok = {"http",    "https",  "socks4",
                                             "socks4a", "socks5", "socks5h"};
    if (!ok.count(scheme)) {
        if (is_valid)
            *is_valid = false;
        return s;
    }
    // Validate host[:port], no path (no '/')
    if (rest.empty() || rest.find('/') != std::string::npos) {
        if (is_valid)
            *is_valid = false;
        return s;
    }
    return scheme + "://" + rest;
}
short IniConfig::resolve_color(const std::string &s, short fallback) {
    std::string t = s;
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    t.erase(0, t.find_first_not_of(" \t"));
    t.erase(t.find_last_not_of(" \t") + 1);
    if (t.empty())
        return fallback;
    // D12-3a: raw ANSI/ncurses color indices (0-7) — decoupled from <ncurses.h> so this
    //   config header need not pull ncurses (see docs/ARCHITECTURE.md §2.1). Values are
    //   identical to ncurses COLOR_BLACK..COLOR_WHITE; -1 = default passthrough.
    static const std::map<std::string, short> names = {
        {"black", 0},   {"red", 1},  {"green", 2}, {"yellow", 3},  {"blue", 4},
        {"magenta", 5}, {"cyan", 6}, {"white", 7}, {"default", -1}};
    auto it = names.find(t);
    if (it != names.end())
        return it->second;
    // Pure number (negative sign allowed)
    bool numeric = !t.empty();
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        bool ok = (c >= '0' && c <= '9') || (i == 0 && c == '-');
        if (!ok) {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        try {
            int n = std::stoi(t);
            if (n >= -1 && n <= 255)
                return (short)n;
        } catch (...) {
        }
    }
    return fallback;
}
short IniConfig::get_node_color(const std::string &key, short fallback) const {
    return resolve_color(get("colors", key), fallback);
}
bool IniConfig::is_url_safe(const std::string &url) const {
    if (!get_reject_unsafe_url())
        return true; // user explicitly disabled
    // Only allow http/https protocols
    return url.size() >= 8 &&
           (url.compare(0, 8, "https://") == 0 || url.compare(0, 7, "http://") == 0);
}
std::string IniConfig::get_config_file() {
    const char *home = std::getenv("HOME");
    return home ? std::string(home) + CONFIG_DIR + "/config.ini" : "";
}

// ── create_default: auto-generated default config template (D31: moved out-of-line) ──
void IniConfig::create_default(const std::string &path) {
    write_atomic(path, [&](std::ostream &f) {
        // UTF-8 BOM first: makes Windows editors (Notepad et al.) auto-detect UTF-8 so the
        //   bilingual comments render instead of mojibake; load() strips it when parsing.
        f << "\xEF\xBB\xBF";
        f << R"(# ============================================================
# panicast Configuration File
# Version: )"
          << VERSION << R"(
# Author: Panic
# ============================================================
#
# 本配置文件由程序自动生成，包含所有可用选项 / Auto-generated; contains all available options
# 修改后重启程序生效 / Restart the program after editing for changes to take effect
#
# ============================================================
# 【界面显示配置】 / Display
# ============================================================
[display]
# 左右面板比例 (0.2-0.8) / Left:right panel ratio (0.2-0.8)
layout_ratio = 0.25
# 右侧 INFO/LOG 高度比例：LOG 区域占右侧面板高度的比例 (0.1-0.8) / Right-panel INFO/LOG split: LOG share of the right panel height (0.1-0.8)
#   0.3 = LOG 30% / INFO 70%(默认)。想让 INFO 更大就调小(如 0.25)，想让 LOG 更大就调大(如 0.4) / 0.3 = LOG 30% / INFO 70% (default). Lower (e.g. 0.25) = bigger INFO; higher (e.g. 0.4) = bigger LOG
log_height_ratio = 0.3
# LOG 压缩阈值：终端高度（含底部 3 行状态栏）低于此值时开始压缩 LOG 区域 / LOG compression threshold: when terminal height (incl. the 3-row status bar) drops below this, LOG is compressed
#   每降 1 行高度，LOG 减 1 行（INFO 保持不变，优先进度条等）；LOG<2 行时隐藏，INFO 独占右面板 / Each row of height lost: LOG shrinks by 1 (INFO preserved — progress bar prioritized); LOG hidden when < 2 rows, INFO takes the right panel
#   终端高度 ≥ 此值时按 log_height_ratio 精确渲染 / At/above this height the ratio renders exactly
#   换显示屏/分辨率可调。终端窗口大小由终端模拟器控制，程序无法锁定，此为降级阈值 / Adjustable for other displays. The window size is controlled by the terminal emulator and cannot be locked by the app — this is a degradation threshold
log_compress_height = 23
# 播放状态统一刷新间隔(ms)：codec/bitrate/网络(速度+缓冲)/进度等均按此节流刷新 / Unified playback-state refresh interval (ms): codec/bitrate/network(speed+buffer)/position all polled at this cadence
#   默认 100ms(10fps)；调高降 CPU/刷新率，调低更平滑。Network 行也按此刷新 / default 100ms (10fps); raise to lower CPU, lower for smoother INFO. The Network line refreshes here too
state_refresh_ms = 100
# 歌词面板：右侧 INFO/LOG 之间显示当前歌词行(本地 .lrc 或 YouTube 字幕)+近期行,自动滚动 / lyric panel between INFO and LOG
#   lyric = on/off(默认 on)；lyric_lines = 显示行数(默认 3,当前行高亮) / lyric=on/off(default on); lyric_lines=rows(default 3, current highlighted)
lyric = true
lyric_lines = 3
# L模式：底部全宽 LYRIC 条(激活时替代状态栏)，仅当字幕就绪(READY)时才显示 / L-mode: full-width LYRIC bar at bottom (replaces status bar when active); appears only when subtitle is READY
#   lyric_bar = on/off(默认 off)；lyric_bar_height = 总行数含边框(默认 5 → 3 行字幕,自动滚动) / lyric_bar=on/off(default off); lyric_bar_height=total rows incl borders(default 5 → 3 lyric lines, auto-scroll)
lyric_bar = false
lyric_bar_height = 5
# 是否显示树形连接线 / Show tree connector lines
show_tree_lines = true
# Y24.12: INFO/LOG 区 URL 用 OSC 8 超链接(整段折行 URL 识别为一个链接) / OSC 8 hyperlinks for URLs
#   不支持 OSC 8 的终端可关 / Off on terminals without OSC 8 support
url_hyperlink = true
# 是否启用标题滚动显示 / Enable scrolling title display
scroll_mode = false
# 图标风格 (emoji|ascii|auto) / Icon style (emoji|ascii|auto)
#   emoji - 使用 Emoji 图标（Linux xterm 推荐）/ Use emoji icons (recommended on Linux xterm)
#   ascii - 使用 ASCII 图标如 [R][P][V]（WSL2/Windows Terminal 推荐）/ Use ASCII icons like [R][P][V] (recommended on WSL2/Windows Terminal)
#   auto  - 自动检测终端类型（Windows Terminal→ascii，Linux→emoji）/ Auto-detect terminal type (Windows Terminal→ascii, Linux→emoji)
icon_style = auto
# Emoji 显示宽度覆盖 (0|1|2) / Emoji display width override (0|1|2)
#   0 - auto（光标探测或终端检测，推荐）/ auto (cursor probe or terminal detection, recommended)
#   1 - 强制 emoji 宽度为 1 列（Windows Terminal 某些字体）/ Force emoji width = 1 column (some Windows Terminal fonts)
#   2 - 强制 emoji 宽度为 2 列（Linux xterm 标准）/ Force emoji width = 2 columns (Linux xterm standard)
# 如果 Emoji 模式下边框错位，尝试改为 1 或 2 / If borders misalign in emoji mode, try 1 or 2
emoji_width = 0
# 边框标题是否启用 Emoji (true|false)，默认 true / Enable emoji in border titles (true|false), default true
#   仅当终端实测 emoji 宽度=2（probe 或 emoji_width=2）时才真正渲染 Emoji 标题 / Emoji titles render only when the measured emoji width is 2 (probe or emoji_width=2)
#   此时所用 Emoji 的 glibc wcwidth 恒为 2（📻🎤💖📜🔍），与终端一致 → 光标不错位 → 边框对齐 / The emojis used have glibc wcwidth=2 (📻🎤💖📜🔍), matching the terminal → no cursor drift → borders align
#   否则（窄 emoji 终端/未探测）自动回退 ASCII 标题，保证边框不错位 / Otherwise (narrow-emoji terminal / not probed) it falls back to ASCII titles to keep borders aligned
#   需终端有 emoji 字体，否则 emoji 显示为 tofu（设 false 回退 ASCII）/ Requires an emoji font; otherwise emoji show as tofu (set false to fall back to ASCII)
title_emoji = true
# 配色方案索引 (0-8)，Ctrl+L 循环切换并自动写回此项 / Color scheme index (0-8); Ctrl+L cycles and writes back here
#   0=Dark(经典黑底) 1=Solarized Light 2=Solarized Dark 3=Dracula / 0=Dark 1=Solarized Light 2=Solarized Dark 3=Dracula
#   4=Gruvbox Dark 5=Nord 6=Monokai 7=Rose Pine 8=Catppuccin Mocha / 4=Gruvbox Dark 5=Nord 6=Monokai 7=Rose Pine 8=Catppuccin Mocha
theme_index = 0

# ============================================================
# 【节点树状态颜色配置】 / Node-tree status colors
# ============================================================
# 节点树中各项按"状态"着色，此处可自定义每种状态的前景色 / Node-tree items are colored by "status"; customize each status's foreground here
# 取值可为颜色名（大小写不敏感）或 0-255 数字代码 / Values: color name (case-insensitive) or a 0-255 numeric code
#   颜色名: black red green yellow blue magenta cyan white default / Names: black red green yellow blue magenta cyan white default
#   数字码: 见本段末尾的《颜色代码参考》/ Codes: see "Color code reference" at the end of this section
# 配置笔误将自动回退到默认值，不会黑屏 / Typos fall back to defaults automatically (no black screen)
[colors]
# 当前正在播放的节点（默认 green，并以加粗显示，与下方 downloaded 区分）/ Currently playing node (default green, bold; distinguished from downloaded below)
currently_playing = green
# 已完整下载/缓存的节点（默认 green，表达"已缓存可用" / Fully downloaded/cached node (default green, meaning "cached/available"
#   若希望与"当前播放"更易区分，可改为其他颜色，如 magenta / yellow）/ Change to another color (e.g. magenta / yellow) to distinguish from "currently playing"
downloaded = green
# 不完整下载（下载中断/失败留下的 .part 半文件）—— 与"完整下载"区分 / Incomplete download (.part file from an interrupted/failed download) — distinct from "fully downloaded"
partial = yellow
# 流缓存（仅解析了节目列表、媒体未下载；非"下载不完整"）/ Stream cache (episode list parsed, media not downloaded; not "incomplete download")
stream_cached = cyan
# 数据库缓存（节目列表来自本地数据库，未下载媒体）/ Database cache (episode list from local DB, media not downloaded)
db_cached = yellow
# 解析失败的节点 / Parse-failed node
parse_failed = red
# 信息蓝（保留位，当前未在节点树使用）/ Info blue (reserved, currently unused in the node tree)
info = blue
#
# 《颜色代码参考》 / Color code reference
# ────────────────────────────────────────────────────────────
# 【8 标准色】（颜色名等价数字）/ 8 standard colors (name = code)
#   0=black(黑)  1=red(红)       2=green(绿)   3=yellow(黄) / 0=black 1=red 2=green 3=yellow
#   4=blue(蓝)   5=magenta(洋红) 6=cyan(青)    7=white(白) / 4=blue 5=magenta 6=cyan 7=white
# 【8 亮色】（高亮版本，数字 8-15）/ 8 bright colors (codes 8-15)
#   8=亮黑(灰)  9=亮红   10=亮绿   11=亮黄 / 8=bright black(grey) 9=bright red 10=bright green 11=bright yellow
#   12=亮蓝     13=亮洋红 14=亮青   15=亮白 / 12=bright blue 13=bright magenta 14=bright cyan 15=bright white
# 【216 色立方体】（数字 16-231）：R/G/B 各 6 级 / 216-color cube (codes 16-231): R/G/B each 6 levels
#   公式: 16 + 36*r + 6*g + b   (r,g,b ∈ 0..5) / Formula: 16 + 36*r + 6*g + b  (r,g,b ∈ 0..5)
#   例: 196=纯红(36*5)  46=纯绿(6*5)  21=纯蓝  226=亮黄  51=亮青 / e.g. 196=red 46=green 21=blue 226=bright yellow 51=bright cyan
# 【24 级灰度】（数字 232-255）：232 最暗 → 255 最亮 / 24 grayscale levels (codes 232-255): 232 darkest → 255 brightest
#   例: 232=近黑  244=中灰  255=近白 / e.g. 232=near black 244=mid grey 255=near white
# 【特殊】 / Special
#   default / -1 = 透传终端默认前景/背景色 / default / -1 = passthrough terminal default fg/bg
# 提示: 256 色需终端支持（大多数现代终端默认支持）/ Note: 256-color requires terminal support (most modern terminals support it)
#       若颜色名与代码都不理想，可结合主题(深/浅)调整 / If names/codes are unsatisfactory, adjust together with the theme (dark/light)

# ============================================================
# 【快捷键说明】 / Keybindings
# ============================================================
# 快捷键在源码中硬编码，不可配置 / Keybindings are hardcoded in source, not configurable
# 完整键位列表请按 ? 键查看帮助屏，或参阅 README.md / Press ? for the full list, or see README.md
# 以下为常用键速查 / Quick reference for common keys:
#   导航: k/j（上下） g/G（顶/底） PgUp/PgDn h（返回） l/Enter（展开/播放）/ Nav: k/j(up/down) g/G(top/bottom) PgUp/PgDn h(back) l/Enter(expand/play)
#   播放: Space/p（暂停） +/-（音量） [/]（速度） \（重置速度）/ Play: Space/p(pause) +/-(volume) [/](speed) \(reset speed)
#   管理: a（添加） A（F模式加本地文件夹） d（删除） e（编辑） f（收藏） r（刷新） D（下载）/ Manage: a(add) A(add local folder in F mode) d(delete) e(edit) f(favourite) r(refresh) D(download)
#   标记: m（标记） v（Visual） V（清除）/ Mark: m(mark) v(visual) V(clear)
#   模式: R/P/F/H/O（直接切换） :（输入播放模式 r/s/c）/ Mode: R/P/F/H/O(switch) :(enter play mode r/s/c)
#   搜索: /（搜索） J/K（下/上个匹配）/ Search: /(search) J/K(next/prev match)
#   界面: T（树线） S（滚动） U（图标风格） Ctrl+L（循环9种配色方案） Ctrl+N（网络代理）/ UI: T(tree lines) S(scroll) U(icon style) Ctrl+L(cycle 9 themes) Ctrl+N(proxy)
#   网络: Ctrl+N 设置代理(socks5h://host:port 或 http://host:port)，留空=直连/透明代理 / Net: Ctrl+N sets proxy (socks5h://host:port or http://host:port); empty = direct/transparent proxy

# ============================================================
# 【网络配置】 / Network
# ============================================================
[network]
# 网络请求超时时间(秒) / Network request timeout (seconds)
timeout = 30
# TLS 证书验证（默认启用，仅在自签名/特殊环境关闭）/ TLS certificate verification (default on; disable only for self-signed/special environments)
# 设为 false 将使所有 HTTPS 请求可被 MITM 攻击，不推荐 / Setting false makes all HTTPS requests MITM-vulnerable; not recommended
tls_verify = true
# 是否拒绝非 http(s) URL（防 SSRF，默认拒绝）/ Reject non-http(s) URLs (SSRF protection; default reject)
reject_unsafe_url = true
# Network proxy (empty = direct connection / system transparent proxy). Press Ctrl+N to set it via a dialog.
# Accepted formats (scheme is case-insensitive; bare socks:// is normalized to socks5h://):
#   socks5h://192.168.1.5:33833   <- recommended (SOCKS5, proxy resolves DNS -> prevents DNS pollution/leak)
#   socks5://192.168.1.5:33833     (SOCKS5, local DNS resolution)
#   socks4://192.168.1.5:33833     (SOCKS4)
#   http://192.168.1.5:33833       (HTTP proxy)
#   https://192.168.1.5:33833      (HTTPS proxy)
# Applied to: all curl fetches (RSS/feed/OPML/Apple/downloads) and yt-dlp (--proxy).
# Takes effect immediately, no restart needed. mpv playback NEVER uses this proxy — mpv is
# playback-only; the network for streaming is the user's concern (transparent proxy / env).
proxy =
# 直连域名列表 / Direct-connection domain list (D45 → 泛化)
# 不需要走全局代理的站点（国内站等），逗号分隔通配符，如 *.bilibili.com,*.example.cn。
# 留空 = 全部走代理。启动时读取，改后需重启。同时作用于 curl 抓取与 yt-dlp。
# Domains that bypass the global proxy and connect directly (CN-domestic sites etc.),
# comma-separated globs. Empty = none. Read once at startup — restart after editing.
# Applies to both curl fetches and yt-dlp.
direct_domains = *.bilibili.com
# Bilibili 直连兼容开关 / Bilibili direct toggle (legacy, D45)
# false = 从上面列表剔除 *.bilibili.com，让 bilibili 也走代理。
# Set false to drop *.bilibili.com from direct_domains and route bilibili through the proxy.
bilibili_direct = true

# ============================================================
# 【存储配置】 / Storage
# ============================================================
[storage]
# 下载文件保存目录 / Download directory
download_dir = ~/Downloads/panicast
# 以下键已移除（从未被代码读取） / The following keys are removed (never read by code):
#   max_log_entries, search_cache_max, podcast_cache_days
# 保留并实际生效的键 / Keys still in effect:
# 播放历史最大条数（DatabaseManager::get_history 使用）/ Max playback history records (used by DatabaseManager::get_history)
history_max_records = 2048
# 播放历史最大天数（0表示不限制天数）/ Max playback history days (0 = unlimited)
history_max_days = 1080

# ============================================================
# 【播放配置】 / Playback
# ============================================================
[playback]
# 播放模式 (repeat|shuffle|cycle)。也可在程序内切换(: 命令输入 r/s/c)，自动写回此处 / Play mode (repeat|shuffle|cycle). Also switchable in-app (: then r/s/c), auto-written back here
#   repeat  - 重复模式：单曲循环(播完当前节目重放)/ repeat: single-track loop (replays current episode)
#   shuffle - 随机模式：随机选取下一个节目 / shuffle: random next episode
#   cycle   - 循环模式：顺序播放，到末尾后回到开头(默认) / cycle: sequential, wraps to start at end (default)
mode = cycle

# ============================================================
# 【MPV 播放器配置】 / MPV player
# ============================================================
# mpv 初始化参数的单一数据源。CLI --vo/--vid/--quiet 覆盖此处的值 / Single source for mpv init params. CLI --vo/--vid/--quiet override these
# 首次运行自动生成；修改后重启生效 / Auto-generated on first run; restart after editing
# ── [mpv] mpv 初始化参数 / mpv init params ──
[mpv]
# Video output driver
#   auto  = auto-select (WSL2->wlshm, native Linux->gpu)
#   null  = no video output (audio only, = --quiet)
#   gpu   = force GPU (fails on WSL2, falls back to wlshm)
#   wlshm = Wayland shared memory (WSL2 recommended)
# CLI: --vo=<val> overrides; --quiet = --vo=null --vid=no
vo = auto
# Video track decoding
#   auto = decode if video track present
#   no   = skip video decoding (audio only, saves CPU)
# CLI: --vid=<val> overrides
vid = auto
# Audio output driver (F37)
#   pulse,alsa (default) = try pulse first, fall back to alsa. On WSLg/pulse systems pulse
#     succeeds first → no pipewire/alsa probe → no library stderr polluting the TUI. alsa is
#     kept as a fallback (not skipped). mpv 0.41 ao=auto probes pipewire first, which fails
#     noisily on WSLg (no native pipewire) — hence the explicit default.
#   pulse / pipewire / alsa / auto = force a specific driver/order
# CLI: --ao=<val> overrides
# 音频输出驱动 / audio output driver
#   auto = mpv 自动探测(pipewire→pulse→alsa…,WSLg 与原生 Linux 通用)/ auto = mpv
#   auto-detect (works on both WSLg and native pipewire/pulse)
#   需要手工钉死时取消注释其一 / to pin manually, uncomment one of:
#ao=pulse
#ao=pipewire
#ao=alsa
ao = auto
# yt-dlp format selection
ytdl_format = bestvideo+bestaudio
# HTTP User-Agent (some CDNs reject default mpv UA)
user_agent = Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0
# Network stream cache. Y24.17: kept as-is (cache=yes) — local files fill cache near-instantly
# (disk throughput >> realtime), so cache isn't the cause of long BUFFERING; the real cause is
# diagnosed via the mpv log subscription + play_current timestamps (see [MPV/log] / [PLAY] in
# panicast-YYYYMMDD.log). play_audio/play_video still tune cache-secs per audio/video.
cache = yes
demuxer_max_bytes = 50MiB
demuxer_max_back_bytes = 25MiB
cache_secs = 10
# TLS certificate verification
tls_verify = true
# keep-open: pause at EOF (overridden at play time by play mode)
keep_open = yes
# 字幕设置（Y14, 视频窗口 mpv 原生渲染用） / subtitle settings (video window, mpv native rendering)
#   sub_align_x = center|left|right        水平对齐（默认 center）/ horizontal alignment
#   sub_align_y = bottom|top|center        垂直对齐（默认 bottom）/ vertical alignment
#   sub_visibility = yes|no                字幕可见（默认 yes）/ subtitle visibility
#   sub_ass_override = auto|no|force|scale ASS 字幕样式覆盖（默认 auto，让 sub-scale/sub-pos 生效）/ ASS override
#   sub_lang = en|zh|...                  多内嵌字幕轨时优先选择的语言（默认 en，English）/ preferred sub language among multiple embedded tracks
#   注：音频播放(无视频窗)走 TUI 歌词面板(🎵 LYRIC)，不在此设 / Note: audio-only uses TUI lyric panel, not these
sub_align_x = center
sub_align_y = bottom
sub_visibility = yes
sub_ass_override = auto
sub_lang = en

# ============================================================
# 【YouTube 配置】 / YouTube
# ============================================================
# yt-dlp 抓取 YouTube 频道/视频时的选项。订阅频道时：yt-dlp 枚举各 TAB 并解析节目 / yt-dlp options for YouTube channels/videos. Subscribing a channel: yt-dlp enumerates tabs and parses episodes
# (唯一方案；curl+RSS 已移除——本环境 RSS 404 不可用) / (the only method; curl+RSS removed — RSS 404 in this environment)
[youtube]
# cookies_file: yt-dlp 播放 YouTube 的唯一 cookies 来源(无浏览器回退)。Netscape 格式 cookies.txt / cookies_file: the SINGLE cookies source for yt-dlp playback (no browser fallback). Netscape cookies.txt
#   Y 模式 OAuth token 仅用于 Data API(订阅/搜索/节目列表)，播放必须用 cookies(yt-dlp 2026.07 已移除 oauth 播放) / Y-mode OAuth token is for Data API only; playback REQUIRES cookies (yt-dlp 2026.07 removed oauth playback)
#   取值: 空 或 bare "youtube_cookie.txt" → <数据目录>/youtube_cookie.txt；"~/..." 展开 $HOME；"/abs/path" 原样；其它相对路径按 <数据目录>/<输入> 解析 / values: empty/bare → <data_dir>/youtube_cookie.txt; "~/..." → $HOME-expanded; "/abs/path" → as-is; other relative → <data_dir>/<input>
#   仅当文件存在时才注入 --cookies(前置条件，非回退)；不存在则匿名→会触发机器人校验 / --cookies injected only if the file exists (precondition, not a fallback); absent → anonymous → bot check
#   导出方法: 浏览器隐私窗口登录 YouTube → 扩展 "Get cookies.txt LOCALLY" 导出 youtube.com → 关闭该隐私窗口(否则 YouTube 轮换 cookie 致失效) / export: incognito-window login → "Get cookies.txt LOCALLY" → CLOSE the incognito window (YouTube rotates active-tab cookies)
cookies_file = youtube_cookie.txt
# player_client: yt-dlp 使用的 YouTube 客户端(逗号分隔，按序尝试) / player_client: YouTube clients yt-dlp uses (comma-separated, tried in order)
#   tv_downgraded,web 是 yt-dlp 对 cookie 鉴权请求的官方默认组合，最不易触发机器人校验 / tv_downgraded,web is yt-dlp's official default for cookie-authed requests; least likely to trip bot check
#   注: 用 cookies 时需 JS 运行时解 nsig(tv_downgraded/web 都 REQUIRE_JS_PLAYER) / Note: with cookies a JS runtime is needed for nsig (tv_downgraded/web both REQUIRE_JS_PLAYER)
player_client = tv_downgraded,web
# js_runtime: yt-dlp 求解 YouTube nsig「n 挑战」用的 JS 运行时 / js_runtime: JS runtime yt-dlp uses to solve YouTube's nsig "n challenge"
#   quickjs = 用 quickjs-ng(打包 qjs ~2MB，冷启动比 deno 快约 10×，推荐) / quickjs = use quickjs-ng (bundled qjs ~2MB, ~10× faster cold-start than deno; RECOMMENDED)
#     ⚠ quickjs 不能从 npm 拉 EJS，需先 pip install -U "yt-dlp[default]"(带入 yt-dlp-ejs) / quickjs can't fetch EJS from npm; install `yt-dlp[default]` (brings yt-dlp-ejs) first
#   deno = 用 deno(yt-dlp 默认；~106MB 二进制，冷启动较慢) / deno = use deno (yt-dlp default; ~106MB binary, slower cold-start)
#   留空(键存在但空) = 不注入，由 yt-dlp 自选默认(deno 若在 PATH) / empty key = don't inject; yt-dlp picks its default (deno if on PATH)
#   注: 键不存在时默认 quickjs(旧 config 自动用 qjs，删了 deno 也不会断 nsig) / Note: default is quickjs when key absent
#   也可带路径: quickjs:/opt/qjs/bin/qjs / optional path form: quickjs:/opt/qjs/bin/qjs
js_runtime = quickjs
# play_format_video: 视频播放的 yt-dlp -f 格式 (Y09 1A DASH 预解析) / video play format (yt-dlp -f)
#   YouTube 无 1080p 单文件(muxed 最高 720p)，1080p 必须 DASH(视频流+音频流分离,mpv 经 audio-file 合流,需 ffmpeg)
#   bestvideo[height<=1080]+bestaudio  = 最高 1080p DASH [默认] / up to 1080p DASH [default]
#   bestvideo+bestaudio                = 最高画质(4K/8K DASH) / max quality (4K/8K DASH)
#   bestvideo[height<=720]+bestaudio   = 最高 720p DASH / up to 720p DASH
#   best                                     = 最佳单文件(已弃用,DASH-only) muxed(≤720p,无需合流) / best single muxed file (≤720p, no merge)
play_format_video = bestvideo[height<=1080]+bestaudio
# play_format_audio: 音频播放的 yt-dlp -f 格式 / audio play format
#   bestaudio = 最高音质 [默认] / highest audio quality [default]
#   best           = 最佳单文件(已弃用)(含视频,较低) / best single file (incl. video, lower)
play_format_audio = bestaudio
# resolve_timeout_sec: 播放解析(yt-dlp -g)每次尝试超时(秒) / per-attempt yt-dlp -g resolve timeout (seconds)
#   旧版固定 30s 过短：经 SOCKS 代理 + quickjs/ejs 解 nsig，单次解析常需 30-60s，30s 上限导致间歇性“YouTube resolve failed”
#   the old fixed 30s was too short through a proxy + nsig solving (legit 30-60s), causing intermittent resolve failures
resolve_timeout_sec = 90
# resolve_retries: 播放解析失败/超时后的重试次数(含首次) / playback resolve attempts on failure/timeout (incl. first)
#   YouTube 解析不稳(nsig + 机器人校验)，重试通常即可成功 / YouTube resolve is flaky; a retry usually succeeds
resolve_retries = 3
# sub_lang: YouTube 字幕语言码(加载软字幕, Y11)。空=不加载(默认, opt-in) / subtitle language; empty=off (default)
#   设后 resolve 用 yt-dlp 取 .vtt 字幕, mpv 以 sub-file 加载 → F/G 缩放、z/Z 同步、r/R 位移、v 显隐均可用, 默认居中
#   例: en / zh-Hans / ja 。无手动字幕时 sub_auto=true 用自动生成 / e.g. en/zh-Hans/ja; sub_auto=true falls back to auto-captions
sub_lang =
sub_auto = true

# ============================================================
# 【YouTube/视频下载配置】 / YouTube / video download
# ============================================================
# YouTube 下载参数硬编码在 YtdlpRunner 中 / YouTube download params are hardcoded in YtdlpRunner
# 当前: -f bestvideo+bestaudio --merge-output-format mp4 / Current: -f bestvideo+bestaudio/best --merge-output-format mp4
# 如需自定义，请编辑 src/panicast.cpp 中 YtdlpRunner::run 调用 / To customize, edit the YtdlpRunner::run call in src

# ============================================================
# 【状态栏颜色配置】 / Status-bar color
# ============================================================
# ============================================================
# 【Bilibili 配置】 / Bilibili (B mode, Y15)
# ============================================================
[bilibili]
# cookies_file: Bilibili cookies.txt 路径(Netscape 格式, yt-dlp --cookies 用) / Bilibili cookies path
#   空/bare "bilibili_cookie.txt" → <数据目录>/bilibili_cookie.txt
#   QR 登录自动写入此文件; 也可手动导入浏览器导出的 cookies.txt
cookies_file = bilibili_cookie.txt

[tiktok]
# Y24.11: T 模式 (TikTok/Douyin) 配置 / T-mode config
# region: T 模式观看区域 (b 键循环) / T-mode viewing region (b cycles)
#   yt-dlp --geo-bypass-country <region> 注入该国家 IP; 创作者自己的视频是全局的,
#   区域主要影响反爬/限流与区域锁定创作者的可达性 / spoofs a residential IP of <region>;
#   a creator's own videos are global — region mainly affects anti-bot + region-locked reach
#   CN = 中国抖音 (访问 douyin.com); 其余为 TikTok 区域 / CN = Douyin (douyin.com); others = TikTok
#   可选: US JP GB DE FR KR ID TH VN MY BR MX CN
region = US
# douyin_cookies_file: 抖音 cookies.txt 路径(Netscape 格式) / Douyin cookies path
#   TikTok 匿名即可; 抖音通常需要登录 cookie + 中国大陆出口 / TikTok is anonymous;
#   Douyin usually needs a logged-in cookies.txt + a CN network exit
douyin_cookies_file = douyin_cookie.txt

[transcription]
# Y24.19: whisper.cpp offline transcription (F mode: cursor/v-mark local files → L).
#   Saves <file>.srt next to the media (replay auto-loads). Real-time transcription = Y24.20.
# Install: Arch `sudo pacman -S whisper-cpp`; Debian `sudo apt install whisper.cpp` (or build from source).
# Model: download ggml-small.en-q5_1.bin (AMD 7735HS) / ggml-tiny.en.bin (weak CPU) from
#   https://huggingface.co/ggerganov/whisper.cpp (the official download-ggml-model.sh mirror).
# Path resolution: bare name → PATH (which); ~ → $HOME; absolute → fs::exists. Model bare name → <data_dir>/models/.
whisper_bin = whisper-cli
model = ~/.local/share/panicast/models/ggml-small.en-q5_1.bin
# max concurrent offline jobs; dispatcher ramps within [1, max] based on CPU load (getloadavg).
max_concurrent = 3
# 播放时自动本地 ASR（默认开）/ Auto local ASR on play (default on)
# 播放本地节目且没有任何更便宜的字幕来源（内嵌轨/本地SRT/在线转录）时，后台自动起 whisper 转写；
# 字幕就位后 LYRIC 面板自动亮起——播放本身零等待。仅对本地文件生效（流媒体想转写请手动按 L，
# 因为那需要先整集下载）；直播/未知时长的流自动跳过。whisper 线程数已预留核心，不会卡 UI/播放。
# When playing a LOCAL episode with no cheaper subtitle source (embedded/local SRT/online), start
# whisper transcription in the background; the LYRIC panel lights up when segments arrive — play is
# never delayed. Local files only (streaming needs a full download first — press L deliberately);
# live/unknown-duration streams are skipped. whisper threads already reserve cores for UI + mpv.
auto_asr = true

[statusbar_color]
# 颜色模式 / Color mode:
#   rainbow  - 彩虹渐变效果 / rainbow gradient
#   random   - 随机颜色 / random color
#   fixed    - 固定颜色(使用fixed_color) / fixed color (uses fixed_color)
#   custom   - 自定义颜色序列 / custom color sequence
mode = rainbow
# 更新间隔(毫秒) / Update interval (ms)
update_interval_ms = 100
# 亮度范围 (0.0-1.0) / Brightness range (0.0-1.0)
brightness_min = 0.5
brightness_max = 1.0
# 时间调整效果 / Time-adjust effect
time_adjust = true
# 固定颜色模式时使用的颜色 / Color used in fixed mode:
#   black red green yellow blue magenta cyan white
fixed_color = cyan
# 彩虹模式速度 (1-10) / Rainbow speed (1-10)
rainbow_speed = 1
# CUSTOM 模式配置 / CUSTOM mode config
# custom_colors: ncurses 颜色码循环序列（逗号分隔）/ custom_colors: cycling list of ncurses color codes (comma-separated)
#   颜色码参考: 0=黑 1=红 2=绿 3=黄 4=蓝 5=洋红 6=青 7=白 / Code ref: 0=black 1=red 2=green 3=yellow 4=blue 5=magenta 6=cyan 7=white
#   高亮: 8-15 为上述颜色的加亮版（9=亮红 10=亮绿 11=亮黄 12=亮蓝 13=亮洋红 14=亮青 15=亮白）/ Bright: 8-15 are bright variants (9=bright red 10=bright green 11=bright yellow 12=bright blue 13=bright magenta 14=bright cyan 15=bright white)
#   256色: 16-231（xterm 256 色彩立方体）/ 256-color: 16-231 (xterm 256-color cube)
# custom_speed: 每 N 个字符切换一次颜色（1=逐字符，2=每2字符，...）/ custom_speed: change color every N chars (1=per char, 2=every 2 chars, ...)
# 示例: 红/黄/绿/蓝/洋红/白 循环，每2字符换色 / Example: red/yellow/green/blue/magenta/white cycle, color every 2 chars
custom_colors = 9,11,10,14,13,15
custom_speed = 2

# ============================================================
# 【高级配置】 / Advanced
# ============================================================
# [advanced] 段已移除（cache_expire_hours/debug 从未被读取） / [advanced] section removed (cache_expire_hours/debug never read)
# 调试请通过环境变量或直接查看日志文件 ~/.local/share/panicast/panicast-YYYYMMDD.log (daily, kept 365 days) / For debugging, use env vars or view the log at ~/.local/share/panicast/panicast-YYYYMMDD.log (daily, kept 365 days)

# ============================================================
# 【iTunes搜索配置】 / iTunes search
# ============================================================
[search]
# cache_max 已移除（从未被读取） / cache_max removed (never read)
# default_region 实际生效（OnlineState 持久化）/ default_region is effective (persisted by OnlineState)
# 默认搜索地区 / Default search region:
#   US - 美国  CN - 中国  TW - 台湾  JP - 日本 / US - United States  CN - China  TW - Taiwan  JP - Japan
#   UK - 英国  DE - 德国  FR - 法国  KR - 韩国  AU - 澳大利亚 / UK - United Kingdom  DE - Germany  FR - France  KR - South Korea  AU - Australia
default_region = US

# ============================================================
# 【远程控制】 / Remote control (N line — network control)
# ============================================================
# Opt-in remote control server (TCP socket). Default off — the local TUI is completely
# unaffected unless you set enable=true. The command protocol is under development (N line).
#   enable     = false|true        off by default; opt-in / 默认关闭，按需开启
#   port       = 8421              TCP listen port / 监听端口
#   bind       = 0.0.0.0           shared by PRP + mini-LMS. 0.0.0.0 = LAN-reachable;
#                                 127.0.0.1 = localhost only / PRP 与 mini-LMS 共用；0.0.0.0 局域网可达
#   auth_token =                   empty = no auth (LAN only); set a token to require it / 空=不鉴权(仅局域网)；设置令牌则强制校验
[remote]
enable = false
port = 8421
bind = 0.0.0.0
auth_token =
# Universal pairing PIN (always valid; for headless/no-display pairing) / 万能配对 PIN（始终有效，无屏场景用）
# Change it here to your own code / 可在此改成自定义码
universal_pin = 6696
# UDP discovery port the APK broadcasts to in order to auto-find players on the LAN / APK 扫描发现用的 UDP 端口
discovery_port = 18430
# N08: mini-LMS（Squeezer 遥控，LMS CLI 行协议控制面）运行开关 / mini-LMS (Squeezer remote control) toggle
#   双层控制：编译层由 CMake PANICAST_REMOTE_LMS 决定（默认 ON 已编入）；本键为运行层，默认开启。
#   Squeezer 手工填 <本机IP>:9090 连接 / Dual gate: compile layer = the CMake PANICAST_REMOTE_LMS
#   option (default ON, builtin); this runtime layer defaults ON — point Squeezer at <host>:9090.
lms_enable = true
# mini-LMS 监听端口（LMS CLI 惯例 9090，Squeezer 手工填写 IP:9090）/ mini-LMS listen port (LMS CLI convention 9090 — enter IP:9090 manually in Squeezer)
lms_port = 9090
# 允许接入的源 IP 白名单（CIDR 逗号分隔；裸 IP 视为 /32）/ allowed source IPs (comma-separated CIDRs; bare IP = /32)
#   默认仅家庭网段 + 本机回环；Squeezer 手机需在此网段内 / default: home LAN + loopback only
#   留空 = 允许任意源（不建议）/ empty = allow ALL sources (not recommended)
lms_allow = 192.168.0.0/16,127.0.0.0/8
# 播放器显示名（Squeezer 播放器列表/正在播放界面显示；空 = "panicast"）
#   player display name in Squeezer's player list & now-playing screen
player_name =
# 服务器显示名（Squeezer 扫描列表里展示；空 = 主机名）/ display name in Squeezer's
#   server picker (empty = hostname). 多台主机各设一个可区分的名字最有用。
server_name =
# 实例唯一 ID — 首次启动自动生成并回写，勿手工修改 / auto-generated on first
#   boot, do not edit. Squeezer 用它区分不同 panicast 主机。
# uuid =
# 登录鉴权：Squeezer 设置里的 Username/Password 与此一致才可控制 / login auth: Squeezer's
#   Username/Password fields must match these to control playback.
#   默认 panicast/panicast（出厂凭据，装好即用）；改成自己的更安全 / default factory
#   credentials panicast/panicast — change them to your own for safety.
#   lms_pass 显式留空 = 关闭鉴权（仅白名单防护）/ explicitly EMPTY lms_pass = auth off
#   注意：密码为明文存放，建议 chmod 600 本文件 / note: plaintext — chmod 600 this file
lms_user = panicast
lms_pass = panicast

# ============================================================
# 【艺术显示颜色代码参考】 / Color code reference
# ============================================================
# ncurses标准颜色代码 / ncurses standard color codes:
#   0 - 默认颜色 / default color
#   1 - 黑色 (black) / black
#   2 - 红色 (red) / red
#   3 - 绿色 (green) / green
#   4 - 黄色 (yellow) / yellow
#   5 - 蓝色 (blue) / blue
#   6 - 洋红 (magenta) / magenta
#   7 - 青色 (cyan) / cyan
#   8 - 白色 (white) / white
#
# 状态栏颜色模式说明 / Status-bar color modes:
#   rainbow  - 自动循环显示所有颜色，形成彩虹效果 / auto-cycles all colors forming a rainbow
#   random   - 每次更新随机选择颜色 / picks a random color each update
#   fixed    - 使用fixed_color指定的单一颜色 / single color from fixed_color
#   custom   - 使用自定义颜色序列(需修改源码) / custom color sequence (requires source edit)
# ============================================================

# ============================================================
# 【热键自定义】 / Hotkey rebinding ([keys])
# ============================================================
# 在此重绑可重绑热键；留空或删整行 = 用默认值；改后重启生效。
# 键值写法：键名(space/enter/esc/tab/backspace) 或 单个字符(如 p + - k) 或 数字 keycode。
# backspace/enter 键名会同时绑定各终端编码(127/8/KEY_BACKSPACE、'\n'/KEY_ENTER)，任何终端都生效。
# 单个数字是字符本身('5' = 数字键5)，数字 keycode 仅用于特殊键(如 259 = 方向键上)。
# 同一动作绑多个键用逗号分隔(如 play_pause = space,p)。仅无状态命令 + 模式切换键可重绑；
# 复杂有状态流(play 'l'/搜索 '/'/下载/标记)保留在程序内——把动作绑到这些键上会覆盖原功能
# (启动日志留 WARNING，原键失效)。
# 两个动作绑到同一键时后者生效并在日志留 WARNING；无法解析或被前置拦截(Ctrl+Y/鼠标)的键值
# 会被跳过并记日志。
# Rebind hotkeys here; empty/deleted line = default; restart after editing.
# Value: a key name (space/enter/esc/tab/backspace), a single char (p + - k), or a numeric keycode.
# backspace/enter names bind every terminal encoding (127/8/KEY_BACKSPACE, '\n'/KEY_ENTER).
# A single digit is the character itself ('5' = the 5 key); numeric keycodes are for special
# keys only (e.g. 259 = KEY_UP).
# Multiple keys per action: comma-separate (play_pause = space,p). Only stateless + mode-switch
# keys are rebindable; complex stateful flows (play 'l' / search '/' / download / mark) stay
# in-app — binding onto their keys OVERRIDES that flow (WARNING logged; the flow's key dies).
# If two actions share a key the later one wins (WARNING logged); unparseable or pre-intercepted
# (Ctrl+Y / mouse) tokens are skipped and logged.
[keys]
play_pause = space,p
volume_up = +
volume_down = -
nav_up = k
nav_down = j
mode_radio = R
mode_podcast = P
mode_favourite = F
mode_history = H
mode_online = O
mode_account = Y
mode_bilibili = B
mode_iptv = I
# ============================================================
)";
    });
}

} // namespace panicast
