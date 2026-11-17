// Y24.7: TranscriptParser facade — only load() (fetch + parse via the registry) remains here.
//   All per-format parsing moved to src/subtitle/subtitle_parser.cpp.
#include "panicast/parsers/transcript_parser.h"

#include <fstream>

#include <fmt/format.h>

#include "panicast/core/logger.h"
#include "panicast/net/network.h"

namespace panicast
{

std::vector<TranscriptSegment> TranscriptParser::load(const std::string &url_or_path) {
    std::string content;
    if (url_or_path.empty())
        return {};
    if (url_or_path.rfind("http", 0) == 0) {
        LOG(fmt::format("[Subtitle] fetching online: {}", url_or_path));
        content = Network::fetch(url_or_path);
        if (content.empty()) {
            LOG(fmt::format("[Subtitle] online fetch returned empty: {}", url_or_path));
            return {};
        }
    } else {
        std::ifstream f(url_or_path, std::ios::binary);
        if (!f) {
            LOG(fmt::format("[Subtitle] file not found: {}", url_or_path));
            return {};
        }
        content.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    if (content.empty()) {
        LOG(fmt::format("[Subtitle] empty: {}", url_or_path));
        return {};
    }
    std::string hint = SubtitleParserRegistry::hint_for_url(url_or_path);
    auto segs = SubtitleParserRegistry::instance().parse(content, hint);
    LOG(fmt::format("[Subtitle] loaded {} segments ({} bytes, type={}) from {}", segs.size(),
                    content.size(), hint.empty() ? "auto" : hint, url_or_path));
    return segs;
}

} // namespace panicast
