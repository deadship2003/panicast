// Remote control TCP server implementation. N01: accept-loop skeleton only.
//   See remote_server.h for the threading model and platform scope.
#include "panicast/net/remote_server.h"

#include "panicast/core/logger.h"
#include "panicast/core/event_log.h"
#include "panicast/config/ini_config.h"
#include "panicast/net/remote_command_bus.h"
#include "panicast/net/remote_session.h"
#include "panicast/net/remote_ws.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace panicast
{


RemoteServer::RemoteServer(RemoteCommandBus &bus) : bus_(bus) {}

RemoteServer::~RemoteServer() {
    stop();
}

// Create a listening socket. For wildcard binds (0.0.0.0 / :: / empty) use an IPv6 dual-stack
//   socket (V6ONLY=0) so it accepts BOTH IPv4 (as ::ffff:a.b.c.d) and IPv6 (incl. ::1) clients —
//   needed so a browser that resolves localhost→::1 can reach the server. Returns -1 on failure.
static int make_listen_fd(const std::string &bind_addr, int port) {
    bool dual = (bind_addr.empty() || bind_addr == "0.0.0.0" || bind_addr == "::");
    int fd = -1;
    if (dual) {
        fd = ::socket(AF_INET6, SOCK_STREAM, 0);
        if (fd >= 0) {
            int v6only = 0; // dual-stack: accept IPv4-mapped + native IPv6
            ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
            int yes = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            struct sockaddr_in6 a6{};
            a6.sin6_family = AF_INET6;
            a6.sin6_port = htons(static_cast<uint16_t>(port));
            a6.sin6_addr = in6addr_any;
            if (::bind(fd, reinterpret_cast<struct sockaddr *>(&a6), sizeof(a6)) < 0) {
                LOG(fmt::format("[REMOTE] bind6(:{}) failed: {}", port, std::strerror(errno)));
                ::close(fd);
                fd = -1;
            }
        }
    }
    if (fd < 0) { // fallback: IPv4 only
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in a4{};
        a4.sin_family = AF_INET;
        a4.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, bind_addr.c_str(), &a4.sin_addr) <= 0) {
            ::close(fd);
            return -1;
        }
        if (::bind(fd, reinterpret_cast<struct sockaddr *>(&a4), sizeof(a4)) < 0) {
            LOG(fmt::format("[REMOTE] bind4({}:{}) failed: {}", bind_addr, port,
                            std::strerror(errno)));
            ::close(fd);
            return -1;
        }
    }
    if (::listen(fd, 8) < 0) {
        LOG(fmt::format("[REMOTE] listen(:{}) failed: {}", port, std::strerror(errno)));
        ::close(fd);
        return -1;
    }
    return fd;
}

// String form of a peer's IP (for logging). Dual-stack: IPv4 clients on an IPv6 socket appear as
//   "::ffff:a.b.c.d". is_loopback covers 127.0.0.1, ::1, and the IPv4-mapped loopback.
static std::string peer_ip(const struct sockaddr_storage *ss) {
    char ip[INET6_ADDRSTRLEN] = {0};
    if (ss->ss_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const struct sockaddr_in *>(ss)->sin_addr, ip,
                  sizeof(ip));
    } else if (ss->ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &reinterpret_cast<const struct sockaddr_in6 *>(ss)->sin6_addr, ip,
                  sizeof(ip));
    }
    return std::string(ip);
}
static bool is_loopback_ip(const std::string &ip) {
    return ip == "127.0.0.1" || ip == "::1" || ip == "::ffff:127.0.0.1" || ip == "::ffff:127.0.0.1";
}

bool RemoteServer::start(const std::string &bind_addr, int port, RemoteControlInterface *control) {
    if (running_.load())
        return true;

    control_ = control;
    dynamic_pin_ = regenerate_pin(); // N04: random 4-digit PIN shown in the panicast popup
    universal_pin_ =
        IniConfig::instance().get_remote_universal_pin(); // N04: configurable (default 6696)

    listen_fd_ = make_listen_fd(bind_addr, port); // dual-stack (IPv4+IPv6) when bind=0.0.0.0
    if (listen_fd_ < 0) {
        return false;
    }

    bind_addr_ = bind_addr;
    port_ = port;
    running_.store(true);

    // N06: WebSocket/HTTP listener on port+1 (serves the embedded BS client + WS bridge).
    ws_listen_fd_ = make_listen_fd(bind_addr, port + 1);
    if (ws_listen_fd_ >= 0) {
        ws_accept_thread_ = std::thread(&RemoteServer::ws_accept_loop, this);
    } else {
        LOG(fmt::format("[REMOTE] WS listener on :{} failed", port + 1));
    }

    accept_thread_ = std::thread(&RemoteServer::accept_loop, this);
    diff_thread_ = std::thread(&RemoteServer::diff_loop, this); // N07: state-diff → idle push
    // N05: UDP discovery responder (APK broadcasts a probe; panicast answers with its ports).
    {
        int dp = IniConfig::instance().get_remote_discovery_port();
        discovery_thread_ = std::thread(&RemoteServer::discovery_loop, this, dp);
    }
    EVENT_LOG(
        fmt::format("Remote control server listening on {}:{} (dual-stack)", bind_addr, port));
    return true;
}

void RemoteServer::stop() {
    if (!running_.exchange(false))
        return; // already stopped / never started

    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR); // unblock accept()
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (ws_listen_fd_ >= 0) { // N06: unblock + close the WS listener
        ::shutdown(ws_listen_fd_, SHUT_RDWR);
        ::close(ws_listen_fd_);
        ws_listen_fd_ = -1;
    }
    if (accept_thread_.joinable())
        accept_thread_.join();
    if (ws_accept_thread_.joinable())
        ws_accept_thread_.join(); // N06
    if (diff_thread_.joinable())
        diff_thread_.join(); // N07
    if (discovery_thread_.joinable())
        discovery_thread_.join(); // N05

    // Join any workers still running. N01 handlers are instant (banner + close), so this is
    //   prompt; the loop is here so future protocol versions stay safe under stop().
    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        for (auto &w : workers_) {
            if (w->t.joinable())
                w->t.join();
        }
        workers_.clear();
    }
}

void RemoteServer::reap_workers() {
    // workers_mtx_ MUST be held by the caller.
    for (auto it = workers_.begin(); it != workers_.end();) {
        if ((*it)->done.load() && (*it)->t.joinable()) {
            (*it)->t.join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
}

void RemoteServer::accept_loop() {
    while (running_.load()) {
        {
            std::lock_guard<std::mutex> lk(workers_mtx_);
            reap_workers();
        }

        struct sockaddr_storage client{};
        socklen_t client_len = sizeof(client);
        int fd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr *>(&client), &client_len);
        if (fd < 0) {
            if (!running_.load())
                break; // listen socket closed by stop()
            if (errno == EINTR)
                continue; // interrupted by signal — retry
            LOG(fmt::format("[REMOTE] accept() failed: {}", std::strerror(errno)));
            continue;
        }

        // N04: localhost (loopback) connections are open (no PIN) — so a browser on the panicast
        //   host controls directly. Off-host connections require the PIN.
        std::string ipstr = peer_ip(&client);
        bool localhost = is_loopback_ip(ipstr);
        // N04: when an off-host client connects, surface the pairing PIN in the LOG area (via the
        //   UI thread) so the user sees it for pairing — non-blocking (a popup would freeze the TUI).
        if (!localhost) {
            bus_.push(RemoteCommand{"_pin_log", {ipstr}, 0});
        }

        auto w = std::make_unique<Worker>();
        auto *wp = w.get();
        int64_t cid = next_client_id_.fetch_add(1);
        w->t = std::thread([this, wp, fd, cid, localhost]() {
            handle_client(fd, cid, localhost);
            wp->done.store(true);
        });
        std::lock_guard<std::mutex> lk(workers_mtx_);
        workers_.push_back(std::move(w));
    }
}

void RemoteServer::ws_accept_loop() {
    // N06: accept HTTP/WS connections. Each runs ws_serve_connection (static-serve OR WS bridge)
    //   on a tracked worker thread — same tracking/reap model as the TCP accept loop.
    while (running_.load()) {
        {
            std::lock_guard<std::mutex> lk(workers_mtx_);
            reap_workers();
        }
        struct sockaddr_storage client{};
        socklen_t client_len = sizeof(client);
        int fd = ::accept(ws_listen_fd_, reinterpret_cast<struct sockaddr *>(&client), &client_len);
        if (fd < 0) {
            if (!running_.load())
                break;
            if (errno == EINTR)
                continue;
            LOG(fmt::format("[REMOTE] ws accept() failed: {}", std::strerror(errno)));
            continue;
        }
        std::string ipstr = peer_ip(&client);
        bool localhost = is_loopback_ip(ipstr);
        auto w = std::make_unique<Worker>();
        auto *wp = w.get();
        w->t = std::thread([this, wp, fd, localhost]() {
            ws_serve_connection(fd, *this, localhost);
            wp->done.store(true);
        });
        std::lock_guard<std::mutex> lk(workers_mtx_);
        workers_.push_back(std::move(w));
    }
}

void RemoteServer::handle_client(int fd, int64_t client_id, bool localhost) {
    // One RemoteSession per connection. It owns fd and closes it on destruction. N02: synchronous
    //   PRP line protocol over raw TCP. (N08: the WS frontend will feed the same engine.)
    RemoteSession session(fd, fd, client_id, bus_, control_, /*open_access=*/localhost,
                          dynamic_pin_, universal_pin_);
    register_session(&session); // N07: so notify() can reach it while idling
    session.run();
    unregister_session(&session);
}

std::string RemoteServer::regenerate_pin() {
    // Random 4-digit PIN (displayed in the panicast popup). std::random_device is non-deterministic.
    std::random_device rd;
    int n = rd() % 10000;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04d", n);
    dynamic_pin_ = buf;
    return dynamic_pin_;
}

void RemoteServer::register_session(RemoteSession *s) {
    std::lock_guard<std::mutex> lk(sessions_mtx_);
    sessions_.push_back(s);
}

void RemoteServer::unregister_session(RemoteSession *s) {
    std::lock_guard<std::mutex> lk(sessions_mtx_);
    sessions_.erase(std::remove(sessions_.begin(), sessions_.end(), s), sessions_.end());
}

void RemoteServer::discovery_loop(int udp_port) {
    // N05: UDP discovery responder. The APK broadcasts "PANICAST_DISCOVER" to udp_port; panicast
    //   answers with a one-line beacon carrying its TCP/WS ports. The APK uses the response's
    //   source address as the player's host. SO_REUSEADDR so rebind works right after restart.
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        LOG(fmt::format("[REMOTE] discovery socket() failed: {}", std::strerror(errno)));
        return;
    }
    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(udp_port));
    if (::bind(s, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        LOG(fmt::format("[REMOTE] discovery bind(:{}) failed: {}", udp_port, std::strerror(errno)));
        ::close(s);
        return;
    }
    LOG(fmt::format("[REMOTE] discovery responder on udp {}", udp_port));

    char buf[256];
    while (running_.load()) {
        struct sockaddr_storage from{};
        socklen_t flen = sizeof(from);
        ssize_t n = ::recvfrom(s, buf, sizeof(buf) - 1, 0,
                               reinterpret_cast<struct sockaddr *>(&from), &flen);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break; // socket closed / error
        }
        if (!running_.load())
            break;
        buf[n > 0 ? n : 0] = '\0';
        if (std::string(buf).rfind("PANICAST_DISCOVER", 0) == 0) {
            // ws port = tcp port + 1 (convention: 18421 TCP, 18422 WS).
            std::string resp = fmt::format("PANICAST 1 tcp={} ws={}\n", port_, port_ + 1);
            ::sendto(s, resp.data(), resp.size(), 0, reinterpret_cast<struct sockaddr *>(&from),
                     flen);
        }
    }
    ::close(s);
}

void RemoteServer::notify(const std::string &subsystem) {
    // Snapshot the set under the lock, then deliver outside the lock (notify_change takes the
    //   session's own idle_mtx_ — avoids holding sessions_mtx_ during per-session work).
    std::vector<RemoteSession *> snap;
    {
        std::lock_guard<std::mutex> lk(sessions_mtx_);
        snap = sessions_;
    }
    for (RemoteSession *s : snap) {
        if (s)
            s->notify_change(subsystem);
    }
}

void RemoteServer::diff_loop() {
    // N07: poll the state snapshot at ~10Hz and emit `changed: <subsystem>` when a tracked field
    //   changes. This catches mpv-internal changes (seek/pause/track-end) without App
    //   instrumentation. The 100ms poll IS the coalesce window. Only the diff thread touches
    //   last_snap_, so no mutex needed for it.
    RemoteStateSnapshot last{};
    bool have_last = false;
    while (running_.load()) {
        // sleep 100ms (interruptible-ish via running_ check)
        for (int i = 0; i < 10 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!running_.load() || !control_)
            continue;

        RemoteStateSnapshot s = control_->snapshot_state();
        if (!have_last) {
            last = s;
            have_last = true;
            continue;
        }

        auto state_str = [](const RemoteStateSnapshot &x) {
            return !x.has_media ? std::string("stop") : (x.paused ? "pause" : "play");
        };
        if (state_str(s) != state_str(last) || s.current_index != last.current_index ||
            s.title != last.title || s.url != last.url || s.elapsed != last.elapsed /* seek */) {
            notify("player");
        }
        if (s.volume != last.volume)
            notify("mixer");
        if (s.speed != last.speed || s.play_mode != last.play_mode ||
            s.sleep_remaining != last.sleep_remaining) {
            notify("options");
        }
        if (s.mode != last.mode)
            notify("mode");
        if (s.subtitle_active != last.subtitle_active)
            notify("subtitle");
        if (s.art_url != last.art_url)
            notify("art");

        last = s;
    }
}


} // namespace panicast
