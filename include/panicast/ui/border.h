// Border drawing and protection: draw_box redraws the full frame; protect_border forces a border redraw and supports title embedding / separator lines.
#pragma once

#include <ncurses.h>

namespace panicast
{

// Draw a full border (including four corners), werase first. Returns immediately when window size < 2x2.
void draw_box(WINDOW *win);

// Border protection: forcibly redraw all four edges and corners, overwriting overflowing content to ensure closure.
//   title_start/title_end: title embedding area (column indices), -1 means none
//   split_y: separator line row number, -1 means no separator
//   split_title_start/split_title_end: separator title area, -1 means none
void protect_border(WINDOW *win, int ww, int wh, int title_start = -1, int title_end = -1,
                    int split_y = -1, int split_title_start = -1, int split_title_end = -1);

} // namespace panicast
