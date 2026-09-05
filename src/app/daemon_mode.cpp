// panicast --daemon mode — implementation. See daemon_mode.h for the contract.
//   Migrated from the former src/panicastd_main.cpp (N09/S1) when the two binaries
//   merged into one (N10): the service subcommand forwarding block is gone (service
//   commands are intercepted in main() long before -d can reach this path), and the
//   double-instance guard + N09 pidfile rename landed here.
#include "panicast/app/daemon_mode.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <signal.h>
#include <unistd.h>

#include <fmt/format.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "panicast/app/app.h"
#include "panicast/config/ini_config.h"
#include "panicast/core/logger.h"
#include "panicast/core/paths.h"
#include "panicast/parsers/xml_helpers.h"
#include "panicast/ui/ui.h" // setup_signal_handlers / tui_cleanup (curses-guarded no-op here)

namespace panicast
{

std::string daemon_pidfile_path() {
    return Paths::get_data_dir() + "/panicast-daemon.pid";
}

bool daemon_pid_alive(int *out_pid) {
    std::ifstream f(daemon_pidfile_path());
    int pid = 0;
    if (!(f >> pid) || pid <= 0)
        return false; // no (valid) pid file → not running via daemon path
    if (out_pid)
        *out_pid = pid;
    return ::kill(pid, 0) == 0; // ESRCH → stale file, daemon is gone
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
    if (out_pid)
        *out_pid = pid;
    return ::kill(pid, 0) == 0;
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
bool lms_port_in_use() {
    IniConfig::instance().load();
    if (!IniConfig::instance().get_remote_lms_enabled())
        return false; // no LMS → nothing to guard
    int port = IniConfig::instance().get_remote_lms_port();
    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    int v6only = 0, yes = 1;
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in6 a6{};
    a6.sin6_family = AF_INET6;
    a6.sin6_port = htons((uint16_t)port);
    a6.sin6_addr = in6addr_any;
    bool busy = ::bind(fd, reinterpret_cast<struct sockaddr *>(&a6), sizeof(a6)) != 0;
    ::close(fd);
    if (busy)
        LOG(fmt::format("[DAEMON] pre-flight: mini-LMS port {} is already in use", port));
    return busy;
}

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
} // namespace

int run_daemon() {
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
    // N10.4: an orphan/older-binary session can hold the mini-LMS port with NO pidfile
    //   (e.g. a TUI from before the pidfile existed). Starting beside it produced a
    //   ZOMBIE daemon — everything up except the very thing the phone connects to.
    //   Fail loudly instead; systemd's restart then self-heals once the port frees.
    if (lms_port_in_use()) {
        std::fprintf(stderr,
                     "panicast --daemon: the mini-LMS port is already in use by "
                     "another process — likely an older panicast session without a pid file.\n"
                     "Exit it (check `panicast status` / running terminals) and try again.\n");
        return 1;
    }

    // N09 → N10 migration: remove a stale panicastd.pid so daemon_pid_alive() can never
    //   see two "running" pid files at once after an upgrade.
    std::remove((Paths::get_data_dir() + "/panicastd.pid").c_str());

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
