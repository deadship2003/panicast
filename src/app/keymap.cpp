// Keymap implementation unit (D44-prof: parse_token moved out of the header — 40 lines of
//   parsing logic inline in keymap.h recompiled in every including TU).
#include "panicast/app/keymap.h"

#include <cctype>
#include <charconv>
#include <string>
#include <vector>

#include <ncurses.h> // KEY_BACKSPACE / KEY_ENTER (multi-encoding aliases below)

namespace {
// D44-prof: was std::atoi — no error detection and UB on overflow ("99999999999"). from_chars
//   reports failure and clamps nothing; we reject unparsed/out-of-range tokens explicitly.
bool parse_keycode(const std::string &digits, int &out) {
    if (!digits.empty() && (digits.front() == '-' || digits.front() == '+'))
        return false; // review-fix: from_chars accepts signs ("-0" → 0 would pass the range check
                       //   below); keycodes are unsigned — reject signed tokens outright.
    int v = 0;
    auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v);
    if (ec != std::errc() || end != digits.data() + digits.size())
        return false;
    if (v < 0 || v > 0xFFFF) // sane keycode ceiling (ncurses KEY_* values live well below)
        return false;
    out = v;
    return true;
}
} // namespace

namespace panicast
{

// D44: parse a [keys] INI token → ncurses keycode (int). Returns -1 if unparseable.
//   Supports: space/enter/esc/tab/backspace names, a single printable char, or a numeric
//   keycode (e.g. 25 = Ctrl+Y). The single-char path returns the char AS-IS — case matters
//   ('r' and 'R' are different keys), matching wget_wch which returns exactly what is typed.
//   Exposed static so build_keymap + unit tests share one parser.
int Keymap::parse_token(const std::string &raw) {
    std::string t = raw;
    auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
    while (!t.empty() && !not_space(static_cast<unsigned char>(t.front())))
        t.erase(0, 1);
    while (!t.empty() && !not_space(static_cast<unsigned char>(t.back())))
        t.pop_back();
    if (t.empty())
        return -1;
    std::string low;
    low.reserve(t.size());
    for (char c : t)
        low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (low == "space")
        return ' ';
    if (low == "enter" || low == "return")
        return '\n';
    if (low == "esc" || low == "escape")
        return 27;
    if (low == "tab")
        return '\t';
    if (low == "backspace" || low == "bs")
        return 127;
    if (t.size() == 1)
        return static_cast<int>(static_cast<unsigned char>(t[0]));
    // Numeric keycode path. parse_keycode alone decides validity: from_chars rejects non-digit
    //   tokens ("ctrl+y" fails at 'c'), trailing garbage ("12x" parses 12 but end != last), signs
    //   (v < 0), and overflow ("99999999999" → result_out_of_range) — the old pre-scan for
    //   all-digits (from the std::atoi era, which silently truncated "12x"→12) duplicated that.
    int code = -1;
    if (parse_keycode(t, code))
        return code;
    return -1;
}

// D44-audit: see keymap.h. Trim+lowercase mirrors parse_token's own normalization (kept as a
//   local twin so parse_token itself stays untouched — behavior-equivalence of the rebind
//   parser is locked by the KeymapParseToken tests).
std::vector<int> Keymap::parse_token_all(const std::string &raw) {
    int primary = parse_token(raw);
    if (primary < 0)
        return {};
    std::string low;
    for (char c : raw)
        low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    low.erase(0, low.find_first_not_of(" \t\r\n")); // all-space input → erases everything
    low.erase(low.find_last_not_of(" \t\r\n") + 1); // already-trimmed-at-front, safe here
    if (low == "backspace" || low == "bs")
        return {127, 8, KEY_BACKSPACE};
    if (low == "enter" || low == "return")
        return {'\n', '\r', KEY_ENTER};
    return {primary};
}

} // namespace panicast
