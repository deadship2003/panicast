// Utility class: path expansion, URL handling, clipboard, executable lookup,
// UTF-8 display width (mk_wcwidth), text truncation/wrapping/scrolling, duration formatting.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <sys/types.h> // pid_t

namespace panicast
{

class Utils {
public:
    static std::string expand_path(const std::string &path);
    static std::string get_download_dir();
    static std::string get_log_file();
    static std::string to_lower(const std::string &s);
    // Convert URLs starting with http:// to https://
    static std::string http_to_https(const std::string &url);
    // Safe executable lookup: traverse $PATH to find an executable regular file named name.
    static std::string which_binary(const std::string &name);
    // Copy text to the system clipboard (posix_spawn + stdin pipe, no shell).
    static bool copy_to_clipboard(const std::string &text);
    // Y24.12: emit an OSC 8 terminal hyperlink at absolute screen cell (row,col) (1-based, matching
    //   the CUP escape). The visible text (already drawn by ncurses) becomes a clickable link to
    //   `url`. Written to /dev/tty to bypass the ncurses screen buffer; no visible output, so the
    //   layout is not garbled. Re-emitted each frame by the UI after doupdate(). No-op if /dev/tty
    //   can't be opened. Terminals without OSC 8 support ignore the sequence (no garbage).
    static void emit_osc8_link(int scr_row, int scr_col, const std::string &visible,
                               const std::string &url, const std::string &id = "");
    // Y24.13: raw-CUP the physical cursor to (scr_row, scr_col) (1-based) via /dev/tty. Used to
    //   RESTORE the cursor after emit_osc8_link() moved it, so ncurses' incremental doupdate() —
    //   which optimizes with relative cursor moves based on its last known position — isn't
    //   desynchronized (desync garbles panels: status-bar art bleeds into the node tree, etc.).
    static void emit_cup(int scr_row, int scr_col);
    // Y24.19: run an external program (posix_spawnp, no shell — safe), capture stdout+exit code.
    //   Blocks until the child exits (no timeout — transcription can take minutes). Used by
    //   TranscriptionEngine to run ffmpeg + whisper-cli. Returns false if launch failed.
    struct ProcResult {
        bool launched = false;
        int exit_code = -1;
        std::string stdout_out;
        std::string stderr_out;
    };
    static ProcResult run_process(const std::string &exe, const std::vector<std::string> &args);
    // Y24.20: run an external program and stream its stdout line-by-line to line_cb (called from a
    //   worker thread) WHILE it runs — for whisper-cli progressive segment output. Blocks until exit.
    //   Returns exit code (-1 if launch failed). stderr is discarded (whisper-cli progress is stdout).
    static int run_process_streaming(const std::string &exe, const std::vector<std::string> &args,
                                     const std::function<void(const std::string &)> &line_cb,
                                     const std::function<bool()> &stop_pred = nullptr);
    // N04-fix: child-process registry. yt-dlp/whisper children spawned via posix_spawn are tracked
    //   so that on shutdown (e.g. SIGTERM → pkill) they can be killed, which unblocks the worker
    //   threads blocked in YtdlpRunner::run's poll()/waitpid(). Without this, pool_.shutdown() joins
    //   a worker stuck in a 600s yt-dlp call → the process appears unkillable by SIGTERM.
    static void register_child_pid(pid_t pid);
    static void unregister_child_pid(pid_t pid);
    static void kill_all_child_processes(); // SIGTERM → 200ms → SIGKILL all tracked children
    // N04-fix: fork+exec a child with PR_SET_PDEATHSIG(SIGKILL) so it auto-dies when panicast dies
    //   — this covers SIGKILL of the parent (kill -9 PID), which cannot run any cleanup code.
    //   new_pgroup=true makes the child a process-group leader (pgid==pid) so kill(-pid) can take
    //   down its whole subtree (e.g. yt-dlp's ffmpeg) on graceful shutdown (pkill panicast).
    //   fd redirection: pass -1 to leave the child's fd as inherited (then closed), or a pipe fd to
    //   dup2 onto 0/1/2. All inherited fds >= 3 are closed in the child (cleaner than posix_spawn).
    static pid_t spawn_child(const std::string &exe, char *const argv[], int in_fd, int out_fd,
                             int err_fd, bool new_pgroup);
    static std::string sanitize_filename(const std::string &name);
    static std::string url_encode(const std::string &s);
    // Y24.27: has_gui/is_ssh_session/has_local_display/is_wsl/has_usable_display removed (dead code, 0 callers).

    static int utf8_char_bytes(unsigned char c);
    // mk_wcwidth: returns display width based on Unicode code point
    // Return value: -1 (control char), 0 (invisible/combining char), 1 (halfwidth), 2 (fullwidth)
    static int mk_wcwidth(uint32_t ucs);
    static int utf8_char_display_width(unsigned char first_byte, const unsigned char *next_bytes,
                                       size_t avail);
    static int utf8_display_width(const std::string &s);
    static std::string truncate_by_display_width(const std::string &s, int max_display_width);
    static std::string truncate_by_display_width_right(const std::string &s, int max_display_width);
    static std::string truncate_middle(const std::string &s, int max_display_width);
    static std::string truncate_by_display_width_strict(const std::string &s,
                                                        int max_display_width);
    // Text wrapping: if max_lines is exceeded, the last line is abbreviated with "..."
    static std::vector<std::string> wrap_text(const std::string &s, int max_width,
                                              int max_lines = 12);
    // Scroll text in a loop (scrolled by terminal display column width)
    static std::string get_scrolling_text(const std::string &text, int max_width,
                                          int scroll_offset);
    static std::string format_duration(int seconds);
};

} // namespace panicast
