// Slim UDP discovery responder (META-7): listens on :3483 for Squeezer's
//   broadcast "eIPAD\0NAME\0JSON\0" and replies with a TLV packet so every
//   panicast host on the LAN shows up in the app's server picker.
//
// Wire format (verified against Squeeze Client 2.4 ServerSetupActivity):
//   request  (broadcast :3483): "e" + NUL-separated 4-char field names
//   response (unicast to source): 'E' + [4-char tag][1-byte len][value]...
//   tags the app reads: NAME (display name), JSON (HTTP port as decimal string).
//   We also include UUID (instance identity — the app tolerates unknown tags)
//   and VERS.
#include "panicast/net/slim_discovery.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "panicast/config/ini_config.h"
#include "panicast/core/logger.h"

namespace panicast
{

namespace
{

// Discover-response fields, TLV-encoded after a leading 'E'.
// Tag is always 4 chars; length is one byte (max 255 per field).
void append_tlv(std::string &out, const char *tag, const std::string &value) {
    out.append(tag, 4);
    out += (char)std::min((size_t)255, value.size());
    out += value;
}

// Load-or-generate the persistent instance UUID. Stored in [remote] uuid so
//   it survives restarts and upgrades — the app deduplicates servers by it.
std::string instance_uuid() {
    std::string v = IniConfig::instance().get("remote", "uuid", "");
    if (!v.empty())
        return v;
    // Generate a v4 UUID from /dev/urandom (16 bytes → hex with dashes).
    unsigned char b[16] = {0};
    {
        std::ifstream f("/dev/urandom", std::ios::binary);
        f.read((char *)b, 16);
    }
    b[6] = (b[6] & 0x0F) | 0x40; // version 4
    b[8] = (b[8] & 0x3F) | 0x80;  // variant 10
    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
                  b[12], b[13], b[14], b[15]);
    v = buf;
    IniConfig::instance().set("remote", "uuid", v);
    LOG(fmt::format("[DISCOVERY] generated instance UUID {}", v));
    return v;
}

} // namespace

SlimDiscovery::SlimDiscovery() = default;

SlimDiscovery::~SlimDiscovery() {
    stop();
}

SlimDiscovery &SlimDiscovery::instance() {
    static SlimDiscovery d;
    return d;
}

bool SlimDiscovery::start(int http_port) {
    if (running_.exchange(true))
        return true;
    http_port_ = http_port;

    // Resolve the display name: [remote] server_name > hostname > "panicast".
    display_name_ = IniConfig::instance().get("remote", "server_name", "");
    if (display_name_.empty()) {
        char hn[256] = {0};
        if (::gethostname(hn, sizeof(hn) - 1) == 0 && hn[0])
            display_name_ = hn;
    }
    if (display_name_.empty())
        display_name_ = "panicast";

    uuid_ = instance_uuid();

    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        running_.store(false);
        return false;
    }
    int bc = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &bc, sizeof(bc));
    struct sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(3483);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd_, (struct sockaddr *)&a, sizeof(a)) != 0) {
        LOG(fmt::format("[DISCOVERY] bind(:3483) failed: {} — another instance on this "
                        "host owns the port (fine: the app finds us via manual IP)",
                        std::strerror(errno)));
        ::close(fd_);
        fd_ = -1;
        running_.store(false);
        return false; // NOT fatal — same-host multi-instance just doesn't advertise
    }

    thread_ = std::thread(&SlimDiscovery::loop, this);
    LOG(fmt::format("[DISCOVERY] Slim UDP discovery on :3483 — name='{}' uuid={} "
                    "http={}",
                    display_name_, uuid_, http_port));
    return true;
}

void SlimDiscovery::stop() {
    if (!running_.exchange(false))
        return;
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
    if (thread_.joinable())
        thread_.join();
}

void SlimDiscovery::loop() {
    char buf[512];
    while (running_.load()) {
        struct sockaddr_in peer {};
        socklen_t plen = sizeof(peer);
        ssize_t n = ::recvfrom(fd_, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &plen);
        if (n <= 0) {
            if (!running_.load())
                break;
            continue;
        }
        // Slim discovery request: starts with 'e', then NUL-separated field names.
        if (buf[0] != 'e')
            continue;

        // Build the TLV response. IPAD = our best local address toward the peer
        //   (the app already knows the source IP from the packet header, but LMS
        //   includes it and the app tolerates it).
        char ip[INET_ADDRSTRLEN] = "?";
        struct sockaddr_in local {};
        socklen_t llen = sizeof(local);
        if (::getsockname(fd_, (struct sockaddr *)&local, &llen) == 0)
            ::inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));

        std::string resp;
        resp += 'E'; // discovery response magic
        append_tlv(resp, "IPAD", ip);
        append_tlv(resp, "NAME", display_name_);
        append_tlv(resp, "JSON", std::to_string(http_port_));
        append_tlv(resp, "VERS", "8.4.0");
        append_tlv(resp, "UUID", uuid_);

        ::sendto(fd_, resp.data(), resp.size(), 0, (struct sockaddr *)&peer, plen);
    }
    LOG("[DISCOVERY] responder stopped");
}

} // namespace panicast
