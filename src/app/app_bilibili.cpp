// Y15: B-mode (Bilibili) controller — login (QR + cookie import), browse, play, search.
//   Reuses YtdlpRunner (yt-dlp --cookies bilibili_cookie.txt), mpv playback, pool async, tree.
#include "panicast/app/app.h"

#include <chrono>
#include <fstream>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "panicast/config/ini_config.h"
#include "panicast/core/crypto.h"
#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/core/paths.h"
#include "panicast/net/bilibili_api.h"
#include "panicast/net/network.h"
#include "panicast/net/ytdlp_runner.h"
#include "panicast/net/url_classifier.h"
#include "panicast/parsers/youtube_channel_parser.h"
#include "panicast/parsers/bilibili_parser.h"
#include "panicast/storage/database.h"

namespace panicast
{
using json = nlohmann::json;

// ── Bilibili account DB ops (simple, using DatabaseManager's db_ handle) ──
//   The bilibili_accounts table was created by SCHEMA_VERSION 42.
// Y24.27: BilibiliAccount struct moved to database.h

// D11-3c: load_bilibili_accounts relocated to LibraryService (public — load_bilibili_root + the
//   ops below call library_.load_bilibili_accounts()). The remaining static DB ops stay here.

int save_bilibili_account(const BilibiliAccount &a) {
    // Y24.27: use DatabaseManager (was direct sqlite3_* — bypassed encapsulation).
    const Key32 mk = machine_key();
    BilibiliAccount enc = a;
    enc.sessdata = a.sessdata.empty() ? "" : token_seal(mk, a.sessdata);
    enc.bili_jct = a.bili_jct.empty() ? "" : token_seal(mk, a.bili_jct);
    enc.dedeuserid = a.dedeuserid.empty() ? "" : token_seal(mk, a.dedeuserid);
    return DatabaseManager::instance().upsert_bilibili_account(enc);
}

// DB-11: delete a Bilibili account row (and its cookies file). Wired to 'd' in B mode.
static bool delete_bilibili_account(int id) {
    return DatabaseManager::instance().delete_bilibili_account(id);
}

// Write SESSDATA cookies to bilibili_cookie.txt (for yt-dlp --cookies).
void write_bilibili_cookies(const BilibiliAccount &a) {
    std::string path = IniConfig::instance().get_bilibili_cookies_file();
    if (path.empty())
        return;
    std::string content = BilibiliAPI::build_cookies_txt(a.sessdata, a.bili_jct, a.dedeuserid);
    std::ofstream f(path);
    if (f) {
        f << content;
        LOG(fmt::format("[Bilibili] cookies written to {}", path));
    }
}

// D11-3c: load_bilibili_root relocated to LibraryService (the bilibili_root_ owner).

// DB-11: delete the Bilibili account under the cursor (wired to 'd' in B mode).
void App::delete_bilibili_account_node(TreeNodePtr node) {
    if (!node || !node->is_account || node->account_id <= 0) {
        EVENT_LOG("B: select an account node to delete");
        return;
    }
    if (!frontend_->confirm_box("Delete this Bilibili account?"))
        return;
    int id = node->account_id;
    if (delete_bilibili_account(id)) {
        // Remove the cookies file too (yt-dlp no longer needs it for this account).
        std::string ck = IniConfig::instance().get_bilibili_cookies_file();
        if (!ck.empty()) {
            std::error_code e;
            std::filesystem::remove(ck, e);
        }
        EVENT_LOG(fmt::format("B: deleted Bilibili account #{}", id));
    } else {
        EVENT_LOG(fmt::format("B: failed to delete account #{}", id));
    }
    library_.load_bilibili_root();
}

// ── Login: QR scan (like Y mode) or cookie import ──
void App::start_bilibili_login() {
    EVENT_LOG("B: requesting Bilibili QR code...");
    auto qr = BilibiliAPI::request_qrcode();
    if (!qr.ok) {
        LOG(fmt::format("[B] QR request failed: {}", qr.error));
        EVENT_LOG(fmt::format("B: login failed — {}", qr.error));
        return;
    }

    // Show QR popup — same sizing approach as Y mode (qr_login_poll).
    //   Popup width = max(60, qr_w + 4) — based on QR size, NOT URL length.
    //   URL is NOT displayed (scanned via QR, no need to show text).
    auto qr_rows = render_qr_rows(qr.url);
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
        EVENT_LOG("B: QR window failed");
        return;
    }
    keypad(win, TRUE);
    nodelay(win, TRUE);

    bool ok = false;
    BilibiliAPI::LoginResult login;
    int interval = 2;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);

    while (std::chrono::steady_clock::now() < deadline) {
        // Draw QR popup — QR + minimal text (no URL display)
        werase(win);
        box(win, 0, 0);
        int y = 1;
        mvwprintw(win, y++, 2, "Bilibili Login");
        y++;
        if (qr_h > 0) {
            for (const auto &row : qr_rows) {
                if (y >= pop_h - 1)
                    break;
                mvwprintw(win, y++, 2, "%s", row.c_str());
            }
        } else {
            mvwprintw(win, y++, 2, "(libqrencode not installed — no QR image)");
        }
        y++;
        mvwprintw(win, y++, 2, "Use Bilibili app to scan");
        mvwprintw(win, y++, 2, "Press 'q' to cancel");
        wrefresh(win);

        int ch = wgetch(win);
        if (ch == 'q' || ch == 'Q')
            break;

        login = BilibiliAPI::poll_qrcode(qr.qrcode_key);
        if (login.ok) {
            ok = true;
            break;
        }
        if (login.code == 86101 || login.code == 86090) {
            // waiting / scanned — keep polling
        } else if (!login.error.empty()) {
            LOG(fmt::format("[B] poll error: {}", login.error));
            break;
        }
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
        EVENT_LOG("B: login cancelled or failed");
        return;
    }

    // Fetch user info
    auto nav = BilibiliAPI::fetch_nav(login.sessdata);
    BilibiliAccount acc;
    acc.sessdata = login.sessdata;
    acc.bili_jct = login.bili_jct;
    acc.dedeuserid = login.dedeuserid;
    acc.uid = nav.ok ? nav.uid : login.dedeuserid;
    acc.uname = nav.ok ? nav.uname : ("Bili #" + acc.uid);

    int aid = save_bilibili_account(acc);
    write_bilibili_cookies(acc);
    EVENT_LOG(fmt::format("B: account #{} logged in ({})", aid, acc.uname));
    library_.load_bilibili_root();
}

// ── Cookie import: user provides a cookies.txt path ──
// Y24.27: import_bilibili_cookies removed (dead code).

// ── Expand a Bilibili account node → following list (UP masters) ──
void App::expand_bilibili_account(TreeNodePtr node) {
    if (!node || !node->is_account)
        return;
    // Y22: mirror Y-mode — create two lazy children: Subscriptions (followings) + History (history).
    //   Each fetches on first expand (expand_bili_followings / expand_bili_history).
    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
    node->children.clear();
    auto subs = std::make_shared<TreeNode>();
    subs->title = "Subscriptions";
    subs->type = NodeType::FOLDER;
    subs->is_bili_followings = true;
    subs->account_id = node->account_id;
    subs->children_loaded = false;
    subs->parent = node;
    node->children.push_back(subs);
    auto hist = std::make_shared<TreeNode>();
    hist->title = "History";
    hist->type = NodeType::FOLDER;
    hist->is_bili_history = true;
    hist->account_id = node->account_id;
    hist->children_loaded = false;
    hist->parent = node;
    node->children.push_back(hist);
    // Y23.1/Y23.2: Search History container — shared shape with Y mode.
    node->children.push_back(library_.make_search_history_child(node, "bilibili", node->account_id));
    node->children_loaded = true;
    node->expanded = true;
}

// Y22: lazy-expand Subscriptions → fetch the account's followings (UP masters) via WBI.
//   CACHE-1: non-forced first expand serves the persisted list (bilibili_follow_cache); 'r'
//   (force=true) always re-fetches and rewrites the cache.
void App::expand_bili_followings(TreeNodePtr node, bool force) {
    if (!node || !node->is_bili_followings)
        return;
    int aid = node->account_id;
    auto accounts = library_.load_bilibili_accounts();
    BilibiliAccount acc;
    for (const auto &a : accounts)
        if (a.id == aid) {
            acc = a;
            break;
        }
    if (acc.sessdata.empty()) {
        EVENT_LOG("B: no SESSDATA for this account");
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(library_.tree_mutex());
        node->loading = true;
    }
    EVENT_LOG(fmt::format("B: fetching followings for {}...", acc.uname));
    std::string sessdata = acc.sessdata;
    std::string uid = acc.uid;
    pool_.submit([this, sessdata, uid, aid, node, force]() {
        // CACHE-1: cache-first on non-forced expand; empty/missing cache falls through to network.
        if (!force) {
            std::string cached = DatabaseManager::instance().load_bili_followings(aid, uid);
            std::vector<TreeNodePtr> kids;
            if (!cached.empty()) {
                try {
                    json j = json::parse(cached);
                    if (j.is_array()) {
                        for (const auto &it : j) {
                            auto c = std::make_shared<TreeNode>();
                            c->title = it.value("title", "");
                            c->url = it.value("url", "");
                            c->type = NodeType::FOLDER;
                            c->is_yt_channel = true; // expandable UP-master folder
                            c->is_bili_up = true;
                            c->is_youtube = false;
                            c->children_loaded = false;
                            c->account_id = aid;
                            std::string sign = it.value("subtext", "");
                            if (!sign.empty())
                                c->subtext = sign;
                            c->parent = node;
                            kids.push_back(c);
                        }
                    }
                } catch (const std::exception &e) {
                    LOG(fmt::format("[B] followings cache parse error: {}", e.what()));
                }
            }
            if (!kids.empty()) {
                std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                node->children.clear();
                for (auto &c : kids)
                    node->children.push_back(c);
                node->children_loaded = true;
                node->expanded = true;
                node->loading = false;
                EVENT_LOG(fmt::format("B: {} followings loaded from cache", kids.size()));
                return;
            }
        }
        auto followings = BilibiliParser::parse_followings(sessdata, uid, aid);
        {
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            node->children.clear();
            for (auto &c : followings) {
                c->parent = node;
                node->children.push_back(c);
            }
            node->children_loaded = true;
            node->expanded = true;
            node->loading = false;
        }
        // CACHE-1: serialize + persist outside the tree lock.
        json arr = json::array();
        for (const auto &c : followings)
            arr.push_back({{"title", c->title}, {"url", c->url}, {"subtext", c->subtext}});
        DatabaseManager::instance().save_bili_followings(aid, uid, arr.dump());
        EVENT_LOG(fmt::format("B: {} followings loaded", followings.size()));
    });
}

// Y22: lazy-expand History → fetch the account's watch history via /x/v2/history.
//   CACHE-1: non-forced first expand serves the persisted list (bilibili_history_cache); 'r'
//   (force=true) always re-fetches and rewrites the cache.
void App::expand_bili_history(TreeNodePtr node, bool force) {
    if (!node || !node->is_bili_history)
        return;
    int aid = node->account_id;
    auto accounts = library_.load_bilibili_accounts();
    BilibiliAccount acc;
    for (const auto &a : accounts)
        if (a.id == aid) {
            acc = a;
            break;
        }
    if (acc.sessdata.empty()) {
        EVENT_LOG("B: no SESSDATA for this account");
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(library_.tree_mutex());
        node->loading = true;
    }
    EVENT_LOG(fmt::format("B: fetching history for {}...", acc.uname));
    std::string sessdata = acc.sessdata;
    std::string uid = acc.uid;
    pool_.submit([this, sessdata, uid, aid, node, force]() {
        // CACHE-1: cache-first on non-forced expand; empty/missing cache falls through to network.
        if (!force) {
            std::string cached = DatabaseManager::instance().load_bili_history(aid, uid);
            std::vector<TreeNodePtr> kids;
            if (!cached.empty()) {
                try {
                    json j = json::parse(cached);
                    if (j.is_array()) {
                        for (const auto &it : j) {
                            auto c = std::make_shared<TreeNode>();
                            c->title = it.value("title", "");
                            c->url = it.value("url", "");
                            c->type = NodeType::PODCAST_EPISODE;
                            c->duration = it.value("duration", 0);
                            c->children_loaded = true;
                            c->account_id = aid;
                            c->parent = node;
                            kids.push_back(c);
                        }
                    }
                } catch (const std::exception &e) {
                    LOG(fmt::format("[B] history cache parse error: {}", e.what()));
                }
            }
            if (!kids.empty()) {
                std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                node->children.clear();
                for (auto &c : kids)
                    node->children.push_back(c);
                node->children_loaded = true;
                node->expanded = true;
                node->loading = false;
                EVENT_LOG(fmt::format("B: {} history items loaded from cache", kids.size()));
                return;
            }
        }
        auto hist = BilibiliParser::parse_history(sessdata, aid);
        {
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            node->children.clear();
            for (auto &c : hist) {
                c->parent = node;
                node->children.push_back(c);
            }
            node->children_loaded = true;
            node->expanded = true;
            node->loading = false;
        }
        // CACHE-1: serialize + persist outside the tree lock.
        json arr = json::array();
        for (const auto &c : hist)
            arr.push_back({{"title", c->title}, {"url", c->url}, {"duration", c->duration}});
        DatabaseManager::instance().save_bili_history(aid, uid, arr.dump());
        EVENT_LOG(fmt::format("B: {} history items loaded", hist.size()));
    });
}

// B-fix: 'r' on Subscriptions — force re-fetch the account's followings (UP masters) via WBI.
//   expand_bili_followings always clears + re-fetches; the loading guard prevents a double fetch.
void App::refresh_bili_followings(TreeNodePtr node) {
    if (!node || !node->is_bili_followings || node->loading)
        return;
    expand_bili_followings(node, /*force=*/true);
}

// B-fix: 'r' on History — force re-fetch the account's watch history via /x/v2/history.
void App::refresh_bili_history(TreeNodePtr node) {
    if (!node || !node->is_bili_history || node->loading)
        return;
    expand_bili_history(node, /*force=*/true);
}

// B-fix: 'r' on a Bilibili account node — re-expand it (recreate the Subs / History / Search
//   children as lazy). Each child re-fetches when expanded or via 'r' on it directly.
void App::refresh_bilibili_account(TreeNodePtr node) {
    if (!node || !node->is_account)
        return;
    expand_bilibili_account(node);
    EVENT_LOG(fmt::format("B: account '{}' refreshed", node->title));
}

// Y23: subscribe to a Bilibili UP master from a search result ('a' on a 👤 result). Adds it to
//   library_.podcast_root() as a feed (P-mode subscription, expandable via WBI arc/search) and caches the
//   avatar/logo URL in bilibili_up_cache for future display.
void App::subscribe_bilibili_up(TreeNodePtr node) {
    if (!node || node->url.empty() || node->channel_id.empty()) {
        EVENT_LOG("B: select a 👤 UP search result to subscribe");
        return;
    }
    std::string mid = node->channel_id;
    std::string uname = node->channel_name.empty() ? node->title : node->channel_name;
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        for (const auto &child : library_.podcast_root()) {
            if (child->url == node->url) {
                EVENT_LOG(fmt::format("Already subscribed: {}", uname));
                return;
            }
        }
        auto n = std::make_shared<TreeNode>();
        n->title = uname;
        n->url = node->url; // https://space.bilibili.com/<mid>/video
        n->type = NodeType::PODCAST_FEED;
        n->is_youtube = false;
        n->is_bili_up = true;       // Y23.2: 👤 icon
        n->art_url = node->art_url; // avatar URL
        n->subtext = node->subtext; // sign
        n->children_loaded = false;
        n->parent.reset();
        library_.podcast_root().insert(library_.podcast_root().begin(), n);
        Persistence::save_cache(library_.radio_root(), library_.podcast_root());
        Persistence::save_data(library_.podcast_root(), library_.fav_root());
    }
    // Cache the UP logo/metadata (mid → upic) for future remote/sixel display.
    DatabaseManager::instance().save_bili_up(mid, uname, node->art_url, 0, node->subtext);
    EVENT_LOG(fmt::format("B: subscribed UP '{}' (mid={}) — appears in P mode", uname, mid));
}

// ── Search Bilibili via the native WBI search/type API (Y21, issue 2) ──
//   Replaces the yt-dlp `bilisearch:` extractor (fragile/slow, same risk control). Works without
//   login (public search); uses the first logged-in account's SESSDATA if present.
void App::perform_bilibili_search(const std::string &preset) {
    std::string q;
    if (!preset.empty()) {
        q = preset;
    } else {
        q = frontend_->input_box("Search Bilibili");
        if (UI::is_input_cancelled(q)) {
            EVENT_LOG("B: search cancelled");
            return;
        }
    }
    if (q.empty())
        return;

    EVENT_LOG(fmt::format("B: searching Bilibili '{}'...", q));
    // Resolve the search to an account node. E: the root node is gone, so anonymous (no-account)
    //   search has no parent to attach results to → require a logged-in account (like Y mode).
    TreeNodePtr acct;
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        if (!library_.bilibili_root().empty() && library_.bilibili_root()[0]->is_account)
            acct = library_.bilibili_root()[0];
    }
    if (!acct) {
        EVENT_LOG("B: login first (press 'a' to add a Bilibili account) to search");
        return;
    }
    std::string query = q;
    pool_.submit([this, query, acct]() {
        // Use the first logged-in account's SESSDATA if available (member-only search); else "".
        std::string sessdata;
        auto accounts = library_.load_bilibili_accounts();
        if (!accounts.empty())
            sessdata = accounts.front().sessdata;

        // Y23.1/Y23.2: search BOTH UP masters (bili_user) and videos; serialize to JSON (mode-specific),
        //   then the shared finalizer builds typed nodes + caches + adds the 🔍 record. UPs first.
        auto users = BilibiliAPI::search_users(sessdata, query);
        auto videos = BilibiliAPI::search_videos(sessdata, query);

        json results_json = json::array();
        for (const auto &u : users) {
            results_json.push_back({{"kind", "up"},
                                    {"mid", u.mid},
                                    {"uname", u.uname},
                                    {"url", u.url},
                                    {"upic", u.upic},
                                    {"fans", u.fans},
                                    {"sign", u.sign}});
        }
        for (const auto &v : videos) {
            results_json.push_back({{"kind", "video"},
                                    {"bvid", v.bvid},
                                    {"title", v.title},
                                    {"url", v.url},
                                    {"pic", v.pic},
                                    {"duration", v.duration}});
        }
        finalize_search("bilibili", acct->account_id, acct, query, results_json);
        EVENT_LOG(fmt::format("B: found {} users + {} videos for '{}' (cached)", users.size(),
                              videos.size(), query));
    });
}

} // namespace panicast
