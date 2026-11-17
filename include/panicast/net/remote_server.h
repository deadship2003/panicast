// Remote control TCP server. N01 ships the accept-loop skeleton only: it listens on a TCP port,
//   accepts connections, and replies with a "protocol not implemented" banner. The command
//   protocol (MPD-style line protocol, pending design confirmation) lands in N02+.
//
// Threading model (follows the concurrency rules in DEVELOPMENT_PRINCIPLES.md):
//   - start() spawns ONE accept thread.
//   - Each accepted connection runs on a tracked worker thread (stored in workers_). No thread
//     is ever detached; stop() joins the accept thread AND every worker.
//   - Workers self-mark done via an atomic flag; the accept loop reaps finished workers each
//     iteration so the worker list stays bounded.
//   - stop() closes the listen socket to unblock accept(), then joins everything.
//
// Platform: POSIX sockets (Linux / macOS).
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "panicast/net/remote_protocol.h"

namespace panicast
{

class RemoteCommandBus;
class RemoteSession;

class RemoteServer {
public:
    explicit RemoteServer(RemoteCommandBus &bus);
    ~RemoteServer();

    RemoteServer(const RemoteServer &) = delete;
    RemoteServer &operator=(const RemoteServer &) = delete;

    // Bind + listen + spawn the accept thread + diff poller. `control` provides the thread-safe
    //   state snapshot for query commands. Returns true on success; on failure logs the error and
    //   leaves the server stopped.
    bool start(const std::string &bind_addr, int port, RemoteControlInterface *control);
    void stop();

    // N04: PIN auth. The dynamic PIN is a random 4-digit code shown in a panicast popup; the
    //   universal "6696" is always valid (for headless/no-display pairing). Localhost connections
    //   are open (no PIN). regenerate_pin() returns the new PIN.
    std::string dynamic_pin() const {
        return dynamic_pin_;
    }
    std::string universal_pin() const {
        return universal_pin_;
    }
    std::string regenerate_pin();

    bool is_running() const {
        return running_.load();
    }
    int port() const {
        return port_;
    }

    // N07: broadcast a subsystem change to all idling subscribed sessions. Safe to call from any
    //   thread (App UI thread or the diff poller). Drives the `changed: <subsystem>` push.
    void notify(const std::string &subsystem);

    // N06: accessors for the WS frontend (remote_ws.cpp) to build/bridge RemoteSessions.
    RemoteCommandBus &command_bus() {
        return bus_;
    }
    RemoteControlInterface *control_interface() {
        return control_;
    }
    void register_session(RemoteSession *s);
    void unregister_session(RemoteSession *s);

private:
    void accept_loop();
    void
    ws_accept_loop(); // N06: accept WebSocket/HTTP connections (serves the BS client + WS bridge)
    void handle_client(int fd, int64_t client_id, bool localhost);
    void diff_loop(); // N07: 10Hz state diff → notify(player/mixer/options/mode/art)
    void discovery_loop(int udp_port); // N05: respond to UDP discovery probes from the APK
    void reap_workers(); // join finished worker threads (called with workers_mtx_ held)

    RemoteCommandBus &bus_;
    RemoteControlInterface *control_ = nullptr;
    std::string dynamic_pin_;   // N04: random 4-digit PIN (6696 always valid too)
    std::string universal_pin_; // N04: configurable universal PIN (default 6696)
    std::atomic<int64_t> next_client_id_{1};
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    int ws_listen_fd_ = -1; // N06: WebSocket/HTTP listener (= port + 1)
    int port_ = 0;
    std::string bind_addr_;

    std::thread accept_thread_;
    std::thread ws_accept_thread_; // N06: WebSocket/HTTP accept
    std::thread diff_thread_;      // N07: state-diff poller (player/mixer/options/mode/art)
    std::thread discovery_thread_; // N05: UDP discovery responder

    // N07: live sessions (for notify broadcast). A session registers on start, unregisters on end.
    std::mutex sessions_mtx_;
    std::vector<RemoteSession *> sessions_;

    // Tracked worker threads. Worker holds its own std::thread + a done flag the worker sets
    //   right before returning, so the accept loop can join finished ones without blocking.
    struct Worker {
        std::thread t;
        std::atomic<bool> done{false};
        Worker() = default;
    };
    std::mutex workers_mtx_;
    std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace panicast
