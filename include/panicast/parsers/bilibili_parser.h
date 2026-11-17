// Y23.1: Bilibili parser — turns BilibiliAPI data into TreeNode subtrees.
//   The net layer (net/bilibili_api.cpp) does HTTP + WBI signing and returns structs; this layer
//   builds tree nodes, mirroring YouTubeChannelParser / RSSParser in the parsers/ module.
#pragma once

#include <string>
#include <vector>

#include "panicast/core/types.h"

namespace panicast
{

class BilibiliParser {
public:
    // Build a UP master's video list as a PODCAST_FEED node (used by spawn_load_feed
    //   BILIBILI_CHANNEL). `url`/`title` are the feed's display values; sessdata may be empty
    //   (public videos). Returns a feed node with video children (empty list if mid empty/failed).
    static TreeNodePtr parse_user_videos(const std::string &sessdata, const std::string &mid,
                                         const std::string &url, const std::string &title);

    // Build followings (UP masters) as child nodes (account_id set; caller sets parent).
    static std::vector<TreeNodePtr> parse_followings(const std::string &sessdata,
                                                     const std::string &uid, int account_id);

    // Build watch-history as child nodes (account_id set; caller sets parent).
    static std::vector<TreeNodePtr> parse_history(const std::string &sessdata, int account_id);
};

} // namespace panicast
