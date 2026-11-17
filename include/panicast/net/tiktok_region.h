// Y24.11: TikTok region helper — the `b` key cycles these in T mode.
//   TikTok is region-partitioned: the For You feed, trending, and (when available) search results
//   depend on the viewer's country. yt-dlp's --geo-bypass-country <CC> spoofs X-Forwarded-For to a
//   residential IP of <CC>, which TikTok honours for access/rate-limit decisions. A creator's OWN
//   posted videos are global (region only affects discovery + anti-bot), so switching region mainly
//   matters for reaching region-locked creators and dodging region-specific throttling.
//   Douyin (douyin.com) is the separate Chinese app — always CN, handled elsewhere (needs a CN exit).
#pragma once

#include <string>
#include <vector>

namespace panicast
{

struct TikTokRegion {
    // Curated 12 markets where TikTok operates (excludes IN/AF — banned — and CN — that's Douyin).
    static std::vector<std::string> all();
    // "US" -> "United States"; unknown code -> itself.
    static std::string name(const std::string &code);
    // Next code in the cycle after cur (wraps). If cur not in list, returns the first (US).
    static std::string next(const std::string &cur);
    // Y24.12: current region (set by App, read by UI to mirror ONLINE's [US] badge). Single source
    //   so UI::draw can show "🎵 TIKTOK [US]" without an App reference or draw() signature change.
    static const std::string &current();
    static void set_current(const std::string &code);
};

} // namespace panicast
