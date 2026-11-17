// Text utilities: UTF-8 width, truncate/wrap/scroll, format_duration, http_to_https, url_encode, to_lower.
// Y24.38: split out of utils.cpp. Methods remain Utils:: static members
//   (declarations stay in utils.h); only implementations live here.
#include "panicast/core/utils.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring> // std::string
#include <ctime>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sstream>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#include <fmt/format.h>

#include "panicast/core/paths.h"
#include "panicast/core/terminal.h"

extern char **environ; // Required by posix_spawnp

namespace panicast
{

std::string Utils::to_lower(const std::string &s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

// HTTP to HTTPS - security first; prefer https over http whenever possible
std::string Utils::http_to_https(const std::string &url) {
    // P3-F1: case-insensitive "http://" check; never blindly upgrade local addresses (breaks local
    //   streams served over plain http on localhost/loopback/lan).
    if (url.size() > 7 &&
        (url.compare(0, 7, "http://") == 0 || url.compare(0, 7, "HTTP://") == 0)) {
        std::string host = url.substr(7);
        size_t slash = host.find('/');
        if (slash != std::string::npos)
            host = host.substr(0, slash);
        size_t colon = host.find(':');
        if (colon != std::string::npos)
            host = host.substr(0, colon);
        // case-insensitive local-host check
        std::string h = host;
        std::transform(h.begin(), h.end(), h.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (h == "localhost" || h == "127.0.0.1" || h == "::1" || h == "0.0.0.0" ||
            h.rfind("127.", 0) == 0 || h.rfind("192.168.", 0) == 0 || h.rfind("10.", 0) == 0 ||
            h.rfind("172.16.", 0) == 0) {
            return url; // leave local http:// as-is
        }
        return "https://" + url.substr(7);
    }
    return url;
}

// URL encoding function
std::string Utils::url_encode(const std::string &s) {
    std::string result;
    result.reserve(s.size() * 3);
    for (char c : s) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            result += buf;
        }
    }
    return result;
}

// Y24.27: has_gui/is_ssh_session/has_local_display/is_wsl/has_usable_display removed (dead code).

int Utils::utf8_char_bytes(unsigned char c) {
    if ((c & 0x80) == 0)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

// =========================================================
// UTF-8 character display width auto-detection - mk_wcwidth algorithm
// Based on the Unicode 15.1 standard, auto-detects the width of all characters
// Replaces hardcoded Unicode ranges, supports any new character
// =========================================================

// mk_wcwidth: returns the display width for a given Unicode codepoint
// Return value: -1 (control character), 0 (invisible / combining character), 1 (halfwidth), 2 (fullwidth)
int Utils::mk_wcwidth(uint32_t ucs) {
    // NULL and control characters
    if (ucs == 0)
        return 0;
    if (ucs < 32 || (ucs >= 0x7F && ucs < 0xA0))
        return -1;

    // ===== Width=0 characters (Non-spacing, Enclosing, Format) =====
    // Combining Diacritical Marks
    if (ucs >= 0x0300 && ucs <= 0x036F)
        return 0;
    // Combining Diacritical Marks Extended
    if (ucs >= 0x1AB0 && ucs <= 0x1AFF)
        return 0;
    // Combining Diacritical Marks Supplement
    if (ucs >= 0x1DC0 && ucs <= 0x1DFF)
        return 0;
    // Combining Diacritical Marks for Symbols
    if (ucs >= 0x20D0 && ucs <= 0x20FF)
        return 0;
    // Combining Half Marks
    if (ucs >= 0xFE20 && ucs <= 0xFE2F)
        return 0;

    // Cyrillic combining marks
    if (ucs >= 0x0483 && ucs <= 0x0489)
        return 0;

    // Arabic combining marks
    if (ucs >= 0x064B && ucs <= 0x065F)
        return 0;
    if (ucs >= 0x0670 && ucs <= 0x0670)
        return 0;
    if (ucs >= 0x06D6 && ucs <= 0x06DC)
        return 0;
    if (ucs >= 0x06DF && ucs <= 0x06E4)
        return 0;
    if (ucs >= 0x06E7 && ucs <= 0x06E8)
        return 0;
    if (ucs >= 0x06EA && ucs <= 0x06ED)
        return 0;

    // Devanagari combining
    if (ucs >= 0x0900 && ucs <= 0x0903)
        return 0;
    if (ucs >= 0x093A && ucs <= 0x093B)
        return 0;
    if (ucs >= 0x093C && ucs <= 0x093C)
        return 0;
    if (ucs >= 0x093E && ucs <= 0x094F)
        return 0;
    if (ucs >= 0x0951 && ucs <= 0x0957)
        return 0;
    if (ucs >= 0x0962 && ucs <= 0x0963)
        return 0;

    // Hebrew combining
    if (ucs >= 0x0591 && ucs <= 0x05BD)
        return 0;
    if (ucs >= 0x05BF && ucs <= 0x05BF)
        return 0;
    if (ucs >= 0x05C1 && ucs <= 0x05C2)
        return 0;
    if (ucs >= 0x05C4 && ucs <= 0x05C5)
        return 0;
    if (ucs >= 0x05C7 && ucs <= 0x05C7)
        return 0;

    // Thai combining
    if (ucs >= 0x0E31 && ucs <= 0x0E31)
        return 0;
    if (ucs >= 0x0E34 && ucs <= 0x0E3A)
        return 0;
    if (ucs >= 0x0E47 && ucs <= 0x0E4E)
        return 0;

    // Zero-width characters
    if (ucs == 0x200B)
        return 0; // Zero Width Space
    if (ucs == 0x200C)
        return 0; // Zero Width Non-Joiner
    if (ucs == 0x200D)
        return 0; // Zero Width Joiner
    if (ucs == 0x200E)
        return 0; // Left-to-Right Mark
    if (ucs == 0x200F)
        return 0; // Right-to-Left Mark

    // Bidirectional formatting
    if (ucs >= 0x202A && ucs <= 0x202E)
        return 0;
    if (ucs >= 0x2060 && ucs <= 0x2063)
        return 0;
    if (ucs >= 0x2066 && ucs <= 0x2069)
        return 0;
    if (ucs >= 0x206A && ucs <= 0x206F)
        return 0;

    // Variation Selectors
    if (ucs >= 0xFE00 && ucs <= 0xFE0F)
        return 0;
    if (ucs >= 0xE0100 && ucs <= 0xE01EF)
        return 0;

    // Other format characters
    if (ucs == 0x061C)
        return 0; // Arabic Letter Mark
    if (ucs == 0x034F)
        return 0; // Combining Grapheme Joiner
    if (ucs == 0x115F)
        return 0; // Hangul Choseong Filler
    if (ucs == 0x1160)
        return 0; // Hangul Jungseong Filler
    if (ucs >= 0x17B4 && ucs <= 0x17B5)
        return 0; // Khmer vowels
    if (ucs >= 0x180B && ucs <= 0x180D)
        return 0; // Mongolian FVS
    if (ucs >= 0x180F && ucs <= 0x180F)
        return 0;
    if (ucs >= 0xFEFF && ucs <= 0xFEFF)
        return 0; // BOM/ZWNBSP

    // ===== Width=2 characters (East Asian Wide/F) =====

    // Hangul Jamo
    if (ucs >= 0x1100 && ucs <= 0x115F)
        return 2;
    if (ucs >= 0x1161 && ucs <= 0x11FF)
        return 2;
    if (ucs >= 0xA960 && ucs <= 0xA97C)
        return 2; // Hangul Jamo Extended-A
    if (ucs >= 0xD7B0 && ucs <= 0xD7C6)
        return 2; // Hangul Jamo Extended-B
    if (ucs >= 0xD7CB && ucs <= 0xD7FB)
        return 2; // Hangul Jamo Extended-B

    // CJK Radicals and Symbols
    if (ucs >= 0x2E80 && ucs <= 0x2EF3)
        return 2; // CJK Radicals Supplement
    if (ucs >= 0x2F00 && ucs <= 0x2FD5)
        return 2; // Kangxi Radicals
    if (ucs >= 0x2FF0 && ucs <= 0x2FFB)
        return 2; // Ideographic Description

    // CJK Symbols and Punctuation
    if (ucs >= 0x3000 && ucs <= 0x303E)
        return 2;
    if (ucs >= 0x3001 && ucs <= 0x3003)
        return 2;
    if (ucs == 0x303F)
        return 1; // Halfwidth space
    if (ucs >= 0x301C && ucs <= 0x301D)
        return 2;

    // Hiragana
    if (ucs >= 0x3040 && ucs <= 0x309F)
        return 2;

    // Katakana
    if (ucs >= 0x30A0 && ucs <= 0x30FF)
        return 2;

    // Bopomofo
    if (ucs >= 0x3100 && ucs <= 0x312F)
        return 2;
    if (ucs >= 0x31A0 && ucs <= 0x31BF)
        return 2; // Bopomofo Extended

    // CJK Strokes
    if (ucs >= 0x31C0 && ucs <= 0x31E3)
        return 2;

    // CJK Unified Ideographs
    if (ucs >= 0x3400 && ucs <= 0x4DBF)
        return 2; // Extension A
    if (ucs >= 0x4E00 && ucs <= 0x9FFF)
        return 2; // Main block
    if (ucs >= 0xA000 && ucs <= 0xA48C)
        return 2; // Yi Syllables
    if (ucs >= 0xA490 && ucs <= 0xA4C6)
        return 2; // Yi Radicals

    // Hangul Syllables
    if (ucs >= 0xAC00 && ucs <= 0xD7A3)
        return 2;

    // CJK Compatibility Ideographs
    if (ucs >= 0xF900 && ucs <= 0xFAFF)
        return 2;

    // Vertical Forms
    if (ucs >= 0xFE10 && ucs <= 0xFE19)
        return 2;
    if (ucs >= 0xFE30 && ucs <= 0xFE52)
        return 2; // CJK Compatibility Forms
    if (ucs >= 0xFE54 && ucs <= 0xFE66)
        return 2;
    if (ucs >= 0xFE68 && ucs <= 0xFE6B)
        return 2;

    // Fullwidth Forms
    if (ucs >= 0xFF01 && ucs <= 0xFF60)
        return 2; // Fullwidth ASCII
    if (ucs >= 0xFFE0 && ucs <= 0xFFE6)
        return 2; // Fullwidth Symbols

    // Angle brackets (Wide)
    if (ucs == 0x2329 || ucs == 0x232A)
        return 2;
    if (ucs == 0x3008 || ucs == 0x3009)
        return 2; // CJK angle brackets

    // Currency/Greek and other East Asian Ambiguous characters: terminal-aware width
    //   xterm/Linux renders them as 2 columns, Windows Terminal renders them as 1 column
    {
        bool narrow = TerminalDetector::narrow_ambiguous();
        if (ucs == 0x00A2 || ucs == 0x00A3 || ucs == 0x00A5 || ucs == 0x00AC || ucs == 0x20A9)
            return narrow ? 1 : 2;
        if (ucs >= 0x0391 && ucs <= 0x03A9)
            return narrow ? 1 : 2; // Greek Capital
        if (ucs >= 0x03B1 && ucs <= 0x03C9)
            return narrow ? 1 : 2; // Greek Small
        if (ucs >= 0x03D0 && ucs <= 0x03F5)
            return narrow ? 1 : 2; // Greek Extended
    }
    if (ucs == 0x00A6)
        return 1; // Broken bar (narrow)

    // CJK Extensions B-G (Supplementary Planes)
    if (ucs >= 0x20000 && ucs <= 0x2A6DF)
        return 2; // Extension B
    if (ucs >= 0x2A700 && ucs <= 0x2B73F)
        return 2; // Extension C
    if (ucs >= 0x2B740 && ucs <= 0x2B81F)
        return 2; // Extension D
    if (ucs >= 0x2B820 && ucs <= 0x2CEAF)
        return 2; // Extension E
    if (ucs >= 0x2CEB0 && ucs <= 0x2EBEF)
        return 2; // Extension F
    if (ucs >= 0x2F800 && ucs <= 0x2FA1F)
        return 2; // Compatibility Supplement
    if (ucs >= 0x30000 && ucs <= 0x3134F)
        return 2; // Extension G
    if (ucs >= 0x31350 && ucs <= 0x323AF)
        return 2; // Extension H

    // ===== Ambiguous-width characters (terminal-aware) =====
    // TerminalDetector::narrow_ambiguous() decides the width based on terminal type
    // Affects layout calculation for characters like ▶ ⏳ ⏸ 🎵 🎙
    {
        bool narrow = TerminalDetector::narrow_ambiguous();
        // Miscellaneous Symbols (U+2600-26FF) - includes ☀ ☁ ☂ ★ ☆ etc.
        if (ucs >= 0x2600 && ucs <= 0x26FF)
            return narrow ? 1 : 2;
        // Dingbats (U+2700-27BF) - includes ✂ ✈ ✉ ✓ ✗ etc.
        if (ucs >= 0x2700 && ucs <= 0x27BF)
            return narrow ? 1 : 2;
        // Miscellaneous Technical (U+2300-23FF) - includes ⏳ ⏸ ⏹ ⏺ etc.
        if (ucs >= 0x2300 && ucs <= 0x23FF)
            return narrow ? 1 : 2;
        // Geometric Shapes (U+25A0-25FF) - includes ▶ ◀ ▼ ▲ ■ ● ○ etc.
        if (ucs >= 0x25A0 && ucs <= 0x25FF)
            return narrow ? 1 : 2;
    }

    // ===== Emoji width (runtime-aware) =====
    // True Emoji (U+1F300+) width is decided by get_emoji_width()
    // Priority: INI override > cursor probe > terminal-detected recommendation
    {
        int ew = get_emoji_width();
        if (ucs >= 0x1F300 && ucs <= 0x1F5FF)
            return ew; // Miscellaneous Symbols and Pictographs
        if (ucs >= 0x1F600 && ucs <= 0x1F64F)
            return ew; // Emoticons
        if (ucs >= 0x1F680 && ucs <= 0x1F6FF)
            return ew; // Transport and Map Symbols
        if (ucs >= 0x1F900 && ucs <= 0x1F9FF)
            return ew; // Supplemental Symbols and Pictographs
        if (ucs >= 0x1FA00 && ucs <= 0x1FAFF)
            return ew; // Extended Pictographs
    }
    if (ucs >= 0x2500 && ucs <= 0x259F)
        return 1; // Box Drawing (border characters, width 1)

    // ===== Width=1 characters (default) =====
    return 1;
}

// UTF-8 character width detection using mk_wcwidth.
// P1-7: `avail` is the number of valid bytes at `next_bytes` (i.e. bytes remaining after
//   first_byte). The decoder bounds-checks against `avail` before each continuation byte and
//   validates the 0x80 continuation prefix, so a truncated/malformed tail can no longer read
//   past the string's NUL terminator (the old code read next_bytes[1]/[2] unconditionally).
int Utils::utf8_char_display_width(unsigned char first_byte, const unsigned char *next_bytes,
                                   size_t avail) {
    // ASCII characters: return directly
    if ((first_byte & 0x80) == 0) {
        return mk_wcwidth(first_byte);
    }

    // Decode the UTF-8 codepoint
    uint32_t codepoint = 0;

    // 2-byte UTF-8
    if ((first_byte & 0xE0) == 0xC0) {
        if (avail < 1 || (next_bytes[0] & 0xC0) != 0x80)
            return 1; // truncated / invalid
        codepoint = ((first_byte & 0x1F) << 6) | (next_bytes[0] & 0x3F);
        return mk_wcwidth(codepoint);
    }

    // 3-byte UTF-8
    if ((first_byte & 0xF0) == 0xE0) {
        if (avail < 2 || (next_bytes[0] & 0xC0) != 0x80 || (next_bytes[1] & 0xC0) != 0x80)
            return 1;
        codepoint =
            ((first_byte & 0x0F) << 12) | ((next_bytes[0] & 0x3F) << 6) | (next_bytes[1] & 0x3F);
        return mk_wcwidth(codepoint);
    }

    // 4-byte UTF-8 (emoji, supplementary characters, etc.)
    if ((first_byte & 0xF8) == 0xF0) {
        if (avail < 3 || (next_bytes[0] & 0xC0) != 0x80 || (next_bytes[1] & 0xC0) != 0x80 ||
            (next_bytes[2] & 0xC0) != 0x80)
            return 1;
        codepoint = ((first_byte & 0x07) << 18) | ((next_bytes[0] & 0x3F) << 12) |
                    ((next_bytes[1] & 0x3F) << 6) | (next_bytes[2] & 0x3F);
        return mk_wcwidth(codepoint);
    }

    // Invalid UTF-8 lead byte
    return 1;
}

int Utils::utf8_display_width(const std::string &s) {
    int width = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int char_bytes = utf8_char_bytes(c);

        // Get the following bytes for complete decoding
        size_t avail = (i + 1 < s.size()) ? (s.size() - (i + 1)) : 0;
        const unsigned char *next = reinterpret_cast<const unsigned char *>(s.c_str() + i + 1);

        width += utf8_char_display_width(c, next, avail);
        i += char_bytes;
    }
    return width;
}

// Truncate-for-display function; uses "..." as the ellipsis marker
// Fixed Chinese character width calculation - must pass the next_bytes parameter
std::string Utils::truncate_by_display_width(const std::string &s, int max_display_width) {
    if (max_display_width <= 0)
        return "";

    int current_width = 0;
    size_t i = 0;

    while (i < s.size()) {
        unsigned char c = s[i];
        int char_bytes = utf8_char_bytes(c);
        // Pass next_bytes to correctly compute Chinese character width (width=2)
        size_t avail = (i + 1 < s.size()) ? (s.size() - (i + 1)) : 0;
        const unsigned char *next = reinterpret_cast<const unsigned char *>(s.c_str() + i + 1);
        int char_width = utf8_char_display_width(c, next, avail);

        if (current_width + char_width > max_display_width)
            break;

        current_width += char_width;
        i += char_bytes;
    }

    std::string result = s.substr(0, i);
    if (i < s.size()) {
        // Use "..." uniformly as the ellipsis marker (three dots, width=3)
        if (current_width + 3 <= max_display_width) {
            result += "...";
        } else if (current_width + 2 <= max_display_width) {
            result += "..";
        } else if (current_width + 1 <= max_display_width) {
            result += ".";
        }
    }

    return result;
}

// Truncate text from the right, preserving the tail
// Used for responsive display of right-side status bar content
std::string Utils::truncate_by_display_width_right(const std::string &s, int max_display_width) {
    if (max_display_width <= 0)
        return "";

    int text_width = utf8_display_width(s);
    if (text_width <= max_display_width)
        return s;

    // Need to elide from the front, preserving the tail
    int skip_width = text_width - max_display_width;

    // Find the start position after skipping skip_width
    int current_width = 0;
    size_t start_pos = 0;

    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int char_bytes = utf8_char_bytes(c);
        size_t avail = (i + 1 < s.size()) ? (s.size() - (i + 1)) : 0;
        const unsigned char *next = reinterpret_cast<const unsigned char *>(s.c_str() + i + 1);
        int char_width = utf8_char_display_width(c, next, avail);

        current_width += char_width;
        i += char_bytes;

        if (current_width >= skip_width) {
            start_pos = i;
            break;
        }
    }

    return s.substr(start_pos);
}

// Truncate text from the middle, preserving head and tail
// Used for responsive display of middle URL content in the status bar
// Example: truncate_middle("https://example.com/path", 15) -> "https://.../path"
std::string Utils::truncate_middle(const std::string &s, int max_display_width) {
    if (max_display_width <= 0)
        return "";

    int text_width = utf8_display_width(s);
    if (text_width <= max_display_width)
        return s;

    // Need to elide from the middle, preserving head and tail
    // "..." occupies 3 columns; remaining space is split evenly between head and tail
    int remaining = max_display_width - 3; // Subtract the width of "..."
    if (remaining < 2)
        return "..."; // Edge case: show only the ellipsis

    int head_width = remaining / 2;
    int tail_width = remaining - head_width;

    // Extract the head (from the beginning)
    std::string head;
    int current_width = 0;
    size_t i = 0;
    while (i < s.size() && current_width < head_width) {
        unsigned char c = s[i];
        int char_bytes = utf8_char_bytes(c);
        size_t avail = (i + 1 < s.size()) ? (s.size() - (i + 1)) : 0;
        const unsigned char *next = reinterpret_cast<const unsigned char *>(s.c_str() + i + 1);
        int char_width = utf8_char_display_width(c, next, avail);

        if (current_width + char_width > head_width)
            break;

        head += s[i];
        current_width += char_width;
        i += char_bytes;
    }

    // Extract the tail (from the end backward)
    std::string tail;
    current_width = 0;
    size_t j = s.size();
    while (j > i && current_width < tail_width) {
        j--;
        // Back up to the UTF-8 character start byte
        while (j > 0 && (s[j] & 0xC0) == 0x80)
            j--;

        unsigned char c = s[j];
        int char_bytes = utf8_char_bytes(c);
        size_t avail = (j + 1 < s.size()) ? (s.size() - (j + 1)) : 0;
        const unsigned char *next = reinterpret_cast<const unsigned char *>(s.c_str() + j + 1);
        int char_width = utf8_char_display_width(c, next, avail);

        if (current_width + char_width > tail_width)
            break;

        tail = s.substr(j, char_bytes) + tail;
        current_width += char_width;
    }

    return head + "..." + tail;
}

// Text wrapping function
// Wraps text to the specified width and returns an array of lines
// max_lines: maximum number of lines; when exceeded, the last line is truncated with "..."
std::vector<std::string> Utils::wrap_text(const std::string &s, int max_width, int max_lines) {
    std::vector<std::string> lines;
    if (max_width <= 0 || s.empty())
        return lines;

    size_t i = 0;
    while (i < s.size() && (int)lines.size() < max_lines) {
        int current_width = 0;
        std::string line;

        // Build a line, not exceeding max_width
        while (i < s.size()) {
            unsigned char c = s[i];
            int char_bytes = utf8_char_bytes(c);
            size_t avail = (i + 1 < s.size()) ? (s.size() - (i + 1)) : 0;
            const unsigned char *next = reinterpret_cast<const unsigned char *>(s.c_str() + i + 1);
            int char_width = utf8_char_display_width(c, next, avail);

            if (current_width + char_width > max_width)
                break;

            line += s.substr(i, char_bytes);
            current_width += char_width;
            i += char_bytes;
        }

        // If this is the last line and there is remaining content, append "..."
        if ((int)lines.size() == max_lines - 1 && i < s.size()) {
            int dots = std::min(3, max_width - current_width);
            if (dots > 0) {
                line += std::string(dots, '.');
            }
            // Fill the last line
            while (current_width + dots < max_width && i < s.size()) {
                unsigned char c = s[i];
                int char_bytes = utf8_char_bytes(c);
                size_t avail = (i + 1 < s.size()) ? (s.size() - (i + 1)) : 0;
                const unsigned char *next =
                    reinterpret_cast<const unsigned char *>(s.c_str() + i + 1);
                int char_width = utf8_char_display_width(c, next, avail);
                if (current_width + dots + char_width > max_width)
                    break;
                line += s.substr(i, char_bytes);
                current_width += char_width;
                i += char_bytes;
            }
        }

        if (!line.empty()) {
            lines.push_back(line);
        }
    }

    return lines;
}

// Industrial-grade scrolling text function
// Core principle: scroll by terminal display columns, not character indices
// Reference: ncurses/vim/tmux Unicode scrolling implementations
std::string Utils::get_scrolling_text(const std::string &text, int max_width, int scroll_offset) {
    if (max_width <= 0)
        return "";

    int text_width = utf8_display_width(text);
    if (text_width <= max_width)
        return text;

    // Build the scrolling content
    // Format: original text + "   " + the beginning of the original text (for seamless looping)
    std::string separator = "   ";
    std::string padded = text + separator;
    int padded_width = utf8_display_width(padded);

    // offset is the scroll offset in terminal columns
    // Use modulo arithmetic to implement looping scroll
    int offset = scroll_offset % padded_width;
    if (offset < 0)
        offset = 0;

    // Step 1 - locate the start byte position by terminal column width
    // Key: when offset falls inside a wide character, skip to the next character
    int current_width = 0;
    size_t start_pos = 0;

    for (size_t i = 0; i < padded.size();) {
        unsigned char c = padded[i];
        int char_bytes = utf8_char_bytes(c);
        size_t avail = (i + 1 < padded.size()) ? (padded.size() - (i + 1)) : 0;
        const unsigned char *next = reinterpret_cast<const unsigned char *>(padded.c_str() + i + 1);
        int char_width = utf8_char_display_width(c, next, avail);

        // If the current character would push the width past offset, start from the next character
        // This avoids "cutting" a wide character in the middle
        if (current_width + char_width > offset) {
            // offset falls inside this character; skip past it
            break;
        }

        current_width += char_width;
        i += char_bytes;
        start_pos = i;
    }

    // Step 2 - starting from start_pos, truncate precisely by terminal column width
    // Ensure the output does not exceed max_width columns strictly
    std::string remaining = padded.substr(start_pos);

    // If the remaining content is shorter than max_width, pad from the beginning (for seamless looping)
    int remaining_width = utf8_display_width(remaining);
    if (remaining_width < max_width) {
        remaining += text; // Append the beginning of the original text
    }

    // Step 3 - strictly truncate the output to max_width
    return truncate_by_display_width_strict(remaining, max_width);
}

// Strict truncation function - output width never exceeds max_width
std::string Utils::truncate_by_display_width_strict(const std::string &s, int max_display_width) {
    if (max_display_width <= 0)
        return "";

    int current_width = 0;
    size_t i = 0;

    while (i < s.size()) {
        unsigned char c = s[i];
        int char_bytes = utf8_char_bytes(c);
        size_t avail = (i + 1 < s.size()) ? (s.size() - (i + 1)) : 0;
        const unsigned char *next = reinterpret_cast<const unsigned char *>(s.c_str() + i + 1);
        int char_width = utf8_char_display_width(c, next, avail);

        // Defensive handling - control characters return -1; treat them as 0 width and skip
        // Avoid current_width + (-1) overflow leading to logic errors
        if (char_width < 0)
            char_width = 0;

        // Strict check - if adding this character would exceed, stop immediately
        if (current_width + char_width > max_display_width)
            break;

        current_width += char_width;
        i += char_bytes;
    }

    return s.substr(0, i);
}

std::string Utils::format_duration(int seconds) {
    if (seconds <= 0)
        return "";
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    if (h > 0)
        return fmt::format("{}h{}m", h, m);
    if (m > 0)
        return fmt::format("{}m", m);
    return fmt::format("{}s", s);
}
} // namespace panicast
