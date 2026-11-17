// UI rendering layer: DisplayItem display entry + UI terminal renderer (tree/panels/statusbar/popups/inputbox).
//   - init/cleanup/handle_resize lifecycle; draw is the main render entry
//   - draw_line/draw_info/draw_status/draw_help and other sub-region drawing
//   - theme system (themes/apply_theme/cycle_theme/init_status_color_pairs) kept with UI
// References already-extracted layers: LayoutMetrics/LayoutGuard/IconManager/ArtRenderer/Border/Colors/Pairs
#pragma once

#include <string>
#include <vector>
#include <set>
#include <ctime>
#include <cstdlib>
#include <atomic>
#include <csignal>
#include <unistd.h>
#include <sys/ioctl.h>
#include <locale.h>
#include <langinfo.h>

#include "panicast/theme/themes.h"
#include "panicast/ui/theme_manager.h" // Y24.30: extracted from UI   // Y11: 12-palette table (struct Theme, themes(), THEME_COUNT)

#include <ncurses.h>
#include <fmt/format.h>
#include <fmt/chrono.h>

#include "panicast/core/types.h"
#include "panicast/core/constants.h"
#include "panicast/core/logger.h"
#include "panicast/core/event_log.h"
#include "panicast/core/terminal.h"
#include "panicast/core/platform.h"
#include "panicast/core/utils.h"
#include "panicast/config/ini_config.h"
#include "panicast/net/url_classifier.h"
#include "panicast/net/tiktok_region.h" // Y24.12: T-mode region badge in the mode title
#include "panicast/parsers/itunes_search.h"
#include "panicast/parsers/transcript_parser.h"
#include "panicast/ui/icons.h"
#include "panicast/ui/art.h"
#include "panicast/ui/layout_metrics.h"
#include "panicast/ui/layout_guard.h"
#include "panicast/ui/border.h"
#include "panicast/theme/colors.h"
#include "panicast/theme/pairs.h"
#include "panicast/storage/cache.h"
#include "panicast/playback/mpv_controller.h"
#include "panicast/playback/sleep_timer.h"
#include "panicast/app/progress.h"
#include "panicast/app/online_state.h"
#include "panicast/ui/frontend.h" // D12-3b: IFrontend contract + the ncurses-free view-model types
                                   //   (DisplayItem/DisplayContext/LyricManual) UI renders from.

namespace panicast
{

// Right panel title embed region within border (column index)
constexpr int TITLE_EMBED_START = 2;      // Right panel title embed start column (" INFO & LOG ")
constexpr int TITLE_EMBED_END = 14;       // Right panel title embed end column
constexpr int SPLIT_TITLE_EMBED_END = 13; // Separator title embed end column (" Event Log ")

// Terminal/ncurses global state (defined in ui.cpp, read/written by UI::init/cleanup)
extern bool g_ncurses_initialized;
extern int g_original_lines;
extern int g_original_cols;
// Terminal cleanup (called from atexit/signal paths; UI::cleanup reuses it)
void tui_cleanup();

// Terminal state save/restore (save on main startup, restore inside tui_cleanup)
void save_terminal_state();
// Signal handler registration (SIGINT/SIGTERM/SIGSEGV etc.; defined in ui.cpp)
void setup_signal_handlers();
// SIGINT exit-request flag (set by signal handler, polled by App main loop)
extern std::atomic<bool> g_exit_requested;
// Termination-signal indicator: set by the handler for SIGHUP/SIGTERM/SIGQUIT (terminal closed /
//   killed). The main loop uses it to flush state and exit WITHOUT a confirm popup (there may be no
//   terminal to interact with), so in-flight YouTube parses finish writing their cache before exit.
extern volatile sig_atomic_t g_crash_sig;

// D12-3b: DisplayItem + DisplayContext (the ncurses-free view-model types) moved to frontend.h;
//   UI inherits IFrontend and renders from them.

// UILayout struct removed: duplicated LayoutMetrics and was never instantiated (dead code).
// Actual layout computation goes through LayoutGuard::compute + LayoutMetrics uniformly.
// =========================================================
// UI rendering - V2.39: fix scrolling display and right alignment
// =========================================================
class UI : public IFrontend {
public:
    void init(float ratio = 0.4f) override;

    void cleanup() override;

    // Proactively handle resize: reset cached size to force recomputation on next frame
    void handle_resize() override;

    void toggle_tree_lines();

    // Y23.4 (method B): set the parsed transcript for the current track. Empty segs clears it
    //   (falls back to mpv sub-text for video subtitles).
    void set_transcript(const std::vector<TranscriptSegment> &segs, const std::string &url) override;

    void toggle_scroll_mode() override;

    void set_scroll_mode(bool mode) override {
        scroll_mode_ = mode;
    }
    void set_show_tree_lines(bool show) override {
        show_tree_lines_ = show;
    }
    bool is_show_tree_lines() const override {
        return show_tree_lines_;
    }

    bool is_scroll_mode() const override {
        return scroll_mode_;
    }

    // Y24: L-mode (LYRIC bar) controls. `lyric_bar_requested_` is the INI master switch (is the
    //   LYRIC feature enabled at all); `lyric_bar_active_` is derived per-frame by App before draw().
    bool is_lyric_bar_requested() const override {
        return lyric_bar_requested_;
    }
    bool is_lyric_bar_active() const override {
        return lyric_bar_active_;
    }
    void set_lyric_bar_active(bool active) override {
        lyric_bar_active_ = active;
    }
    // Y24.43: set the user's LYRIC-bar request state (persisted). Distinct from set_lyric_bar_active
    //   (per-frame derived). Used by the L key to open/close without the flip ambiguity of toggle.
    void set_lyric_bar_requested(bool requested) override;
    void toggle_lyric_bar() override;

    // Y24.48: LyricManual (Auto/Open/Closed) is defined on IFrontend (frontend.h); UI inherits it.
    LyricManual lyric_manual() const override {
        return lyric_manual_;
    }
    void set_lyric_manual(LyricManual m) override {
        lyric_manual_ = m;
    }
    // Y24.48: true once an embedded mp4 sub cue (sub_text) has been seen for the current track.
    //   Sticky per-track (avoids flicker between cues); reset on track change.
    bool embedded_sub_confirmed() const override {
        return embedded_sub_confirmed_;
    }

    // Y24: per-frame lyric history update — shared by the right-panel LYRIC region (L off) and
    //   the bottom LYRIC bar (L on). Computes the current lyric line from the parsed transcript
    //   (method B, by time_pos) or mpv sub-text, and pushes it to lyric_history_ (deduped, capped).
    //   Called unconditionally so the history stays fresh regardless of which panel renders it.
    void update_lyric_history(const MPVController::State &state) override;

    // Y24.2: the LYRIC bar border title — "🎵 LYRIC" plus the live sync offset when non-zero.
    std::string lyric_bar_title() const;

    // Y24.6: shared LYRIC renderer — used by BOTH the L-mode bottom bar (draw_lyric_bar) and the
    //   right-panel LYRIC region (draw_info, L off). Same display logic, different display area:
    //   the caller picks the window, start row, row count and inner width.
    //   Renders a window of segments centered on the CURRENT line (prev above / current middle /
    //   next below), current bold+green; ALL active segments (overlapping speakers) bolded; short
    //   lines horizontally centered, long lines marquee. Falls back to lyric_history_ (current at
    //   bottom row) when there's no transcript segment list (mpv sub-text only).
    void draw_lyric_content(WINDOW *win, int y_start, int rows, int inner_w,
                            const MPVController::State &state);

    // Y24: full-width bottom LYRIC bar (replaces the status bar when L-mode is active).
    //   Thin wrapper around draw_lyric_content: draw the box + title, then render `lyric_bar_lines_`
    //   content rows starting at row 1 (below the top border), full terminal width.
    void draw_lyric_bar(WINDOW *win, const MPVController::State &state);

    void draw(AppMode mode, const std::vector<DisplayItem> &list, int selected,
              const MPVController::State &state, int view_start, AppState app_state,
              int marked_count, const std::string &search_query,
              int current_match, int total_matches, TreeNodePtr selected_node,
              const std::vector<DownloadProgress> &downloads, bool visual_mode, int visual_start,
              const std::vector<PlaylistItem> &playlist = {}, int playlist_index = -1,
              //Play mode + INFO play-context (7-line: 3 history + current + 3 next)
              PlayMode play_mode = PlayMode::CYCLE,
              const std::vector<std::string> &history_titles = {},
              const std::vector<int> &next_indices = {},
              // D12-1: ambient runtime display state (sleep timer + regions). Defaulted so the
              //   single existing caller can adopt it incrementally; never left empty in practice.
              const DisplayContext &dctx = {}) override;

    //Input cancel marker (uses string concatenation to avoid hex-escape issues)
    static constexpr const char *INPUT_CANCELLED =
        "\x01"
        "CANCELLED"
        "\x01"; // special marker indicating user cancellation

    //Fix UTF-8 Chinese/CJK input + URL adaptive display
    // Reference V0.05B9n3d version: complete Chinese/CJK IME support
    //Improvement: window width adapts to URL length, long URLs auto-truncated
    std::string input_box(const std::string &prompt, const std::string &default_val = "",
                          bool prefill = false) override;

    //Check whether the result is the cancel marker
    static bool is_input_cancelled(const std::string &result);

    std::string dialog(const std::string &msg) override;

    // Show full stream URL popup: wrap by display width to ensure the full URL is visible and selectable with the terminal mouse
    //   (the program does not enable mousemask, so the terminal's native text selection works). copied=true means it has been written to the clipboard.
    void show_url_popup(const std::string &url, bool copied);
    // N04: centered pairing-PIN popup (dynamic + universal). Larger, centered text for
    //   readability when pairing a phone/browser.
    void show_pin_popup(const std::string &dynamic_pin, const std::string &universal_pin) override;

    //Y/N confirmation dialog
    //YES/NO placed on both sides of the lower part
    //Title shown in the middle of the border's top line, not repeated inside the box
    bool confirm_box(const std::string &prompt = "Quit?") override;

    // V2.39-FF: public method, show help popup
    void show_help(const MPVController::State &state) override;

    // ─── Theme system: 15 color schemes (cycle with Ctrl+L) ─────────────────────────
    // Index 0 = classic Dark (keeps the original pure-ANSI black background, author's favorite, do not change)
    // Index 1 = Solarized Light (keeps the original light eye-friendly scheme)
    // Index 2-8 = popular Linux community schemes: Solarized Dark / Dracula / Gruvbox /
    //            Nord / Monokai / Rose Pine / Catppuccin Mocha
    //   Picks the combination with the largest tonal differences (warm brown/purple/blue/green/rose/lavender), avoiding multiple similar schemes.
    // Each theme redefines the 8 ANSI colors (COLOR_BLACK..COLOR_WHITE).
    //   Dark theme: COLOR_BLACK=background, COLOR_WHITE=foreground; light theme the reverse.
    // Y11: theme palettes moved to a dedicated file (src/theme/themes.cpp, declared in
    //   panicast/theme/themes.h) so the 15 palettes can be edited without touching UI code.
    //   struct Theme / THEME_COUNT / themes() are at namespace panicast scope (name lookup).
    // The foreground of state color pairs (pair 10-15) still comes from the [colors] config (color name -> corresponding color in the theme palette),
    //   so switching themes makes "downloaded=green" automatically use that theme's green, keeping things coherent.

    //Cycle theme (Ctrl+L)
    void toggle_theme() override {
        ThemeManager::instance().toggle();
        EVENT_LOG(fmt::format("Theme: {}", ThemeManager::instance().current_name()));
    }

    //Current theme name
    std::string current_theme_name() const {
        return ThemeManager::instance().current_name();
    }

    // Left panel geometry (for mouse-click hit testing): origin (0,0), height top_h, width left_w
    int get_left_w() const override {
        return left_w;
    }
    int get_top_h() const override {
        return top_h;
    }

    // Initialize node-tree state color pairs (pair 10-15): foreground from [colors] config, background follows the theme.
    //   pair10=stream cached cyan   pair11=currently playing green   pair12=db cached yellow
    //   pair13=parse failed red   pair14=info blue      pair15=downloaded green (default)
    // Note: pair15 is only for "downloaded" nodes, so the default is green to convey "cached and available".
    //   pair11 (currently playing) overlays A_BOLD in draw_line, distinguished from pair15's plain green.
    // Y24.30: init_state_color_pairs moved to ThemeManager.

    //Apply theme colors (table-driven, 15 schemes)
    void apply_theme() {
        ThemeManager::instance().apply();
    } // Y24.30: delegate

private:
    WINDOW *left_win = nullptr, *right_win = nullptr, *status_win = nullptr,
           *lyric_win = nullptr; // initialized to nullptr
    int h = 0, w = 0, left_w = 0, right_w = 0, top_h = 0;
    int cols_ = 0; //track previous column count, used to detect resize
    // Y24: L-mode (LYRIC bar). requested = user intent (INI-persisted); active = derived
    //   (requested && transcript READY), set per-frame by App via set_lyric_bar_active().
    //   last_lyric_bar_active_ detects active↔inactive transitions to force a full redraw
    //   (top_h changes → resize + clearok) and avoid buffer artifacts (garbled screen).
    bool lyric_bar_requested_ = false;
    bool lyric_bar_active_ = false;
    bool last_lyric_bar_active_ = false;
    LyricManual lyric_manual_ = LyricManual::Auto; // Y24.48: per-track L override
    bool embedded_sub_confirmed_ = false;          // Y24.48: embedded cue seen this track
    int lyric_bar_height_ = 5;
    int lyric_bar_lines_ = 3; // lyric content rows = lyric_bar_height_ - 2 (borders)
    // Y24.30: ThemeManager::instance().statusbar_config() moved to ThemeManager.
    bool show_tree_lines_ = true;
    bool scroll_mode_ = false;  //scroll display off by default
    bool url_hyperlink_ = true; // Y24.27: cached [display] url_hyperlink (was per-frame INI read)
    bool title_emoji_ =
        true; // border title Emoji switch (default Emoji; narrow-emoji terminals auto-fallback to ASCII)
    // Y24.12: OSC 8 hyperlinks collected during draw_info (absolute screen coords), emitted to
    //   /dev/tty AFTER doupdate() so ncurses doesn't clobber them. Each entry = one linked span.
    struct Osc8Emit {
        int row;
        int col;
        std::string visible;
        std::string url;
        std::string id;
    };
    std::vector<Osc8Emit> pending_osc8_;

    //Theme index - 15 color schemes cycle (0=Dark classic, author's favorite)
    // Y24.30: theme_index_ moved to ThemeManager.

    // Y12: lyric history (recent sub-text lines) for the lyric panel. Updated each frame from
    //   state.sub_text (push when it changes); the panel shows the last `lyric_lines` entries,
    //   current (newest) highlighted, auto-scrolling as the lyric advances.
    std::deque<std::string> lyric_history_;
    std::string last_lyric_url_; // Y12: detect track change → clear lyric history
    // Y23.4 (method B): parsed podcast transcript segments for the current track. When non-empty,
    //   the LYRIC panel is driven by playback time_pos (TranscriptParser::find_at) instead of mpv
    //   sub-text — so JSON transcripts (which mpv can't parse) display natively.
    std::vector<TranscriptSegment> current_transcript_;
    std::string current_transcript_url_;

    //Use the unified layout manager
    // All scroll offsets are managed uniformly by LayoutMetrics, auto-reset on window resize
    LayoutMetrics &layout_ = LayoutMetrics::instance();

    // Incremental-redraw state tracking and needs_full_redraw/update_redraw_state have been removed:
    // needs_full_redraw always returns true (incremental redraw was never enabled); related members are write-only dead code.

    void draw_line(WINDOW *win, int y, const DisplayItem &item, bool selected, bool in_visual,
                   int max_len, const std::string &current_url = "");

    //Responsive status bar - smart abbreviation strategy
    // ┌─────────────────────────────────────────────────────────────────────────┐
    // │ [Status bar structure]                                                    │
    // │   ░▒▓█ ★ panicast V0.05B9n3e5g2RF ★ █▓▒░   [ URL ]   ░▒▓█ By author time █▓▒░ │
    // │   └────────── left art ──────────┘   └mid┘   └────────── right art ─┘ │
    // ├─────────────────────────────────────────────────────────────────────────┤
    // │ [Protection rules]                                                        │
    // │   Left fixed: ░▒▓█ ★  and  █▓▒░ (outer art always kept)                  │
    // │   Right fixed: ░▒▓█ By and  █▓▒░ (outer art always kept)                 │
    // │   Middle fixed: [ and ] (the brackets themselves are always kept)        │
    // ├─────────────────────────────────────────────────────────────────────────┤
    // │ [Abbreviation priority (high to low)]                                     │
    // │   Priority 1: URL content inside middle [] - truncate from the middle with ... │
    // │   Priority 2: left version number - truncate from the right (toward the middle) │
    // │   Priority 3: right author time - truncate from the left (toward the middle) │
    // └─────────────────────────────────────────────────────────────────────────┘
    void draw_status(WINDOW *win, const MPVController::State &state, TreeNodePtr selected_node,
                     const DisplayContext &dctx = {});

    // V2.39: INFO area display
    //Added playlist parameter
    //Added List mode parameter
    void draw_info(WINDOW *win, const MPVController::State &state, AppState app_state,
                   const DisplayContext &dctx, int marked_count, const std::string &search_query,
                   int current_match, int total_matches, TreeNodePtr selected_node,
                   const std::vector<DownloadProgress> &downloads, bool visual_mode, int cw,
                   const std::vector<PlaylistItem> &playlist = {}, int playlist_index = -1,
                   PlayMode play_mode = PlayMode::CYCLE,
                   const std::vector<std::string> &history_titles = {},
                   const std::vector<int> &next_indices = {});

    //Help popup - complete key definitions
    void draw_help(WINDOW *win, const MPVController::State &state, int cw);
};
} // namespace panicast
