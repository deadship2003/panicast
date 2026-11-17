// UI rendering layer — extracted implementation unit (Y24.33–Y24.36).
//   Methods remain UI members (they touch private UI state); only their
//   implementations live here. Declarations stay in ui.h.
#include "panicast/ui/ui.h"

#include <algorithm>
#include <string>

#include <ncurses.h>

namespace panicast
{

void UI::update_lyric_history(const MPVController::State &state) {
    // D14-3: state.current_url here is a per-track change-detection key only (never displayed).
    //   The played path changes per track, so the reload trigger fires correctly; the canonical
    //   source URL adds no behavioral gain, and this is an IFrontend contract method — left as-is.
    if (state.current_url != last_lyric_url_) {
        lyric_history_.clear();
        last_lyric_url_ = state.current_url;
        // Y24.48: reset per-track LYRIC state on track change (manual override + embedded flag).
        lyric_manual_ = LyricManual::Auto;
        embedded_sub_confirmed_ = false;
    }
    std::string cur_line;
    if (!current_transcript_.empty()) {
        int idx = TranscriptParser::find_at(current_transcript_, state.time_pos);
        if (idx >= 0) {
            const auto &seg = current_transcript_[idx];
            cur_line = seg.speaker.empty() ? seg.text : ("[" + seg.speaker + "] " + seg.text);
        }
    } else if (!state.sub_text.empty()) {
        cur_line = state.sub_text;
        // Y24.46: collapse embedded newlines to a single space so each subtitle cue is ONE
        //   display line. mpv mov_text sub-text may contain '\n' for multi-line cues; leaving
        //   it would let '\n' reach mvwprintw (jumps to the next row → garbling) when the cue
        //   later becomes the history/prev row. Collapsing here keeps every history entry
        //   single-line and lets draw_row center/scroll it safely.
        for (char &c : cur_line)
            if (c == '\n' || c == '\r')
                c = ' ';
        // Y24.48: an embedded cue is actually displaying right now → confirm the source.
        embedded_sub_confirmed_ = true;
    }
    if (!cur_line.empty() && (lyric_history_.empty() || lyric_history_.back() != cur_line)) {
        lyric_history_.push_back(cur_line);
        int cap = std::max(IniConfig::instance().get_display_lyric_lines(), lyric_bar_lines_) + 4;
        while ((int)lyric_history_.size() > cap)
            lyric_history_.pop_front();
    }
}

std::string UI::lyric_bar_title() const {
    return " \xF0\x9F\x8E\xB5 LYRIC "; // "🎵 LYRIC"
}

void UI::draw_lyric_content(WINDOW *win, int y_start, int rows, int inner_w,
                            const MPVController::State &state) {
    if (rows <= 0)
        return;
    if (inner_w < 4)
        inner_w = 4;
    auto now = std::chrono::steady_clock::now();
    int scroll =
        (int)std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() /
        300;
    int mid = rows / 2; // the current line sits on this content row (0-based)
    auto fmt_seg = [](const TranscriptSegment &s) -> std::string {
        return s.speaker.empty() ? s.text : ("[" + s.speaker + "] " + s.text);
    };
    auto draw_row = [&](int row, const std::string &line, bool is_current) {
        if (row < 0 || row >= rows)
            return;
        std::string s = Utils::get_scrolling_text(line, inner_w, scroll);
        int sw = Utils::utf8_display_width(s);
        int x = (sw < inner_w) ? 2 + (inner_w - sw) / 2 : 2; // center short, left-align long
        if (is_current)
            wattron(win, A_BOLD | COLOR_PAIR(11)); // green + bold (theme-adaptive)
        mvwprintw(win, y_start + row, x, "%s", s.c_str());
        if (is_current)
            wattroff(win, A_BOLD | COLOR_PAIR(11));
    };

    if (!current_transcript_.empty()) {
        // find_active = ALL active segments (overlapping speakers).
        auto active = TranscriptParser::find_active(current_transcript_, state.time_pos);
        int idx = active.empty() ? TranscriptParser::find_at(current_transcript_, state.time_pos)
                                 : active.back();
        if (idx >= 0) {
            auto is_active = [&](int si) -> bool {
                for (int a : active)
                    if (a == si)
                        return true;
                return false;
            };
            for (int r = 0; r < rows; ++r) {
                int si = idx - mid + r; // segment index for this row
                if (si < 0 || si >= (int)current_transcript_.size())
                    continue; // blank
                draw_row(r, fmt_seg(current_transcript_[si]), is_active(si));
            }
            return;
        }
    }
    // Fallback (no transcript / find_at miss): embedded mp4 sub-text (lyric_history_).
    //   Y24.46: each cue is ONE line (newlines collapsed in update_lyric_history) → render with
    //   the SAME draw_row as the transcript path: long line → horizontal auto-scroll, short →
    //   centered. Current line vertically centered (row `mid`); previous subtitle one row above.
    //   This fixes the garbling that happened when a multi-line cue became the history row
    //   (its '\n' made mvwprintw jump rows). No '\n' ever reaches mvwprintw now.
    if (lyric_history_.empty())
        return;
    draw_row(mid, lyric_history_.back(), true); // current, vertically centered
    if (lyric_history_.size() >= 2) {
        draw_row(mid - 1, lyric_history_[lyric_history_.size() - 2], false); // previous subtitle
    }
}

void UI::draw_lyric_bar(WINDOW *win, const MPVController::State &state) {
    // Y24.48: update_lyric_history is now called every frame by App (so embedded subs are
    //   detected even when the bar is inactive); no need to call it here.
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, "%s",
              lyric_bar_title().c_str()); // "🎵 LYRIC" (+ offset) in the top border
    int ww = getmaxx(win);
    draw_lyric_content(win, 1, lyric_bar_lines_, ww - 4, state);
}

} // namespace panicast
