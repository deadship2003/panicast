// Per-connection PRP protocol engine (RemoteSession). Runs on a RemoteServer worker thread.
//   Reads line-delimited commands from the socket, dispatches CONTROL commands into the
//   RemoteCommandBus (for the UI thread) and answers QUERY commands (status / currentsong / ping /
//   password) inline from the state snapshot.
//
//   N07: `idle [subsystems]` enters event-subscription mode (MPD semantics). The session blocks
//   emitting `changed: <subsystem>` lines + `OK` until a subscribed subsystem changes OR the client
//   sends `noidle` / another command / closes. Multiplexing is done with poll(fd, timeout) so the
//   socket is watched for noidle/close WHILE waiting for server-side notify. notify_change() is
//   called by RemoteServer (driven by App explicit notify + the 10Hz diff poller).
//
//   Transport-agnostic: the same engine is fed by the raw-TCP frontend (N02) and the WebSocket
//   frontend (N08, one text frame per line).
#pragma once

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "panicast/net/remote_command_bus.h"
#include "panicast/net/remote_protocol.h"

namespace panicast
{

class RemoteSession {
public:
    RemoteSession(int read_fd, int write_fd, int64_t client_id, RemoteCommandBus &bus,
                  RemoteControlInterface *control, bool open_access, const std::string &dynamic_pin,
                  const std::string &universal_pin);
    ~RemoteSession();

    RemoteSession(const RemoteSession &) = delete;
    RemoteSession &operator=(const RemoteSession &) = delete;

    // Blocks until the peer closes or the connection drops. Called on a worker thread.
    void run();

    // N07: deliver a subsystem change to this session if it is idling and subscribed. Called from
    //   any server thread (RemoteServer::notify). No-op if not idling / not subscribed.
    void notify_change(const std::string &subsystem);

private:
    bool send_str(const std::string &s);
    void send_ok();
    void send_ack(int code, const std::string &command, const std::string &msg);

    void handle_line(const std::string &line);
    void cmd_status();
    void cmd_currentsong();
    void forward(std::string action, std::vector<std::string> args);

    // Idle (N07). Returns true if idle was active and has now ended (response sent).
    bool handle_idle(const std::vector<std::string> &subsystems);
    // Poll the socket for up to timeout_ms; if a complete line arrives, return it. Returns false
    //   on timeout or closed/partial. Shares recv_buf_ with run().
    bool poll_line(std::string &out, int timeout_ms);
    void emit_idle_response(); // flush idle_pending_ → changed: lines + OK

    int read_fd_;  // PRP commands arrive here (raw TCP socket, or the read end of a WS bridge pipe)
    int write_fd_; // PRP responses go here (raw TCP socket, or the write end of a WS bridge pipe)
    int64_t client_id_;
    RemoteCommandBus &bus_;
    RemoteControlInterface *control_;
    std::string dynamic_pin_;   // server's current dynamic PIN
    std::string universal_pin_; // configurable universal PIN (default 6696), always valid
    bool authed_ = false;
    bool closed_ = false; // set by the `close` command or a send/recv failure → run() exits

    std::string recv_buf_; // bytes received but not yet line-terminated

    // Idle subscription state. idle_active_ is read/written only on the session thread except
    //   notify_change() (other server threads) which guards with idle_mtx_.
    std::mutex idle_mtx_;
    bool idle_active_ = false;
    bool idle_all_ = false;                 // idle with no subsystem filter → subscribe to all
    std::set<std::string> subscribed_;      // subsystems the client subscribed to
    std::vector<std::string> idle_pending_; // subsystems with pending changes (deduped)
};

} // namespace panicast
