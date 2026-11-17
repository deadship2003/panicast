// Terminal detection and emoji width runtime configuration.
// TerminalDetector identifies the terminal type; get_emoji_width() is the unified entry point for emoji display width
// (priority: INI override > cursor probing > terminal detection recommendation).
#pragma once

#include <string>

namespace panicast
{

class TerminalDetector {
public:
    enum class TerminalType {
        UNKNOWN,          // Unknown terminal (treated as 2 columns per Linux standard)
        XTERM,            // Linux xterm / rxvt / gnome-terminal, etc.
        WINDOWS_TERMINAL, // Windows Terminal (WSL2 default)
        CMD_CONSOLE,      // Legacy Windows cmd.exe
        APPLE_TERMINAL,   // macOS Terminal.app / iTerm2
    };

    static TerminalType detect();
    // Ambiguous-width characters should be 1 column under this terminal
    static bool narrow_ambiguous();
    // Recommended emoji display width
    static int recommended_emoji_width();
    static std::string terminal_name();

private:
    static TerminalType do_detect();
};

// Emoji width runtime configuration (0 = auto)
//   g_emoji_width_override: INI config override (0=auto, 1=force 1 column, 2=force 2 columns)
//   g_emoji_width_probed:   cursor probing result (0=not probed, 1/2=probed result)
extern int g_emoji_width_override;
extern int g_emoji_width_probed;

// Get emoji display width (shared by mk_wcwidth and IconManager)
int get_emoji_width();

// Cursor probing — measures the terminal's actual rendered width of emoji
// Return value: 1 or 2 (returns 0 on probe failure, falls back to terminal detection)
int probe_emoji_width();

} // namespace panicast
