// Y01: Google OAuth device flow + YouTube Data API v3 + InnerTube watch history.
// See header for the SmartTube-style rationale and the known API limits.
#include "panicast/net/google_oauth.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/core/paths.h"
#include "panicast/net/network.h"
#include "panicast/core/utils.h"

namespace panicast
{

using json = nlohmann::json;

namespace
{
// Y24.29: url_encode removed — use Utils::url_encode (was duplicated).

int64_t now_epoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ── Runtime client-credential loading ────────────────────────────────────────
// Reads the user's downloaded client_secret JSON (Google "Desktop app" client) from the data dir
// so real secrets never live in the binary. Returns loaded=true only when both fields are present.
struct ClientCreds {
    std::string id;
    std::string secret;
    bool loaded = false;
};

const ClientCreds &client_creds() {
    static const ClientCreds c = []() {
        ClientCreds out;
        std::string dir = Paths::get_data_dir();
        if (!dir.empty()) {
            namespace fs = std::filesystem;
            std::error_code ec;
            for (auto &entry : fs::directory_iterator(dir, ec)) {
                if (ec)
                    break;
                std::string name = entry.path().filename().string();
                if (name.rfind("client_secret", 0) != 0 || entry.path().extension() != ".json")
                    continue;
                std::ifstream f(entry.path());
                if (!f)
                    continue;
                std::string content((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
                try {
                    json j = json::parse(content);
                    // Desktop client download uses {"installed": {...}}; web uses {"web": {...}}.
                    const json *obj = j.contains("installed")
                                          ? &j["installed"]
                                          : (j.contains("web") ? &j["web"] : nullptr);
                    if (obj) {
                        out.id = obj->value("client_id", "");
                        out.secret = obj->value("client_secret", "");
                        if (!out.id.empty() && !out.secret.empty()) {
                            out.loaded = true;
                            LOG("[OAuth] loaded client credentials from " + entry.path().string());
                            break;
                        }
                    }
                } catch (...) { /* malformed file — try the next one */
                }
            }
        }
        if (!out.loaded) {
            LOG("[OAuth] no client_secret*.json in data dir — using built-in Desktop-app client");
        }
        return out;
    }();
    return c;
}
} // namespace

// Built-in OAuth client (Google "Desktop app", supports the device authorization grant RFC 8628).
//   Used as the fallback when no client_secret*.json is found in the data dir, so Y-mode login
//   works out-of-the-box without the user manually placing a JSON file. The previously-used public
//   SmartTube TV client (861556708454-...) was blocked by Google (invalid_client).
//   Y09: BUILTIN_CLIENT_ID/SECRET come from client_secret_builtin.h, which CMake generates at
//   configure time — baking secrets/client_secret.json if present (other users drop their own
//   there and rebuild), else the project fallback. A runtime <data_dir>/client_secret*.json still
//   takes precedence over this builtin (see client_creds() above).
#include "panicast/net/client_secret_builtin.h"

std::string GoogleOAuth::client_id() {
    const auto &c = client_creds();
    return c.loaded ? c.id : std::string(BUILTIN_CLIENT_ID);
}

std::string GoogleOAuth::client_secret() {
    const auto &c = client_creds();
    return c.loaded ? c.secret : std::string(BUILTIN_CLIENT_SECRET);
}

GoogleOAuth::DeviceCode GoogleOAuth::request_device_code() {
    DeviceCode dc;
    // scope: youtube (read/write subscriptions, needed for play/download login state via yt-dlp oauth2).
    std::string body =
        fmt::format("client_id={}&scope={}", Utils::url_encode(GoogleOAuth::client_id()),
                    Utils::url_encode("https://www.googleapis.com/auth/youtube"));
    std::string resp = Network::post("https://oauth2.googleapis.com/device/code", body);
    if (resp.empty()) {
        dc.error = "network";
        return dc;
    }
    try {
        json j = json::parse(resp);
        if (j.contains("error")) {
            dc.error = j.value("error", "unknown");
            // Log the full Google response so endpoint rejections (invalid_client /
            //   disallowed_useragent / etc.) are visible in panicast.log for diagnosis.
            LOG(fmt::format("[OAuth] device code rejected: {} — {}", dc.error,
                            j.value("error_description", "")));
            return dc;
        }
        dc.device_code = j.value("device_code", "");
        dc.user_code = j.value("user_code", "");
        dc.verification_url = j.value("verification_url", "https://www.google.com/device");
        dc.expires_in = j.value("expires_in", 1800);
        dc.interval = j.value("interval", 5);
        dc.ok = !dc.device_code.empty();
    } catch (const std::exception &e) {
        dc.error = std::string("parse: ") + e.what();
        LOG(fmt::format("[OAuth] device code parse error: {}", e.what()));
    }
    return dc;
}

GoogleOAuth::TokenResult GoogleOAuth::poll_token(const std::string &device_code) {
    TokenResult tr;
    std::string body =
        fmt::format("client_id={}&client_secret={}&device_code={}&grant_type={}",
                    Utils::url_encode(GoogleOAuth::client_id()),
                    Utils::url_encode(GoogleOAuth::client_secret()), Utils::url_encode(device_code),
                    Utils::url_encode("urn:ietf:params:oauth:grant-type:device_code"));
    std::string resp = Network::post("https://oauth2.googleapis.com/token", body);
    if (resp.empty()) {
        tr.error = "network";
        return tr;
    }
    try {
        json j = json::parse(resp);
        if (j.contains("error")) {
            tr.error = j.value("error", "unknown");
            tr.error_desc = j.value("error_description", "");
            // authorization_pending → caller keeps polling; anything else → stop (and is logged).
            if (tr.error != "authorization_pending" && tr.error != "slow_down") {
                LOG(fmt::format("[OAuth] token error: {} — {}", tr.error, tr.error_desc));
            }
            return tr;
        }
        tr.access_token = j.value("access_token", "");
        tr.refresh_token = j.value("refresh_token", "");
        tr.expires_in = j.value("expires_in", 3600);
        tr.scope = j.value("scope", "");
        tr.ok = !tr.access_token.empty();
        tr.obtained_at = now_epoch();
    } catch (const std::exception &e) {
        tr.error = std::string("parse: ") + e.what();
        LOG(fmt::format("[OAuth] token parse error: {}", e.what()));
    }
    return tr;
}

GoogleOAuth::TokenResult GoogleOAuth::refresh(const std::string &refresh_token) {
    TokenResult tr;
    if (refresh_token.empty()) {
        tr.error = "no_refresh_token";
        return tr;
    }
    std::string body = fmt::format(
        "client_id={}&client_secret={}&refresh_token={}&grant_type=refresh_token",
        Utils::url_encode(GoogleOAuth::client_id()),
        Utils::url_encode(GoogleOAuth::client_secret()), Utils::url_encode(refresh_token));
    std::string resp = Network::post("https://oauth2.googleapis.com/token", body);
    if (resp.empty()) {
        tr.error = "network";
        return tr;
    }
    try {
        json j = json::parse(resp);
        if (j.contains("error")) {
            tr.error = j.value("error", "unknown");
            tr.error_desc = j.value("error_description", "");
            return tr;
        }
        tr.access_token = j.value("access_token", "");
        tr.expires_in = j.value("expires_in", 3600);
        tr.scope = j.value("scope", "");
        tr.ok = !tr.access_token.empty();
        tr.obtained_at = now_epoch();
        // refresh_token is NOT returned on refresh; caller keeps the existing one.
    } catch (const std::exception &e) {
        tr.error = std::string("parse: ") + e.what();
        LOG(fmt::format("[OAuth] refresh parse error: {}", e.what()));
    }
    return tr;
}

// Log a YouTube Data API v3 error envelope `{"error":{"code","message","errors":[{reason}]}}`.
//   Data API failures (403 accessNotConfigured = API not enabled in GCP project; 403 quotaExceeded;
//   401 invalid_token; 403 forbidden = OAuth consent screen in Testing mode / scope not granted) were
//   previously swallowed silently — fetch_* returned empty and the user saw "no data" with no clue.
//   This surfaces the real Google reason to panicast.log and the LOG panel.
void log_api_error(const std::string &op, const json &j) {
    if (!j.contains("error"))
        return;
    const auto &e = j["error"];
    int code = e.value("code", 0);
    std::string msg = e.value("message", "");
    std::string reason;
    if (e.contains("errors") && !e["errors"].empty()) {
        reason = e["errors"][0].value("reason", "");
    }
    LOG(fmt::format("[OAuth] {} API error: HTTP {} {} — {}", op, code, reason, msg));
    EVENT_LOG(fmt::format("[OAuth] {} failed: HTTP {} {}", op, code, reason));
}

GoogleOAuth::ChannelIdentity GoogleOAuth::fetch_identity(const std::string &access_token) {
    ChannelIdentity id;
    std::string url = "https://www.googleapis.com/youtube/v3/channels?part=snippet&mine=true";
    std::string resp = Network::fetch_auth(url, access_token);
    if (resp.empty())
        return id;
    try {
        json j = json::parse(resp);
        log_api_error("identity", j);
        if (j.contains("items") && !j["items"].empty()) {
            const auto &item = j["items"][0];
            id.channel_id = item.value("id", "");
            id.title = item["snippet"].value("title", "");
            id.ok = !id.channel_id.empty();
        }
    } catch (const std::exception &e) {
        LOG(fmt::format("[OAuth] identity parse error: {}", e.what()));
    }
    return id;
}

std::vector<YouTubeSubRow> GoogleOAuth::fetch_subscriptions(const std::string &access_token) {
    std::vector<YouTubeSubRow> out;
    std::string page_token;
    int pages = 0;
    int order = 0;
    do {
        std::string url = "https://www.googleapis.com/youtube/v3/"
                          "subscriptions?part=snippet&maxResults=50&mine=true";
        if (!page_token.empty())
            url += "&pageToken=" + Utils::url_encode(page_token);
        std::string resp = Network::fetch_auth(url, access_token);
        if (resp.empty())
            break;
        try {
            json j = json::parse(resp);
            log_api_error("subscriptions", j);
            if (j.contains("items")) {
                for (const auto &it : j["items"]) {
                    YouTubeSubRow r;
                    const auto &sn = it["snippet"];
                    r.channel_id = sn["resourceId"].value("channelId", "");
                    r.channel_name = sn.value("title", "");
                    r.channel_url = "https://www.youtube.com/channel/" + r.channel_id;
                    r.subscription_order = order++;
                    out.push_back(r);
                }
            }
            page_token = j.value("nextPageToken", "");
        } catch (const std::exception &e) {
            LOG(fmt::format("[OAuth] subscriptions parse error: {}", e.what()));
            break;
        }
    } while (!page_token.empty() && ++pages < 40);
    return out;
}

bool GoogleOAuth::subscribe(const std::string &access_token, const std::string &channel_id) {
    // subscriptions.insert requires a snippet channelId in the body.
    json body = {
        {"snippet", {{"resourceId", {{"kind", "youtube#channel"}, {"channelId", channel_id}}}}}};
    std::string resp =
        Network::post("https://www.googleapis.com/youtube/v3/subscriptions?part=snippet",
                      body.dump(), "application/json", {"Authorization: Bearer " + access_token});
    return !resp.empty() && resp.find("\"error\"") == std::string::npos;
}

bool GoogleOAuth::unsubscribe(const std::string &access_token, const std::string &channel_id) {
    // Need the subscription id for the channel: list subscriptions filtered to this channel.
    std::string url = "https://www.googleapis.com/youtube/v3/subscriptions?part=id&mine=true"
                      "&forChannelId=" +
                      Utils::url_encode(channel_id);
    std::string resp = Network::fetch_auth(url, access_token);
    if (resp.empty())
        return false;
    std::string sub_id;
    try {
        json j = json::parse(resp);
        if (j.contains("items") && !j["items"].empty())
            sub_id = j["items"][0].value("id", "");
    } catch (...) {
        return false;
    }
    if (sub_id.empty())
        return false;
    std::string del_url =
        "https://www.googleapis.com/youtube/v3/subscriptions?id=" + Utils::url_encode(sub_id);
    std::string del_resp = Network::del(del_url, access_token);
    // Data API returns 204 No Content (empty body) on success; an error JSON contains "error".
    // P2 (Y23.7): empty response = transport failure (Network::del returns "" on error) — was
    //   falsely reported as success (no "error" substring in empty string). Also 204 No-Content
    //   (successful unsubscribe) returns empty body, so we can't distinguish 204 from failure by
    //   body alone. Treat empty as failure (conservative — user can retry).
    if (del_resp.empty()) {
        LOG("[OAuth] unsubscribe: empty response (transport failure or 204)");
        return false;
    }
    return del_resp.find("\"error\"") == std::string::npos;
}

std::vector<YouTubeHistoryRow> GoogleOAuth::fetch_watch_history(const std::string &access_token) {
    // YouTube Data API v3 has no watch-history endpoint. Use authenticated InnerTube /browse with
    //   browseId=FEhistory (the same shelf as youtube.com/feed/history). Best-effort: parsing the
    //   InnerTube response is fragile and changes; if it fails, return empty (local plays still show).
    std::vector<YouTubeHistoryRow> out;
    json body = {{"context",
                  {{"client",
                    {{"clientName", "WEB"},
                     {"clientVersion", "2.20240719.00.00"},
                     {"hl", "en"},
                     {"gl", "US"}}}}},
                 {"browseId", "FEhistory"}};
    std::vector<std::string> headers = {"Authorization: Bearer " + access_token};
    std::string resp = Network::post("https://www.youtube.com/youtubei/v1/browse", body.dump(),
                                     "application/json", headers);
    if (resp.empty())
        return out;
    try {
        json j = json::parse(resp);
        // Walk the nested shelves for video renderers.
        std::function<void(const json &)> walk = [&](const json &node) {
            if (!node.is_object() && !node.is_array())
                return;
            if (node.is_object()) {
                if (node.contains("videoRenderer")) {
                    const auto &vr = node["videoRenderer"];
                    YouTubeHistoryRow r;
                    r.video_id = vr.value("videoId", "");
                    if (vr.contains("title") && vr["title"].contains("simpleText"))
                        r.title = vr["title"].value("simpleText", "");
                    else if (vr.contains("title") && vr["title"].contains("runs") &&
                             !vr["title"]["runs"].empty())
                        r.title = vr["title"]["runs"][0].value("text", "");
                    if (vr.contains("longBylineText") && vr["longBylineText"].contains("runs") &&
                        !vr["longBylineText"]["runs"].empty())
                        r.channel_name = vr["longBylineText"]["runs"][0].value("text", "");
                    r.source = "youtube";
                    if (!r.video_id.empty())
                        out.push_back(r);
                }
                for (auto it = node.begin(); it != node.end(); ++it)
                    walk(*it);
            } else {
                for (const auto &el : node)
                    walk(el);
            }
        };
        walk(j);
    } catch (const std::exception &e) {
        LOG(fmt::format("[OAuth] watch history parse error: {}", e.what()));
    }
    return out;
}

std::vector<YouTubeVideoInfo> GoogleOAuth::fetch_channel_videos(const std::string &access_token,
                                                                const std::string &channel_id) {
    std::vector<YouTubeVideoInfo> out;
    if (access_token.empty() || channel_id.empty())
        return out;

    // 1) Resolve the channel's uploads playlist id (contentDetails.relatedPlaylists.uploads).
    //    mine=false → use id=<channel_id>. Works with any valid OAuth token (or API key).
    std::string cid = channel_id;
    // Accept a full channel URL; extract the UC... id.
    auto pos = cid.find("/channel/");
    if (pos != std::string::npos)
        cid = cid.substr(pos + 9);
    auto slash = cid.find('/');
    if (slash != std::string::npos)
        cid = cid.substr(0, slash);
    if (cid.empty())
        return out;

    std::string ch_url = "https://www.googleapis.com/youtube/v3/channels?part=contentDetails&id=" +
                         Utils::url_encode(cid);
    std::string ch_resp = Network::fetch_auth(ch_url, access_token);
    std::string uploads_id;
    if (!ch_resp.empty()) {
        try {
            json j = json::parse(ch_resp);
            log_api_error("channel_videos", j);
            if (j.contains("items") && !j["items"].empty()) {
                uploads_id =
                    j["items"][0]["contentDetails"]["relatedPlaylists"].value("uploads", "");
            }
        } catch (const std::exception &e) {
            LOG(fmt::format("[OAuth] channel_videos channels.list parse error: {}", e.what()));
        }
    }
    if (uploads_id.empty()) {
        LOG(fmt::format(
            "[OAuth] channel_videos: no uploads playlist for {} (private/empty or API error)",
            cid));
        return out;
    }

    // 2) Page through playlistItems.list of the uploads playlist.
    std::string page_token;
    int pages = 0;
    do {
        if (++pages > 20)
            break; // safety cap (~1000 videos)
        std::string pl_url = "https://www.googleapis.com/youtube/v3/"
                             "playlistItems?part=snippet&maxResults=50&playlistId=" +
                             Utils::url_encode(uploads_id);
        if (!page_token.empty())
            pl_url += "&pageToken=" + Utils::url_encode(page_token);
        std::string resp = Network::fetch_auth(pl_url, access_token);
        if (resp.empty())
            break;
        try {
            json j = json::parse(resp);
            log_api_error("channel_videos", j);
            if (j.contains("items")) {
                for (const auto &it : j["items"]) {
                    const auto &sn = it["snippet"];
                    std::string vid = sn["resourceId"].value("videoId", "");
                    if (vid.empty())
                        continue;
                    YouTubeVideoInfo v;
                    v.id = vid;
                    v.title = sn.value("title", "Untitled");
                    v.url = "https://www.youtube.com/watch?v=" + vid;
                    // ART-2: pick the largest available thumbnail variant
                    if (sn.contains("thumbnails")) {
                        const auto &th = sn["thumbnails"];
                        for (const char *k : {"maxres", "high", "medium", "default"})
                            if (th.contains(k) && th[k].contains("url")) {
                                v.thumbnail = th[k].value("url", "");
                                if (!v.thumbnail.empty())
                                    break;
                            }
                    }
                    out.push_back(v);
                }
            }
            page_token = j.value("nextPageToken", "");
        } catch (const std::exception &e) {
            LOG(fmt::format("[OAuth] channel_videos playlistItems parse error: {}", e.what()));
            break;
        }
    } while (!page_token.empty() && ++pages < 40);
    LOG(fmt::format("[OAuth] channel_videos: {} videos for {}", out.size(), cid));
    return out;
}

std::vector<YouTubeSearchRow> GoogleOAuth::search(const std::string &access_token,
                                                  const std::string &query,
                                                  const std::string &type_filter, bool music_only) {
    std::vector<YouTubeSearchRow> out;
    if (query.empty())
        return out;
    std::string url = "https://www.googleapis.com/youtube/v3/search?part=snippet&maxResults=25&q=" +
                      Utils::url_encode(query);
    // type filter. music forces video + category 10.
    if (music_only) {
        url += "&type=video&videoCategoryId=10";
    } else if (type_filter == "video" || type_filter == "channel" || type_filter == "playlist") {
        url += "&type=" + type_filter;
    } // else: mixed (no type) → video/channel/playlist
    std::string resp = Network::fetch_auth(url, access_token);
    if (resp.empty()) {
        LOG("[OAuth] search: empty response");
        return out;
    }
    try {
        json j = json::parse(resp);
        log_api_error("search", j);
        if (j.contains("error")) {
            return out;
        }
        if (!j.contains("items"))
            return out;
        for (const auto &it : j["items"]) {
            const auto &id = it["id"];
            std::string kind = id.value("kind", "");
            YouTubeSearchRow r;
            r.music = music_only;
            if (kind == "youtube#video") {
                r.kind = YouTubeSearchRow::Kind::VIDEO;
                r.id = id.value("videoId", "");
                r.url = "https://www.youtube.com/watch?v=" + r.id;
            } else if (kind == "youtube#channel") {
                r.kind = YouTubeSearchRow::Kind::CHANNEL;
                r.id = id.value("channelId", "");
                r.url = "https://www.youtube.com/channel/" + r.id;
            } else if (kind == "youtube#playlist") {
                r.kind = YouTubeSearchRow::Kind::PLAYLIST;
                r.id = id.value("playlistId", "");
                r.url = "https://www.youtube.com/playlist?list=" + r.id;
            } else {
                continue;
            }
            const auto &sn = it.value("snippet", json::object());
            r.title = sn.value("title", "");
            r.channel_title = sn.value("channelTitle", "");
            // Y23: thumbnail (prefer medium, fall back to default).
            if (sn.contains("thumbnails")) {
                const auto &th = sn["thumbnails"];
                if (th.contains("medium"))
                    r.thumbnail_url = th["medium"].value("url", "");
                else if (th.contains("default"))
                    r.thumbnail_url = th["default"].value("url", "");
            }
            if (!r.id.empty())
                out.push_back(r);
        }
    } catch (const std::exception &e) {
        LOG(fmt::format("[OAuth] search parse error: {}", e.what()));
    }
    return out;
}

} // namespace panicast
