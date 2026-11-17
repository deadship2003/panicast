#include "panicast/ui/border.h"

namespace panicast
{

void draw_box(WINDOW *win) {
    if (!win)
        return; // Guard against NULL (extreme cases like safe_wresize failure)
    // Clear window contents
    werase(win);

    // Get window size
    int ww, wh;
    getmaxyx(win, wh, ww);

    // Boundary protection: window must be at least 2x2 to draw a border
    if (ww < 2 || wh < 2)
        return;

    // ════ Draw the four edges (start from corners to ensure closure) ════
    // Top border: from top-left to top-right (excluding corners)
    for (int i = 1; i < ww - 1; i++) {
        mvwaddch(win, 0, i, ACS_HLINE);
    }
    // Bottom border: from bottom-left to bottom-right (excluding corners)
    for (int i = 1; i < ww - 1; i++) {
        mvwaddch(win, wh - 1, i, ACS_HLINE);
    }
    // Left border: from top-left to bottom-left (excluding corners)
    for (int i = 1; i < wh - 1; i++) {
        mvwaddch(win, i, 0, ACS_VLINE);
    }
    // Right border: from top-right to bottom-right (excluding corners)
    for (int i = 1; i < wh - 1; i++) {
        mvwaddch(win, i, ww - 1, ACS_VLINE);
    }

    // ════ Draw the four corners (drawn last to ensure closure) ════
    mvwaddch(win, 0, 0, ACS_ULCORNER);           // ┌ top-left
    mvwaddch(win, 0, ww - 1, ACS_URCORNER);      // ┐ top-right
    mvwaddch(win, wh - 1, 0, ACS_LLCORNER);      // └ bottom-left
    mvwaddch(win, wh - 1, ww - 1, ACS_LRCORNER); // ┘ bottom-right
}

// ═══════════════════════════════════════════════════════════════════════════
// Border protection function - optimized
// Purpose: force-redraw the border, overwriting any overflow content, ensuring all four corners and edges are fully closed
// Parameters:
//   win: window pointer
//   ww, wh: window width and height
//   title_start, title_end: title embed region (column index), -1 means none
//   split_y: separator line row number, -1 means no separator
//   split_title_start, split_title_end: separator title region
// ═══════════════════════════════════════════════════════════════════════════
void protect_border(WINDOW *win, int ww, int wh, int title_start, int title_end, int split_y,
                    int split_title_start, int split_title_end) {
    if (!win)
        return; // Guard against NULL
    // Boundary protection: window must be at least 2x2
    if (ww < 2 || wh < 2)
        return;

    // ════ Step 1: draw the four edges ════

    // ===== Top border (skip the title embed region) =====
    if (title_start < 0) {
        // No embed, redraw the whole line (excluding corners)
        for (int i = 1; i < ww - 1; i++) {
            mvwaddch(win, 0, i, ACS_HLINE);
        }
    } else {
        // Embedded, redraw in segments: left part + right part
        for (int i = 1; i < title_start; i++) {
            mvwaddch(win, 0, i, ACS_HLINE);
        }
        for (int i = title_end; i < ww - 1; i++) {
            mvwaddch(win, 0, i, ACS_HLINE);
        }
    }

    // ===== Bottom border (redraw the whole line, excluding corners) =====
    for (int i = 1; i < ww - 1; i++) {
        mvwaddch(win, wh - 1, i, ACS_HLINE);
    }

    // ===== Left and right borders (full redraw, excluding corners) =====
    for (int i = 1; i < wh - 1; i++) {
        mvwaddch(win, i, 0, ACS_VLINE);
        mvwaddch(win, i, ww - 1, ACS_VLINE);
    }

    // ════ Step 2: draw the four corners (drawn last to ensure closure) ════
    mvwaddch(win, 0, 0, ACS_ULCORNER);           // ┌ top-left
    mvwaddch(win, 0, ww - 1, ACS_URCORNER);      // ┐ top-right
    mvwaddch(win, wh - 1, 0, ACS_LLCORNER);      // └ bottom-left
    mvwaddch(win, wh - 1, ww - 1, ACS_LRCORNER); // ┘ bottom-right

    // ════ Step 3: separator line (if any) ════
    if (split_y > 0 && split_y < wh - 1) {
        // Draw left and right connectors first
        mvwaddch(win, split_y, 0, ACS_LTEE);      // ├ left connector
        mvwaddch(win, split_y, ww - 1, ACS_RTEE); // ┤ right connector

        // Then draw the horizontal part of the separator line
        if (split_title_start < 0) {
            for (int i = 1; i < ww - 1; i++) {
                mvwaddch(win, split_y, i, ACS_HLINE);
            }
        } else {
            for (int i = 1; i < split_title_start; i++) {
                mvwaddch(win, split_y, i, ACS_HLINE);
            }
            for (int i = split_title_end; i < ww - 1; i++) {
                mvwaddch(win, split_y, i, ACS_HLINE);
            }
        }
    }
}

} // namespace panicast
