// Y01: AccountsManager — Google account registry + per-account YouTube data, single shared DB.
//   Owns all SQL against the Y01 account tables (accounts / youtube_subscriptions /
//   youtube_history / account_sync_state). Shares DatabaseManager's single sqlite connection
//   (raw_handle/raw_mutex) — never opens a second connection.
//   OAuth tokens are stored encrypted (core/crypto.cpp) in the accounts table; the manager
//   decrypts on demand and keeps the active account's plaintext tokens cached in memory.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "panicast/core/crypto.h"

namespace panicast
{

struct GoogleAccount {
    int account_id = 0;
    std::string type; // "google"
    std::string label;
    std::string google_email;
    std::string gaia_id;
    std::string channel_id;
    std::string token_scope;
    int64_t token_expires_at = 0; // unix epoch
    int64_t created_at = 0;
    int64_t last_login_at = 0;
    int64_t last_sync_at = 0;
    bool is_active = false;

    // Plaintext tokens — only populated on demand via get_tokens(); never stored long-term.
    std::string access_token;
    std::string refresh_token;
};

struct YouTubeSubRow {
    std::string channel_id;
    std::string channel_name;
    std::string channel_url;
    int subscription_order = 0;
};

// Y02: a YouTube search hit (mixed types). Used by Y-mode `/` search.
struct YouTubeSearchRow {
    enum class Kind { VIDEO, CHANNEL, PLAYLIST } kind = Kind::CHANNEL;
    std::string id; // videoId / channelId / playlistId
    std::string title;
    std::string channel_title;
    std::string url;           // watch?v= / channel/ / playlist?list=
    bool music = false;        // true if returned from a music-category search
    std::string thumbnail_url; // Y23: snippet.thumbnails (art_url for display/cache)
};

struct YouTubeHistoryRow {
    std::string video_id;
    std::string title;
    std::string channel_name;
    int duration = 0;
    std::string watched_at;
    std::string source; // "youtube" | "local"
};

class AccountsManager {
public:
    static AccountsManager &instance();

    // ── account CRUD ──
    // Create a new Google account row from the OAuth result. Returns its account_id (>0), or 0 on failure.
    int add_account(const std::string &google_email, const std::string &gaia_id,
                    const std::string &channel_id, const std::string &access_token,
                    const std::string &refresh_token, int64_t expires_at, const std::string &scope,
                    const std::string &label = "");
    // Update tokens for an existing account (e.g. after refresh or re-login).
    bool update_tokens(int account_id, const std::string &access_token,
                       const std::string &refresh_token, int64_t expires_at,
                       const std::string &scope = "");
    // Update profile fields (email/channel/etc. discovered after first token use).
    bool update_profile(int account_id, const std::string &google_email, const std::string &gaia_id,
                        const std::string &channel_id);
    bool delete_account(int account_id);
    bool set_label(int account_id, const std::string &label);

    std::vector<GoogleAccount> list_accounts();
    // Touch last_login_at = now.
    void touch_login(int account_id);
    void touch_sync(int account_id);

    // ── active account ──
    // The active Google account supplies the OAuth token for yt-dlp/mpv and scopes YouTube data.
    // 0 = none active (no login state; podcast/local play unaffected).
    int active_account_id();
    void set_active_account(int account_id); // 0 to clear; updates accounts.is_active + stats.
    bool get_account(int account_id, GoogleAccount &out);
    // Fill out.access_token/refresh_token (decrypted). Refreshes if expired (calls GoogleOAuth).
    // Returns false if no valid token could be obtained.
    bool get_tokens(int account_id, GoogleAccount &out);

    // ── youtube_subscriptions ──
    void replace_subscriptions(int account_id, const std::vector<YouTubeSubRow> &subs);
    std::vector<YouTubeSubRow> load_subscriptions(int account_id);

    // ── youtube_history ──
    void upsert_history(int account_id, const YouTubeHistoryRow &row);
    std::vector<YouTubeHistoryRow> load_history(int account_id, int limit = 200);
    void clear_history(int account_id);

    // ── account_sync_state ──
    void set_sync_state(int account_id, const std::string &sync_type,
                        const std::string &cursor = "");
    int64_t get_sync_last(int account_id, const std::string &sync_type);

    // ── yt-dlp / mpv OAuth injection (task 6) ──
private:
    AccountsManager();
    // Encrypt plaintext token string for db storage.
    std::string seal(const std::string &plaintext);
    bool open_sealed(const std::string &b64, std::string &out);

    int64_t now_epoch();

    std::mutex mtx_;
};

} // namespace panicast
