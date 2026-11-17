// RemoteSession implementation — PRP line protocol engine (N02) + idle subscription (N07). POSIX.
//   See remote_session.h for the threading/transport contract.
#include "panicast/net/remote_session.h"

#include "panicast/core/constants.h"
#include "panicast/core/logger.h"

#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sstream>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace panicast
{

namespace
{

std::vector<std::string> tokenize(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quote = false;
    bool has_token = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quote) {
            if (c == '"') {
                in_quote = false;
            } else {
                cur.push_back(c);
            }
            continue;
        }
        if (c == '"') {
            in_quote = true;
            has_token = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            if (has_token) {
                out.push_back(cur);
                cur.clear();
                has_token = false;
            }
            continue;
        }
        cur.push_back(c);
        has_token = true;
    }
    if (has_token)
        out.push_back(cur);
    return out;
}

std::string fmt1(double v) {
    return fmt::format("{:.1f}", v);
}

} // namespace


RemoteSession::RemoteSession(int read_fd, int write_fd, int64_t client_id, RemoteCommandBus &bus,
                             RemoteControlInterface *control, bool open_access,
                             const std::string &dynamic_pin, const std::string &universal_pin)
    : read_fd_(read_fd), write_fd_(write_fd), client_id_(client_id), bus_(bus), control_(control),
      dynamic_pin_(dynamic_pin), universal_pin_(universal_pin) {
    authed_ = open_access; // localhost (or no-auth) connections are pre-authed
}

// A PIN is valid if it matches the server's current dynamic PIN or the configured universal PIN.
static bool pin_valid(const std::string &pin, const std::string &dynamic_pin,
                      const std::string &universal_pin) {
    if (!universal_pin.empty() && pin == universal_pin)
        return true;
    return !dynamic_pin.empty() && pin == dynamic_pin;
}

RemoteSession::~RemoteSession() {
    if (read_fd_ >= 0)
        ::close(read_fd_);
    if (write_fd_ >= 0 && write_fd_ != read_fd_)
        ::close(write_fd_);
}

bool RemoteSession::send_str(const std::string &s) {
    if (write_fd_ < 0)
        return false;
    size_t sent = 0;
    while (sent < s.size()) {
#ifdef MSG_NOSIGNAL
        ssize_t n = ::send(write_fd_, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
#else
        ssize_t n = ::send(write_fd_, s.data() + sent, s.size() - sent, 0);
#endif
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            closed_ = true;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

void RemoteSession::send_ok() {
    send_str("OK\n");
}

void RemoteSession::send_ack(int code, const std::string &command, const std::string &msg) {
    send_str(fmt::format("ACK [{}@0] {{{}}} {}\n", code, command, msg));
}

void RemoteSession::forward(std::string action, std::vector<std::string> args) {
    bus_.push(RemoteCommand{std::move(action), std::move(args), client_id_});
    send_ok();
}

void RemoteSession::cmd_status() {
    if (!control_) {
        send_ack(50, "status", "control interface unavailable");
        return;
    }
    RemoteStateSnapshot s = control_->snapshot_state();
    std::string state = !s.has_media ? "stop" : (s.paused ? "pause" : "play");
    std::ostringstream o;
    o << "volume: " << s.volume << "\n"
      << "state: " << state << "\n"
      << "elapsed: " << fmt1(s.elapsed) << "\n"
      << "duration: " << fmt1(s.duration) << "\n"
      << "speed: " << fmt1(s.speed) << "\n"
      << "mode: " << s.mode << "\n"
      << "play_mode: " << s.play_mode << "\n"
      << "playlistlength: " << s.playlist.size() << "\n"
      << "song: " << (s.current_index >= 0 ? s.current_index : -1) << "\n"
      << "nextsong: "
      << (s.current_index >= 0 && !s.playlist.empty()
              ? (s.current_index + 1) % static_cast<int>(s.playlist.size())
              : -1)
      << "\n"
      << "video: " << (s.has_video ? 1 : 0) << "\n"
      << "audio_codec: " << s.audio_codec << "\n"
      << "net_speed_bps: " << static_cast<long long>(s.net_speed_bps) << "\n"
      << "buffering_pct: " << s.buffering_pct << "\n"
      << "sleep_remaining: " << s.sleep_remaining << "\n"
      << "subtitle: " << (s.subtitle_active ? 1 : 0) << "\n"
      << "OK\n";
    send_str(o.str());
}

void RemoteSession::cmd_currentsong() {
    if (!control_) {
        send_ack(50, "currentsong", "control interface unavailable");
        return;
    }
    RemoteStateSnapshot s = control_->snapshot_state();
    if (!s.has_media) {
        send_ok();
        return;
    }
    std::ostringstream o;
    o << "file: " << s.url << "\n"
      << "title: " << s.title << "\n"
      << "duration: " << fmt1(s.duration) << "\n"
      << "art: " << s.art_url << "\n"
      << "OK\n";
    send_str(o.str());
}

// N07: deliver a subsystem change if this session is idling and subscribed (any thread).
void RemoteSession::notify_change(const std::string &subsystem) {
    std::lock_guard<std::mutex> lk(idle_mtx_);
    if (!idle_active_)
        return;
    if (!idle_all_ && subscribed_.find(subsystem) == subscribed_.end())
        return;
    if (std::find(idle_pending_.begin(), idle_pending_.end(), subsystem) == idle_pending_.end()) {
        idle_pending_.push_back(subsystem); // dedup
    }
}

void RemoteSession::emit_idle_response() {
    std::vector<std::string> pend;
    {
        std::lock_guard<std::mutex> lk(idle_mtx_);
        pend.swap(idle_pending_);
    }
    std::string out;
    for (const auto &s : pend)
        out += "changed: " + s + "\n";
    out += "OK\n";
    send_str(out);
}

// Poll the socket for up to timeout_ms; if a complete line arrives, return it via `out`.
//   Shares recv_buf_ with run(). Returns false on timeout (no full line) or connection close.
bool RemoteSession::poll_line(std::string &out, int timeout_ms) {
    size_t pos = recv_buf_.find('\n');
    if (pos != std::string::npos) {
        out = recv_buf_.substr(0, pos);
        recv_buf_.erase(0, pos + 1);
        return true;
    }
    int waited = 0;
    const int step = 50;
    while (read_fd_ >= 0 && !closed_) {
        int t = (timeout_ms >= 0) ? std::min(step, timeout_ms - waited) : step;
        if (t < 0)
            t = 0;
        struct pollfd pfd;
        pfd.fd = read_fd_;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int rc = ::poll(&pfd, 1, t);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (rc > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            char chunk[4096];
            ssize_t n = ::recv(read_fd_, chunk, sizeof(chunk), 0);
            if (n <= 0)
                return false; // peer closed / error
            recv_buf_.append(chunk, static_cast<size_t>(n));
            pos = recv_buf_.find('\n');
            if (pos != std::string::npos) {
                out = recv_buf_.substr(0, pos);
                recv_buf_.erase(0, pos + 1);
                return true;
            }
            // partial line — keep polling
        }
        waited += t;
        if (timeout_ms >= 0 && waited >= timeout_ms)
            return false; // timeout
    }
    return false;
}

// N07: enter idle mode. Block until a subscribed subsystem changes (emit changed: + OK) OR the
//   client sends noidle / another command / closes. Returns when idle ends.
bool RemoteSession::handle_idle(const std::vector<std::string> &subsystems) {
    {
        std::lock_guard<std::mutex> lk(idle_mtx_);
        idle_active_ = true;
        idle_all_ = subsystems.empty();
        subscribed_.clear();
        for (const auto &s : subsystems)
            subscribed_.insert(s);
        idle_pending_.clear();
    }
    while (read_fd_ >= 0 && !closed_) {
        // Flush any pending change first.
        bool have_pending = false;
        {
            std::lock_guard<std::mutex> lk(idle_mtx_);
            have_pending = !idle_pending_.empty();
        }
        if (have_pending) {
            emit_idle_response();
            break;
        }

        // Watch the socket for noidle / a queued command / close, 100ms at a time.
        std::string line;
        if (!poll_line(line, 100))
            continue; // timeout → re-check pending (notify may have fired)

        // A line arrived during idle → end idle (emit pending + OK), then handle the line.
        emit_idle_response();
        {
            std::lock_guard<std::mutex> lk(idle_mtx_);
            idle_active_ = false;
        }
        if (line == "noidle")
            return true;   // idle cancelled, OK already sent
        handle_line(line); // a real command queued during idle
        return true;
    }
    std::lock_guard<std::mutex> lk(idle_mtx_);
    idle_active_ = false;
    return true;
}

void RemoteSession::handle_line(const std::string &line) {
    auto tok = tokenize(line);
    if (tok.empty())
        return;
    const std::string &cmd = tok[0];
    std::vector<std::string> args(tok.begin() + 1, tok.end());

    if (!authed_ && cmd != "password") {
        send_ack(5, cmd, "auth required (send: password <token>)");
        return;
    }

    if (cmd == "ping") {
        send_ok();
        return;
    }
    if (cmd == "close") {
        send_ok();
        closed_ = true;
        return;
    }
    if (cmd == "password") {
        // PIN auth: valid if it matches the dynamic PIN or the universal 6696.
        if (args.empty()) {
            send_ack(5, "password", "missing pin");
            return;
        }
        if (pin_valid(args[0], dynamic_pin_, universal_pin_)) {
            authed_ = true;
            send_ok();
        } else {
            send_ack(5, "password", "bad pin");
        }
        return;
    }
    if (cmd == "idle") {
        handle_idle(args);
        return;
    } // N07: subscription
    if (cmd == "noidle") {
        send_ok();
        return;
    } // outside idle → no-op OK
    if (cmd == "status") {
        cmd_status();
        return;
    }
    if (cmd == "currentsong") {
        cmd_currentsong();
        return;
    }
    if (cmd == "playlistinfo") {
        if (!control_) {
            send_ack(50, "playlistinfo", "control interface unavailable");
            return;
        }
        RemoteStateSnapshot s = control_->snapshot_state();
        std::ostringstream o;
        for (size_t i = 0; i < s.playlist.size(); ++i) {
            o << "pos: " << i << "\n"
              << "title: " << s.playlist[i].title << "\n"
              << "duration: " << s.playlist[i].duration << "\n"
              << "video: " << (s.playlist[i].is_video ? 1 : 0) << "\n";
        }
        o << "OK\n";
        send_str(o.str());
        return;
    }

    forward(cmd, std::move(args));
}

void RemoteSession::run() {
    if (!send_str(fmt::format("OK {}\n", FULL_VERSION)))
        return; // greeting (MPD-style: OK <version>)

    while (read_fd_ >= 0 && !closed_) {
        // Drain any complete lines already buffered (e.g. multiple commands in one recv).
        size_t pos;
        while ((pos = recv_buf_.find('\n')) != std::string::npos) {
            std::string line = recv_buf_.substr(0, pos);
            recv_buf_.erase(0, pos + 1);
            handle_line(line);
            if (closed_)
                return; // 'close'
        }
        // Block for the next line (long timeout ≈ blocking, but via poll so it's clean).
        std::string line;
        if (!poll_line(line, 3600000))
            break; // closed / dropped
        handle_line(line);
        if (closed_)
            return;
    }
}


} // namespace panicast
