// Layout guard: unified init/draw clamping + safe resize + safe popups.
#pragma once

#include <algorithm>

#include <ncurses.h>

#include "panicast/config/ini_config.h"
#include "panicast/ui/layout_metrics.h" // LayoutMetrics::DEFAULT_LAYOUT_RATIO

namespace panicast
{

// Ratio of the event log area height to top_h (same value as the layout ratio DEFAULT_LAYOUT_RATIO but different meaning; do not mix them up)
constexpr float LOG_HEIGHT_RATIO = 0.4f;

// ─── P1-1~P1-7: Layout guard (unified init/draw clamping + safe resize + safe popups) ─
struct LayoutDims {
    int left_w, right_w, top_h;
};

class LayoutGuard {
public:
    static constexpr int MIN_W = 20, MIN_H = 8;
    static constexpr int MIN_LEFT = 10, MIN_RIGHT = 10, MIN_TOP = 5;
    static constexpr int STATUS_H = 3;

    // Single layout computation entry, shared by init() and draw() to eliminate inconsistency.
    // Reads INI layout_ratio (same source as LayoutMetrics) — previously the window geometry
    // hardcoded 40%, while truncation width used the INI ratio, causing layout_ratio to be
    // ineffective and the content area to misalign with the window.
    static LayoutDims compute(int term_w, int term_h) {
        LayoutDims d;
        float r = IniConfig::instance().get_float("display", "layout_ratio",
                                                  LayoutMetrics::DEFAULT_LAYOUT_RATIO);
        if (r < 0.2f)
            r = 0.2f;
        if (r > 0.8f)
            r = 0.8f;
        int ratio_pct = static_cast<int>(r * 100.0f + 0.5f);
        if (ratio_pct < 20)
            ratio_pct = 20;
        if (ratio_pct > 80)
            ratio_pct = 80;
        d.left_w = term_w * ratio_pct / 100;
        d.right_w = term_w - d.left_w;
        d.top_h = term_h - STATUS_H;
        d.top_h = std::max(d.top_h, MIN_TOP);
        if (term_w >= MIN_W) {
            // Normal width: keep minimum widths on both sides, and the sum must not exceed term_w (avoid drawing off-screen)
            d.left_w = std::max(d.left_w, MIN_LEFT);
            d.right_w = std::max(d.right_w, MIN_RIGHT);
            if (d.left_w + d.right_w > term_w) {
                d.left_w = std::min(d.left_w, term_w - MIN_RIGHT);
                d.right_w = term_w - d.left_w;
            }
        } else {
            // Extremely narrow terminal: keep both sides visible proportionally, do not enforce minimum widths, to avoid the right panel drawing off-screen
            d.left_w = std::max(1, d.left_w);
            if (d.left_w + d.right_w > term_w)
                d.left_w = std::max(1, term_w / 2);
            d.right_w = std::max(1, term_w - d.left_w);
        }
        return d;
    }
    // Safe split_y computation: always returns a valid range or -1 (do not draw the separator)
    static int safe_split_y(int top_h) {
        if (top_h < 8)
            return -1;
        // LOG/INFO split ratio is INI-configurable ([display] log_height_ratio, default 0.4 = LOG 40%).
        float ratio = IniConfig::instance().get_log_height_ratio();
        if (ratio < 0.1f)
            ratio = 0.1f;
        if (ratio > 0.8f)
            ratio = 0.8f;
        // F36: log_compress_height (terminal height, default 23) → top_h threshold. At/above it the
        //   ratio holds. Below it, LOG is compressed 1 row per 1 row of height loss (INFO preserved
        //   at its threshold value — progress bar prioritized over LOG). LOG hidden when < 2 rows.
        //   Replaces F31's min_h/floor-6 (which couldn't restrict the window and starved INFO when
        //   small). The app cannot lock the terminal size — this is a degradation threshold.
        int compress_top_h = IniConfig::instance().get_log_compress_height() - STATUS_H;
        if (compress_top_h < 8)
            compress_top_h = 8;
        int log_h;
        if (top_h >= compress_top_h) {
            log_h = static_cast<int>((top_h - 2) * ratio); // ratio zone (70:30)
        } else {
            int log_at_th = static_cast<int>((compress_top_h - 2) * ratio); // LOG rows at threshold
            log_h = log_at_th - (compress_top_h - top_h); // shrink 1:1, INFO preserved
        }
        if (log_h > top_h / 2)
            log_h = top_h / 2;
        if (log_h < 2)
            return -1; // LOG can't fit 2 rows → hide separator, INFO takes the panel
        int split_y = (top_h - 1) - log_h;
        if (split_y <= 0 || split_y >= top_h - 1)
            return -1;
        return split_y;
    }
    // Safe popup construction: clamped to terminal bounds; returning nullptr means the caller should skip this frame
    static WINDOW *make_centered_popup(int min_h, int min_w, int term_w, int term_h) {
        int h = std::min(min_h, term_h - 4);
        int w = std::min(std::max(min_w, term_w * 8 / 10), term_w - 2);
        if (h < 3 || w < 10)
            return nullptr;
        int x = (term_w - w) / 2;
        int y = std::max(2, (term_h - h) / 3);
        WINDOW *win = newwin(h, w, y, x);
        if (win) {
            keypad(win, TRUE);
            scrollok(win, FALSE);
        }
        return win;
    }
    // Safe wresize: rebuild the window on failure
    static bool safe_wresize(WINDOW *&win, int h, int w) {
        if (!win)
            return false;
        if (wresize(win, h, w) == OK)
            return true;
        // Failed: rebuild (position is adjusted by the caller via mvwin)
        delwin(win);
        win = newwin(h, w, 0, 0);
        return win != nullptr;
    }
};

} // namespace panicast
