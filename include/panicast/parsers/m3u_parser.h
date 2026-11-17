// Y24.50: M3U playlist parser — parses #EXTM3U/#EXTINF playlists (iptv-org and generic .m3u)
//   into a list of channels (name, stream url, logo, group). Used by I-mode (IPTV).
#pragma once

#include <string>
#include <vector>

namespace panicast
{

struct IptvChannel {
    std::string name;  // channel title (text after the last ',' in #EXTINF)
    std::string url;   // stream URL (the line following #EXTINF)
    std::string logo;  // tvg-logo attribute (optional)
    std::string group; // group-title attribute (optional)
};

// Parse an .m3u / .m3u8 playlist body into channels. Lines starting with '#' are directives
//   (#EXTINF carries name/logo/group; others ignored). The first non-# non-empty line after an
//   #EXTINF is the stream URL. Returns empty on no/invalid content.
std::vector<IptvChannel> parse_m3u(const std::string &content);

} // namespace panicast
