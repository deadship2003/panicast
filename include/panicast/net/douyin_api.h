// DouyinApi — direct Douyin web API client with X-Bogus signature.
//
// Ported from PodRadio-Win_Qt `src/net/douyin_api.{h,cpp}` (方案B 步骤4, verified 2026-08-01).
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

    // ── 接口A: query/user — get the logged-in user's own numeric uid ───────
    void fetchMyUserId(std::function<void(const std::string &userId, const std::string &err)> cb);

    // ── 接口A2: profile/other — get the logged-in user's nickname + sec_uid ─
    struct MyProfile {
        bool ok = false;
        std::string nickname;
        std::string secUid;
        std::string signature;
        std::string err;
    };
    void fetchMyProfile(const std::string &userId, std::function<void(const MyProfile &)> cb);

    // ── 接口B: following/list — one page of followed UP masters ────────────
    struct FollowingUser {
        std::string uid;       // numeric uid
        std::string secUid;    // MS4w... stable id (for fetchUserVideos later)
        std::string nickname;  // display name (昵称)
        std::string signature; // bio (简介)
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

    // ── 接口C: aweme/post — one page of a UP's posted videos ────────────────
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

    // ── 接口D: aweme/favorite — a UP's liked videos (❤️ 喜欢 TAB) ──────────
    void fetchUserLikes(const std::string &secUserId, long long maxCursor, int count,
                        std::function<void(const UserVideoResult &)> cb);

    // ── 接口F: mix/aweme — videos of a UP's collection/合集 (📚 合集 TAB) ────
    // Paginated by `cursor` (NOT max_cursor). Requires a mix_id (see fetchFirstMixId).
    void fetchUserMix(const std::string &mixId, long long cursor, int count,
                      std::function<void(const UserVideoResult &)> cb);

    // ── 合集辅助: 取某 UP 首个含 mix_info 的视频的 mix_id ─────────────────────
    void fetchFirstMixId(const std::string &secUserId,
                         std::function<void(const std::string &mixId, const std::string &err)> cb);

    // ── 扫码登录（终端二维码，纯 API）───────────────────────────────────────────
    // Douyin 登录端点（sso.douyin.com）不走 X-Bogus 签名；用 curl 的 cookie jar 直连，
    //   登录态唯一判据是 `sessionid`（非空）。cookie 由 jar 写入 douyin_cookie.txt。
    // ⚠️ 端点/字段为公开资料拼的猜测，需连真机 F12 校正（见 douyin_api.cpp 内注释）。
    struct LoginQR {
        bool ok = false;
        std::string qr_content; // 二维码要编码的内容（可扫 URL）；可能是 base64 图片（不可渲染）
        std::string token;      // 轮询 check_qrconnect 的 token
        std::string err;
    };
    struct LoginResult {
        bool ok = false;       // 登录成功（sessionid 已写入 cookie jar）
        std::string sessionid; // 登录态凭据（非空 = 已登录）
        int code = 0;          // 轮询状态：1=未扫 2=已扫待确认 3=成功
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
