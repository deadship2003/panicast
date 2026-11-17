#include "panicast/core/terminal.h"

#include <cstdlib>
#include <cstdio> // sscanf
#include <cstring>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace panicast
{

int g_emoji_width_override = 0; // 0 = auto (uses terminal detection)
int g_emoji_width_probed = 0;   // 0 = not probed, 1/2 = probe result

TerminalDetector::TerminalType TerminalDetector::detect() {
    static TerminalType cached = do_detect();
    return cached;
}

bool TerminalDetector::narrow_ambiguous() {
    TerminalType t = detect();
    return t == TerminalType::WINDOWS_TERMINAL || t == TerminalType::CMD_CONSOLE;
}

int TerminalDetector::recommended_emoji_width() {
    TerminalType t = detect();
    if (t == TerminalType::WINDOWS_TERMINAL || t == TerminalType::CMD_CONSOLE) {
        return 1;
    }
    return 2;
}

std::string TerminalDetector::terminal_name() {
    TerminalType t = detect();
    switch (t) {
    case TerminalType::XTERM:
        return "xterm/Linux";
    case TerminalType::WINDOWS_TERMINAL:
        return "Windows Terminal";
    case TerminalType::CMD_CONSOLE:
        return "Windows cmd.exe";
    case TerminalType::APPLE_TERMINAL:
        return "Apple Terminal";
    default:
        return "Unknown";
    }
}

TerminalDetector::TerminalType TerminalDetector::do_detect() {
    if (std::getenv("WT_SESSION")) {
        return TerminalType::WINDOWS_TERMINAL;
    }
    const char *tp = std::getenv("TERM_PROGRAM");
    if (tp) {
        std::string s(tp);
        if (s == "iTerm.app" || s == "Apple_Terminal") {
            return TerminalType::APPLE_TERMINAL;
        }
        if (s == "vscode") {
            return TerminalType::XTERM;
        }
    }
    const char *term = std::getenv("TERM");
    if (term) {
        std::string t(term);
        if (t == "dumb" || t.empty()) {
            return TerminalType::CMD_CONSOLE;
        }
    }
    return TerminalType::XTERM;
}

int get_emoji_width() {
    // Priority: INI override > cursor probe > terminal detection recommendation
    if (g_emoji_width_override > 0)
        return g_emoji_width_override;
    if (g_emoji_width_probed > 0)
        return g_emoji_width_probed;
    return TerminalDetector::recommended_emoji_width();
}

// Cursor probe method - measures the terminal's actual rendered width of an emoji
// Principle: before ncurses initialization, use raw terminal output of an emoji + query cursor position
// Returns: 1 or 2 (returns 0 on probe failure, falls back to terminal detection)
// Uses poll for timeout control (300ms first byte + 50ms to collect), covering slow SSH round-trips.
int probe_emoji_width() {
    // Do not probe on non-tty (output redirected/piped/CI): tcgetattr failure means return immediately
    int stdin_fd = STDIN_FILENO;
    struct termios old_tio, new_tio;
    if (tcgetattr(stdin_fd, &old_tio) != 0)
        return 0;

    // Set raw mode (disable echo and line buffering)
    new_tio = old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    new_tio.c_cc[VMIN] = 0;
    new_tio.c_cc[VTIME] = 0; // Timeout is controlled by poll instead of the termios timer
    if (tcsetattr(stdin_fd, TCSANOW, &new_tio) != 0)
        return 0;

    int result = 0;
    // Output: CR + emoji + query cursor position
    // \r returns to column start, outputs 🎵 (U+1F3B5, UTF-8: F0 9F 8E B5), then \033[6n to query
    const char probe[] = "\r🎵\033[6n";
    constexpr size_t probe_len = sizeof(probe) - 1; // Exclude the NUL terminator
    if (write(STDOUT_FILENO, probe, probe_len) == (ssize_t)probe_len) {
        // Read reply: \033[row;colR
        // poll waits for the first byte (up to 300ms, covers SSH round-trip); once it arrives, a short window (50ms) collects remaining bytes
        char reply[32];
        int idx = 0;
        struct pollfd pfd{stdin_fd, POLLIN, 0};
        if (poll(&pfd, 1, 300) > 0) {
            while (idx < 31) {
                ssize_t n = read(stdin_fd, &reply[idx], 31 - idx);
                if (n <= 0)
                    break;
                idx += static_cast<int>(n);
                if (reply[idx - 1] == 'R')
                    break; // End of reply
                struct pollfd p2{stdin_fd, POLLIN, 0};
                if (poll(&p2, 1, 50) <= 0)
                    break; // Subsequent bytes did not arrive in time
            }
        }
        reply[idx] = '\0';

        // Parse the col value: \033[row;colR
        int row = 0, col = 0;
        if (sscanf(reply, "\033[%d;%dR", &row, &col) == 2) {
            // col is 1-based; after outputting 🎵 the cursor should be at col 1+width
            // If col=2, emoji width=1; if col=3, emoji width=2
            result = (col >= 3) ? 2 : 1;
        }
    }

    // Cleanup: carriage return + clear line, erase probe output
    write(STDOUT_FILENO, "\r\033[K", 4);

    // Restore terminal attributes
    tcsetattr(stdin_fd, TCSANOW, &old_tio);

    return result;
}

} // namespace panicast
