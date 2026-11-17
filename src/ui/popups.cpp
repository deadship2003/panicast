// Popup / modal dialog rendering unit (Y24.32: extracted from ui.cpp).
//   - input_box / is_input_cancelled / dialog / show_url_popup / confirm_box / show_help
//   - These remain UI member functions (they touch private geometry w,h and INPUT_CANCELLED);
//     only their implementations live here. Declarations stay in ui.h.
#include "panicast/ui/ui.h"

#include <algorithm> // std::min / std::max / std::transform
#include <cctype>    // ::tolower
#include <string>

#include <ncurses.h>

namespace panicast
{

std::string UI::input_box(const std::string &prompt, const std::string &default_val, bool prefill) {
    // prefill=true: prefills default_val into the editable input box (for "edit current value"
    //   scenarios, e.g. proxy config — load the current value, edit/delete/save); when false,
    //   default_val is only the "Default:" prompt line and the input box starts empty (legacy behavior).
    // Extremely small terminal: return directly to avoid dereferencing NULL after newwin returns a negative-size NULL
    if (w < 10 || h < 5)
        return default_val;
    // noecho: input is manually redrawn by update_input_display; disable ncurses auto-echo —
    //   otherwise backspace left-shifts + erases the char at cursor in the echo layer, conflicting
    //   with the manual redraw and causing cursor drift / border chars being embedded.
    noecho();
    curs_set(1);

    //Compute window width, adapt to URL length
    int prompt_w = Utils::utf8_display_width(prompt);
    int default_w =
        default_val.empty() ? 0 : Utils::utf8_display_width(default_val) + 10; // "Default: " prefix
    int min_w = 50;
    int iw = std::max({prompt_w + 20, default_w + 4, min_w});

    // Limit max width to screen width - 4
    int max_w = w - 4;
    if (iw > max_w)
        iw = max_w;
    if (iw < 1)
        iw = 1; // clamp lower bound

    //If default_val is too long, the window height needs to increase to show the full URL
    int ih = 5; // default 5 rows
    int default_display_w = default_val.empty() ? 0 : Utils::utf8_display_width(default_val);
    int available_w = iw - 6; // available display width (minus borders and prefix)

    if (default_display_w > available_w) {
        // URL too long: need an extra row to show it fully, or truncate
        // Option 1: increase window height to show the full URL (but cap at 7 rows)
        // Option 2: truncate the URL display
        // We adopt the truncation approach to keep the window concise
        // url_truncated = true; // kept for future UI hint
    }

    int iy = h / 2 - ih / 2, ix = (w - iw) / 2;
    if (iy < 0)
        iy = 0;
    if (ix < 0)
        ix = 0;

    WINDOW *inp = newwin(ih, iw, iy, ix);
    if (!inp) {
        noecho();
        curs_set(0);
        return default_val;
    } // NULL check
    keypad(inp, TRUE);
    box(inp, 0, 0);

    //Determine the title based on prompt
    std::string title;
    std::string lower_prompt = prompt;
    std::transform(lower_prompt.begin(), lower_prompt.end(), lower_prompt.begin(), ::tolower);
    if (lower_prompt.find("search") != std::string::npos) {
        title = " SEARCH ";
    } else if (lower_prompt.find("title") != std::string::npos) {
        title = " EDIT TITLE ";
    } else if (lower_prompt.find("url") != std::string::npos) {
        title = " EDIT URL ";
    } else {
        title = " " + prompt + " ";
    }

    // Row 0: title centered
    int title_display_w = Utils::utf8_display_width(title);
    int title_x = (iw - title_display_w) / 2;
    if (title_x < 1)
        title_x = 1;
    mvwprintw(inp, 0, title_x, "%s", title.c_str());

    // Row 1: Default centered (V0.05B9n3e5g2RF2: long URL truncation)
    //   In prefill mode the value is already in the input box; do not repeat the Default prompt line
    if (!default_val.empty() && !prefill) {
        // Clear row 1
        wmove(inp, 1, 1);
        for (int i = 1; i < iw - 1; i++) {
            waddch(inp, ' ');
        }

        // Compute available width
        int prefix_w = 10;              // "Default: " width
        int content_available = iw - 4; // total available width (minus borders)

        std::string display_text;
        if (default_display_w <= content_available - prefix_w) {
            // Show in full
            display_text = "Default: " + default_val;
        } else {
            //URL too long: truncate and add "..." hint
            int max_url_w = content_available - prefix_w - 3; // reserve 3 chars for "..."
            std::string truncated_url = Utils::truncate_by_display_width(default_val, max_url_w);
            display_text = "Default: " + truncated_url + "...";
        }

        int display_w = Utils::utf8_display_width(display_text);
        int display_x = (iw - display_w) / 2;
        if (display_x < 1)
            display_x = 1;

        // Ensure it does not exceed the border
        if (display_x + display_w > iw - 2) {
            display_x = 2;
        }
        mvwprintw(inp, 1, display_x, "%s", display_text.c_str());
    }

    // Row 3: buttons centered
    std::string btn_enter = "[Enter]Confirm";
    std::string btn_esc = "[ESC]Cancel";
    int left_margin = iw / 6;
    int middle_gap = iw / 3;
    int btn_enter_x = left_margin;
    int btn_esc_x = left_margin + static_cast<int>(btn_enter.length()) + middle_gap;

    if (btn_esc_x + static_cast<int>(btn_esc.length()) > iw - 2) {
        btn_enter_x = 2;
        btn_esc_x = iw - static_cast<int>(btn_esc.length()) - 2;
    }

    mvwprintw(inp, 3, btn_enter_x, "%s", btn_enter.c_str());
    mvwprintw(inp, 3, btn_esc_x, "%s", btn_esc.c_str());

    wrefresh(inp);

    //Input buffer
    std::string input;
    if (prefill)
        input = default_val;            // prefill the current value, editable/deletable/savable
    size_t cursor_pos = input.length(); // caret byte offset; placed at the end when prefilled
    int max_input = iw - 4;

    // Update the input line display (centered)
    auto update_input_display = [&]() {
        // Clear row 2
        wmove(inp, 2, 1);
        for (int i = 1; i < iw - 1; i++) {
            waddch(inp, ' ');
        }

        if (!input.empty()) {
            // Compute display width of input content
            int display_width = Utils::utf8_display_width(input);
            // Centered position
            int input_x = (iw - display_width) / 2;
            if (input_x < 2)
                input_x = 2;

            //Ensure it does not exceed the border
            if (input_x + display_width > iw - 2) {
                // Input too long: truncate for display
                std::string truncated = Utils::truncate_by_display_width(input, iw - 4);
                display_width = Utils::utf8_display_width(truncated);
                input_x = 2;
                mvwprintw(inp, 2, input_x, "%s", truncated.c_str());
            } else {
                mvwprintw(inp, 2, input_x, "%s", input.c_str());
            }

            // Cursor position: at the caret (display width of the first cursor_pos bytes of input)
            int caret_w = Utils::utf8_display_width(input.substr(0, cursor_pos));
            int cx = input_x + caret_w;
            if (cx < 2)
                cx = 2;
            if (cx > iw - 2)
                cx = iw - 2;
            wmove(inp, 2, cx);
        } else {
            // Empty input: cursor centered
            wmove(inp, 2, iw / 2);
        }
        wrefresh(inp);
    };

    // Initial display
    update_input_display();

    while (true) {
        int ch = wgetch(inp);

        // P2 (Y23.4): ERR = terminal closed (SIGHUP) / stdin lost — break out instead of
        //   spinning at 100% CPU forever (the loop had no ERR branch).
        if (ch == ERR) {
            delwin(inp);
            curs_set(0);
            noecho();
            return INPUT_CANCELLED;
        }

        if (ch == '\n' || ch == KEY_ENTER) {
            break;             // confirm input
        } else if (ch == 27) { // ESC key
            // ESC may be: a real cancel / a CSI(ESC[)/SS3(ESC O) escape sequence (arrow keys/Delete
            //   etc., when keypad didn't translate) / an IME sequence. Must distinguish, to avoid
            //   escape sequences leaking as input garbage.
            nodelay(inp, TRUE);
            int next = wgetch(inp);
            if (next == ERR) {
                nodelay(inp, FALSE);
                // A real ESC key (no following bytes); cancel input
                delwin(inp);
                curs_set(0);
                noecho();
                return INPUT_CANCELLED;
            } else if (next == '[' || next == 'O') {
                // CSI(ESC[)/SS3(ESC O) sequence: read until the terminating byte (0x40~0x7E) and
                //   discard the whole thing, preventing [, letters, etc. from leaking as input garbage.
                while (true) {
                    int b = wgetch(inp);
                    if (b == ERR || (b >= 0x40 && b <= 0x7E))
                        break;
                }
                nodelay(inp, FALSE);
            } else {
                // Other ESC sequences (IME etc.): put it back and continue
                ungetch(next);
                nodelay(inp, FALSE);
            }
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            // Backspace: delete the UTF-8 char before the caret (caret shifts left with it)
            if (cursor_pos > 0) {
                size_t pos = cursor_pos - 1;
                while (pos > 0 && (input[pos] & 0xC0) == 0x80)
                    pos--;
                input.erase(pos, cursor_pos - pos);
                cursor_pos = pos;
                update_input_display();
            }
        } else if (ch == KEY_DC) {
            // Delete: delete the UTF-8 char at the caret (caret stays)
            if (cursor_pos < input.length()) {
                size_t pos = cursor_pos + 1;
                while (pos < input.length() && (input[pos] & 0xC0) == 0x80)
                    pos++;
                input.erase(cursor_pos, pos - cursor_pos);
                update_input_display();
            }
        } else if (ch == KEY_LEFT) {
            // Move caret left one UTF-8 char
            if (cursor_pos > 0) {
                size_t pos = cursor_pos - 1;
                while (pos > 0 && (input[pos] & 0xC0) == 0x80)
                    pos--;
                cursor_pos = pos;
                update_input_display();
            }
        } else if (ch == KEY_RIGHT) {
            // Move caret right one UTF-8 char
            if (cursor_pos < input.length()) {
                size_t pos = cursor_pos + 1;
                while (pos < input.length() && (input[pos] & 0xC0) == 0x80)
                    pos++;
                cursor_pos = pos;
                update_input_display();
            }
        } else if (ch == KEY_HOME) {
            if (cursor_pos != 0) {
                cursor_pos = 0;
                update_input_display();
            }
        } else if (ch == KEY_END) {
            if (cursor_pos != input.length()) {
                cursor_pos = input.length();
                update_input_display();
            }
        } else if (ch == 21) { // CTRL+U clears all input
            if (!input.empty()) {
                input.clear();
                cursor_pos = 0;
                update_input_display();
            }
        } else if (ch >= 32 && ch < 127) {
            // ASCII char: insert at the caret
            if (input.length() < (size_t)(max_input - 2)) {
                input.insert(cursor_pos, 1, (char)ch);
                cursor_pos += 1;
                update_input_display();
            }
        } else if (ch >= 0x100) {
            // Other ncurses function keys (PgUp/PgDn/F1.. etc.): ignore, to avoid falling into the UTF-8 branch and producing garbage.
            // (KEY_ENTER/BACKSPACE/DC/LEFT/RIGHT/HOME/END are already handled above.)
        } else if (ch >= 0x80) {
            // UTF-8 char: insert the full character at the caret
            int expected = 0;
            if ((ch & 0xE0) == 0xC0)
                expected = 1;
            else if ((ch & 0xF0) == 0xE0)
                expected = 2; // Chinese falls into this category
            else if ((ch & 0xF8) == 0xF0)
                expected = 3;
            if (input.length() + expected + 1 < (size_t)(max_input - 2)) {
                std::string bytes(1, (char)ch);
                for (int i = 0; i < expected; i++) {
                    int next = wgetch(inp);
                    if (next != ERR && next > 0)
                        bytes += (char)next;
                }
                input.insert(cursor_pos, bytes);
                cursor_pos += bytes.size();
                update_input_display();
            }
        }
    }

    delwin(inp);
    curs_set(0);
    noecho();

    // Prefill mode: return the user's actual input (including an empty string after clearing) — clearing means clearing,
    //   do not fall back to default_val (otherwise clearing the proxy would be overwritten by the old value, and the proxy could not be cleared).
    // Non-prefill mode: empty input = adopt the Default prompt value (legacy behavior).
    return (prefill || !input.empty()) ? input : default_val;
}

bool UI::is_input_cancelled(const std::string &result) {
    return result == INPUT_CANCELLED;
}

std::string UI::dialog(const std::string &msg) {
    // Extremely small terminal protection + iw uses display width not byte count (CJK) + NULL check
    if (w < 12 || h < 5)
        return "";
    echo();
    curs_set(1);

    int msg_w = Utils::utf8_display_width(msg);
    int ih = 3, iw = std::min(msg_w + 10, w - 10);
    if (iw < 1)
        iw = 1;
    int iy = h / 2 - 1, ix = (w - iw) / 2;
    if (iy < 0)
        iy = 0;
    if (ix < 0)
        ix = 0;

    WINDOW *dlg = newwin(ih, iw, iy, ix);
    if (!dlg) {
        noecho();
        curs_set(0);
        return "";
    }
    box(dlg, 0, 0);
    std::string disp = Utils::truncate_by_display_width(msg, std::max(1, iw - 4));
    mvwprintw(dlg, 1, 2, "%s", disp.c_str());
    wrefresh(dlg);

    char buf[16] = {0};
    int input_x = std::min((int)(msg_w + 3), iw - 4);
    if (input_x < 2)
        input_x = 2;
    mvwgetnstr(dlg, 1, input_x, buf, sizeof(buf) - 1);

    delwin(dlg);
    noecho();
    curs_set(0);

    return std::string(buf);
}

void UI::show_url_popup(const std::string &url, bool copied) {
    if (w < 30 || h < 7) { // extremely small terminal protection
        EVENT_LOG(fmt::format("URL: {}", url));
        return;
    }
    int iw = std::min(w - 4, 88);
    if (iw < 30)
        iw = 30;
    int inner_w = iw - 4; // 2-column margin on each side
    auto lines = Utils::wrap_text(url, inner_w, 24);
    if (lines.empty())
        lines.push_back(url);
    int ih = std::min(h - 4, (int)lines.size() + 4);
    int ix = (w - iw) / 2, iy = (h - ih) / 2;
    if (ix < 0)
        ix = 0;
    if (iy < 0)
        iy = 0;
    WINDOW *win = newwin(ih, iw, iy, ix);
    if (!win) {
        EVENT_LOG(fmt::format("URL: {}", url));
        return;
    }
    keypad(win, TRUE);
    box(win, 0, 0);
    std::string title =
        copied ? " Stream URL — copied to clipboard " : " Stream URL — select with mouse to copy ";
    mvwprintw(win, 0, 2, "%s", Utils::truncate_by_display_width(title, iw - 4).c_str());
    int yy = 1;
    for (const auto &ln : lines) {
        if (yy >= ih - 2)
            break; // reserve the bottom hint line
        mvwaddstr(win, yy++, 2, ln.c_str());
    }
    mvwaddstr(win, ih - 2, 2, "(press any key to close)");
    wrefresh(win);
    wgetch(win);
    delwin(win);
}

void UI::show_pin_popup(const std::string &dynamic_pin, const std::string &universal_pin) {
    if (w < 30 || h < 9) {
        EVENT_LOG(fmt::format("Remote PIN: {} (universal: {})", dynamic_pin, universal_pin));
        return;
    }
    int iw = std::min(w - 4, 46);
    if (iw < 30)
        iw = 30;
    int ih = 9;
    int ix = (w - iw) / 2, iy = (h - ih) / 2;
    if (ix < 0)
        ix = 0;
    if (iy < 0)
        iy = 0;
    WINDOW *win = newwin(ih, iw, iy, ix);
    if (!win) {
        EVENT_LOG(fmt::format("Remote PIN: {} (universal: {})", dynamic_pin, universal_pin));
        return;
    }
    keypad(win, TRUE);
    box(win, 0, 0);

    // Center a string on row y (within the inner width).
    auto center = [&](const std::string &s, int y) {
        int x = (iw - (int)s.size()) / 2;
        if (x < 1)
            x = 1;
        mvwaddstr(win, y, x, s.c_str());
    };

    // Title on the top border (centered).
    std::string title = " Remote Pairing ";
    mvwprintw(win, 0, (iw - (int)title.size()) / 2, "%s", title.c_str());

    center("panicast Remote Control", 2);
    center("Pairing PIN", 3);
    // Dynamic PIN with spaces between digits for emphasis, bold.
    std::string spaced;
    for (size_t i = 0; i < dynamic_pin.size(); ++i) {
        if (i)
            spaced += ' ';
        spaced += dynamic_pin[i];
    }
    wattron(win, A_BOLD);
    center(spaced, 4);
    wattroff(win, A_BOLD);
    center("Universal: " + universal_pin, 6);
    center("(press any key to close)", 7);

    wrefresh(win);
    wgetch(win);
    delwin(win);
}

bool UI::confirm_box(const std::string &prompt) {
    // Extract the title from prompt (strip possible "?" / "(CTRL+C)" etc. suffixes)
    std::string title = prompt;
    size_t qpos = title.find('?');
    if (qpos != std::string::npos) {
        title = title.substr(0, qpos);
    }

    // Border title format: " QUIT panicast "
    std::string border_title = " " + title + " ";
    int border_title_w = Utils::utf8_display_width(border_title);

    if (w < 20 || h < 5)
        return false; // extremely narrow/short terminal protection, to avoid negative coords like iw-7

    // Window size: minimum width must accommodate the border title
    int min_w = std::max(border_title_w + 4, 24);
    int ih = 4, iw = min_w; // height reduced by one row (drop the in-box title)
    if (iw > w - 2)
        iw = std::max(1, w - 2); // do not exceed screen width
    int iy = h / 2 - ih / 2, ix = (w - iw) / 2;
    if (iy < 0)
        iy = 0;
    if (ix < 0)
        ix = 0;

    WINDOW *dlg = newwin(ih, iw, iy, ix);
    if (!dlg)
        return false; // NULL check
    keypad(dlg, TRUE);

    // Draw the top border, with the title in the middle
    // ┌── QUIT panicast ──┐
    waddch(dlg, ACS_ULCORNER); // ┌
    int side_dashes = (iw - 2 - border_title_w) / 2;
    if (side_dashes < 0)
        side_dashes = 0; // guard against negative
    for (int i = 0; i < side_dashes; i++)
        waddch(dlg, ACS_HLINE); // ─
    // Truncate the title if too wide, to avoid breaking through the right border
    std::string draw_title = border_title;
    int max_title_w = std::max(1, iw - 2);
    if (border_title_w > max_title_w) {
        draw_title = Utils::truncate_by_display_width(border_title, max_title_w);
    }
    wprintw(dlg, "%s", draw_title.c_str()); // title
    int remaining = iw - 2 - side_dashes - Utils::utf8_display_width(draw_title);
    if (remaining < 0)
        remaining = 0;
    for (int i = 0; i < remaining; i++)
        waddch(dlg, ACS_HLINE); // ─
    waddch(dlg, ACS_URCORNER);  // ┐

    // Draw side borders and bottom border
    mvwaddch(dlg, 1, 0, ACS_VLINE);
    mvwaddch(dlg, 1, iw - 1, ACS_VLINE);
    mvwaddch(dlg, 2, 0, ACS_VLINE);
    mvwaddch(dlg, 2, iw - 1, ACS_VLINE);
    mvwaddch(dlg, 3, 0, ACS_LLCORNER);
    for (int i = 1; i < iw - 1; i++)
        waddch(dlg, ACS_HLINE);
    waddch(dlg, ACS_LRCORNER);

    //YES/NO placed on both sides of the lower part, more aesthetic
    // [Y]es on the left, [N]o on the right
    mvwprintw(dlg, 2, 2, "[Y]es");
    mvwprintw(dlg, 2, iw - 7, "[N]o");

    // Force the dialog on top of any stale window content from the previous frame
    //   (deleted but leaves pixels on the physical screen).
    //   touchwin marks every cell dirty so wrefresh repaints the whole dialog, including its
    //   blank interior, over the underlying list — otherwise the dialog can appear obscured.
    touchwin(dlg);
    wrefresh(dlg);

    // Wait for user input
    int ch;
    bool result = false;
    while ((ch = wgetch(dlg)) != ERR) {
        if (ch == 'y' || ch == 'Y') {
            result = true;
            break;
        } else if (ch == 'n' || ch == 'N' || ch == 27) { // ESC also cancels
            result = false;
            break;
        }
    }

    delwin(dlg);
    return result;
}

void UI::show_help(const MPVController::State &state) {
    draw_help(nullptr, state, 0);
}

} // namespace panicast
