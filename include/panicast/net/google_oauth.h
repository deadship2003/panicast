// Y01: Google OAuth 2.0 Device Authorization Grant (RFC 8628) + YouTube Data API v3 + InnerTube.
//   "SmartTube-style" login: a user-owned OAuth 2.0 "Desktop app" client (device authorization
//   grant, RFC 8628), the verification_url is rendered as a terminal QR (ui/qr.cpp) so the user
//   scans it with a phone.
//   All HTTP goes through Network (curl). Tokens are NOT stored here — the caller (AccountsManager)
//   persists them encrypted.
//
//   API reality (documented in CHANGELOG Y01 "Known limits"):
//   - subscriptions list/insert/delete: official Data API v3. ✅ bidirectional.
//   - watch history PULL: YouTube Data API v3 does NOT expose watch history → use authenticated
//     InnerTube /browse (the same feed youtube.com/feed/history uses). Unofficial but works.
//   - watch history PUSH / resume-position writeback: YouTube has no public API for this → not done
//     (local plays are recorded locally in youtube_history instead).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "panicast/storage/accounts.h"      // YouTubeSubRow / YouTubeHistoryRow
#include "panicast/storage/youtube_cache.h" // YouTubeVideoInfo (fetch_channel_videos)

namespace panicast
{

// OAuth client credentials are loaded at RUNTIME from the user's client_secret JSON (a Google
// "Desktop app" client that supports the device authorization grant, RFC 8628), expected in the
// data dir (~/.local/share/panicast/client_secret*.json). If the file is absent, the app falls
// back to the project's BUILT-IN Desktop-app client (see BUILTIN_CLIENT_ID/SECRET in google_oauth.cpp)
// so Y-mode login works out-of-the-box. The previously-used public SmartTube TV client was blocked
// by Google (invalid_client); a runtime client_secret*.json still takes precedence if present.
//   For a Desktop-app client the secret is not truly confidential (Google's installed-app model
//   ships it in the binary regardless), so embedding it is the standard pattern.
//   Verification URL is rendered as a terminal QR (ui/qr.cpp) for phone scanning.

class GoogleOAuth {
public:
    // Client credentials, loaded at runtime from <data_dir>/client_secret*.json (Desktop-app
    // client). Falls back to the built-in Desktop-app client when no file is present.
    static std::string client_id();
    static std::string client_secret();

    struct DeviceCode {
        std::string device_code;
        std::string user_code;
        std::string verification_url; // e.g. https://www.google.com/device
        int expires_in = 0;           // seconds
        int interval = 5;             // polling interval seconds
        bool ok = false;
        std::string error;
    };

    struct TokenResult {
        bool ok = false;
        std::string access_token;
        std::string refresh_token;
        int expires_in = 0; // seconds (access token lifetime)
        std::string scope;
        // When ok==false: "authorization_pending" means keep polling; "expired_token"/"slow_down"/others stop.
        std::string error;
        std::string error_desc;
        int64_t obtained_at = 0; // unix epoch when obtained (caller computes expires_at)
    };

    struct ChannelIdentity {
        bool ok = false;
        std::string channel_id;
        std::string title;
        std::string email; // may be empty (email scope not always returned)
    };

    // Step 1: request device code. Returns verification_url + user_code to render as QR.
    static DeviceCode request_device_code();

    // Step 2: poll the token endpoint ONCE. Caller loops with `interval` until ok or expired.
    //   ok=true → tokens obtained. error=="authorization_pending" → keep polling.
    static TokenResult poll_token(const std::string &device_code);

    // Refresh an expired access_token using a refresh_token.
    static TokenResult refresh(const std::string &refresh_token);

    // Fetch the authenticated user's YouTube channel identity (channels?mine=true&part=snippet).
    static ChannelIdentity fetch_identity(const std::string &access_token);

    // ── subscriptions (official Data API v3) ──
    // List the authenticated user's subscriptions (paginated). Returns channel rows.
    static std::vector<YouTubeSubRow> fetch_subscriptions(const std::string &access_token);
    // Subscribe to a channel. channel_id is UC... id.
    static bool subscribe(const std::string &access_token, const std::string &channel_id);
    // Unsubscribe: looks up the subscription id for the channel, then deletes it.
    static bool unsubscribe(const std::string &access_token, const std::string &channel_id);

    // ── watch history (InnerTube /browse, unofficial) ──
    // Returns recent watch-history videos. May return empty if the account has history disabled.
    static std::vector<YouTubeHistoryRow> fetch_watch_history(const std::string &access_token);

    // ── channel uploads (official Data API v3) ── Y04
    // Fetch a channel's recent uploads via Data API v3 (channels.list?part=contentDetails → uploads
    //   playlist → playlistItems.list), using the OAuth access token. This bypasses yt-dlp entirely,
    //   so the Y-mode episode list works WITHOUT cookies / proxy-for-youtube / a JS runtime — only the
    //   OAuth token (which already powers subscriptions) is needed. Quota: 1 (channels.list) + 1/page.
    //   Returns up to ~200 most recent uploads (YouTube caps the uploads playlist). Empty on error.
    //   channel_id is the UC... id (or the URL is parsed by the caller).
    static std::vector<YouTubeVideoInfo> fetch_channel_videos(const std::string &access_token,
                                                              const std::string &channel_id);

    // ── search (official Data API v3) ── Y02
    // Search YouTube. type_filter: "" = mixed (video/channel/playlist), or "video"/"channel"/"playlist".
    // music_only = true → video results filtered to the Music category (videoCategoryId=10).
    // Quota: 100 units/call (default daily quota 10k → ~100 searches).
    static std::vector<YouTubeSearchRow> search(const std::string &access_token,
                                                const std::string &query,
                                                const std::string &type_filter = "",
                                                bool music_only = false);
};

} // namespace panicast
