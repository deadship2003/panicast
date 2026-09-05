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
        // N10.4: held streaming listener (Squeeze Client's /meta/connect event stream —
        //   it reads the response body as a NEVER-ENDING stream; a complete response
        //   is EOF → "connection failed" → reconnect loop). The reader thread becomes
        //   the single writer: everything the client must see on the stream goes
        //   through outq and is drained as ONE JSON array per chunk (two arrays in a
        //   single client read break its incremental parser permanently).
        bool listener = false;
        std::string bayeux_cid;           // Bayeux clientId it listens as
        std::mutex queue_mtx;             // guards outq
        std::vector<nlohmann::json> outq; // pending stream messages
    };

    void accept_loop();
    void client_loop(Conn *c); // read HTTP requests → handle_http → write response
    std::string handle_http(Conn &c, const std::string &method, const std::string &path,
                            const std::string &headers, const std::string &body, bool &held);
    // N10.4: pump for a held /meta/connect stream — runs on the conn's reader thread
    //   until the client goes away. Drains outq (one array chunk), pushes playerstatus
    //   on change, re-pushes every 20s as a read-timeout heartbeat (the client's
    //   OkHttp read timeout is subscription-interval + 5s ≈ 65s).
    void listen_loop(Conn *c);
    // Chunked-transfer write of one JSON array; false → connection is gone.
    bool write_chunk(Conn &c, const std::string &payload);
    // Queue a message for <cid>'s held stream (one-shot publish replies are delivered
    //   BOTH on their POST body and on the stream — master Squeeze Client only reads
    //   the stream). No-op when that clientId has no live listener.
    void stream_deliver(const std::string &cid, const nlohmann::json &msg);
    nlohmann::json json_slim_request(Conn &c, const std::vector<std::string> &cmd);
    // players/serverstatus payload (players_loop + prefs echo for requested keys).
    //   Shared by the command handler and the listen pump's periodic serverstatus
    //   push (Squeeze Client subscribes "serverstatus subscribe:60" and drops the
    //   connection when nothing arrives — cmd = {} omits the prefs echo).
    nlohmann::json serverstatus_data(const std::vector<std::string> &cmd);
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
    std::atomic<bool> any_subscribed_{false}; // any status interest seen
    std::atomic<bool> muted_{false};          // `mixer muting` state (LMS
                                              //   encodes mute as a NEGATIVE
                                              //   mixer volume; mpv holds the
                                              //   real audio state)

    // N10.4: Bayeux clientId → held listen connection. Guarded by listeners_mtx_.
    std::mutex listeners_mtx_;
    std::map<std::string, Conn *> listeners_;
    std::mutex push_mtx_;                                 // guards last_push_by_cid_
    std::map<std::string, std::string> last_push_by_cid_; // cid → last pushed dump

    std::mutex conns_mtx_;
    std::vector<std::unique_ptr<Conn>> conns_;
};

} // namespace panicast
