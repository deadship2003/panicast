// panicast CLI service subcommands — implementation. See cli_commands.h for the contract.
#include "panicast/app/cli_commands.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include "panicast/app/daemon_mode.h"
#include "panicast/config/ini_config.h"
#include "panicast/core/paths.h"

namespace panicast
{

namespace
{
// N10: the unit runs `panicast -d` (formerly the separate panicastd binary, N09/S1 —
//   unit renamed panicastd.service → panicast.service with it).
const char *UNIT = "panicast.service";

int systemctl(const char *verb, bool use_sudo) {
    std::string cmd = use_sudo ? "sudo " : "";
    cmd += std::string("systemctl ") + verb + " " + UNIT;
    return ::system(cmd.c_str());
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
    return systemctl("start", false);
}

int cmd_status() {
    IniConfig::instance().load();
    int pid = 0;
    bool alive = daemon_pid_alive(&pid);
    printf("panicast daemon: %s", alive ? "running" : "stopped");
    if (alive)
        printf(" (pid %d)", pid);
    else if (::system(("systemctl is-enabled " + std::string(UNIT) + " >/dev/null 2>&1").c_str()) ==
             0)
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
    bool follow = false;
    int tail_n = 20;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-f" || a == "--follow")
            follow = true;
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
    if (!follow)
        return 0;
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
    std::string stop_cmd = "systemctl stop " + std::string(UNIT) + " 2>/dev/null";
    if (::system(stop_cmd.c_str()) != 0) { // no polkit rule yet — sudo fallback
        std::string sudo_cmd = "sudo systemctl stop " + std::string(UNIT);
        ::system(sudo_cmd.c_str());
    }
    // N10.2: a MANUALLY started `panicast -d` (unit inactive, pidfile alive) survives
    //   the systemctl attempt — stop it by pid so the same clean-exit flush runs
    //   (SIGTERM is exactly what systemd sends).
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
    std::string start_cmd = "systemctl start " + std::string(UNIT) + " 2>/dev/null";
    ::system(start_cmd.c_str());
}

int run_cli_command(int argc, char *argv[]) {
    if (argc < 2)
        return -1;
    std::string cmd = argv[1];
    if (cmd == "status")
        return cmd_status();
    if (cmd == "start")
        return cmd_start();
    if (cmd == "stop")
        return systemctl("stop", false);
    if (cmd == "restart")
        return systemctl("restart", false);
    if (cmd == "enable" || cmd == "disable")
        return systemctl(cmd.c_str(), true); // deliberate sudo: autostart is explicit opt-in
    if (cmd == "log")
        return cmd_log(argc, argv);
    return -1;
}

} // namespace panicast
