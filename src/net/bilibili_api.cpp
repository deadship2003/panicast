// Y15: Bilibili API client implementation.
//   QR login: generate → poll → SESSDATA cookie (in JSON body cookie_info).
//   Nav/followings: GET with Cookie: SESSDATA=... header.
//   No WBI signing (search/arc use yt-dlp extraction instead).
#include "panicast/net/bilibili_api.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <strings.h> // strncasecmp (P2-S4)
#include <openssl/evp.h>
// Suppress OpenSSL 3.0 MD5 deprecation warning (still works, no alternative for WBI)

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "panicast/config/ini_config.h"
#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/net/network.h"
#include "panicast/core/utils.h"

namespace panicast
{
using json = nlohmann::json;

// URL-encode a string (for API query params).
// Y24.29: url_encode removed — use Utils::url_encode (was duplicated).

BilibiliAPI::QRCode BilibiliAPI::request_qrcode() {
    QRCode qr;
    std::string resp =
        Network::fetch("https://passport.bilibili.com/x/passport-login/web/qrcode/generate");
    if (resp.empty()) {
        qr.error = "network";
        return qr;
    }
    try {
        json j = json::parse(resp);
        if (j.value("code", -1) != 0) {
            qr.error = j.value("message", "api error");
            return qr;
        }
        qr.url = j["data"].value("url", "");
        qr.qrcode_key = j["data"].value("qrcode_key", "");
        qr.ok = !qr.qrcode_key.empty();
    } catch (const std::exception &e) {
        qr.error = std::string("parse: ") + e.what();
    }
    return qr;
}

BilibiliAPI::LoginResult BilibiliAPI::poll_qrcode(const std::string &qrcode_key) {
    LoginResult r;
    std::string url = "https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key=" +
                      Utils::url_encode(qrcode_key);

    // Bilibili returns SESSDATA via Set-Cookie HTTP response headers (NOT in the JSON body).
    // Network::fetch() only returns the body, so we use curl directly with a header callback
    // to capture Set-Cookie lines.
    CurlRAII curl_raii;
    CURL *curl = curl_raii.handle;
    if (!curl) {
        r.error = "curl init";
        return r;
    }

    std::string body;
    std::string headers;
    // Header callback: append all response headers (for Set-Cookie parsing).
    auto header_cb = +[](char *buffer, size_t size, size_t nitems, void *userdata) -> size_t {
        static_cast<std::string *>(userdata)->append(buffer, size * nitems);
        return size * nitems;
    };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    auto write_cb = +[](void *ptr, size_t size, size_t nmemb, void *data) -> size_t {
        static_cast<std::string *>(data)->append((char *)ptr, size * nmemb);
        return size * nmemb;
    };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) panicast");
    apply_network_proxy(curl, url, "bilibili");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    bool tls = IniConfig::instance().get_tls_verify();
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, tls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, tls ? 2L : 0L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        r.error = "network";
        return r;
    }
    if (body.empty()) {
        r.error = "empty response";
        return r;
    }

    try {
        json j = json::parse(body);
        if (j.value("code", -1) != 0) {
            r.error = j.value("message", "api error");
            return r;
        }
        int data_code = j["data"].value("code", -1);
        r.code = data_code;
        if (data_code == 86101 || data_code == 86090)
            return r; // waiting / scanned
        if (data_code != 0) {
            r.error = "login failed";
            return r;
        }

        // Success (code=0) — parse Set-Cookie headers for SESSDATA, bili_jct, DedeUserID.
        //   These come as HTTP response headers like:
        //   Set-Cookie: SESSDATA=abc123; Path=/; Domain=.bilibili.com; ...
        std::istringstream hs(headers);
        std::string hline;
        while (std::getline(hs, hline)) {
            // P2-S4: truly case-insensitive "Set-Cookie:" prefix match (handles "Set-cookie:", etc.).
            //   Header name is 11 chars ("Set-Cookie:"); anything after is the cookie string.
            if (hline.size() > 11 && strncasecmp(hline.c_str(), "Set-Cookie:", 11) == 0) {
                std::string cookie_str = hline.substr(11);
                while (!cookie_str.empty() && cookie_str.front() == ' ')
                    cookie_str.erase(0, 1);
                // cookie_str = "SESSDATA=xxx; Path=/; Domain=..."
                size_t semi = cookie_str.find(';');
                std::string kv =
                    (semi != std::string::npos) ? cookie_str.substr(0, semi) : cookie_str;
                size_t eq = kv.find('=');
                if (eq != std::string::npos) {
                    std::string name = kv.substr(0, eq);
                    std::string val = kv.substr(eq + 1);
                    // Trim whitespace
                    while (!name.empty() && name.back() == ' ')
                        name.pop_back();
                    while (!val.empty() && val.front() == ' ')
                        val.erase(0, 1);
                    if (name == "SESSDATA")
                        r.sessdata = val;
                    else if (name == "bili_jct")
                        r.bili_jct = val;
                    else if (name == "DedeUserID")
                        r.dedeuserid = val;
                }
            }
        }

        // Also try body's cookie_info (some API versions return it in-body).
        if (r.sessdata.empty() && j["data"].contains("cookie_info") &&
            j["data"]["cookie_info"].contains("cookie")) {
            const auto &ck = j["data"]["cookie_info"]["cookie"];
            r.sessdata = ck.value("SESSDATA", "");
            r.bili_jct = ck.value("bili_jct", "");
            r.dedeuserid = ck.value("DedeUserID", "");
        }

        if (r.sessdata.empty()) {
            r.error = "no SESSDATA in response";
            LOG("[Bilibili] poll_qrcode: code=0 but no SESSDATA in headers or body");
            // P2-S3: redact cookie values before logging headers (SESSDATA/etc. must not reach the log).
            std::string redacted = headers;
            for (const char *secret : {"SESSDATA=", "bili_jct=", "DedeUserID="}) {
                size_t p = 0;
                while ((p = redacted.find(secret, p)) != std::string::npos) {
                    size_t vstart = p + std::strlen(secret);
                    size_t vend = redacted.find(';', vstart);
                    if (vend == std::string::npos)
                        vend = redacted.find('\n', vstart);
                    if (vend == std::string::npos)
                        vend = redacted.size();
                    redacted.replace(vstart, vend - vstart, "***");
                    p = vstart + 3;
                }
            }
            LOG(fmt::format("[Bilibili] response headers (redacted): {}", redacted));
            return r;
        }
        r.ok = true;
        LOG(fmt::format("[Bilibili] login OK: uid={}, sessdata={}...", r.dedeuserid,
                        r.sessdata.substr(0, 10)));
    } catch (const std::exception &e) {
        r.error = std::string("parse: ") + e.what();
    }
    return r;
}

// Y24.49: forward decl — fetch_nav (below) uses build_bilibili_cookie (defined further down, after
//   the fingerprint helper it depends on).
static std::string build_bilibili_cookie(const std::string &sessdata);

BilibiliAPI::NavInfo BilibiliAPI::fetch_nav(const std::string &sessdata) {
    NavInfo info;
    // Y24.49: use the full bilibili cookie (SESSDATA + buvid3 + b_nut) — raw SESSDATA alone is
    //   rejected by Bilibili risk control (nav returns -101 "not logged in"), which made the QR-login
    //   flow fall back to "Bili #<uid>" (UID instead of username). buvid3 comes from the spi endpoint.
    std::string cookie = build_bilibili_cookie(sessdata); // defined below (forward-declared)
    std::string resp = Network::fetch_cookie("https://api.bilibili.com/x/web-interface/nav", cookie,
                                             "https://www.bilibili.com");
    if (resp.empty()) {
        info.error = "network";
        EVENT_LOG("B: nav — network empty");
        return info;
    }
    try {
        json j = json::parse(resp);
        if (j.value("code", -1) != 0) {
            info.error = j.value("message", "nav failed");
            EVENT_LOG(fmt::format("B: nav failed — {}", info.error));
            return info;
        }
        info.uid = j["data"].value("mid", "");
        info.uname = j["data"].value("uname", "");
        info.face = j["data"].value("face", "");
        info.ok = !info.uid.empty();
    } catch (const std::exception &e) {
        info.error = std::string("parse: ") + e.what();
    }
    return info;
}

// ── Y18: WBI signing (Bilibili risk-control signature) ──────────────────────
// Required for /x/space/wbi/arc/search and /x/relation/followings (since 2024).
// Algorithm: fetch wbi_img from nav → extract img_key+sub_key → compute mixin_key
//   via fixed permutation table → sign params with MD5(query + mixin_key).
static const int WBI_MIXIN_TAB[] = {46, 47, 18, 2,  53, 8,  23, 32, 15, 50, 10, 31, 58, 3,  45, 35,
                                    27, 43, 5,  49, 33, 9,  42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
                                    37, 48, 7,  16, 24, 55, 40, 61, 26, 17, 0,  20, 11, 21, 4,  25,
                                    54, 6,  34, 51, 1,  36, 44, 30, 52, 22, 59, 57, 60, 56, 63};

static std::string md5_hex(const std::string &input) {
    // Y18: use EVP API (OpenSSL 3.0+ non-deprecated) instead of MD5().
    // Y21 (P2-C15): check every EVP return — on failure return "" so the caller treats the
    //   signature as invalid rather than computing w_rid from garbage.
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        return "";
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    bool ok = EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, input.c_str(), input.size()) == 1 &&
              EVP_DigestFinal_ex(ctx, digest, &len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok)
        return "";
    char hex[33];
    for (unsigned int i = 0; i < len && i < 16; i++)
        sprintf(hex + i * 2, "%02x", digest[i]);
    hex[32] = 0;
    return std::string(hex);
}

// Y21 (issue 1): fetch the public fingerprint (buvid3/buvid4) from the spi endpoint and cache it.
//   Bilibili risk control (-352) increasingly requires buvid3/b_nut alongside SESSDATA for WBI
//   endpoints. This is the correct single path (no yt-dlp fallback). Cached for the process lifetime.
struct BilibiliFingerprint {
    std::string buvid3, buvid4;
};
static BilibiliFingerprint &bilibili_fingerprint() {
    static BilibiliFingerprint fp;
    static std::once_flag flag;
    std::call_once(flag, []() {
        std::string resp = Network::fetch("https://api.bilibili.com/x/frontend/finger/spi");
        if (resp.empty()) {
            LOG("[Bilibili] spi: empty response (buvid3 unavailable)");
            return;
        }
        try {
            json j = json::parse(resp);
            if (j.value("code", -1) == 0 && j.contains("data")) {
                fp.buvid3 = j["data"].value("b_3", "");
                fp.buvid4 = j["data"].value("b_4", "");
            } else {
                LOG(fmt::format("[Bilibili] spi: code={}", j.value("code", -999)));
            }
        } catch (...) {
            LOG("[Bilibili] spi: parse error");
        }
    });
    return fp;
}

// Y21 (issue 1): build the full Cookie header bilibili WBI endpoints expect. SESSDATA may be empty
//   (public calls like search/arc still work with buvid3+b_nut). b_nut is a unix-seconds timestamp
//   cookie; one value is generated per process and reused.
static std::string build_bilibili_cookie(const std::string &sessdata) {
    std::string c;
    if (!sessdata.empty())
        c += "SESSDATA=" + sessdata;
    const auto &fp = bilibili_fingerprint();
    if (!fp.buvid3.empty()) {
        if (!c.empty())
            c += "; ";
        c += "buvid3=" + fp.buvid3;
    }
    if (!fp.buvid4.empty()) {
        if (!c.empty())
            c += "; ";
        c += "buvid4=" + fp.buvid4;
    }
    static std::string b_nut;
    static int64_t b_nut_ts = 0;
    int64_t now_ts = std::time(nullptr);
    if (b_nut.empty() || now_ts - b_nut_ts > 86400) {
        b_nut = std::to_string(now_ts);
        b_nut_ts = now_ts;
    }
    if (!c.empty())
        c += "; ";
    c += "b_nut=" + b_nut;
    return c;
}

// Fetch wbi keys from nav API, compute mixin_key. Returns empty string on failure.
// Y21 (P3-D5): cache the mixin_key for 5 minutes (it changes at most daily) to avoid a nav API
//   round-trip on every WBI call. Y18: pass SESSDATA cookie (nav with cookie is more reliable).
static std::mutex g_wbi_mtx;
static std::string g_mixin_key_cache;
static int64_t g_mixin_key_ts = 0;
static std::string get_wbi_mixin_key(const std::string &sessdata = "") {
    {
        std::lock_guard<std::mutex> lk(g_wbi_mtx);
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
        if (!g_mixin_key_cache.empty() && (now - g_mixin_key_ts) < 300)
            return g_mixin_key_cache;
    }
    std::string cookie = build_bilibili_cookie(sessdata);
    std::string resp = cookie.empty()
                           ? Network::fetch("https://api.bilibili.com/x/web-interface/nav")
                           : Network::fetch_cookie("https://api.bilibili.com/x/web-interface/nav",
                                                   cookie, "https://www.bilibili.com");
    if (resp.empty()) {
        LOG("[Bilibili] WBI: nav API empty response");
        return "";
    }
    std::string mixin;
    try {
        json j = json::parse(resp);
        if (!j.contains("data") || !j["data"].contains("wbi_img")) {
            LOG(fmt::format("[Bilibili] WBI: nav API no wbi_img (code={})", j.value("code", -999)));
            return "";
        }
        std::string img_url = j["data"]["wbi_img"].value("img_url", "");
        std::string sub_url = j["data"]["wbi_img"].value("sub_url", "");
        auto extract_key = [](const std::string &url) -> std::string {
            size_t slash = url.rfind('/');
            size_t dot = url.rfind('.');
            if (slash == std::string::npos)
                return "";
            size_t end = (dot != std::string::npos && dot > slash) ? dot : url.size();
            return url.substr(slash + 1, end - slash - 1);
        };
        std::string img_key = extract_key(img_url);
        std::string sub_key = extract_key(sub_url);
        std::string raw = img_key + sub_key;
        if (raw.size() < 64) {
            LOG("[Bilibili] WBI: raw key too short");
            return "";
        }
        for (int i = 0; i < 32; i++)
            mixin += raw[WBI_MIXIN_TAB[i]];
    } catch (...) {
        LOG("[Bilibili] WBI: nav API parse error");
        return "";
    }
    if (mixin.empty())
        return "";
    std::lock_guard<std::mutex> lk(g_wbi_mtx);
    g_mixin_key_cache = mixin;
    g_mixin_key_ts = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return mixin;
}

// WBI-sign a set of params and return the full signed URL.
// Y18: pass sessdata to get_wbi_mixin_key for reliable nav API access.
static std::string wbi_sign_url(const std::string &base_url,
                                std::vector<std::pair<std::string, std::string>> params,
                                const std::string &sessdata = "") {
    std::string mixin_key = get_wbi_mixin_key(sessdata);
    if (mixin_key.empty()) {
        // Fallback: no WBI (will likely fail with -352, but try)
        std::string url = base_url + "?";
        for (size_t i = 0; i < params.size(); i++) {
            if (i)
                url += "&";
            url += params[i].first + "=" + Utils::url_encode(params[i].second);
        }
        return url;
    }
    // Add wts (timestamp)
    auto now = std::chrono::system_clock::now();
    int64_t wts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    params.push_back({"wts", std::to_string(wts)});
    // Sort params alphabetically by key
    std::sort(params.begin(), params.end());
    // Build query string (filtered: remove !'()* )
    std::string query;
    for (size_t i = 0; i < params.size(); i++) {
        if (i)
            query += "&";
        std::string val = params[i].second;
        // Filter special chars
        val.erase(std::remove_if(val.begin(), val.end(),
                                 [](char c) {
                                     return c == '!' || c == '\'' || c == '(' || c == ')' ||
                                            c == '*';
                                 }),
                  val.end());
        query += params[i].first + "=" + val;
    }
    // Compute w_rid = MD5(query + mixin_key)
    std::string w_rid = md5_hex(query + mixin_key);
    // Build final URL
    std::string url = base_url + "?" + query + "&w_rid=" + w_rid;
    return url;
}

std::vector<BilibiliAPI::Following> BilibiliAPI::fetch_followings(const std::string &sessdata,
                                                                  const std::string &uid, int page,
                                                                  int page_size) {
    std::vector<Following> out;
    std::string cookie = build_bilibili_cookie(sessdata);
    // Y18: followings API also needs WBI signing now. Pass sessdata for reliable nav.
    std::string url = wbi_sign_url("https://api.bilibili.com/x/relation/followings",
                                   {{"vmid", uid},
                                    {"pn", std::to_string(page)},
                                    {"ps", std::to_string(page_size)},
                                    {"order", "desc"}},
                                   sessdata);
    std::string resp =
        Network::fetch_cookie(url, cookie, "https://space.bilibili.com/" + uid + "/fans/follow");
    if (resp.empty()) {
        LOG("[Bilibili] followings: empty response");
        EVENT_LOG("B: followings — network empty");
        return out;
    }
    try {
        json j = json::parse(resp);
        if (j.value("code", -1) != 0) {
            LOG(fmt::format("[Bilibili] followings error: code={} msg={}", j.value("code", 0),
                            j.value("message", "?")));
            EVENT_LOG(fmt::format("B: followings error — {}", j.value("message", "?")));
            return out;
        }
        // Y21: the followings API returns the list directly as data.list (an ARRAY of following
        //   objects), NOT data.list.followings (the old code looked there → always 0 followings).
        //   Handle both shapes defensively.
        if (j["data"].contains("list")) {
            const auto &list = j["data"]["list"];
            auto extract_following = [&](const json &f) {
                Following fg;
                fg.mid = std::to_string(f.value("mid", 0));
                fg.uname = f.value("uname", "");
                fg.face = f.value("face", "");
                fg.sign = f.value("sign", "");
                out.push_back(fg);
            };
            if (list.is_array()) {
                for (const auto &f : list)
                    extract_following(f);
            } else if (list.is_object() && list.contains("followings") &&
                       list["followings"].is_array()) {
                for (const auto &f : list["followings"])
                    extract_following(f);
            }
        }
    } catch (const std::exception &e) {
        LOG(fmt::format("[Bilibili] followings parse error: {}", e.what()));
    }
    return out;
}

std::vector<BilibiliAPI::VideoInfo> BilibiliAPI::fetch_user_videos(const std::string &sessdata,
                                                                   const std::string &mid, int page,
                                                                   int page_size) {
    std::vector<VideoInfo> out;
    std::string cookie = build_bilibili_cookie(sessdata);
    // Y18: WBI-signed arc/search API — returns real video titles. Pass sessdata for nav.
    std::string url = wbi_sign_url("https://api.bilibili.com/x/space/wbi/arc/search",
                                   {{"mid", mid},
                                    {"pn", std::to_string(page)},
                                    {"ps", std::to_string(page_size)},
                                    {"order", "pubdate"}},
                                   sessdata);
    std::string resp =
        Network::fetch_cookie(url, cookie, "https://space.bilibili.com/" + mid + "/video");
    if (resp.empty()) {
        LOG("[Bilibili] user_videos: empty response");
        EVENT_LOG("B: user videos — network empty");
        return out;
    }
    try {
        json j = json::parse(resp);
        if (j.value("code", -1) != 0) {
            LOG(fmt::format("[Bilibili] user_videos error: code={} msg={}", j.value("code", 0),
                            j.value("message", "?")));
            return out;
        }
        if (j["data"].contains("list") && j["data"]["list"].contains("vlist")) {
            for (const auto &v : j["data"]["list"]["vlist"]) {
                VideoInfo vi;
                vi.bvid = v.value("bvid", "");
                vi.title = v.value("title", "Untitled");
                vi.url = vi.bvid.empty()
                             ? ""
                             : fmt::format("https://www.bilibili.com/video/{}", vi.bvid);
                // duration comes as "14:05" or "1:23:45" string — parse to seconds (multi-segment)
                std::string dur_str = v.value("length", "0");
                int dur = 0;
                if (dur_str.find(':') != std::string::npos) {
                    int total = 0, part = 0;
                    for (char c : dur_str) {
                        if (c == ':') {
                            total = total * 60 + part;
                            part = 0;
                        } else if (c >= '0' && c <= '9')
                            part = part * 10 + (c - '0');
                    }
                    dur = total * 60 + part;
                } else {
                    dur = std::atoi(dur_str.c_str());
                }
                vi.duration = dur;
                vi.up_name = v.value("author", "");
                vi.pic = v.value("pic", "");
                out.push_back(vi);
            }
        }
        LOG(fmt::format("[Bilibili] user_videos: {} videos for mid={}", out.size(), mid));
    } catch (const std::exception &e) {
        LOG(fmt::format("[Bilibili] user_videos parse error: {}", e.what()));
    }
    return out;
}

// Y21 (issue 2): native Bilibili video search via the WBI-signed search/type API — the correct
//   single path, replacing the yt-dlp `bilisearch:` extractor (which is fragile/slow and blocked
//   by the same risk control). SESSDATA may be empty (public search works with buvid3+b_nut+WBI).
std::vector<BilibiliAPI::VideoInfo> BilibiliAPI::search_videos(const std::string &sessdata,
                                                               const std::string &keyword, int page,
                                                               int page_size) {
    std::vector<VideoInfo> out;
    if (keyword.empty())
        return out;
    std::string cookie = build_bilibili_cookie(sessdata);
    std::string url = wbi_sign_url("https://api.bilibili.com/x/web-interface/wbi/search/type",
                                   {{"search_type", "video"},
                                    {"keyword", keyword},
                                    {"page", std::to_string(page)},
                                    {"page_size", std::to_string(page_size)},
                                    {"order", "pubdate"}},
                                   sessdata);
    std::string resp = Network::fetch_cookie(url, cookie, "https://www.bilibili.com");
    if (resp.empty()) {
        LOG("[Bilibili] search: empty response");
        EVENT_LOG("B: search — network empty");
        return out;
    }
    try {
        json j = json::parse(resp);
        if (j.value("code", -1) != 0) {
            LOG(fmt::format("[Bilibili] search error: code={} msg={}", j.value("code", 0),
                            j.value("message", "?")));
            EVENT_LOG(fmt::format("B: search error — {}", j.value("message", "?")));
            return out;
        }
        if (j["data"].contains("result") && j["data"]["result"].is_array()) {
            for (const auto &v : j["data"]["result"]) {
                VideoInfo vi;
                vi.bvid = v.value("bvid", "");
                vi.url = vi.bvid.empty()
                             ? ""
                             : fmt::format("https://www.bilibili.com/video/{}", vi.bvid);
                // search/type titles contain <em>…</em> highlight markers — strip them.
                std::string title = v.value("title", "Untitled");
                for (size_t p = title.find('<'); p != std::string::npos; p = title.find('<', p)) {
                    size_t gt = title.find('>', p);
                    if (gt == std::string::npos)
                        break;
                    title.erase(p, gt - p + 1);
                }
                vi.title = title.empty() ? "Untitled" : title;
                // Y21: search/type returns duration as a STRING "MM:SS" (or "H:MM:SS"), not int.
                //   v.value("duration", 0) throws type_error on a string → killed the whole loop →
                //   0 results. Parse both shapes.
                vi.duration = 0;
                if (v.contains("duration")) {
                    const auto &d = v["duration"];
                    if (d.is_number_integer())
                        vi.duration = d.get<int>();
                    else if (d.is_string()) {
                        std::string ds = d.get<std::string>();
                        int total = 0, part = 0;
                        for (char c : ds) {
                            if (c == ':') {
                                total = total * 60 + part;
                                part = 0;
                            } else if (c >= '0' && c <= '9')
                                part = part * 10 + (c - '0');
                        }
                        vi.duration = total * 60 + part;
                    }
                }
                vi.up_name = v.value("author", "");
                vi.pic = v.value("pic", "");
                if (!vi.pic.empty() && vi.pic.find("://") == std::string::npos)
                    vi.pic = "https:" + vi.pic; // protocol-relative "//i0.hdslb.com/..."
                if (!vi.bvid.empty())
                    out.push_back(vi);
            }
        }
        LOG(fmt::format("[Bilibili] search: {} results for '{}'", out.size(), keyword));
    } catch (const std::exception &e) {
        LOG(fmt::format("[Bilibili] search parse error: {}", e.what()));
    }
    return out;
}

// Y22: watch history via /x/v2/history (requires SESSDATA, no WBI signing). Each item has top-level
//   bvid/title/duration/view_at. Returns most-recent-first (the API order).
std::vector<BilibiliAPI::HistoryItem> BilibiliAPI::fetch_history(const std::string &sessdata,
                                                                 int page, int page_size) {
    std::vector<HistoryItem> out;
    if (sessdata.empty()) {
        EVENT_LOG("B: history requires login (no SESSDATA)");
        return out;
    }
    std::string cookie = build_bilibili_cookie(sessdata);
    std::string url = "https://api.bilibili.com/x/v2/history?pn=" + std::to_string(page) +
                      "&ps=" + std::to_string(page_size);
    std::string resp = Network::fetch_cookie(url, cookie, "https://www.bilibili.com");
    if (resp.empty()) {
        LOG("[Bilibili] history: empty response");
        return out;
    }
    try {
        json j = json::parse(resp);
        if (j.value("code", -1) != 0) {
            LOG(fmt::format("[Bilibili] history error: code={} msg={}", j.value("code", 0),
                            j.value("message", "?")));
            EVENT_LOG(fmt::format("B: history error — {}", j.value("message", "?")));
            return out;
        }
        if (j["data"].is_array()) {
            for (const auto &v : j["data"]) {
                HistoryItem h;
                h.bvid = v.value("bvid", "");
                if (h.bvid.empty())
                    continue; // skip non-video history entries (articles/live)
                h.url = fmt::format("https://www.bilibili.com/video/{}", h.bvid);
                h.title = v.value("title", "Untitled");
                h.duration = v.value("duration", 0);
                h.view_at = v.value("view_at", (int64_t)0);
                out.push_back(h);
            }
        }
        LOG(fmt::format("[Bilibili] history: {} items", out.size()));
    } catch (const std::exception &e) {
        LOG(fmt::format("[Bilibili] history parse error: {}", e.what()));
    }
    return out;
}

// Y23: search UP masters / bloggers via search_type=bili_user (WBI-signed, same path as video
//   search). Returns users with avatar (upic) for logo display/storage. SESSDATA may be empty.
std::vector<BilibiliAPI::UserInfo> BilibiliAPI::search_users(const std::string &sessdata,
                                                             const std::string &keyword, int page,
                                                             int page_size) {
    std::vector<UserInfo> out;
    if (keyword.empty())
        return out;
    std::string cookie = build_bilibili_cookie(sessdata);
    std::string url = wbi_sign_url("https://api.bilibili.com/x/web-interface/wbi/search/type",
                                   {{"search_type", "bili_user"},
                                    {"keyword", keyword},
                                    {"page", std::to_string(page)},
                                    {"page_size", std::to_string(page_size)},
                                    {"order", "fans"}},
                                   sessdata);
    std::string resp = Network::fetch_cookie(url, cookie, "https://www.bilibili.com");
    if (resp.empty()) {
        LOG("[Bilibili] search_users: empty response");
        return out;
    }
    try {
        json j = json::parse(resp);
        if (j.value("code", -1) != 0) {
            LOG(fmt::format("[Bilibili] search_users error: code={} msg={}", j.value("code", 0),
                            j.value("message", "?")));
            return out;
        }
        if (j["data"].contains("result") && j["data"]["result"].is_array()) {
            for (const auto &u : j["data"]["result"]) {
                UserInfo ui;
                ui.mid = std::to_string(u.value("mid", 0));
                if (ui.mid == "0")
                    continue;
                ui.uname = u.value("uname", "Untitled");
                ui.sign = u.value("usign", "");
                ui.fans = u.value("fans", 0);
                ui.videos = u.value("videos", 0);
                ui.upic = u.value("upic", "");
                if (!ui.upic.empty() && ui.upic.find("://") == std::string::npos)
                    ui.upic =
                        "https:" + ui.upic; // API returns protocol-relative "//i2.hdslb.com/..."
                ui.url = fmt::format("https://space.bilibili.com/{}/video", ui.mid);
                out.push_back(ui);
            }
        }
        LOG(fmt::format("[Bilibili] search_users: {} users for '{}'", out.size(), keyword));
    } catch (const std::exception &e) {
        LOG(fmt::format("[Bilibili] search_users parse error: {}", e.what()));
    }
    return out;
}

std::string BilibiliAPI::build_cookies_txt(const std::string &sessdata, const std::string &bili_jct,
                                           const std::string &dedeuserid) {
    // Netscape cookies.txt format for yt-dlp --cookies.
    //   domain  include_subdomains  path  secure  expiry  name  value
    int64_t expiry = 1893456000; // ~2030 (far future; Bilibili cookies last ~1 year)
    return fmt::format("# Netscape HTTP Cookie File\n"
                       "# Generated by panicast B-mode login.\n"
                       ".bilibili.com\tTRUE\t/\tTRUE\t{}\tSESSDATA\t{}\n"
                       ".bilibili.com\tTRUE\t/\tTRUE\t{}\tbili_jct\t{}\n"
                       ".bilibili.com\tTRUE\t/\tTRUE\t{}\tDedeUserID\t{}\n",
                       expiry, sessdata, expiry, bili_jct, expiry, dedeuserid);
}

std::string BilibiliAPI::extract_sessdata_from_cookies_txt(const std::string &content) {
    // Parse Netscape cookies.txt, find SESSDATA value.
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        // Fields: domain  flag  path  secure  expiry  name  value
        std::istringstream ls(line);
        std::string domain, flag, path, secure, expiry, name, value;
        if (std::getline(ls, domain, '\t') && std::getline(ls, flag, '\t') &&
            std::getline(ls, path, '\t') && std::getline(ls, secure, '\t') &&
            std::getline(ls, expiry, '\t') && std::getline(ls, name, '\t') &&
            std::getline(ls, value, '\t')) {
            if (name == "SESSDATA")
                return value;
        }
    }
    return "";
}

} // namespace panicast
