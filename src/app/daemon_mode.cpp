// panicast --daemon mode — implementation. See daemon_mode.h for the contract.
//   Migrated from the former src/panicastd_main.cpp (N09/S1) when the two binaries
//   merged into one (N10): the service subcommand forwarding block is gone (service
//   commands are intercepted in main() long before -d can reach this path), and the
//   double-instance guard + N09 pidfile rename landed here.
#include "panicast/app/daemon_mode.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <signal.h>
#include <unistd.h>

#include <fmt/format.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "panicast/app/app.h"
#include "panicast/config/ini_config.h"
#include "panicast/net/lms_server.h"
#include "panicast/core/logger.h"
#include "panicast/core/paths.h"
#include "panicast/parsers/xml_helpers.h"
#include "panicast/ui/ui.h" // setup_signal_handlers / tui_cleanup (curses-guarded no-op here)

namespace panicast
{

std::string daemon_pidfile_path() {
    return Paths::get_data_dir() + "/panicast-daemon.pid";
}

// META-6: pid-alive AND the pid still belongs to a panicast process. kill(pid,0)
//   alone gives false positives when the pid got reused by an unrelated process
//   after a crash — which would block startup forever with a misleading message.
bool pid_is_panicast(int pid) {
    std::ifstream c("/proc/" + std::to_string(pid) + "/comm");
    std::string comm;
    std::getline(c, comm);
    return comm == "panicast";
}

bool daemon_pid_alive(int *out_pid) {
    std::ifstream f(daemon_pidfile_path());
    int pid = 0;
    if (!(f >> pid) || pid <= 0)
        return false; // no (valid) pid file → not running via daemon path
    if (::kill(pid, 0) != 0)
        return false; // ESRCH → stale file, daemon is gone
    if (!pid_is_panicast(pid)) {
        std::remove(daemon_pidfile_path().c_str()); // pid reused → stale
        return false;
    }
    if (out_pid)
        *out_pid = pid;
    return true;
}

// ── TUI session ownership (N10.2) ────────────────────────────────────────────
//   Same pidfile-liveness pattern as the daemon side; see daemon_mode.h.
std::string tui_pidfile_path() {
    return Paths::get_data_dir() + "/panicast-tui.pid";
}

bool tui_pid_alive(int *out_pid) {
    std::ifstream f(tui_pidfile_path());
    int pid = 0;
    if (!(f >> pid) || pid <= 0)
        return false;
    if (::kill(pid, 0) != 0)
        return false;
    if (!pid_is_panicast(pid)) {
        std::remove(tui_pidfile_path().c_str()); // pid reused → stale
        return false;
    }
    if (out_pid)
        *out_pid = pid;
    return true;
}

// Human-readable owner of a TUI session for refusal messages: "pid N (tty)" —
//   tells the user WHERE the live session is instead of a bare pid.
std::string tui_session_tty(int pid) {
    char buf[128] = {0};
    ssize_t n =
        ::readlink(("/proc/" + std::to_string(pid) + "/fd/0").c_str(), buf, sizeof(buf) - 1);
    if (n <= 0)
        return "";
    std::string t(buf, (size_t)n);
    size_t p = t.rfind('/');
    return p == std::string::npos ? t : t.substr(p + 1);
}

void write_tui_pidfile() {
    std::string p = tui_pidfile_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
    std::ofstream f(p);
    if (f.is_open())
        f << getpid() << "\n";
}

void remove_tui_pidfile() {
    std::remove(tui_pidfile_path().c_str());
}

// Probe-bind with the SAME semantics LmsServer's listener uses (dual-stack any +
//   SO_REUSEADDR): fails exactly when an ACTIVE listener holds the port — TIME_WAIT
//   leftovers of a just-exited daemon do NOT trip it (important for systemd restart).
//   ── RETIRED (LIF-001 guard standardization): the pre-flight port probe was an
//   iteration-era compensation for orphan sessions without pidfiles. The standard
//   design is a FATAL bind failure in the headless daemon (headless_fatal_) +
//   systemd restart; no probing before the fact.

namespace
{
void write_pidfile() {
    std::string p = daemon_pidfile_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
    std::ofstream f(p);
    if (f.is_open())
        f << getpid() << "\n";
}

void remove_pidfile() {
    std::remove(daemon_pidfile_path().c_str());
}

// LIF-002 (--debug): true when the user unit is active per the init system.
bool unit_active_via_systemd() {
    FILE *f =
        ::popen("systemctl --user is-active panicast.service 2>/dev/null", "r");
    if (!f)
        return false;
    char buf[64] = {0};
    size_t n = ::fread(buf, 1, sizeof(buf) - 1, f);
    ::pclose(f);
    return n > 0 && strncmp(buf, "active", 6) == 0;
}

// LIF-002 (--debug): restore the unit's runtime context — WorkingDirectory and
//   every Environment= line — for a foreground debug run. The current unit
//   carries no Environment= entries (deliberately; see ensure_user_unit), but
//   the parser is general so future unit edits stay aligned automatically.
void align_unit_environment() {
    const char *home = std::getenv("HOME");
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base =
        xdg && *xdg ? std::string(xdg) : std::string(home ? home : "") + "/.config";
    std::ifstream f(base + "/systemd/user/panicast.service");
    if (!f.is_open())
        return; // no unit yet — run with the current environment
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("WorkingDirectory=", 0) == 0) {
            std::string wd = line.substr(strlen("WorkingDirectory="));
            if (!wd.empty())
                ::chdir(wd.c_str());
        } else if (line.rfind("Environment=", 0) == 0) {
            std::string kv = line.substr(strlen("Environment="));
            size_t eq = kv.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = kv.substr(0, eq);
            std::string val = kv.substr(eq + 1);
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
                val = val.substr(1, val.size() - 2); // systemd quoted value
            if (!key.empty())
                ::setenv(key.c_str(), val.c_str(), 1);
        }
    }
}
} // namespace

int run_daemon(bool debug) {
    if (debug) {
        // LIF-002: instance mutual exclusion — never race the managed instance.
        if (unit_active_via_systemd()) {
            std::fprintf(stderr,
                         "panicast --debug: the systemd-managed daemon is running — stop it "
                         "first (`panicast service stop`), then debug.\n");
            return 1;
        }
        // LIF-002: restore the production runtime context, then mirror the log
        //   stream to the console for the foreground session.
        align_unit_environment();
        Logger::instance().set_console_echo(true);
        std::fprintf(stderr, "panicast --debug: foreground engine (unit env aligned; "
                             "Ctrl+C exits cleanly)\n");
    }
    // Double-instance guard: a manual `panicast -d` while the systemd service (or
    //   another manual run) is already alive would fight over :9090, the mpv instance
    //   and the SQLite writes. Refuse instead of racing.
    if (daemon_pid_alive()) {
        std::fprintf(stderr,
                     "panicast --daemon: another daemon instance is already running (pid file "
                     "%s).\nUse `panicast status` to inspect it, or `panicast restart` "
                     "to recycle it.\n",
                     daemon_pidfile_path().c_str());
        return 1;
    }
    // N10.2: a TUI session owns the engine while it runs (session handover, N09/S1-4)
    //   and restarts this service on its way out — starting alongside it would race
    //   mpv and the DB from behind the user's back.
    if (tui_pid_alive()) {
        std::fprintf(stderr, "panicast --daemon: a TUI session owns playback right now — exit it "
                             "first.\n(It restarts the background service automatically when it "
                             "exits.)\n");
        return 1;
    }
    // N10.5 zero-drop restore: a TUI on its way out may hand its listener + live
    //   phone connections to us. Listen on the boot socket for a few seconds; any
    //   received fds are STAGED (LmsServer::start adopts them instead of binding).
    {
        const char *xdg = std::getenv("XDG_RUNTIME_DIR");
        std::string dir = xdg && *xdg ? xdg : "/tmp";
        std::string path = dir + "/panicast-daemon-hs.sock";
        // The exiting TUI drops an INTENT marker right before `systemctl start` —
        //   only then does a transfer actually come, so only then do we BLOCK (up to
        //   10s) for it; otherwise a detached thread watches (≤10s) so boot is not
        //   slowed by a wait that has no sender.
        std::string intent = dir + "/panicast-handover.intent";
        bool expect_transfer = ::access(intent.c_str(), F_OK) == 0;
        ::unlink(path.c_str());
        int ls = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (ls >= 0) {
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
            if (::bind(ls, (sockaddr *)&addr, sizeof(addr)) == 0 && ::listen(ls, 1) == 0) {
                auto receive_one = [ls]() {
                    pollfd p{ls, POLLIN, 0};
                    if (::poll(&p, 1, 10000) > 0) {
                        int conn = ::accept(ls, nullptr, nullptr);
                        if (conn >= 0) {
                            int listen_fd = -1;
                            std::vector<LmsAdoptedConn> conns;
                            if (lms_recv_handover_msg(conn, listen_fd, conns) && listen_fd >= 0) {
                                LmsServer::stage_handover(listen_fd, std::move(conns));
                                LOG("[LMS-HANDOVER] boot-side: staged incoming fds");
                            }
                            ::close(conn);
                        }
                    }
                };
                if (expect_transfer)
                    receive_one();
                else
                    std::thread(receive_one).detach();
            }
            if (expect_transfer) {
                ::close(ls);
                ::unlink(path.c_str());
            } else {
                // the detached thread owns ls + the socket file now; it cleans up:
                // simplest is a short-lived janitor — close-on-exec not needed, let the
                // thread close it after its window.
                std::thread([ls, path]() {
                    ::sleep(11);
                    ::close(ls);
                    ::unlink(path.c_str());
                }).detach();
            }
        }
        ::unlink(intent.c_str());
    }

    // LIF-001 guard standardization: no pre-flight port probe. A busy mini-LMS
    //   port surfaces as a FATAL bind failure inside App::run() (headless_fatal_ →
    //   exit 1) — systemd's Restart=on-failure self-heals once the port frees,
    //   instead of the old probe's guesswork or a "running" but unreachable daemon.
    //   (N10.5 caveat covered: when a handover was adopted above, the exiting owner
    //   legitimately still holds a dup of the listener while we take it over.)

    // Same boot sequence as the TUI main (minus the terminal save — no terminal here).
    Paths::migrate_legacy();
    curl_global_init(CURL_GLOBAL_ALL);
    xmlInitParser();
    xmlSetGenericErrorFunc(NULL, xml_error_handler);
    xmlSetStructuredErrorFunc(NULL, (xmlStructuredErrorFunc)xml_structured_error_handler);

    setup_signal_handlers(); // SIGTERM/SIGHUP/SIGINT → the clean flush-and-exit path
    atexit(tui_cleanup);     // no-op until ncurses initializes (never does here)

    write_pidfile();
    int rc = 0;
    {
        App app;
        app.set_headless();
        app.set_exit_hook(remove_pidfile);
        try {
            app.run();
        } catch (const std::exception &e) {
            std::fprintf(stderr, "panicast --daemon: fatal: %s\n", e.what());
            rc = 1;
        } catch (...) {
            std::fprintf(stderr, "panicast --daemon: fatal: unknown exception\n");
            rc = 1;
        }
    } // ~App joins everything before the pid file disappears
    remove_pidfile();

    curl_global_cleanup();
    xmlCleanupParser();
    return rc;
}

} // namespace panicast
