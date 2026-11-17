// mini-LMS server implementation — Squeezer-only cometd/Bayeux JSON-RPC control plane.
//   See lms_server.h for the protocol/threading/gating contract.
#include "panicast/net/lms_server.h"

#include "panicast/config/ini_config.h"
#include "panicast/net/bilibili_api.h"
#include "panicast/net/douyin_api.h"
#include "panicast/net/google_oauth.h"
#include "panicast/core/logger.h"
#include "panicast/net/remote_command_bus.h"
#include "panicast/net/slim_discovery.h"

#include <fmt/format.h>
#include <fmt/ranges.h> // fmt::join (separate header since fmt 8; Arch's fmt needs it)

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace panicast
{

namespace
{
// Same dual-stack listener as remote_server.cpp's make_listen_fd.
int make_listen_fd(const std::string &bind_addr, int port) {
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
                LOG(fmt::format("[LMS] bind6(:{}) failed: {}", port, std::strerror(errno)));
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
        if (::inet_pton(AF_INET, bind_addr.c_str(), &a4.sin_addr) <= 0)
            a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<struct sockaddr *>(&a4), sizeof(a4)) < 0) {
            LOG(fmt::format("[LMS] bind(:{}) failed: {}", port, std::strerror(errno)));
            ::close(fd);
            return -1;
        }
    }
    if (::listen(fd, 16) < 0) {
        LOG(fmt::format("[LMS] listen(:{}) failed: {}", port, std::strerror(errno)));
        ::close(fd);
        return -1;
    }
    return fd;
}

// Parse "a.b.c.d[/n],..." into network-order CIDRs. Bare IP = /32. Invalid entries are
//   logged and skipped (a typo must not silently open the list to everything).
std::vector<LmsCidr> parse_cidrs(const std::string &csv) {
    std::vector<LmsCidr> out;
    std::string cur;
    auto emit = [&](const std::string &entry) {
        std::string e = entry;
        e.erase(0, e.find_first_not_of(" \t"));
        e.erase(e.find_last_not_of(" \t") + 1);
        if (e.empty())
            return;
        int bits = 32;
        std::string ip = e;
        size_t slash = e.find('/');
        if (slash != std::string::npos) {
            ip = e.substr(0, slash);
            bits = std::atoi(e.c_str() + slash + 1);
            if (bits < 0 || bits > 32) {
                LOG(fmt::format("[LMS] lms_allow: bad prefix in '{}', skipped", e));
                return;
            }
        }
        struct in_addr a{};
        if (::inet_pton(AF_INET, ip.c_str(), &a) != 1) {
            LOG(fmt::format("[LMS] lms_allow: bad IP in '{}', skipped", e));
            return;
        }
        uint32_t host_mask = (bits == 0) ? 0u : (0xFFFFFFFFu << (32 - bits));
        out.push_back({a.s_addr & htonl(host_mask), htonl(host_mask)});
    };
    for (char c : csv) {
        if (c == ',') {
            emit(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    emit(cur);
    return out;
}

// Constant-time equality — a byte-wise diff accumulator, so a probing client can't learn
//   the password one character at a time from response timing.
bool ct_equal(const std::string &a, const std::string &b) {
    unsigned char diff = (unsigned char)(a.size() ^ b.size());
    size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        diff |= (unsigned char)(a[i % a.size()]) ^ (unsigned char)(b[i % b.size()]);
    return diff == 0;
}

// Minimal base64 decode (standard alphabet) for HTTP Basic auth.
std::string b64decode(const std::string &in) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto dv = [&](char c) -> int {
        const char *p = std::strchr(tbl, c);
        return (c && p) ? (int)(p - tbl) : -1;
    };
    std::string out;
    int val = 0, bits = 0;
    for (char c : in) {
        if (c == '=')
            break;
        int d = dv(c);
        if (d < 0)
            continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((val >> bits) & 0xFF);
        }
    }
    return out;
}
} // namespace

LmsServer &LmsServer::instance() {
    static LmsServer s;
    return s;
}

LmsServer::~LmsServer() {
    stop();
}

// ── N10.5 zero-drop handover ────────────────────────────────────────────────────

bool lms_recv_handover_msg(int conn_fd, int &listen_fd, std::vector<LmsAdoptedConn> &out) {
    // One sendmsg carried: iov = "<metadata JSON>\n", cmsg = all fds (listen first).
    std::vector<char> buf(65536);
    std::vector<char> cmsg_buf(CMSG_SPACE(sizeof(int) * 64));
    struct msghdr msgh{};
    struct iovec iov{buf.data(), buf.size()};
    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control = cmsg_buf.data();
    msgh.msg_controllen = cmsg_buf.size();
    ssize_t n = ::recvmsg(conn_fd, &msgh, 0);
    if (n <= 0) {
        LOG(fmt::format("[LMS-HANDOVER] recvmsg failed: {}", std::strerror(errno)));
        return false;
    }
    std::vector<int> fds;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh); cmsg; cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int *data = (int *)CMSG_DATA(cmsg);
            size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            fds.assign(data, data + count);
        }
    }
    if (fds.empty()) {
        LOG("[LMS-HANDOVER] no fds in message");
        return false;
    }
    std::string payload(buf.data(), (size_t)n);
    size_t nl = payload.find('\n');
    try {
        nlohmann::json meta = nlohmann::json::parse(
            payload.substr(0, nl == std::string::npos ? payload.size() : nl), nullptr, false);
        int lfd = meta.value("listen", -1);
        if (lfd < 0 || lfd >= (int)fds.size()) {
            LOG("[LMS-HANDOVER] bad listen index in metadata");
            for (int fd : fds)
                ::close(fd);
            return false;
        }
        listen_fd = fds[lfd];
        for (const auto &j : meta.value("conns", nlohmann::json::array())) {
            LmsAdoptedConn ac;
            int idx = j.value("fd", -1);
            if (idx < 0 || idx >= (int)fds.size())
                continue;
            ac.fd = fds[idx];
            ac.listener = j.value("listener", false);
            ac.authed = j.value("authed", false);
            ac.bayeux_cid = j.value("cid", std::string());
            out.push_back(ac);
        }
        // close fds we did not map into entries (incl. the listen if unmapped)
        for (size_t i = 0; i < fds.size(); ++i) {
            bool used = (int)i == lfd;
            for (const auto &ac : out)
                used = used || ac.fd == fds[i];
            if (!used)
                ::close(fds[i]);
        }
        return true;
    } catch (const std::exception &e) {
        LOG(fmt::format("[LMS-HANDOVER] metadata parse failed: {}", e.what()));
        for (int fd : fds)
            ::close(fd);
        return false;
    }
}

// Staged adoption state (process-global: the TUI receives the fds BEFORE App/LmsServer
//   exist, start() picks them up when the engine boots).
namespace
{
std::mutex g_stage_mtx;
int g_stage_listen_fd = -1;
std::vector<LmsAdoptedConn> g_stage_conns;
} // namespace

void LmsServer::stage_handover(int listen_fd, std::vector<LmsAdoptedConn> conns) {
    std::lock_guard<std::mutex> lk(g_stage_mtx);
    if (g_stage_listen_fd >= 0)
        ::close(g_stage_listen_fd); // superseded staging (shouldn't happen)
    for (auto &c : g_stage_conns)
        if (c.fd >= 0)
            ::close(c.fd);
    g_stage_listen_fd = listen_fd;
    g_stage_conns = std::move(conns);
}

bool LmsServer::handover_staged() {
    std::lock_guard<std::mutex> lk(g_stage_mtx);
    return g_stage_listen_fd >= 0;
}

void LmsServer::send_handover_fds(const std::string &unix_path) {
    // Snapshot the fds to transfer under the conn lock (thread-safe against reaping).
    std::vector<int> fds;
    nlohmann::json meta;
    meta["conns"] = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lk(conns_mtx_);
        reap_done();
        if (listen_fd_ < 0) {
            LOG("[LMS-HANDOVER] no listener to transfer (server not running) — takeover falls "
                "back to plain stop");
            return;
        }
        fds.push_back(listen_fd_);
        meta["listen"] = 0;
        for (auto &c : conns_) {
            if (c->fd < 0)
                continue;
            fds.push_back(c->fd);
            nlohmann::json j;
            j["fd"] = (int)fds.size() - 1;
            j["listener"] = c->listener;
            j["authed"] = c->authed.load();
            j["cid"] = c->bayeux_cid;
            meta["conns"].push_back(j);
        }
    }
    int us = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (us < 0)
        return;
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, unix_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(us, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG(fmt::format("[LMS-HANDOVER] connect {} failed: {}", unix_path, std::strerror(errno)));
        ::close(us);
        return;
    }
    // One sendmsg: metadata JSON line as iovec + every fd in a single SCM_RIGHTS cmsg.
    std::string payload = meta.dump() + "\n";
    std::vector<char> cmsg_buf(CMSG_SPACE(sizeof(int) * fds.size()));
    struct msghdr msgh{};
    struct iovec iov{payload.data(), payload.size()};
    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control = cmsg_buf.data();
    msgh.msg_controllen = cmsg_buf.size();
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
    memcpy(CMSG_DATA(cmsg), fds.data(), sizeof(int) * fds.size());
    msgh.msg_controllen = cmsg->cmsg_len;
    if (::sendmsg(us, &msgh, 0) < 0) {
        LOG(fmt::format("[LMS-HANDOVER] sendmsg failed: {}", std::strerror(errno)));
        ::close(us);
        return;
    }
    ::shutdown(us, SHUT_WR); // EOF signals "that's all"
    LOG(fmt::format("[LMS-HANDOVER] sent {} fds (1 listener + {} conns) to {}", fds.size(),
                    fds.size() - 1, unix_path));
    ::close(us);
}

void LmsServer::detach_for_exit() {
    // Quiesce WITHOUT touching the sockets: shutdown()/close on OUR copies of the
    //   CONNECTION fds is harmless to the new owner (kernel-duped), but shutdown()
    //   on the LISTENING socket would kill accepting for the dup too — so the
    //   acceptor is woken with a self-connect instead, and connection fds are simply
    //   left open. Held-stream pumps notice running_=false within one 250ms poll and
    //   exit; request readers blocked in recv() linger until _exit() reaps the
    //   process (the Conn objects are intentionally leaked with them).
    if (!running_.exchange(false))
        return;
    if (listen_fd_ >= 0 && port_ > 0) { // self-connect: accept() returns, sees the flag
        int w = ::socket(AF_INET, SOCK_STREAM, 0);
        if (w >= 0) {
            struct sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons((uint16_t)port_);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::connect(w, (struct sockaddr *)&a, sizeof(a)) == 0)
                ::close(w); // accepted + dropped by the exiting accept loop
            else
                ::close(w);
        }
    }
    if (accept_thread_.joinable())
        accept_thread_.join();
    {
        std::lock_guard<std::mutex> lk(conns_mtx_);
        for (auto &c : conns_)
            if (c->listener && c->reader.joinable())
                c->reader.join(); // pumps poll the flag; bounded
    }
    LOG("[LMS-HANDOVER] detached for exit (connection fds live in the new owner)");
}

bool LmsServer::start(const std::string &bind_addr, int port, RemoteControlInterface *control,
                      RemoteCommandBus *bus) {
    if (running_.load())
        return true;

    // Access policy (read once — restart after editing lms_allow/lms_user/lms_pass).
    std::string allow_csv = IniConfig::instance().get_remote_lms_allow();
    allow_all_ = allow_csv.find_first_not_of(" \t") == std::string::npos;
    allow_ = parse_cidrs(allow_csv);
    lms_user_ = IniConfig::instance().get_remote_lms_user();
    lms_pass_ = IniConfig::instance().get_remote_lms_pass();
    auth_required_ = !lms_pass_.empty();

    control_ = control;
    bus_ = bus;

    // N10.5: a staged handover ADOPTS the previous owner's listener + live phone
    //   connections instead of binding — the phone never saw a disconnect.
    {
        std::lock_guard<std::mutex> lk(g_stage_mtx);
        if (g_stage_listen_fd >= 0) {
            listen_fd_ = g_stage_listen_fd;
            port_ = port;
            bind_addr_ = bind_addr;
            running_.store(true);
            accept_thread_ = std::thread(&LmsServer::accept_loop, this);
            for (auto &ac : g_stage_conns) {
                auto c = std::make_unique<Conn>();
                c->fd = ac.fd;
                c->client_id = next_client_id_++;
                c->authed.store(ac.authed);
                c->bayeux_cid = ac.bayeux_cid;
                c->listener = ac.listener;
                Conn *raw = c.get();
                if (ac.listener) {
                    {
                        std::lock_guard<std::mutex> lk2(listeners_mtx_);
                        listeners_[ac.bayeux_cid] = raw;
                    }
                    // the pump continues on a fresh thread; the initial ack chunk was
                    //   already flushed by the previous owner
                    c->reader = std::thread([this, raw] {
                        std::string last_push; // start fresh — next change re-pushes
                        (void)last_push;
                        listen_loop(raw);
                    });
                } else {
                    c->reader = std::thread([this, raw] { client_loop(raw); });
                }
                conns_.push_back(std::move(c));
            }
            int adopted = (int)conns_.size();
            g_stage_listen_fd = -1;
            g_stage_conns.clear();
            LOG(fmt::format("[LMS-HANDOVER] adopted listener + {} live connections", adopted));
            return true;
        }
    }

    listen_fd_ = make_listen_fd(bind_addr, port);
    if (listen_fd_ < 0)
        return false;

    bind_addr_ = bind_addr;
    port_ = port;
    running_.store(true);
    accept_thread_ = std::thread(&LmsServer::accept_loop, this);
    LOG(fmt::format("[LMS] mini-LMS (Squeezer cometd control plane) listening on {}:{} — point "
                    "Squeezer at <this host>:{}",
                    bind_addr, port, port));
    LOG(fmt::format("[LMS] allowlist: {} | Basic auth: {}", allow_all_ ? "ALL sources" : allow_csv,
                    auth_required_ ? fmt::format("on (user '{}')", lms_user_) : "off"));

    // META-7: start the Slim UDP discovery responder alongside the LMS server so
    //   Squeezer's auto-scan finds every panicast host on the LAN.
    SlimDiscovery::instance().start(port);
    return true;
}

void LmsServer::stop() {
    if (!running_.exchange(false))
        return;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR); // unblock accept()
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable())
        accept_thread_.join();
    { // closing fds unblocks the readers; then join + drop them
        std::lock_guard<std::mutex> lk(conns_mtx_);
        for (auto &c : conns_) {
            if (c->fd >= 0) {
                ::shutdown(c->fd, SHUT_RDWR);
                ::close(c->fd);
                c->fd = -1;
            }
        }
        for (auto &c : conns_)
            if (c->reader.joinable())
                c->reader.join();
        conns_.clear();
    }
    SlimDiscovery::instance().stop();
    LOG("[LMS] mini-LMS server stopped");
}

void LmsServer::accept_loop() {
    while (running_.load()) {
        struct sockaddr_storage peer{};
        socklen_t plen = sizeof(peer);
        int fd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr *>(&peer), &plen);
        if (fd < 0) {
            if (!running_.load())
                break; // listen fd closed by stop()
            continue;
        }
        // Source-IP gate (lms_allow): reject before the protocol layer ever speaks.
        if (!peer_allowed(peer)) {
            char ip[INET6_ADDRSTRLEN] = "?";
            if (peer.ss_family == AF_INET6)
                ::inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&peer)->sin6_addr, ip, sizeof(ip));
            else if (peer.ss_family == AF_INET)
                ::inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr, ip, sizeof(ip));
            LOG(fmt::format("[LMS] rejected connection from {} (not in lms_allow)", ip));
            ::close(fd);
            continue;
        }
        int nodelay = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        std::lock_guard<std::mutex> lk(conns_mtx_);
        reap_done();
        auto c = std::make_unique<Conn>();
        c->fd = fd;
        c->client_id = next_client_id_++;
        Conn *raw = c.get();
        c->reader = std::thread([this, raw] { client_loop(raw); });
        conns_.push_back(std::move(c));
    }
}

// lms_allow check. IPv4 and v4-mapped IPv6 go through the CIDR list; native IPv6 only
//   passes for ::1 (loopback) — the default list is v4-shaped by design.
bool LmsServer::peer_allowed(const sockaddr_storage &peer) const {
    if (allow_all_)
        return true;
    const unsigned char *v4 = nullptr;
    if (peer.ss_family == AF_INET) {
        v4 = (const unsigned char *)&((const struct sockaddr_in *)&peer)->sin_addr;
    } else if (peer.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&peer;
        const unsigned char *b = (const unsigned char *)&a6->sin6_addr;
        static const unsigned char v4mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        static const unsigned char v6loop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        if (std::equal(b, b + 12, v4mapped_prefix))
            v4 = b + 12;
        else if (std::equal(b, b + 16, v6loop))
            return true;
        else
            return false;
    } else {
        return false;
    }
    uint32_t ip;
    std::memcpy(&ip, v4, sizeof(ip)); // network order
    for (const auto &c : allow_)
        if ((ip & c.mask) == c.net)
            return true;
    return false;
}

void LmsServer::reap_done() { // conns_mtx_ held
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                [](const std::unique_ptr<Conn> &c) {
                                    if (c->done.load() && c->reader.joinable())
                                        c->reader.join();
                                    return c->done.load();
                                }),
                 conns_.end());
}

void LmsServer::client_loop(Conn *c) {
    LOG(fmt::format("[LMS] client {} connected", c->client_id));
    std::string pending;
    char buf[4096];
    bool checked_first = false;
    while (running_.load()) {
        ssize_t n = ::recv(c->fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break; // EOF / error → connection gone
        pending.append(buf, static_cast<size_t>(n));

        // Squeezer-only: the first request line must be HTTP. Anything else (probes,
        //   scanners, legacy CLI tools) is logged and dropped.
        if (!checked_first) {
            size_t nl = pending.find('\n');
            if (nl != std::string::npos) {
                checked_first = true;
                std::string first = pending.substr(0, nl);
                bool http = false;
                for (const char *v : {"POST ", "GET ", "PUT ", "DELETE ", "OPTIONS ", "HEAD "})
                    if (first.rfind(v, 0) == 0) {
                        http = true;
                        break;
                    }
                if (!http) {
                    LOG(fmt::format("[LMS] client {} speaks non-HTTP (Squeezer/cometd only) — "
                                    "closing: {}",
                                    c->client_id, first.substr(0, 60)));
                    break;
                }
            }
        }

        // Extract complete HTTP requests (headers + Content-Length body), keep-alive.
        for (;;) {
            size_t hdr_end = pending.find("\r\n\r\n");
            if (hdr_end == std::string::npos)
                break;
            size_t cl = 0;
            {
                std::string h = pending.substr(0, hdr_end);
                std::transform(h.begin(), h.end(), h.begin(),
                               [](unsigned char ch) { return (char)std::tolower(ch); });
                size_t p = h.find("content-length:");
                if (p != std::string::npos)
                    cl = (size_t)std::strtoull(h.c_str() + p + 15, nullptr, 10);
            }
            size_t total = hdr_end + 4 + cl;
            if (pending.size() < total)
                break; // body not fully buffered yet
            size_t sp1 = pending.find(' ');
            size_t sp2 = pending.find(' ', sp1 + 1);
            std::string method = pending.substr(0, sp1);
            std::string pathq = sp2 != std::string::npos ? pending.substr(sp1 + 1, sp2 - sp1 - 1)
                                                         : pending.substr(sp1 + 1);
            std::string path = pathq.substr(0, pathq.find('?'));
            std::string headers = pending.substr(0, hdr_end);
            std::string body = pending.substr(hdr_end + 4, cl);
            pending.erase(0, total);

            std::string resp;
            bool held = false;
            resp = handle_http(*c, method, path, headers, body, held);
            if (held) {
                // N10.4: this connection is now a held event stream — pump it until the
                //   client goes away, then fall through to the normal teardown.
                listen_loop(c);
                break;
            }
            {
                std::lock_guard<std::mutex> lk(c->wmtx);
                if (c->fd >= 0)
                    ::send(c->fd, resp.data(), resp.size(), MSG_NOSIGNAL);
            }
        }
        if (pending.size() > (1u << 20))
            pending.clear(); // runaway garbage guard
    }
    LOG(fmt::format("[LMS] client {} disconnected", c->client_id));
    if (c->fd >= 0) {
        ::close(c->fd);
        c->fd = -1;
    }
    c->done.store(true);
}

// ── HTTP/cometd (Bayeux JSON-RPC) — the remote apps' transport ──────────────────
//   POST /cometd with a JSON array of Bayeux messages; /meta/* keep the long-poll
//   session alive, /service/* carry JSON-RPC "slim.request" calls whose params are
//   LMS command arrays. Auth = HTTP Basic against lms_user/lms_pass.
//
//   N10.4 held streams: Squeeze Client's cometd layer is NOT a long-poll client — it
//   sends connectionType "streaming" and reads the /meta/connect response body as a
//   NEVER-ENDING event stream (readFromEventStream treats EOF as "connection failed"
//   and restarts the whole session). For those connects we answer with a chunked
//   response that stays open, ack the batch, and pump events from listen_loop().
//   One-shot publish replies are ALSO queued onto the stream (master Squeeze Client
//   waits for them there, not on the POST body).
std::string LmsServer::handle_http(Conn &c, const std::string &method, const std::string &path,
                                   const std::string &headers, const std::string &body,
                                   bool &held) {
    auto http_resp = [](int code, const char *status, const std::string &b,
                        const char *extra = "") {
        return fmt::format("HTTP/1.1 {} {}\r\nContent-Type: application/json;charset=UTF-8\r\n"
                           "Content-Length: {}\r\nConnection: keep-alive\r\n{}\r\n{}",
                           code, status, b.size(), extra, b);
    };
    auto header_val = [&](const std::string &key) -> std::string {
        // Search case-insensitively on a lowered COPY, but slice the ORIGINAL string —
        //   the transform is 1:1 so indices align; returning the lowered copy would turn
        //   "Authorization: Basic ..." into "basic ..." and break the scheme match below.
        std::string h = headers;
        std::transform(h.begin(), h.end(), h.begin(),
                       [](unsigned char ch) { return (char)std::tolower(ch); });
        size_t p = h.find(key + ":");
        if (p == std::string::npos)
            return "";
        p += key.size() + 1;
        while (p < h.size() && (h[p] == ' ' || h[p] == '\t'))
            p++;
        size_t e = headers.find("\r\n", p);
        return headers.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };

    // Basic-auth gate → lms_user/lms_pass (Squeezer sends credentials preemptively).
    if (auth_required_ && !c.authed.load()) {
        bool ok = false;
        std::string auth = header_val("authorization");
        if (auth.rfind("Basic ", 0) == 0) {
            std::string dec = b64decode(auth.substr(6));
            size_t colon = dec.find(':');
            if (colon != std::string::npos && ct_equal(dec.substr(0, colon), lms_user_) &&
                ct_equal(dec.substr(colon + 1), lms_pass_)) {
                ok = true;
                c.http_auth_user = dec.substr(0, colon);
            }
        }
        if (!ok) {
            LOG(fmt::format("[LMS-HTTP] 401 {} {} (bad or missing Basic credentials)", method,
                            path));
            return http_resp(401, "Unauthorized", "[]",
                             "WWW-Authenticate: Basic realm=\"panicast\"\r\n");
        }
        c.authed.store(true);
    }

    LOG(fmt::format("[LMS-HTTP] >> {} {} {}", method, path, body.substr(0, 300)));
    if (path.find("/cometd") == std::string::npos && path.find("/jsonrpc") == std::string::npos)
        return http_resp(404, "Not Found", "[]");

    bool is_cometd = path.find("/cometd") != std::string::npos;
    nlohmann::json out = nlohmann::json::array();
    try {
        nlohmann::json msgs = nlohmann::json::parse(body, nullptr, false);
        if (msgs.is_discarded()) // errors were silently dropped here for the whole N10.5
            //   era: a malformed TUI handover body produced an empty `[]` reply and no
            //   dispatch, with nothing in the log to point at it. Say it now.
            LOG(fmt::format("[LMS-HTTP] body parse FAILED ({} bytes) — request dropped: {}...",
                            body.size(), body.substr(0, 120)));
        if (msgs.is_object()) // /jsonrpc.js style: single object in, single object out
            msgs = nlohmann::json::array({msgs});
        else if (!msgs.is_array())
            msgs = nlohmann::json::array();

        // N10.4: Squeeze Client's streaming connect → HELD event stream (see the
        //   transport comment above). The batch is [connect] (release builds) or
        //   [connect, subscribe] (master bundles both) — ack every meta message in it
        //   as the first chunk, register the listener, and let listen_loop() own the
        //   connection from here.
        bool streaming_connect = false;
        for (const auto &m : msgs)
            if (m.value("channel", std::string()).rfind("/meta/connect", 0) == 0 &&
                m.value("connectionType", std::string()) == "streaming")
                streaming_connect = true;
        if (streaming_connect) {
            nlohmann::json acks = nlohmann::json::array();
            std::string cid;
            for (const auto &m : msgs) {
                std::string ch = m.value("channel", std::string());
                nlohmann::json id = m.contains("id") ? m["id"] : nlohmann::json(nullptr);
                cid = m.value("clientId", cid);
                if (ch.rfind("/meta/connect", 0) == 0) {
                    char ts[32];
                    std::time_t now = std::time(nullptr);
                    std::strftime(ts, sizeof(ts), "%FT%TZ", std::gmtime(&now));
                    nlohmann::json j;
                    j["channel"] = "/meta/connect";
                    j["successful"] = true;
                    j["clientId"] = m.value("clientId", std::string());
                    j["timestamp"] = ts;
                    nlohmann::json adv;
                    adv["reconnect"] = "retry";
                    adv["interval"] = 800;
                    adv["timeout"] = 25000;
                    j["advice"] = adv;
                    j["id"] = id;
                    acks.push_back(j);
                } else if (ch.rfind("/meta/subscribe", 0) == 0) {
                    nlohmann::json j;
                    j["channel"] = ch;
                    j["successful"] = true;
                    j["clientId"] = m.value("clientId", std::string());
                    j["subscription"] = m.value("subscription", std::string());
                    j["id"] = id;
                    acks.push_back(j);
                }
            }
            std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/json;charset=UTF-8\r\n"
                               "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n";
            {
                std::lock_guard<std::mutex> lk(c.wmtx);
                if (c.fd >= 0)
                    ::send(c.fd, head.data(), head.size(), MSG_NOSIGNAL);
            }
            // Bounded send timeout — a stalled phone must not wedge the pump.
            timeval tv{10, 0};
            ::setsockopt(c.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            c.listener = true;
            c.bayeux_cid = cid;
            {
                std::lock_guard<std::mutex> lk(listeners_mtx_);
                listeners_[cid] = &c; // re-handshake: the newest stream replaces
            }
            write_chunk(c, acks.dump());
            LOG(fmt::format("[LMS] client {} → held event stream (bayeux {})", c.client_id, cid));
            held = true;
            return "";
        }

        for (const auto &m : msgs) {
            std::string ch = m.value("channel", std::string());
            nlohmann::json id = m.contains("id") ? m["id"] : nlohmann::json(nullptr);
            std::string m_cid = m.value("clientId", std::string());

            if (ch.rfind("/meta/handshake", 0) == 0) {
                static std::atomic<uint64_t> next_cid{1};
                nlohmann::json j;
                j["channel"] = "/meta/handshake";
                j["successful"] = true;
                j["authSuccessful"] = true;
                j["version"] = "1.0";
                // ECHO the client's offered transports: Bayeux requires a non-empty
                //   intersection with the server's list — Squeezer offers ["streaming"],
                //   and answering ["long-polling"] made it abort right after the
                //   handshake (observed as handshake → /meta/disconnect). Our per-POST
                //   reply pattern satisfies any of these types.
                j["supportedConnectionTypes"] = (m.contains("supportedConnectionTypes") &&
                                                 m["supportedConnectionTypes"].is_array() &&
                                                 !m["supportedConnectionTypes"].empty())
                                                    ? m["supportedConnectionTypes"]
                                                    : nlohmann::json::array({"long-polling"});
                j["clientId"] = fmt::format("lc{:016x}", next_cid.fetch_add(1));
                j["id"] = id;
                out.push_back(j);
            } else if (ch.rfind("/meta/connect", 0) == 0) {
                // Immediate reply + advice.interval keeps the client from busy-spinning
                //   without holding a thread per long-poll (state arrives on requests).
                char ts[32];
                std::time_t now = std::time(nullptr);
                std::strftime(ts, sizeof(ts), "%FT%TZ", std::gmtime(&now));
                nlohmann::json j;
                j["channel"] = "/meta/connect";
                j["successful"] = true;
                j["clientId"] = m.value("clientId", std::string());
                j["timestamp"] = ts;
                nlohmann::json adv;
                adv["reconnect"] = "retry";
                adv["interval"] = 800;
                adv["timeout"] = 25000;
                j["advice"] = adv;
                j["id"] = id;
                out.push_back(j);
                // Bidirectional state sync: piggyback a playerstatus push on this connect
                //   reply whenever the playback state changed since the last push to this
                //   Bayeux clientId (first connect always pushes once). The client
                //   subscribed <cid>/slim/playerstatus/* and updates its UI from it.
                //   State is server-global — the app spreads requests over several
                //   sockets, so per-connection bookkeeping would miss pushes.
                if (any_subscribed_.load() && !m_cid.empty() && running_.load()) {
                    std::string st = status_data().dump();
                    std::lock_guard<std::mutex> lk(push_mtx_);
                    auto it = last_push_by_cid_.find(m_cid);
                    if (it == last_push_by_cid_.end() || it->second != st) {
                        nlohmann::json push;
                        push["channel"] =
                            fmt::format("/{}/slim/playerstatus/{}", m_cid, player_id());
                        push["data"] = status_data();
                        push["id"] = nullptr;
                        out.push_back(push);
                        last_push_by_cid_[m_cid] = st;
                    }
                }
            } else if (ch.rfind("/meta/subscribe", 0) == 0 ||
                       ch.rfind("/meta/unsubscribe", 0) == 0) {
                nlohmann::json j;
                j["channel"] = ch;
                j["successful"] = true;
                j["clientId"] = m.value("clientId", std::string());
                j["subscription"] = m.value("subscription", std::string());
                j["id"] = id;
                out.push_back(j);
            } else if (ch.rfind("/meta/disconnect", 0) == 0) {
                nlohmann::json j;
                j["channel"] = ch;
                j["successful"] = true;
                j["clientId"] = m.value("clientId", std::string());
                j["id"] = id;
                out.push_back(j);
            } else {
                // LMS cometd publish style (Squeezer): {"channel":"/slim/request",
                //   "data":{"request":[playerid,[cmd,...]],"response":"<cid>/slim/request","id":N}}
                //   Execute the command, then deliver the result hash on the channel named
                //   by data.response (exactly what the client subscribed to). Also accept
                //   the jsonrpc.js style {"method":"slim.request","params":[pid,[cmd]]}.
                const nlohmann::json *rpc =
                    m.contains("data") && m["data"].is_object() ? &m["data"] : &m;
                std::vector<std::string> cmd;
                bool have_cmd = false;
                if (rpc->contains("request") && (*rpc)["request"].is_array() &&
                    (*rpc)["request"].size() >= 2 && (*rpc)["request"][1].is_array()) {
                    for (const auto &e : (*rpc)["request"][1])
                        cmd.push_back(e.is_string() ? e.get<std::string>() : e.dump());
                    have_cmd = true;
                } else if (rpc->value("method", std::string()) == "slim.request" &&
                           (*rpc).contains("params") && (*rpc)["params"].is_array() &&
                           (*rpc)["params"].size() >= 2 && (*rpc)["params"][1].is_array()) {
                    for (const auto &e : (*rpc)["params"][1])
                        cmd.push_back(e.is_string() ? e.get<std::string>() : e.dump());
                    have_cmd = true;
                }
                nlohmann::json result = nlohmann::json::object();
                if (have_cmd)
                    result = json_slim_request(c, cmd);

                if (!is_cometd && rpc->value("method", std::string()) == "slim.request") {
                    // direct JSON-RPC → {"id":..,"method":..,"result":..}
                    nlohmann::json rpc_resp;
                    rpc_resp["id"] = rpc->contains("id") ? (*rpc)["id"] : nlohmann::json(nullptr);
                    rpc_resp["method"] = rpc->value("method", std::string());
                    rpc_resp["result"] = result;
                    out = rpc_resp;
                } else {
                    // cometd publish → TWO messages in the response array:
                    //   1) ECHO the published message back on its own channel (same id) —
                    //      real LMS does this, and Squeezer's PublishListener fires on that
                    //      echo to advance its serialized command queue
                    //      (MSG_PUBLISH_RESPONSE_RECIEVED). Without it the queue wedges
                    //      after the first publish and the app goes mute.
                    //   2) the actual result, delivered on the channel named by
                    //      data.response (what the client subscribed to).
                    out.push_back(m);
                    std::string resp_ch = rpc->value("response", std::string());
                    if (resp_ch.empty())
                        resp_ch = ch.empty() ? "/service/slim/request" : ch;
                    nlohmann::json msg;
                    msg["channel"] = resp_ch;
                    nlohmann::json data = result;
                    if (rpc->contains("id"))
                        data["id"] = (*rpc)["id"];
                    msg["data"] = data;
                    msg["id"] = id;
                    out.push_back(msg);
                    // N10.4: master Squeeze Client waits for one-shot replies ON THE
                    //   EVENT STREAM, not on the POST body — queue it for the
                    //   clientId's held listener (release builds read the POST body;
                    //   they ignore the duplicate stream copy).
                    if (!m_cid.empty())
                        stream_deliver(m_cid, msg);
                }
            }
        }
    } catch (const std::exception &e) {
        LOG(fmt::format("[LMS-HTTP] body parse error: {}", e.what()));
        return http_resp(400, "Bad Request", "[]");
    }

    std::string dump = out.dump();
    LOG(fmt::format("[LMS-HTTP] << {}", dump.substr(0, 300)));
    return http_resp(200, "OK", dump);
}

// ── N10.4: held event-stream plumbing ───────────────────────────────────────────

bool LmsServer::write_chunk(Conn &c, const std::string &payload) {
    if (c.fd < 0)
        return false;
    // ONE JSON array per chunk, always: the client's incremental parser accumulates
    //   raw bytes and re-parses the whole buffer at every "}]" — two concatenated
    //   arrays in a single read are invalid JSON and wedge that parser forever.
    std::string chunk = fmt::format("{:x}\r\n{}\r\n", payload.size(), payload);
    std::lock_guard<std::mutex> lk(c.wmtx);
    if (c.fd < 0)
        return false;
    return ::send(c.fd, chunk.data(), chunk.size(), MSG_NOSIGNAL) == (ssize_t)chunk.size();
}

void LmsServer::stream_deliver(const std::string &cid, const nlohmann::json &msg) {
    std::lock_guard<std::mutex> lk(listeners_mtx_);
    auto it = listeners_.find(cid);
    if (it == listeners_.end() || !it->second->listener)
        return;
    Conn *c = it->second;
    std::lock_guard<std::mutex> qk(c->queue_mtx);
    c->outq.push_back(msg);
}

void LmsServer::listen_loop(Conn *c) {
    std::string last_push;
    auto last_write = std::chrono::steady_clock::now();
    while (running_.load()) {
        // Socket liveness: the client closes the stream (or, unexpectedly, sends —
        //   the listen connection carries no further requests; drop any bytes).
        struct pollfd p{c->fd, POLLIN, 0};
        int pr = ::poll(&p, 1, 250);
        if (pr > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
            char tmp[512];
            ssize_t n = ::recv(c->fd, tmp, sizeof(tmp), MSG_DONTWAIT);
            if (n <= 0)
                break; // stream closed by the client
        }
        if (!running_.load())
            break;
        // Queued one-shot replies (+ anything else routed here) → one array chunk.
        nlohmann::json batch = nlohmann::json::array();
        {
            std::lock_guard<std::mutex> qk(c->queue_mtx);
            for (auto &m : c->outq)
                batch.push_back(std::move(m));
            c->outq.clear();
        }
        // playerstatus push on change; unconditional re-push every 20s doubles as the
        //   read-timeout heartbeat (the client's OkHttp read timeout is subscription
        //   interval + 5s ≈ 65s; a silent stream is a dead stream to it).
        auto now = std::chrono::steady_clock::now();
        bool heartbeat =
            std::chrono::duration_cast<std::chrono::seconds>(now - last_write).count() >= 20;
        // serverstatus subscription feed (Squeeze Client subscribes
        //   "serverstatus subscribe:60" and treats a silent channel as a dead
        //   connection — observed as a full reconnect ~70s in). Real LMS pushes this
        //   per subscription interval; every 20s heartbeat slot is well within it.
        if (heartbeat && running_.load()) {
            nlohmann::json push;
            push["channel"] = fmt::format("/{}/slim/serverstatus", c->bayeux_cid);
            push["data"] = serverstatus_data({});
            push["id"] = nullptr;
            batch.push_back(std::move(push));
        }
        if (any_subscribed_.load() && running_.load()) {
            nlohmann::json d = status_data();
            std::string st = d.dump();
            if (st != last_push || heartbeat) {
                nlohmann::json push;
                push["channel"] =
                    fmt::format("/{}/slim/playerstatus/{}", c->bayeux_cid, player_id());
                push["data"] = std::move(d);
                push["id"] = nullptr;
                batch.push_back(std::move(push));
                last_push = std::move(st);
            }
        }
        if (!batch.empty()) {
            if (!write_chunk(*c, batch.dump()))
                break;
            last_write = std::chrono::steady_clock::now();
        }
    }
    LOG(fmt::format("[LMS] client {} event stream ended", c->client_id));
    {
        std::lock_guard<std::mutex> lk(listeners_mtx_);
        auto it = listeners_.find(c->bayeux_cid);
        if (it != listeners_.end() && it->second == c)
            listeners_.erase(it);
    }
}

// Shared status builder: the object returned for slim "status" requests AND pushed on
//   /meta/connect replies when the state changed (bidirectional sync).
//
//   JSON TYPES MATTER (Squeeze Client, de.maniac103.squeezeclient): it decodes with
//   kotlinx.serialization in STRICT mode (coerceInputValues + ignoreUnknownKeys, no
//   isLenient) — PlayerStatusResponse.count is Int, time/duration are Float,
//   player_connected/power use a decodeInt() serializer, playlist_timestamp decodes a
//   DOUBLE. A quoted "4" in any of those makes the WHOLE response fail to decode and
//   the app renders nothing. Real LMS numeric-coerces exactly these fields; so do we:
//   numbers where the parsers declare numbers, strings only for the enum/string
//   fields ("mode", "playlist shuffle/repeat", names, ids). Old-Squeezer-style Java
//   parsers (Util.getInt/getDouble) accept both, so nothing regresses there.
//
//   item_loop entries carry track/artist/album (Squeeze Client's
//   PlayerStatusResponse.Item DROPS the item when any of the three is null — the
//   now-playing bar and every playlist row vanished without them) plus text/actions
//   for the jive browse rendering of the same response.
nlohmann::json LmsServer::status_data(int start, int window) {
    nlohmann::json r;
    if (!control_)
        return r;
    auto s = control_->snapshot_state();
    const char *mode = !s.has_media ? "stop" : (s.paused ? "pause" : "play");
    int idx = std::max(0, s.current_index);
    r["player_name"] = player_name();
    r["player_connected"] = 1;
    r["playerid"] = player_id();
    r["power"] = 1;
    r["mode"] = mode;
    r["playlist_tracks"] = (int)s.playlist.size();
    r["playlist_cur_index"] = idx;
    // Enum strings — the Squeezer family reads these as strings ("0"/"1"/"2"). Mapped
    //   from panicast's tri-state PlayMode: repeat = single-track loop = LMS "repeat
    //   song" (1); shuffle = random next (1); cycle (the default) = both off.
    r["playlist repeat"] = s.play_mode == "repeat" ? "1" : "0";
    r["playlist shuffle"] = s.play_mode == "shuffle" ? "1" : "0";
    // Content-keyed epoch timestamp (DOUBLE on the wire). Squeeze Client's
    //   PlaylistFragment re-fetches the list when playlist_timestamp CHANGES; a
    //   constant hides queue edits. Cache: same content → same timestamp (change
    //   detection by equality stays exact even when edits ping-pong).
    {
        uint64_t h = 1469598103934665603ull; // FNV-1a over size + titles + durations
        auto mix = [&](const std::string &v) {
            for (unsigned char c : v) {
                h ^= c;
                h *= 1099511628211ull;
            }
        };
        mix(std::to_string(s.playlist.size()));
        for (const auto &it : s.playlist) {
            mix(it.title);
            mix(std::to_string(it.duration));
        }
        static std::mutex ts_mtx;
        static uint64_t last_hash = 0;
        static double last_ts = 0;
        std::lock_guard<std::mutex> lk(ts_mtx);
        if (h != last_hash || last_ts == 0) {
            last_hash = h;
            last_ts = (double)std::time(nullptr);
        }
        r["playlist_timestamp"] = last_ts;
    }
    r["playlist_name"] = "";
    r["will_sleep_in"] = s.sleep_remaining > 0 ? s.sleep_remaining : 0;
    r["sleep"] = 0;
    r["remote"] = 1;
    r["sync_master"] = "";
    r["sync_slaves"] = "";
    r["song"] = idx;
    r["seq_no"] = 0;
    r["rate"] = 1;
    r["time"] = s.elapsed;
    r["duration"] = s.duration;
    r["canseek"] = 1;
    r["digital_volume_control"] = 1;
    // LMS mute convention: a NEGATIVE mixer volume means muted (Squeeze Client reads
    //   vol < 0 as muted, |vol| as the level). Encoded as -(level)-1 so level 0 stays
    //   distinguishable from "not muted at 0".
    r["mixer volume"] = muted_.load() ? -(s.volume) - 1 : s.volume;
    r["count"] = (int)s.playlist.size();
    r["browse_sig"] = s.browse_sig; // N10.6: client-TUI refetch trigger (change = list moved)
    // NOTE: no "offset" — PlayerStatusResponse declares it String? while the browse
    //   decoders declare Int; omitting satisfies both (defaults cover it).
    // META-1: radio streams override the display title with the ICY song
    //   ("Artist - Title" → split on the first " - "). Node title stays the album-ish
    //   station identity; the now-playing rows show what is actually on air.
    std::string disp_title = s.title, disp_artist = s.artist;
    if (!s.icy_title.empty()) {
        size_t sp = s.icy_title.find(" - ");
        if (sp != std::string::npos && sp > 0) {
            disp_artist = s.icy_title.substr(0, sp);
            disp_title = s.icy_title.substr(sp + 3);
        } else {
            disp_title = s.icy_title;
        }
    }
    if (s.has_media) {
        r["current_title"] = disp_title;
        // TOP-LEVEL metadata — real LMS sends artist/album alongside title.
        //   The Android notification (MediaService → MediaMetadata) reads these;
        //   missing → "Unknown artist/album" in the mini-player bar.
        r["artist"] = disp_artist.empty() ? "panicast" : disp_artist;
        r["album"] = s.album.empty() ? "panicast" : s.album;
        r["title"] = disp_title;
        if (!s.art_url.empty())
            r["art_url"] = s.art_url;
        // item_loop[0] = current song (parsePlayerStatus / asModelStatus build the
        //   now-playing item from it).
        nlohmann::json item;
        item["id"] = idx;
        item["track"] = disp_title.empty() ? "Live stream" : disp_title;
        item["title"] = disp_title;
        item["artist"] = disp_artist.empty() ? "panicast" : disp_artist;
        item["album"] = s.album.empty() ? "panicast" : s.album;
        item["duration"] = s.duration;
        if (!s.art_url.empty()) {
            item["icon"] = s.art_url;
            item["artwork_url"] = s.art_url;
        }
        if (start < 0)
            r["item_loop"] = nlohmann::json::array({item}); // current-song shape
    }
    if (start >= 0) {
        // Playlist-page shape (Squeeze Client's PlaylistFragment pages through
        //   PlayerStatusRequest; the jive browse view renders the same reply). One
        //   record per queue entry: track/artist/album for the playlist decoders,
        //   text for the browse renderer, and a per-row go action ("play this row
        //   now") for the browse click path.
        nlohmann::json loop = nlohmann::json::array();
        size_t from = (size_t)std::max(0, start);
        size_t to = s.playlist.size();
        if (window > 0)
            to = std::min(to, from + (size_t)window);
        for (size_t i = from; i < to; ++i) {
            nlohmann::json it;
            it["id"] = (int)i;
            it["track"] = s.playlist[i].title;
            it["text"] = s.playlist[i].title;
            // Per-row values from the queue item (META-1 carried artist/album through
            //   PlaylistItem); fall back to the current track's, then "panicast" — a
            //   mixed queue (jumps between feeds) keeps each row's real metadata.
            it["artist"] = !s.playlist[i].artist.empty()
                               ? s.playlist[i].artist
                               : (s.artist.empty() ? "panicast" : s.artist);
            it["album"] = !s.playlist[i].album.empty() ? s.playlist[i].album
                                                       : (s.album.empty() ? "panicast" : s.album);
            it["duration"] = s.playlist[i].duration;
            if (!s.playlist[i].art_url.empty())
                it["icon"] = s.playlist[i].art_url;
            else if (!s.art_url.empty())
                it["icon"] = s.art_url; // fallback: current track's artwork
            nlohmann::json go;
            go["cmd"] = nlohmann::json::array({"playlist", "index", std::to_string(i)});
            it["actions"] = nlohmann::json({{"go", go}});
            loop.push_back(it);
        }
        r["item_loop"] = loop;
    }
    // The queue in LMS's flat spelling — the MINI-PLAYER BAR reads from
    //   playlist_loop[0] (title / artist / album / artwork_url), NOT from
    //   item_loop. Missing these fields is exactly why the bar shows
    //   "Unknown track / Unknown artist / Unknown album".
    nlohmann::json loop = nlohmann::json::array();
    for (size_t i = 0; i < s.playlist.size() && i < 200; ++i) {
        nlohmann::json it;
        it["playlist index"] = std::to_string(i);
        it["id"] = std::to_string(i);
        it["title"] = s.playlist[i].title;
        it["duration"] = std::to_string(s.playlist[i].duration);
        it["artist"] = !s.playlist[i].artist.empty() ? s.playlist[i].artist
                                                     : (s.artist.empty() ? "panicast" : s.artist);
        it["album"] = !s.playlist[i].album.empty() ? s.playlist[i].album
                                                   : (s.album.empty() ? "panicast" : s.album);
        if (!s.playlist[i].art_url.empty())
            it["artwork_url"] = s.playlist[i].art_url;
        else if (!s.art_url.empty())
            it["artwork_url"] = s.art_url;
        loop.push_back(it);
    }
    r["playlist_loop"] = loop;
    return r;
}
// players/serverstatus payload (players_loop + prefs echo for requested keys). Shared
//   by the command handler and the listen pump's periodic serverstatus push (Squeeze
//   Client subscribes "serverstatus subscribe:60" and drops the connection when
//   nothing arrives on that channel — cmd = {} omits the prefs echo).
nlohmann::json LmsServer::serverstatus_data(const std::vector<std::string> &cmd) {
    nlohmann::json p;
    p["playerindex"] = "0";
    p["playerid"] = player_id();
    p["name"] = player_name();
    p["model"] = "squeezelite";
    p["modelname"] = "SqueezeLite";
    p["isplayer"] = "1";
    p["connected"] = "1";
    p["power"] = "1";
    p["displaytype"] = "graphic-280x16";
    p["seq_no"] = "0";
    nlohmann::json r;
    r["count"] = "1";
    r["player_count"] = "1";
    r["version"] = "8.4.0";
    r["sn"] = "0";
    // Squeezer names the prefs it wants ("prefs:k1,k2..." / "playerprefs:k1,k2...") and
    //   stalls initializing until they come back (observed: it re-sends serverstatus
    //   forever when they're absent). Echo sensible defaults for every requested key.
    auto add_prefs = [&](const std::string &prefix, nlohmann::json &target) {
        static const std::map<std::string, nlohmann::json> defaults = {
            {"mediadirs", nlohmann::json::array()}, // ARRAY — a String "" crashes the
                                                    // app's (Object[]) cast in Util
            {"defeatDestructiveTouchToPlay", "0"},
            {"defeatDestru", "0"}, // truncated form seen on the wire
            {"digitalVolumeControl", "1"},
            {"alarmDefaultVolume", "40"},
            {"alarmfadeseconds", "0"},
            {"alarmSnoozeSeconds", "600"},
            {"alarmTimeoutSeconds", "3600"},
            {"alarmsEnabled", "0"},
            {"playtrackalbum", "0"},
            {"syncVolume", "1"},
            {"syncPower", "1"},
        };
        for (const auto &a : cmd) {
            if (a.rfind(prefix, 0) != 0)
                continue;
            std::string list = a.substr(prefix.size());
            std::string cur;
            auto emit_key = [&](const std::string &key) {
                auto it = defaults.find(key);
                target[key] = it != defaults.end() ? it->second : nlohmann::json("0");
            };
            for (char c : list) {
                if (c == ',') {
                    emit_key(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            emit_key(cur);
        }
    };
    add_prefs("prefs:", r);
    add_prefs("playerprefs:", p); // FLAT in the player record — Player.java reads
                                  // record.get(prefName), not a nested object
    p["ip"] = "127.0.0.1";        // Player.java reads it (cosmetic, but it parses it)
    r["players_loop"] = nlohmann::json::array({p}); // AFTER all p mutations (copies!)
    return r;
}

// JSON-RPC command mapping — LMS-JSON shaped: stringly-typed values, players_loop array
//   for player listings, flat status object. The surface mirrors what Squeezer actually
//   sends (verified against its source: SqueezeService.java builds these cmd arrays).
nlohmann::json LmsServer::json_slim_request(Conn &c, const std::vector<std::string> &cmd) {
    if (cmd.empty())
        return nlohmann::json::object();
    const std::string &k = cmd[0];
    auto push = [&](const char *action) {
        if (bus_)
            bus_->push({action, {}, c.client_id});
    };
    auto push_arg = [&](const char *action, const std::string &arg) {
        if (bus_)
            bus_->push({action, {arg}, c.client_id});
    };
    auto push_args = [&](const char *action, const std::string &a1, const std::string &a2) {
        if (bus_)
            bus_->push({action, {a1, a2}, c.client_id});
    };
    auto empty_page = [&]() { // browse-shaped "no content": count/offset NUMBERS + empty item_loop
        nlohmann::json r;
        r["count"] = 0;
        r["offset"] = 0;
        r["item_loop"] = nlohmann::json::array();
        return r;
    };
    // Trailing int arg helper ("playlist index 5 2" — the trailing 2 is Squeezer's
    //   fadeInSecs; LMS tolerates and ignores unknown trailing params, we do too).
    auto int_arg = [&](size_t i) -> int { return i < cmd.size() ? std::atoi(cmd[i].c_str()) : 0; };

    if (k == "players" || k == "serverstatus") {
        return serverstatus_data(cmd);
    }
    if (k == "status") {
        any_subscribed_.store(true); // any status interest enables connect-time pushes
        // Window args: "status - 1 ..." = current-song probe / subscription push shape;
        //   "status <start> <window> ..." = a playlist page — Squeeze Client's
        //   PlaylistFragment (PlayerStatusRequest + PlayerStatusResponse → item_loop
        //   rows) and the jive browse view of the same reply.
        int start = -1;
        if (cmd.size() > 1 && cmd[1] != "-")
            start = std::atoi(cmd[1].c_str());
        int window = cmd.size() > 2 ? std::atoi(cmd[2].c_str()) : 0;
        return status_data(start, window);
    }
    if (k == "songinfo") { // current-track details for the now-playing screen
        nlohmann::json r;
        r["count"] = 0;
        r["songinfo_loop"] = nlohmann::json::array();
        if (control_) {
            auto s = control_->snapshot_state();
            if (s.has_media) {
                nlohmann::json it;
                it["id"] = std::to_string(std::max(0, s.current_index));
                it["title"] = s.title;
                it["duration"] = s.duration;
                if (!s.art_url.empty())
                    it["art_url"] = s.art_url;
                r["songinfo_loop"] = nlohmann::json::array({it});
                r["count"] = 1;
            }
        }
        return r;
    }
    if (k == "login" || k == "listen")
        return nlohmann::json::object();
    if (k == "version") {
        nlohmann::json r;
        r["_version"] = "8.4.0";
        return r;
    }
    if (k == "play") {
        // Remote-control intuition: resume if something is loaded (paused or playing);
        //   only fall through to "play the tree cursor node" when nothing is loaded —
        //   the bare "play" action is Enter-on-cursor, which surprised phone users.
        //   Squeezer sends "play" (from stop) or "play <fade>" (togglePausePlay).
        if (control_) {
            auto st = control_->snapshot_state();
            if (st.has_media) {
                if (st.paused)
                    push("resume");
                return nlohmann::json::object();
            }
        }
        push("play");
    } else if (k == "pause") {
        // Squeezer always sends the explicit form ("pause 1" / "pause 0 [fade]") —
        //   it refuses ambiguous toggles (see SqueezeService.togglePausePlay).
        std::string want = cmd.size() > 1 ? cmd[1] : "";
        if (want == "1")
            push("pause");
        else if (want == "0")
            push("resume");
        else
            push("play_pause");
    } else if (k == "stop") {
        push("stop");
    } else if (k == "power") {
        // Squeezer's power button toggles ("power" bare from the now-playing screen,
        //   explicit 0/1 from player settings). We mirror it onto pause state — the
        //   queue survives a power-off, like a stopped Squeezebox.
        std::string want = cmd.size() > 1 ? cmd[1] : "";
        if (want == "0")
            push("pause");
        else if (want == "1")
            push("resume");
        else
            push("play_pause");
    } else if (k == "next") {
        push("next");
    } else if (k == "prev") {
        push("previous");
    } else if (k == "button" && cmd.size() > 1) {
        // IR-button spellings (Squeeze Client's prev/next/shuffle/repeat buttons).
        //   LMS cycles 2-3 modes per press; our PlayMode is a tri-state, so the
        //   toggles flip ON from any other state and back OFF when already active.
        if (cmd[1] == "jump_fwd") {
            push("next");
        } else if (cmd[1] == "jump_rew") {
            push("previous");
        } else if (cmd[1] == "shuffle") {
            std::string cur = control_ ? control_->snapshot_state().play_mode : "";
            push(cur == "shuffle" ? "cycle" : "shuffle");
        } else if (cmd[1] == "repeat") {
            std::string cur = control_ ? control_->snapshot_state().play_mode : "";
            push(cur == "repeat" ? "cycle" : "repeat");
        } else {
            LOG(fmt::format("[LMS-JSON] unhandled button: {}", cmd[1]));
        }
    } else if (k == "playlist" && cmd.size() > 1) {
        const std::string &sub = cmd[1];
        if (sub == "index" && cmd.size() > 2) {
            if (cmd[2] == "+1")
                push("next");
            else if (cmd[2] == "-1")
                push("previous");
            else {
                // "playlist index <N> [fade]" — tapping row N in the playlist view.
                //   THE cursor move: without it Squeezer taps do nothing.
                int n = std::atoi(cmd[2].c_str());
                if (n >= 0)
                    push_arg("jump", std::to_string(n));
            }
        } else if (sub == "jump") { // alt spelling, same semantics
            int n = int_arg(2);
            if (n >= 0)
                push_arg("jump", std::to_string(n));
        } else if (sub == "next") {
            push("next");
        } else if (sub == "prev") {
            push("previous");
        } else if (sub == "delete" && cmd.size() > 2) { // swipe-to-remove on a row
            push_arg("playlist_remove", cmd[2]);
        } else if (sub == "move" && cmd.size() > 3) { // drag-to-reorder
            push_args("playlist_move", cmd[2], cmd[3]);
        } else if (sub == "clear") {
            push("queue_clear");
        } else if (sub == "save" && cmd.size() > 2) {
            LOG(fmt::format("[LMS-JSON] playlist save '{}' — not supported (queue is "
                            "derived from the tree, not persisted)",
                            cmd[2]));
        } else {
            LOG(fmt::format("[LMS-JSON] unhandled playlist subcommand: {}",
                            fmt::join(cmd.begin(), cmd.end(), " ")));
        }
    } else if (k == "mixer" && cmd.size() > 1) {
        if (cmd[1] == "volume") {
            if (cmd.size() > 2 && cmd[2] == "?") {
                // Query form: Squeezer's mixer response handler reads "_volume" and
                //   RE-SENDS the query when it's absent — never leave it out.
                nlohmann::json r;
                r["_volume"] = std::to_string(control_ ? control_->snapshot_state().volume : 0);
                return r;
            }
            if (cmd.size() > 2 && cmd[2] != "?") {
                const std::string &v = cmd[2];
                if (!v.empty() && (v[0] == '+' || v[0] == '-'))
                    push_arg("volume_rel", v); // relative step ("+5"/"-5")
                else
                    push_arg("volume", v);
            }
        } else if (cmd[1] == "muting" && cmd.size() > 2) {
            // Squeeze Client's mute button ("mixer muting 1/0"). mpv holds the audio
            //   state; we track the flag only to re-encode it as a negative mixer
            //   volume in status (LMS convention).
            bool on = cmd[2] == "toggle" ? !muted_.load() : cmd[2] == "1";
            muted_.store(on);
            if (bus_) // "set mute yes|no" via the mpv passthrough action
                bus_->push({"mpv", {"set", "mute", on ? "yes" : "no"}, c.client_id});
        }
    } else if (k == "time") {
        if (cmd.size() > 1 && cmd[1] != "?")
            push_arg("seekto", cmd[1]);
    } else if (k == "sleep" && cmd.size() > 1 && cmd[1] != "?") {
        // LMS sleep takes SECONDS; the app's sleep dialog sends minutes-as-seconds —
        //   pass through (SleepTimer accepts "90" = 90 minutes, so scale: LMS n sec →
        //   the same duration expressed as seconds for our timer).
        int secs = std::atoi(cmd[1].c_str());
        if (secs > 0)
            push_arg("sleep", std::to_string(secs) + "s");
        else
            push("sleep_cancel");
    } else if (k == "playerpref" || k == "pref" || k == "setting") {
        // Squeezer writes player prefs (e.g. alarm volume) — acknowledge, don't apply.
        return nlohmann::json::object();
    } else if (k == "displaystatus" || k == "menustatus") {
        // Display/menu push subscriptions (Squeeze Client subscribes both at connect).
        //   Nothing to push — acknowledge the subscribe so the app's serialized
        //   command queue keeps flowing.
        return nlohmann::json::object();
    } else if (k == "panicast" && cmd.size() > 2 && (cmd[1] == "playmode" || cmd[1] == "speed")) {
        // META-5: client-TUI convenience verbs — map straight onto existing actions.
        const std::string &v = cmd[2];
        if (cmd[1] == "playmode") {
            if (v == "repeat")
                push("repeat");
            else if (v == "shuffle")
                push("shuffle");
            else
                push("cycle");
        } else {
            if (v == "up")
                push("speed_up");
            else if (v == "down")
                push("speed_down");
            else
                push("speed_reset");
        }
        return nlohmann::json::object();
    } else if (k == "panicast" && cmd.size() > 2 && cmd[1] == "mpv") {
        // META-5: client-TUI ':' box — raw mpv command passthrough (args joined).
        if (bus_ && cmd.size() > 2)
            bus_->push({"mpv", std::vector<std::string>(cmd.begin() + 2, cmd.end()), c.client_id});
        return nlohmann::json::object();
    } else if (k == "panicast" && cmd.size() > 2 && cmd[1] == "search") {
        // META-4: search submitted from the phone's input box. Runs the current
        //   mode's query search on the UI thread, waits (bounded) for the mirrored
        //   list to update, then answers with the refreshed page.
        if (control_) {
            std::string before_sig = control_->snapshot_state().browse_sig;
            push_arg("search_query", cmd[2]);
            std::string prev = before_sig;
            int still = 0;
            for (int i = 0; i < 200 && running_.load(); ++i) { // ≤10s: network search
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                std::string cur = control_->snapshot_state().browse_sig;
                if (cur != before_sig) {
                    still = cur == prev ? still + 1 : 0;
                    prev = cur;
                    if (still >= 6)
                        break;
                } else {
                    prev = cur;
                }
            }
        }
        // fall through: same page builder as `browse` below (fresh results)
        return json_slim_request(c, {"panicast", "browse", "root", "0", "200"});
    } else if (k == "panicast" && cmd.size() > 2 && cmd[1] == "login") {
        // META-6: start a mode login (youtube device-flow / bilibili QR / tiktok
        //   reserved). Reply = a small page: tappable authorization link (weblink —
        //   the browser opens it; the phone's Bilibili/Google app confirms), the
        //   user code where applicable, and a status line. Completion lands on the
        //   pool; the account tree refreshes and the list follows via browse_sig.
        nlohmann::json r;
        nlohmann::json loop = nlohmann::json::array();
        std::string mode = cmd[2];
        // The login runs on the UI thread via the bus; but we need the URL NOW —
        //   request_device_code / request_qrcode are network calls, so they cannot
        //   run inline. Perform them here (this IS a server thread, not the UI
        //   thread) by calling the shared starters directly.
        std::string url, code, err;
        // The starters need App context only for the pool submit; the LMS server
        //   reaches App via control_, so route through a dedicated action that
        //   returns the URL synchronously is not possible — instead run the two
        //   pure-API starters here and push only the FINISH via the bus.
        if (mode == "youtube") {
            auto dc = GoogleOAuth::request_device_code();
            if (dc.ok) {
                url = dc.verification_url;
                code = dc.user_code;
                // Finish on the pool via the bus (App handles poll + account add).
                if (bus_)
                    bus_->push({"_remote_login_youtube",
                                {dc.device_code, std::to_string(dc.interval)},
                                c.client_id});
            } else {
                err = dc.error.empty() ? "network error" : dc.error;
            }
        } else if (mode == "bilibili") {
            auto qr = BilibiliAPI::request_qrcode();
            if (qr.ok) {
                url = qr.url;
                if (bus_)
                    bus_->push({"_remote_login_bilibili", {qr.qrcode_key}, c.client_id});
            } else {
                err = qr.error.empty() ? "network error" : qr.error;
            }
        } else if (mode == "tiktok") {
            // META-6c: Douyin QR login (sso.douyin.com, no X-Bogus). The QR
            //   content is the scannable URL — served as the weblink so the phone
            //   browser opens it; the app confirms. Cookie jar writes sessionid.
            DouyinApi dy;
            auto qr = dy.request_qrcode();
            if (qr.ok && !qr.qr_content.empty()) {
                url = qr.qr_content;
                if (bus_)
                    bus_->push({"_remote_login_tiktok", {qr.token}, c.client_id});
            } else {
                err = qr.err.empty() ? "QR request failed (CN network needed?)" : qr.err;
            }
        } else {
            err = "unknown mode";
        }
        if (!url.empty()) {
            // META-7d: auto-fill for Google (user_code param)
            if (mode == "youtube" && !code.empty()) {
                std::string sep = url.find('?') != std::string::npos ? "&" : "?";
                url += sep + "user_code=" + code;
            }
            nlohmann::json link;
            link["text"] = "🔓 打开授权页完成登录 / open to authorize";
            link["weblink"] = url;
            loop.push_back(link);
            if (!code.empty()) {
                nlohmann::json cr;
                cr["text"] = "code: " + code;
                loop.push_back(cr);
            }
            nlohmann::json st;
            st["text"] = "等待授权…完成后账号树自动出现 / waiting for authorization…";
            loop.push_back(st);
        } else {
            nlohmann::json it;
            it["text"] = "登录失败: " + err;
            loop.push_back(it);
        }
        r["count"] = (int)loop.size();
        r["offset"] = 0;
        r["item_loop"] = loop;
        return r;
    } else if (k == "panicast" && cmd.size() > 2 && cmd[1] == "context") {
        // META-7g: context menu → Squeeze Client's ContextMenuBottomSheetFragment
        nlohmann::json r;
        nlohmann::json loop = nlohmann::json::array();
        std::string mode_s = control_ ? control_->snapshot_state().mode : "";
        int row_i = std::atoi(cmd[2].c_str());
        auto snap = control_ ? control_->snapshot_state() : RemoteStateSnapshot{};
        bool is_search = row_i >= 0 && row_i < (int)snap.browse.size() &&
                         snap.browse[row_i].title.rfind("🔍", 0) == 0;
        if (mode_s == "FAVOURITE") {
            nlohmann::json it;
            it["text"] = "✕ Remove from favourites";
            nlohmann::json go;
            go["cmd"] = nlohmann::json::array({"panicast", "unfav", cmd[2]});
            it["actions"] = nlohmann::json({{"go", go}});
            loop.push_back(it);
        } else if (is_search && mode_s == "ONLINE") {
            nlohmann::json it;
            it["text"] = "✕ Delete search record";
            nlohmann::json go;
            go["cmd"] = nlohmann::json::array({"panicast", "unfav", cmd[2]});
            it["actions"] = nlohmann::json({{"go", go}});
            loop.push_back(it);
        } else {
            nlohmann::json it;
            it["text"] = "★ Add to favourites";
            nlohmann::json go;
            go["cmd"] = nlohmann::json::array({"panicast", "fav", cmd[2]});
            it["actions"] = nlohmann::json({{"go", go}});
            loop.push_back(it);
        }
        r["count"] = (int)loop.size();
        r["offset"] = 0;
        r["item_loop"] = loop;
        return r;
    } else if (k == "panicast" && cmd.size() > 2 && (cmd[1] == "fav" || cmd[1] == "unfav")) {
        // META-7f: favourites with user feedback. The reply is a small page so the
        //   app shows a toast-like confirmation ("★ Added to favourites" / "Removed
        //   from favourites") instead of silently doing nothing.
        std::string action, index;
        if (cmd[2] == "current") {
            action = "fav_current";
            index = "current";
        } else if (cmd[1] == "fav") {
            action = "fav_row";
            index = cmd[2];
        } else {
            action = "unfav_row";
            index = cmd[2];
        }
        push_arg(action.c_str(), index);

        nlohmann::json r;
        nlohmann::json loop = nlohmann::json::array();
        nlohmann::json msg;
        msg["text"] = (cmd[1] == "fav") ? "★ Added to favourites" : "✓ Removed from favourites";
        // show the target name if we can resolve it from the browse mirror
        if (control_ && cmd[2] != "current") {
            auto snap = control_->snapshot_state();
            int row = std::atoi(cmd[2].c_str());
            if (row >= 0 && row < (int)snap.browse.size())
                msg["text"] =
                    std::string(cmd[1] == "fav" ? "★ ★ " : "✓ Removed: ") + snap.browse[row].title;
        }
        loop.push_back(msg);
        r["count"] = 1;
        r["offset"] = 0;
        r["item_loop"] = loop;
        return r;
    } else if (k == "panicast" && cmd.size() > 2 && cmd[1] == "handover") {
        // N10.5 zero-drop takeover: the TUI asks us to hand our listener + live phone
        //   connections to it (fds duplicated via SCM_RIGHTS; the phone never drops).
        //   The transfer runs on a detached thread so this reply goes out first; the
        //   TUI then stops us with systemctl for the usual clean-exit state flush.
        std::string path = cmd[2];
        std::thread([this, path]() { send_handover_fds(path); }).detach();
        nlohmann::json r;
        r["status"] = "ok";
        return r;
    } else if (k == "panicast" && cmd.size() > 1 && (cmd[1] == "browse" || cmd[1] == "mode")) {
        // ── Remote library browse (Squeeze Client's main screen) ──────────────────
        //   The remote mirrors the TUI's CURRENT mode list: rows are display_list
        //   entries, tapping a row = cursor+Enter (nav_activate) — branch rows
        //   descend, leaf rows play. "back" pops one level. After a navigation the
        //   daemon waits (bounded) for the flattened list to change so the reply
        //   contains the destination, not the origin.
        std::string where = cmd.size() > 2 ? cmd[2] : "root";
        int start = cmd.size() > 3 ? std::atoi(cmd[3].c_str()) : 0;
        int window = cmd.size() > 4 ? std::atoi(cmd[4].c_str()) : 0;
        const bool is_mode_switch = cmd[1] == "mode";
        if (is_mode_switch) {
            // `panicast mode <NAME>` — tap on a home-menu mode entry: switch the App
            //   mode (same action the PRP remote uses), wait for the mirrored list to
            //   follow (the mode is part of browse_sig, so the switch is visible to
            //   the settle logic), then fall through to the page builder below.
            static const std::vector<std::string> modes = {
                "RADIO",   "PODCAST",  "FAVOURITE", "HISTORY", "ONLINE",
                "ACCOUNT", "BILIBILI", "TIKTOK",    "IPTV",
            };
            bool known = std::find(modes.begin(), modes.end(), where) != modes.end();
            if (!known)
                return empty_page();
            if (control_) {
                std::string before_sig = control_->snapshot_state().browse_sig;
                std::string cur_mode = control_->snapshot_state().mode;
                if (cur_mode != where) {
                    push_arg("mode", where);
                    std::string prev = before_sig;
                    int still = 0;
                    for (int i = 0; i < 60 && running_.load(); ++i) { // ≤3s
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        std::string cur = control_->snapshot_state().browse_sig;
                        if (cur != before_sig) {
                            still = cur == prev ? still + 1 : 0;
                            prev = cur;
                            if (still >= 6)
                                break;
                        } else {
                            prev = cur;
                        }
                    }
                }
            }
            // The page builder below needs to see the "root" of the new mode.
            start = 0;
        }
        if (control_ && !is_mode_switch) {
            std::string before_sig = control_->snapshot_state().browse_sig;
            bool wait_change = false;
            int wait_budget = 100; // 50ms units: 5s for async branch loads
            if (where == "back") {
                push("nav_back");
                wait_change = true; // the pop is in-memory; the next frame's flatten
                                    //   reflects it — without waiting we'd reply with
                                    //   the PRE-back list
                wait_budget = 20;   // 1s: a back settles within a couple of frames
            } else if (where != "root") {
                int idx = std::atoi(where.c_str());
                auto before = control_->snapshot_state();
                bool is_branch =
                    idx >= 0 && idx < (int)before.browse.size() && before.browse[idx].is_branch;
                push_arg("nav_activate", where);
                wait_change = is_branch; // leaf rows PLAY (no list change to wait for)
            }
            if (wait_change) {
                // Feed children load asynchronously on the UI thread — wait for
                //   the signature to change, then hold until it stops moving
                //   (300ms stillness) or the ~5s budget runs out.
                std::string prev = before_sig;
                int still = 0;
                for (int i = 0; i < wait_budget && running_.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    std::string cur = control_->snapshot_state().browse_sig;
                    if (cur != before_sig) {
                        still = cur == prev ? still + 1 : 0;
                        prev = cur;
                        if (still >= 6)
                            break;
                    } else {
                        prev = cur;
                    }
                }
            }
        }
        // Build the page from the (possibly just-navigated) snapshot.
        {
            nlohmann::json r;
            nlohmann::json loop = nlohmann::json::array();
            size_t total = 0, from = (size_t)std::max(0, start), to = 0;
            std::vector<RemoteBrowseItem> rows;
            bool not_root = false;
            if (control_) {
                auto s = control_->snapshot_state();
                rows = std::move(s.browse);
                not_root = !rows.empty() && rows[0].depth > 0;
            }
            total = rows.size() + (not_root ? 1 : 0);
            to = total;
            if (window > 0)
                to = std::min(to, from + (size_t)window);
            // META-4: the first page carries the virtual search row — shrink the data
            //   window by one so the NEXT page request (client counts every item,
            //   search row included) lands exactly on the right data offset.
            if (window > 0 && from == 0 && control_) {
                std::string m = control_->snapshot_state().mode;
                int virt = 0;
                if (m == "ONLINE" || m == "BILIBILI" || m == "ACCOUNT" || m == "TIKTOK")
                    virt += 1; // search row
                if (m == "ACCOUNT" || m == "BILIBILI" || m == "TIKTOK")
                    virt += 1; // login row
                if (virt > 0)
                    to = std::max(from, std::min(to, from + (size_t)window - (size_t)virt));
            }
            // META-4: search input row at the very top of the MODE ROOT page for
            //   modes with a query-taking search. Tapping it opens the phone's text
            //   input; submitting executes `panicast search <query>` (__INPUT__ is
            //   replaced client-side), which runs the search and this refreshed page
            //   then shows the results.
            if (from == 0 && control_) {
                std::string m = control_->snapshot_state().mode;
                // META-6: login entry at the top of account-bearing modes (before
                //   the search row) — tappable, opens the authorization page.
                if (m == "ACCOUNT" || m == "BILIBILI" || m == "TIKTOK") {
                    // META-7e: unified one-tap login row (Y/B/T share this pattern).
                    //   Pre-fetch the auth URL (cached 10 min), put it as a weblink
                    //   → one tap opens the browser on the auth page. Background
                    //   poll rides the bus action. Fallback: go-action on failure.
                    auto &lc = login_cache_[m];
                    bool fresh = lc.valid && std::chrono::steady_clock::now() - lc.fetched_at <
                                                 std::chrono::seconds(600);
                    if (!fresh) {
                        lc.valid = false;
                        if (m == "ACCOUNT") {
                            auto dc = GoogleOAuth::request_device_code();
                            if (dc.ok) {
                                lc.url = dc.verification_url;
                                if (!dc.user_code.empty())
                                    lc.url += "?user_code=" + dc.user_code;
                                lc.user_code = dc.user_code;
                                lc.poll_key = dc.device_code;
                                lc.bus_action = "_remote_login_youtube";
                                lc.valid = true;
                            }
                        } else if (m == "BILIBILI") {
                            auto qr = BilibiliAPI::request_qrcode();
                            if (qr.ok && !qr.url.empty()) {
                                lc.url = qr.url;
                                lc.user_code = "";
                                lc.poll_key = qr.qrcode_key;
                                lc.bus_action = "_remote_login_bilibili";
                                lc.valid = true;
                            }
                        } else if (m == "TIKTOK") {
                            DouyinApi dy;
                            auto qr = dy.request_qrcode();
                            if (qr.ok && !qr.qr_content.empty()) {
                                lc.url = qr.qr_content;
                                lc.user_code = "";
                                lc.poll_key = qr.token;
                                lc.bus_action = "_remote_login_tiktok";
                                lc.valid = true;
                            }
                        }
                        if (lc.valid) {
                            lc.fetched_at = std::chrono::steady_clock::now();
                            // start the background poll
                            if (bus_ && !lc.bus_action.empty())
                                bus_->push({lc.bus_action, {lc.poll_key}, 0});
                        }
                    }
                    if (lc.valid) {
                        nlohmann::json row;
                        row["text"] = lc.user_code.empty() ? "🔓 Login (opens browser)"
                                                           : "🔓 Login · " + lc.user_code;
                        row["weblink"] = lc.url;
                        loop.push_back(row);
                    } else {
                        // fallback: go-action (returns a page with the link)
                        nlohmann::json go;
                        go["cmd"] = nlohmann::json::array({"panicast", "login", m});
                        nlohmann::json row;
                        row["text"] = "🔓 Login";
                        row["actions"] = nlohmann::json({{"go", go}});
                        loop.push_back(row);
                    }
                } // close if (m == "ACCOUNT" || m == "BILIBILI" || m == "TIKTOK")

                if (m == "ONLINE" || m == "BILIBILI" || m == "ACCOUNT" || m == "TIKTOK") {
                    nlohmann::json do_cmd =
                        nlohmann::json::array({"panicast", "search", "__INPUT__"});
                    nlohmann::json row;
                    row["text"] = "🔍 Search…";
                    nlohmann::json inp;
                    inp["len"] = 200;
                    inp["initialText"] = "";
                    inp["_inputStyle"] = "text";
                    row["input"] = inp;
                    row["actions"] = nlohmann::json({{"do", {{"cmd", do_cmd}}}});
                    loop.push_back(row);
                }
            }
            for (size_t p = from; p < to; ++p) {
                nlohmann::json it;
                nlohmann::json go;
                if (not_root && p == 0) {
                    it["text"] = "..";
                    go["cmd"] = nlohmann::json::array({"panicast", "browse", "back"});
                } else {
                    const auto &row = rows[not_root ? p - 1 : p];
                    std::string row_idx = std::to_string(not_root ? p - 1 : p);
                    // Depth indent + branch marker: the flat mirror carries depth — show
                    //   it, or an expanded tree reads as one undifferentiated list.
                    std::string text(row.depth * 2, ' ');
                    text += row.is_branch ? "▸ " : "";
                    text += row.title;
                    if (!row.subtext.empty())
                        text += "\n" + row.subtext;
                    it["text"] = text;
                    if (!row.art_url.empty())
                        it["icon"] = row.art_url;
                    go["cmd"] = nlohmann::json::array({"panicast", "browse", row_idx});
                    // META-7h: search records + F-mode → DIRECT delete (no menu);
                    //   normal rows → context menu (bottom sheet with useContextMenu)
                    nlohmann::json more;
                    {
                        std::string cm = control_ ? control_->snapshot_state().mode : "";
                        bool is_search = row.title.rfind("🔍", 0) == 0 && cm == "ONLINE";
                        if (is_search || cm == "FAVOURITE") {
                            more["cmd"] = nlohmann::json::array({"panicast", "unfav", row_idx});
                        } else {
                            more["cmd"] = nlohmann::json::array({"panicast", "context", row_idx});
                            more["params"] = nlohmann::json({{"useContextMenu", "1"}});
                        }
                    }
                    it["actions"] = nlohmann::json({{"go", go}, {"more", more}});
                    loop.push_back(it);
                    continue;
                }
                it["actions"] = nlohmann::json({{"go", go}});
                loop.push_back(it);
            }
            bool search_row = false, login_row = false;
            if (from == 0 && control_) {
                std::string m = control_->snapshot_state().mode;
                search_row = m == "ONLINE" || m == "BILIBILI" || m == "ACCOUNT" || m == "TIKTOK";
                login_row = m == "ACCOUNT" || m == "BILIBILI" || m == "TIKTOK";
            }
            r["count"] = (int)total + (search_row ? 1 : 0) +
                         (login_row ? 1 : 0); // META-4/META-6 virtual rows
            r["offset"] = start > 0 ? start : 0;
            r["item_loop"] = loop;
            return r;
        }
    } else if (k == "menu") {
        // Home-menu request (`menu 0 <n> direct:1` at connect) — Squeeze Client's
        //   MAIN screen renders this list. Two entries: the CURRENT mode's library
        //   (browse mirror of the TUI list) and the current playlist (browse view of
        //   the queue; rows carry per-item "playlist index" go actions from
        //   status_data). Requires count/offset as JSON NUMBERS
        //   (JiveHomeItemListResponse: Int) and every item needs id+node (Strings).
        std::string mode_name = control_ ? control_->snapshot_state().mode : "RADIO";
        nlohmann::json r;
        nlohmann::json loop = nlohmann::json::array();
        // One entry per App mode (tap = switch + open that mode's list). The CURRENT
        //   mode is marked so the phone shows where you are; switching from the phone
        //   switches the TUI too — both frontends share one engine by design.
        //   node MUST be "home" on every entry: release builds (2.4) render the home
        //   screen by filtering on node == "home" — anything else never shows.
        // Internal name (sent in the go cmd) + display label — Y mode is labelled the
        //   way the TUI user knows it, not as the internal "ACCOUNT".
        // META-5: per-mode icons (Wikimedia Commons emoji thumbs, verified
        //   reachable; absolute URLs — the app fetches them directly, no auth).
        static const std::vector<std::tuple<std::string, const char *, const char *>> modes = {
            {"RADIO", "Radio",
             "https://upload.wikimedia.org/wikipedia/commons/thumb/6/61/"
             "Emoji_u1f4fb.svg/250px-Emoji_u1f4fb.svg.png"},
            {"PODCAST", "Podcasts",
             "https://upload.wikimedia.org/wikipedia/commons/thumb/b/b5/Emoji_u1f3a7.svg/"
             "250px-Emoji_u1f3a7.svg.png"},
            {"FAVOURITE", "Favourites",
             "https://upload.wikimedia.org/wikipedia/commons/thumb/0/0d/Emoji_u1f31f.svg/"
             "250px-Emoji_u1f31f.svg.png"},
            {"HISTORY", "History",
             "https://upload.wikimedia.org/wikipedia/commons/thumb/c/ce/Emoji_u1f553.svg/"
             "250px-Emoji_u1f553.svg.png"},
            {"ONLINE", "Online (O)",
             "https://upload.wikimedia.org/wikipedia/commons/thumb/9/9b/Emoji_u1f50d.svg/"
             "250px-Emoji_u1f50d.svg.png"},
            {"ACCOUNT", "YouTube (Y)",
             "https://upload.wikimedia.org/wikipedia/commons/thumb/0/08/Emoji_u1f3ac.svg/"
             "250px-Emoji_u1f3ac.svg.png"},
            {"BILIBILI", "Bilibili (B)",
             "https://upload.wikimedia.org/wikipedia/commons/thumb/1/1b/Emoji_u1f3b6.svg/"
             "250px-Emoji_u1f3b6.svg.png"},
            {"TIKTOK", "TikTok (T)",
             "https://upload.wikimedia.org/wikipedia/commons/thumb/3/36/Emoji_u1f4fa.svg/"
             "250px-Emoji_u1f4fa.svg.png"},
            {"IPTV", "IPTV",
             "https://upload.wikimedia.org/wikipedia/commons/thumb/8/8c/Emoji_u1f4d6.svg/"
             "250px-Emoji_u1f4d6.svg.png"},
        };
        int weight = 1;
        for (const auto &mi : modes) {
            const std::string &m = std::get<0>(mi);
            nlohmann::json go;
            go["cmd"] = nlohmann::json::array({"panicast", "mode", m});
            nlohmann::json item;
            item["id"] = "mode-" + m;
            item["node"] = "home";
            item["text"] = std::string(m == mode_name ? "▶ " : "") + std::get<1>(mi);
            item["icon"] = std::get<2>(mi); // META-5
            item["weight"] = weight++;
            item["actions"] = nlohmann::json({{"go", go}});
            loop.push_back(item);
        }
        {
            nlohmann::json go;
            go["cmd"] = nlohmann::json::array({"status"});
            nlohmann::json item;
            item["id"] = "currentplaylist";
            item["node"] = "home";
            item["text"] = "Current Playlist";
            item["icon"] = "https://upload.wikimedia.org/wikipedia/commons/thumb/f/f1/"
                           "Emoji_u1f3bc.svg/250px-Emoji_u1f3bc.svg.png"; // META-5
            item["weight"] = weight;
            item["actions"] = nlohmann::json({{"go", go}});
            loop.push_back(item);
        }
        r["count"] = (int)loop.size();
        r["offset"] = 0;
        r["item_loop"] = loop;
        return r;
    } else if (k == "alarm" || k == "alarms") {
        nlohmann::json r;
        r["count"] = 0;
        r["alarms_loop"] = nlohmann::json::array();
        return r;
    } else if (std::find(cmd.begin(), cmd.end(), "items") != cmd.end() || k == "artists" ||
               k == "albums" || k == "titles" || k == "tracks" || k == "genres" || k == "years" ||
               k == "playlists" || k == "favorites" || k == "browsers" || k == "musicfolder" ||
               k == "sync" || k == "name") {
        // Any browse-style query / single-player no-op we don't serve → an empty page
        //   (NOT an empty object: the ItemListener contract needs count/item_loop or
        //   the app spins).
        return empty_page();
    } else {
        LOG(fmt::format("[LMS-JSON] unhandled command: {}",
                        fmt::join(cmd.begin(), cmd.end(), " ")));
    }
    return nlohmann::json::object();
}

} // namespace panicast
