#include "panicast/net/tiktok_region.h"

#include <algorithm>

namespace panicast
{

std::vector<std::string> TikTokRegion::all() {
    // Y24.13: CN = Douyin (the Chinese app, douyin.com). Selecting CN makes T-mode target the
    //   douyin domain; the other 12 are TikTok regions (tiktok.com). One unified T mode → global.
    return {"US", "JP", "GB", "DE", "FR", "KR", "ID", "TH", "VN", "MY", "BR", "MX", "CN"};
}

std::string TikTokRegion::name(const std::string &code) {
    if (code == "US")
        return "United States";
    if (code == "JP")
        return "Japan";
    if (code == "GB")
        return "United Kingdom";
    if (code == "DE")
        return "Germany";
    if (code == "FR")
        return "France";
    if (code == "KR")
        return "South Korea";
    if (code == "ID")
        return "Indonesia";
    if (code == "TH")
        return "Thailand";
    if (code == "VN")
        return "Vietnam";
    if (code == "MY")
        return "Malaysia";
    if (code == "BR")
        return "Brazil";
    if (code == "MX")
        return "Mexico";
    if (code == "CN")
        return "China (Douyin)";
    return code;
}

std::string TikTokRegion::next(const std::string &cur) {
    auto list = all();
    auto it = std::find(list.begin(), list.end(), cur);
    if (it == list.end() || it + 1 == list.end())
        return list.front();
    return *(it + 1);
}

// Y24.12: current-region singleton (UI reads this; App sets it on init + 'b' cycle).
static std::string g_current_region = "US";
const std::string &TikTokRegion::current() {
    return g_current_region;
}
void TikTokRegion::set_current(const std::string &code) {
    g_current_region = code;
}

} // namespace panicast
