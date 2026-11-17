// Y23.1: Bilibili parser implementation — see header. Wraps BilibiliAPI (net) → TreeNode subtrees.
#include "panicast/parsers/bilibili_parser.h"

#include <fmt/format.h>

#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/net/bilibili_api.h"

namespace panicast
{

TreeNodePtr BilibiliParser::parse_user_videos(const std::string &sessdata, const std::string &mid,
                                              const std::string &url, const std::string &title) {
    auto feed = std::make_shared<TreeNode>();
    feed->url = url;
    feed->type = NodeType::PODCAST_FEED;
    feed->title = title;
    feed->is_youtube = false;
    feed->children_loaded =
        true; // a feed with no videos is still "loaded" (avoids infinite re-expand)
    if (mid.empty())
        return feed;

    auto videos = BilibiliAPI::fetch_user_videos(sessdata, mid);
    int vt = 0;
    for (const auto &v : videos) {
        auto ep = std::make_shared<TreeNode>();
        ep->type = NodeType::PODCAST_EPISODE;
        ep->url = v.url;
        ep->title = v.title;
        ep->duration = v.duration;
        ep->art_url = v.pic;      // cover URL (stored, not rendered in TUI)
        ep->artist = feed->title; // META-3: creator = artist
        ep->album = "Bilibili";   // META-3: platform context
        ep->track_num = ++vt;     // META-3
        ep->children_loaded = true;
        ep->parent = feed;
        feed->children.push_back(ep);
    }
    LOG(fmt::format("[Bilibili] parse_user_videos: {} videos for mid={}", videos.size(), mid));
    return feed;
}

std::vector<TreeNodePtr> BilibiliParser::parse_followings(const std::string &sessdata,
                                                          const std::string &uid, int account_id) {
    std::vector<TreeNodePtr> out;
    auto followings = BilibiliAPI::fetch_followings(sessdata, uid);
    for (const auto &f : followings) {
        auto c = std::make_shared<TreeNode>();
        c->title = f.uname;
        c->url = fmt::format("https://space.bilibili.com/{}/video", f.mid);
        c->type = NodeType::FOLDER;
        c->is_yt_channel = true; // expandable UP-master folder (spawn_load_feed → BILIBILI_CHANNEL)
        c->is_bili_up = true;    // Y23.2: 👤 icon
        c->is_youtube = false;
        c->children_loaded = false;
        c->account_id = account_id;
        if (!f.sign.empty())
            c->subtext = f.sign;
        out.push_back(c);
    }
    return out;
}

std::vector<TreeNodePtr> BilibiliParser::parse_history(const std::string &sessdata,
                                                       int account_id) {
    std::vector<TreeNodePtr> out;
    auto hist = BilibiliAPI::fetch_history(sessdata);
    for (const auto &h : hist) {
        auto c = std::make_shared<TreeNode>();
        c->title = h.title;
        c->url = h.url;
        c->type = NodeType::PODCAST_EPISODE;
        c->duration = h.duration;
        c->children_loaded = true;
        c->account_id = account_id;
        out.push_back(c);
    }
    return out;
}

} // namespace panicast
