// Remote-admin surface for the LMS plane (META-6): login flows + favourites +
//   TikTok keyword search, all drivable from Squeezer / the client TUI without a
//   terminal popup. The TUI popups stay untouched; the post-auth "finish" bodies
//   are shared with them where practical.
#include "panicast/app/app.h"

#include <fmt/format.h>

#include <chrono>
#include <string>
#include <thread>

#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/net/bilibili_api.h"
#include "panicast/net/google_oauth.h"
#include "panicast/storage/accounts.h"

namespace panicast
{

// ── Login: start + background poll ────────────────────────────────────────────
//   Returns the authorization URL immediately; completion runs on the pool and
//   refreshes the account tree (browse_sig moves → the remote list updates).

bool App::start_remote_login(const std::string &m, std::string &url_out, std::string &code_out,
                             std::string &err_out) {
    err_out.clear();
    url_out.clear();
    code_out.clear();

    if (m == "youtube") {
        EVENT_LOG("Y: requesting Google device code (remote)...");
        auto dc = GoogleOAuth::request_device_code();
        if (!dc.ok) {
            err_out = dc.error.empty() ? "network error" : dc.error;
            return false;
        }
        url_out = dc.verification_url;
        code_out = dc.user_code;
        pool_.submit([this, dc]() {
            // Device-flow poll (no popup): typical interval 5s, generous 15 min cap.
            int interval = 5;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(900);
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::seconds(interval));
                auto tr = GoogleOAuth::poll_token(dc.device_code);
                if (tr.ok) {
                    EVENT_LOG("Y: remote login authorized; syncing...");
                    finish_google_login(tr);
                    return;
                }
                if (tr.error == "slow_down")
                    interval += 5;
                else if (tr.error != "authorization_pending") {
                    LOG(fmt::format("[Y] remote token poll error: {}", tr.error));
                    return;
                }
            }
            LOG("[Y] remote login timed out");
        });
        return true;
    }

    if (m == "bilibili") {
        EVENT_LOG("B: requesting Bilibili QR (remote)...");
        auto qr = BilibiliAPI::request_qrcode();
        if (!qr.ok) {
            err_out = qr.error.empty() ? "network error" : qr.error;
            return false;
        }
        url_out = qr.url;
        pool_.submit([this, key = qr.qrcode_key]() {
            for (int i = 0; i < 180; ++i) { // ~3 min, 1s cadence
                std::this_thread::sleep_for(std::chrono::seconds(1));
                auto login = BilibiliAPI::poll_qrcode(key);
                if (login.ok) {
                    finish_bilibili_login(login);
                    return;
                }
                if (login.code != 86101 && login.code != 86090 && !login.error.empty()) {
                    LOG(fmt::format("[B] remote poll error: {}", login.error));
                    return;
                }
            }
            LOG("[B] remote login timed out");
        });
        return true;
    }

    if (m == "tiktok") {
        // Reserved per user decision: Douyin QR passport endpoints need their own
        //   signing/verify chain — the slot stays visible, the flow lands later.
        err_out = "reserved";
        return false;
    }

    err_out = "unknown mode";
    return false;
}

// Shared post-auth bodies (extracted verbatim from the TUI popup flows so both
//   paths converge on identical account handling).

void App::finish_google_login(const GoogleOAuth::TokenResult &tr) {
    std::string channel_id, label;
    auto id = GoogleOAuth::fetch_identity(tr.access_token);
    if (id.ok) {
        channel_id = id.channel_id;
        label = id.title;
    }
    int64_t expires_at = tr.obtained_at + tr.expires_in;
    int aid = 0;
    if (!channel_id.empty()) {
        for (const auto &a : AccountsManager::instance().list_accounts()) {
            if (!a.channel_id.empty() && a.channel_id == channel_id) {
                aid = a.account_id;
                break;
            }
        }
    }
    if (aid > 0) {
        AccountsManager::instance().update_tokens(aid, tr.access_token, tr.refresh_token,
                                                  expires_at, tr.scope);
        AccountsManager::instance().touch_login(aid);
        AccountsManager::instance().set_active_account(aid);
        EVENT_LOG(fmt::format("Y: account #{} refreshed ({}); syncing...", aid, label));
    } else {
        aid = AccountsManager::instance().add_account(/*email*/ "", /*gaia*/ "", channel_id,
                                                      tr.access_token, tr.refresh_token,
                                                      expires_at, tr.scope, label);
        if (aid <= 0) {
            LOG("[Y] login failed: could not save account");
            return;
        }
        AccountsManager::instance().touch_login(aid);
        AccountsManager::instance().set_active_account(aid);
        EVENT_LOG(fmt::format("Y: account #{} logged in ({}); syncing...", aid, label));
    }
    sync_account_subscriptions(aid);
    sync_account_history(aid);
    library_.load_accounts_root();
}

void App::finish_bilibili_login(const BilibiliAPI::LoginResult &login) {
    auto nav = BilibiliAPI::fetch_nav(login.sessdata);
    BilibiliAccount acc;
    acc.sessdata = login.sessdata;
    acc.bili_jct = login.bili_jct;
    acc.dedeuserid = login.dedeuserid;
    acc.uid = nav.ok ? nav.uid : login.dedeuserid;
    acc.uname = nav.ok ? nav.uname : ("Bili #" + acc.uid);
    int aid = panicast::save_bilibili_account(acc);
    write_bilibili_cookies(acc);
    EVENT_LOG(fmt::format("B: account #{} logged in ({})", aid, acc.uname));
    library_.load_bilibili_root();
}

} // namespace panicast
