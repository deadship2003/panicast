// DouyinApi — direct Douyin web API client with X-Bogus signature.
//
// Ported from PodRadio-Win_Qt `src/net/douyin_api.cpp` (Plan B step 4, verified 2026-08-01).
//   X-Bogus is a pure static algorithm (MD5 + RC4 + fixed magic constants) — no JS engine.
//   Reference algorithm: f2/utils/xbogus.py (Apache-2.0, Johnserf-Seed/f2).
//
// All methods are synchronous curl calls — run them on a worker thread (they block on the network).
#include "panicast/net/douyin_api.h"

#include "panicast/core/utils.h" // Utils::url_encode (META-6 search)

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#include <curl/curl.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "panicast/config/ini_config.h"
#include "panicast/core/crypto.h"
#include "panicast/core/logger.h"
#include "panicast/net/network.h"

using json = nlohmann::json;

namespace panicast
{

// Custom base64 alphabet used by Douyin's X-Bogus encoder (NOT the standard RFC 4648 alphabet).
static const char kXBogusAlphabet[] =
    "Dkdpgh4ZKsQB80/Mfvw36XI1R25-WUAlEi7NLboqYTOPuzmFjJnryx9HVGcaStCe=";

// Fixed Chrome UA — the signing UA and the HTTP request UA must match (Douyin cross-checks).
// The browser_*/os_* query params below are keyed to Chrome 150 on Windows to stay consistent.
static const char *kDouyinUA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                               "(KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36";

void DouyinApi::setSession(const std::string &cookieHeader, const std::string &userAgent) {
    cookie_ = cookieHeader;
    ua_ = userAgent.empty() ? kDouyinUA : userAgent;
}

// ── crypto helpers ────────────────────────────────────────────────────────────────

std::vector<unsigned char> DouyinApi::rc4(const std::vector<unsigned char> &key,
                                          const std::vector<unsigned char> &data) {
    unsigned char S[256];
    for (int i = 0; i < 256; ++i)
        S[i] = (unsigned char)i;
    int j = 0;
    for (int i = 0; i < 256; ++i) {
        unsigned char k = key.empty() ? 0 : key[i % key.size()];
        j = (j + S[i] + k) & 0xff;
        std::swap(S[i], S[j]);
    }
    std::vector<unsigned char> out;
    out.reserve(data.size());
    int i = 0;
    j = 0;
    for (unsigned char b : data) {
        i = (i + 1) & 0xff;
        j = (j + S[i]) & 0xff;
        std::swap(S[i], S[j]);
        out.push_back((unsigned char)(b ^ S[(S[i] + S[j]) & 0xff]));
    }
    return out;
}

// Raw 16-byte MD5 via OpenSSL EVP (the reference used QCryptographicHash::Md5).
std::vector<unsigned char> DouyinApi::md5Raw(const std::string &data) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        return {};
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    bool ok = EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, data.c_str(), data.size()) == 1 &&
              EVP_DigestFinal_ex(ctx, digest, &len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok)
        return {};
    return std::vector<unsigned char>(digest, digest + len);
}

// md5_str_to_array: if the string is a hex digest (≤32 chars) → hex-decode it; otherwise
//   (already decoded bytes) → widen each char to int. Mirrors f2's md5_str_to_array.
std::vector<int> DouyinApi::md5StrToArray(const std::string &s) {
    std::vector<int> out;
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return 0;
    };
    if (s.size() > 32) {
        out.reserve(s.size());
        for (unsigned char c : s)
            out.push_back((int)c);
        return out;
    }
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back((hexVal(s[i]) << 4) | hexVal(s[i + 1]));
    return out;
}

static std::string bytesToHex(const std::vector<unsigned char> &raw) {
    static const char *hexd = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char b : raw) {
        out.push_back(hexd[b >> 4]);
        out.push_back(hexd[b & 0x0f]);
    }
    return out;
}

std::string DouyinApi::md5Hex(const std::string &s) {
    return bytesToHex(md5Raw(s));
}

std::string DouyinApi::md5HexOfArray(const std::vector<int> &data) {
    std::string bytes;
    bytes.reserve(data.size());
    for (int v : data)
        bytes.push_back((char)(v & 0xff));
    return bytesToHex(md5Raw(bytes));
}

// ── X-Bogus ───────────────────────────────────────────────────────────────────────
// compute_x_bogus(url_params, user_agent): pure static algorithm, ported verbatim from the
//   verified C++ reference. Byte-exact — do not "simplify" any step.
std::string DouyinApi::computeXBogus(const std::string &urlParams) const {
    // array1: base64(rc4(ua_key, user_agent)) → md5 hex → bytes
    static const std::vector<unsigned char> uaKey = {0x00, 0x01, 0x0c};
    std::vector<unsigned char> uaBytes(ua_.begin(), ua_.end());
    auto rc4Ua = rc4(uaKey, uaBytes);
    std::string b64Str =
        base64_encode(reinterpret_cast<const uint8_t *>(rc4Ua.data()), rc4Ua.size());
    std::vector<int> array1 = md5StrToArray(md5Hex(b64Str));

    // array2: md5 hex of the md5 of the empty-string digest, as bytes
    std::vector<int> array2 =
        md5StrToArray(md5HexOfArray(md5StrToArray("d41d8cd98f00b204e9800998ecf8427e")));

    // upa: md5 hex of (md5 hex of url_params → bytes)
    std::vector<int> upa = md5StrToArray(md5HexOfArray(md5StrToArray(md5Hex(urlParams))));

    long long ts = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    const long long ct = 536919696;

    std::vector<int> newArr = {
        64,
        0, // 0.00390625 stored as int 0 per reference
        1,
        12,
        upa.size() > 15 ? upa[14] : 0,
        upa.size() > 15 ? upa[15] : 0,
        array2.size() > 15 ? array2[14] : 0,
        array2.size() > 15 ? array2[15] : 0,
        array1.size() > 15 ? array1[14] : 0,
        array1.size() > 15 ? array1[15] : 0,
        (int)((ts >> 24) & 255),
        (int)((ts >> 16) & 255),
        (int)((ts >> 8) & 255),
        (int)(ts & 255),
        (int)((ct >> 24) & 255),
        (int)((ct >> 16) & 255),
        (int)((ct >> 8) & 255),
        (int)(ct & 255),
    };

    int xr = newArr[0];
    for (size_t i = 1; i < newArr.size(); ++i)
        xr ^= newArr[i];
    newArr.push_back(xr);

    // split even/odd, then concatenate
    std::vector<int> half1, half2;
    for (size_t i = 0; i < newArr.size(); ++i)
        (i % 2 == 0 ? half1 : half2).push_back(newArr[i]);
    std::vector<int> merge;
    merge.insert(merge.end(), half1.begin(), half1.end());
    merge.insert(merge.end(), half2.begin(), half2.end());

    // positional reorder (merge[0,10,1,11,2,12,...]) — verified mapping from the C++ reference
    std::vector<unsigned char> y = {
        (unsigned char)merge[0],  (unsigned char)merge[10], (unsigned char)merge[1],
        (unsigned char)merge[11], (unsigned char)merge[2],  (unsigned char)merge[12],
        (unsigned char)merge[3],  (unsigned char)merge[13], (unsigned char)merge[4],
        (unsigned char)merge[14], (unsigned char)merge[5],  (unsigned char)merge[15],
        (unsigned char)merge[6],  (unsigned char)merge[16], (unsigned char)merge[7],
        (unsigned char)merge[17], (unsigned char)merge[8],  (unsigned char)merge[18],
        (unsigned char)merge[9],
    };

    static const std::vector<unsigned char> finalKey = {0xff};
    auto enc = rc4(finalKey, y);

    // Sized construction (resize + copy) instead of reserve+push_back: GCC 14's
    //   -Wfree-nonheap-object fires a false positive on the inlined realloc-append
    //   path of push_back-after-reserve here (deallocate "on pointer with nonzero
    //   offset" for a provably-heap pointer).
    std::vector<unsigned char> garbled(enc.size() + 2);
    garbled[0] = 2;
    garbled[1] = 255;
    std::copy(enc.begin(), enc.end(), garbled.begin() + 2);

    // custom base64: 3 bytes → 4 chars over the Douyin alphabet
    std::string xb;
    xb.reserve((garbled.size() / 3 + 1) * 4);
    for (size_t i = 0; i + 2 < garbled.size(); i += 3) {
        unsigned int a = garbled[i];
        unsigned int b = garbled[i + 1];
        unsigned int c = garbled[i + 2];
        unsigned int x = (a << 16) | (b << 8) | c;
        xb += kXBogusAlphabet[(x & 16515072) >> 18];
        xb += kXBogusAlphabet[(x & 258048) >> 12];
        xb += kXBogusAlphabet[(x & 4032) >> 6];
        xb += kXBogusAlphabet[x & 63];
    }
    return xb;
}

// ── HTTP ──────────────────────────────────────────────────────────────────────────

std::string DouyinApi::signedGet(const std::string &endpoint, const std::string &params,
                                 std::string &err) const {
    err.clear();
    std::string xb = computeXBogus(params);
    std::string url = "https://www.douyin.com" + endpoint + "?" + params + "&X-Bogus=" + xb;

    CurlRAII curl_raii;
    CURL *curl = curl_raii.handle;
    if (!curl) {
        err = "curl init failed";
        return {};
    }

    std::string body;
    auto write_cb = +[](void *ptr, size_t size, size_t nmemb, void *data) -> size_t {
        static_cast<std::string *>(data)->append(static_cast<char *>(ptr), size * nmemb);
        return size * nmemb;
    };

    struct curl_slist *hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Accept: application/json, text/plain, */*");
    hdrs = curl_slist_append(hdrs, "Referer: https://www.douyin.com/");
    if (!cookie_.empty())
        hdrs = curl_slist_append(hdrs, ("Cookie: " + cookie_).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua_.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    apply_network_proxy(curl, url);

    bool tls = IniConfig::instance().get_tls_verify();
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, tls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, tls ? 2L : 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);

    if (res != CURLE_OK) {
        err = std::string("curl: ") + curl_easy_strerror(res);
        return {};
    }
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (code != 200)
        err = fmt::format("HTTP {}", code);
    return body;
}

// ── Cookie helpers ────────────────────────────────────────────────────────────────

// Parse one line of a Netscape/curl cookie file (7 whitespace-separated fields:
//   domain  flag  path  secure  expiry  name  value). Returns false for comments/blank lines.
// Handles curl's "#HttpOnly_" domain prefix (curl writes HttpOnly cookies that way).
static bool parse_cookie_line(const std::string &line, std::string &domain, std::string &name,
                              std::string &value) {
    std::istringstream ls(line);
    std::string d, f1, f2, f3, f4, n, v;
    if (!(ls >> d >> f1 >> f2 >> f3 >> f4 >> n))
        return false;
    std::getline(ls, v);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
        v.erase(v.begin());
    const std::string pfx = "#HttpOnly_";
    if (d.rfind(pfx, 0) == 0)
        d = d.substr(pfx.size());
    domain = std::move(d);
    name = std::move(n);
    value = std::move(v);
    return true;
}

// Raw GET against the (unsigned) Douyin login endpoints, using a persistent cookie jar so
//   bootstrap cookies (ttwid / passport_csrf_token) carry forward and the final sessionid lands
//   on disk. Unlike signedGet, this does NOT append X-Bogus.
static std::string douyin_login_get(const std::string &url, const std::string &jar_path,
                                    const std::string &ua, std::string &err) {
    err.clear();
    CurlRAII curl_raii;
    CURL *curl = curl_raii.handle;
    if (!curl) {
        err = "curl init failed";
        return {};
    }
    std::string body;
    auto write_cb = +[](void *ptr, size_t size, size_t nmemb, void *data) -> size_t {
        static_cast<std::string *>(data)->append(static_cast<char *>(ptr), size * nmemb);
        return size * nmemb;
    };
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    if (!jar_path.empty()) {
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, jar_path.c_str()); // read existing cookies
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR, jar_path.c_str());  // persist merged cookies
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    apply_network_proxy(curl, url);
    bool tls = IniConfig::instance().get_tls_verify();
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, tls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, tls ? 2L : 0L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        err = std::string("curl: ") + curl_easy_strerror(res);
        return {};
    }
    return body;
}

// Read a named cookie's value from a Netscape/curl cookie file (returns "" if absent).
static std::string jar_cookie(const std::string &jar_path, const std::string &cookie_name) {
    std::ifstream f(jar_path);
    if (!f)
        return {};
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::string domain, name, value;
        if (!parse_cookie_line(line, domain, name, value))
            continue;
        if (name == cookie_name)
            return value;
    }
    return {};
}

std::string DouyinApi::build_cookie_header_from_txt(const std::string &cookies_txt,
                                                    const std::string &domain) {
    std::string out;
    std::istringstream iss(cookies_txt);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::string cdomain, name, value;
        if (!parse_cookie_line(line, cdomain, name, value))
            continue;
        // Match ".douyin.com" (leading dot), "douyin.com", or a subdomain cookie.
        bool match = (cdomain == domain) || (cdomain == "." + domain) ||
                     (cdomain.size() > domain.size() + 1 &&
                      cdomain.compare(cdomain.size() - domain.size() - 1, domain.size() + 1,
                                      "." + domain) == 0);
        if (!match)
            continue;
        if (!out.empty())
            out += "; ";
        out += name + "=" + value;
    }
    return out;
}

// ── API methods ───────────────────────────────────────────────────────────────────

// Endpoint A: query/user — logged-in user's numeric uid
void DouyinApi::fetchMyUserId(std::function<void(const std::string &, const std::string &)> cb) {
    std::string err;
    std::string params =
        "device_platform=webapp&aid=6383&channel=channel_pc_web&cookie_enabled=true"
        "&browser_language=zh-CN&browser_platform=Win32&browser_name=Chrome"
        "&browser_version=150.0.0.0&browser_online=true&engine_name=Blink"
        "&engine_version=150.0.0.0&os_name=Windows&os_version=10&platform=PC"
        "&screen_width=1920&screen_height=1080";
    std::string body = signedGet("/aweme/v1/web/query/user/", params, err);
    if (body.empty()) {
        cb("", err.empty() ? "empty response" : err);
        return;
    }
    try {
        auto j = json::parse(body);
        if (j.value("status_code", -1) != 0) {
            cb("", j.value("status_msg", std::string("status_code != 0")));
            return;
        }
        std::string uid = j.value("user", json::object()).value("uid", "");
        cb(uid, uid.empty() ? "no uid in response" : "");
    } catch (const std::exception &e) {
        cb("", std::string("parse: ") + e.what());
    }
}

// Endpoint A2: profile/other — logged-in user's nickname + sec_uid
void DouyinApi::fetchMyProfile(const std::string &userId,
                               std::function<void(const MyProfile &)> cb) {
    MyProfile out;
    std::string params =
        "device_platform=webapp&aid=6383&channel=channel_pc_web&publish_video_strategy_type=2"
        "&personal_center_strategy=1&source=channel_pc_web&user_id=" +
        userId +
        "&cookie_enabled=true&browser_language=zh-CN&browser_platform=Win32"
        "&browser_name=Chrome&browser_version=150.0.0.0&browser_online=true"
        "&engine_name=Blink&engine_version=150.0.0.0&os_name=Windows&os_version=10"
        "&platform=PC&screen_width=1920&screen_height=1080";
    std::string body = signedGet("/aweme/v1/web/profile/other/", params, out.err);
    if (body.empty()) {
        if (out.err.empty())
            out.err = "empty response";
        cb(out);
        return;
    }
    try {
        auto j = json::parse(body);
        if (j.value("status_code", -1) != 0) {
            out.err = j.value("status_msg", std::string("status_code != 0"));
            cb(out);
            return;
        }
        auto user = j.value("user", json::object());
        out.nickname = user.value("nickname", "");
        out.secUid = user.value("sec_uid", "");
        out.signature = user.value("signature", "");
        out.ok = true;
    } catch (const std::exception &e) {
        out.err = std::string("parse: ") + e.what();
    }
    cb(out);
}

// Endpoint B: following/list — one page of followed UP masters (requires login cookie)
void DouyinApi::fetchFollowing(const std::string &userId, int offset, int count,
                               std::function<void(const FollowingResult &)> cb) {
    FollowingResult out;
    std::string params =
        "device_platform=webapp&aid=6383&channel=channel_pc_web&user_id=" + userId +
        "&offset=" + std::to_string(offset) + "&limit=" + std::to_string(count) +
        "&address_book_access=0&gps_access=0&source_type=10&sec_user_id=&publish_video_strategy_"
        "type=2"
        "&cookie_enabled=true&browser_language=zh-CN&browser_platform=Win32"
        "&browser_name=Chrome&browser_version=150.0.0.0&browser_online=true"
        "&engine_name=Blink&engine_version=150.0.0.0&os_name=Windows&os_version=10"
        "&platform=PC&screen_width=1920&screen_height=1080";
    std::string body = signedGet("/aweme/v1/web/following/list/", params, out.err);
    if (body.empty()) {
        if (out.err.empty())
            out.err = "empty response";
        cb(out);
        return;
    }
    try {
        auto j = json::parse(body);
        if (j.value("status_code", -1) != 0) {
            out.err = j.value("status_msg", std::string("status_code != 0"));
            cb(out);
            return;
        }
        out.total = j.value("total", 0);
        out.hasMore = j.value("has_more", 0) != 0;
        out.nextOffset = offset + count;
        for (const auto &fu : j.value("followings", json::array())) {
            FollowingUser u;
            u.uid = fu.value("uid", "");
            u.secUid = fu.value("sec_uid", "");
            u.nickname = fu.value("nickname", "");
            u.signature = fu.value("signature", "");
            u.followerCount = fu.value("follower_count", 0);
            u.avatarUrl =
                fu.value("avatar_thumb", json::object()).value("url_list", json::array()).empty()
                    ? ""
                    : fu.value("avatar_thumb", json::object())
                          .value("url_list", json::array())[0]
                          .get<std::string>();
            out.users.push_back(std::move(u));
        }
        out.ok = true;
    } catch (const std::exception &e) {
        out.err = std::string("parse: ") + e.what();
    }
    cb(out);
}

// Endpoint C: aweme/post — one page of a UP's posted videos
void DouyinApi::fetchUserVideos(const std::string &secUserId, long long maxCursor, int count,
                                std::function<void(const UserVideoResult &)> cb) {
    UserVideoResult out;
    std::string params =
        "device_platform=webapp&aid=6383&channel=channel_pc_web&sec_user_id=" + secUserId +
        "&max_cursor=" + std::to_string(maxCursor) + "&count=" + std::to_string(count) +
        "&publish_video_strategy_type=2&need_time_list=1&time_list_query=0"
        "&cookie_enabled=true&screen_width=1920&screen_height=1080"
        "&browser_language=zh-CN&browser_platform=Win32&browser_name=Chrome"
        "&browser_version=150.0.0.0&browser_online=true&engine_name=Blink"
        "&engine_version=150.0.0.0&os_name=Windows&os_version=10";
    std::string body = signedGet("/aweme/v1/web/aweme/post/", params, out.err);
    if (body.empty()) {
        if (out.err.empty())
            out.err = "empty response";
        cb(out);
        return;
    }
    try {
        auto j = json::parse(body);
        if (j.value("status_code", -1) != 0) {
            out.err = j.value("status_msg", std::string("status_code != 0"));
            cb(out);
            return;
        }
        out.hasMore = j.value("has_more", 0) != 0;
        out.nextCursor = j.value("max_cursor", maxCursor);
        for (const auto &aw : j.value("aweme_list", json::array())) {
            UserVideo v;
            v.awemeId = aw.value("aweme_id", "");
            v.desc = aw.value("desc", "");
            auto video = aw.value("video", json::object());
            v.duration = video.value("duration", 0);
            auto play = video.value("play_addr", json::object()).value("url_list", json::array());
            if (!play.empty())
                v.playUrl = play[0].get<std::string>();
            auto cover = video.value("cover", json::object()).value("url_list", json::array());
            if (!cover.empty())
                v.coverUrl = cover[0].get<std::string>();
            out.videos.push_back(std::move(v));
        }
        out.ok = true;
    } catch (const std::exception &e) {
        out.err = std::string("parse: ") + e.what();
    }
    cb(out);
}

// META-6: general keyword search — /aweme/v1/web/search/item/, same signed GET
//   pipeline and result parsing as fetchUserVideos.
void DouyinApi::searchKeyword(const std::string &keyword, int offset, int count,
                              std::function<void(const UserVideoResult &)> cb) {
    UserVideoResult out;
    std::string params = "device_platform=webapp&aid=6383&channel=channel_pc_web"
                         "&search_source=normal_search&type=1&keyword=" +
                         Utils::url_encode(keyword) + "&start=" + std::to_string(offset) +
                         "&count=" + std::to_string(count) +
                         "&cookie_enabled=true&screen_width=1920&screen_height=1080"
                         "&browser_language=zh-CN&browser_platform=Win32&browser_name=Chrome"
                         "&browser_version=150.0.0.0&browser_online=true&engine_name=Blink"
                         "&engine_version=150.0.0.0&os_name=Windows&os_version=10";
    std::string body = signedGet("/aweme/v1/web/search/item/", params, out.err);
    if (body.empty()) {
        if (out.err.empty())
            out.err = "empty response";
        cb(out);
        return;
    }
    try {
        auto j = json::parse(body);
        // Search replies carry data[] with aweme_info inside (not a flat aweme_list).
        auto entries = j.contains("data") && j["data"].is_array() ? j["data"] : json::array();
        out.hasMore = j.value("has_more", 0) != 0;
        for (const auto &e : entries) {
            auto aw = e.contains("aweme_info") ? e["aweme_info"] : json::object();
            if (aw.empty())
                continue;
            UserVideo v;
            v.awemeId = aw.value("aweme_id", "");
            v.desc = aw.value("desc", "");
            auto video = aw.value("video", json::object());
            v.duration = video.value("duration", 0);
            auto play = video.value("play_addr", json::object()).value("url_list", json::array());
            if (!play.empty())
                v.playUrl = play[0].get<std::string>();
            auto cover = video.value("cover", json::object()).value("url_list", json::array());
            if (!cover.empty())
                v.coverUrl = cover[0].get<std::string>();
            out.videos.push_back(std::move(v));
        }
        out.ok = true;
    } catch (const std::exception &e) {
        out.err = std::string("parse: ") + e.what();
    }
    cb(out);
}

// Endpoint D: aweme/favorite — a UP's liked videos
void DouyinApi::fetchUserLikes(const std::string &secUserId, long long maxCursor, int count,
                               std::function<void(const UserVideoResult &)> cb) {
    UserVideoResult out;
    std::string params =
        "device_platform=webapp&aid=6383&channel=channel_pc_web&sec_user_id=" + secUserId +
        "&max_cursor=" + std::to_string(maxCursor) + "&count=" + std::to_string(count) +
        "&publish_video_strategy_type=2&need_time_list=1&time_list_query=0"
        "&cookie_enabled=true&screen_width=1920&screen_height=1080"
        "&browser_language=zh-CN&browser_platform=Win32&browser_name=Chrome"
        "&browser_version=150.0.0.0&browser_online=true&engine_name=Blink"
        "&engine_version=150.0.0.0&os_name=Windows&os_version=10";
    std::string body = signedGet("/aweme/v1/web/aweme/favorite/", params, out.err);
    if (body.empty()) {
        if (out.err.empty())
            out.err = "empty response";
        cb(out);
        return;
    }
    try {
        auto j = json::parse(body);
        if (j.value("status_code", -1) != 0) {
            out.err = j.value("status_msg", std::string("status_code != 0"));
            cb(out);
            return;
        }
        out.hasMore = j.value("has_more", 0) != 0;
        out.nextCursor = j.value("max_cursor", maxCursor);
        for (const auto &aw : j.value("aweme_list", json::array())) {
            UserVideo v;
            v.awemeId = aw.value("aweme_id", "");
            v.desc = aw.value("desc", "");
            auto video = aw.value("video", json::object());
            v.duration = video.value("duration", 0);
            auto play = video.value("play_addr", json::object()).value("url_list", json::array());
            if (!play.empty())
                v.playUrl = play[0].get<std::string>();
            auto cover = video.value("cover", json::object()).value("url_list", json::array());
            if (!cover.empty())
                v.coverUrl = cover[0].get<std::string>();
            out.videos.push_back(std::move(v));
        }
        out.ok = true;
    } catch (const std::exception &e) {
        out.err = std::string("parse: ") + e.what();
    }
    cb(out);
}

// Endpoint F: mix/aweme — a UP's collection videos (paginated by `cursor`)
void DouyinApi::fetchUserMix(const std::string &mixId, long long cursor, int count,
                             std::function<void(const UserVideoResult &)> cb) {
    UserVideoResult out;
    std::string params = "device_platform=webapp&aid=6383&channel=channel_pc_web&mix_id=" + mixId +
                         "&cursor=" + std::to_string(cursor) + "&count=" + std::to_string(count) +
                         "&cookie_enabled=true&screen_width=1920&screen_height=1080"
                         "&browser_language=zh-CN&browser_platform=Win32&browser_name=Chrome"
                         "&browser_version=150.0.0.0&browser_online=true&engine_name=Blink"
                         "&engine_version=150.0.0.0&os_name=Windows&os_version=10";
    std::string body = signedGet("/aweme/v1/web/mix/aweme/", params, out.err);
    if (body.empty()) {
        if (out.err.empty())
            out.err = "empty response";
        cb(out);
        return;
    }
    try {
        auto j = json::parse(body);
        if (j.value("status_code", -1) != 0) {
            out.err = j.value("status_msg", std::string("status_code != 0"));
            cb(out);
            return;
        }
        out.hasMore = j.value("has_more", 0) != 0;
        out.nextCursor = j.value("cursor", cursor);
        for (const auto &aw : j.value("aweme_list", json::array())) {
            UserVideo v;
            v.awemeId = aw.value("aweme_id", "");
            v.desc = aw.value("desc", "");
            auto video = aw.value("video", json::object());
            v.duration = video.value("duration", 0);
            auto play = video.value("play_addr", json::object()).value("url_list", json::array());
            if (!play.empty())
                v.playUrl = play[0].get<std::string>();
            auto cover = video.value("cover", json::object()).value("url_list", json::array());
            if (!cover.empty())
                v.coverUrl = cover[0].get<std::string>();
            out.videos.push_back(std::move(v));
        }
        out.ok = true;
    } catch (const std::exception &e) {
        out.err = std::string("parse: ") + e.what();
    }
    cb(out);
}

// Collection helper: get the mix_id of a UP's first video carrying mix_info
void DouyinApi::fetchFirstMixId(const std::string &secUserId,
                                std::function<void(const std::string &, const std::string &)> cb) {
    // Reuse Endpoint C with a single item and inspect its mix_info.
    UserVideoResult res;
    fetchUserVideos(secUserId, 0, 1, [&](const UserVideoResult &r) { res = r; });
    if (!res.ok) {
        cb("", res.err.empty() ? "no videos" : res.err);
        return;
    }
    // mix_info lives on the aweme_list entry, but fetchUserVideos already stripped it.
    // Fetch one page raw and look for mix_info directly.
    std::string err;
    std::string params =
        "device_platform=webapp&aid=6383&channel=channel_pc_web&sec_user_id=" + secUserId +
        "&max_cursor=0&count=20"
        "&publish_video_strategy_type=2&need_time_list=1&time_list_query=0"
        "&cookie_enabled=true&screen_width=1920&screen_height=1080"
        "&browser_language=zh-CN&browser_platform=Win32&browser_name=Chrome"
        "&browser_version=150.0.0.0&browser_online=true&engine_name=Blink"
        "&engine_version=150.0.0.0&os_name=Windows&os_version=10";
    std::string body = signedGet("/aweme/v1/web/aweme/post/", params, err);
    if (body.empty()) {
        cb("", err.empty() ? "empty response" : err);
        return;
    }
    try {
        auto j = json::parse(body);
        if (j.value("status_code", -1) != 0) {
            cb("", j.value("status_msg", std::string("status_code != 0")));
            return;
        }
        for (const auto &aw : j.value("aweme_list", json::array())) {
            auto mix = aw.value("mix_info", json::object());
            if (mix.contains("mix_id") && mix["mix_id"].is_string()) {
                cb(mix["mix_id"].get<std::string>(), "");
                return;
            }
        }
        cb("", "no mix on this user");
    } catch (const std::exception &e) {
        cb("", std::string("parse: ") + e.what());
    }
}

// ── QR login (terminal QR code, pure API) ─────────────────────────────────────
// Douyin login endpoints are unsigned (X-Bogus only guards the business endpoints).
//   The whole flow rides one curl cookie jar: bootstrap → fetch code → poll; on
//   success sessionid/sessionid_ss land in douyin_cookie.txt automatically.
//
// ⚠️ Endpoints/params are guesses assembled from public material — verify against
//   a real browser's F12 capture:
//   - get_qrcode's data.qrcode may be a scannable URL string OR a base64 image.
//     A base64 image cannot be rendered as a terminal QR code (a field/endpoint
//     returning a URL would be needed instead).
//   - check_qrconnect's status field name (status / error_code) also needs
//     confirming against real responses.
DouyinApi::LoginQR DouyinApi::request_qrcode() {
    LoginQR qr;
    std::string jar = IniConfig::instance().get_tiktok_douyin_cookies_file();
    std::string err;

    // 1. Bootstrap: visit the douyin.com homepage anonymously so curl's jar picks
    //    up ttwid + passport_csrf_token. (No login-state check — the csrf token is
    //    set on first visit anyway; login state is judged by sessionid alone.)
    douyin_login_get("https://www.douyin.com/", jar, kDouyinUA, err);

    // 2. Fetch the QR code + token. Field names/params pending F12 verification.
    std::string qr_url =
        "https://sso.douyin.com/get_qrcode/?aid=6383"
        "&service=https%3A%2F%2Fwww.douyin.com"
        "&device_platform=webapp&cookie_enabled=true&browser_language=zh-CN"
        "&browser_platform=Win32&browser_name=Chrome&browser_version=150.0.0.0"
        "&browser_online=true&engine_name=Blink&engine_version=150.0.0.0"
        "&os_name=Windows&os_version=10&platform=PC&screen_width=1920&screen_height=1080";
    std::string body = douyin_login_get(qr_url, jar, kDouyinUA, err);
    if (body.empty()) {
        qr.err = err.empty() ? "empty response" : err;
        return qr;
    }
    try {
        auto j = json::parse(body);
        auto d = j.value("data", json::object());
        qr.token = d.value("token", "");
        // Prefer a scannable URL field; fall back to data.qrcode (may be base64 image).
        qr.qr_content = d.value("qrcode_index_url", "");
        if (qr.qr_content.empty())
            qr.qr_content = d.value("qrcode_url", "");
        if (qr.qr_content.empty())
            qr.qr_content = d.value("url", "");
        if (qr.qr_content.empty())
            qr.qr_content = d.value("qrcode", "");
        qr.ok = !qr.token.empty();
        if (!qr.ok)
            qr.err = "no token in get_qrcode response";
    } catch (const std::exception &e) {
        qr.err = std::string("parse: ") + e.what();
    }
    return qr;
}

DouyinApi::LoginResult DouyinApi::poll_qrcode(const std::string &token) {
    LoginResult r;
    std::string jar = IniConfig::instance().get_tiktok_douyin_cookies_file();
    std::string err;
    std::string url = "https://sso.douyin.com/check_qrconnect/?aid=6383&token=" + token +
                      "&service=https%3A%2F%2Fwww.douyin.com&device_platform=webapp"
                      "&cookie_enabled=true&browser_language=zh-CN&browser_platform=Win32"
                      "&browser_name=Chrome&browser_version=150.0.0.0&browser_online=true"
                      "&engine_name=Blink&engine_version=150.0.0.0&os_name=Windows&os_version=10"
                      "&platform=PC&screen_width=1920&screen_height=1080";
    std::string body = douyin_login_get(url, jar, kDouyinUA, err);
    if (body.empty()) {
        r.err = err.empty() ? "empty response" : err;
        return r;
    }
    try {
        auto j = json::parse(body);
        auto d = j.value("data", json::object());
        // Status: 1=not scanned 2=scanned, awaiting confirmation 3=success. Field
        //   names (status/error_code) pending F12 verification.
        if (d.contains("status"))
            r.code = d.value("status", 0);
        else if (d.contains("error_code"))
            r.code = d.value("error_code", 0);
        if (r.code == 3) {
            // Login succeeded: sessionid was written to douyin_cookie.txt by the cookie jar.
            r.sessionid = jar_cookie(jar, "sessionid");
            r.ok = !r.sessionid.empty();
            if (!r.ok)
                r.err = "status=3 but no sessionid cookie";
        }
    } catch (const std::exception &e) {
        r.err = std::string("parse: ") + e.what();
    }
    return r;
}

} // namespace panicast
