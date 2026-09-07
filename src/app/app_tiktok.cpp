// Y24.11: T-mode (TikTok/Douyin) controller — anonymous creator subscription + video listing.
//   No login: TikTok user listings work anonymously via yt-dlp (--geo-bypass-country for region).
//   Douyin usually needs a cookies.txt + a CN network exit. Search is a best-effort search-engine
//   fallback (site:tiktok.com/@) because yt-dlp has no TikTok search extractor.
#include "panicast/app/app.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <regex>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "panicast/config/ini_config.h"
#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/core/paths.h"
#include "panicast/net/douyin_api.h"
#include "panicast/net/tiktok_region.h"
#include "panicast/net/ytdlp_runner.h"
#include "panicast/storage/database.h"
#include "panicast/ui/qr.h"

namespace panicast
{
using json = nlohmann::json;

// ── tiktok_accounts DB ops (mirror bilibili_accounts style; no credentials to encrypt) ──
// Y24.27: TiktokAccount struct moved to database.h

// D11-3c: load_tiktok_accounts (a one-liner) was inlined into LibraryService::load_tiktok_root.

static int save_tiktok_account(const TiktokAccount &a) {
    return DatabaseManager::instance().upsert_tiktok_account(a);
}

static bool delete_tiktok_account(int id) {
    return DatabaseManager::instance().delete_tiktok_account(id);
}

// Normalize user input into (platform, handle, url). Accepts:
//   "@user" / "user"                  → tiktok, "@user", https://www.tiktok.com/@user
//   "tiktok.com/@user[.../video/<id>]" → tiktok, "@user", https://www.tiktok.com/@user  (video URL → its @user)
//   "douyin.com/video/<id>"           → douyin, <video-id>, https://www.douyin.com/video/<id>
//   "douyin.com/user/<sec_uid>"       → douyin, <sec_uid>, https://www.douyin.com/user/<sec_uid>  (listable via native X-Bogus)
// Returns false if no handle could be extracted.
static bool normalize_tiktok_input(const std::string &raw, std::string &platform,
                                   std::string &handle, std::string &url) {
    std::string s = raw;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '/' || s.back() == '\n'))
        s.pop_back();
    if (s.empty())
        return false;

    if (s.find("douyin.com") != std::string::npos) {
        platform = "douyin";
        url = (s.rfind("http", 0) == 0) ? s : ("https://www." + s);
        std::smatch m;
        if (std::regex_search(url, m, std::regex("/video/([^/?#]+)")))
            handle = m[1].str(); // video id
        else if (std::regex_search(url, m, std::regex("/user/([^/?#]+)")))
            handle = m[1].str(); // sec_uid
        else
            handle = url;
        return true;
    }
    // TikTok: extract @username (present in both profile and video URLs).
    std::smatch m;
    if (std::regex_search(s, m, std::regex("@([A-Za-z0-9._\\-]{2,24})"))) {
        platform = "tiktok";
        handle = "@" + m[1].str();
        url = "https://www.tiktok.com/" + handle; // profile URL (video URL → subscribe the creator)
        return true;
    }
    // Bare username (no @, no domain) → TikTok handle.
    if (s.find('/') == std::string::npos && s.find('.') == std::string::npos) {
        platform = "tiktok";
        handle = "@" + s;
        url = "https://www.tiktok.com/" + handle;
        return true;
    }
    return false;
}

// Does a normalized douyin URL point to a (playable) video?
static bool douyin_is_video_url(const std::string &url) {
    return url.find("/video/") != std::string::npos;
}

// Build the yt-dlp arg vector for a creator listing (shared by probe + parse).
static std::vector<std::string> tiktok_listing_args(const std::string &url,
                                                    const std::string &region,
                                                    const std::string &cookies_file) {
    std::vector<std::string> args;
    args.push_back("--flat-playlist");
    args.push_back("--dump-json");
    args.push_back("--no-warnings");
    if (!region.empty()) {
        args.push_back("--geo-bypass-country");
        args.push_back(region);
    }
    if (!cookies_file.empty() && std::filesystem::exists(cookies_file)) {
        args.push_back("--cookies");
        args.push_back(cookies_file);
    }
    args.push_back(url);
    return args;
}

// Probe a TikTok creator's display name from the first listed video's "channel"/"uploader" field.
//   (Douyin user URLs are unsupported by yt-dlp — no DouyinUserIE — so this is TikTok-only.)
static std::string probe_tiktok_uname(const std::string &url, const std::string &region,
                                      const std::string &cookies_file) {
    auto args = tiktok_listing_args(url, region, cookies_file);
    args.push_back("--playlist-items");
    args.push_back("1:1");
    std::string uname;
    YtdlpRunner::run(
        args,
        [&](const std::string &line) {
            if (!uname.empty())
                return;
            try {
                auto j = json::parse(line);
                std::string ch = j.value("channel", "");
                if (!ch.empty()) {
                    uname = ch;
                    return;
                }
                std::string up = j.value("uploader", "");
                if (!up.empty())
                    uname = up;
            } catch (...) {
            }
        },
        60, url);
    return uname;
}

// Y24.11: list a creator's videos via yt-dlp --flat-playlist. Retries on transient empty/failed
//   output (TikTok's JS challenge cookie sometimes blanks the first attempt). region is the
//   geo-bypass country code (TikTok: current region; Douyin: "CN"). cookies_file is optional.
//   TikTok-only in practice — yt-dlp has no DouyinUserIE, so douyin.com/user/ URLs fail here.
TreeNodePtr App::parse_tiktok_user_videos(const std::string &url, const std::string &region,
                                          const std::string &cookies_file,
                                          const std::string &title) {
    auto args = tiktok_listing_args(url, region, cookies_file);
    std::vector<std::string> lines;
    constexpr int MAX_ATTEMPTS = 3;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt) {
        lines.clear();
        auto r = YtdlpRunner::run(
            args, [&](const std::string &line) { lines.push_back(line); }, 30, url);
        if (!lines.empty())
            break;
        // Empty output — retry once more after backoff (challenge cookie / transient block).
        if (attempt < MAX_ATTEMPTS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(800 * attempt));
        } else {
            LOG(fmt::format("[TikTok] listing empty for {} (exit={}, region={}) stderr: {}",
                            url.substr(0, 80), r.exit_code, region,
                            r.stderr_output.substr(0, 200)));
        }
    }

    auto result = std::make_shared<TreeNode>();
    result->url = url;
    result->type = NodeType::PODCAST_FEED;
    result->title = title;
    result->is_youtube = false;
    for (const auto &line : lines) {
        try {
            auto j = json::parse(line);
            if (!j.is_object())
                continue;
            auto ep = std::make_shared<TreeNode>();
            ep->type = NodeType::PODCAST_EPISODE;
            std::string id = j.value("id", "");
            ep->url = j.value("url", id);
            ep->title = j.value("title", id.empty() ? "Untitled" : id);
            ep->duration = j.value("duration", 0);
            ep->children_loaded = true;
            ep->parent = result;
            result->children.push_back(ep);
        } catch (...) {
        }
    }
    result->children_loaded = true;
    return result;
}

// D11-3c: load_tiktok_root relocated to LibraryService (the tiktok_root_ owner).

// Y24.16: shared subscribe core used by 'a' (add) and '/' (direct input) — no duplication.
//   TikTok @user/profile/video URL → creator (listable via yt-dlp tiktok:user).
//   Douyin video URL → playable video leaf (option A).
//   Douyin user URL → creator (listable via native DouyinApi X-Bogus, see douyin_api.cpp).
void App::tiktok_subscribe(const std::string &input) {
    std::string platform, handle, url;
    if (!normalize_tiktok_input(input, platform, handle, url)) {
        EVENT_LOG("T: could not parse a @user or URL from input");
        return;
    }
    if (platform == "douyin") {
        if (douyin_is_video_url(url)) {
            EVENT_LOG(fmt::format("T: adding Douyin video {}", handle));
            pool_.submit([this, handle, url]() {
                TiktokAccount a{0, "douyin_video", handle, url, ""};
                int id = save_tiktok_account(a);
                EVENT_LOG(
                    fmt::format("T: saved Douyin video {} ({})", handle, id ? "ok" : "failed"));
                library_.load_tiktok_root();
            });
        } else {
            // Douyin creator — subscribed as platform "douyin" (sec_uid = handle). Expansion goes
            //   through DouyinApi::fetchUserVideos (Endpoint C), not yt-dlp (which has no DouyinUserIE).
            EVENT_LOG(fmt::format("T: adding Douyin creator {}", handle));
            pool_.submit([this, handle, url]() {
                TiktokAccount a{0, "douyin", handle, url, ""};
                int id = save_tiktok_account(a);
                EVENT_LOG(
                    fmt::format("T: saved Douyin creator {} ({})", handle, id ? "ok" : "failed"));
                library_.load_tiktok_root();
            });
        }
        return;
    }
    // TikTok creator (normalize built the profile URL from @user or a video URL).
    EVENT_LOG(fmt::format("T: adding TikTok {} ({})", handle, url));
    std::string region = tiktok_region_;
    std::string cookies = IniConfig::instance().get_tiktok_cookies_file();
    pool_.submit([this, handle, url, region, cookies]() {
        std::string uname = probe_tiktok_uname(url, region, cookies);
        TiktokAccount a{0, "tiktok", handle, url, uname};
        int id = save_tiktok_account(a);
        EVENT_LOG(fmt::format("T: saved TikTok {}{} ({})", handle,
                              uname.empty() ? "" : (" (" + uname + ")"), id ? "ok" : "failed"));
        library_.load_tiktok_root();
    });
}

// 'a' in T mode: Douyin QR scan login (terminal QR, pure API — mirrors B-mode start_bilibili_login).
//   Douyin login is cookie-only (sessionid): the curl cookie jar writes it to douyin_cookie.txt,
//   so there's no account DB row. After login, Endpoint C (creator listing) carries sessionid.
void App::start_douyin_login() {
    EVENT_LOG("T: requesting Douyin QR code...");
    DouyinApi api;
    auto qr = api.request_qrcode();
    if (!qr.ok) {
        LOG(fmt::format("[Douyin] QR request failed: {}", qr.err));
        EVENT_LOG(fmt::format("T: Douyin login failed — {}", qr.err.empty() ? "no token" : qr.err));
        return;
    }

    auto qr_rows = render_qr_rows(qr.qr_content);
    int qr_h = qr_available() ? (int)qr_rows.size() : 0;
    int qr_w = qr_h ? (int)qr_rows[0].size() : 0;
    int text_h = 5; // title + blank + hint + cancel + blank
    int pop_h = std::max(qr_h, 1) + text_h + 4;
    int pop_w = std::max({60, qr_w + 4});
    if (pop_w > COLS)
        pop_w = COLS;
    if (pop_h > LINES)
        pop_h = LINES;
    int py = (LINES - pop_h) / 2;
    int px = (COLS - pop_w) / 2;
    if (py < 0)
        py = 0;
    if (px < 0)
        px = 0;

    WINDOW *win = newwin(pop_h, pop_w, py, px);
    if (!win) {
        EVENT_LOG("T: Douyin QR window failed");
        return;
    }
    keypad(win, TRUE);
    nodelay(win, TRUE);

    bool ok = false;
    DouyinApi::LoginResult login;
    int interval = 2;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);

    while (std::chrono::steady_clock::now() < deadline) {
        werase(win);
        box(win, 0, 0);
        int y = 1;
        mvwprintw(win, y++, 2, "Douyin Login");
        y++;
        if (qr_h > 0) {
            for (const auto &row : qr_rows) {
                if (y >= pop_h - 1)
                    break;
                mvwprintw(win, y++, 2, "%s", row.c_str());
            }
        } else {
            mvwprintw(win, y++, 2, "(no QR content — check get_qrcode response)");
        }
        y++;
        mvwprintw(win, y++, 2, "Use Douyin app to scan");
        mvwprintw(win, y++, 2, "Press 'q' to cancel");
        wrefresh(win);

        int ch = wgetch(win);
        if (ch == 'q' || ch == 'Q')
            break;

        login = api.poll_qrcode(qr.token);
        if (login.ok) {
            ok = true;
            break;
        }
        if (!login.err.empty()) {
            LOG(fmt::format("[Douyin] poll error: {}", login.err));
            break;
        }
        // waiting / scanned (code 1/2) or unknown status — keep polling until deadline
        for (int s = 0; s < interval * 10; ++s) {
            int c = wgetch(win);
            if (c == 'q' || c == 'Q')
                goto done;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
done:
    delwin(win);
    flushinp();
    keypad(stdscr, TRUE);
    timeout(30);
    curs_set(0);
    noecho();
    touchwin(stdscr);
    refresh();

    if (!ok) {
        EVENT_LOG("T: Douyin login cancelled or failed");
        return;
    }
    EVENT_LOG("T: Douyin logged in (sessionid saved to douyin_cookie.txt)");
    library_.load_tiktok_root();
}

// 'd' in T mode: delete the creator under the cursor.
void App::delete_tiktok_user_node(TreeNodePtr node) {
    if (!node || !node->is_account || node->account_id <= 0) {
        EVENT_LOG("T: select a creator node to delete");
        return;
    }
    if (!frontend_->confirm_box("Delete this creator?"))
        return;
    int id = node->account_id;
    if (delete_tiktok_account(id)) {
        EVENT_LOG(fmt::format("T: deleted creator #{}", id));
    } else {
        EVENT_LOG(fmt::format("T: failed to delete creator #{}", id));
    }
    library_.load_tiktok_root();
}

// Y24.16: '/' in T mode — direct input (no full-text search; that's infeasible anonymously:
//   search engines don't index tiktok.com/douyin.com content pages, and yt-dlp has no search
//   extractor). Accepts @user / #tag / URL; a bare keyword gets a clear "no keyword search" prompt.

// tag_browse: list a tag's videos via yt-dlp --flat-playlist (reuses parse_tiktok_user_videos,
//   generic over any yt-dlp list URL). tiktok:tag is currently _WORKING=False upstream → fails
//   gracefully; auto-works if yt-dlp re-enables it. Result shown under a transient folder (not saved).
void App::tag_browse(const std::string &tag) {
    bool cn = (tiktok_region_ == "CN");
    std::string domain = cn ? "douyin.com" : "tiktok.com";
    std::string tag_url = "https://www." + domain + "/tag/" + tag;
    std::string region = cn ? std::string("CN") : tiktok_region_;
    std::string cookies = cn ? IniConfig::instance().get_tiktok_douyin_cookies_file()
                             : IniConfig::instance().get_tiktok_cookies_file();
    std::string label = "#" + tag;
    EVENT_LOG(fmt::format("T: browsing {} (yt-dlp tag extractor disabled upstream — may be empty)",
                          tag_url));
    pool_.submit([this, tag_url, region, cookies, label]() {
        auto result = parse_tiktok_user_videos(tag_url, region, cookies, label);
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        for (auto it = library_.tiktok_root().begin(); it != library_.tiktok_root().end();) {
            if ((*it)->is_yt_search)
                it = library_.tiktok_root().erase(it);
            else
                ++it;
        }
        auto folder = std::make_shared<TreeNode>();
        folder->title = fmt::format("\U0001F50D {} ({} videos)", label, result->children.size());
        folder->type = NodeType::FOLDER;
        folder->is_yt_search = true;
        folder->expanded = true;
        folder->children_loaded = true;
        folder->parent.reset();
        for (auto &c : result->children) {
            c->parent = folder;
            folder->children.push_back(c);
        }
        library_.tiktok_root().push_back(folder);
        if (result->children.empty())
            EVENT_LOG(fmt::format("T: {} — no videos (yt-dlp tag extractor disabled upstream; try "
                                  "@user or a video URL)",
                                  label));
        else
            EVENT_LOG(fmt::format("T: {} — {} videos", label, result->children.size()));
    });
}

void App::tiktok_direct_input() {
    std::string input = frontend_->input_box("Open (@user / #tag / URL)");
    if (UI::is_input_cancelled(input)) {
        EVENT_LOG("T: open cancelled");
        return;
    }
    while (!input.empty() && (input.front() == ' ' || input.front() == '\t'))
        input.erase(input.begin());
    if (input.empty())
        return;
    if (input[0] == '#') {
        std::string tag = input.substr(1);
        while (!tag.empty() && tag.front() == ' ')
            tag.erase(tag.begin());
        if (tag.empty()) {
            EVENT_LOG("T: empty #tag");
            return;
        }
        tag_browse(tag);
        return;
    }
    if (input[0] == '@' || input.find("tiktok.com") != std::string::npos ||
        input.find("douyin.com") != std::string::npos ||
        input.find("vm.tiktok.com") != std::string::npos) {
        tiktok_subscribe(input); // shared with 'a'
        return;
    }
    // META-6: bare keyword → Douyin signed keyword search (DouyinApi). TikTok region
    //   keeps the old prompt (overseas TikTok has no anonymous keyword API).
    if (TikTokRegion::current() == "CN") { // Douyin — has the signed keyword API
        run_tiktok_keyword_search(input);
        return;
    }
    EVENT_LOG("T: anonymous keyword search unavailable TikTok/Douyin, enter @user / #tag / URL (or "
              "use 'a' add)");
}

// META-6: keyword search via DouyinApi::searchKeyword. Results become a search-record
//   node at the T root (same shape as O/B/Y search history) — the remote browse and
//   the TUI both see it via the ordinary tree mirror.
void App::run_tiktok_keyword_search(const std::string &query) {
    EVENT_LOG(fmt::format("T: searching Douyin for '{}'", query));
    pool_.submit([this, query]() {
        auto api = std::make_unique<DouyinApi>();
        // session cookies (may be empty — ttwid bootstrap often still works)
        std::string ck = Paths::get_data_dir() + "/douyin_cookie.txt";
        api->setSession(DouyinApi::build_cookie_header_from_txt(ck, "douyin.com"));
        api->searchKeyword(query, 0, 20, [this, query](const DouyinApi::UserVideoResult &r) {
            if (!r.ok) {
                EVENT_LOG(fmt::format("T: Douyin search failed — {}", r.err));
                return;
            }
            // Build the record node: title = query, children = video leaves.
            auto rec = std::make_shared<TreeNode>();
            rec->title = "🔍 " + query;
            rec->subtext = fmt::format("{} results", r.videos.size());
            rec->type = NodeType::FOLDER;
            rec->children_loaded = true;
            int vt = 0;
            for (const auto &v : r.videos) {
                if (v.playUrl.empty())
                    continue;
                auto ep = std::make_shared<TreeNode>();
                ep->type = NodeType::PODCAST_EPISODE;
                ep->title = v.desc.empty() ? v.awemeId : v.desc;
                ep->url = v.playUrl;
                ep->duration = v.duration / 1000; // ms → s
                ep->art_url = v.coverUrl;
                ep->artist = query; // search-term context as artist
                ep->album = "Douyin";
                ep->track_num = ++vt;
                ep->children_loaded = true;
                ep->parent = rec;
                rec->children.push_back(ep);
            }
            {
                std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                auto &root = library_.tiktok_root();
                // replace an existing record with the same title, then front-insert
                for (size_t i = 0; i < root.size(); ++i) {
                    if (root[i]->title == rec->title) {
                        root.erase(root.begin() + (long)i);
                        break;
                    }
                }
                rec->parent.reset();
                root.insert(root.begin(), rec);
            }
            EVENT_LOG(fmt::format("T: Douyin search '{}' — {} videos", query, vt));
        });
    });
}

} // namespace panicast
