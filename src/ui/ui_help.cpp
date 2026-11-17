// UI rendering layer — extracted implementation unit (D17 god-object split).
//   draw_help(): the scrollable help/keymap overlay window. The method remains
//   a UI member (it reads private UI screen size h/w); only its implementation
//   lives here. Declaration stays in ui.h. Mechanical verbatim move from ui.cpp.
#include "panicast/ui/ui.h"

#include <algorithm>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "panicast/core/constants.h"
#include "panicast/core/utils.h"

namespace panicast
{

void UI::draw_help(WINDOW *win, const MPVController::State &state, int cw) {
    (void)win;
    (void)state;
    (void)cw; // keep params for future use
    //Help content definition, including all key definitions
    //Update hotkey descriptions
    static const std::vector<std::string> help_lines = {
        fmt::format("{} {}★ - Help", APP_NAME, VERSION),
        "",
        "---- Navigation ----",
        "  k / j      Move up/down",
        "  g / G      Go to first/last item",
        "  PgUp/PgDn  Page up/down",
        "  N          Jump to the currently playing node (Now playing; switches to its mode)",
        "  h          Collapse/Go back",
        "  l / Enter  Expand/Play (marked: add to playlist)",
        "",
        "---- Playback ----",
        "  Space / p  Play/Pause",
        "  - / +      Volume down / up",
        "  [ / ]      Speed slower/faster",
        "  \\          Reset speed to 1.0x",
        "",
        "---- Video/Audio Quality (INI) ----",
        "  play_format_video  = bestvideo[height<=1080]+bestaudio/best (1080p DASH, default)",
        "  play_format_audio  = bestaudio/best[height<=144] (smallest muxed fallback, default)",
        "  state_refresh_ms   = 100 (playback-state/Network refresh interval, ms)",
        "",
        "---- Playlist ----",
        "  (implicit) The peers (siblings) of the playing episode are the playlist; auto-advance "
        "per play mode",
        "  C          Clear playlist (keep playing) — stop auto-advance, current track keeps "
        "playing",
        "  L          LYRIC bar toggle; in F mode → transcribe selected/v-marked "
        "(whisper.cpp→.srt)",
        "  Ctrl+L     Cycle theme (22 palettes: Solarized "
        "Dark/Gruvbox/Nord/Dracula/Catppuccin/...)",
        "  Ctrl+N     Configure network proxy (socks5h:// / http:// ...)",
        "  Ctrl+B     Set cookies.txt (context: YouTube / Bilibili / TikTok-Douyin)",
        "",
        "---- Actions ----",
        "  a          Add feed (PODCAST) / Subscribe (ONLINE) / Add local folder (FAVOURITE)",
        "  d          Delete node/record (all modes)",
        "  D          Download (single, or batch if marks set)",
        "  b          Switch region (ONLINE/TIKTOK mode)",
        "  e          Edit node title/URL",
        "  f          Add to Favourites",
        "  m          Toggle mark",
        "  v          Enter Visual mode",
        "  V          Clear all marks",
        "  C          Clear playlist (keep playing)",
        "  r          Refresh node",
        "  o          Toggle sort order (asc/desc)",
        "",
        "---- Modes ----",
        "  R          Radio mode",
        "  P          Podcast mode",
        "  F          Favourite mode",
        "  H          History mode",
        "  O          Online (iTunes) mode",
        "  Y          Account (Google) mode  [Y01]",
        "  B          Bilibili mode  [Y15]",
        "  T          TikTok / Douyin mode  [Y24.11]",
        "  M          Cycle through all modes",
        "",
        "---- UI Settings ----",
        "  S          Toggle scroll mode",
        "  U          Toggle icon style (ASCII/Emoji)",
        "  Ctrl+Y     Copy playing stream URL to clipboard (cursor node if idle)",
        "",
        "---- Search ----",
        "  /          Search (local) / Search iTunes (ONLINE) / Search YouTube (Y mode)",
        "             Y-mode YouTube search: c/v/p/m prefix filters channel/video/playlist/music",
        "  J / K      Jump to next/prev match",
        "",
        "---- Y Mode (Google Accounts) [Y01] ----",
        "  a          Login (scan QR) / Subscribe [C] channel result to active account",
        "  A          Login ANOTHER Google account",
        "  l / Enter  Activate account / expand History/Subscriptions/Search",
        "  j / k      Select account",
        "  r          Re-sync account / refresh Subs·History node",
        "  d          Delete selected account",
        "  :secret    Import Google client_secret.json into data dir (Y-mode OAuth client)",
        "  (active account = primary, default 1#; expand another to switch)",
        "",
        "---- Playback ----",
        "  l / Enter  Play selected episode (its peers become the list)",
        "  Space / p  Pause / resume",
        "  :          Command window — play mode (≥2-char prefix) or an mpv hotkey (single char)",
        "    :re/:sh/:cy or :repeat/:shuffle/:cycle (≥2 chars, prefix match)",
        "    mpv hotkeys (aligned to mpv native; type the char after `:`):",
        "      video:  + zoom in | - zoom out | = reset zoom | f fullscreen | A aspect | d "
        "deinterlace",
        "      sub:    F/G size -/+ | z/Z sync -/+ | r/R pos up/down | v hide-show | j/J track "
        "next/prev",
        "      audio:  # cycle track | m mute",
        "      osd:    o progress | O level | i stats | I stats-toggle",
        "      other:  l ab-loop | s screenshot | S screenshot(no sub) | 1-8 "
        "contrast/bright/gamma/sat",
        "  INFO area shows: 3 history ^ / current / 3 next v  +  Network: speed | Buffering: sec",
        "",
        "---- B Mode (Bilibili) [Y15] ----",
        "  a          Login (QR scan → SESSDATA cookie, like Y mode OAuth)",
        "  A          Login another Bilibili account (QR scan)",
        "  Ctrl+B     Set cookie file path (context-aware, same as Y mode)",
        "  l / Enter  Expand account (following list, WBI API) / UP master (video list) / play",
        "  /          Search Bilibili (yt-dlp bilisearch)",
        "  r          Re-fetch followings / history / account",
        "  (QR=cookie; followings need cookie; video list uses WBI; HD playback needs cookie)",
        "  (no quickjs/deno needed — Bilibili has no nsig)",
        "",
        "---- T Mode (TikTok / Douyin) [Y24.11/16] ----",
        "  T          Enter T mode (TikTok + Douyin, no region grouping)",
        "  a          Douyin QR login (terminal) — writes douyin_cookie.txt",
        "  /          Add/Open: @user / #tag / tiktok.com or douyin.com video URL",
        "             (NO keyword search — anonymous infeasible)",
        "  l / Enter  Expand creator → video list (yt-dlp) / play video",
        "  r          Re-fetch creator video list (replace local cache)",
        "  d          Delete the item under the cursor",
        "  Ctrl+B     Import TikTok cookies.txt (Netscape); Douyin cookies come from login",
        "  (TikTok anonymous; Douyin needs cookies.txt + CN network exit)",
        "  (playback via mpv ytdl_hook; yt-dlp has no DouyinUserIE so Douyin is single-video only)",
        "",
        "---- Command Line ----",
        "  -a <url>   Add feed from URL",
        "  -i <file>  Import OPML subscriptions",
        "  -e <file>  Export podcasts to OPML",
        "  -t <time>  Sleep timer (5h/30m/1:25:15)",
        "  --purge    Clear cache (preserves subscriptions/history/favourites)",
        "  --quiet    Pure audio mode (vo=null, vid=no)",
        "  --vid <val> Override video track (auto/no)",
        "  --vo <val> Override video output (auto/null/gpu/wlshm)",
        "  --ao <val> Override audio output (default pulse,alsa; or pulse/alsa/pipewire/auto)",
        "",
        "---- Data Storage ----",
        "  Database: ~/.local/share/panicast/panicast.db",
        "  Config:   ~/.config/panicast/config.ini",
        "  Downloads: ~/Downloads/panicast/",
        "  Log:      ~/.local/share/panicast/panicast-YYYYMMDD.log (daily, kept 365 days)",
        "",
        "  Note: All data in SQLite database.",
        "",
        "---- Contact ----",
        "  Email:  Deadship2003@gmail.com",
        "",
        "Press 'q' or '?' to close"};

    // V0.03: compute the required window size
    int content_height = help_lines.size();
    int content_width = 0;
    for (const auto &line : help_lines) {
        int w = Utils::utf8_display_width(line) + 4; // border + margin
        if (w > content_width)
            content_width = w;
    }

    // V0.03: limit max size to 90% of the screen
    int max_h = (int)(h * 0.9);
    int max_w = (int)(w * 0.9);
    int help_h = std::min(content_height + 2, max_h); // +2 for border
    int help_w = std::min(content_width, max_w);

    // Ensure minimum size
    if (help_h < 10)
        help_h = 10;
    if (help_w < 40)
        help_w = 40;

    int help_y = (h - help_h) / 2;
    int help_x = (w - help_w) / 2;
    if (help_y < 0)
        help_y =
            0; // when terminal is too short, stick to the top, to avoid newwin returning NULL on negative coords

    WINDOW *help_win = newwin(help_h, help_w, help_y, help_x);
    if (!help_win)
        return; // NULL guard: avoid box(NULL) crash
    keypad(help_win, TRUE);
    // Anti-flicker: keep the terminal cursor out of the refresh path — wgetch() repositions
    //   the physical cursor to the window's cursor on every keypress, which reads as
    //   flicker while scrolling.
    leaveok(help_win, TRUE);

    // Draw the border ONCE — it never changes; redrawing it on every scroll step flashed
    //   the whole overlay region.
    box(help_win, 0, 0);

    // V0.03: if content overflows the window, add scrolling
    int scroll_offset = 0;
    bool needs_scroll = content_height > help_h - 2;

    auto draw_content = [&]() {
        if (scroll_offset < 0)
            scroll_offset = 0; // defensive clamp
        int y = 1;
        int visible_lines = help_h - 2;

        // Clear ONLY the inner rows (mvwhline stops short of the border columns) — the old
        //   werase()+box() full-window repaint flashed the overlay on every j/k.
        for (int row = 1; row < help_h - 1; ++row)
            mvwhline(help_win, row, 1, ' ', help_w - 2);

        for (int i = scroll_offset; i < content_height && y < help_h - 1; ++i) {
            const std::string &line = help_lines[i];
            std::string display = Utils::truncate_by_display_width(line, help_w - 4);

            if (i == 0) {
                //Title centered
                int title_width = Utils::utf8_display_width(display);
                int x_pos = (help_w - title_width) / 2;
                if (x_pos < 2)
                    x_pos = 2;
                wattron(help_win, A_BOLD);
                mvwprintw(help_win, y++, x_pos, "%s", display.c_str());
                wattroff(help_win, A_BOLD);
            } else if (line.find("----") == 0) {
                // Section heading
                wattron(help_win, A_DIM);
                mvwprintw(help_win, y++, 2, "%s", display.c_str());
                wattroff(help_win, A_DIM);
            } else if (line == "Press 'q' or '?' to close") {
                // Bottom hint
                wattron(help_win, A_DIM);
                mvwprintw(help_win, y++, 2, "%s", display.c_str());
                wattroff(help_win, A_DIM);
            } else {
                mvwprintw(help_win, y++, 2, "%s", display.c_str());
            }
        }

        // V0.03: show scroll indicators
        if (needs_scroll) {
            if (scroll_offset > 0) {
                mvwprintw(help_win, 0, help_w - 6, "▲");
            }
            if (scroll_offset + visible_lines < content_height) {
                mvwprintw(help_win, help_h - 1, help_w - 6, "▼");
            }
        }

        wrefresh(help_win);
    };

    draw_content();

    // V0.03: scrolling key handling
    //Add g/G jump to top/bottom
    int ch;
    while ((ch = wgetch(help_win)) != 'q' && ch != '?' && ch != 27) {
        if (ch == 'k' || ch == KEY_UP) {
            if (scroll_offset > 0) {
                scroll_offset--;
                draw_content();
            }
        } else if (ch == 'j' || ch == KEY_DOWN) {
            if (scroll_offset + help_h - 2 < content_height) {
                scroll_offset++;
                draw_content();
            }
        } else if (ch == KEY_PPAGE) {
            scroll_offset = std::max(0, scroll_offset - 5);
            draw_content();
        } else if (ch == KEY_NPAGE) {
            // Only allow paging when scrolling is needed, and clamp the lower bound with std::max —
            //   the original code's content_height-help_h+2 went negative when content fit in the window,
            //   making scroll_offset negative and draw_content access help_lines with a negative index, crashing
            if (needs_scroll) {
                scroll_offset =
                    std::max(0, std::min(content_height - help_h + 2, scroll_offset + 5));
                draw_content();
            }
        } else if (ch == 'g') {
            //Jump to top
            scroll_offset = 0;
            draw_content();
        } else if (ch == 'G') {
            //Jump to bottom
            scroll_offset = std::max(0, content_height - help_h + 2);
            draw_content();
        }
    }

    delwin(help_win);
}
} // namespace panicast
