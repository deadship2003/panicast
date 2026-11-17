// Status bar color renderer: five modes (rainbow/random/time-brightness/fixed/custom), thread-safe.
#pragma once

#include <string>

#include <ncurses.h>

#include "panicast/config/ini_config.h"

namespace panicast
{

// Status bar dynamic color rendering: returns a 256-color code (16-231) according to the configured mode.
// Callers then init_pair(color+1, color, bg) and use COLOR_PAIR(color+1).
class StatusBarColorRenderer {
public:
    static int get_color(const StatusBarColorConfig &config, int offset);

private:
    static float calculate_brightness(const StatusBarColorConfig &config);
    static int get_rainbow_color(int hue, float brightness);
    static int get_random_color(float brightness);
    static int get_time_adjusted_color(float brightness);
    static int get_fixed_color(const std::string &color_name, float brightness);

    // CUSTOM mode - cycle through a user-defined color sequence
    // offset is the character position index; colors are picked in groups by custom_speed
    static int get_custom_color(const StatusBarColorConfig &config, int offset);
};

} // namespace panicast
