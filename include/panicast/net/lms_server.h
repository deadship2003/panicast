// mini-LMS server — Squeezer remote control (N08). Speaks ONE protocol: the LMS
//   cometd/Bayeux JSON-RPC control plane over HTTP (what current Squeezer builds use —
//   verified by on-device capture; the older CLI line protocol was removed per design
//   decision "adapt Squeezer only"). POST /cometd carries a JSON array of Bayeux
//   messages: /meta/* keep the long-poll session, /service/* carry JSON-RPC
//   "slim.request" calls whose params are LMS command arrays.
//
//   Auth: HTTP Basic against [remote] lms_user/lms_pass (factory default
//   panicast/panicast — auth ON out of the box; explicitly empty lms_pass disables).
//   Transport-layer gate: [remote] lms_allow CIDR source allowlist checked at accept().
//
//   Reads go through RemoteControlInterface::snapshot_state() (thread-safe); writes are
//   pushed as RemoteCommands onto the bus and executed on the TUI main thread — the same
//   single sanctioned network→UI crossing the PRP/WS servers use.
//
// Compile layer: built only when CMake PANICAST_REMOTE_LMS=ON (default). Runtime layer:
//   [remote] lms_enable (default true). Singleton so the compile switch needs no #ifdef
//   in app.h.
//
// Threading model:
//   - start() spawns ONE accept thread.
//   - Each connection runs a long-lived reader thread; requests are answered inline
//     (keep-alive). No thread is held per long-poll — /meta/connect replies immediately
//     with advice.interval so the client paces itself.
//   - stop() shuts the listen fd down to unblock accept(), closes every client fd to
//     unblock the readers, then joins everything. No detached threads.
//
// Platform: POSIX sockets (Linux / macOS).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <sys/socket.h> // sockaddr_storage (peer_allowed — POSIX-only module)

#include "panicast/net/remote_protocol.h"

namespace panicast
{

class RemoteCommandBus;

// One IPv4 CIDR entry (network order). Bare IPs parse as /32. Namespace scope so the
//   file-local parser in lms_server.cpp can build the list.
struct LmsCidr {
    uint32_t net;
    uint32_t mask;
};

class LmsServer {
public:
    static LmsServer &instance();

    LmsServer(const LmsServer &) = delete;
    LmsServer &operator=(const LmsServer &) = delete;

    // Bind + listen + spawn the accept thread. `control` is the thread-safe state
    //   snapshot source; `bus` receives the transport/volume/seek commands (executed on
    //   the TUI main thread). Returns true on success.
    bool start(const std::string &bind_addr, int port, RemoteControlInterface *control,
               RemoteCommandBus *bus);
    void stop();

    bool is_running() const {
        return running_.load();
    }
    int port() const {
        return port_;
    }
    // Stable virtual-player identity Squeezer sees (also answers for any playerid).
    static const char *player_id() {
        return "00:00:00:00:84:21";
    }
    static const char *player_name() {
        return "panicast";
    }

private:
    LmsServer() = default;
    ~LmsServer();

    // One live controller connection: fd + write mutex + Basic-auth cache.
    struct Conn {
        int fd = -1;
        int64_t client_id = 0;
        std::mutex wmtx;                 // serializes response writes
        std::thread reader;              // blocked in client_loop()
        std::atomic<bool> done{false};   // set by client_loop right before returning
        std::atomic<bool> authed{false}; // Basic credentials verified (cached per conn)
        std::string http_auth_user;      // verified user (logging)
    };

    void accept_loop();
    void client_loop(Conn *c); // read HTTP requests → handle_http → write response
    std::string handle_http(Conn &c, const std::string &method, const std::string &path,
                            const std::string &headers, const std::string &body);
    nlohmann::json json_slim_request(Conn &c, const std::vector<std::string> &cmd);
    // Shared status builder. start < 0 → "current song" shape (item_loop[0] = playing track,
    //   what parsePlayerStatus builds the CurrentPlaylistItem from; also the push payload).
    //   start >= 0 → "playlist page" shape (Squeezer's CurrentPlaylistActivity orders
    //   `status <start> <window> menu:menu` pages): item_loop = jive items [start,
    //   start+window), count = total tracks. window <= 0 → everything from start.
    nlohmann::json status_data(int start = -1, int window = 0);
    void reap_done(); // join + drop finished conns (conns_mtx_ held)
    bool peer_allowed(const sockaddr_storage &peer) const; // lms_allow CIDR check

    RemoteControlInterface *control_ = nullptr;
    RemoteCommandBus *bus_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> next_client_id_{1};
    int listen_fd_ = -1;
    int port_ = 0;
    std::string bind_addr_;

    // Access policy, parsed once at start() from [remote] lms_allow / lms_user / lms_pass.
    std::vector<LmsCidr> allow_; // empty + !allow_all_ = nothing gets in (defensive)
    bool allow_all_ = false;     // lms_allow explicitly empty
    std::string lms_user_;
    std::string lms_pass_;
    bool auth_required_ = false; // non-empty lms_pass → Basic auth gate

    std::thread accept_thread_;

    // Push-state (server-global, NOT per-Conn: the app spreads requests over several
    //   sockets, so subscription state and last-push bookkeeping must survive across
    //   connections — keyed by Bayeux clientId).
    std::atomic<bool> any_subscribed_{false};             // any status interest seen
    std::mutex push_mtx_;                                 // guards last_push_by_cid_
    std::map<std::string, std::string> last_push_by_cid_; // cid → last pushed dump

    std::mutex conns_mtx_;
    std::vector<std::unique_ptr<Conn>> conns_;
};

} // namespace panicast
