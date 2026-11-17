// Y24.7: Subtitle parser — PARSER-architecture (mirrors IFeedParser/ParserRegistry).
//   ISubtitleParser is the unified per-format parser interface; SubtitleParserRegistry dispatches
//   by type hint ("json"|"srt"|"vtt"|"lrc") or auto-detects by content/extension. Adding a new
//   subtitle format = add one ISubtitleParser subclass + register it in the registry ctor.
#pragma once

#include <string>
#include <vector>

namespace panicast
{

// One timed lyric/transcript line. start/end in seconds; speaker optional (JSON transcripts).
struct TranscriptSegment {
    double start = 0.0; // seconds
    double end = 0.0;   // seconds (== start for LRC / no-end JSON → line stays until next cue)
    std::string text;
    std::string speaker;
};

// Parse a "HH:MM:SS,mmm" / "HH:MM:SS.mmm" / "MM:SS.mmm" timestamp → seconds (shared by cue parsers).
double parse_timestamp(const std::string &s);

// Unified subtitle parser interface. Each subclass parses one format.
class ISubtitleParser {
public:
    virtual ~ISubtitleParser() = default;
    virtual const char *name() const = 0; // "json" | "srt" | "vtt" | "lrc"
    virtual std::vector<TranscriptSegment> parse(const std::string &content) const = 0;
};

class JsonSubtitleParser : public ISubtitleParser {
public:
    const char *name() const override {
        return "json";
    }
    std::vector<TranscriptSegment> parse(const std::string &content) const override;
};

class SrtSubtitleParser : public ISubtitleParser {
public:
    const char *name() const override {
        return "srt";
    }
    std::vector<TranscriptSegment> parse(const std::string &content) const override;
};

class VttSubtitleParser : public ISubtitleParser {
public:
    const char *name() const override {
        return "vtt";
    }
    std::vector<TranscriptSegment> parse(const std::string &content) const override;
};

class LrcSubtitleParser : public ISubtitleParser {
public:
    const char *name() const override {
        return "lrc";
    }
    std::vector<TranscriptSegment> parse(const std::string &content) const override;
};

// Y24.9: omny.fm "TextWithTimestamps" — "HH:MM:SS" alone on a line, then "Speaker N: text" (possibly
//   multi-line) until the next timestamp line. Fallback for feeds that provide no SRT/VTT/JSON.
class TextWithTimestampsParser : public ISubtitleParser {
public:
    const char *name() const override {
        return "twt";
    }
    std::vector<TranscriptSegment> parse(const std::string &content) const override;
};

// Registry (Meyers singleton). parse() dispatches by type_hint, auto-detecting when empty.
//   hint_for_url() infers a hint from a file/URL extension (.json/.srt/.vtt/.lrc).
class SubtitleParserRegistry {
    SubtitleParserRegistry();

public:
    static SubtitleParserRegistry &instance();
    // Parse with an explicit type hint; empty hint → auto-detect by content.
    std::vector<TranscriptSegment> parse(const std::string &content,
                                         const std::string &type_hint) const;
    // Infer a type hint from a URL/path extension ("" if unknown).
    static std::string hint_for_url(const std::string &url_or_path);
};

// Timed-segment lookups (segs must be sorted by start).
//   find_at: index of the last segment with start <= time (line stays until next cue), or -1.
//   find_active: ALL segments active at time (overlapping speakers); for no-end segments the line
//   stays until the next segment's start. Sequential/LRC transcripts → exactly one.
int find_at(const std::vector<TranscriptSegment> &segs, double time);
std::vector<int> find_active(const std::vector<TranscriptSegment> &segs, double time);

} // namespace panicast
