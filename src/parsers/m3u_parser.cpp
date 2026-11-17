// Y24.50: M3U playlist parser implementation.
#include "panicast/parsers/m3u_parser.h"

#include <regex>
#include <sstream>

namespace panicast
{

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<IptvChannel> parse_m3u(const std::string &content) {
    std::vector<IptvChannel> out;
    if (content.empty())
        return out;
    std::istringstream ss(content);
    std::string line;
    IptvChannel cur;
    bool have_ext = false;
    while (std::getline(ss, line)) {
        std::string l = trim(line);
        if (l.empty())
            continue;
        if (l[0] == '#') {
            if (l.rfind("#EXTINF", 0) == 0) {
                cur = IptvChannel{};
                std::smatch m;
                if (std::regex_search(l, m, std::regex("tvg-logo=\"([^\"]*)\"")))
                    cur.logo = m[1].str();
                if (std::regex_search(l, m, std::regex("group-title=\"([^\"]*)\"")))
                    cur.group = m[1].str();
                size_t comma = l.find_last_of(',');
                cur.name = (comma != std::string::npos) ? trim(l.substr(comma + 1)) : trim(l);
                have_ext = true;
            }
            // other directives (#EXTM3U, #EXTVLCOPT, ...) are ignored
        } else if (have_ext) {
            cur.url = l;
            if (cur.name.empty())
                cur.name = cur.url;
            out.push_back(cur);
            cur = IptvChannel{};
            have_ext = false;
        }
    }
    return out;
}

} // namespace panicast
