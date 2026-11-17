// UI rendering layer implementation unit:
//   1) Terminal/signal lifecycle (save/restore terminal attributes, ncurses cleanup, signal handler registration);
//   2) Accommodates future out-of-line definitions and static data member definitions;
//   3) Included as a translation unit in CMakeLists to keep -Wweak-linkage and other checks consistent.
#include "panicast/ui/ui.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include <ncurses.h>

namespace panicast
{

void UI::draw(
    AppMode mode, const std::vector<DisplayItem> &list, int selected,
    const MPVController::State &state, int view_start, AppState app_state,
    int marked_count, const std::string &search_query, int current_match,
    int total_matches, TreeNodePtr selected_node, const std::vector<DownloadProgress> &downloads,
    bool visual_mode, int visual_start, const std::vector<PlaylistItem> &playlist,
    int playlist_index, //Play mode + INFO play-context (7-line: 3 history + current + 3 next)
    PlayMode play_mode, const std::vector<std::string> &history_titles,
    const std::vector<int> &next_indices, const DisplayContext &dctx) {
    getmaxyx(stdscr, h, w);
    // NULL window guard: safe_wresize may null the window on failure; skip this frame to avoid dereferencing NULL
    if (!left_win || !right_win || !status_win || !lyric_win)
        return;
    // Use LayoutGuard::compute for unified clamping (eliminates init/draw inconsistency)
    // Old code did new_top_h = h - 3 directly; on small terminals this became 0/negative, causing draw_box to return early and borders to disappear
    LayoutDims dims = LayoutGuard::compute(w, h);
    int new_left_w = dims.left_w;
    int new_right_w = dims.right_w;

    // Y24: bottom strip = LYRIC bar (when active) or status bar (otherwise). When the LYRIC
    //   bar is active the status bar is hidden (space efficiency) and the top area shrinks by
    //   lyric_bar_height_. Fall back to the status bar if the terminal is too short for the bar.
    bool lyric_active = lyric_bar_active_ && (h >= 16);
    int bottom_h = lyric_active ? lyric_bar_height_ : LayoutGuard::STATUS_H;
    int new_top_h = h - bottom_h;
    if (new_top_h < 5) {
        new_top_h = 5;
    } // match existing min guard

    //Detect whether window size changed (incl. L-mode active↔inactive transition → full redraw)
    bool size_changed = (new_left_w != left_w || new_right_w != right_w || new_top_h != top_h ||
                         w != cols_ || lyric_active != last_lyric_bar_active_);

    if (size_changed) {
        //When window size changes, first clear all window contents
        // This avoids old border characters remaining on screen
        werase(left_win);
        werase(right_win);
        werase(status_win);
        werase(lyric_win);
        werase(stdscr);

        // Force the next-frame doupdate to clear and redraw the entire physical screen.
        //   After resize, ncurses's virtual screen may be out of sync with the terminal's
        //   physical screen; werase alone is insufficient.
        //   clearok(stdscr, TRUE) makes doupdate clear first then full-redraw,
        //   thoroughly eliminating leftover gaps/broken lines/open corners when dragging the window.
        //   Y24: also fires on L-mode toggle (top_h changes) to avoid buffer artifacts (garbled screen).
        clearok(stdscr, TRUE);

        // Update cached size
        left_w = new_left_w;
        right_w = new_right_w;
        top_h = new_top_h;
        cols_ = w;
        last_lyric_bar_active_ = lyric_active;
    }

    // Use LayoutGuard::safe_wresize; rebuild window on failure
    // Old code did not check wresize's return value; on failure the window size was inconsistent with the cache
    LayoutGuard::safe_wresize(left_win, top_h, left_w);
    LayoutGuard::safe_wresize(right_win, top_h, right_w);
    mvwin(right_win, 0, left_w);
    // Y24: resize BOTH bottom windows so the inactive one is cleared/sized away cleanly;
    //   only the active one is drawn below. mvwin places whichever is active at y=top_h.
    LayoutGuard::safe_wresize(status_win, LayoutGuard::STATUS_H, w);
    LayoutGuard::safe_wresize(lyric_win, lyric_bar_height_, w);
    if (lyric_active) {
        mvwin(lyric_win, top_h, 0);
        werase(status_win); // keep the hidden status bar clear (no stale content)
    } else {
        mvwin(status_win, top_h, 0);
        werase(lyric_win); // keep the hidden lyric bar clear
    }

    //More conservatively compute the title-bar net width, protecting the top-right border
    // Top-right needs to reserve: ┐ and ─, 2 columns total
    int title_max =
        left_w - 6; // left border 1 + space 1 + title + space 1 + top-right protection 3
    if (title_max < 5)
        title_max = 5; // Minimum reservation

    std::string mode_str;
    // Title Emoji is enabled only when "terminal measured width (probed)==2".
    //   Root cause: ncurses advances its internal cursor per glibc wcwidth; the glibc width
    //   of title emoji must match the terminal's actual render width, otherwise the cursor
    //   misaligns -> subsequent corners/borders on the same line are drawn at wrong positions
    //   -> verticals and corners don't line up. The user's terminal probed=2, so only emoji
    //   with glibc wcwidth=2 are used (📻🎤💖📜🔍).
    //   Note: 🎙(U+1F399) has glibc=1 but terminal renders as 2 -> would misalign; replaced
    //   with 🎤(U+1F3A4, glibc=2).
    //   When probed!=2 (narrow-emoji terminal / not probed), fall back to ASCII to keep borders aligned.
    const bool use_emoji_title =
        title_emoji_ && (g_emoji_width_probed == 2 || g_emoji_width_override == 2);
    switch (mode) {
    case AppMode::RADIO:
        mode_str = use_emoji_title ? "📻 RADIO" : "[R] RADIO";
        break;
    case AppMode::PODCAST:
        mode_str = use_emoji_title ? "🎤 PODCAST" : "[P] PODCAST";
        break;
    case AppMode::FAVOURITE:
        mode_str = use_emoji_title ? "💖 FAVOURITE" : "[F] FAVOURITE";
        break;
    case AppMode::HISTORY:
        mode_str = use_emoji_title ? "📜 HISTORY" : "[H] HISTORY";
        break; //new
    case AppMode::ONLINE:
        mode_str =
            use_emoji_title
                ? fmt::format("🔍 ONLINE [{}]", dctx.online_region_name)
                : fmt::format("[O] ONLINE [{}]", dctx.online_region_name);
        break;
    case AppMode::ACCOUNT:
        mode_str = use_emoji_title ? "Ｙ YOUTUBE" : "[Y] YOUTUBE";
        break; // Y01 (fullwidth Y: glibc wcwidth=2)
    case AppMode::BILIBILI:
        mode_str = use_emoji_title ? "Ｂ BILIBILI" : "[B] BILIBILI";
        break; // Y15 (fullwidth B: glibc wcwidth=2)
    case AppMode::TIKTOK:
        // Y24.xx: region grouping removed — T mode holds both TikTok and Douyin.
        mode_str = use_emoji_title ? "🎵 TikTok/Douyin" : "[T] TikTok/Douyin";
        break;
    case AppMode::IPTV:
        mode_str = use_emoji_title ? "📺 IPTV" : "[I] IPTV";
        break; // Y24.50
    }
    if (visual_mode)
        mode_str = use_emoji_title ? "§ VISUAL §" : "[V] VISUAL";
    if (marked_count > 0)
        mode_str += use_emoji_title ? " [🔖 x" + std::to_string(marked_count) + "]"
                                    : " [* x" + std::to_string(marked_count) + "]";
    // V2.39-FF: T and S status indicators. Y24.11: T freed for TikTok mode — tree lines are
    //   now always ON (no keybind), so only the S (scroll) indicator remains.
    if (scroll_mode_)
        mode_str += " [S]";

    std::string title_display = Utils::truncate_by_display_width(mode_str, title_max);

    //Incremental redraw - do not call draw_box here

    //Window structure (after box()):
    //   column 0 = left border │
    //   column 1 to column left_w-2 = content area (width = left_w-2)
    //   column left_w-1 = right border │

    //Make full use of the content area width
    //   content area width = left_w - 2
    //   no longer reserve a right-side space for protection; content sits flush against the right border
    //   tree_line_width = left_w - 2

    // Note: content starts printing at column 1
    int tree_line_width = left_w - 2;
    if (tree_line_width < 10)
        tree_line_width = 10; // Minimum width protection

    // Full redraw of the left panel each frame. Incremental redraw has been removed — needs_full_redraw
    // always returns true; its else branch and the last_selected_idx_/last_view_start_/last_mode_/last_display_size_
    // tracking members are write-only dead code.
    int line_num = 1;
    // Full redraw: clear the window and draw all rows. The border is drawn uniformly by protect_border(-1) below.
    werase(left_win);

    for (int i = view_start; i < (int)list.size() && line_num < top_h - 1; ++i) {
        bool is_sel = (i == selected);
        bool in_visual =
            visual_mode && visual_start >= 0 &&
            ((visual_start <= i && i <= selected) || (selected <= i && i <= visual_start));
        draw_line(left_win, line_num, list[i], is_sel, in_visual, tree_line_width,
                  dctx.now_playing_url);
        line_num++;
    }

    // Draw the full border first (the whole top border as HLINE), then overlay the title on top.
    //   Key point: the top border has a complete ─ base first, and the title only overwrites the
    //   cells it occupies — regardless of whether the emoji width matches the terminal, no gap can
    //   appear (each cell is either ─ or a title character, no empty holes). Emoji width is provided
    //   by probe and is only used for the truncation layout in truncate_by_display_width above.
    //   Combined with per-frame redraw, toggling [T]/[S] leaves no residue. This drawing approach
    //   works for both ASCII/Emoji titles; both go through this unified path.
    protect_border(left_win, left_w, top_h,
                   -1); // -1 = draw the whole top border as HLINE (no skip)
    mvwprintw(left_win, 0, 2, " %s ", title_display.c_str());
    mvwaddch(left_win, 0, left_w - 1,
             ACS_URCORNER); // top-right corner protection (redrawn when title approaches)
    mvwaddch(left_win, 0, left_w - 2, ACS_HLINE);

    wnoutrefresh(left_win); // double-buffer optimization

    // Right Panel
    werase(
        right_win); // just clear; the border is drawn uniformly by protect_border at the end of draw_info
    mvwprintw(right_win, 0, 2, " INFO & LOG ");

    //Right panel structure: column 0 = left border, columns 1~right_w-2 = content area, column right_w-1 = right border
    //content area width = window_width - 3
    int right_inner_width = getmaxx(right_win) - 3;

    //Pass playlist data
    pending_osc8_.clear(); // Y24.12: reset per-frame OSC 8 link list before draw_info populates it
    draw_info(right_win, state, app_state, dctx, marked_count, search_query, current_match,
              total_matches, selected_node, downloads, visual_mode, right_inner_width, playlist,
              playlist_index, play_mode, history_titles, next_indices);

    // draw_info already used protect_border(2,14,split_y,2,13) to draw the right panel's full border
    // (including the separator line); not redrawn here (previously caused the border to be drawn 3 times per frame).
    wnoutrefresh(right_win); // double-buffer optimization

    // Bottom strip: Y24 — LYRIC bar when L-mode is active (status bar hidden),
    //   otherwise the normal status bar.
    if (lyric_bar_active_ && (h >= 16)) {
        draw_lyric_bar(lyric_win, state);
        wnoutrefresh(lyric_win);
    } else {
        draw_status(status_win, state, selected_node, dctx);
        wnoutrefresh(status_win); // double-buffer optimization
    }

    //Double-buffer refresh - update all windows at once
    doupdate();
    // Y24.12: emit OSC 8 hyperlinks AFTER doupdate so ncurses has written the visible URL
    //   text first; the link markers (zero-width) are then layered on top and survive the
    //   incremental refresh (cell content matches → ncurses won't rewrite). Re-done per frame.
    if (!pending_osc8_.empty()) {
        // Y24.13: emit_osc8_link() moves the physical cursor (CUP + visible text), which
        //   desyncs ncurses' relative-cursor optimization → next doupdate garbles panels.
        //   Save ncurses' assumed cursor (curscr) before, restore it after via raw CUP.
        int sy = 0, sx = 0;
        getyx(curscr, sy, sx);
        for (const auto &e : pending_osc8_)
            Utils::emit_osc8_link(e.row, e.col, e.visible, e.url, e.id);
        Utils::emit_cup(sy + 1, sx + 1); // re-sync (CUP is 1-based)
        pending_osc8_.clear();
    }
}

} // namespace panicast
