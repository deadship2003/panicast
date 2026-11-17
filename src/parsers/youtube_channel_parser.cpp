// YouTube channel parser implementation.
#include "panicast/parsers/youtube_channel_parser.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <unistd.h> // access/X_OK for find_qjs_binary

#include "panicast/storage/accounts.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "panicast/config/ini_config.h"
#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/net/url_classifier.h"
#include "panicast/net/url_guard.h"
#include "panicast/net/ytdlp_runner.h"

namespace panicast
{

namespace fs = std::filesystem;
using json = nlohmann::json;

// Find the quickjs executable on PATH: try "qjs" then "qjsng" (quickjs-ng on some distros names the
//   binary qjsng). Debian "quickjs" and Arch "quickjs-ng" both ship `qjs`; qjsng is covered too so
//   detection works across distros regardless of package name. Returns the absolute path or "".
//   yt-dlp's quickjs provider runs the executable; passing `--js-runtimes quickjs:<abspath>` makes
//   the call work no matter what the binary is named or whether it's on PATH.
static std::string find_qjs_binary() {
    auto is_exec_file = [](const std::string &p) -> bool {
        std::error_code ec;
        return fs::is_regular_file(p, ec) && access(p.c_str(), X_OK) == 0;
    };
    const char *path_env = std::getenv("PATH");
    if (!path_env)
        return "";
    std::string dirs = path_env;
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t end = dirs.find(':', start);
        std::string dir =
            (end == std::string::npos) ? dirs.substr(start) : dirs.substr(start, end - start);
        if (!dir.empty()) {
            for (const char *name : {"qjs", "qjsng"}) {
                std::string cand = dir + "/" + name;
                if (is_exec_file(cand))
                    return cand;
            }
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return "";
}

std::vector<std::string> YouTubeChannelParser::ytdlp_youtube_args() {
    // yt-dlp 2026.07+ removed OAuth playback (`--username oauth2` errors); the Y-mode OAuth token
    //   powers only the Data API (search/subscribe/identity/episode-list), NOT yt-dlp playback.
    //   Playback uses cookies as the single auth path (see ytdlp_youtube_args_parse).
    return ytdlp_youtube_args_parse();
}

std::vector<std::string> YouTubeChannelParser::ytdlp_youtube_args_parse() {
    // SINGLE cookies flow: `--cookies <resolved path>` when the file exists (a precondition, not a
    //   fallback method — there is no browser auto-detect). Absent cookies → yt-dlp runs anonymous
    //   and playback trips the bot check, surfacing a clear error. player_client + js_runtime apply
    //   alongside. Used by parse_video_list (episode-list fallback) and resolve_youtube_url (playback).
    std::vector<std::string> a;
    std::string cf = IniConfig::instance().get_youtube_cookies_file(); // resolved absolute path
    std::error_code ec;
    if (!cf.empty() && fs::exists(cf, ec)) {
        a.push_back("--cookies");
        a.push_back(cf);
    }
    std::string pc = IniConfig::instance().get_youtube_player_client();
    if (!pc.empty()) {
        a.push_back("--extractor-args");
        a.push_back("youtube:player_client=" + pc);
    }
    auto jsr = js_runtime_args();
    a.insert(a.end(), jsr.begin(), jsr.end());
    return a;
}

std::vector<std::string> YouTubeChannelParser::js_runtime_args() {
    // [youtube] js_runtime: "quickjs" | "deno" | "quickjs:/path/to/qjs" | "" (yt-dlp default).
    //   For bare "quickjs" we resolve the qjs/qjsng binary (find_qjs_binary) and pass
    //   `quickjs:<abspath>` so it works across distros (Arch quickjs-ng / Debian quickjs) without
    //   depending on the binary name being exactly `qjs` on PATH. If not found, fall back to bare
    //   `quickjs` (yt-dlp will report it can't find it — a clear error). quickjs needs the EJS solver
    //   via `yt-dlp[default]` (yt-dlp-ejs) since, unlike deno, it can't fetch EJS deps from npm.
    std::string v = IniConfig::instance().get_youtube_js_runtime();
    std::vector<std::string> a;
    if (v.empty())
        return a;
    if (v == "quickjs") {
        std::string qjs = find_qjs_binary();
        v = qjs.empty() ? std::string("quickjs") : ("quickjs:" + qjs);
    }
    a.push_back("--js-runtimes");
    a.push_back(v);
    return a;
}

bool YouTubeChannelParser::is_bare_channel(const std::string &url) {
    std::string path = URLClassifier::url_path(url); // strip scheme/query/fragment
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    // Known tab segments (based on tabs a channel may actually expose; dynamic
    //   enumeration is still decided by -J; this only distinguishes whether tab
    //   enumeration is needed)
    static const std::vector<std::string> tab_suffixes = {
        "/videos",   "/shorts", "/streams", "/playlists", "/posts",
        "/featured", "/home",   "/about",   "/community", "/channels"};
    for (const auto &s : tab_suffixes) {
        if (lower.size() >= s.size() && lower.compare(lower.size() - s.size(), s.size(), s) == 0) {
            return false;
        }
    }
    return true;
}

std::string YouTubeChannelParser::diag_tail(const YtdlpRunner::Result &result) {
    std::string stderr_tail = result.stderr_output;
    size_t pos = stderr_tail.find_last_of('\n');
    if (pos != std::string::npos && pos > 0) {
        size_t prev = stderr_tail.find_last_of('\n', pos - 1);
        stderr_tail = stderr_tail.substr(prev == std::string::npos ? 0 : prev + 1,
                                         prev == std::string::npos ? pos : pos - prev - 1);
    }
    return fmt::format("exit={} | {}", result.exit_code, stderr_tail);
}

int YouTubeChannelParser::parse_video_list(const std::string &url, TreeNodePtr parent,
                                           std::vector<YouTubeVideoInfo> &videos,
                                           std::string &err) {
    std::vector<std::string> args =
        ytdlp_youtube_args_parse(); // Y02: cookies for reliable tab parsing
    args.push_back("--flat-playlist");
    args.push_back("--dump-json");
    args.push_back("--no-warnings");
    args.push_back(url);
    std::vector<std::string> lines;
    auto result = YtdlpRunner::run(
        args, [&](const std::string &line) { lines.push_back(line); },
        90, // 90s timeout (large tabs like shorts are slower)
        url);
    if (!result.launched) {
        err = "yt-dlp not launched";
        return 0;
    }
    auto is_valid_video_id = [](const std::string &s) -> bool {
        if (s.size() != 11)
            return false;
        for (char c : s) {
            if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-'))
                return false;
        }
        return true;
    };
    int count = 0;
    for (const auto &line : lines) {
        try {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
                l.pop_back();
            if (l.empty() || l[0] != '{')
                continue;
            auto j = json::parse(l);
            if (!j.is_object())
                continue; // skip null/non-object lines (yt-dlp may output "null" when rate-limited)
            std::string id = j.value("id", "");
            std::string title = j.value("title", "Untitled");
            std::string etype = j.value("_type", "");
            std::string eurl = j.value("url", "");
            std::string wpurl = j.value("webpage_url", "");
            int dur = j.value("duration", 0);
            // ART-2: flat entries carry a thumbnails array (or a plain thumbnail URL) —
            //   pick the LAST (largest) entry.
            std::string thumb;
            if (j.contains("thumbnails") && j["thumbnails"].is_array() &&
                !j["thumbnails"].empty()) {
                thumb = j["thumbnails"].back().value("url", "");
            }
            if (thumb.empty())
                thumb = j.value("thumbnail", "");

            // Video entry: _type empty or "url", and id is a valid 11-char id
            if ((etype.empty() || etype == "url") && is_valid_video_id(id)) {
                auto ep = std::make_shared<TreeNode>();
                ep->type = NodeType::PODCAST_EPISODE;
                ep->title = title;
                ep->url = fmt::format("https://www.youtube.com/watch?v={}", id);
                ep->is_youtube = true;
                ep->duration = dur;
                ep->art_url = thumb; // ART-2
                ep->children_loaded = true;
                ep->parent = parent;
                parent->children.push_back(ep);
                videos.push_back({id, title, ep->url, thumb});
                count++;
            } else if (!wpurl.empty() || !eurl.empty()) {
                // Sub-playlist (e.g. each playlist inside the Playlists tab) -> lazy feed, expandable
                std::string sub = wpurl.empty() ? eurl : wpurl;
                auto sub_node = std::make_shared<TreeNode>();
                sub_node->type = NodeType::PODCAST_FEED;
                sub_node->title = title;
                sub_node->url = sub;
                sub_node->is_youtube = true;
                sub_node->children_loaded = false; // parsed on expand
                sub_node->parent = parent;
                int pc = j.value("playlist_count", 0);
                if (pc > 0)
                    sub_node->subtext = fmt::format("{} videos", pc);
                parent->children.push_back(sub_node);
                count++;
            }
        } catch (...) {
        }
    }
    if (count == 0)
        err = diag_tail(result);
    return count;
}

int YouTubeChannelParser::parse_channel_tabs(const std::string &channel_url,
                                             TreeNodePtr channel_node, std::string &err) {
    std::vector<std::string> args =
        ytdlp_youtube_args_parse(); // Y02: cookies for reliable tab parsing
    args.push_back("--flat-playlist");
    args.push_back(
        "--dump-single-json"); // -J: top level is the tab list, do not recurse into videos
    args.push_back("--no-warnings");
    args.push_back(channel_url);
    // -J outputs a single JSON (may contain newlines), so line_cb is unreliable; use stdout_output directly
    auto result = YtdlpRunner::run(args, nullptr, 30, channel_url);
    if (!result.launched) {
        err = "yt-dlp not launched";
        return 0;
    }
    if (result.stdout_output.empty()) {
        err = fmt::format("empty output | {}", diag_tail(result));
        return 0;
    }
    int count = 0;
    try {
        auto j = json::parse(result.stdout_output);
        // When yt-dlp fails/is rate-limited, -J may output "null" or a non-object; check explicitly
        //   for a clear diagnosis, avoiding a type_error.306 from .value() (caught by try/catch
        //   but with an obscure message).
        if (!j.is_object()) {
            err = fmt::format(
                "yt-dlp returned non-object output ({}), possibly rate-limited/failed | {}",
                j.is_null() ? "null" : j.type_name(), diag_tail(result));
            return 0;
        }
        auto entries = j.value("entries", json::array());
        for (auto &e : entries) {
            std::string title = e.value("title", "");
            std::string wpurl = e.value("webpage_url", "");
            int pc = e.value("playlist_count", 0);
            if (wpurl.empty())
                continue;
            std::string tabname = title;
            size_t dash = title.rfind(" - ");
            if (dash != std::string::npos)
                tabname = title.substr(dash + 3);
            if (tabname.empty())
                tabname = "Videos";
            auto tab = std::make_shared<TreeNode>();
            tab->type = NodeType::PODCAST_FEED;
            tab->title = pc > 0 ? fmt::format("{} ({})", tabname, pc) : tabname;
            tab->url = wpurl; // .../videos | .../streams | .../shorts | .../playlists ...
            tab->is_youtube = true;
            tab->children_loaded = false; // parse this tab's videos on expand
            tab->parent = channel_node;
            channel_node->children.push_back(tab);
            count++;
        }
    } catch (const std::exception &e) {
        err = std::string("json parse: ") + e.what();
        return 0;
    }
    if (count == 0)
        err = diag_tail(result);
    return count;
}

TreeNodePtr YouTubeChannelParser::parse(const std::string &channel_url) {
    auto channel_node = std::make_shared<TreeNode>();
    channel_node->type = NodeType::PODCAST_FEED;
    channel_node->children_loaded = false;
    channel_node->is_youtube = true;
    channel_node->title = URLClassifier::extract_channel_name(channel_url);
    channel_node->channel_name = channel_node->title;

    EVENT_LOG(fmt::format("[YouTube] Parsing channel: {}", channel_url));
    LOG(fmt::format("[YouTube] Parsing: {}", channel_url));

    if (UrlGuard::reject(channel_url, "YouTubeChannelParser")) {
        channel_node->parse_failed = true;
        channel_node->error_msg = "Unsafe URL rejected";
        return channel_node;
    }

    std::string proxy_status =
        IniConfig::instance().get_proxy().empty() ? "none" : IniConfig::instance().get_proxy();

    URLType utype = URLClassifier::classify(channel_url);

    // ── 1) tab URL or playlist URL: fetch that tab/playlist's video list directly ──────
    if (utype == URLType::YOUTUBE_PLAYLIST ||
        (utype == URLType::YOUTUBE_CHANNEL && !is_bare_channel(channel_url))) {
        EVENT_LOG(fmt::format("[YouTube] Parsing tab/playlist: {}", channel_url));
        std::vector<YouTubeVideoInfo> videos;
        std::string err;
        int count = parse_video_list(channel_url, channel_node, videos, err);
        if (count > 0) {
            channel_node->children_loaded = true;
            EVENT_LOG(fmt::format("[YouTube] tab ok: {} videos", count));
            LOG(fmt::format("[YouTube] tab parsed {} videos", count));
            return channel_node;
        }
        channel_node->parse_failed = true;
        channel_node->error_msg = fmt::format("tab parse failed (proxy={}): {}", proxy_status, err);
        EVENT_LOG(fmt::format("[YouTube] tab parse failed: {}", err));
        LOG(fmt::format("[YouTube] tab failed: {}", err));
        return channel_node;
    }

    // ── 2) Bare channel: enumerate all tabs (multi-tab child tree) ────────────
    EVENT_LOG("[YouTube] Enumerating channel tabs...");
    std::string err_tabs;
    int tabcount = parse_channel_tabs(channel_url, channel_node, err_tabs);
    if (tabcount > 0) {
        channel_node->children_loaded = true;
        EVENT_LOG(fmt::format("[YouTube] Found {} tabs: {}", tabcount, [&] {
            std::string s;
            for (auto &c : channel_node->children) {
                if (!s.empty())
                    s += ", ";
                s += c->title;
            }
            return s;
        }()));
        LOG(fmt::format("[YouTube] parsed {} tabs", tabcount));
        return channel_node;
    }

    // ── 3) Failure — diagnostics
    channel_node->parse_failed = true;
    channel_node->error_msg =
        fmt::format("yt-dlp parse failed (proxy={}): {}", proxy_status, err_tabs);
    EVENT_LOG(fmt::format("[YouTube] parse failed: yt-dlp (proxy={}): {}", proxy_status, err_tabs));
    LOG(fmt::format("[YouTube] parse failed: proxy={}, tab: {}", proxy_status, err_tabs));
    return channel_node;
}

// ── ParserRegistry self-registration ──
REGISTER_PARSER(YouTubeChannelParser)
// YOUTUBE_PLAYLIST is also handled by YouTubeChannelParser (dispatched internally by URL shape); registered separately.
static ::panicast::ParserRegistrar
    _reg_yt_playlist(::panicast::URLType::YOUTUBE_PLAYLIST,
                     []() -> std::unique_ptr<::panicast::IFeedParser> {
                         return std::make_unique<YouTubeChannelParser>();
                     });

} // namespace panicast
