// UI lifecycle layer — extracted implementation unit (D22 god-object split).
//   Terminal/signal lifecycle (save/restore termios, signal handler registration,
//   ncurses atexit cleanup) + ncurses-window member lifecycle (UI::init / cleanup /
//   handle_resize). The member methods stay UI members (declarations in ui.h); only
//   their implementations live here. The 3 file-local globals (anon-namespace
//   g_original_termios/g_termios_saved, static g_term_sig_count) move WITH the
//   terminal cluster that is their sole user; the 5 ui.h-extern globals move their
//   definitions here. Include set mirrors ui.cpp exactly (compile-equivalent via
//   ui.h's transitive includes).
//   Mechanical verbatim move from ui.cpp.
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

// =========================================================
// Terminal state save and restore
// =========================================================
namespace
{
struct termios g_original_termios;
bool g_termios_saved = false;
} // namespace

bool g_ncurses_initialized = false;
int g_original_lines = 0;
int g_original_cols = 0;

// Global exit request flag (used for SIGINT)
std::atomic<bool> g_exit_requested{false};
// Async-signal-safe flag (SIGSEGV/SIGABRT only set the flag; the main loop does cleanup).
// Also set for SIGHUP/SIGTERM/SIGQUIT so the main loop can flush state and exit without a confirm popup.
volatile sig_atomic_t g_crash_sig{0};
// N04-fix: count termination signals; a 2nd one means graceful cleanup is stuck (e.g. a worker
//   blocked in a 600s yt-dlp call) → force _exit so `pkill panicast` run again kills it instantly.
static std::atomic<int> g_term_sig_count{0};

// Save original terminal attributes
void save_terminal_state() {
    if (tcgetattr(STDIN_FILENO, &g_original_termios) == 0) {
        g_termios_saved = true;
    }
    // Save original terminal size
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        g_original_lines = ws.ws_row;
        g_original_cols = ws.ws_col;
    }
}

// Restore original terminal attributes
static void restore_terminal_state() {
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_original_termios);
        // NOTE: do NOT clear g_termios_saved. tui_cleanup runs again at atexit (after ~App), and the
        //   destructor's teardown (remote/transcription/kill/pool) or terminal-specific behavior
        //   (WSL2/WSLg + mpv VO window close) can alter the line discipline after run()'s restore;
        //   re-tcsetattr on every call corrects it so the shell gets the original termios.
    }
    tcflush(STDIN_FILENO, TCIFLUSH);
}

// Async-signal-safe termios restore for the force-exit signal paths. tcsetattr() is NOT
// POSIX async-signal-safe; ioctl() is the underlying syscall and works inside a signal handler.
// The ioctl command is OS-specific: Linux uses TCSETS, macOS/BSD uses TIOCSETA. Without this, a
// forced _exit (2nd SIGINT, or a crash signal) leaves the line discipline in cbreak/raw mode →
// the shell then echoes ESC as "^[" and Ctrl+C as "^C" (ISIG off), and the terminal feels broken.
// RIS (\033c) only resets the terminal's internal state, NOT the kernel line discipline — so
// termios must be restored explicitly here.
static void restore_termios_async() {
    if (g_termios_saved) {
#ifdef __APPLE__
        ioctl(STDIN_FILENO, TIOCSETA, &g_original_termios); // BSD name (macOS)
#else
        ioctl(STDIN_FILENO, TCSETS, &g_original_termios);   // Linux
#endif
        tcflush(STDIN_FILENO, TCIFLUSH); // discard typeahead so it doesn't carry to the shell
    }
}

// Terminal cleanup function (called by atexit and signal handlers)
void tui_cleanup() {
    static std::atomic<bool> cleaned{false}; // Idempotent for endwin (the costly ncurses call)
    bool first = !cleaned.exchange(true);
    if (first && g_ncurses_initialized) {
        endwin(); // ncurses standard: exits alt-screen + restores cursor. Don't add manual escapes.
        g_ncurses_initialized = false;
    }
    // Restore termios (every call, incl. atexit after ~App) + drain typeahead. NO manual cursor
    //   escapes or newlines — those fight bash's own prompt redraw (cursor glued, no clean prompt).
    //   endwin handled the screen; bash handles the prompt placement naturally.
    restore_terminal_state();
    fflush(stdout);
    fflush(stderr);
}

// Signal handler function
static void signal_handler(int sig) {
    // SIGHUP = controlling terminal closed (user closed the window / SSH dropped). The TUI is gone,
    //   so die IMMEDIATELY. We must NOT just set a flag here: the flag is checked by the main loop,
    //   and if the main thread is blocked in a long operation (heavy feed parse, a join, etc.) the
    //   flag is never checked and the process lingers — holding the wlshm video window open and
    //   forcing the user to kill it manually. Restore the line discipline + leave alt-screen, then
    //   _exit (all async-signal-safe). Terminal-close → process always dies.
    if (sig == SIGHUP) {
        restore_termios_async();
        const char restore[] = "\033[?1049l"; // leave alt-screen; bash redraws the prompt
        (void)write(STDERR_FILENO, restore, sizeof(restore) - 1);
        _exit(128 + sig);
    }
    // N04-fix: a 2nd termination signal means graceful cleanup is stuck (worker blocked in
    //   yt-dlp/whisper). Restore the terminal and force _exit so a repeated `pkill panicast`
    //   kills the process immediately instead of hanging in pool_.shutdown()'s join.
    if (sig != SIGSEGV && sig != SIGABRT) {
        if (++g_term_sig_count >= 2) {
            restore_termios_async(); // restore line discipline before _exit (escape seqs alone can't)
            // Exit alt-screen only — let bash handle the cursor/prompt naturally (no manual cursor
            //   forcing or newlines that fight bash's prompt redraw).
            const char restore[] = "\033[?1049l";
            (void)write(STDERR_FILENO, restore, sizeof(restore) - 1);
            _exit(128 + sig); // async-signal-safe; 128+sig is the conventional exit code
        }
    }
    if (sig == SIGINT) {
        // CTRL+C does not exit directly; set the flag and let the main loop handle it
        g_exit_requested = true;
        return;
    }
    if (sig == SIGSEGV || sig == SIGABRT) {
        // Crash signals only set the flag; do not call any non-async-signal-safe functions
        g_crash_sig = sig;
        restore_termios_async(); // restore line discipline before the crash/core-dump
        // Exit alt-screen only — let bash handle the cursor/prompt naturally.
        const char restore[] = "\033[?1049l";
        (void)write(STDERR_FILENO, restore, sizeof(restore) - 1);
        // Restore the default handler and re-raise so the system generates a core dump
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    // SIGTERM/SIGQUIT: likewise only set the flag; the main loop does unified cleanup on exit
    g_exit_requested = true;
    g_crash_sig = sig;
}

// Register signal handlers
void setup_signal_handlers() {
    // SIGHUP = terminal closed / SSH disconnected: must flush & exit (default action would terminate
    //   immediately, losing in-flight YouTube parse cache and any unflushed state).
    int sigs[] = {SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGSEGV, SIGABRT};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(int); i++) {
        struct sigaction sa;
        sa.sa_handler =
            signal_handler; // sigaction has well-defined semantics (avoids SYSV/BSD differences of signal())
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(sigs[i], &sa, nullptr);
    }
}

void UI::init(float ratio) {
    (void)ratio; // P3 (Y23.7): layout_ratio_ removed (LayoutGuard reads INI directly)
    //Get terminal size immediately, before any terminal operation
    // This is the key to fixing UI sizing issues in SSH/TTY environments
    int term_rows = 0, term_cols = 0;
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        term_rows = ws.ws_row;
        term_cols = ws.ws_col;
    }
    // Fallback: read from environment variables
    if (term_rows <= 0 || term_cols <= 0) {
        const char *lines_env = std::getenv("LINES");
        const char *cols_env = std::getenv("COLUMNS");
        if (lines_env)
            term_rows = std::atoi(lines_env);
        if (cols_env)
            term_cols = std::atoi(cols_env);
    }
    // Use the global saved value if it is larger
    if (g_original_lines > term_rows)
        term_rows = g_original_lines;
    if (g_original_cols > term_cols)
        term_cols = g_original_cols;

    //Set locale (no terminal output, log only)
    // Key fix: do not dup2-redirect around setlocale, to avoid breaking the UTF-8 environment
    // WSL2/Debian minimal images often lack a UTF-8 locale; when setlocale falls back to
    //   C/POSIX the wide-char library still garbles. Here we respect the environment locale
    //   first; if the codeset is not UTF-8, fall back to C.UTF-8 (built into glibc, no
    //   locale-gen needed, always available on WSL2/containers).
    //   We do not unconditionally force C.UTF-8 — zh_CN/en_US.UTF-8 users keep their %b
    //   month name/number formatting unchanged.
    char *locale_result = setlocale(LC_ALL, "");
    auto codeset_is_utf8 = [](const char *cs) -> bool {
        if (!cs)
            return false;
        std::string s(cs);
        for (auto &ch : s)
            ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
        return s.find("UTF-8") != std::string::npos || // nl_langinfo usually returns "UTF-8"
               s.find("UTF8") != std::string::npos;    // tolerate "UTF8" spelling
    };
    const char *codeset = locale_result ? nl_langinfo(CODESET) : nullptr;
    if (!codeset_is_utf8(codeset)) {
        // Environment is not UTF-8: fall back to C.UTF-8 (only this branch changes behavior; the original locale was already garbled)
        char *fb = setlocale(LC_ALL, "C.UTF-8");
        if (fb) {
            locale_result = fb;
            codeset = nl_langinfo(CODESET);
        }
    }
    if (locale_result) {
        LOG(fmt::format("[INIT] Locale set to: {} (codeset={})", locale_result,
                        codeset ? codeset : "unknown"));
    }
    if (!codeset_is_utf8(codeset)) {
        // Still not UTF-8 (very old glibc without C.UTF-8 and no generated locale): wide chars will garble
        const char *msg =
            "panicast: terminal is not using a UTF-8 locale; CJK/Emoji will be garbled.\n"
            "  Fix (any one of):\n"
            "    1) Set LANG=en_US.UTF-8 or LANG=C.UTF-8 in ~/.bashrc or /etc/locale.conf\n"
            "    2) Run sudo dpkg-reconfigure locales and enable en_US.UTF-8 / zh_CN.UTF-8\n"
            "    3) Temporary: LANG=C.UTF-8 panicast\n";
        fputs(msg, stderr);
        LOG(fmt::format("[INIT] WARNING: non-UTF-8 locale (codeset={}), CJK/Emoji will garble",
                        codeset ? codeset : "unknown"));
    }

    //Initialize ncurses
    initscr();
    g_ncurses_initialized = true;

    //Force the correct terminal size
    if (term_rows > 0 && term_cols > 0) {
        resizeterm(term_rows, term_cols);
        LINES = term_rows;
        COLS = term_cols;
    }
    wrefresh(stdscr);

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(
        30); //30ms poll (≈33FPS) — snappy input response; heavy work is on pool_ (10 workers), not the main loop
    // Enable mouse: left-click to select and expand/play node, wheel to page up/down
    //   Register only button events (not REPORT_MOUSE_POSITION, to avoid event flooding).
    mmask_t mouse_events = BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON4_PRESSED;
#ifdef BUTTON5_PRESSED
    mouse_events |= BUTTON5_PRESSED; // wheel-down; ncurses 5.7 (macOS) has no BUTTON5
#endif
    mousemask(mouse_events, nullptr);
    start_color();
    use_default_colors();

    for (int i = 0; i < 256; ++i)
        init_pair(i + 1, i, -1);

    // Node-tree state color pairs: foreground from [colors] config (default if missing), black background

    getmaxyx(stdscr, h, w);
    //Use integer arithmetic to avoid floating-point precision issues
    // F31: use LayoutGuard::compute (reads INI layout_ratio) — consistent with handle_resize().
    //   Previously hardcoded w*40/100, ignoring INI layout_ratio on the first render.
    LayoutDims dims = LayoutGuard::compute(w, h);
    left_w = dims.left_w;
    right_w = dims.right_w;
    top_h = dims.top_h;

    // Bounds protection: ensure minimum window size
    if (left_w < 10)
        left_w = 10;
    if (right_w < 10)
        right_w = 10;
    if (top_h < 5)
        top_h = 5;

    left_win = newwin(top_h, left_w, 0, 0);
    right_win = newwin(top_h, right_w, 0, left_w);
    status_win = newwin(3, w, top_h, 0);
    lyric_win = newwin(lyric_bar_height_, w, top_h, 0); // Y24: full-width LYRIC bar

    //Null-pointer check to prevent crashes
    if (!left_win || !right_win || !status_win || !lyric_win) {
        endwin();
        fprintf(stderr, "Error: Failed to create windows\n");
        exit(1);
    }

    //Disable window auto-wrap and scrolling to prevent content overflow
    scrollok(left_win, FALSE);
    scrollok(right_win, FALSE);
    scrollok(status_win, FALSE);
    scrollok(lyric_win, FALSE);
    immedok(left_win, FALSE);
    immedok(right_win, FALSE);
    immedok(status_win, FALSE);
    immedok(lyric_win, FALSE);

    ThemeManager::instance().statusbar_config() =
        IniConfig::instance().get_statusbar_color_config();
    show_tree_lines_ = IniConfig::instance().get_bool("display", "show_tree_lines", true);
    url_hyperlink_ =
        IniConfig::instance().get_bool("display", "url_hyperlink", true); // Y24.27: cache once
    scroll_mode_ = IniConfig::instance().get_bool("display", "scroll_mode", false); //default false
    // Y24: L-mode (LYRIC bar) — user intent persisted; active is derived per-frame by App.
    lyric_bar_requested_ = IniConfig::instance().get_display_lyric_bar();
    lyric_bar_height_ = IniConfig::instance().get_display_lyric_bar_height();
    if (lyric_bar_height_ < 4)
        lyric_bar_height_ = 4; // need ≥3 lyric lines + 2 borders
    if (lyric_bar_height_ > 12)
        lyric_bar_height_ = 12;
    lyric_bar_lines_ = lyric_bar_height_ - 2; // lyric content rows (excluding top+bottom borders)
    // Whether border title uses Emoji. Default false (ASCII, safe everywhere).
    //   When set to true, the left panel top-border title uses an Emoji badge; the border
    //   switches to a "full ─ base + title overlaid" drawing style, so emoji width mismatch
    //   with the terminal never produces gaps. Requires an emoji font in the terminal,
    //   otherwise emoji render as tofu (a terminal font limitation, not fixable in code).
    title_emoji_ = IniConfig::instance().get_bool("display", "title_emoji", true);

    // Load icon style config (emoji|ascii|auto) + emoji width probing
    {
        // 1. Read INI emoji_width override (0=auto, 1/2=forced)
        int ew_override = IniConfig::instance().get_int("display", "emoji_width", 0);
        if (ew_override == 1 || ew_override == 2) {
            g_emoji_width_override = ew_override;
            LOG(fmt::format("[UI] Emoji width override: {} (from INI)", ew_override));
        }

        // 2. If not overridden, run cursor probing (unconditionally, no longer WT-only)
        //    WT_SESSION is not forwarded over SSH, so only cursor probing can detect narrow-emoji terminals.
        if (g_emoji_width_override == 0 && g_emoji_width_probed == 0) {
            int probed = probe_emoji_width();
            if (probed > 0) {
                g_emoji_width_probed = probed;
                LOG(fmt::format("[UI] Emoji width probed: {} (cursor detection)", probed));
            }
        }

        // 3. Load icon style
        std::string style = IniConfig::instance().get("display", "icon_style", "auto");
        if (style == "emoji") {
            IconManager::set_style(IconStyle::EMOJI);
        } else if (style == "ascii") {
            IconManager::set_style(IconStyle::ASCII);
        } else {
            // auto: driven by cursor-probe result (not env vars), ASCII-first conservative
            //   probe=1 -> narrow-emoji terminal (Windows Terminal, detectable over SSH too) -> ASCII
            //   probe=2 -> wide-emoji terminal (Linux xterm/gnome etc.) -> Emoji
            //   probe=0 -> probe failed/no response/non-interactive -> ASCII (safe, avoids tofu/misalignment)
            //   Note: border title is already fixed to ASCII (see mode_str); this only affects list-item icons.
            int probed = g_emoji_width_probed;
            if (probed == 2) {
                IconManager::set_style(IconStyle::EMOJI);
                LOG(fmt::format("[UI] Auto icon style: Emoji (probed emoji width=2, terminal={})",
                                TerminalDetector::terminal_name()));
            } else {
                IconManager::set_style(IconStyle::ASCII);
                LOG(fmt::format("[UI] Auto icon style: ASCII (probed={}, terminal={})", probed,
                                TerminalDetector::terminal_name()));
            }
        }

        // 4. Log the final emoji-width decision (for debugging)
        LOG(fmt::format("[UI] Emoji width final: {} (override={}, probed={}, recommended={})",
                        get_emoji_width(), g_emoji_width_override, g_emoji_width_probed,
                        TerminalDetector::recommended_emoji_width()));
    }

    // Load persisted theme index and apply (15 schemes; default 0=Dark if missing)
    {
        ThemeManager::instance().init(); // Y24.30: theme + statusbar config
    }

    CacheManager::instance().load();
}

void UI::cleanup() {
    delwin(left_win);
    delwin(right_win);
    delwin(status_win);
    delwin(lyric_win);
    //Use the global cleanup function to ensure cursor and terminal attributes are restored
    tui_cleanup();
}

void UI::handle_resize() {
    // Zero cached size so size_changed in draw() evaluates to true
    left_w = right_w = top_h = 0;
    cols_ = 0;
    last_lyric_bar_active_ = !lyric_bar_active_; // Y24: force re-detect → full redraw
    // Use wnoutrefresh instead of wrefresh: avoids a standalone physical refresh causing a blank flicker frame;
    // the actual clear is done uniformly by draw()'s clearok(stdscr,TRUE)+doupdate.
    werase(stdscr);
    wnoutrefresh(stdscr);
}
} // namespace panicast
