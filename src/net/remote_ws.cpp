// WebSocket frontend implementation (N06). POSIX. See remote_ws.h.
#include "panicast/net/remote_ws.h"

#include "panicast/core/logger.h"
#include "panicast/net/remote_command_bus.h"
#include "panicast/net/remote_protocol.h"
#include "panicast/net/remote_server.h"
#include "panicast/net/remote_session.h"

#include <openssl/sha.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

namespace panicast
{


namespace
{

// ── base64 (only used for the 20-byte SHA1 digest in the WS handshake) ──
std::string base64_encode(const unsigned char *data, size_t len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = data[i] << 16;
        if (i + 1 < len)
            n |= data[i + 1] << 8;
        if (i + 2 < len)
            n |= data[i + 2];
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? tbl[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? tbl[n & 63] : '=');
    }
    return out;
}

std::string ws_compute_accept(const std::string &key) {
    std::string in = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char *>(in.data()), in.size(), digest);
    return base64_encode(digest, SHA_DIGEST_LENGTH);
}

// Server→client text frame (unmasked — servers must not mask).
std::string ws_encode_text_frame(const std::string &payload) {
    std::string f;
    f.push_back(char(0x81)); // FIN + text
    size_t len = payload.size();
    if (len < 126) {
        f.push_back(char(len));
    } else if (len < 65536) {
        f.push_back(char(126));
        f.push_back(char((len >> 8) & 0xff));
        f.push_back(char(len & 0xff));
    } else {
        f.push_back(char(127));
        for (int i = 7; i >= 0; --i)
            f.push_back(char((len >> (8 * i)) & 0xff));
    }
    f += payload;
    return f;
}

// Client→server frame decode (client frames are masked). Returns true if a complete frame was
//   decoded (consumed bytes removed from buf by caller via `consumed`). payload set for text;
//   is_close set for close frames. Pings are answered by the caller.
bool ws_decode_frame(const std::string &buf, size_t &consumed, std::string &payload, bool &is_close,
                     bool &is_ping) {
    consumed = 0;
    is_close = false;
    is_ping = false;
    if (buf.size() < 2)
        return false;
    unsigned char b0 = (unsigned char)buf[0];
    unsigned char b1 = (unsigned char)buf[1];
    int opcode = b0 & 0x0f;
    bool masked = (b1 & 0x80) != 0;
    size_t len = b1 & 0x7f;
    size_t header = 2;
    if (len == 126) {
        if (buf.size() < 4)
            return false;
        len = (unsigned char)buf[2] << 8 | (unsigned char)buf[3];
        header = 4;
    } else if (len == 127) {
        if (buf.size() < 10)
            return false;
        len = 0;
        for (int i = 0; i < 8; ++i)
            len = (len << 8) | (unsigned char)buf[2 + i];
        header = 10;
    }
    if (masked)
        header += 4;
    if (buf.size() < header + len)
        return false; // not whole yet

    const char *payload_start = buf.data() + header;
    payload.assign(payload_start, len);
    if (masked) {
        const char *mask = buf.data() + header - 4;
        for (size_t i = 0; i < len; ++i)
            payload[i] ^= mask[i % 4];
    }
    consumed = header + len;
    if (opcode == 0x8)
        is_close = true;
    else if (opcode == 0x9)
        is_ping = true;
    return true;
}

bool send_all(int fd, const std::string &s) {
    size_t sent = 0;
    while (sent < s.size()) {
#ifdef MSG_NOSIGNAL
        ssize_t n = ::send(fd, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
#else
        ssize_t n = ::send(fd, s.data() + sent, s.size() - sent, 0);
#endif
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

// Read until the full HTTP request headers (\r\n\r\n). Returns the raw request text.
bool read_http_request(int fd, std::string &req) {
    char chunk[2048];
    while (req.find("\r\n\r\n") == std::string::npos) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (::poll(&pfd, 1, 5000) <= 0)
            return false;
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0)
            return false;
        req.append(chunk, (size_t)n);
        if (req.size() > 16384)
            return false; // sanity cap
    }
    return true;
}

std::string header_value(const std::string &req, const std::string &name) {
    std::string n = name;
    for (auto &c : n)
        c = (char)tolower(c);
    std::string low = req;
    for (auto &c : low)
        c = (char)tolower(c);
    size_t pos = low.find(n + ":");
    if (pos == std::string::npos)
        return "";
    pos += n.size() + 1;
    while (pos < low.size() && (low[pos] == ' ' || low[pos] == '\t'))
        pos++;
    size_t end = low.find("\r\n", pos);
    return req.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// The embedded BS web client — a single self-contained HTML (inline CSS+JS, IE11-compatible).
//   No external files → "open http://host:port/ in any IE and control directly".
const char *WS_HTML =
#include "panicast/net/panicast_web_index.h"
    ;

} // namespace

void ws_serve_connection(int client_fd, RemoteServer &server, bool localhost) {
    std::string req;
    if (!read_http_request(client_fd, req)) {
        ::close(client_fd);
        return;
    }

    bool ws_upgrade = (req.find("Upgrade: websocket") != std::string::npos ||
                       req.find("Upgrade: WebSocket") != std::string::npos);

    if (!ws_upgrade) {
        // Serve the embedded BS client for any GET.
        std::string body(WS_HTML);
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) +
                           "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n" + body;
        send_all(client_fd, resp);
        ::close(client_fd);
        return;
    }

    // ── WS handshake ──
    std::string key = header_value(req, "Sec-WebSocket-Key");
    if (key.empty()) {
        ::close(client_fd);
        return;
    }
    std::string accept = ws_compute_accept(key);
    std::string handshake = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                            "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                            accept + "\r\n\r\n";
    if (!send_all(client_fd, handshake)) {
        ::close(client_fd);
        return;
    }

    // N04: surface the pairing PIN in the LOG area for off-host WS clients (browsers).
    if (!localhost) {
        server.command_bus().push(RemoteCommand{"_pin_log", {std::string{"ws"}}, 0});
    }

    // ── Bridge: socketpair feeds a RemoteSession (PRP), the bridge shuttles WS frames. ──
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        ::close(client_fd);
        return;
    }
    int session_fd = sv[0]; // RemoteSession reads/writes here
    int bridge_fd = sv[1];  // bridge reads/writes here

    RemoteCommandBus &bus = server.command_bus();
    RemoteControlInterface *control = server.control_interface();
    std::string pin = server.dynamic_pin();
    std::string upin = server.universal_pin();

    RemoteSession *session = new RemoteSession(session_fd, session_fd, 0, bus, control,
                                               /*open_access=*/localhost, pin, upin);
    server.register_session(session);
    std::thread sess_thread([session]() { session->run(); });

    // Bridge loop: poll(client_fd, bridge_fd). client→decode→write bridge_fd; bridge_fd→frame→client.
    std::string ws_in;  // accumulated WS frame bytes from the browser
    std::string prp_in; // accumulated PRP bytes from the session (to frame)
    bool ok = true;
    while (ok) {
        struct pollfd pfd[2];
        pfd[0].fd = client_fd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = bridge_fd;
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;
        if (::poll(pfd, 2, 30000) <= 0)
            break; // timeout / error

        // Browser → session: decode WS frames, write PRP lines to bridge_fd.
        if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char chunk[4096];
            ssize_t n = ::recv(client_fd, chunk, sizeof(chunk), 0);
            if (n <= 0)
                break;
            ws_in.append(chunk, (size_t)n);
            size_t consumed = 0;
            std::string payload;
            bool is_close = false, is_ping = false;
            while (ws_decode_frame(ws_in, consumed, payload, is_close, is_ping)) {
                ws_in.erase(0, consumed);
                if (is_close) {
                    ok = false;
                    break;
                }
                if (is_ping) {
                    // Reply with a pong (opcode 0xA) carrying the ping payload.
                    std::string pong;
                    pong.push_back(char(0x8A));
                    size_t l = payload.size();
                    if (l < 126)
                        pong.push_back(char(l));
                    else {
                        pong.push_back(char(126));
                        pong.push_back(char((l >> 8) & 0xff));
                        pong.push_back(char(l & 0xff));
                    }
                    pong += payload;
                    send_all(client_fd, pong);
                    continue;
                }
                // text frame → a PRP line. Ensure it ends with \n, forward to the session.
                payload.erase(payload.find_last_not_of("\r\n") + 1);
                payload.push_back('\n');
                if (::write(bridge_fd, payload.data(), payload.size()) <= 0) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                break;
        }

        // Session → browser: read PRP response bytes, frame as WS text, send.
        if (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            char chunk[4096];
            ssize_t n = ::read(bridge_fd, chunk, sizeof(chunk));
            if (n <= 0)
                break;
            prp_in.append(chunk, (size_t)n);
            // Send each PRP line as its own WS text frame (browsers parse PRP lines from frames).
            size_t pos;
            while ((pos = prp_in.find('\n')) != std::string::npos) {
                std::string line = prp_in.substr(0, pos + 1);
                prp_in.erase(0, pos + 1);
                if (!send_all(client_fd, ws_encode_text_frame(line))) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                break;
        }
    }

    // Tear down: signal the session to exit (close its fd), join, unregister.
    ::close(bridge_fd); // session's read sees EOF → poll_line returns false → run() exits
    if (sess_thread.joinable())
        sess_thread.join();
    server.unregister_session(session);
    delete session;
    ::close(client_fd);
}


} // namespace panicast
