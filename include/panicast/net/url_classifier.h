// URL classifier: table-driven recognition of OPML/RSS/YouTube/radio stream/video file and other URL types.
#pragma once

#include <string>

#include "panicast/core/types.h"

namespace panicast
{

class URLClassifier {
public:
    // Prefix match table (ordered by priority, first match wins)
    // suffix=true means only match the suffix of the URL path (after stripping the query string/fragment),
    //              to avoid ".mp4" matching "mp4-tools", "/feed" matching "/feedback", etc.
    struct UrlPattern {
        const char *needle;
        URLType type;
        bool suffix;
    };

    static constexpr UrlPattern PATTERNS[] = {
        // YouTube family (more specific patterns first)
        {"youtube.com/feeds/videos.xml", URLType::YOUTUBE_RSS, false},
        {"youtube.com/playlist", URLType::YOUTUBE_PLAYLIST, false},
        {"youtube.com/watch", URLType::YOUTUBE_VIDEO, false},
        {"youtu.be/", URLType::YOUTUBE_VIDEO, false},
        {"youtube.com/@", URLType::YOUTUBE_CHANNEL, false},
        {"youtube.com/channel/", URLType::YOUTUBE_CHANNEL, false},
        {"youtube.com/c/", URLType::YOUTUBE_CHANNEL, false},
        // Y16: Bilibili (space.bilibili.com/<mid>/video → channel; bilibili.com/video/BV... → video)
        {"space.bilibili.com/", URLType::BILIBILI_CHANNEL, false},
        {"bilibili.com/video/", URLType::BILIBILI_VIDEO, false},
        {"b23.tv/", URLType::BILIBILI_VIDEO, false},
        // Y16: Douyin (douyin.com/user/<sec_uid> → user; douyin.com/video/<id> → video)
        {"douyin.com/user/", URLType::DOUYIN_USER, false},
        {"douyin.com/video/", URLType::DOUYIN_VIDEO, false},
        // Y24.11: TikTok shortlink (vm.tiktok.com/<id>) → video. The @user vs @user/video/<id>
        //   distinction is handled in classify() (both contain "tiktok.com/@", table find can't tell).
        {"vm.tiktok.com/", URLType::TIKTOK_VIDEO, false},
        {"podcasts.apple.com", URLType::APPLE_PODCAST, false},
        // Video file extensions (only match path suffix, case-insensitive)
        {".mp4", URLType::VIDEO_FILE, true},
        {".webm", URLType::VIDEO_FILE, true},
        {".mkv", URLType::VIDEO_FILE, true},
        {".avi", URLType::VIDEO_FILE, true},
        {".mov", URLType::VIDEO_FILE, true},
        // Audio stream extensions
        {".m3u8", URLType::RADIO_STREAM, true},
        {".mp3", URLType::RADIO_STREAM, true},
        {".aac", URLType::RADIO_STREAM, true},
        // OPML directory
        {"Browse.ashx", URLType::OPML, false},
        {".opml", URLType::OPML, true},
        // RSS/Atom feed
        {".xml", URLType::RSS_PODCAST, true},
        {"/feed", URLType::RSS_PODCAST, true},
        {"/rss", URLType::RSS_PODCAST, true},
    };

    // Take the path part of the URL (after the scheme up to before '?' or '#'), used for suffix matching
    static std::string url_path(const std::string &url);
    static bool ends_with_ci(const std::string &s, const char *suffix);
    // D14-3b: the explicit "is this a local file" gate (file:// or absolute path) — the check
    //   classify() applies internally as its first branch, exposed as a boolean because classify()
    //   returns RADIO_STREAM/VIDEO_FILE and cannot express "local" as a category.
    static bool is_local_file(const std::string &url);
    static URLType classify(const std::string &url);
    static std::string type_name(URLType type);
    static bool is_youtube(URLType type);
    static bool is_video(URLType type);
    static std::string extract_channel_name(const std::string &url);
    // Coarse media category for display (platform-priority; local/online + m3u8/IPTV layered on top
    //   of classify() because URLType alone can't distinguish them).
    static MediaType classifyMediaType(const std::string &url);
};

} // namespace panicast
