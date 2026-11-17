#pragma once

#include <mutex>
#include <vector>

#include "panicast/core/types.h"             // TreeNodePtr
#include "panicast/storage/database.h"       // D11-3c: BilibiliAccount (load_bilibili_accounts return)
#include "panicast/ui/frontend.h"             // D12-3b: DisplayItem (ncurses-free view model for display_list_)

namespace panicast
{
// D10-4: LibraryService — owns the per-mode tree DATA MODEL (the 8 top-level item lists +
//   6 "loaded" flags). Extracted from App's god-object (ownership cut, mirrors D8b-1/D10-1/D10-2).
//
//   The members behind accessors are the same std::vector<TreeNodePtr> / bool App held; callers
//   read/write via the returned reference under tree_mutex() (D11-2: now co-located here — it
//   guards both the tree data above and the view state below, so the lock lives with what it
//   protects). display_list_/selected_idx_/view_start_ + pending_select_ also moved in (D11-2):
//   they were App view-state members the lock already guarded, so co-locating them removes App's
//   last view-state ownership and unblocks D11-3 (method-body relocation can reach them via
//   accessors instead of direct member access).
//
//   D11-3c: the tree-building METHODS (load_radio_root / load_accounts_root / load_bilibili_root /
//   load_tiktok_root / load_iptv_root / load_history_to_root) + the shared helpers
//   (make_search_history_child, load_bilibili_accounts) are NOW relocated here from App — the
//   library domain owns both its data (D10-4) and its construction. Bodies are verbatim from App
//   (internal member access: X_root_ / tree_mutex_ / X_loaded_); deps (AccountsManager /
//   DatabaseManager / Network / parsers / Persistence / crypto) are downward. The account-mode
//   builder lives here (no AccountService — D10-5); its UI-coupled ops (QR login / activate /
//   delete) stay in App.
class LibraryService
{
public:
    // ── 8 per-mode root item lists (top-level items live directly in the vector; the root NODE
    //   was eliminated — name kept xxx_root for historical reasons) ──
    std::vector<TreeNodePtr> &radio_root() { return radio_root_; }
    const std::vector<TreeNodePtr> &radio_root() const { return radio_root_; }
    std::vector<TreeNodePtr> &podcast_root() { return podcast_root_; }
    const std::vector<TreeNodePtr> &podcast_root() const { return podcast_root_; }
    std::vector<TreeNodePtr> &fav_root() { return fav_root_; }
    const std::vector<TreeNodePtr> &fav_root() const { return fav_root_; }
    std::vector<TreeNodePtr> &history_root() { return history_root_; }
    const std::vector<TreeNodePtr> &history_root() const { return history_root_; }
    std::vector<TreeNodePtr> &account_root() { return account_root_; }
    const std::vector<TreeNodePtr> &account_root() const { return account_root_; }
    std::vector<TreeNodePtr> &bilibili_root() { return bilibili_root_; }
    const std::vector<TreeNodePtr> &bilibili_root() const { return bilibili_root_; }
    std::vector<TreeNodePtr> &tiktok_root() { return tiktok_root_; }
    const std::vector<TreeNodePtr> &tiktok_root() const { return tiktok_root_; }
    std::vector<TreeNodePtr> &iptv_root() { return iptv_root_; }
    const std::vector<TreeNodePtr> &iptv_root() const { return iptv_root_; }

    // ── 6 per-mode "loaded" flags (were the root node's children_loaded before root removal) ──
    bool &radio_loaded() { return radio_loaded_; }
    bool radio_loaded() const { return radio_loaded_; }
    bool &podcast_loaded() { return podcast_loaded_; }
    bool podcast_loaded() const { return podcast_loaded_; }
    bool &account_loaded() { return account_loaded_; }
    bool account_loaded() const { return account_loaded_; }
    bool &bilibili_loaded() { return bilibili_loaded_; }
    bool bilibili_loaded() const { return bilibili_loaded_; }
    bool &tiktok_loaded() { return tiktok_loaded_; }
    bool tiktok_loaded() const { return tiktok_loaded_; }
    bool &iptv_loaded() { return iptv_loaded_; }
    bool iptv_loaded() const { return iptv_loaded_; }

    // ── View state + its guard (D11-2 relocated from App) ──
    //   tree_mutex guards BOTH the tree data (roots above) and this view state; it is the SAME
    //   recursive_mutex App held (moved, not copied) — every former `lock_guard<recursive_mutex>
    //   lock(tree_mutex)` site now writes `lock(library_.tree_mutex())`, lock order unchanged.
    //   display_list/selected_idx/view_start are the rendered list + cursor + scroll; pending_select_
    //   is the Y11 async-selection handoff (a pool task sets it under tree_mutex, the UI thread
    //   consumes it next frame). All read/written under tree_mutex() by the caller, as before.
    std::recursive_mutex &tree_mutex() { return tree_mutex_; }
    std::vector<DisplayItem> &display_list() { return display_list_; }
    const std::vector<DisplayItem> &display_list() const { return display_list_; }
    int &selected_idx() { return selected_idx_; }
    int selected_idx() const { return selected_idx_; }
    int &view_start() { return view_start_; }
    int view_start() const { return view_start_; }
    TreeNodePtr &pending_select() { return pending_select_; }
    const TreeNodePtr &pending_select() const { return pending_select_; }

    // ── Tree-building methods (D11-3c relocated from App — verbatim bodies, internal member
    //   access). Each (re)builds one mode's root list from its data source under tree_mutex. ──
    // Y-mode (Google account) root. UI-free (the QR-login/activate/delete ops stay in App).
    void load_accounts_root();
    // B-mode (Bilibili) root + its shared account loader (decrypts credentials; public — 3 other
    //   App bilibili ops also call it).
    void load_bilibili_root();
    std::vector<BilibiliAccount> load_bilibili_accounts();
    // T-mode (TikTok/Douyin) root.
    void load_tiktok_root();
    // I-mode (IPTV) catalog top-level (All/Region/Country/Category/Language/Custom).
    void load_iptv_root();
    // History root (most recent 100 played entries).
    void load_history_to_root();
    // Radio root (fetched from radio-browser OPML).
    void load_radio_root();
    // Shared "Search History" container child (Y + B use it) — pure node construction.
    TreeNodePtr make_search_history_child(TreeNodePtr account_node, const std::string &source,
                                          int account_id);

private:
    std::vector<TreeNodePtr> radio_root_, podcast_root_, fav_root_, history_root_, account_root_,
        bilibili_root_, tiktok_root_, iptv_root_;
    bool radio_loaded_ = false, podcast_loaded_ = false, account_loaded_ = false,
         bilibili_loaded_ = false, tiktok_loaded_ = false, iptv_loaded_ = false;
    // D11-2: view state + guard (moved from App — same objects, same invariants).
    std::recursive_mutex tree_mutex_;
    std::vector<DisplayItem> display_list_;
    int selected_idx_ = 0, view_start_ = 0;
    TreeNodePtr pending_select_;
};

} // namespace panicast
