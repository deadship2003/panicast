// Y23.4: TranscriptParser — thin facade over the subtitle/ PARSER architecture (Y24.7).
//   The per-format parsers (Json/Srt/Vtt/Lrc) + SubtitleParserRegistry now live in
//   subtitle/subtitle_parser.h. This facade keeps the historical TranscriptParser API stable so
//   existing callers (ui.h, app_playback.cpp) compile unchanged; it delegates to the registry.
#pragma once

#include <string>
#include <vector>

#include "panicast/subtitle/subtitle_parser.h" // TranscriptSegment + registry + find_at/find_active

namespace panicast
{

// Legacy facade. Prefer SubtitleParserRegistry / SubtitleManager for new code.
class TranscriptParser {
public:
    // Parse transcript content into timed segments. type_hint = "json"|"srt"|"vtt"|"lrc"|"" (auto).
    static std::vector<TranscriptSegment> parse(const std::string &content,
                                                const std::string &type_hint = "") {
        return SubtitleParserRegistry::instance().parse(content, type_hint);
    }

    // Fetch a transcript URL (or read a local path) and parse it. Returns empty on failure.
    static std::vector<TranscriptSegment> load(const std::string &url_or_path);

    static int find_at(const std::vector<TranscriptSegment> &segs, double time) {
        return panicast::find_at(segs, time);
    }
    static std::vector<int> find_active(const std::vector<TranscriptSegment> &segs, double time) {
        return panicast::find_active(segs, time);
    }

    static double parse_timestamp(const std::string &s) {
        return panicast::parse_timestamp(s);
    }
};

} // namespace panicast
