#pragma once
#include <ncurses.h>
#include <string>
#include "panicast/config/ini_config.h"
#include "panicast/theme/themes.h"
#include "panicast/theme/colors.h"

namespace panicast
{

// Y24.30: ThemeManager — extracted from UI (was 4 methods + 2 members in the god class).
//   Owns the 15-palette theme cycle + status-bar color config.
class ThemeManager {
public:
    static ThemeManager &instance() {
        static ThemeManager t;
        return t;
    }

    void init() {
        int ti = IniConfig::instance().get_int("display", "theme_index", DEFAULT_THEME_INDEX);
        if (ti < 0 || ti >= THEME_COUNT)
            ti = DEFAULT_THEME_INDEX;
        theme_index_ = ti;
        statusbar_color_config_ = IniConfig::instance().get_statusbar_color_config();
        apply();
    }

    void toggle() {
        theme_index_ = (theme_index_ + 1) % THEME_COUNT;
        apply();
        IniConfig::instance().set("display", "theme_index", std::to_string(theme_index_));
    }

    std::string current_name() const {
        return themes()[theme_index_].name;
    }
    int index() const {
        return theme_index_;
    }
    int count() const {
        return THEME_COUNT;
    }
    StatusBarColorConfig &statusbar_config() {
        return statusbar_color_config_;
    }

public:
    void apply() {
        const Theme &t = themes()[theme_index_];
        init_color(COLOR_BLACK, t.rgb[0][0], t.rgb[0][1], t.rgb[0][2]);
        init_color(COLOR_RED, t.rgb[1][0], t.rgb[1][1], t.rgb[1][2]);
        init_color(COLOR_GREEN, t.rgb[2][0], t.rgb[2][1], t.rgb[2][2]);
        init_color(COLOR_YELLOW, t.rgb[3][0], t.rgb[3][1], t.rgb[3][2]);
        init_color(COLOR_BLUE, t.rgb[4][0], t.rgb[4][1], t.rgb[4][2]);
        init_color(COLOR_MAGENTA, t.rgb[5][0], t.rgb[5][1], t.rgb[5][2]);
        init_color(COLOR_CYAN, t.rgb[6][0], t.rgb[6][1], t.rgb[6][2]);
        init_color(COLOR_WHITE, t.rgb[7][0], t.rgb[7][1], t.rgb[7][2]);
        // Dark: bg=BLACK/fg=WHITE; light: bg=WHITE/fg=BLACK
        short bg = t.dark ? COLOR_BLACK : COLOR_WHITE;
        short fg = t.dark ? COLOR_WHITE : COLOR_BLACK;
        for (int i = 1; i <= 7; ++i)
            init_pair(i, i, bg);
        init_pair(8, COLOR_WHITE, COLOR_BLACK);
        init_pair(9, COLOR_BLACK, COLOR_WHITE);
        init_state_color_pairs(bg);
        // Border colors (Y24.41: restored — pair 20/21 are used by the INFO panel + playback
        //   progress bar via PAIR_BORDER_STD; without this they fell back to an arbitrary
        //   256-color and the bar/border rendered the wrong color).
        init_pair(20, fg, bg);                               // standard border = theme foreground
        init_pair(21, t.dark ? COLOR_CYAN : COLOR_BLUE, bg); // info border = cyan/blue
        // Y24.41: assume_default_colors makes the DEFAULT window background = theme bg (not the
        //   terminal's transparent default). Combined with text pairs whose bg = theme bg, this
        //   eliminates the colored background block behind strings — text shows foreground color
        //   only, the window shows the theme background, and the selected line keeps its A_REVERSE
        //   highlight bar. (Restores Y24.20 behavior; use_default_colors() in UI init is overridden.)
        assume_default_colors(fg, bg);
    }

private:
    int theme_index_ = 0;
    StatusBarColorConfig statusbar_color_config_;

    void init_state_color_pairs(short bg) {
        init_pair(10, IniConfig::instance().get_node_color("stream_cached", COLOR_CYAN), bg);
        init_pair(11, IniConfig::instance().get_node_color("currently_playing", COLOR_GREEN), bg);
        init_pair(12, IniConfig::instance().get_node_color("db_cached", COLOR_YELLOW), bg);
        init_pair(13, IniConfig::instance().get_node_color("parse_failed", COLOR_RED), bg);
        init_pair(14, IniConfig::instance().get_node_color("info", COLOR_BLUE), bg);
        init_pair(15, IniConfig::instance().get_node_color("downloaded", COLOR_GREEN), bg);
        init_pair(16, IniConfig::instance().get_node_color("partial", COLOR_YELLOW), bg);
    }
};

} // namespace panicast
