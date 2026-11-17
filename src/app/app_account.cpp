// Y01: Y-mode (Google account) controller — account tree, QR login, node activation.
// See app.h declarations.
#include "panicast/app/app.h"

#include <algorithm>
#include <cctype>
#include <thread>
#include <chrono>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"

namespace panicast
{

// D11-3c: load_accounts_root relocated to LibraryService (the account_root_ owner). The UI-coupled
//   account ops below (QR login / activate / delete / refresh) stay here in App.
// ── QR-login popup + device-flow poll ────────────────────────────────────────
namespace
{
// Draw the QR login popup and poll for the token. Returns true on success (token in `tr`).
bool qr_login_poll(const GoogleOAuth::DeviceCode &dc, GoogleOAuth::TokenResult &tr_out) {
    std::string qr_text = dc.verification_url;
    // Append user_code as a query param so scanning opens the page with the code prefilled.
    qr_text += (qr_text.find('?') == std::string::npos ? "?" : "&");
    qr_text += "user_code=" + dc.user_code;

    auto qr_rows = render_qr_rows(qr_text);
    int qr_h = qr_available() ? (int)qr_rows.size() : 0;
    int qr_w = qr_h ? (int)qr_rows[0].size() : 0;

    int text_h = 9; // instructions + user_code + url + browser-hint + status + cancel
    int pop_h = std::max(qr_h, 1) + text_h + 4;
    int pop_w = std::max({60, qr_w + 4, (int)dc.verification_url.size() + 6});
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
    if (!win)
        return false;
    keypad(win, TRUE);
    nodelay(win, TRUE); // non-blocking wgetch for cancel

    auto draw = [&](const std::string &status) {
        werase(win);
        box(win, 0, 0);
        int y = 1;
        mvwprintw(win, y++, 2, "Google Login (SmartTube-style)");
        y++;
        if (qr_h > 0) {
            int xoff = 2;
            for (const auto &row : qr_rows) {
                if (y >= pop_h - 1)
                    break;
                mvwprintw(win, y, xoff, "%s", row.c_str());
                y++;
            }
        } else {
            mvwprintw(win, y++, 2, "(libqrencode not installed — no QR image)");
        }
        y++;
        mvwprintw(win, y++, 2, "Code: %s", dc.user_code.c_str());
        mvwprintw(win, y++, 2, "URL:  %s", dc.verification_url.c_str());
        y++;
        mvwprintw(win, y++, 2, "Open in Chrome/Safari; do not use WeChat/QR-scan in-app browser");
        mvwprintw(win, y++, 2, "%s", status.c_str());
        mvwprintw(win, y++, 2, "Press 'q' to cancel");
        wrefresh(win);
    };

    draw("Scanning... waiting for authorization");

    int interval = dc.interval > 0 ? dc.interval : 5;
    int expires = dc.expires_in > 0 ? dc.expires_in : 1800;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(expires);
    bool ok = false;
    while (std::chrono::steady_clock::now() < deadline) {
        // Check for cancel key.
        int ch = wgetch(win);
        if (ch == 'q' || ch == 'Q') {
            draw("Cancelled");
            break;
        }

        GoogleOAuth::TokenResult tr = GoogleOAuth::poll_token(dc.device_code);
        if (tr.ok) {
            tr_out = tr;
            ok = true;
            draw("Authorized! ✓");
            break;
        }
        if (tr.error == "authorization_pending") {
            // keep polling
        } else if (tr.error == "slow_down") {
            interval += 5;
        } else {
            draw(std::string("Error: ") + tr.error);
            LOG(fmt::format("[Y] token poll error: {}", tr.error));
            EVENT_LOG(fmt::format("Y: login failed — {}", tr.error));
            break;
        }
        // Sleep in small slices so 'q' stays responsive.
        for (int s = 0; s < interval * 10; ++s) {
            int c = wgetch(win);
            if (c == 'q' || c == 'Q') {
                draw("Cancelled");
                goto done;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
done:
    delwin(win);
    // Restore main-UI input focus on return. The popup used its own window (keypad/nodelay on `win`)
    // and polled wgetch for seconds; without restoring, (a) typeahead / half-read escape sequences
    // accumulated during the poll make the main loop's wget_wch(stdscr) mis-parse j/k, and (b) stdscr's
    // input mode must be re-asserted. flushinp + keypad/timeout/curs_set/noecho bring the UI back.
    flushinp();
    keypad(stdscr, TRUE);
    timeout(30);
    curs_set(0);
    noecho();
    touchwin(stdscr);
    refresh();
    return ok;
}
} // namespace

void App::start_account_login(bool another) {
    (void)another; // both a/A add an account via the same flow; 'another' only signals intent.
    EVENT_LOG("Y: requesting Google device code...");
    auto dc = GoogleOAuth::request_device_code();
    if (!dc.ok) {
        // Errors must NOT pop up — log to file and print in the LOG panel. Only the QR pops up.
        std::string err = dc.error.empty() ? "network error" : dc.error;
        LOG(fmt::format("[Y] login failed: {}", err));
        EVENT_LOG(fmt::format("Y: login failed — {}", err));
        return;
    }
    GoogleOAuth::TokenResult tr;
    if (!qr_login_poll(dc, tr)) {
        EVENT_LOG("Y: login cancelled or failed");
        return;
    }
    // Y11: fetch_identity (Data API network) + account setup + initial sync all run in a pool
    //   thread so the UI returns immediately after the QR scan (no post-scan freeze). The account
    //   appears in the tree once load_accounts_root() runs at the end of this task.
    EVENT_LOG("Y: login authorized; finishing + syncing...");
    pool_.submit([this, tr]() {
        // Fetch channel identity (channel id + title) for the new account.
        std::string channel_id, label;
        auto id = GoogleOAuth::fetch_identity(tr.access_token);
        if (id.ok) {
            channel_id = id.channel_id;
            label = id.title;
        }

        int64_t expires_at = tr.obtained_at + tr.expires_in;

        // De-duplicate: if this Google account (same channel_id) is already logged in, refresh
        // its tokens and reactivate it instead of creating a duplicate row.
        int aid = 0;
        if (!channel_id.empty()) {
            for (const auto &a : AccountsManager::instance().list_accounts()) {
                if (!a.channel_id.empty() && a.channel_id == channel_id) {
                    aid = a.account_id;
                    break;
                }
            }
        }
        if (aid > 0) {
            AccountsManager::instance().update_tokens(aid, tr.access_token, tr.refresh_token,
                                                      expires_at, tr.scope);
            if (!label.empty())
                AccountsManager::instance().set_label(aid, label);
            AccountsManager::instance().touch_login(aid);
            AccountsManager::instance().set_active_account(aid);
            EVENT_LOG(fmt::format("Y: account #{} re-logged in ({}); syncing...", aid, label));
        } else {
            aid = AccountsManager::instance().add_account(
                /*email*/ "", /*gaia*/ "", channel_id, tr.access_token, tr.refresh_token,
                expires_at, tr.scope, label);
            if (aid <= 0) {
                LOG("[Y] login failed: could not save account");
                EVENT_LOG("Y: login failed — could not save account");
                return;
            }
            AccountsManager::instance().touch_login(aid);
            AccountsManager::instance().set_active_account(aid);
            EVENT_LOG(fmt::format("Y: account #{} logged in ({}); syncing...", aid, label));
        }

        // Initial sync (subscriptions + watch history) — best-effort, in this pool task.
        sync_account_subscriptions(aid);
        sync_account_history(aid);
        library_.load_accounts_root();
    });
    // Success is already reported in the LOG panel (EVENT_LOG above); no popup — only QR pops up.
}

// ── Y-mode node activation ───────────────────────────────────────────────────
void App::enter_account_node(TreeNodePtr node) {
    if (!node)
        return;

    if (node->is_account) {
        // Activate this account + expand to show its history/subscriptions children.
        AccountsManager::instance().set_active_account(node->account_id);
        EVENT_LOG(fmt::format("Y: active account -> #{} ({})", node->account_id, node->title));
        {
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            for (auto &a : library_.account_root())
                a->is_cached = (a->account_id == node->account_id);
        }
        node->expanded = !node->expanded;
        return;
    }

    if (node->is_yt_history || node->is_yt_subscriptions) {
        if (!node->expanded) {
            expand_account_child(node);
            node->expanded = true;
        } else {
            node->expanded = false;
        }
        return;
    }

    // Y23.1: Search History container — lazy-load the account's past search records from the cache.
    if (node->is_search_parent) {
        if (!node->children_loaded)
            expand_search_history(node);
        else
            node->expanded = !node->expanded;
        return;
    }
    // Y23.1: a 🔍 search record — lazy-load cached results on first expand.
    if (node->is_yt_search && node->url.rfind("search:", 0) == 0) {
        if (!node->children_loaded) {
            load_search_history_children(node);
            node->expanded = true;
        } else
            node->expanded = !node->expanded;
        return;
    }

    if (node->is_yt_channel) {
        // Expand the channel → load its videos.
        if (!node->children_loaded && !node->loading) {
            node->loading = true;
            TreeNodePtr n = node;
            std::string url = node->url;
            int aid = node->account_id;
            pool_.submit([this, n, url, aid]() {
                // CACHE-1: consult youtube_cache first (mirror parse_feed_by_type). A hit with a
                //   non-empty video list skips the network; an empty cached list is a miss (Y23.3).
                if (YouTubeCache::instance().has(url)) {
                    auto cached = YouTubeCache::instance().get(url);
                    if (!cached.videos.empty()) {
                        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                        n->children.clear();
                        int vt = 0;
                        for (const auto &v : cached.videos) {
                            auto ep = std::make_shared<TreeNode>();
                            ep->type = NodeType::PODCAST_EPISODE;
                            ep->title = v.title;
                            ep->url = v.url;
                            ep->is_youtube = true;
                            ep->art_url = v.thumbnail; // ART-2
                            ep->artist = n->title;     // META-2: channel = artist
                            ep->album = "YouTube";     // META-2: platform context
                            ep->track_num = ++vt;
                            ep->children_loaded = true;
                            ep->parent = n;
                            n->children.push_back(ep);
                        }
                        n->children_loaded = true;
                        n->expanded = true;
                        n->loading = false;
                        EVENT_LOG(
                            fmt::format("Y: channel cache hit: {} videos", cached.videos.size()));
                        return;
                    }
                }
                std::vector<YouTubeVideoInfo> vids;
                std::string err;
                int cnt = 0;

                // Y04: prefer Data API (OAuth token) for the episode list — it works WITHOUT
                //   cookies / proxy-for-youtube / a JS runtime, using the same token that powers
                //   subscriptions. yt-dlp (below) is the fallback when the token/account is absent.
                GoogleAccount acc;
                if (aid > 0 && AccountsManager::instance().get_tokens(aid, acc)) {
                    std::string cid;
                    auto p = url.find("/channel/");
                    if (p != std::string::npos) {
                        cid = url.substr(p + 9);
                        auto sl = cid.find('/');
                        if (sl != std::string::npos)
                            cid = cid.substr(0, sl);
                    }
                    if (!cid.empty()) {
                        vids = GoogleOAuth::fetch_channel_videos(acc.access_token, cid);
                        cnt = (int)vids.size();
                        if (cnt > 0)
                            EVENT_LOG(
                                fmt::format("Y: channel {} -> {} videos (Data API)", cid, cnt));
                    }
                }

                // Fallback: yt-dlp --flat-playlist (needs cookies + proxy; list extraction needs no JS runtime).
                if (cnt == 0) {
                    vids.clear();
                    cnt = YouTubeChannelParser::parse_video_list(url, n, vids, err);
                    if (cnt > 0)
                        EVENT_LOG(fmt::format("Y: channel -> {} videos (yt-dlp)", cnt));
                }

                {
                    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
                    if (cnt > 0) {
                        // OAuth path returns a vector (build nodes here); yt-dlp path already built into n->children.
                        if (n->children.empty() && !vids.empty()) {
                            int vt = 0;
                            for (const auto &v : vids) {
                                auto ep = std::make_shared<TreeNode>();
                                ep->type = NodeType::PODCAST_EPISODE;
                                ep->title = v.title;
                                ep->url = v.url;
                                ep->is_youtube = true;
                                ep->art_url = v.thumbnail; // ART-2
                                ep->artist = n->title;     // META-2
                                ep->album = "YouTube";     // META-2
                                ep->track_num = ++vt;
                                ep->children_loaded = true;
                                ep->parent = n;
                                n->children.push_back(ep);
                            }
                        }
                        n->children_loaded = true;
                        n->expanded = true;
                        int yt = 0;
                        for (auto &c : n->children) {
                            c->is_youtube = true;
                            c->parent = n;
                            // META-2: yt-dlp path (children built by the parser) — fill
                            //   display metadata post-hoc when absent.
                            if (c->artist.empty())
                                c->artist = n->title;
                            if (c->album.empty())
                                c->album = "YouTube";
                            if (c->track_num == 0)
                                c->track_num = ++yt;
                        }
                        // ART-2: the channel row itself gets the first video's thumbnail
                        //   when nothing better is on the node (avatar needs an extra API
                        //   call; this approximation reads well in the remote browse).
                        if (n->art_url.empty() && !n->children.empty())
                            n->art_url = n->children.front()->art_url;
                    } else {
                        n->parse_failed = true;
                        n->error_msg = err.empty() ? "no videos" : err;
                    }
                    n->loading = false;
                }
                // CACHE-1: persist the fetched video list outside the tree lock (same global cache
                //   keyed by channel URL as P mode's cache_youtube_videos).
                if (cnt > 0 && !vids.empty())
                    YouTubeCache::instance().update(url, n->title, vids);
            });
        } else {
            node->expanded = !node->expanded;
        }
        return;
    }

    // A video leaf: play it (its peers become the playlist) and record to account history.
    if (node->type == NodeType::PODCAST_EPISODE) {
        play_episode(node);
        // Determine owning account (nearest account ancestor).
        TreeNodePtr p = node->parent.lock();
        while (p && !p->is_account)
            p = p->parent.lock();
        if (p && p->account_id > 0) {
            // Extract video id from watch?v=ID.
            std::string vid;
            auto pos = node->url.find("v=");
            if (pos != std::string::npos) {
                vid = node->url.substr(pos + 2);
                auto end = vid.find_first_of("&");
                if (end != std::string::npos)
                    vid = vid.substr(0, end);
            }
            record_youtube_play(vid, node->title, p->title);
        }
    }
}

// ── Lazy-expand "History" / "Subscriptions" ──────────────────────────────────────
void App::expand_account_child(TreeNodePtr node) {
    if (!node)
        return;
    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
    node->children.clear();
    int aid = node->account_id;

    if (node->is_yt_history) {
        auto rows = AccountsManager::instance().load_history(aid);
        for (const auto &r : rows) {
            auto c = std::make_shared<TreeNode>();
            c->title = r.title.empty() ? r.video_id : r.title;
            c->url = "https://www.youtube.com/watch?v=" + r.video_id;
            c->type = NodeType::PODCAST_EPISODE;
            c->is_youtube = true;
            c->is_db_cached = true;
            c->children_loaded = true;
            c->subtext = r.channel_name + (r.source == "local" ? " · local" : " · youtube");
            c->parent = node;
            node->children.push_back(c);
        }
        node->children_loaded = true;
    } else if (node->is_yt_subscriptions) {
        auto rows = AccountsManager::instance().load_subscriptions(aid);
        for (const auto &r : rows) {
            auto c = std::make_shared<TreeNode>();
            c->title = r.channel_name.empty() ? r.channel_id : r.channel_name;
            c->url = r.channel_url.empty() ? ("https://www.youtube.com/channel/" + r.channel_id)
                                           : r.channel_url;
            c->type = NodeType::FOLDER;
            c->is_yt_channel = true;
            c->account_id = aid;
            c->is_youtube = true;
            c->children_loaded = false;
            c->parent = node;
            node->children.push_back(c);
        }
        node->children_loaded = true;
    }
}

// ── d: delete account ────────────────────────────────────────────────────────
void App::delete_account_node(TreeNodePtr node) {
    if (!node || !node->is_account) {
        // If on a child, find the owning account.
        TreeNodePtr p = node;
        while (p && !p->is_account)
            p = p->parent.lock();
        if (!p)
            return;
        node = p;
    }
    if (!frontend_->confirm_box("Delete Google account '" + node->title +
                                "'? (all its YouTube data)"))
        return;
    AccountsManager::instance().delete_account(node->account_id);
    EVENT_LOG(fmt::format("Y: account #{} deleted", node->account_id));
    library_.load_accounts_root();
}

// ── r: re-sync the selected account ──────────────────────────────────────────
void App::resync_account_node(TreeNodePtr node) {
    if (!node)
        return;
    TreeNodePtr p = node;
    while (p && !p->is_account)
        p = p->parent.lock();
    if (!p) {
        EVENT_LOG("Y: select an account to sync");
        return;
    }
    int aid = p->account_id;
    EVENT_LOG(fmt::format("Y: syncing account #{} ...", aid));
    pool_.submit([this, aid]() {
        sync_account_subscriptions(aid);
        sync_account_history(aid);
        library_.load_accounts_root();
    });
}

// Y-fix: 'r' on the Subscriptions container — re-sync only subscriptions from Google and reload
//   this one subtree. Unlike resync_account_node (which rebuilds the WHOLE account tree, collapsing
//   every expanded channel), this preserves the rest of the tree and other accounts' expansion.
void App::refresh_account_subs(TreeNodePtr node) {
    if (!node || !node->is_yt_subscriptions)
        return;
    int aid = node->account_id;
    TreeNodePtr n = node;
    EVENT_LOG(fmt::format("Y: re-syncing subscriptions for account #{} ...", aid));
    pool_.submit([this, aid, n]() {
        sync_account_subscriptions(aid); // refresh DB rows from Google (network)
        expand_account_child(n); // clears + rebuilds this subtree from DB (under tree_mutex)
        {
            std::lock_guard<std::recursive_mutex> lk(library_.tree_mutex());
            n->expanded = true;
        }
        EVENT_LOG("Y: subscriptions re-synced");
    });
}

// Y-fix: 'r' on the History container — re-sync only watch history from Google and reload this subtree.
void App::refresh_account_history(TreeNodePtr node) {
    if (!node || !node->is_yt_history)
        return;
    int aid = node->account_id;
    TreeNodePtr n = node;
    EVENT_LOG(fmt::format("Y: re-syncing watch history for account #{} ...", aid));
    pool_.submit([this, aid, n]() {
        sync_account_history(aid);
        expand_account_child(n);
        {
            std::lock_guard<std::recursive_mutex> lk(library_.tree_mutex());
            n->expanded = true;
        }
        EVENT_LOG("Y: watch history re-synced");
    });
}

// ── Y02: YouTube search + subscribe ──────────────────────────────────────────
void App::perform_youtube_search(const std::string &preset) {
    std::string q;
    if (!preset.empty()) {
        q = preset;
    } else {
        q = frontend_->input_box("Search YouTube  [c/v/p/m prefix to filter]");
        if (UI::is_input_cancelled(q)) {
            EVENT_LOG("Search cancelled");
            return;
        }
    }
    if (q.empty())
        return;

    // Optional single-letter type filter prefix: "c "/"v "/"p "/"m ".
    std::string filter;
    bool music = false;
    if (q.size() >= 2 && q[1] == ' ' &&
        (q[0] == 'c' || q[0] == 'v' || q[0] == 'p' || q[0] == 'm' || q[0] == 'C' || q[0] == 'V' ||
         q[0] == 'P' || q[0] == 'M')) {
        char c = (char)std::tolower((unsigned char)q[0]);
        if (c == 'm')
            music = true;
        else if (c == 'c')
            filter = "channel";
        else if (c == 'v')
            filter = "video";
        else if (c == 'p')
            filter = "playlist";
        q = q.substr(2);
        size_t s = q.find_first_not_of(' ');
        if (s == std::string::npos)
            return;
        q = q.substr(s);
    }
    if (q.empty())
        return;

    int aid = AccountsManager::instance().active_account_id();
    if (aid <= 0) {
        LOG("[Y] search: no active account");
        EVENT_LOG("Y: login first (press 'a' on an account)");
        return;
    }
    GoogleAccount acc;
    if (!AccountsManager::instance().get_tokens(aid, acc)) {
        LOG("[Y] search: no valid token");
        EVENT_LOG("Y: token unavailable — re-login");
        return;
    }
    EVENT_LOG(fmt::format("Y: searching YouTube '{}' ...", q));
    // Y11: run the Data API search + tree build in a pool thread so the UI stays fluid (search is
    //   ~1-2s network). The result folder is added under tree_mutex; pending_select_ moves the
    //   cursor there on the next UI frame (consumed in App::run's flatten block).
    std::string token = acc.access_token;
    pool_.submit([this, q, filter, music, aid, token]() {
        auto results = GoogleOAuth::search(token, q, filter, music);

        TreeNodePtr acct;
        {
            std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
            for (auto &c : library_.account_root())
                if (c->is_account && c->account_id == aid) {
                    acct = c;
                    break;
                }
        }
        if (!acct) {
            EVENT_LOG("Y: active account not found in tree");
            return;
        }

        // Y23.1/Y23.2: serialize results to JSON (mode-specific); the finalizer (build nodes + cache +
        //   add 🔍 record) is shared with B mode via finalize_search.
        json results_json = json::array();
        for (const auto &r : results) {
            std::string kind = (r.kind == YouTubeSearchRow::Kind::CHANNEL) ? "channel"
                               : (r.kind == YouTubeSearchRow::Kind::PLAYLIST)
                                   ? "playlist"
                                   : (r.music ? "music" : "video");
            results_json.push_back({{"kind", kind},
                                    {"id", r.id},
                                    {"title", r.title},
                                    {"url", r.url},
                                    {"channel_title", r.channel_title},
                                    {"thumbnail", r.thumbnail_url}});
        }
        finalize_search("youtube", aid, acct, q, results_json);
        EVENT_LOG(fmt::format("Y: found {} results for '{}' (cached)", results.size(), q));
    });
}

void App::subscribe_youtube_channel(TreeNodePtr node) {
    if (!node || !node->is_yt_search_result || !node->is_yt_channel || node->channel_id.empty()) {
        EVENT_LOG("Y: select a [C] channel result to subscribe");
        return;
    }
    int aid =
        node->account_id > 0 ? node->account_id : AccountsManager::instance().active_account_id();
    if (aid <= 0) {
        EVENT_LOG("Y: login first");
        return;
    }
    GoogleAccount acc;
    if (!AccountsManager::instance().get_tokens(aid, acc)) {
        EVENT_LOG("Y: token unavailable");
        return;
    }
    std::string who = node->channel_name.empty() ? node->title : node->channel_name;
    std::string channel_id = node->channel_id;
    EVENT_LOG(fmt::format("Y: subscribing {} ...", who));
    // Y11: subscribe + re-fetch subscriptions are Data API network calls — run in a pool thread so
    //   the UI stays fluid. load_accounts_root() (tree rebuild, under tree_mutex) runs at the end.
    std::string token = acc.access_token;
    pool_.submit([this, aid, token, channel_id, who]() {
        bool ok = GoogleOAuth::subscribe(token, channel_id);
        if (!ok) {
            LOG(fmt::format("[Y] subscribe failed: {}", channel_id));
            EVENT_LOG("Y: subscribe failed (see log)");
            return;
        }
        // re-fetch subscriptions to keep DB in sync with YouTube, then refresh the tree
        auto subs = GoogleOAuth::fetch_subscriptions(token);
        AccountsManager::instance().replace_subscriptions(aid, subs);
        library_.load_accounts_root();
        EVENT_LOG(fmt::format("Y: subscribed {}", who));
    });
}

} // namespace panicast
