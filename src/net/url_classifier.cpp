#include "panicast/net/url_classifier.h"

#include <algorithm>
#include <cctype>

namespace panicast
{

std::string URLClassifier::url_path(const std::string &url) {
    size_t start = 0;
    size_t scheme = url.find("://");
    if (scheme != std::string::npos)
        start = scheme + 3;
    size_t q = url.find('?', start);
    size_t h = url.find('#', start);
    size_t end = std::min(q, h);
    return url.substr(start, (end == std::string::npos) ? std::string::npos : end - start);
}

bool URLClassifier::ends_with_ci(const std::string &s, const char *suffix) {
    size_t slen = s.size(), n = 0;
    while (suffix[n])
        ++n;
    if (n == 0 || slen < n)
        return false;
    for (size_t i = 0; i < n; ++i) {
        char a = (char)std::tolower((unsigned char)s[slen - n + i]);
        char b = (char)std::tolower((unsigned char)suffix[i]);
        if (a != b)
            return false;
    }
    return true;
}

bool URLClassifier::is_local_file(const std::string &url) {
    // D14-3b: the explicit "is this a local file" gate — file:// URL or an absolute path.
    //   classify() applies this same check as its first branch but returns RADIO_STREAM/VIDEO_FILE
    //   (distinguished by extension); it cannot express "local" as a category. ASR callers
    //   (is_streaming) and the offline-transcription local-reachability gate need the boolean, so
    //   this is the single source of truth they share with classify().
    return !url.empty() && (url.rfind("file://", 0) == 0 || url[0] == '/');
}

URLType URLClassifier::classify(const std::string &url) {
    if (url.empty())
        return URLType::UNKNOWN;

    // Local files (file:// or absolute path): distinguish audio/video by extension,
    //   so local .mp4/.mkv etc. play in the video window, everything else as an audio stream.
    if (is_local_file(url)) {
        std::string lpath = url_path(url);
        static const char *vid_ext[] = {".mp4", ".m4v", ".webm", ".mkv",  ".avi", ".mov", ".flv",
                                        ".ogv", ".ogm", ".ts",   ".m2ts", ".mts", ".mpg", ".mpeg",
                                        ".vob", ".wmv", ".asf",  ".rmvb", ".3gp", ".3g2", ".f4v",
                                        ".mxf", ".y4m", ".divx", ".nut"};
        for (auto e : vid_ext) {
            if (ends_with_ci(lpath, e))
                return URLType::VIDEO_FILE;
        }
        return URLType::RADIO_STREAM;
    }

    // Tune.ashx special handling: distinguish directory browsing from audio stream
    if (url.find("Tune.ashx") != std::string::npos) {
        if (url.find("c=pbrowse") != std::string::npos ||
            url.find("c=sbrowse") != std::string::npos) {
            return URLType::OPML; // Directory browsing
        }
        return URLType::RADIO_STREAM; // Audio stream
    }

    // Y24.11: TikTok — @user profile vs @user/video/<id>. Both contain "tiktok.com/@", so the
    //   table-driven find can't distinguish; check "/video/" explicitly first.
    if (url.find("tiktok.com") != std::string::npos) {
        if (url.find("/video/") != std::string::npos)
            return URLType::TIKTOK_VIDEO;
        if (url.find("/@") != std::string::npos)
            return URLType::TIKTOK_USER;
    }

    // Table-driven matching
    std::string path = url_path(url);
    for (const auto &p : PATTERNS) {
        if (p.suffix) {
            if (ends_with_ci(path, p.needle))
                return p.type;
        } else {
            if (url.find(p.needle) != std::string::npos)
                return p.type;
        }
    }
    return URLType::UNKNOWN;
}

std::string URLClassifier::type_name(URLType type) {
    switch (type) {
    case URLType::OPML:
        return "OPML";
    case URLType::RSS_PODCAST:
        return "RSS";
    case URLType::YOUTUBE_RSS:
        return "YouTube RSS";
    case URLType::YOUTUBE_CHANNEL:
        return "YouTube Channel";
    case URLType::YOUTUBE_VIDEO:
        return "YouTube Video";
    case URLType::YOUTUBE_PLAYLIST:
        return "YouTube Playlist";
    case URLType::APPLE_PODCAST:
        return "Apple Podcast";
    case URLType::RADIO_STREAM:
        return "Radio Stream";
    case URLType::VIDEO_FILE:
        return "Video File";
    case URLType::BILIBILI_CHANNEL:
        return "Bilibili Channel";
    case URLType::BILIBILI_VIDEO:
        return "Bilibili Video";
    case URLType::DOUYIN_USER:
        return "Douyin User";
    case URLType::DOUYIN_VIDEO:
        return "Douyin Video";
    case URLType::TIKTOK_USER:
        return "TikTok User";
    case URLType::TIKTOK_VIDEO:
        return "TikTok Video";
    default:
        return "Unknown";
    }
}

bool URLClassifier::is_youtube(URLType type) {
    return type == URLType::YOUTUBE_RSS || type == URLType::YOUTUBE_CHANNEL ||
           type == URLType::YOUTUBE_VIDEO || type == URLType::YOUTUBE_PLAYLIST;
}

bool URLClassifier::is_video(URLType type) {
    // Y21 (issue 3): Bilibili/Douyin videos are DASH video streams — they must render in the video
    //   window just like YouTube/VIDEO_FILE. Without this, build_peer_list sets is_video=false and
    //   MPVController::play routes them to play_audio (no video window, no audio-file merge).
    return is_youtube(type) || type == URLType::VIDEO_FILE || type == URLType::BILIBILI_VIDEO ||
           type == URLType::DOUYIN_VIDEO || type == URLType::TIKTOK_VIDEO;
}

std::string URLClassifier::extract_channel_name(const std::string &url) {
    size_t pos;
    if ((pos = url.find("/@")) != std::string::npos) {
        std::string name = url.substr(pos + 2);
        size_t end = name.find_first_of("/?#");
        if (end != std::string::npos)
            name = name.substr(0, end);
        return name.empty() ? "YouTube Channel" : name;
    }
    if ((pos = url.find("/channel/")) != std::string::npos) {
        std::string name = url.substr(pos + 9);
        size_t end = name.find_first_of("/?#");
        if (end != std::string::npos)
            name = name.substr(0, end);
        return name.empty() ? "YouTube Channel"
                            : "ch:" + name.substr(0, std::min((size_t)8, name.length()));
    }
    if ((pos = url.find("/c/")) != std::string::npos) {
        std::string name = url.substr(pos + 3);
        size_t end = name.find_first_of("/?#");
        if (end != std::string::npos)
            name = name.substr(0, end);
        return name.empty() ? "YouTube Channel" : name;
    }
    return "YouTube Channel";
}

MediaType URLClassifier::classifyMediaType(const std::string &url) {
    if (url.empty())
        return MediaType::Radio;

    // 1. Local files (file:// or absolute path): video ext → LocalVideo, else LocalAudio.
    //    classify() already makes the VIDEO_FILE-vs-RADIO_STREAM split for local paths — reuse it.
    if (url.compare(0, 7, "file://") == 0 || url[0] == '/') {
        return (classify(url) == URLType::VIDEO_FILE) ? MediaType::LocalVideo
                                                      : MediaType::LocalAudio;
    }

    // 2. IPTV: `iptv:` scheme OR .m3u/.m3u8 stream/playlist extension. URLType lumps .m3u8 into
    //    RADIO_STREAM, so this MUST be checked before delegating to classify().
    if (url.compare(0, 5, "iptv:") == 0)
        return MediaType::Iptv;
    std::string path = url_path(url);
    if (ends_with_ci(path, ".m3u8") || ends_with_ci(path, ".m3u"))
        return MediaType::Iptv;

    // 3. Delegate to URLType. Platform patterns win over generic extensions inside classify(),
    //    so YouTube is never VIDEO_FILE and a radio stream is never RSS_PODCAST here.
    switch (classify(url)) {
    case URLType::YOUTUBE_RSS:
    case URLType::YOUTUBE_CHANNEL:
    case URLType::YOUTUBE_VIDEO:
    case URLType::YOUTUBE_PLAYLIST:
        return MediaType::Youtube;
    case URLType::BILIBILI_CHANNEL:
    case URLType::BILIBILI_VIDEO:
        return MediaType::Bilibili;
    // Douyin folds into Tiktok (the CN counterpart; not listable, kept as placeholder).
    case URLType::TIKTOK_USER:
    case URLType::TIKTOK_VIDEO:
    case URLType::DOUYIN_USER:
    case URLType::DOUYIN_VIDEO:
        return MediaType::Tiktok;
    case URLType::RADIO_STREAM:
    case URLType::OPML:
        return MediaType::Radio;
    case URLType::RSS_PODCAST:
    case URLType::APPLE_PODCAST:
        return MediaType::OnlineAudio;
    case URLType::VIDEO_FILE:
        return MediaType::OnlineVideo;
    default:
        return MediaType::Radio; // UNKNOWN fallback
    }
}

} // namespace panicast
