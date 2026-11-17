// Process / IPC utilities: which_binary, run_process(_streaming), copy_to_clipboard, emit_osc8_link, emit_cup (+ osc52/base64 helpers).
// Y24.38: split out of utils.cpp. Methods remain Utils:: static members
//   (declarations stay in utils.h); only implementations live here.
#include "panicast/core/utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring> // std::string
#include <ctime>
#include <errno.h>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <sstream>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h> // PR_SET_PDEATHSIG (Linux-only; macOS uses a no-op below)
#endif
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <fcntl.h>

#include <fmt/format.h>

#include "panicast/core/paths.h"
#include "panicast/core/terminal.h"
#include "panicast/core/logger.h"

extern char **environ; // Required by posix_spawnp

namespace panicast
{

// Remove the shell sink from popen/system (command injection risk).
// Safe executable lookup: walk $PATH looking for a regular executable file named name.
std::string Utils::which_binary(const std::string &name) {
    if (name.empty())
        return "";
    const char *path_env = std::getenv("PATH");
    std::string pe = path_env ? path_env : "";
    std::istringstream ss(pe);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty())
            continue;
        std::string candidate = dir + "/" + name;
        struct stat st;
        if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    // Fallback to common paths
    const char *fallbacks[] = {"/usr/bin/yt-dlp", "/usr/local/bin/yt-dlp"};
    for (const char *fb : fallbacks) {
        struct stat st;
        if (stat(fb, &st) == 0 && S_ISREG(st.st_mode) && access(fb, X_OK) == 0 &&
            std::string(fb).find(name) != std::string::npos) {
            return fb;
        }
    }
    return "";
}

// Copy text to the system clipboard. Looks up an available tool by priority (wl-copy/xclip/xsel/pbcopy/clip.exe),
//   writes via posix_spawn + argv + stdin pipe (no shell, no command injection).
//   Returns true if a tool was found and exited with 0; returns false if no tool is available
//   (the caller should fall back to showing a popup).
// Base64 encoding (OSC 52 clipboard protocol requires a base64 payload)
static std::string base64_encode(const std::string &in) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(tbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6)
        out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4)
        out.push_back('=');
    return out;
}

// OSC 52 terminal clipboard protocol: writes text to the local system clipboard (pasteable with Ctrl+V).
//   Under SSH/terminals xclip/xsel are unavailable without DISPLAY; OSC 52 reaches the local clipboard
//   directly via terminal escape sequences.
//   Sequence: ESC ] 52 ; c ; <base64> BEL. Written to /dev/tty to bypass the ncurses screen buffer;
//   no visible output, so the screen is not garbled.
static bool copy_via_osc52(const std::string &text) {
    std::string seq = "\033]52;c;" + base64_encode(text) + "\007";
    int fd = open("/dev/tty", O_WRONLY);
    if (fd < 0)
        return false;
    ssize_t total = (ssize_t)seq.size(), written = 0;
    while (written < total) {
        ssize_t n = write(fd, seq.data() + written, total - written);
        if (n > 0)
            written += n;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    close(fd);
    return written == total;
}

// Y24.12: OSC 8 hyperlink — make a screen cell range a clickable link to `url`.
//   Y24.15: `id` groups multiple spans as ONE logical link (OSC 8 ; id=<id> ; <uri> ST) — terminals
//   that support it underline ALL spans sharing the id when hovering any one, so a wrapped URL
//   shows the underline on every line synchronously instead of just the hovered line.
//   Sequence: CUP(row,col) + OSC8-open(id,url) + visible-text + OSC8-close. Written to /dev/tty
//   (bypasses ncurses; the visible text is the same bytes ncurses already drew, so no visual change).
void Utils::emit_osc8_link(int scr_row, int scr_col, const std::string &visible,
                           const std::string &url, const std::string &id) {
    if (url.empty() || scr_row < 1 || scr_col < 1)
        return;
    // CUP is 1-based: \033[<row>;<col>H. Open: \033]8;<params>;<url>\033\\  (params = id=<id> if non-empty)
    std::string params = id.empty() ? "" : ("id=" + id);
    std::string seq = "\033[" + std::to_string(scr_row) + ";" + std::to_string(scr_col) + "H" +
                      "\033]8;" + params + ";" + url + "\033\\" + visible + "\033]8;;\033\\";
    int fd = open("/dev/tty", O_WRONLY);
    if (fd < 0)
        return;
    ssize_t total = (ssize_t)seq.size(), written = 0;
    while (written < total) {
        ssize_t n = write(fd, seq.data() + written, total - written);
        if (n > 0)
            written += n;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    close(fd);
}

// Y24.13: raw-CUP to (scr_row, scr_col) — restores the physical cursor after emit_osc8_link()
//   moved it, re-syncing with ncurses' cursor-tracking (prevents panel garbling next frame).
void Utils::emit_cup(int scr_row, int scr_col) {
    if (scr_row < 1 || scr_col < 1)
        return;
    std::string seq = "\033[" + std::to_string(scr_row) + ";" + std::to_string(scr_col) + "H";
    int fd = open("/dev/tty", O_WRONLY);
    if (fd < 0)
        return;
    ssize_t total = (ssize_t)seq.size(), written = 0;
    while (written < total) {
        ssize_t n = write(fd, seq.data() + written, total - written);
        if (n > 0)
            written += n;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    close(fd);
}

bool Utils::copy_to_clipboard(const std::string &text) {
    struct Tool {
        const char *name;
        const char *a1;
        const char *a2;
    };
    static const Tool tools[] = {
        {"wl-copy", nullptr, nullptr},      {"xclip", "-selection", "clipboard"},
        {"xsel", "--clipboard", "--input"}, {"pbcopy", nullptr, nullptr},
        {"clip.exe", nullptr, nullptr},
    };
    auto is_exec = [](const std::string &p) -> bool {
        struct stat st;
        return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0;
    };
    std::string exe;
    const char *a1 = nullptr;
    const char *a2 = nullptr;
    const char *path_env = std::getenv("PATH");
    std::string pe = path_env ? path_env : "";
    for (const auto &t : tools) {
        // POSIX PATH is ':'-separated; Windows compatibility: a candidate may also be a bare name
        std::istringstream ss(pe);
        std::string dir;
        while (std::getline(ss, dir, ':')) {
            if (dir.empty())
                continue;
            std::string candidate = dir + "/" + t.name;
            if (is_exec(candidate)) {
                exe = candidate;
                a1 = t.a1;
                a2 = t.a2;
                break;
            }
        }
        if (exe.empty() && is_exec(t.name)) {
            exe = t.name;
            a1 = t.a1;
            a2 = t.a2;
        }
        if (!exe.empty())
            break;
    }
    if (exe.empty())
        return false;

    // Build argv (elements must stay alive)
    std::vector<std::string> storage;
    storage.push_back(exe);
    if (a1)
        storage.push_back(a1);
    if (a2)
        storage.push_back(a2);
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (auto &s : storage)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    int in_pipe[2];
    if (pipe(in_pipe) != 0)
        return false;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return false;
    }
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions, in_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);

    pid_t pid;
    int rc = posix_spawnp(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(in_pipe[0]);
    if (rc != 0) {
        close(in_pipe[1]);
        return false;
    }

    // Write text (URL is far smaller than the 64KB pipe buffer; still loop to handle partial writes)
    const char *data = text.c_str();
    size_t total = text.size();
    size_t written = 0;
    while (written < total) {
        ssize_t n = write(in_pipe[1], data + written, total - written);
        if (n > 0)
            written += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    close(in_pipe[1]); // Close stdin to signal EOF and let the tool exit

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;
    // Clipboard tool failed (SSH with no DISPLAY, etc.) -> fall back to the OSC 52 terminal clipboard protocol
    return copy_via_osc52(text);
}

// Y24.19: run an external program (no shell, posix_spawnp), capture stdout+stderr+exit code.
//   Blocks until exit (transcription can take minutes; no timeout). For TranscriptionEngine.
Utils::ProcResult Utils::run_process(const std::string &exe, const std::vector<std::string> &args) {
    ProcResult res;
    // Build argv (elements must stay alive).
    std::vector<std::string> storage;
    storage.push_back(exe);
    for (const auto &a : args)
        storage.push_back(a);
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (auto &s : storage)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0)
        return res;
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return res;
    }

    // N04-fix: fork+exec with PR_SET_PDEATHSIG + new pgroup (see Utils::spawn_child).
    pid_t pid = Utils::spawn_child(exe, argv.data(), -1, out_pipe[1], err_pipe[1], true);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (pid < 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        return res;
    }
    res.launched = true;
    Utils::register_child_pid(pid); // N04-fix: track for shutdown kill

    // Read stdout + stderr fully (child writes then exits; we close after).
    auto drain = [](int fd, std::string &out) {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            out.append(buf, n);
    };
    drain(out_pipe[0], res.stdout_out);
    drain(err_pipe[0], res.stderr_out);
    close(out_pipe[0]);
    close(err_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    Utils::unregister_child_pid(pid);
    if (WIFEXITED(status))
        res.exit_code = WEXITSTATUS(status);
    return res;
}

// Y24.20: run a program and stream stdout line-by-line to line_cb while it runs (whisper-cli
//   progressive segment output). Blocks until exit; returns exit code (-1 if launch failed).
int Utils::run_process_streaming(const std::string &exe, const std::vector<std::string> &args,
                                 const std::function<void(const std::string &)> &line_cb,
                                 const std::function<bool()> &stop_pred) {
    std::vector<std::string> storage;
    storage.push_back(exe);
    for (const auto &a : args)
        storage.push_back(a);
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (auto &s : storage)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    int out_pipe[2];
    if (pipe(out_pipe) != 0)
        return -1;
    int devnull = open("/dev/null", O_WRONLY); // stderr sink (whisper-cli model-load log is noisy)

    // N04-fix: fork+exec with PR_SET_PDEATHSIG + new pgroup (see Utils::spawn_child).
    pid_t pid = Utils::spawn_child(exe, argv.data(), -1, out_pipe[1], devnull, true);
    close(out_pipe[1]);
    if (devnull >= 0)
        close(devnull);
    if (pid < 0) {
        close(out_pipe[0]);
        return -1;
    }
    Utils::register_child_pid(pid); // N04-fix: track for shutdown kill

    // Read stdout line-by-line, invoking line_cb per line (progressive). If stop_pred returns true,
    // kill the child (real-time transcription stopped) and stop reading.
    FILE *fp = fdopen(out_pipe[0], "r");
    bool stopped = false;
    if (fp) {
        char *line = nullptr;
        size_t cap = 0;
        ssize_t n;
        while ((n = getline(&line, &cap, fp)) > 0) {
            if (stop_pred && stop_pred()) {
                stopped = true;
                break;
            }
            if (line_cb) {
                std::string s(line, (n > 0 && line[n - 1] == '\n') ? n - 1 : n);
                line_cb(s);
            }
        }
        free(line);
        fclose(fp);
    } else {
        close(out_pipe[0]);
    }
    if (stopped) {
        kill(-pid, SIGTERM);
    } // N04-fix: group kill
    int status = 0;
    waitpid(pid, &status, 0);
    Utils::unregister_child_pid(pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// ── N04-fix: child-process registry (see utils.h comment) ─────────────────────
namespace
{
std::mutex &child_pids_mtx() {
    static std::mutex m;
    return m;
}
std::vector<pid_t> &child_pids() {
    static std::vector<pid_t> v;
    return v;
}
} // namespace

void Utils::register_child_pid(pid_t pid) {
    if (pid <= 0)
        return;
    std::lock_guard<std::mutex> lk(child_pids_mtx());
    child_pids().push_back(pid);
}

void Utils::unregister_child_pid(pid_t pid) {
    std::lock_guard<std::mutex> lk(child_pids_mtx());
    auto &v = child_pids();
    v.erase(std::remove(v.begin(), v.end(), pid), v.end());
}

void Utils::kill_all_child_processes() {
    std::vector<pid_t> pids;
    {
        std::lock_guard<std::mutex> lk(child_pids_mtx());
        pids = child_pids();
        child_pids().clear();
    }
    if (pids.empty())
        return;
    // Each tracked child is a process-group leader (pgid==pid, set by spawn_child), so kill(-pid)
    // takes down the whole subtree (e.g. yt-dlp + its ffmpeg merge child) on graceful shutdown.
    // SIGTERM first (lets yt-dlp/whisper clean up), then SIGKILL survivors after a short grace.
    for (pid_t p : pids)
        kill(-p, SIGTERM);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (pid_t p : pids) {
        if (waitpid(p, nullptr, WNOHANG) == 0) { // direct child still alive
            kill(-p, SIGKILL);
            waitpid(p, nullptr, 0); // reap the direct child (grandchildren reaped by init)
        }
    }
    LOG(fmt::format("[Shutdown] Killed {} tracked child process group(s)", pids.size()));
}

// N04-fix: fork+exec helper with PR_SET_PDEATHSIG (see utils.h). Replaces posix_spawn for the
//   yt-dlp / ffmpeg / whisper children so that `kill -9 PID` (SIGKILL of panicast) still kills them
//   — the kernel delivers the parent-death signal even though no handler can run.
pid_t Utils::spawn_child(const std::string &exe, char *const argv[], int in_fd, int out_fd,
                         int err_fd, bool new_pgroup) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        // child
        if (new_pgroup)
            setpgid(0, 0); // become a new process-group leader (pgid == our pid)
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGKILL); // auto-die when panicast dies (covers SIGKILL of parent)
#endif
        if (in_fd >= 0) {
            dup2(in_fd, STDIN_FILENO);
        } else {
            // ASR-fix (2026-08-15): no stdin given → attach /dev/null. The child inherits the
            //   parent's terminal stdin otherwise; ffmpeg (interactive 'q' key polling) reads it
            //   from a BACKGROUND process group → kernel SIGTTIN → child stopped forever (state T,
            //   zero CPU) — the "LYRIC open but no ASR text ever appears" bug (whisper waits on a
            //   wav ffmpeg never writes). /dev/null keeps interactive children (yt-dlp prompts,
            //   ffmpeg keys) at EOF instead of stealing the tty.
            int nd = open("/dev/null", O_RDONLY);
            if (nd >= 0) {
                dup2(nd, STDIN_FILENO);
                if (nd != STDIN_FILENO)
                    close(nd);
            }
        }
        if (out_fd >= 0)
            dup2(out_fd, STDOUT_FILENO);
        if (err_fd >= 0)
            dup2(err_fd, STDERR_FILENO);
        // Close every inherited fd >= 3 (pipe ends, log, db, sockets) — the child execs immediately
        // and must not hold the parent's read ends (which would prevent EOF). Cleaner than posix_spawn
        // inheritance, which leaks all non-CLOEXEC parent fds.
        for (int fd = 3; fd < 1024; ++fd)
            close(fd);
        execvp(exe.c_str(), argv);
        _exit(127); // exec failed
    }
    // parent — close the race where the child exec'd (and possibly spawned its own children) before
    //   we could record its pgid. setpgid on an already-exec'd child still sets the leader if it
    //   hasn't changed groups; EACCES/ESRCH are harmless here.
    if (new_pgroup)
        setpgid(pid, pid);
    return pid;
}
} // namespace panicast
