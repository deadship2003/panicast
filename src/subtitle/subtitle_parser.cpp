// Y24.7: Subtitle parser implementations — JSON (Podcasting 2.0 / Whisper) / SRT / VTT / LRC.
//   Each format is an ISubtitleParser subclass; SubtitleParserRegistry dispatches + auto-detects.
#include "panicast/subtitle/subtitle_parser.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include <nlohmann/json.hpp>

#include <fmt/format.h>

#include "panicast/core/logger.h"

namespace panicast
{
using json = nlohmann::json;

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

double parse_timestamp(const std::string &s) {
    // Accepts "HH:MM:SS,mmm" / "HH:MM:SS.mmm" / "MM:SS.mmm". Returns seconds.
    double h = 0, m = 0, sec = 0;
    std::string t = s;
    for (auto &c : t)
        if (c == ',')
            c = '.';
    size_t p1 = t.find(':');
    if (p1 == std::string::npos) {
        try {
            return std::stod(t);
        } catch (...) {
            return 0.0;
        }
    }
    size_t p2 = t.find(':', p1 + 1);
    try {
        if (p2 == std::string::npos) {
            m = std::stod(t.substr(0, p1));
            sec = std::stod(t.substr(p1 + 1));
        } else {
            h = std::stod(t.substr(0, p1));
            m = std::stod(t.substr(p1 + 1, p2 - p1 - 1));
            sec = std::stod(t.substr(p2 + 1));
        }
    } catch (...) {
        return 0.0;
    }
    return h * 3600 + m * 60 + sec;
}

// ── JSON (Podcasting 2.0 {"segments":[...]} / bare array / Whisper {"segments":[{start,end,text}]}) ──
std::vector<TranscriptSegment> JsonSubtitleParser::parse(const std::string &content) const {
    std::vector<TranscriptSegment> out;
    try {
        json j = json::parse(content);
        const json *segs = nullptr;
        if (j.is_object() && j.contains("segments") && j["segments"].is_array())
            segs = &j["segments"];
        else if (j.is_array())
            segs = &j; // bare array of segments
        if (!segs)
            return out;
        for (const auto &s : *segs) {
            if (!s.is_object())
                continue;
            TranscriptSegment seg;
            // Podcasting 2.0 uses startTime/endTime; Whisper uses start/end (often as strings).
            seg.start = s.value("startTime", s.value("start", 0.0));
            seg.end = s.value("endTime", s.value("end", 0.0));
            seg.text = trim(s.value("body", s.value("text", "")));
            seg.speaker = s.value("speaker", "");
            // Tolerate no/invalid endTime (common in Podcasting 2.0 JSON) — clamp to start so the
            //   line stays until the next cue, instead of dropping the segment (gaps → wrong highlight).
            if (seg.end < seg.start)
                seg.end = seg.start;
            if (!seg.text.empty())
                out.push_back(seg);
        }
        std::sort(out.begin(), out.end(),
                  [](const TranscriptSegment &a, const TranscriptSegment &b) {
                      return a.start < b.start;
                  });
    } catch (const std::exception &e) {
        LOG(fmt::format("[Subtitle] JSON parse error: {}", e.what()));
    }
    return out;
}

// Parse a "HH:MM:SS,mmm --> HH:MM:SS,mmm" cue header line → start/end. Returns false if not a cue.
static bool parse_cue_times(const std::string &line, double &start, double &end) {
    size_t arrow = line.find("-->");
    if (arrow == std::string::npos)
        return false;
    std::string a = trim(line.substr(0, arrow));
    std::string b = trim(line.substr(arrow + 3));
    size_t sp = b.find_first_of(" \t");
    if (sp != std::string::npos)
        b = b.substr(0, sp);
    start = parse_timestamp(a);
    end = parse_timestamp(b);
    return true;
}

std::vector<TranscriptSegment> SrtSubtitleParser::parse(const std::string &content) const {
    std::vector<TranscriptSegment> out;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        double start = 0, end = 0;
        if (!parse_cue_times(line, start, end))
            continue;
        std::string body, l;
        while (std::getline(ss, l)) {
            std::string t = trim(l);
            if (t.empty())
                break;
            if (!body.empty())
                body += "\n";
            body += t;
        }
        TranscriptSegment seg;
        seg.start = start;
        seg.end = end;
        seg.text = trim(body);
        if (!seg.text.empty())
            out.push_back(seg);
    }
    return out;
}

std::vector<TranscriptSegment> VttSubtitleParser::parse(const std::string &content) const {
    // VTT is cue-format like SRT (minus the numeric cue index, plus "WEBVTT" header).
    return SrtSubtitleParser{}.parse(content);
}

// Parse an "[mm:ss.xx]" / "[mm:ss]" / "[ss.xx]" tag → seconds, or -1 if it's a metadata tag.
static double parse_lrc_tag(const std::string &s) {
    size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0)
        return -1;
    std::string before = s.substr(0, colon);
    for (char c : before)
        if (!std::isdigit((unsigned char)c))
            return -1; // metadata (ti/ar/...)
    try {
        double mm = std::stod(before);
        double ss = std::stod(s.substr(colon + 1));
        return mm * 60.0 + ss;
    } catch (...) {
        return -1;
    }
}

std::vector<TranscriptSegment> LrcSubtitleParser::parse(const std::string &content) const {
    std::vector<TranscriptSegment> out;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        std::vector<double> stamps;
        size_t pos = 0;
        while (pos < line.size() && line[pos] == '[') {
            size_t close = line.find(']', pos);
            if (close == std::string::npos)
                break;
            double t = parse_lrc_tag(line.substr(pos + 1, close - pos - 1));
            if (t < 0)
                break; // metadata tag → not a lyric timestamp
            stamps.push_back(t);
            pos = close + 1;
        }
        if (stamps.empty())
            continue;
        std::string text = trim(line.substr(pos));
        if (text.empty())
            continue;
        for (double t : stamps) {
            TranscriptSegment seg;
            seg.start = t;
            seg.end = t; // LRC has no end; line stays until the next cue
            seg.text = text;
            out.push_back(seg);
        }
    }
    std::sort(out.begin(), out.end(), [](const TranscriptSegment &a, const TranscriptSegment &b) {
        return a.start < b.start;
    });
    return out;
}

// Y24.9: a line that is exactly "HH:MM:SS" / "H:MM:SS" (digits + 2 colons, no fraction, no "-->").
//   Used to detect omny TextWithTimestamps and to parse it.
static bool is_hms_only(const std::string &s) {
    int colons = 0;
    size_t digits = 0;
    for (char c : s) {
        if (c == ':') {
            if (++colons > 2)
                return false;
        } else if (std::isdigit((unsigned char)c))
            ++digits;
        else
            return false;
    }
    return colons == 2 && digits >= 5;
}

std::vector<TranscriptSegment> TextWithTimestampsParser::parse(const std::string &content) const {
    std::vector<TranscriptSegment> out;
    std::istringstream ss(content);
    std::string line;
    TranscriptSegment cur;
    bool in_seg = false;
    auto flush = [&]() {
        if (in_seg) {
            cur.text = trim(cur.text);
            if (!cur.text.empty()) {
                if (cur.end < cur.start)
                    cur.end = cur.start;
                out.push_back(cur);
            }
            cur = TranscriptSegment{};
            in_seg = false;
        }
    };
    while (std::getline(ss, line)) {
        std::string t = trim(line);
        if (is_hms_only(t)) { // a timestamp line starts a new segment
            flush();
            cur.start = parse_timestamp(t);
            cur.end = cur.start;
            in_seg = true;
        } else if (in_seg) {
            if (!cur.text.empty())
                cur.text += "\n";
            cur.text += t;
        }
    }
    flush();
    // end = next segment's start (the line stays until the next cue, like LRC)
    for (size_t i = 0; i + 1 < out.size(); ++i)
        out[i].end = out[i + 1].start;
    return out;
}

// ── Registry ──
SubtitleParserRegistry::SubtitleParserRegistry() {
    // Register the built-in format parsers. A new format = add one line here.
    // (Stored by name for dispatch; the registry owns them.)
    // NOTE: kept simple (4 known formats) — the ISubtitleParser interface + registry is the
    //   extension point, mirroring IFeedParser/ParserRegistry.
}

SubtitleParserRegistry &SubtitleParserRegistry::instance() {
    static SubtitleParserRegistry r;
    return r;
}

std::string SubtitleParserRegistry::hint_for_url(const std::string &url_or_path) {
    auto ends_with = [](const std::string &s, const char *suf) {
        size_t n = 0;
        while (suf[n])
            ++n;
        if (s.size() < n)
            return false;
        for (size_t i = 0; i < n; ++i)
            if ((char)std::tolower((unsigned char)s[s.size() - n + i]) !=
                (char)std::tolower((unsigned char)suf[i]))
                return false;
        return true;
    };
    if (ends_with(url_or_path, ".lrc"))
        return "lrc";
    if (ends_with(url_or_path, ".json"))
        return "json";
    if (ends_with(url_or_path, ".vtt"))
        return "vtt";
    if (ends_with(url_or_path, ".srt"))
        return "srt";
    return "";
}

std::vector<TranscriptSegment> SubtitleParserRegistry::parse(const std::string &content,
                                                             const std::string &type_hint) const {
    if (content.empty())
        return {};
    std::string t = type_hint;
    if (t.empty()) {
        // Auto-detect by content: JSON '{' or '[' (pretty or compact); VTT "WEBVTT"; LRC "[mm:ss]";
        //   else SRT.
        size_t first = content.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            char c = content[first];
            if (c == '{')
                t = "json";
            else if (content.compare(first, 6, "WEBVTT") == 0)
                t = "vtt";
            else if (c == '[') {
                // Distinguish JSON array "[{...}]" / "[\"...\"]" from LRC "[tag]text": JSON's first
                //   non-whitespace char after '[' is '{' or '"'. Skip whitespace (pretty-printed JSON).
                size_t j = first + 1;
                while (j < content.size() && (content[j] == ' ' || content[j] == '\t' ||
                                              content[j] == '\r' || content[j] == '\n'))
                    ++j;
                char next = (j < content.size()) ? content[j] : '\0';
                t = (next == '{' || next == '"') ? "json" : "lrc";
            } else {
                // Scan up to a few lines for "[mm:ss" (LRC) or an "HH:MM:SS"-only line (TextWithTimestamps).
                t = "srt";
                std::istringstream scan(content);
                std::string l;
                int scanned = 0;
                while (std::getline(scan, l) && scanned < 20) {
                    ++scanned;
                    std::string ll = trim(l);
                    size_t lb = ll.find_first_not_of(" \t\r\n");
                    if (lb != std::string::npos && lb + 1 < ll.size() && ll[lb] == '[') {
                        size_t cl = ll.find(']', lb);
                        if (cl != std::string::npos &&
                            parse_lrc_tag(ll.substr(lb + 1, cl - lb - 1)) >= 0) {
                            t = "lrc";
                            break;
                        }
                    }
                    if (is_hms_only(ll)) {
                        t = "twt";
                        break;
                    } // Y24.9: omny TextWithTimestamps
                }
            }
        }
    }
    if (t == "json")
        return JsonSubtitleParser{}.parse(content);
    if (t == "vtt")
        return VttSubtitleParser{}.parse(content);
    if (t == "lrc")
        return LrcSubtitleParser{}.parse(content);
    if (t == "twt")
        return TextWithTimestampsParser{}.parse(content); // Y24.9
    return SrtSubtitleParser{}.parse(content);            // srt default
}

// ── Timed lookups ──
int find_at(const std::vector<TranscriptSegment> &segs, double time) {
    if (segs.empty() || time < 0)
        return -1;
    int lo = 0, hi = (int)segs.size() - 1, ans = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (segs[mid].start <= time) {
            ans = mid;
            lo = mid + 1;
        } else
            hi = mid - 1;
    }
    return ans; // line stays until the next cue
}

std::vector<int> find_active(const std::vector<TranscriptSegment> &segs, double time) {
    std::vector<int> out;
    if (segs.empty() || time < 0)
        return out;
    int n = (int)segs.size();
    for (int i = 0; i < n; ++i) {
        if (segs[i].start > time)
            break; // sorted → no later segment can be active
        double eff_end =
            (segs[i].end > segs[i].start) ? segs[i].end : (i + 1 < n ? segs[i + 1].start : 1e18);
        if (time < eff_end)
            out.push_back(i);
    }
    return out;
}

} // namespace panicast
