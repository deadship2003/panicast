// Y11: theme palette table, isolated in its own translation unit (src/theme/themes.cpp) so the
//   12 palettes can be edited without touching UI code. struct Theme + themes() are at namespace
//   scope; UI methods reference them via name lookup.
//
//   rgb[8][3] index order: [0]=COLOR_BLACK(bg) [1]=RED [2]=GREEN [3]=YELLOW [4]=BLUE [5]=MAGENTA
//   [6]=CYAN [7]=COLOR_WHITE(fg). Values are RGB 0-1000 (ncurses init_color scale).
//   All 12 palettes are dark with SOFT foregrounds (no pure #ffffff) — fixes the glaring-white issue.
#pragma once

namespace panicast
{

struct Theme {
    const char *name;
    bool dark;     // true=dark (bg=BLACK, fg=WHITE); false=light (bg=WHITE, fg=BLACK)
    int rgb[8][3]; // RGB(0-1000) for COLOR_BLACK..COLOR_WHITE
};

// 22 terminal palettes (15 original + 7 added Y24.41: One Dark / Rose Pine / Monokai Pro /
//   Night Owl / Tomorrow Night / Edge Dark / Deep Ocean).
// Default index = 11 (GitHub Dark). Ctrl+L cycles them.
static constexpr int THEME_COUNT = 22;
static constexpr int DEFAULT_THEME_INDEX = 11; // GitHub Dark (user preference)

// Returns the static palette table (defined in src/theme/themes.cpp). Edit that file to adjust.
const Theme *themes();

} // namespace panicast
