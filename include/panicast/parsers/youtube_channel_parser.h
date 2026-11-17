// YouTube channel parser (multi-tab structure, yt-dlp-only approach).
//   1) Resolve channel_id (lightweight yt-dlp --print channel_id, cached per session)
//   2) Fetch the official RSS feeds/videos.xml?channel_id=... (curl + proxy, plain XML, no anti-bot)
//   3) If RSS fails/empty -> fall back to yt-dlp --flat-playlist --dump-json
//   4) On failure, write diagnostics to error_msg (path/exit code/stderr last line/proxy status)
// Registered with ParserRegistry (URLType::YOUTUBE_CHANNEL).
#pragma once

#include <string>
#include <vector>

#include "panicast/core/types.h"
#include "panicast/net/ytdlp_runner.h"
#include "panicast/parsers/feed_parser.h"
#include "panicast/storage/youtube_cache.h"

namespace panicast
{

class YouTubeChannelParser : public IFeedParser {
public:
    // Build yt-dlp YouTube-specific args (cookies [single path] + player_client + js_runtime).
    // proxy is injected globally by YtdlpRunner, not duplicated here.
    static std::vector<std::string> ytdlp_youtube_args();
    // Y02: args for parsing (tab/video listing). Read-only public data → use cookies for reliability
    //   (oauth2 with a stale cache makes yt-dlp attempt an interactive device flow and fail, which
    //   broke channel-tab parsing in Y01). Account identity is still applied at play/download.
    static std::vector<std::string> ytdlp_youtube_args_parse();
    // JS-runtime args derived from [youtube] js_runtime (e.g. {"--js-runtimes","quickjs"}).
    //   Empty vector when js_runtime is unset (yt-dlp uses its default, deno). Injecting this
    //   forces yt-dlp to use the chosen runtime for nsig solving — quickjs (~2MB, ~10× faster
    //   cold-start than deno) is the lightweight replacement for the 106MB bundled deno binary.
    //   Append to any yt-dlp arg list (parse / play-resolve / download / mpv -13 fallback).
    static std::vector<std::string> js_runtime_args();

    // Whether the URL is a "bare channel" (no tab suffix).
    static bool is_bare_channel(const std::string &url);

    // yt-dlp diagnostic last line (stderr last line + exit code), written to error_msg on failure.
    static std::string diag_tail(const YtdlpRunner::Result &result);

    // Parse a "video list" path: for a tab URL or playlist URL, call
    //   yt-dlp --flat-playlist --dump-json, filling parent->children from NDJSON line by line.
    // Returns the video count; writes diagnostics to err when 0.
    static int parse_video_list(const std::string &url, TreeNodePtr parent,
                                std::vector<YouTubeVideoInfo> &videos, std::string &err);

    // Enumerate all channel tabs: for a bare channel, call yt-dlp --flat-playlist --dump-single-json (-J),
    //   the top-level entries are the tabs. Returns the tab count; writes diagnostics to err when 0.
    static int parse_channel_tabs(const std::string &channel_url, TreeNodePtr channel_node,
                                  std::string &err);

    // Main entry: tab/playlist URL -> parse_video_list; bare channel -> parse_channel_tabs.
    static TreeNodePtr parse(const std::string &channel_url);

    // ── ParserRegistry self-registration ──
    static URLType type() {
        return URLType::YOUTUBE_CHANNEL;
    }
    URLType supports() const override {
        return URLType::YOUTUBE_CHANNEL;
    }
    TreeNodePtr parse(const ParseInput &in) override {
        return parse(in.url);
    }
};

} // namespace panicast
