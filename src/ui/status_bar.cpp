// UI rendering layer — extracted implementation unit (Y24.33–Y24.36).
//   Methods remain UI members (they touch private UI state); only their
//   implementations live here. Declarations stay in ui.h.
#include "panicast/ui/ui.h"

#include <algorithm>
#include <string>

#include <ncurses.h>

namespace panicast
{

void UI::draw_status(WINDOW *win, const MPVController::State &state, TreeNodePtr selected_node,
                     const DisplayContext &dctx) {
    werase(win);
    box(win, 0, 0);
    int ww = getmaxx(win);
    int inner_w = ww - 2; // available width (minus left/right borders)

    // ═══════════════════════════════════════════════════════════════════
    //Use modular art config
    // ═══════════════════════════════════════════════════════════════════

    // Variable content
    // Status-bar bottom-right shows the current system time, format "%b %d %Y %H:%M:%S".
    // version_str made static to avoid per-frame allocation.
    // F34: format includes seconds, so cache per-SECOND, not per-minute. Old code cached
    //   per minute (now_t/60) → seconds frozen up to 60s (looked "not updating"). Now
    //   strftime runs once per second (not every ~30ms frame); seconds tick visibly.
    static const std::string version_str = fmt::format("{} {}", APP_NAME, VERSION);
    static std::string cached_author_time;
    static std::time_t cached_second = 0;
    auto now_tp = std::chrono::system_clock::now();
    std::time_t now_t = std::chrono::system_clock::to_time_t(now_tp);
    if (now_t != cached_second) {
        char tbuf[32] = {0};
        struct tm tm_local;
        if (std::strftime(tbuf, sizeof(tbuf), "%b %d %Y %H:%M:%S",
                          localtime_local(&now_t, &tm_local)) > 0) { // thread-safe
            cached_author_time = fmt::format("{}@{}", AUTHOR, tbuf);
        }
        cached_second = now_t;
    }
    const std::string &author_time = cached_author_time;

    // Get the middle URL content (the part inside the brackets)
    std::string mid_content; // content inside the brackets
    // D12-1: sleep-timer state is pushed in via dctx (was SleepTimer::instance() — runtime state
    //   the UI must not query; see docs/ARCHITECTURE.md §2.1).
    bool show_timer = dctx.sleep_active;
    if (show_timer) {
        int remaining = dctx.sleep_remaining;
        int hours = remaining / 3600;
        int minutes = (remaining % 3600) / 60;
        int seconds = remaining % 60;
        mid_content = fmt::format("⏰ {:02d}:{:02d}:{:02d}", hours, minutes, seconds);
    } else {
        std::string url_to_show =
            state.has_media ? dctx.now_playing_url : (selected_node ? selected_node->url : "");
        url_to_show = Utils::http_to_https(url_to_show);
        mid_content = url_to_show;
    }

    // ═══════════════════════════════════════════════════════════════════
    // Compute each part's width
    // ═══════════════════════════════════════════════════════════════════

    //Use global modular art config
    // Left structure: ArtRenderer::outer_left() + version + ArtRenderer::outer_right()
    // Right structure: ArtRenderer::right_prefix() + author time + ArtRenderer::right_suffix()
    // Middle structure: ArtRenderer::bracket_left() + content + ArtRenderer::bracket_right()

    // Fixed art-char width
    int left_art_w = Utils::utf8_display_width(ArtRenderer::outer_left()) +
                     Utils::utf8_display_width(ArtRenderer::outer_right());
    int right_art_w = Utils::utf8_display_width(ArtRenderer::right_prefix()) +
                      Utils::utf8_display_width(ArtRenderer::right_suffix());

    // Variable content width
    int version_w = Utils::utf8_display_width(version_str);
    int author_w = Utils::utf8_display_width(author_time);
    int mid_w = Utils::utf8_display_width(mid_content);

    // Middle [] fixed part
    int bracket_fixed_w = Utils::utf8_display_width(ArtRenderer::bracket_left()) +
                          Utils::utf8_display_width(ArtRenderer::bracket_right());

    // Total width needed for full display
    int total_needed = left_art_w + version_w + right_art_w + author_w + bracket_fixed_w + mid_w;

    // Colors
    int left_color =
        StatusBarColorRenderer::get_color(ThemeManager::instance().statusbar_config(), 0);
    int right_color =
        StatusBarColorRenderer::get_color(ThemeManager::instance().statusbar_config(), 180);
    int mid_color =
        StatusBarColorRenderer::get_color(ThemeManager::instance().statusbar_config(), 90);

    // ═══════════════════════════════════════════════════════════════════
    //Smart abbreviation logic (using modular art config)
    // ═══════════════════════════════════════════════════════════════════

    std::string left_display, mid_display, right_display;

    // Compute total fixed-part width (art chars + [] fixed part)
    int fixed_total = left_art_w + right_art_w + bracket_fixed_w;

    if (total_needed <= inner_w) {
        // ════ Case 1: width is enough, show in full ════
        left_display =
            std::string(ArtRenderer::outer_left()) + version_str + ArtRenderer::outer_right();
        mid_display = mid_content.empty() ? ""
                                          : std::string(ArtRenderer::bracket_left()) + mid_content +
                                                ArtRenderer::bracket_right();
        right_display =
            std::string(ArtRenderer::right_prefix()) + author_time + ArtRenderer::right_suffix();

    } else if (inner_w < fixed_total + 6) {
        // ════ Case 2: extremely narrow window, keep art chars, abbreviate content to ... ════
        left_display = std::string(ArtRenderer::outer_left()) + "..." + ArtRenderer::outer_right();
        mid_display =
            std::string(ArtRenderer::bracket_left()) + "..." + ArtRenderer::bracket_right();
        right_display =
            std::string(ArtRenderer::right_prefix()) + "..." + ArtRenderer::right_suffix();

    } else {
        // ════ Case 3: abbreviate by priority ════
        // Available width for the variable region
        int variable_available = inner_w - fixed_total;

        // Stage 1: first try to show version and author time in full, abbreviate the middle URL
        int lr_needed = version_w + author_w;
        int mid_available = variable_available - lr_needed;

        if (mid_available >= 3) {
            // Version and author time can be shown in full, abbreviate the middle URL
            if (mid_w <= mid_available) {
                mid_display = std::string(ArtRenderer::bracket_left()) + mid_content +
                              ArtRenderer::bracket_right();
            } else {
                // Truncate the URL from the middle
                int inner_bracket_w = mid_available - bracket_fixed_w;
                mid_display = std::string(ArtRenderer::bracket_left()) +
                              Utils::truncate_middle(mid_content, inner_bracket_w) +
                              ArtRenderer::bracket_right();
            }
            left_display =
                std::string(ArtRenderer::outer_left()) + version_str + ArtRenderer::outer_right();
            right_display = std::string(ArtRenderer::right_prefix()) + author_time +
                            ArtRenderer::right_suffix();

        } else {
            // Stage 2: need to abbreviate version and/or author time
            // First give the middle URL minimum space (3 chars "...")
            int mid_min = 3;
            int remaining_for_lr = variable_available - mid_min;

            if (remaining_for_lr < 4) {
                // Too little space: show only art chars
                left_display = ArtRenderer::outer_left();
                mid_display =
                    std::string(ArtRenderer::bracket_left()) + "..." + ArtRenderer::bracket_right();
                right_display = ArtRenderer::right_suffix();
            } else {
                // Allocate remaining space to version and author time
                int half_remaining = remaining_for_lr / 2;

                // Abbreviate version
                std::string left_version_part;
                if (version_w <= half_remaining) {
                    left_version_part = version_str;
                } else if (half_remaining > 3) {
                    left_version_part =
                        Utils::truncate_by_display_width(version_str, half_remaining - 3) + "...";
                } else {
                    left_version_part = "...";
                }
                left_display = std::string(ArtRenderer::outer_left()) + left_version_part +
                               ArtRenderer::outer_right();

                // Abbreviate author time
                int right_remaining =
                    remaining_for_lr - Utils::utf8_display_width(left_version_part);
                std::string right_author_part;
                if (author_w <= right_remaining) {
                    right_author_part = author_time;
                } else if (right_remaining > 3) {
                    right_author_part = "..." + Utils::truncate_by_display_width_right(
                                                    author_time, right_remaining - 3);
                } else {
                    right_author_part = "...";
                }
                right_display = std::string(ArtRenderer::right_prefix()) + right_author_part +
                                ArtRenderer::right_suffix();

                // Middle shows only [...]
                mid_display =
                    std::string(ArtRenderer::bracket_left()) + "..." + ArtRenderer::bracket_right();
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    //[] centering logic - abbreviate the middle URL first to protect art chars
    // ═══════════════════════════════════════════════════════════════════

    // Compute the centered position of the middle region
    int mid_display_w = Utils::utf8_display_width(mid_display);
    int mid_x = (ww - mid_display_w) / 2; // centered start position
    if (mid_x < 1)
        mid_x = 1; // protect the left border

    // Compute the left side's max available width: up to the middle region's left boundary
    int left_max_w = mid_x - 2; // minus left border and space
    if (left_max_w < 0)
        left_max_w = 0;

    // Compute the right side's max available width: from the middle region's right boundary to the right border
    int right_start_x = mid_x + mid_display_w + 1; // position after the middle content ends
    int right_max_w = ww - right_start_x - 1;      // minus right border
    if (right_max_w < 0)
        right_max_w = 0;

    //Abbreviate the middle URL first, protecting the integrity of left/right art chars
    int left_display_w = Utils::utf8_display_width(left_display);
    int right_display_w = Utils::utf8_display_width(right_display);

    // Check whether the middle URL needs abbreviation
    bool need_shrink_mid = false;
    int total_overflow = 0;

    if (left_display_w > left_max_w) {
        total_overflow += (left_display_w - left_max_w);
        need_shrink_mid = true;
    }
    if (right_display_w > right_max_w) {
        total_overflow += (right_display_w - right_max_w);
        need_shrink_mid = true;
    }

    // If the middle URL needs abbreviation
    if (need_shrink_mid && mid_display_w > 8) { // keep at least "[ ... ]"
        int new_mid_w =
            mid_display_w - total_overflow - 4; // reduce 4 extra chars to ensure alignment
        if (new_mid_w < 8)
            new_mid_w = 8;

        // Rebuild the abbreviated middle content
        if (!mid_content.empty()) {
            int inner_w = new_mid_w - 4; // "[ " and " ]" take 4 chars
            if (inner_w < 3)
                inner_w = 3;
            mid_display = "[ " + Utils::truncate_middle(mid_content, inner_w) + " ]";
        } else {
            mid_display = "[...]";
        }

        // Recompute the centered position
        mid_display_w = Utils::utf8_display_width(mid_display);
        mid_x = (ww - mid_display_w) / 2;
        if (mid_x < 1)
            mid_x = 1;

        // Recompute left/right available width
        left_max_w = mid_x - 2;
        if (left_max_w < 0)
            left_max_w = 0;

        right_start_x = mid_x + mid_display_w + 1;
        right_max_w = ww - right_start_x - 1;
        if (right_max_w < 0)
            right_max_w = 0;
    }

    // Final safeguard: if still overflowing, truncate the version (protecting the art-char suffix)
    left_display_w = Utils::utf8_display_width(left_display);
    // Also handle when left_max_w==0 (the original && left_max_w>0 would skip, leaving a full-width string covering the middle)
    if (left_display_w > left_max_w) {
        if (left_max_w <= 0) {
            left_display = "";
        } else {
            // Protect the suffix art char, only truncate the version
            int suffix_w = Utils::utf8_display_width(ArtRenderer::outer_right());
            int prefix_w = Utils::utf8_display_width(ArtRenderer::outer_left());
            int available_for_version = left_max_w - prefix_w - suffix_w;
            if (available_for_version > 0) {
                left_display =
                    std::string(ArtRenderer::outer_left()) +
                    Utils::truncate_by_display_width(version_str, available_for_version) +
                    ArtRenderer::outer_right();
            } else {
                left_display = std::string(ArtRenderer::outer_left()) + ArtRenderer::outer_right();
                if (Utils::utf8_display_width(left_display) > left_max_w) {
                    left_display = Utils::truncate_by_display_width(left_display, left_max_w);
                }
            }
        }
    }

    // Final safeguard: if the right side still overflows, truncate the author time (protecting the art-char suffix)
    right_display_w = Utils::utf8_display_width(right_display);
    if (right_display_w > right_max_w) {
        if (right_max_w <= 0) {
            right_display = "";
        } else {
            int suffix_w = Utils::utf8_display_width(ArtRenderer::right_suffix());
            int prefix_w = Utils::utf8_display_width(ArtRenderer::right_prefix());
            int available_for_author = right_max_w - prefix_w - suffix_w;
            if (available_for_author > 0) {
                right_display =
                    std::string(ArtRenderer::right_prefix()) +
                    Utils::truncate_by_display_width_right(author_time, available_for_author) +
                    ArtRenderer::right_suffix();
            } else {
                right_display =
                    std::string(ArtRenderer::right_prefix()) + ArtRenderer::right_suffix();
                if (Utils::utf8_display_width(right_display) > right_max_w) {
                    right_display =
                        Utils::truncate_by_display_width_right(right_display, right_max_w);
                }
            }
        }
    }

    // Y24.2: re-center the [] in the GAP between the left and right blocks so the gaps on
    //   either side are equal (looks centered relative to the surrounding art, not just the
    //   window borders). For symmetric left/right this is identical to window-centering; it
    //   only shifts when the version and author-time blocks differ in width. Falls back to
    //   the existing window-centering if the gap is too narrow for the middle.
    {
        int lw = Utils::utf8_display_width(left_display);
        int rw = Utils::utf8_display_width(right_display);
        int gap_w = ww - rw - lw - 2; // content columns between the left and right blocks
        if (gap_w >= mid_display_w) {
            mid_x = lw + 1 + (gap_w - mid_display_w) / 2;
            if (mid_x < 1)
                mid_x = 1;
            if (mid_x + mid_display_w > ww - 1)
                mid_x = ww - 1 - mid_display_w;
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // Draw the status bar ([] centered)
    // ═══════════════════════════════════════════════════════════════════

    // Left side (starts at x=1, not exceeding the middle region's left boundary)
    wattron(win, COLOR_PAIR(left_color + 1));
    mvwprintw(win, 1, 1, "%s", left_display.c_str());
    wattroff(win, COLOR_PAIR(left_color + 1));

    // Middle (fixed center)
    if (!mid_display.empty()) {
        wattron(win, COLOR_PAIR(mid_color + 1));
        mvwprintw(win, 1, mid_x, "%s", mid_display.c_str());
        wattroff(win, COLOR_PAIR(mid_color + 1));
        // Y24.15: OSC 8 the status-bar URL — visible is the truncated mid_display, link
        //   target is the FULL url (mid_content), so Ctrl+click opens the complete URL
        //   (not the truncated one). Only when mid_content is an http(s) URL.
        if (url_hyperlink_ && mid_content.rfind("http", 0) == 0) {
            int sby = 0, sbx = 0;
            getbegyx(win, sby, sbx);
            pending_osc8_.push_back({sby + 1 + 1, sbx + mid_x + 1, mid_display, mid_content, "s"});
        }
    }

    // Right side (right-aligned, not exceeding the middle region's right boundary)
    right_display_w = Utils::utf8_display_width(right_display);
    int right_x = ww - right_display_w - 1;
    // Ensure the right side does not overlap the middle
    if (right_x < right_start_x) {
        // Space conflict, need to abbreviate
        int new_right_w = ww - right_start_x - 1;
        if (new_right_w > 0) {
            //Use modular art config constants
            int suffix_w = Utils::utf8_display_width(ArtRenderer::right_suffix());
            int prefix_w = Utils::utf8_display_width(ArtRenderer::right_prefix());
            int available_for_author = new_right_w - prefix_w - suffix_w;
            if (available_for_author > 0) {
                right_display =
                    std::string(ArtRenderer::right_prefix()) +
                    Utils::truncate_by_display_width_right(author_time, available_for_author) +
                    ArtRenderer::right_suffix();
            } else if (new_right_w >= prefix_w + suffix_w) {
                right_display =
                    std::string(ArtRenderer::right_prefix()) + ArtRenderer::right_suffix();
            } else {
                right_display = ArtRenderer::right_suffix();
                if (Utils::utf8_display_width(right_display) > new_right_w) {
                    right_display = "";
                }
            }
            right_x = right_start_x;
        } else {
            right_display = "";
            right_x = right_start_x;
        }
    }
    if (!right_display.empty()) {
        wattron(win, COLOR_PAIR(right_color + 1));
        mvwprintw(win, 1, right_x, "%s", right_display.c_str());
        wattroff(win, COLOR_PAIR(right_color + 1));
    }

    //Status bar border protection (no title embedded)
    protect_border(win, ww, 3);

    //Remove wrefresh; unified double-buffer refresh by draw()
}

} // namespace panicast
