// WINDOW RAII wrapper: newwin + keypad/scrollok on construction, delwin on destruction.
#pragma once

#include <ncurses.h>

namespace panicast
{

class WinRAII {
public:
    WINDOW *win;
    WinRAII(int h, int w, int y, int x) : win(newwin(h, w, y, x)) {
        if (win) {
            keypad(win, TRUE);
            scrollok(win, FALSE);
        }
    }
    ~WinRAII() {
        if (win)
            delwin(win);
    }
    WinRAII(const WinRAII &) = delete;
    WinRAII &operator=(const WinRAII &) = delete;
    operator WINDOW *() const {
        return win;
    }
    operator bool() const {
        return win != nullptr;
    }
};

} // namespace panicast
