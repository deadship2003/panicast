// Keymap — key (int) → Action. handle_input looks the key up here FIRST; if bound, the Action is
// published on the message bus (UI never calls Core directly for bound keys). Unbound keys fall
// through to the legacy switch (complex flows not yet migrated). Defaults are built in code by
// App::build_keymap from a defaults table; an INI [keys] section overrides them (D44 — rebindable
// hotkeys, D7's full goal). parse_token is the single parser shared by build_keymap + tests.
// (D7 build_keymap; D44 INI override + parse_token.)
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "panicast/app/actions.h"

namespace panicast
{

class Keymap {
public:
    void bind(int key, Action a) { map_[key] = std::move(a); }
    const Action *lookup(int key) const {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    bool contains(int key) const { return map_.count(key) > 0; }

    // D44: parse a [keys] INI token → ncurses keycode (int). Returns -1 if unparseable.
    //   Supports: space/enter/esc/tab/backspace names, a single printable char, or a numeric
    //   keycode (e.g. 259 = KEY_UP). The single-char path returns the char AS-IS — case matters
    //   ('r' and 'R' are different keys), matching wget_wch which returns exactly what is typed.
    //   Single DIGITS are therefore characters, not keycodes ("5" = the '5' key, not keycode 5).
    //   Note 25 (Ctrl+Y) and KEY_MOUSE are intercepted before the keymap (clipboard/mouse) —
    //   build_keymap rejects bindings there; see its comment.
    //   Exposed static so build_keymap + unit tests share one parser.
    //   Impl in src/app/keymap.cpp (D44-prof: was inline here — recompiled in every TU).
    static int parse_token(const std::string &raw);

    // D44-audit: every keycode a token means on real terminals. With keypad(stdscr, TRUE) —
    //   this app's mode — backspace arrives as 127 (DEL) / 8 (Ctrl+H) / KEY_BACKSPACE and
    //   enter as '\n' / '\r' / KEY_ENTER depending on terminfo (popups.cpp already defends
    //   this trio for text input). build_keymap binds ALL encodings so a rebind fires on
    //   every terminal; other tokens return {parse_token(raw)}; unparseable → empty.
    static std::vector<int> parse_token_all(const std::string &raw);

private:
    std::unordered_map<int, Action> map_;
};

}  // namespace panicast
