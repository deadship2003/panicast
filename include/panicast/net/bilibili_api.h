// Y15: Bilibili API client — QR login + cookie auth + following list.
//   No OAuth2 (QR scan → SESSDATA cookie, simpler than Google device flow).
//   No WBI signing needed for nav/followings (search/arc use yt-dlp extraction instead).
//   No nsig/JS runtime (Bilibili has no n-challenge).
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace panicast
{

class BilibiliAPI {
public:
    struct QRCode {
        bool ok = false;
        std::string url;        // QR scan URL (render as terminal QR)
        std::string qrcode_key; // poll key
        std::string error;
    };

    struct LoginResult {
        bool ok = false;
        std::string sessdata;   // SESSDATA cookie (main auth)
        std::string bili_jct;   // CSRF token
        std::string dedeuserid; // Bilibili UID
        std::string error;
        int code = 0; // poll status: 0=success, 86101=waiting, 86090=scanned
    };

    struct NavInfo {
        bool ok = false;
        std::string uid;
        std::string uname;
        std::string face;
        std::string error;
    };

    struct Following {
        std::string mid;   // UP master UID
        std::string uname; // UP master name
        std::string face;  // avatar URL
        std::string sign;  // bio
    };

    struct VideoInfo {
        std::string bvid; // BV ID
        std::string title;
        std::string url;     // https://www.bilibili.com/video/<bvid>
        int duration = 0;    // seconds
        std::string up_name; // UP master name
        std::string pic;     // cover URL
    };

    // Y22: a watch-history entry (Bilibili /x/v2/history). bvid/title/duration at top level.
    struct HistoryItem {
        std::string bvid;
        std::string title;
        std::string url;
        int duration = 0;
        int64_t view_at = 0; // unix seconds of last watch
    };

    // Y23: a user/UP-master search result (search_type=bili_user). upic is the avatar (logo) URL.
    struct UserInfo {
        std::string mid;
        std::string uname;
        std::string sign; // bio
        int fans = 0;
        int videos = 0;   // upload count
        std::string upic; // avatar URL (//i2.hdslb.com/...)
        std::string url;  // https://space.bilibili.com/<mid>/video
    };

    // Step 1: request QR code for login.
    static QRCode request_qrcode();

    // Step 2: poll QR login status. code 86101=waiting, 86090=scanned, 0=success.
    static LoginResult poll_qrcode(const std::string &qrcode_key);

    // Fetch user info (uid, uname) using SESSDATA cookie.
    static NavInfo fetch_nav(const std::string &sessdata);

    // Fetch the user's following list (UP masters they follow). Paginated.
    static std::vector<Following> fetch_followings(const std::string &sessdata,
                                                   const std::string &uid, int page = 1,
                                                   int page_size = 50);

    // Y18: Fetch a UP master's video list with real titles via WBI-signed arc/search API.
    //   Requires SESSDATA for auth. Returns videos with title/bvid/duration.
    static std::vector<VideoInfo> fetch_user_videos(const std::string &sessdata,
                                                    const std::string &mid, int page = 1,
                                                    int page_size = 50);

    // Y21 (issue 2): Native Bilibili video search via WBI-signed search/type API (the correct path,
    //   replacing the yt-dlp `bilisearch:` extractor). SESSDATA may be empty for public search.
    static std::vector<VideoInfo> search_videos(const std::string &sessdata,
                                                const std::string &keyword, int page = 1,
                                                int page_size = 20);

    // Y22: Fetch the logged-in user's watch history (/x/v2/history). Requires SESSDATA. No WBI.
    static std::vector<HistoryItem> fetch_history(const std::string &sessdata, int page = 1,
                                                  int page_size = 50);

    // Y23: Search UP masters / bloggers (search_type=bili_user). SESSDATA may be empty (public).
    //   Returns users with mid/uname/sign/fans/upic(avatar). 'user' is deprecated (-1200); use bili_user.
    static std::vector<UserInfo> search_users(const std::string &sessdata,
                                              const std::string &keyword, int page = 1,
                                              int page_size = 20);

    // Build a Netscape cookies.txt content from SESSDATA + bili_jct + dedeuserid.
    //   Written to <data_dir>/bilibili_cookie.txt for yt-dlp --cookies.
    static std::string build_cookies_txt(const std::string &sessdata, const std::string &bili_jct,
                                         const std::string &dedeuserid);

    // Parse a Netscape cookies.txt file and extract SESSDATA (for cookie import).
    //   Returns empty string if not found.
    static std::string extract_sessdata_from_cookies_txt(const std::string &content);
};

} // namespace panicast
