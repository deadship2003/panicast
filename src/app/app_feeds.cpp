#include <unistd.h> // _exit (skip destructors on clean exit)
#include "panicast/app/app.h"
#include "panicast/app/actions.h"
#include "panicast/app/playback_events.h"
#include "panicast/core/event_bus.h"
#include "panicast/net/bilibili_api.h"
#include "panicast/net/douyin_api.h"
#include "panicast/net/tiktok_region.h"
#include "panicast/parsers/bilibili_parser.h"
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>

namespace panicast
{

// Y24.xx: native Douyin creator listing via DouyinApi (X-Bogus direct API, 接口C aweme/post).
//   Returns a PODCAST_FEED tree with PODCAST_EPISODE children (title=desc, url=CDN play_addr).
//   yt-dlp has no DouyinUserIE, so douyin.com/user/<sec_uid> must go through this path instead
//   of parse_tiktok_user_videos. Cookies are optional (douyin_cookie.txt); empty → anonymous
//   attempt (Douyin may still answer, or error with a "needs login cookie" hint).
static TreeNodePtr parse_douyin_user_videos(const std::string &url, const std::string &title) {
    auto result = std::make_shared<TreeNode>();
    result->url = url;
    result->type = NodeType::PODCAST_FEED;
    result->title = title;
    result->is_youtube = false;

    // Extract sec_uid from douyin.com/user/<sec_uid> (already normalized by tiktok_subscribe).
    std::string sec_uid;
    size_t pos = url.find("/user/");
    if (pos != std::string::npos) {
        sec_uid = url.substr(pos + 6);
        size_t q = sec_uid.find_first_of("/?#");
        if (q != std::string::npos)
            sec_uid = sec_uid.substr(0, q);
    }
    if (sec_uid.empty()) {
        result->children_loaded = true;
        result->parse_failed = true;
        result->error_msg = "Douyin user URL missing sec_uid";
        return result;
    }

    // Cookies (optional) from douyin_cookie.txt (Netscape). Empty → anonymous attempt.
    std::string cookie_header;
    std::string ck = IniConfig::instance().get_tiktok_douyin_cookies_file();
    std::error_code ec;
    if (!ck.empty() && std::filesystem::exists(ck, ec)) {
        std::ifstream cf(ck);
        std::string content((std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
        cookie_header = DouyinApi::build_cookie_header_from_txt(content, "douyin.com");
    }

    DouyinApi api;
    api.setSession(cookie_header); // fixed Chrome UA (matches browser_* params)

    constexpr int PER_PAGE = 20;
    constexpr int MAX_PAGES = 5; // cap at ~100 videos to avoid 风控
    long long cursor = 0;
    std::string last_err;

    for (int page = 0; page < MAX_PAGES; ++page) {
        DouyinApi::UserVideoResult page_res;
        api.fetchUserVideos(sec_uid, cursor, PER_PAGE,
                            [&](const DouyinApi::UserVideoResult &r) { page_res = r; });
        if (!page_res.ok) {
            last_err = page_res.err;
            if (result->children.empty()) {
                EVENT_LOG(fmt::format(
                    "T: Douyin listing failed for {} — {} (可能需登录 Cookie：Ctrl+B 导入 "
                    "douyin cookies.txt)",
                    sec_uid, page_res.err.empty() ? "no data" : page_res.err));
            }
            break;
        }
        for (const auto &v : page_res.videos) {
            if (v.playUrl.empty())
                continue; // ad / deleted entries have no play_addr
            auto ep = std::make_shared<TreeNode>();
            ep->type = NodeType::PODCAST_EPISODE;
            ep->url = v.playUrl; // CDN direct link (Referer injected at playback, see mpv_play.cpp)
            ep->title = v.desc.empty() ? v.awemeId : v.desc;
            ep->duration = v.duration;
            ep->children_loaded = true;
            ep->parent = result;
            result->children.push_back(ep);
        }
        if (!page_res.hasMore)
            break;
        cursor = page_res.nextCursor;
        std::this_thread::sleep_for(std::chrono::milliseconds(800)); // 防风控
    }

    result->children_loaded = true;
    if (result->children.empty()) {
        result->parse_failed = true;
        result->error_msg = last_err.empty() ? "No videos" : last_err;
    }
    return result;
}

void App::spawn_load_radio(TreeNodePtr node, bool force) {
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        if (node->loading)
            return;
        node->loading = true;
    }

    // Unified print of the full URL
    EVENT_LOG(fmt::format("Loading: [RADIO] {}", node->url));

    if (node->children_loaded && !force) {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        node->loading = false;
        node->expanded = true;
        return;
    }

    // Use thread pool (old code: std::thread([this,node]{...}).detach();)
    pool_.submit([this, node]() {
        std::string url = node->url;
        // Reset XML error state
        reset_xml_error_state();

        std::string data = Network::fetch(url);
        if (!data.empty()) {
            auto parsed = OPMLParser::parse(data);
            if (parsed) {
                std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                node->children = parsed->children;
                node->children_loaded = true;
                node->expanded = true;
                node->parse_failed = false;
                node->is_cached = true; // parsed — episode_cache is the persistence
                EVENT_LOG(fmt::format("Loaded: {} items", node->children.size()));
                Persistence::save_cache(library_.radio_root(), library_.podcast_root());
            }
        }
        {
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            node->loading = false;
        }

        // Sync LINK node status
        sync_link_node_status(node);
    });
}

void App::spawn_load_feed(TreeNodePtr node) {
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        if (node->loading)
            return;
        node->loading = true;
        node->parse_failed = false;
    }

    // Reset XML error state
    reset_xml_error_state();

    std::string url = node->url;
    URLType url_type = URLClassifier::classify(url);

    // Unified print of the full URL
    EVENT_LOG(fmt::format("Loading: [{}] {}", URLClassifier::type_name(url_type), url));

    // Use thread pool (old code: std::thread detach)
    // Note: Apple Podcast feed lookup (a network request) runs inside the thread to avoid blocking the UI thread.
    pool_.submit([this, node, url, url_type]() {
        // Reset XML error state (inside thread)
        reset_xml_error_state();

        // Y24.40: parse + commit delegated to focused helpers.
        std::string cur_url;
        bool abort = false;
        TreeNodePtr result = parse_feed_by_type(node, url, url_type, cur_url, abort);
        if (abort)
            return; // Apple lookup failed; node state already set inside parse_feed_by_type
        commit_feed_result(node, result, cur_url);
    });
}

// Apple-lookup + YouTube-cache + ParserRegistry dispatch. Produces the parsed result tree.
//   cur_url_out receives the (possibly Apple-rewritten) URL used for parsing and the episode-cache key.
//   abort_out=true means Apple lookup failed and node state is already set; caller must skip commit.
TreeNodePtr App::parse_feed_by_type(TreeNodePtr node, const std::string &url, URLType url_type,
                                    std::string &cur_url_out, bool &abort_out) {
    abort_out = false;

    // Apple Podcast: look up the feed URL first (network request, in-thread to avoid blocking the UI for 30s)
    std::string cur_url = url;
    URLType cur_type = url_type;
    if (cur_type == URLType::APPLE_PODCAST) {
        std::string feed = Network::lookup_apple_feed(cur_url);
        if (!feed.empty()) {
            cur_url = feed;
            cur_type = URLClassifier::classify(cur_url);
            EVENT_LOG(fmt::format("Apple -> {}", cur_url));
        } else {
            // lookup_apple_feed() already emitted a detailed EVENT_LOG with the reason + retry count.
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            node->loading = false;
            node->parse_failed = true;
            node->error_msg = "Apple lookup failed (see LOG)";
            abort_out = true;
            cur_url_out = cur_url;
            return nullptr;
        }
    }
    cur_url_out = cur_url;

    TreeNodePtr result = nullptr;

    switch (cur_type) {
    case URLType::YOUTUBE_CHANNEL:
    case URLType::YOUTUBE_PLAYLIST: {
        // Channels (bare -> enumerate tabs / tab segment -> episode list) and playlists (-> episode list)
        //   are uniformly dispatched by YouTubeChannelParser::parse based on URL form
        //   via ParserRegistry self-registration (YouTube parsing brings its own network/yt-dlp, no pre-fetch needed)
        // BUG5: consult YouTubeCache first — on a cache hit, build the result tree directly
        //   from the cached video list and skip the network/yt-dlp parse entirely. On a miss,
        //   parse normally and feed the parsed children back into the cache.
        // Y23.3: a cache hit with an EMPTY video list is a stale bare-channel entry (pre-Y23.3
        //   bug: tabs were skipped → empty list cached → next load "No items found" → ✗). Treat
        //   an empty cache entry as a miss and re-parse.
        bool cache_hit = false;
        if (YouTubeCache::instance().has(cur_url)) {
            auto cache = YouTubeCache::instance().get(cur_url);
            if (!cache.videos.empty()) {
                result = std::make_shared<TreeNode>();
                result->is_youtube = true;
                result->type = NodeType::PODCAST_FEED;
                result->title = cache.channel_name;
                result->channel_name = cache.channel_name;
                int vt = 0;
                for (const auto &v : cache.videos) {
                    auto ep = std::make_shared<TreeNode>();
                    ep->type = NodeType::PODCAST_EPISODE;
                    ep->title = v.title;
                    ep->url = v.url;
                    ep->is_youtube = true;
                    ep->art_url = v.thumbnail; // ART-2
                    ep->artist =
                        cache.channel_name.empty() ? result->title : cache.channel_name; // META-2
                    ep->album = "YouTube";                                               // META-2
                    ep->track_num = ++vt;
                    ep->children_loaded = true;
                    ep->parent = result;
                    result->children.push_back(ep);
                }
                EVENT_LOG(fmt::format("YouTube cache hit: {} videos for {}", cache.videos.size(),
                                      cur_url));
                cache_hit = true;
            }
        }
        if (!cache_hit) {
            cache_youtube_videos(node, cur_type, cur_url, result);
        }
        break;
    } // close case YOUTUBE_CHANNEL/PLAYLIST
    case URLType::YOUTUBE_RSS:
    case URLType::RSS_PODCAST: {
        // Dispatch via ParserRegistry self-registration; fetch still happens here (consistent with original behavior)
        if (auto p = ParserRegistry::instance().create(cur_type)) {
            std::string data = Network::fetch(cur_url);
            if (!data.empty())
                result = p->parse({data, cur_url});
        }
        break;
    }
    case URLType::OPML: {
        if (auto p = ParserRegistry::instance().create(cur_type)) {
            std::string data = Network::fetch(cur_url);
            if (!data.empty())
                result = p->parse({data, cur_url});
        }
        break;
    }
    // Y18: Bilibili uses WBI-signed arc/search API (real titles); Douyin uses yt-dlp flat-playlist.
    case URLType::BILIBILI_CHANNEL: {
        // Y18: Only WBI-signed API (real titles). No flat-playlist fallback.
        //   arc/search doesn't need SESSDATA (public videos), only WBI signing.
        //   SESSDATA is read if available (for member-only content) but not required.
        std::string mid;
        auto pos = cur_url.find("space.bilibili.com/");
        if (pos != std::string::npos) {
            mid = cur_url.substr(pos + 19);
            auto slash = mid.find('/');
            if (slash != std::string::npos)
                mid = mid.substr(0, slash);
        }
        std::string sessdata;
        std::string ck = IniConfig::instance().get_bilibili_cookies_file();
        std::error_code ec;
        if (!ck.empty() && std::filesystem::exists(ck, ec)) {
            std::ifstream cf(ck);
            std::string content((std::istreambuf_iterator<char>(cf)),
                                std::istreambuf_iterator<char>());
            sessdata = BilibiliAPI::extract_sessdata_from_cookies_txt(content);
        }
        result = BilibiliParser::parse_user_videos(sessdata, mid, cur_url, node->title);
        break;
    }
    case URLType::DOUYIN_USER:
        // Y24.xx: native Douyin creator listing via DouyinApi (X-Bogus direct API 接口C).
        //   yt-dlp has no DouyinUserIE, so douyin.com/user/<sec_uid> is handled here instead.
        result = parse_douyin_user_videos(cur_url, node->title);
        break;
    case URLType::TIKTOK_USER: {
        // Y24.11: TikTok creator video listing via yt-dlp --flat-playlist + geo-bypass.
        std::string region = tiktok_region_;
        std::string cookies = IniConfig::instance().get_tiktok_cookies_file();
        result = parse_tiktok_user_videos(cur_url, region, cookies, node->title);
        break;
    }
    default: {
        std::string data = Network::fetch(cur_url);
        if (!data.empty())
            result = RSSParser::parse(data, cur_url);
        break;
    }
    }

    return result;
}

// YouTube cache miss: parse via ParserRegistry and backfill YouTubeCache with the parsed videos.
void App::cache_youtube_videos(TreeNodePtr node, URLType cur_type, const std::string &cur_url,
                               TreeNodePtr &result) {
    if (auto p = ParserRegistry::instance().create(cur_type)) {
        result = p->parse({"", cur_url});
        // BUG5: persist the parsed result into the YouTube cache for next time.
        if (result && !result->children.empty()) {
            std::vector<YouTubeVideoInfo> videos;
            videos.reserve(result->children.size());
            for (const auto &child : result->children) {
                if (child->type != NodeType::PODCAST_EPISODE)
                    continue;
                // Extract video id from watch?v=ID URL (best-effort; empty if not found).
                std::string id;
                size_t vp = child->url.find("v=");
                if (vp != std::string::npos) {
                    size_t start = vp + 2;
                    size_t end = child->url.find_first_of("&", start);
                    id = child->url.substr(start, end == std::string::npos ? std::string::npos
                                                                           : end - start);
                }
                videos.push_back({id, child->title, child->url, child->art_url});
            }
            std::string ch_name = result->channel_name;
            if (ch_name.empty())
                ch_name = result->title;
            if (ch_name.empty())
                ch_name = node->title;
            // Y23.3: only cache a NON-EMPTY video list. A bare channel parses to TAB
            //   children (PODCAST_FEED), which the loop above skips → videos empty. Caching
            //   that empty list made the next load hit the cache with 0 videos → "No items
            //   found" → ✗. Bare channels (tabs) are left uncached so they re-parse each time.
            if (!videos.empty()) {
                YouTubeCache::instance().update(cur_url, ch_name, videos);
            }
        }
    }
}

// Commit the parsed result: write children/error state under tree_mutex, then persist
//   episode_cache + full-tree cache OUTSIDE the lock (per-episode DB writes would block UI draw/flatten).
void App::commit_feed_result(TreeNodePtr node, TreeNodePtr result, const std::string &cur_url) {
    // Build the episode snapshot outside the lock first (result->children is local to this task, no lock needed),
    //   to avoid keeping the time-consuming per-episode DB writes inside the lock and blocking UI draw()/flatten().
    bool has_children = result && !result->children.empty();
    json episodes_json = json::array();
    if (has_children) {
        for (const auto &child : result->children) {
            episodes_json.push_back({
                {"url", child->url},
                {"title", child->title},
                {"duration", child->duration},
                {"is_youtube", child->is_youtube},
                {"has_subtitle", child->has_subtitle}, // Y23.10: persist 📜 flag
                {"subtitle_url", child->subtitle_url}  // Y23.10: persist transcript URL
            });
        }
    }

    // Hold the lock only for tree-structure changes + lightweight subscription persistence (subscription list is small, fast),
    //   ensuring the loading state and draw()'s is_loading detection stay in sync.
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        if (result) {
            // ART-2: the parsed channel carries the feed cover (itunes:image) — the
            //   live subscription node keeps its identity but ADOPTS the artwork and
            //   the description line; without this every refresh dropped them.
            if (!result->art_url.empty())
                node->art_url = result->art_url;
            if (!result->subtext.empty() && node->subtext.empty())
                node->subtext = result->subtext;
            if (has_children) {
                node->children = result->children;
                int vt = 0;
                for (auto &c : node->children) {
                    c->parent = node; // BUG1: reset parent (result was a temporary)
                    // META-2: YouTube channel/playlist feeds — display metadata for the
                    //   remote rows (live-parse children get it here; cache-path nodes
                    //   already carry it and are skipped by the emptiness guards).
                    if (c->is_youtube || node->is_youtube) {
                        if (c->artist.empty())
                            c->artist =
                                node->channel_name.empty() ? node->title : node->channel_name;
                        if (c->album.empty())
                            c->album = "YouTube";
                        if (c->track_num == 0)
                            c->track_num = ++vt;
                    }
                }
                node->children_loaded = true;
                node->expanded = true; // auto-expand to show the loaded content
                node->parse_failed = false;
                node->is_cached = true;
                if (result->is_youtube)
                    node->is_youtube = true;
                EVENT_LOG(fmt::format("Loaded: {} items", result->children.size()));
                // Y24: mark the feed node with 📜 when ANY of its episodes has a transcript,
                //   so users can spot transcript-bearing feeds in the collapsed subscription
                //   list. (Y23.10 used "all", which left feeds like DOAC — where most but not
                //   every episode has a transcript — unmarked.) Persisted via save_tree below.
                int sub_count = 0;
                for (const auto &c : node->children)
                    if (c->has_subtitle)
                        ++sub_count;
                node->has_subtitle = (sub_count > 0);
                if (sub_count > 0)
                    LOG(fmt::format("[RSS] feed '{}' marked 📜 ({} of {} episodes have transcript)",
                                    node->title, sub_count, node->children.size()));
                // P2-C13: the full-tree save_cache is moved OUTSIDE the lock (below) so it
                //   doesn't block UI draw()/flatten() on every feed load.
                // Save the subscription list to the database (ensure correct loading after restart)
                Persistence::save_data(library_.podcast_root(), library_.fav_root());
            } else if (result->parse_failed) {
                node->parse_failed = true;
                node->error_msg = result->error_msg;
                EVENT_LOG(fmt::format("Parse failed: {}", result->error_msg));
            } else {
                node->parse_failed = true;
                node->error_msg = "No items found";
                EVENT_LOG("No items found in feed");
            }
        } else {
            node->parse_failed = true;
            node->error_msg = "Parser returned null";
            EVENT_LOG("Parser returned null");
        }
        // Set loading=false at the end to ensure correct state-update order
        node->loading = false;
        // Sync LINK node status (after the target finishes loading, sync LINK nodes referencing it)
        sync_link_node_status(node);
    } // release tree_mutex

    // Time-consuming DB writes outside the lock (episode cache: per-episode INSERT loop + json dump).
    //   Does not read the tree; does not block UI draw()/flatten().
    if (has_children) {
        node->is_cached = true; // parsed — episode_cache (saved below) is the persistence
        DatabaseManager::instance().save_episode_cache(cur_url, episodes_json.dump());
        // P2-C13: full-tree cache save outside the lock (best-effort; a concurrent tree
        //   mutation only makes the restart cache slightly stale, never corrupts live state).
        Persistence::save_cache(library_.radio_root(), library_.podcast_root());
    }
}

// Built-in global popular podcasts; deleted subscriptions are not auto-restored on restart
void App::load_default_podcasts() {
    // DB query runs outside the lock to avoid blocking other lock-waiting threads while holding tree_mutex
    auto removed = DatabaseManager::instance().load_removed_defaults();
    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
    struct BuiltinPodcast {
        const char *name;
        const char *url;
    };

    // Collect existing URLs to avoid duplicate additions
    std::set<std::string> existing_urls;
    for (const auto &child : library_.podcast_root()) {
        if (!child->url.empty()) {
            existing_urls.insert(child->url);
        }
    }

    // TOP200 Apple Podcasts popular podcasts (updated 2026-03-26)
    // Data source: Apple RSS Marketing Tools API
    // Categories: News, Comedy, True Crime, Business,
    //             Society & Culture, History, Sports,
    //             Health & Fitness, Education, Religion, Technology
    std::vector<BuiltinPodcast> defaults = {
        // ═══════════════════════════════════════════════════════════════════
        // [TOP1-20 popular podcasts]
        // ═══════════════════════════════════════════════════════════════════
        {"Call Her Daddy", "https://podcasts.apple.com/us/podcast/call-her-daddy/id1418960261"},
        {"The Joe Rogan Experience",
         "https://podcasts.apple.com/us/podcast/the-joe-rogan-experience/id360084272"},
        {"The Rest Is History",
         "https://podcasts.apple.com/us/podcast/the-rest-is-history/id1537788786"},
        {"Good Hang with Amy Poehler",
         "https://podcasts.apple.com/us/podcast/good-hang-with-amy-poehler/id1795483480"},
        {"The Mel Robbins Podcast",
         "https://podcasts.apple.com/us/podcast/the-mel-robbins-podcast/id1646101002"},
        {"The Daily", "https://podcasts.apple.com/us/podcast/the-daily/id1200361736"},
        {"The Diary Of A CEO", "https://podcasts.apple.com/us/podcast/"
                               "the-diary-of-a-ceo-with-steven-bartlett/id1291423644"},
        {"Crime Junkie", "https://podcasts.apple.com/us/podcast/crime-junkie/id1322200189"},
        {"The Weekly Show with Jon Stewart",
         "https://podcasts.apple.com/us/podcast/the-weekly-show-with-jon-stewart/id1583132133"},
        {"Dateline NBC", "https://podcasts.apple.com/us/podcast/dateline-nbc/id1464919521"},
        {"The Secret World of Roald Dahl",
         "https://podcasts.apple.com/us/podcast/the-secret-world-of-roald-dahl/id1868436905"},
        {"48 Hours", "https://podcasts.apple.com/us/podcast/48-hours/id965818306"},
        {"Love Trapped", "https://podcasts.apple.com/us/podcast/love-trapped/id1878220033"},
        {"Pardon My Take", "https://podcasts.apple.com/us/podcast/pardon-my-take/id1089022756"},
        {"Pod Save America", "https://podcasts.apple.com/us/podcast/pod-save-america/id1192761536"},
        {"20/20", "https://podcasts.apple.com/us/podcast/20-20/id987967575"},
        {"Morbid", "https://podcasts.apple.com/us/podcast/morbid/id1379959217"},
        {"The Ezra Klein Show",
         "https://podcasts.apple.com/us/podcast/the-ezra-klein-show/id1548604447"},
        {"The Bill Simmons Podcast",
         "https://podcasts.apple.com/us/podcast/the-bill-simmons-podcast/id1043699613"},
        {"SmartLess", "https://podcasts.apple.com/us/podcast/smartless/id1521578868"},

        // ═══════════════════════════════════════════════════════════════════
        // [TOP21-50 News & Politics]
        // ═══════════════════════════════════════════════════════════════════
        {"This Past Weekend w/ Theo Von",
         "https://podcasts.apple.com/us/podcast/this-past-weekend-w-theo-von/id1190981360"},
        {"Stuff You Should Know",
         "https://podcasts.apple.com/us/podcast/stuff-you-should-know/id278981407"},
        {"The Bulwark Podcast",
         "https://podcasts.apple.com/us/podcast/the-bulwark-podcast/id1447684472"},
        {"The Shawn Ryan Show",
         "https://podcasts.apple.com/us/podcast/the-shawn-ryan-show/id1492492083"},
        {"The MeidasTouch Podcast",
         "https://podcasts.apple.com/us/podcast/the-meidastouch-podcast/id1510240831"},
        {"The Toast", "https://podcasts.apple.com/us/podcast/the-toast/id1368081567"},
        {"Trace of Suspicion",
         "https://podcasts.apple.com/us/podcast/trace-of-suspicion/id1880711858"},
        {"Up First from NPR",
         "https://podcasts.apple.com/us/podcast/up-first-from-npr/id1222114325"},
        {"Candace", "https://podcasts.apple.com/us/podcast/candace/id1750591415"},
        {"The Megyn Kelly Show",
         "https://podcasts.apple.com/us/podcast/the-megyn-kelly-show/id1532976305"},
        {"Armchair Expert",
         "https://podcasts.apple.com/us/podcast/armchair-expert-with-dax-shepard/id1345682353"},
        {"Giggly Squad", "https://podcasts.apple.com/us/podcast/giggly-squad/id1536352412"},
        {"Real Vikings", "https://podcasts.apple.com/us/podcast/real-vikings/id1876255143"},
        {"Betrayal Season 5",
         "https://podcasts.apple.com/us/podcast/betrayal-season-5/id1615637724"},
        {"My Favorite Murder",
         "https://podcasts.apple.com/us/podcast/my-favorite-murder/id1074507850"},
        {"The Rest Is Politics",
         "https://podcasts.apple.com/us/podcast/the-rest-is-politics/id1611374685"},
        {"The Ben Shapiro Show",
         "https://podcasts.apple.com/us/podcast/the-ben-shapiro-show/id1047335260"},
        {"Bridge of Lies", "https://podcasts.apple.com/us/podcast/bridge-of-lies/id1874641982"},
        {"Global News Podcast",
         "https://podcasts.apple.com/us/podcast/global-news-podcast/id135067274"},
        {"MrBallen Podcast", "https://podcasts.apple.com/us/podcast/mrballen-podcast/id1608813794"},
        {"The Tucker Carlson Show",
         "https://podcasts.apple.com/us/podcast/the-tucker-carlson-show/id1719657632"},
        {"Conan O'Brien Needs A Friend",
         "https://podcasts.apple.com/us/podcast/conan-obrien-needs-a-friend/id1438054347"},
        {"The Rest Is Politics: US",
         "https://podcasts.apple.com/us/podcast/the-rest-is-politics-us/id1743030473"},
        {"The Bible in a Year",
         "https://podcasts.apple.com/us/podcast/the-bible-in-a-year/id1539568321"},
        {"The Ramsey Show", "https://podcasts.apple.com/us/podcast/the-ramsey-show/id77001367"},
        {"REAL AF with Andy Frisella",
         "https://podcasts.apple.com/us/podcast/real-af-with-andy-frisella/id1012570406"},
        {"The Deck", "https://podcasts.apple.com/us/podcast/the-deck/id1603011962"},
        {"Huberman Lab", "https://podcasts.apple.com/us/podcast/huberman-lab/id1545953110"},
        {"The Dan Le Batard Show",
         "https://podcasts.apple.com/us/podcast/the-dan-le-batard-show/id934820588"},
        {"This American Life",
         "https://podcasts.apple.com/us/podcast/this-american-life/id201671138"},

        // ═══════════════════════════════════════════════════════════════════
        // [TOP51-100 more popular]
        // ═══════════════════════════════════════════════════════════════════
        {"Mick Unplugged", "https://podcasts.apple.com/us/podcast/mick-unplugged/id1731755953"},
        {"Matt and Shane's Secret Podcast",
         "https://podcasts.apple.com/us/podcast/matt-and-shanes-secret-podcast/id1177068388"},
        {"Off Duty | The Guardian", "https://podcasts.apple.com/us/podcast/off-duty/id1731314182"},
        {"The News Agents", "https://podcasts.apple.com/us/podcast/the-news-agents/id1640878689"},
        {"The Louis Theroux Podcast",
         "https://podcasts.apple.com/us/podcast/the-louis-theroux-podcast/id1725833532"},
        {"The Romesh Ranganathan Show",
         "https://podcasts.apple.com/us/podcast/the-romesh-ranganathan-show/id1838594107"},
        {"Snapped: Women Who Murder",
         "https://podcasts.apple.com/us/podcast/snapped-women-who-murder/id1145089790"},
        {"Pivot", "https://podcasts.apple.com/us/podcast/pivot/id1073226719"},
        {"The Rest Is Entertainment",
         "https://podcasts.apple.com/us/podcast/the-rest-is-entertainment/id1718287198"},
        {"Parenting Hell", "https://podcasts.apple.com/us/podcast/parenting-hell/id1510251497"},
        {"Page 94: The Private Eye Podcast",
         "https://podcasts.apple.com/us/podcast/page-94/id973958702"},
        {"The Dan Bongino Show",
         "https://podcasts.apple.com/us/podcast/the-dan-bongino-show/id965293227"},
        {"The Rest Is Science",
         "https://podcasts.apple.com/us/podcast/the-rest-is-science/id1853007888"},
        {"Newscast", "https://podcasts.apple.com/us/podcast/newscast/id1234185718"},
        {"The Book Club", "https://podcasts.apple.com/us/podcast/the-book-club/id1876049295"},
        {"Off Menu", "https://podcasts.apple.com/us/podcast/off-menu/id1442950743"},
        {"Freakonomics Radio",
         "https://podcasts.apple.com/us/podcast/freakonomics-radio/id354668519"},
        {"Hanging Out With Ant & Dec",
         "https://podcasts.apple.com/us/podcast/hanging-out/id1867521360"},
        {"Dish", "https://podcasts.apple.com/us/podcast/dish/id1626354833"},
        {"Morning Wire", "https://podcasts.apple.com/us/podcast/morning-wire/id1576594336"},
        {"Sh**ged Married Annoyed",
         "https://podcasts.apple.com/us/podcast/sh-ged-married-annoyed/id1451489585"},
        {"No Such Thing As A Fish",
         "https://podcasts.apple.com/us/podcast/no-such-thing-as-a-fish/id840986946"},
        {"NPR News Now", "https://podcasts.apple.com/us/podcast/npr-news-now/id121493675"},
        {"Foundling | Tortoise", "https://podcasts.apple.com/us/podcast/foundling/id1590561275"},
        {"Elis James and John Robins",
         "https://podcasts.apple.com/us/podcast/elis-james-john-robins/id1465290044"},
        {"Off Air with Jane & Fi", "https://podcasts.apple.com/us/podcast/off-air/id1648663774"},
        {"The Joe Budden Podcast",
         "https://podcasts.apple.com/us/podcast/the-joe-budden-podcast/id1535809341"},
        {"Wolf & Owl", "https://podcasts.apple.com/us/podcast/wolf-owl/id1540826523"},
        {"Focus: Adults in the Room", "https://podcasts.apple.com/us/podcast/focus/id1733735613"},
        {"The Idiot", "https://podcasts.apple.com/us/podcast/the-idiot/id1884735227"},
        {"Happy Place", "https://podcasts.apple.com/us/podcast/happy-place/id1353058891"},
        {"The Dylan Gemelli Podcast",
         "https://podcasts.apple.com/us/podcast/the-dylan-gemelli/id1780873400"},
        {"We Need To Talk", "https://podcasts.apple.com/us/podcast/we-need-to-talk/id1765126946"},
        {"The Viall Files", "https://podcasts.apple.com/us/podcast/the-viall-files/id1448210981"},
        {"Fin vs History", "https://podcasts.apple.com/us/podcast/fin-vs-history/id1790458615"},
        {"My Therapist Ghosted Me",
         "https://podcasts.apple.com/us/podcast/my-therapist-ghosted-me/id1560176558"},
        {"Last Podcast On The Left",
         "https://podcasts.apple.com/us/podcast/last-podcast-on-the-left/id437299706"},
        {"Today in Focus", "https://podcasts.apple.com/us/podcast/today-in-focus/id1440133626"},
        {"RedHanded", "https://podcasts.apple.com/us/podcast/redhanded/id1250599915"},
        {"The Rest Is Classified",
         "https://podcasts.apple.com/us/podcast/the-rest-is-classified/id1780384916"},
        {"Rosebud with Gyles Brandreth",
         "https://podcasts.apple.com/us/podcast/rosebud/id1704806594"},
        {"Staying Relevant", "https://podcasts.apple.com/us/podcast/staying-relevant/id1651140064"},
        {"Unblinded with Sean Callagy",
         "https://podcasts.apple.com/us/podcast/unblinded/id1844970260"},
        {"WW2 Pod: We Have Ways", "https://podcasts.apple.com/us/podcast/ww2-pod/id1457552694"},
        {"Park Predators", "https://podcasts.apple.com/us/podcast/park-predators/id1517651197"},
        {"Pod Save the World",
         "https://podcasts.apple.com/us/podcast/pod-save-the-world/id1200016351"},
        {"Short History Of...",
         "https://podcasts.apple.com/us/podcast/short-history-of/id1579040306"},
        {"Last One Laughing Podcast",
         "https://podcasts.apple.com/us/podcast/last-one-laughing/id1885620207"},
        {"Fresh Air", "https://podcasts.apple.com/us/podcast/fresh-air/id214089682"},
        {"Football Weekly", "https://podcasts.apple.com/us/podcast/football-weekly/id188674007"},

        // ═══════════════════════════════════════════════════════════════════
        // [TOP101-150 News & Business]
        // ═══════════════════════════════════════════════════════════════════
        {"IHIP News", "https://podcasts.apple.com/us/podcast/ihip-news/id1761444284"},
        {"Empire: World History", "https://podcasts.apple.com/us/podcast/empire/id1639561921"},
        {"LuAnna: The Podcast", "https://podcasts.apple.com/us/podcast/luanna/id1496019465"},
        {"Money Rehab with Nicole Lapin",
         "https://podcasts.apple.com/us/podcast/money-rehab/id1559564016"},
        {"British Scandal", "https://podcasts.apple.com/us/podcast/british-scandal/id1563775446"},
        {"Dig It with Jo Whiley", "https://podcasts.apple.com/us/podcast/dig-it/id1825368127"},
        {"Alan Carr's Life's a Beach",
         "https://podcasts.apple.com/us/podcast/lifes-a-beach/id1550998864"},
        {"Habits and Hustle",
         "https://podcasts.apple.com/us/podcast/habits-and-hustle/id1451897026"},
        {"The Rest Is Football",
         "https://podcasts.apple.com/us/podcast/the-rest-is-football/id1701022490"},
        {"Young and Profiting",
         "https://podcasts.apple.com/us/podcast/young-profiting/id1368888880"},
        {"Americast", "https://podcasts.apple.com/us/podcast/americast/id1473150244"},
        {"Proven Podcast", "https://podcasts.apple.com/us/podcast/proven-podcast/id1744386875"},
        {"Stick to Football",
         "https://podcasts.apple.com/us/podcast/stick-to-football/id1709142395"},
        {"Help I Sexted My Boss",
         "https://podcasts.apple.com/us/podcast/help-i-sexted/id1357127065"},
        {"Fraudacious", "https://podcasts.apple.com/us/podcast/fraudacious/id1879610796"},
        {"FT News Briefing", "https://podcasts.apple.com/us/podcast/ft-news-briefing/id1438449989"},
        {"The Matt Walsh Show",
         "https://podcasts.apple.com/us/podcast/the-matt-walsh-show/id1367210511"},
        {"Great Company with Jamie Laing",
         "https://podcasts.apple.com/us/podcast/great-company/id1735702250"},
        {"Spittin Chiclets", "https://podcasts.apple.com/us/podcast/spittin-chiclets/id1112425552"},
        {"Three Bean Salad", "https://podcasts.apple.com/us/podcast/three-bean-salad/id1564066507"},
        {"Casefile True Crime",
         "https://podcasts.apple.com/us/podcast/casefile-true-crime/id998568017"},
        {"Raging Moderates", "https://podcasts.apple.com/us/podcast/raging-moderates/id1774505095"},
        {"Front Burner", "https://podcasts.apple.com/us/podcast/front-burner/id1439621628"},
        {"Dan Snow's History Hit",
         "https://podcasts.apple.com/us/podcast/history-hit/id1042631089"},
        {"Someone Knows Something",
         "https://podcasts.apple.com/us/podcast/someone-knows-something/id1089216339"},
        {"The Vault Unlocked",
         "https://podcasts.apple.com/us/podcast/the-vault-unlocked/id1837193185"},
        {"The Archers", "https://podcasts.apple.com/us/podcast/the-archers/id265970428"},
        {"OverDrive", "https://podcasts.apple.com/us/podcast/overdrive/id679367618"},
        {"Desert Island Discs",
         "https://podcasts.apple.com/us/podcast/desert-island-discs/id342735925"},
        {"All-In Podcast", "https://podcasts.apple.com/us/podcast/all-in/id1502871393"},
        {"The Rewatchables", "https://podcasts.apple.com/us/podcast/the-rewatchables/id1268527882"},
        {"Joe Marler Will See You Now",
         "https://podcasts.apple.com/us/podcast/joe-marler/id1850736713"},
        {"The Therapy Crouch",
         "https://podcasts.apple.com/us/podcast/the-therapy-crouch/id1665665408"},
        {"Bad Friends", "https://podcasts.apple.com/us/podcast/bad-friends/id1496265971"},
        {"Feel Better, Live More",
         "https://podcasts.apple.com/us/podcast/feel-better-live-more/id1333552422"},
        {"Watch What Crappens",
         "https://podcasts.apple.com/us/podcast/watch-what-crappens/id498130432"},
        {"That Peter Crouch Podcast",
         "https://podcasts.apple.com/us/podcast/that-peter-crouch/id1616744464"},
        {"The Bridge with Peter Mansbridge",
         "https://podcasts.apple.com/us/podcast/the-bridge/id1478036186"},
        {"World Report", "https://podcasts.apple.com/us/podcast/world-report/id278657031"},
        {"Chatabix", "https://podcasts.apple.com/us/podcast/chatabix/id1560965008"},
        {"The Cult Queen of Canada",
         "https://podcasts.apple.com/us/podcast/cult-queen/id1364665348"},
        {"The Rest Is Politics: Leading",
         "https://podcasts.apple.com/us/podcast/rest-is-politics-leading/id1665265193"},
        {"The Journal", "https://podcasts.apple.com/us/podcast/the-journal/id1469394914"},
        {"Begin Again with Davina McCall",
         "https://podcasts.apple.com/us/podcast/begin-again/id1773104705"},
        {"The Bible Recap", "https://podcasts.apple.com/us/podcast/the-bible-recap/id1440833267"},
        {"Museum of Pop Culture",
         "https://podcasts.apple.com/us/podcast/museum-pop-culture/id1863943807"},
        {"Frank Skinner Podcast",
         "https://podcasts.apple.com/us/podcast/frank-skinner/id308800732"},
        {"Adam Carolla Show", "https://podcasts.apple.com/us/podcast/adam-carolla/id306390087"},
        {"ZOE Science & Nutrition",
         "https://podcasts.apple.com/us/podcast/zoe-science-nutrition/id1611216298"},
        {"Blair & Barker", "https://podcasts.apple.com/us/podcast/blair-barker/id541972447"},

        // ═══════════════════════════════════════════════════════════════════
        // [TOP151-200 more popular podcasts]
        // ═══════════════════════════════════════════════════════════════════
        {"Digital Social Hour",
         "https://podcasts.apple.com/us/podcast/digital-social-hour/id1676846015"},
        {"The Daily Show: Ears Edition",
         "https://podcasts.apple.com/us/podcast/the-daily-show/id1334878780"},
        {"Table Manners", "https://podcasts.apple.com/us/podcast/table-manners/id1305228910"},
        {"32 Thoughts: The Podcast",
         "https://podcasts.apple.com/us/podcast/32-thoughts/id1332150124"},
        {"How To Fail With Elizabeth Day",
         "https://podcasts.apple.com/us/podcast/how-to-fail/id1407451189"},
        {"The Determined Society",
         "https://podcasts.apple.com/us/podcast/determined-society/id1555922064"},
        {"Real Kyper & Bourne",
         "https://podcasts.apple.com/us/podcast/real-kyper-bourne/id1588452517"},
        {"What Did You Do Yesterday?",
         "https://podcasts.apple.com/us/podcast/what-did-you-do/id1765600990"},
        {"On Purpose with Jay Shetty",
         "https://podcasts.apple.com/us/podcast/on-purpose/id1450994021"},
        {"Canadian True Crime",
         "https://podcasts.apple.com/us/podcast/canadian-true-crime/id1197095887"},
        {"The Learning Leader Show",
         "https://podcasts.apple.com/us/podcast/learning-leader/id985396258"},
        {"ill-advised by Bill Nighy",
         "https://podcasts.apple.com/us/podcast/ill-advised/id1842190360"},
        {"Nothing much happens",
         "https://podcasts.apple.com/us/podcast/nothing-much-happens/id1378040733"},
        {"The High Performance Podcast",
         "https://podcasts.apple.com/us/podcast/high-performance/id1500444735"},
        {"Hidden Brain", "https://podcasts.apple.com/us/podcast/hidden-brain/id1028908750"},
        {"Serial", "https://podcasts.apple.com/us/podcast/serial/id917918570"},
        {"The Daily T", "https://podcasts.apple.com/us/podcast/the-daily-t/id1489612924"},
        {"Get Birding", "https://podcasts.apple.com/us/podcast/get-birding/id1551111133"},
        {"What's My Age Again?",
         "https://podcasts.apple.com/us/podcast/whats-my-age-again/id1806655079"},
        {"Football Daily", "https://podcasts.apple.com/us/podcast/football-daily/id261291929"},
        {"Mike Ward Sous Écoute", "https://podcasts.apple.com/us/podcast/mike-ward/id1053196322"},
        {"Your World Tonight",
         "https://podcasts.apple.com/us/podcast/your-world-tonight/id250083757"},
        {"The Jamie Kern Lima Show",
         "https://podcasts.apple.com/us/podcast/jamie-kern-lima/id1728723635"},
        {"Crime Next Door", "https://podcasts.apple.com/us/podcast/crime-next-door/id1744545000"},
        {"Power & Politics", "https://podcasts.apple.com/us/podcast/power-politics/id337361397"},
        {"Creating Confidence",
         "https://podcasts.apple.com/us/podcast/creating-confidence/id1462192400"},
        {"Football Ramble", "https://podcasts.apple.com/us/podcast/football-ramble/id254078311"},
        {"The Money Mondays",
         "https://podcasts.apple.com/us/podcast/the-money-mondays/id1664983297"},
        {"Strangers on a Bench",
         "https://podcasts.apple.com/us/podcast/strangers-bench/id1770745380"},
        {"Smosh Reads Reddit Stories",
         "https://podcasts.apple.com/us/podcast/smosh-reads/id1697425543"},
        {"Sword and Scale", "https://podcasts.apple.com/us/podcast/sword-and-scale/id790487079"},
        {"Amanda Knox Hosts | DOUBT", "https://podcasts.apple.com/us/podcast/doubt/id1877870463"},
        {"The Prof G Pod", "https://podcasts.apple.com/us/podcast/prof-g-pod/id1498802610"},
        {"The NPR Politics Podcast",
         "https://podcasts.apple.com/us/podcast/npr-politics/id1057255460"},
        {"The Ancients", "https://podcasts.apple.com/us/podcast/the-ancients/id1520403988"},
        {"The Basement Yard",
         "https://podcasts.apple.com/us/podcast/the-basement-yard/id1034354283"},
        {"You're Dead to Me",
         "https://podcasts.apple.com/us/podcast/youre-dead-to-me/id1479973402"},
        {"Ologies with Alie Ward", "https://podcasts.apple.com/us/podcast/ologies/id1278815517"},
        {"Breaking Points", "https://podcasts.apple.com/us/podcast/breaking-points/id1570045623"},
        {"The David Frum Show",
         "https://podcasts.apple.com/us/podcast/david-frum-show/id1305908387"},
        {"Modern Wisdom", "https://podcasts.apple.com/us/podcast/modern-wisdom/id1347973549"},
        {"Two Blocks from the White House",
         "https://podcasts.apple.com/us/podcast/two-blocks/id1866939902"},
        {"Today, Explained", "https://podcasts.apple.com/us/podcast/today-explained/id1346207297"},
        {"Passion Struck", "https://podcasts.apple.com/us/podcast/passion-struck/id1553279283"},
        {"Joe and James Fact Up",
         "https://podcasts.apple.com/us/podcast/joe-james-fact-up/id1838423659"},
        {"Prof G Markets", "https://podcasts.apple.com/us/podcast/prof-g-markets/id1744631325"},
        {"The Decibel", "https://podcasts.apple.com/us/podcast/the-decibel/id1565410296"},
        {"The Martin Lewis Podcast",
         "https://podcasts.apple.com/us/podcast/martin-lewis/id520802069"},
        {"3 Takeaways", "https://podcasts.apple.com/us/podcast/3-takeaways/id1526080983"},
        {"The Good, The Bad & The Football",
         "https://podcasts.apple.com/us/podcast/good-bad-football/id1839425706"},
        {"followHIM", "https://podcasts.apple.com/us/podcast/followhim/id1545433056"},
        {"Behind the Bastards",
         "https://podcasts.apple.com/us/podcast/behind-the-bastards/id1373812661"},

        // ═══════════════════════════════════════════════════════════════════
        // [YouTube channels]
        // ═══════════════════════════════════════════════════════════════════
        {"56BelowTV (YouTube)", "https://www.youtube.com/@56BelowTV"},
    };

    // Load the user's deleted built-in podcast records to avoid re-adding them on restart
    //   (the original code only looked at current subscriptions; after deletion, restart would always revive them, contrary to the comment's promise)
    // removed was queried at the function entry (outside the lock)

    for (auto &p : defaults) {
        // Skip already-existing podcasts to avoid duplicates
        if (existing_urls.count(p.url))
            continue;
        // Skip built-in podcasts the user has deleted
        if (removed.count(p.url))
            continue;

        auto node = std::make_shared<TreeNode>();
        node->title = p.name;
        node->url = p.url;
        node->type = NodeType::PODCAST_FEED;
        // Initially show the subscription, but the episode list is not loaded
        // Parsing is triggered only on enter (expand)
        node->children_loaded = false;
        node->expanded = false;
        node->parent.reset(); // set parent pointer
        // Mark YouTube channels
        if (std::string(p.url).find("youtube.com") != std::string::npos) {
            node->is_youtube = true;
        }
        library_.podcast_root().push_back(node);
    }
    library_.podcast_loaded() = true;
}

} // namespace panicast
