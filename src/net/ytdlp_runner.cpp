#include "panicast/net/ytdlp_runner.h"
#include "panicast/net/proxy_manager.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <fmt/format.h>

#include "panicast/config/ini_config.h"
#include "panicast/core/logger.h"
#include "panicast/core/utils.h"

extern char **environ; // Required by posix_spawnp

namespace panicast
{

YtdlpRunner::Result YtdlpRunner::run(const std::vector<std::string> &args,
                                     std::function<void(const std::string &)> line_cb,
                                     int timeout_sec,
                                     const std::string &source_url) {
    Result res;
    std::string ytdlp = find_ytdlp();
    if (ytdlp.empty()) {
        LOG("[YtdlpRunner] yt-dlp not found in PATH");
        return res;
    }

    // Build argv (C-style array; elements must stay alive)
    std::vector<std::string> storage;
    storage.reserve(args.size() + 3);
    storage.push_back(ytdlp);
    // D3/D45: resolve proxy via the Connectivity layer (IProxyManager). source_url lets domain
    //   rules match the request host (e.g. *.bilibili.com → direct even with a global proxy set);
    //   "" falls back to the global [network] proxy only (the pre-D45 behavior).
    std::string proxy = ProxyManager::instance().resolveProxy(source_url).url;
    if (!proxy.empty()) {
        storage.push_back("--proxy");
        storage.push_back(proxy);
    }
    for (const auto &a : args)
        storage.push_back(a);
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (auto &s : storage)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0) {
        LOG("[YtdlpRunner] pipe() failed");
        return res;
    }
    if (pipe(err_pipe) != 0) {
        LOG("[YtdlpRunner] pipe() failed");
        close(out_pipe[0]);
        close(out_pipe[1]);
        return res;
    }

    // N04-fix: fork+exec with PR_SET_PDEATHSIG + new process group (see Utils::spawn_child).
    //   PDEATHSIG makes the yt-dlp child auto-die when panicast is killed (even by SIGKILL);
    //   the new pgroup lets graceful shutdown kill(-pid) take down yt-dlp + its ffmpeg merge child.
    pid_t pid = Utils::spawn_child(ytdlp, argv.data(), -1, out_pipe[1], err_pipe[1], true);
    close(out_pipe[1]);
    close(err_pipe[1]);

    if (pid < 0) {
        LOG(fmt::format("[YtdlpRunner] fork/spawn failed: {}", strerror(errno)));
        close(out_pipe[0]);
        close(err_pipe[0]);
        return res;
    }
    res.launched = true;
    Utils::register_child_pid(pid); // N04-fix: track for shutdown kill

    // Concurrently read stdout/stderr to avoid deadlock when one side fills the pipe buffer before the other reaches EOF
    constexpr size_t MAX_CAPTURE = 8 * 1024 * 1024; // Prevent unbounded growth
    int effective_timeout = (timeout_sec > 0) ? timeout_sec : 30;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(effective_timeout);
    std::string out_pending;
    bool out_open = true, err_open = true;
    bool timed_out = false;

    while (out_open || err_open) {
        struct pollfd fds[2];
        int nfds = 0;
        if (out_open) {
            fds[nfds].fd = out_pipe[0];
            fds[nfds].events = POLLIN;
            nfds++;
        }
        if (err_open) {
            fds[nfds].fd = err_pipe[0];
            fds[nfds].events = POLLIN;
            nfds++;
        }
        int pr = poll(fds, nfds, 1000); // Wake up once per second to check timeout
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                timed_out = true;
                break;
            }
            continue;
        }
        for (int i = 0; i < nfds; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            char buf[4096];
            ssize_t n = read(fds[i].fd, buf, sizeof(buf));
            if (n > 0) {
                bool is_out = (fds[i].fd == out_pipe[0]);
                std::string &dst = is_out ? res.stdout_output : res.stderr_output;
                if (dst.size() < MAX_CAPTURE) {
                    size_t add = std::min((size_t)n, MAX_CAPTURE - dst.size());
                    dst.append(buf, add);
                }
                if (is_out && line_cb) {
                    if (out_pending.size() < MAX_CAPTURE)
                        out_pending.append(buf, n);
                    size_t pos;
                    while ((pos = out_pending.find('\n')) != std::string::npos) {
                        line_cb(out_pending.substr(0, pos));
                        out_pending.erase(0, pos + 1);
                    }
                }
            } else if (n == 0) {
                if (fds[i].fd == out_pipe[0]) {
                    out_open = false;
                    if (line_cb && !out_pending.empty()) {
                        line_cb(out_pending);
                        out_pending.clear();
                    }
                } else {
                    err_open = false;
                }
            } else if (errno != EINTR) {
                if (fds[i].fd == out_pipe[0])
                    out_open = false;
                else
                    err_open = false;
            }
        }
    }
    close(out_pipe[0]);
    close(err_pipe[0]);

    if (timed_out) {
        LOG(fmt::format("[YtdlpRunner] timeout after {}s, killing child", effective_timeout));
        kill(-pid, SIGTERM); // N04-fix: group kill (yt-dlp + its ffmpeg child)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (waitpid(pid, nullptr, WNOHANG) == 0)
            kill(-pid, SIGKILL); // Force kill if still not exited
    }

    int status = 0;
    if (waitpid(pid, &status, 0) > 0) {
        res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    } else {
        res.exit_code = -1;
    }
    Utils::unregister_child_pid(pid); // N04-fix: untrack (reaped)
    return res;
}

std::string YtdlpRunner::find_ytdlp() {
    // Add a regular-file check (access X_OK is also true for directories)
    auto is_exec_file = [](const std::string &p) -> bool {
        struct stat st;
        return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0;
    };
    std::string path_env = std::getenv("PATH") ? std::getenv("PATH") : "";
    std::istringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty())
            continue;
        std::string candidate = dir + "/yt-dlp";
        if (is_exec_file(candidate))
            return candidate;
    }
    if (is_exec_file("/usr/bin/yt-dlp"))
        return "/usr/bin/yt-dlp";
    if (is_exec_file("/usr/local/bin/yt-dlp"))
        return "/usr/local/bin/yt-dlp";
    return "";
}

} // namespace panicast
