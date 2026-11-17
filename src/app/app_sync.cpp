// Y01: YouTube sync — subscriptions (bidirectional via Data API v3) + watch history
// (pull via InnerTube /browse; local plays recorded locally). See CHANGELOG Y01 known limits.
#include "panicast/app/app.h"

#include <fmt/format.h>

#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"

namespace panicast
{

// Pull the account's YouTube subscriptions and replace the local cache.
// (Subscribe/unsubscribe push happens elsewhere — when the user adds/removes a YouTube channel
//  subscription in P mode under an active account; Y01 wires the pull here, push is available
//  via GoogleOAuth::subscribe/unsubscribe for Y02 to bind to the add/delete actions.)
void App::sync_account_subscriptions(int account_id) {
    if (account_id <= 0)
        return;
    GoogleAccount acc;
    if (!AccountsManager::instance().get_tokens(account_id, acc)) {
        EVENT_LOG(fmt::format("Y: sync subs #{} — no valid token", account_id));
        return;
    }
    EVENT_LOG(fmt::format("Y: pulling subscriptions for #{} ...", account_id));
    auto subs = GoogleOAuth::fetch_subscriptions(acc.access_token);
    AccountsManager::instance().replace_subscriptions(account_id, subs);
    AccountsManager::instance().set_sync_state(account_id, "subscriptions");
    AccountsManager::instance().touch_sync(account_id);
    EVENT_LOG(fmt::format("Y: #{} subscriptions synced: {}", account_id, subs.size()));
}

// Pull the account's YouTube watch history (InnerTube /browse) into youtube_history.
void App::sync_account_history(int account_id) {
    if (account_id <= 0)
        return;
    GoogleAccount acc;
    if (!AccountsManager::instance().get_tokens(account_id, acc))
        return;
    EVENT_LOG(fmt::format("Y: pulling watch history for #{} ...", account_id));
    auto rows = GoogleOAuth::fetch_watch_history(acc.access_token);
    for (const auto &r : rows) {
        AccountsManager::instance().upsert_history(account_id, r);
    }
    AccountsManager::instance().set_sync_state(account_id, "watch_history");
    AccountsManager::instance().touch_sync(account_id);
    EVENT_LOG(fmt::format("Y: #{} watch-history rows pulled: {}", account_id, rows.size()));
}

// Record a YouTube play under the active account (local side — YouTube has no "mark watched" API).
void App::record_youtube_play(const std::string &video_id, const std::string &title,
                              const std::string &channel_name) {
    if (video_id.empty())
        return;
    int aid = AccountsManager::instance().active_account_id();
    if (aid <= 0)
        return;
    YouTubeHistoryRow r;
    r.video_id = video_id;
    r.title = title;
    r.channel_name = channel_name;
    r.source = "local";
    AccountsManager::instance().upsert_history(aid, r);
}

} // namespace panicast
