// D11-3c: LibraryService implementation — the tree-building METHODS (load_*_root) relocated here
//   from App. LibraryService already owned the per-mode tree DATA (the 8 root lists + 6 loaded
//   flags, D10-4) + the view state + tree_mutex (D11-2); D11-3c moves the methods that POPULATE
//   those roots so the library domain owns both its data and its construction. Each method body is
//   relocated VERBATIM from its App site (behaviour-identical); only the access changes —
//   `library_.X_root()` → `X_root_`, `library_.tree_mutex()` → `tree_mutex_`, `library_.X_loaded()`
//   → `X_loaded_` (internal member access, same objects). Dependencies (AccountsManager /
//   DatabaseManager / Network / parsers / Persistence / crypto) are all downward (service →
//   storage/runtime/core), layering-correct. The account-mode builder (load_accounts_root) lives
//   here too — there is no AccountService (D10-5 decided against one), and it is UI-free (the
//   UI-coupled account ops — QR login / activate / delete — stay in App).
#include "panicast/app/library_service.h"

#include <mutex>

#include <fmt/format.h>

#include "panicast/core/crypto.h" // D11-3c: token_open/token_seal/machine_key/Key32 (load_bilibili_accounts)
#include "panicast/core/event_log.h"      // EVENT_LOG (load_radio_root)
#include "panicast/core/logger.h"         // LOG (load_bilibili_accounts)
#include "panicast/net/network.h"         // Network::fetch (load_radio_root)
#include "panicast/net/url_classifier.h"  // URLClassifier (load_history_to_root)
#include "panicast/parsers/opml_parser.h" // OPMLParser (load_radio_root)
#include "panicast/storage/accounts.h"    // AccountsManager (load_accounts_root)
#include "panicast/storage/database.h"    // DatabaseManager + BilibiliAccount/TiktokAccount
#include "panicast/storage/persistence.h" // Persistence::save_cache (load_radio_root)

namespace panicast
{

// ── Shared node-construction helper (relocated from App::make_search_history_child) ──
//   Y23.2: shared "Search History" container child — used by both Y (load_accounts_root) and
//   B (expand_bilibili_account) so the container shape is identical. Pure node construction.
TreeNodePtr LibraryService::make_search_history_child(TreeNodePtr account_node,
                                                      const std::string &source, int account_id) {
    auto shist = std::make_shared<TreeNode>();
    shist->title = "Search History";
    shist->type = NodeType::FOLDER;
    shist->is_search_parent = true;
    shist->url = "searchhist:" + source;
    shist->account_id = account_id;
    shist->children_loaded = false;
    shist->parent = account_node;
    return shist;
}

// ── Y-mode (Google account) root builder (relocated from App::load_accounts_root) ──
void LibraryService::load_accounts_root() {
    // Y02: ensure a primary account is active. The active account is the global "primary" used by
    //   P-mode YouTube parsing/subscribe; default to 1# (first logged-in) when none is set yet.
    if (AccountsManager::instance().active_account_id() <= 0) {
        auto all0 = AccountsManager::instance().list_accounts();
        if (!all0.empty())
            AccountsManager::instance().set_active_account(all0.front().account_id);
    }
    auto accounts = AccountsManager::instance().list_accounts();
    int active = AccountsManager::instance().active_account_id();
    std::lock_guard<std::recursive_mutex> lock(tree_mutex_);
    account_root_.clear();

    if (accounts.empty()) {
        auto hint = std::make_shared<TreeNode>();
        hint->title = "(no Google account — press 'a' to login)";
        hint->type = NodeType::FOLDER;
        hint->is_account = false;
        hint->account_id = 0;
        account_root_.push_back(hint);
        account_loaded_ = true;
        return;
    }

    for (const auto &a : accounts) {
        auto node = std::make_shared<TreeNode>();
        node->is_account = true;
        node->account_id = a.account_id;
        node->type = NodeType::FOLDER;
        node->title = a.label.empty() ? a.google_email : a.label;
        if (a.google_email != node->title) {
            node->title = node->title + "  <" + a.google_email + ">";
        }
        node->account_email = a.google_email;
        node->account_token_expires = a.token_expires_at;
        node->account_last_sync = a.last_sync_at;
        node->account_sub_count =
            (int)AccountsManager::instance().load_subscriptions(a.account_id).size();
        node->is_cached = (a.account_id == active); // reuse is_cached to mark "active"
        node->expanded = false;
        node->children_loaded = false;
        node->parent.reset();

        // Two fixed children: History + Subscriptions (lazy-loaded on expand).
        auto hist = std::make_shared<TreeNode>();
        hist->title = "History";
        hist->type = NodeType::FOLDER;
        hist->is_yt_history = true;
        hist->account_id = a.account_id;
        hist->parent = node;
        node->children.push_back(hist);

        auto subs = std::make_shared<TreeNode>();
        subs->title = "Subscriptions";
        subs->type = NodeType::FOLDER;
        subs->is_yt_subscriptions = true;
        subs->account_id = a.account_id;
        subs->parent = node;
        node->children.push_back(subs);

        // Y23.1/Y23.2: Search History container (mirror O-mode online_root) — shared shape with B mode.
        node->children.push_back(make_search_history_child(node, "youtube", a.account_id));

        account_root_.push_back(node);
    }
    account_loaded_ = true;
}

// ── Bilibili account loader (relocated from the file-static load_bilibili_accounts in
//   app_bilibili.cpp). Public: load_bilibili_root + 3 other App bilibili ops call it. ──
std::vector<BilibiliAccount> LibraryService::load_bilibili_accounts() {
    // Y24.27: use DatabaseManager (was direct sqlite3_* — bypassed encapsulation).
    //   P2-S7: sessdata/bili_jct/dedeuserid are stored encrypted (token_seal). Decrypt on load;
    //   legacy v43 plaintext rows fail token_open → treated as plaintext and re-encrypted in place.
    auto raw = DatabaseManager::instance().list_bilibili_accounts();
    std::vector<BilibiliAccount> out;
    bool reencrypt_needed = false;
    const Key32 mk = machine_key();
    for (auto &a : raw) {
        std::string s_enc = a.sessdata, j_enc = a.bili_jct, d_enc = a.dedeuserid;
        if (s_enc.empty() || !token_open(mk, s_enc, a.sessdata)) {
            a.sessdata = s_enc;
            if (!s_enc.empty())
                reencrypt_needed = true;
        }
        if (j_enc.empty() || !token_open(mk, j_enc, a.bili_jct)) {
            a.bili_jct = j_enc;
            if (!j_enc.empty())
                reencrypt_needed = true;
        }
        if (d_enc.empty() || !token_open(mk, d_enc, a.dedeuserid)) {
            a.dedeuserid = d_enc;
            if (!d_enc.empty())
                reencrypt_needed = true;
        }
        out.push_back(a);
    }
    if (reencrypt_needed) {
        for (const auto &a : out) {
            BilibiliAccount enc = a;
            enc.sessdata = a.sessdata.empty() ? "" : token_seal(mk, a.sessdata);
            enc.bili_jct = a.bili_jct.empty() ? "" : token_seal(mk, a.bili_jct);
            enc.dedeuserid = a.dedeuserid.empty() ? "" : token_seal(mk, a.dedeuserid);
            DatabaseManager::instance().upsert_bilibili_account(enc);
        }
        LOG("[Bilibili] re-encrypted legacy plaintext credentials at rest");
    }
    return out;
}

// ── B-mode (Bilibili) root builder (relocated from App::load_bilibili_root) ──
void LibraryService::load_bilibili_root() {
    std::lock_guard<std::recursive_mutex> lock(tree_mutex_);
    bilibili_root_.clear();
    auto accounts = load_bilibili_accounts();
    if (accounts.empty()) {
        auto hint = std::make_shared<TreeNode>();
        hint->title = "(no Bilibili account — press 'a' to login)";
        hint->type = NodeType::FOLDER;
        hint->children_loaded = true;
        hint->parent.reset();
        bilibili_root_.push_back(hint);
    } else {
        for (const auto &a : accounts) {
            auto node = std::make_shared<TreeNode>();
            node->title = a.uname.empty() ? ("Bili #" + a.uid) : (a.uname + "  <Bili>");
            node->type = NodeType::FOLDER;
            node->is_account = true; // reuse is_account flag
            node->account_id = a.id;
            node->expanded = false;
            node->children_loaded = false;
            node->parent.reset();
            bilibili_root_.push_back(node);
        }
    }
    bilibili_loaded_ = true;
}

// ── T-mode (TikTok/Douyin) root builder (relocated from App::load_tiktok_root). The one-line
//   file-static load_tiktok_accounts (its only caller) is inlined as list_tiktok_accounts(). ──
void LibraryService::load_tiktok_root() {
    std::lock_guard<std::recursive_mutex> lock(tree_mutex_);
    tiktok_root_.clear();
    // Y24.16: root title is static — the region is shown only in the status-bar border
    //   (🎵 TIKTOK [US] / 🎵 Douyin [CN]), so 'b' switching region can't desync the root label.
    auto accounts = DatabaseManager::instance().list_tiktok_accounts();
    if (accounts.empty()) {
        auto hint = std::make_shared<TreeNode>();
        hint->title = "(no TikTok/Douyin items — press 'a' to add @user/video URL, '/' to open "
                      "@user/#tag/URL)";
        hint->type = NodeType::FOLDER;
        hint->children_loaded = true;
        hint->parent.reset();
        tiktok_root_.push_back(hint);
    } else {
        for (const auto &a : accounts) {
            auto node = std::make_shared<TreeNode>();
            node->is_account = true; // reuse is_account flag so 'd' can delete it
            node->account_id = a.id;
            node->url = a.url;
            node->parent.reset();
            if (a.platform == "douyin_video") {
                // Y24.16: Douyin single video (option A) — playable leaf. yt-dlp has no DouyinUserIE,
                //   so Douyin can't list a creator's videos; we subscribe individual videos instead.
                node->title = (a.uname.empty() ? std::string("Douyin Video") : a.uname);
                node->type = NodeType::PODCAST_EPISODE;
                node->children_loaded = true; // playable leaf: Enter plays (peers = siblings)
            } else {
                std::string tag = (a.platform == "douyin") ? " Douyin" : " TikTok";
                node->title = (a.uname.empty() ? a.handle : a.uname) + " <" + tag + ">";
                node->type = NodeType::FOLDER;
                node->expanded = false;
                node->children_loaded =
                    false; // spawn_load_feed classifies → TIKTOK_USER / DOUYIN_USER
            }
            tiktok_root_.push_back(node);
        }
    }
    tiktok_loaded_ = true;
}

// ── I-mode (IPTV) root builder (relocated from App::load_iptv_root) ──
void LibraryService::load_iptv_root() {
    std::lock_guard<std::recursive_mutex> lock(tree_mutex_);
    if (!iptv_root_.empty())
        return; // catalog already built

    auto add = [&](const std::string &title, const std::string &url) {
        auto n = std::make_shared<TreeNode>();
        n->title = title;
        n->url = url;
        n->type = NodeType::FOLDER;
        n->children_loaded = false; // lazy fetch on expand
        n->parent.reset();
        iptv_root_.push_back(n);
    };
    add("All Channels", "iptv:all");
    add("By Region", "iptv:regions");
    add("By Country", "iptv:countries");
    add("By Category", "iptv:categories");
    add("By Language", "iptv:languages");
    add("Custom", "iptv:custom");
    iptv_loaded_ = true;
}

// ── History root builder (relocated from App::load_history_to_root) ──
void LibraryService::load_history_to_root() {
    auto history = DatabaseManager::instance().get_history(100); // get the most recent 100 entries
    std::lock_guard<std::recursive_mutex> lock(tree_mutex_);
    history_root_.clear();

    for (const auto &[url, title, timestamp, mt, h_artist, h_album, h_art] : history) {
        auto node = std::make_shared<TreeNode>();
        node->title = title.empty() ? "Unknown" : title;
        node->url = url;
        // N06: display category from the DB (no runtime URL inference of the icon).
        node->media_type = static_cast<MediaType>(mt);
        node->media_type_set = true;
        // Determine type from the URL
        URLType url_type = URLClassifier::classify(url);
        if (URLClassifier::is_video(url_type)) {
            node->type = NodeType::PODCAST_EPISODE;
            node->is_youtube = true;
        } else if (url.find(".mp3") != std::string::npos || url.find(".aac") != std::string::npos ||
                   url.find(".m3u8") != std::string::npos) {
            node->type = NodeType::RADIO_STREAM;
        } else {
            node->type = NodeType::PODCAST_EPISODE;
        }
        node->children_loaded = true;
        node->subtext = timestamp; // store timestamp in subtext
        history_root_.push_back(node);
    }
}

// ── Radio root builder (relocated from App::load_radio_root) ──
void LibraryService::load_radio_root() {
    EVENT_LOG("Fetching Radio stations...");
    std::string data = Network::fetch("https://opml.radiotime.com/Browse.ashx?formats=mp3,aac");
    if (!data.empty()) {
        auto parsed = OPMLParser::parse(data);
        if (parsed) {
            std::lock_guard<std::recursive_mutex> lock(tree_mutex_);
            radio_root_ = parsed->children;
            radio_loaded_ = true;
            EVENT_LOG(fmt::format("Radio: {} stations loaded", radio_root_.size()));
            Persistence::save_cache(radio_root_, podcast_root_);
        }
    }
}

} // namespace panicast
