// DouyinApi — direct Douyin web API client with X-Bogus signature.
//
// Ported from PodRadio-Win_Qt `src/net/douyin_api.{h,cpp}` (Plan B step 4, verified 2026-08-01).
//   X-Bogus is a pure static algorithm (MD5 + RC4 + fixed magic constants) — no JS engine.
//   Reference algorithm: f2/utils/xbogus.py (Apache-2.0, Johnserf-Seed/f2).
//
// Domain dispatch rule (must be honored by callers):
//   tiktok.com   → yt-dlp (overseas TikTok, anonymous)
//   douyin.com   → DouyinApi (domestic Douyin, X-Bogus direct call, never yt-dlp)
//   douyinvod.com→ mpv stream (CDN direct link, needs Referer header)
//
// Auth: relies on a Cookie header + User-Agent. The signing UA must match the request UA
//   exactly (Douyin cross-checks). Cookies come from a Netscape cookies.txt the user imports
//   (Ctrl+B in T mode), or stay empty (Douyin may still answer with a ttwid bootstrap).
//
// Model: synchronous curl (like BilibiliAPI). Callers run these in a worker thread (std::thread).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace panicast
{

class DouyinApi {
public:
    DouyinApi() = default;

    // Set session credentials. userAgent empty → a fixed Chrome UA (matches the browser_* params).
    void setSession(const std::string &cookieHeader, const std::string &userAgent = "");

    // ── Endpoint A: query/user — get the logged-in user's own numeric uid ──
    void fetchMyUserId(std::function<void(const std::string &userId, const std::string &err)> cb);

    // ── Endpoint A2: profile/other — the user's nickname + sec_uid ─────────
    struct MyProfile {
        bool ok = false;
        std::string nickname;
        std::string secUid;
        std::string signature;
        std::string err;
    };
    void fetchMyProfile(const std::string &userId, std::function<void(const MyProfile &)> cb);

    // ── Endpoint B: following/list — one page of followed UP masters ────────
    struct FollowingUser {
        std::string uid;       // numeric uid
        std::string secUid;    // MS4w... stable id (for fetchUserVideos later)
        std::string nickname;  // display name
        std::string signature; // bio
        int followerCount = 0;
        std::string avatarUrl;
    };
    struct FollowingResult {
        bool ok = false;
        std::vector<FollowingUser> users;
        bool hasMore = false;
        int nextOffset = 0;
        int total = 0;
        std::string err;
    };
    void fetchFollowing(const std::string &userId, int offset, int count,
                        std::function<void(const FollowingResult &)> cb);

    // ── Endpoint C: aweme/post — one page of a UP's posted videos ──────────
    // Expanding a subscribed-UP node calls this with the UP's sec_uid. Returns video entries
    // with play_addr CDN direct links (mpv can play directly).
    struct UserVideo {
        std::string awemeId; // stable video id
        std::string desc;    // title/description
        std::string playUrl; // CDN direct link (play_addr.url_list[0])
        int duration = 0;
        std::string coverUrl;
    };
    struct UserVideoResult {
        bool ok = false;
        std::vector<UserVideo> videos;
        bool hasMore = false;
        long long nextCursor = 0; // max_cursor for the next page
        std::string err;
    };

    // META-6: general keyword search (T-mode 🔍). Same result shape as
    //   fetchUserVideos (UserVideo), one page.
    void searchKeyword(const std::string &keyword, int offset, int count,
                       std::function<void(const UserVideoResult &)> cb);
    void fetchUserVideos(const std::string &secUserId, long long maxCursor, int count,
                         std::function<void(const UserVideoResult &)> cb);

    // ── Endpoint D: aweme/favorite — a UP's liked videos (❤️ likes tab) ────
    void fetchUserLikes(const std::string &secUserId, long long maxCursor, int count,
                        std::function<void(const UserVideoResult &)> cb);

    // ── Endpoint F: mix/aweme — a UP's collection videos (📚 collections tab) ─
    // Paginated by `cursor` (NOT max_cursor). Requires a mix_id (see fetchFirstMixId).
    void fetchUserMix(const std::string &mixId, long long cursor, int count,
                      std::function<void(const UserVideoResult &)> cb);

    // ── Collection helper: first mix_id among a UP's videos w/ mix_info ────
    void fetchFirstMixId(const std::string &secUserId,
                         std::function<void(const std::string &mixId, const std::string &err)> cb);

    // ── QR login (terminal QR code, pure API) ────────────────────────────────
    // Douyin login endpoints (sso.douyin.com) skip the X-Bogus signature; they are
    //   hit directly via curl's cookie jar; login state is judged solely by a
    //   non-empty `sessionid`. The jar persists cookies to douyin_cookie.txt.
    struct LoginQR {
        bool ok = false;
        std::string qr_content; // QR payload (scannable URL); may be a base64 image (unrenderable)
        std::string token;      // token for polling check_qrconnect
        std::string err;
    };
    struct LoginResult {
        bool ok = false;       // login succeeded (sessionid in the cookie jar)
        std::string sessionid; // login credential (non-empty = logged in)
        int code = 0;          // poll status: 1=pending 2=scanned 3=success
        std::string err;
    };
    LoginQR request_qrcode();
    LoginResult poll_qrcode(const std::string &token);

    // ── Cookie helper: build a "n1=v1; n2=v2; ..." header from a Netscape cookies.txt,
    //   keeping only cookies whose domain matches `domain` (e.g. "douyin.com"). Handles
    //   curl's "#HttpOnly_" domain prefix (sessionid/sessionid_ss are HttpOnly). ──
    static std::string build_cookie_header_from_txt(const std::string &cookies_txt,
                                                    const std::string &domain);

private:
    std::string computeXBogus(const std::string &urlParams) const;
    static std::vector<unsigned char> rc4(const std::vector<unsigned char> &key,
                                          const std::vector<unsigned char> &data);
    static std::vector<unsigned char> md5Raw(const std::string &data); // → 16 bytes
    static std::vector<int> md5StrToArray(const std::string &s);
    static std::string md5Hex(const std::string &s);
    static std::string md5HexOfArray(const std::vector<int> &data);
    std::string signedGet(const std::string &endpoint, const std::string &params,
                          std::string &err) const;

    std::string cookie_;
    std::string ua_;
};

} // namespace panicast
