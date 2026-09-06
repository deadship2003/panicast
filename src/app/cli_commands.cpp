// panicast CLI service subcommands — implementation. See cli_commands.h for the contract.
#include "panicast/app/cli_commands.h"

#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <fmt/format.h>

#include "panicast/app/daemon_mode.h"
#include "panicast/config/ini_config.h"
#include "panicast/core/paths.h"
#include "panicast/net/lms_server.h"

namespace panicast
{

namespace
{
// N10: the unit runs `panicast --daemon`. N10.3: it is a USER unit — every verb goes
//   through `systemctl --user`, so no sudo/polkit is involved anywhere.
const char *UNIT = "panicast.service";

int systemctl(const char *verb, bool) {
    std::string cmd = std::string("systemctl --user ") + verb + " " + UNIT;
    // ::system() returns the RAW wait status (256 per exit-code unit) — the process
    //   exit code would be truncated (& 0xFF); normalize to 0/1.
    return ::system(cmd.c_str()) == 0 ? 0 : 1;
}

std::string today_log_path() {
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
    return Paths::get_data_dir() + "/panicast-" + buf + ".log";
}

// Minimal base64 encode for the Basic-auth header.
std::string b64encode(const std::string &in) {
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = 0;
    for (unsigned char c : in) {
        val = (val << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out += tbl[(val >> bits) & 0x3F];
        }
    }
    if (bits > 0)
        out += tbl[(val << (6 - bits)) & 0x3F];
    while (out.size() % 4)
        out += '=';
    return out;
}

// One-shot LMS status query over the cometd endpoint; returns the raw reply body ("" on
//   any failure). Used by `panicast status` to report what the daemon is playing.
std::string lms_status_reply() {
    IniConfig::instance().load();
    int port = IniConfig::instance().get_remote_lms_port();
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return "";
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, (sockaddr *)&a, sizeof(a)) != 0) {
        ::close(fd);
        return "";
    }
    std::string auth = b64encode(IniConfig::instance().get_remote_lms_user() + ":" +
                                 IniConfig::instance().get_remote_lms_pass());
    std::string body = "[{\"channel\":\"/slim/request\",\"data\":{\"request\":"
                       "[\"00:00:00:00:84:21\",[\"status\",\"-\",\"1\"]],"
                       "\"response\":\"/cli/s\"}}]";
    std::string req =
        "POST /cometd HTTP/1.1\r\nHost: localhost\r\nAuthorization: Basic " + auth +
        "\r\nContent-Type: text/json\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body;
    if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) <= 0) {
        ::close(fd);
        return "";
    }
    std::string resp;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        resp.append(buf, (size_t)n);
    ::close(fd);
    size_t sep = resp.find("\r\n\r\n");
    return sep == std::string::npos ? "" : resp.substr(sep + 4);
}

// ── N10.5 zero-drop handover (TUI side) ───────────────────────────────────────
// Squeeze Client must not see a disconnect when the engine owner changes. The
//   outgoing owner duplicates its listener + live connection fds via SCM_RIGHTS;
//   the incoming owner adopts them. Both directions funnel through these helpers,
//   and every failure falls back to the plain systemctl flow.

static std::string handover_sock_dir() {
    const char *xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg)
        return xdg;
    return "/tmp";
}

// POST one slim command to the daemon's LMS port (fire-and-forget; the reply body is
//   not needed — send_handover_fds runs on a detached thread inside the daemon).
static bool post_lms_handover(const std::string &sock_path) {
    IniConfig::instance().load();
    int port = IniConfig::instance().get_remote_lms_port();
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    timeval tv{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, (sockaddr *)&a, sizeof(a)) != 0) {
        ::close(fd);
        return false;
    }
    std::string auth = b64encode(IniConfig::instance().get_remote_lms_user() + ":" +
                                 IniConfig::instance().get_remote_lms_pass());
    std::string body = "[{\"channel\":\"/slim/request\",\"data\":{\"response\":\"/handover\","
                       "\"request\":[\"00:00:00:00:84:21\",[\"panicast\",\"handover\",\"" +
                       sock_path + "\"]]}},\"id\":\"999\"}]";
    std::string req =
        "POST /cometd HTTP/1.1\r\nHost: localhost\r\nAuthorization: Basic " + auth +
        "\r\nContent-Type: text/json\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body;
    bool ok = ::send(fd, req.data(), req.size(), MSG_NOSIGNAL) == (ssize_t)req.size();
    char sink[512];
    while (::recv(fd, sink, sizeof(sink), 0) > 0)
        ; // drain (the daemon's detached transfer thread needs a beat to connect)
    ::close(fd);
    return ok;
}

// Listen on a per-pid unix socket, ask the daemon to hand over, receive + stage.
static bool takeover_via_fd_passing() {
    std::string path =
        handover_sock_dir() + "/panicast-tui-hs-" + std::to_string(::getpid()) + ".sock";
    ::unlink(path.c_str());
    int ls = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0)
        return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(ls, (sockaddr *)&addr, sizeof(addr)) != 0 || ::listen(ls, 1) != 0) {
        ::close(ls);
        ::unlink(path.c_str());
        return false;
    }
    if (!post_lms_handover(path)) {
        ::close(ls);
        ::unlink(path.c_str());
        return false;
    }
    // Bounded wait for the daemon's detached transfer thread to connect.
    pollfd p{ls, POLLIN, 0};
    if (::poll(&p, 1, 3000) <= 0) {
        ::close(ls);
        ::unlink(path.c_str());
        return false;
    }
    int conn = ::accept(ls, nullptr, nullptr);
    ::close(ls);
    ::unlink(path.c_str());
    if (conn < 0)
        return false;
    int listen_fd = -1;
    std::vector<LmsAdoptedConn> conns;
    bool ok = lms_recv_handover_msg(conn, listen_fd, conns);
    ::close(conn);
    if (!ok || listen_fd < 0) {
        if (listen_fd >= 0)
            ::close(listen_fd);
        return false;
    }
    LmsServer::stage_handover(listen_fd, std::move(conns));
    return true;
}

// N10.1: `panicast start` refuses when the daemon is already running (user-final
//   semantics — starting twice would be a silent no-op via systemctl otherwise).
int cmd_start() {
    int pid = 0;
    if (daemon_pid_alive(&pid)) {
        printf("panicast daemon: already running (pid %d) — nothing to start.\n", pid);
        printf("Use `panicast restart` to recycle it.\n");
        return 1;
    }
    // N10.2: a TUI session owns the engine while it runs and restarts the service on
    //   exit — starting the daemon beside it would race mpv and the DB.
    if (tui_pid_alive(&pid)) {
        printf("panicast daemon: a TUI session owns playback right now (pid %d).\n", pid);
        printf("Exit the TUI first — it restarts the service automatically.\n");
        return 1;
    }
    // N10.4: refuse to start into a port conflict — an orphan/older session holding
    //   :9090 with no pidfile would otherwise yield a "running" but unreachable daemon.
    if (lms_port_in_use()) {
        printf("panicast daemon: the mini-LMS port is already in use by another "
               "process —\nlikely an older panicast session without a pid file. Exit it "
               "first (see `panicast status`).\n");
        return 1;
    }
    return systemctl("start", false);
}

int cmd_status() {
    IniConfig::instance().load();
    int pid = 0;
    bool alive = daemon_pid_alive(&pid);
    printf("panicast daemon: %s", alive ? "running" : "stopped");
    if (alive)
        printf(" (pid %d)", pid);
    else if (::system(("systemctl --user is-enabled " + std::string(UNIT) + " >/dev/null 2>&1")
                          .c_str()) == 0)
        printf(" [enabled]");
    printf("\n");
    if (tui_pid_alive(&pid)) {
        printf("panicast TUI:   running (pid %d) — owns playback; the service is handed "
               "back on its exit\n",
               pid);
        return 0;
    }
    if (!alive)
        return 0;
    // What is it playing? Ask the daemon's own LMS endpoint.
    std::string reply = lms_status_reply();
    if (reply.empty()) {
        printf("mini-LMS : no reply on :%d\n", IniConfig::instance().get_remote_lms_port());
        return 0;
    }
    // Cheap field extraction — nlohmann is available but the shapes are tiny and flat.
    auto field = [&](const char *key) -> std::string {
        std::string pat = std::string("\"") + key + "\":";
        size_t p = reply.find(pat);
        if (p == std::string::npos)
            return "";
        p += pat.size();
        if (reply[p] == '"') {
            size_t e = reply.find('"', p + 1);
            return reply.substr(p + 1, e - p - 1);
        }
        size_t e = reply.find_first_of(",}", p);
        return reply.substr(p, e - p);
    };
    std::string mode = field("mode");
    printf("mini-LMS : listening on :%d (%s)\n", IniConfig::instance().get_remote_lms_port(),
           field("player_name").c_str());
    printf("playback : %s", mode.empty() ? "unknown" : mode.c_str());
    std::string title = field("current_title");
    if (!title.empty())
        printf(" — %s", title.c_str());
    std::string time = field("time"), dur = field("duration");
    if (!time.empty())
        printf(" (%ss/%ss)", time.c_str(), dur.c_str());
    std::string vol = field("mixer volume");
    if (!vol.empty())
        printf(" vol=%s%%", vol.c_str());
    printf("\n");
    return 0;
}

int cmd_log(int argc, char **argv) {
    // journalctl -fu behaviour BY DEFAULT: print the tail, then keep following —
    //   interacting with the TUI/daemon keeps producing lines. History depth via -n N;
    //   -f/--follow accepted as an explicit no-op. Ctrl+C exits.
    int tail_n = 20;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-f" || a == "--follow")
            ; // already the default
        else if (a == "-n" && i + 1 < argc)
            tail_n = std::atoi(argv[++i]);
    }
    std::string path = today_log_path();
    std::vector<std::string> lines;
    std::ifstream f(path);
    std::string l;
    while (std::getline(f, l))
        lines.push_back(l);
    if (lines.size() > (size_t)tail_n)
        lines.erase(lines.begin(), lines.end() - tail_n);
    for (auto &s : lines)
        printf("%s\n", s.c_str());
    // journalctl -f equivalent: poll the file (handles the midnight rollover by
    //   re-resolving the "today" name each second).
    fflush(stdout);
    size_t last_size = lines.size();
    for (;;) {
        sleep(1);
        std::string now_path = today_log_path();
        std::ifstream g(now_path);
        std::vector<std::string> cur;
        while (std::getline(g, l))
            cur.push_back(l);
        if (now_path != path) { // rolled over to a new day
            path = now_path;
            last_size = 0;
        }
        for (size_t i = last_size; i < cur.size(); ++i)
            printf("%s\n", cur[i].c_str());
        if (cur.size() >= last_size)
            last_size = cur.size();
        else
            last_size = 0; // truncated/rotated file — replay from the top
        fflush(stdout);
    }
}
} // namespace

bool service_handover_takeover() {
    int pid = 0;
    if (!daemon_pid_alive(&pid))
        return false;
    // N10.5: ZERO-DROP takeover — receive the daemon's listener + live phone
    //   connections (staged; LmsServer::start adopts them). Falls through to the
    //   plain stop on any failure, exactly the pre-N10.5 behaviour.
    takeover_via_fd_passing();
    // N10.3: the service lives in the USER manager — no auth needed. A pre-N10.3
    //   install may still run the SYSTEM unit (with its polkit rule); stop that too
    //   before the pidfile fallback so a migration-era daemon can't survive beside
    //   the TUI.
    ::system(("systemctl --user stop " + std::string(UNIT) + " 2>/dev/null").c_str());
    if (daemon_pid_alive())
        ::system(("systemctl stop " + std::string(UNIT) + " 2>/dev/null").c_str());
    // N10.2: a MANUALLY started daemon (unit inactive, pidfile alive) survives both
    //   attempts — stop it by pid so the same clean-exit flush runs (SIGTERM is
    //   exactly what systemd sends).
    if (daemon_pid_alive())
        ::kill(pid, SIGTERM);
    // Wait for the daemon to finish its clean shutdown (bounded; it takes ~2-3s).
    for (int i = 0; i < 100; ++i) {
        if (!daemon_pid_alive())
            return true;
        usleep(100 * 1000);
    }
    return true;
}

void service_handover_restore() {
    // N10.5: ZERO-DROP restore — when THIS process owns live phone connections (it
    //   took them over at boot, or served them itself), transfer them to the newly
    //   started daemon before exiting. The daemon listens on its boot socket for a
    //   few seconds (see run_daemon); we connect and send; our copies then simply
    //   die with _exit while the daemon's dups keep serving.
    if (LmsServer::instance().is_running()) {
        std::string intent = handover_sock_dir() + "/panicast-handover.intent";
        FILE *f = fopen(intent.c_str(), "w"); // tells the booting daemon to expect us
        if (f)
            fclose(f);
        ::system(("systemctl --user start " + std::string(UNIT) + " 2>/dev/null").c_str());
        std::string path = handover_sock_dir() + "/panicast-daemon-hs.sock";
        for (int i = 0; i < 40; ++i) { // bounded wait for the daemon's boot socket
            if (::access(path.c_str(), F_OK) == 0)
                break;
            usleep(250 * 1000);
        }
        LmsServer::instance().send_handover_fds(path); // no-op on failure
        LmsServer::instance().detach_for_exit();
        ::unlink(intent.c_str());
    } else {
        // N10.3: user unit — start needs no privileges. If the user manager isn't
        //   available (no session), this fails silently; the next `panicast start` or
        //   TUI session retries.
        ::system(("systemctl --user start " + std::string(UNIT) + " 2>/dev/null").c_str());
    }
}

// N10.3: install (or refresh) the USER-space service unit — the whole point is that
//   NO sudo is ever needed: the unit lives under $XDG_CONFIG_HOME (~/.config), and
//   start/stop/enable go through `systemctl --user`. ExecStart points at the RUNNING
//   binary (via /proc/self/exe) so the service always runs the installed build.
//   Called on the first TUI run (auto-setup) and by `panicast start`.
void ensure_user_unit() {
    const char *home = std::getenv("HOME");
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base = xdg && *xdg ? std::string(xdg) : std::string(home ? home : "") + "/.config";
    if (base.empty())
        return;
    std::string dir = base + "/systemd/user";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return;
    char self[4096];
    ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
    std::string exe = n > 0 ? std::string(self, (size_t)n) : "/usr/local/bin/panicast";
    // NOTE: no audio env on purpose. libpulse discovers the socket via the standard
    //   $XDG_RUNTIME_DIR/pulse/native path — pipewire/pulseaudio provide it natively,
    //   and WSLg maintains it as a symlink into /mnt/wslg (verified: pactl connects
    //   through it with PULSE_SERVER unset). Hardcoding PULSE_SERVER here once pointed
    //   Arch at a nonexistent WSLg path → AO=null → silent playback.
    // The unit body as a raw string — reads exactly like the file on disk, and the
    //   {} slots make it a single literal token that cannot be orphaned by an edit.
    std::string unit = fmt::format(
        R"(# panicast user-space service (installed automatically by the first run — N10.3).
#   Manage with: panicast start|stop|restart|enable|disable (all sudo-free, systemctl --user).
[Unit]
Description=panicast headless media daemon (Squeeze Client remote)
After=network.target

[Service]
Type=simple
ExecStart={} --daemon
WorkingDirectory={}
Restart=on-failure
RestartSec=3
# The daemon's clean shutdown (mpv stop joins) takes ~2-3s.
TimeoutStopSec=15

[Install]
WantedBy=default.target
)",
        exe, std::string(home ? home : ""));
    std::string path = dir + "/" + UNIT;
    std::string existing;
    {
        std::ifstream f(path);
        std::string l;
        while (std::getline(f, l))
            existing += l + "\n";
    }
    if (existing == unit)
        return; // already current — no write, no daemon-reload churn
    {
        std::ofstream f(path);
        if (!f.is_open())
            return;
        f << unit;
    }
    ::system("systemctl --user daemon-reload 2>/dev/null");
}

int run_cli_command(int argc, char *argv[]) {
    if (argc < 2)
        return -1;
    std::string cmd = argv[1];
    if (cmd == "status")
        return cmd_status();
    if (cmd == "start") {
        ensure_user_unit();
        return cmd_start();
    }
    if (cmd == "stop")
        return systemctl("stop", false);
    if (cmd == "restart") {
        ensure_user_unit();
        return systemctl("restart", false);
    }
    // N10.3: enable/disable of a USER unit needs no sudo — it's a file in $XDG_CONFIG_HOME.
    if (cmd == "enable" || cmd == "disable") {
        ensure_user_unit();
        return systemctl(cmd.c_str(), false);
    }
    if (cmd == "log")
        return cmd_log(argc, argv);
    return -1;
}

} // namespace panicast
