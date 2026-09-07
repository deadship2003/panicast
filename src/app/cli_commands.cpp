// panicast service lifecycle CLI — implementation. See cli_commands.h for the contract.
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
#include <nlohmann/json.hpp>

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

// LIF-001 unified exit codes: 0 success / 1 invalid arguments / 2 insufficient
//   privileges / 3 service operation failed / 4 unsupported scope.
enum : int {
    EXIT_OK = 0,
    EXIT_ARGS = 1,
    EXIT_PRIV = 2,
    EXIT_SVC = 3,
    EXIT_SCOPE = 4,
};

// ── systemctl plumbing — the init system is the truth source (LIF-001 red line) ──

// Run one `systemctl --user <verb> panicast.service`; maps onto the unified codes.
int systemctl_user(const char *verb) {
    std::string cmd = std::string("systemctl --user ") + verb + " " + UNIT;
    // ::system() returns the RAW wait status (256 per exit-code unit) — normalize.
    return ::system(cmd.c_str()) == 0 ? EXIT_OK : EXIT_SVC;
}

// One `systemctl --user show` call returning the properties status needs
//   (name=value lines; works for a not-installed unit too — empty/inactive values).
std::string systemctl_show_props() {
    std::string cmd = std::string("systemctl --user show ") + UNIT +
                      " -p FragmentPath -p UnitFileState -p ActiveState -p MainPID"
                      " -p ActiveEnterTimestamp -p ExecMainStatus -p Result -p NRestarts"
                      " 2>/dev/null";
    FILE *f = ::popen(cmd.c_str(), "r");
    if (!f)
        return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    ::pclose(f);
    return out;
}

std::string show_prop(const std::string &props, const char *key) {
    std::string pat = std::string(key) + "=";
    size_t p = props.find(pat);
    if (p == std::string::npos)
        return "";
    p += pat.size();
    size_t e = props.find('\n', p);
    return props.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

// ── Status model (LIF-001 #6: fixed fields; pidfile = bare-run clue only) ──────
struct ServiceState {
    std::string scope = "user";
    std::string unitPath;      // FragmentPath ("" when not installed)
    bool installed = false;
    bool enabled = false;
    std::string enabledState;  // raw UnitFileState (enabled/disabled/static/…)
    bool running = false;      // ActiveState == active
    int pid = 0;               // MainPID
    long long uptimeSeconds = -1;
    int lastExitCode = -1;     // ExecMainStatus
    std::string lastError;     // Result ("" when success / unknown)
    int restartCount = 0;      // NRestarts
    std::string platformInit = "systemd --user";
    // Cross-check clue (NOT truth): a live pidfile while the unit is inactive
    //   (or naming a different pid) = bare-run instance / anomaly → status flags it.
    bool pidfileAlive = false;
    int pidfilePid = 0;
    bool pidfileAnomaly = false;
};

ServiceState query_service_state() {
    ServiceState s;
    std::string props = systemctl_show_props();
    s.unitPath = show_prop(props, "FragmentPath");
    // systemctl reports a path even for the implicit unit; treat installed = the
    //   file actually exists on disk.
    s.installed = !s.unitPath.empty() && std::filesystem::exists(s.unitPath);
    s.enabledState = show_prop(props, "UnitFileState");
    s.enabled = s.enabledState == "enabled";
    s.running = show_prop(props, "ActiveState") == "active";
    s.pid = std::atoi(show_prop(props, "MainPID").c_str());
    s.lastExitCode = std::atoi(show_prop(props, "ExecMainStatus").c_str());
    std::string result = show_prop(props, "Result");
    s.lastError = (result.empty() || result == "success") ? "" : result;
    s.restartCount = std::atoi(show_prop(props, "NRestarts").c_str());
    // ActiveEnterTimestamp: "Sun 2026-09-07 14:13:22 CST" → epoch (local time).
    std::string ts = show_prop(props, "ActiveEnterTimestamp");
    if (s.running && !ts.empty()) {
        std::tm tmv{};
        if (strptime(ts.c_str(), "%a %Y-%m-%d %H:%M:%S", &tmv)) {
            time_t started = ::mktime(&tmv);
            if (started > 0)
                s.uptimeSeconds = (long long)::time(nullptr) - (long long)started;
        }
    }
    // pidfile cross-check — clue only (LIF-001 red line: init is the truth source).
    s.pidfileAlive = daemon_pid_alive(&s.pidfilePid);
    s.pidfileAnomaly = s.pidfileAlive && (!s.running || s.pid != s.pidfilePid);
    return s;
}

// ── Dual output (LIF-001 #6): --json everywhere ───────────────────────────────

nlohmann::json verb_result_json(const char *cmd, bool ok, const std::string &message) {
    return nlohmann::json{{"scope", "user"},
                          {"cmd", cmd},
                          {"ok", ok},
                          {"message", message}};
}

void emit(const nlohmann::json &j, bool json, const std::string &human) {
    if (json)
        printf("%s\n", j.dump().c_str());
    else if (!human.empty())
        printf("%s\n", human.c_str());
}

// ── JSON status (fixed field set per LIF-001 #6) ──────────────────────────────
nlohmann::json status_json(const ServiceState &s) {
    return nlohmann::json{{"scope", s.scope},
                          {"unitPath", s.installed ? s.unitPath : ""},
                          {"installed", s.installed},
                          {"enabled", s.enabled},
                          {"running", s.running},
                          {"pid", s.pid},
                          {"uptimeSeconds", s.uptimeSeconds},
                          {"lastExitCode", s.lastExitCode},
                          {"lastError", s.lastError},
                          {"restartCount", s.restartCount},
                          {"platformInit", s.platformInit},
                          {"pidfileAnomaly", s.pidfileAnomaly}};
}

// Human-readable uptime ("2h 13m" / "45s").
std::string human_uptime(long long sec) {
    if (sec < 0)
        return "?";
    long long h = sec / 3600, m = (sec % 3600) / 60, s2 = sec % 60;
    if (h > 0)
        return fmt::format("{}h {}m", h, m);
    if (m > 0)
        return fmt::format("{}m {}s", m, s2);
    return fmt::format("{}s", s2);
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
//   any failure). Used by human-mode `panicast service status` to report what the
//   daemon is playing.
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
    timeval tv{0, 250 * 1000}; // 250ms — response arrives in ~1ms; this only bounds pathology
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
    // Startup-latency fix: this body was MALFORMED JSON for the whole N10.5 era — the
    //   second array element lacked its opening brace (`}},"id":"999"}]` instead of
    //   `]}},{"id":"999"}]`), so the daemon's errors-discarded nlohmann parse silently
    //   dropped every takeover request: the fd transfer NEVER ran, the TUI ate the full
    //   poll() bound every launch, and the phone's live connections tore down with the
    //   daemon (zero-drop was entirely broken).
    std::string body = "[{\"channel\":\"/slim/request\",\"data\":{\"response\":\"/handover\","
                       "\"request\":[\"00:00:00:00:84:21\",[\"panicast\",\"handover\",\"" +
                       sock_path + "\"]]}},{\"id\":\"999\"}]";
    std::string req =
        "POST /cometd HTTP/1.1\r\nHost: localhost\r\nAuthorization: Basic " + auth +
        "\r\nContent-Type: text/json\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body;
    bool ok = ::send(fd, req.data(), req.size(), MSG_NOSIGNAL) == (ssize_t)req.size();
    // Startup-latency fix: the daemon keeps cometd connections ALIVE, so the old
    //   "drain until timeout" burned the full SO_RCVTIMEO (3s) on EVERY TUI launch
    //   for nothing. Read the response properly instead: headers → Content-Length →
    //   exactly that many body bytes → close. (The transfer thread connects to our
    //   unix socket within ~1ms of the POST — poll() below covers the wait, not this.)
    std::string resp;
    char sink[512];
    while (resp.size() < 64 * 1024) {
        ssize_t n = ::recv(fd, sink, sizeof(sink), 0);
        if (n <= 0)
            break; // timeout or EOF — either way we have all we're getting
        resp.append(sink, (size_t)n);
        size_t hdr_end = resp.find("\r\n\r\n");
        if (hdr_end == std::string::npos)
            continue;
        size_t cl_pos = resp.find("Content-Length:");
        if (cl_pos == std::string::npos || cl_pos > hdr_end)
            break; // no length to wait for (shouldn't happen — daemon always sends it)
        size_t v = cl_pos + strlen("Content-Length:");
        long want = strtol(resp.c_str() + v, nullptr, 10);
        if ((long)(resp.size() - hdr_end - 4) >= want)
            break; // full body received — done, close now
    }
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
    // Bounded wait for the daemon's detached transfer thread to connect. It connects
    //   within ~1ms of the POST when healthy (measured); 1s only bounds the failure
    //   path (old daemon / not running) before falling back to the plain stop.
    pollfd p{ls, POLLIN, 0};
    if (::poll(&p, 1, 1000) <= 0) {
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

// ── Subcommands ───────────────────────────────────────────────────────────────

// LIF-001 #2: install is idempotent registration — regenerate/refresh the unit,
//   never start/enable anything.
int cmd_install(bool json) {
    ensure_user_unit();
    ServiceState s = query_service_state();
    std::string msg = s.installed
                          ? fmt::format("installed (unit: {})", s.unitPath)
                          : "install failed (unit file not present after write)";
    emit(verb_result_json("install", s.installed, msg), json, "panicast: " + msg);
    return s.installed ? EXIT_OK : EXIT_SVC;
}

// LIF-001 #2: uninstall removes the registration — stop, disable, delete the unit,
//   daemon-reload. Uninstall when not installed = success message, not an error.
int cmd_uninstall(bool json) {
    ServiceState s = query_service_state();
    if (!s.installed) {
        std::string msg = "not installed — nothing to do";
        emit(verb_result_json("uninstall", true, msg), json, "panicast: " + msg);
        return EXIT_OK;
    }
    if (s.running)
        systemctl_user("stop");
    if (s.enabled)
        systemctl_user("disable");
    std::error_code ec;
    std::filesystem::remove(s.unitPath, ec);
    ::system("systemctl --user daemon-reload 2>/dev/null");
    std::string msg = ec ? fmt::format("failed to remove {}", s.unitPath)
                         : fmt::format("uninstalled (unit removed: {})", s.unitPath);
    emit(verb_result_json("uninstall", !ec, msg), json, "panicast: " + msg);
    return ec ? EXIT_SVC : EXIT_OK;
}

// LIF-001 #2 + red line ①: start re-run while already running = idempotent success
//   (the PROCESS-level single-instance guard lives in run_daemon; this is the verb
//   layer). Refused (3) while a TUI session owns the engine — starting beside it
//   would race mpv and the DB.
int cmd_start(bool json) {
    ServiceState s = query_service_state();
    if (s.running) {
        std::string msg = fmt::format("already running (pid {}, up {}) — nothing to start",
                                      s.pid, human_uptime(s.uptimeSeconds));
        emit(verb_result_json("start", true, msg), json, "panicast daemon: " + msg);
        return EXIT_OK;
    }
    int pid = 0;
    if (tui_pid_alive(&pid)) {
        std::string msg = fmt::format(
            "a TUI session owns playback right now (pid {}) — exit it first; it "
            "restarts the service automatically on exit",
            pid);
        emit(verb_result_json("start", false, msg), json, "panicast daemon: " + msg);
        return EXIT_SVC;
    }
    ensure_user_unit();
    int rc = systemctl_user("start");
    if (rc == EXIT_OK) {
        emit(verb_result_json("start", true, "started"), json, "panicast daemon: started");
    } else {
        emit(verb_result_json("start", false, "systemctl start failed (see `journalctl --user -u panicast`)"),
             json, "panicast daemon: start FAILED — `journalctl --user -u panicast` for details");
    }
    return rc;
}

// Generic passthrough verbs (stop / restart / enable / disable). enable refreshes
//   the unit first (registration upkeep); none of them start/stop anything beyond
//   their own verb (LIF-001 #3: transient control vs persistent auto-start split).
int cmd_verb(const char *cmd, bool json, bool refresh_unit_first) {
    if (refresh_unit_first)
        ensure_user_unit();
    int rc = systemctl_user(cmd);
    emit(verb_result_json(cmd, rc == EXIT_OK, rc == EXIT_OK ? std::string(cmd) + " ok"
                                                            : std::string(cmd) + " failed"),
         json, std::string("panicast daemon: ") + cmd + (rc == EXIT_OK ? " OK" : " FAILED"));
    return rc;
}

// LIF-001 #6 + red line ③: truth source = the init system; pidfile is a bare-run
//   clue and a coexisting/mismatching one is flagged as an anomaly. Human mode
//   additionally shows what the daemon is playing (mini-LMS query).
int cmd_status(bool json) {
    IniConfig::instance().load();
    ServiceState s = query_service_state();

    if (json) {
        printf("%s\n", status_json(s).dump().c_str());
        return EXIT_OK;
    }

    printf("panicast daemon: %s", s.running ? "running" : "stopped");
    if (s.running)
        printf(" (pid %d, up %s)", s.pid, human_uptime(s.uptimeSeconds).c_str());
    printf(" [%s]\n", s.enabled ? "enabled" : s.enabledState.empty() ? "not-installed"
                                                                     : s.enabledState.c_str());
    if (s.installed)
        printf("unit: %s (%s)\n", s.unitPath.c_str(), s.platformInit.c_str());
    if (s.lastExitCode > 0 || !s.lastError.empty())
        printf("last exit: code %d, result %s (restarts: %d)\n", s.lastExitCode,
               (s.lastError.empty() ? "success" : s.lastError.c_str()), s.restartCount);
    if (s.pidfileAnomaly) {
        printf("ANOMALY: the pid file claims pid %d while the unit is %s — a bare-run "
               "instance? Stop it (`kill %d` or exit that terminal) or `panicast "
               "service restart`.\n",
               s.pidfilePid, s.running ? "running a different pid" : "inactive",
               s.pidfilePid);
        return EXIT_OK;
    }
    int tui_pid = 0;
    if (tui_pid_alive(&tui_pid)) {
        printf("panicast TUI:   running (pid %d) — owns playback; the service is handed "
               "back on its exit\n",
               tui_pid);
        return EXIT_OK;
    }
    if (!s.running)
        return EXIT_OK;

    // What is it playing? Ask the daemon's own LMS endpoint (human mode only).
    std::string reply = lms_status_reply();
    if (reply.empty()) {
        printf("mini-LMS : no reply on :%d\n", IniConfig::instance().get_remote_lms_port());
        return EXIT_OK;
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
    printf("mini-LMS : listening on :%d (%s)\n", IniConfig::instance().get_remote_lms_port(),
           field("player_name").c_str());
    std::string mode = field("mode");
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
    return EXIT_OK;
}
} // namespace

bool service_handover_takeover() {
    ServiceState s = query_service_state();
    int pid = 0;
    if (!s.running) {
        // Unit inactive — only a BARE-run daemon (pidfile clue) needs stopping.
        if (!daemon_pid_alive(&pid))
            return false;
    }
    // N10.5: ZERO-DROP takeover — receive the daemon's listener + live phone
    //   connections (staged; LmsServer::start adopts them). Falls through to the
    //   plain stop on any failure, exactly the pre-N10.5 behaviour.
    takeover_via_fd_passing();
    // Truth source = init: stop through the user manager (synchronous).
    ::system(("systemctl --user stop " + std::string(UNIT) + " 2>/dev/null").c_str());
    // N10.2: a BARE-run daemon (unit inactive, pidfile alive) survives the
    //   systemctl stop — stop it by pid so the same clean-exit flush runs (SIGTERM
    //   is exactly what systemd sends).
    if (daemon_pid_alive(&pid))
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
        //   available (no session), this fails silently; the next `panicast service
        //   start` or TUI session retries.
        ::system(("systemctl --user start " + std::string(UNIT) + " 2>/dev/null").c_str());
    }
}

// N10.3: install (or refresh) the USER-space service unit — the whole point is that
//   NO sudo is ever needed: the unit lives under $XDG_CONFIG_HOME (~/.config), and
//   start/stop/enable go through `systemctl --user`. ExecStart points at the RUNNING
//   binary (via /proc/self/exe) so the service always runs the installed build.
//   LIF-001 #7/#8: the crash-restart policy lives HERE (generated at install time,
//   never hand-edited); re-running install refreshes it.
//   Called by `panicast service install` / `start` / `restart` / `enable` and on the
//   first TUI run (auto-setup).
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
        R"(# panicast user-space service (installed by `panicast service install` — N10.3).
#   Manage with: panicast service start|stop|restart|enable|disable|status
#   (all sudo-free, systemctl --user).
[Unit]
Description=panicast headless media daemon (Squeeze Client remote)
After=network.target
# LIF-001 #7: crash-restart policy with a retry cap — a permanently occupied port
#   (fatal bind) must back off to 'failed', not crash-loop every RestartSec forever.
StartLimitIntervalSec=60
StartLimitBurst=5

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

    // `panicast service <subcmd> [flags]` (LIF-001 unified entry) or the bare
    //   alias `panicast <subcmd> [flags]` — identical dispatch.
    std::string cmd = argv[1];
    int argi = 1;
    if (cmd == "service") {
        if (argc < 3) {
            std::fprintf(stderr,
                         "usage: panicast service <install|uninstall|start|stop|restart|"
                         "enable|disable|status> [--json] [--user]\n");
            return EXIT_ARGS;
        }
        cmd = argv[2];
        argi = 2;
    }

    bool json = false;
    bool have_user = false, have_system = false;
    for (int i = argi + 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json")
            json = true;
        else if (a == "--user")
            have_user = true;
        else if (a == "--system")
            have_system = true;
        else {
            std::fprintf(stderr, "panicast: unknown service option '%s'\n", a.c_str());
            return EXIT_ARGS;
        }
    }
    // LIF-001 #4: the scope flag exists for interface uniformity; the system scope
    //   is refused BY DESIGN (N10.3: user-space unit, sudo-free) with exit code 4.
    if (have_system) {
        std::string msg =
            "--system is not supported: panicast runs as a USER systemd unit by "
            "design (N10.3 — sudo-free). Omit the flag or pass --user.";
        emit(verb_result_json(cmd.c_str(), false, msg), json, "panicast: " + msg);
        return EXIT_SCOPE;
    }
    (void)have_user; // --user is the default; accepted as an explicit no-op

    if (cmd == "install")
        return cmd_install(json);
    if (cmd == "uninstall")
        return cmd_uninstall(json);
    if (cmd == "start")
        return cmd_start(json);
    if (cmd == "status")
        return cmd_status(json);
    if (cmd == "stop")
        return cmd_verb("stop", json, false);
    if (cmd == "restart")
        return cmd_verb("restart", json, true);
    if (cmd == "enable")
        return cmd_verb("enable", json, true);
    if (cmd == "disable")
        return cmd_verb("disable", json, false);

    // Unknown subcommand: inside the explicit `service` namespace it is a typo
    //   (invalid args); as a bare word it may be a normal CLI path (e.g. "-a url").
    if (argi == 2) {
        std::fprintf(stderr, "panicast: unknown service subcommand '%s'\n", cmd.c_str());
        return EXIT_ARGS;
    }
    return -1;
}

} // namespace panicast
