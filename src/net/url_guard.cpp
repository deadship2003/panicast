#include "panicast/net/url_guard.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <fmt/format.h>

#include "panicast/core/logger.h"

namespace panicast
{

bool UrlGuard::is_safe_url(const std::string &url) {
    if (url.size() < 8)
        return false;
    size_t host_start, host_end;
    if (url.compare(0, 8, "https://") == 0)
        host_start = 8;
    else if (url.compare(0, 7, "http://") == 0)
        host_start = 7;
    else
        return false;

    // Extract host (including possible [IPv6]), up to : / / ? #
    host_end = url.size();
    for (size_t i = host_start; i < url.size(); ++i) {
        char c = url[i];
        if (c == '/' || c == '?' || c == '#') {
            host_end = i;
            break;
        }
    }
    // host:port separation (but [IPv6]:port needs special handling)
    std::string host = url.substr(host_start, host_end - host_start);
    if (host.empty())
        return false; // No hostname
    if (host[0] == '[') {
        // IPv6 literal such as [::1]
        size_t rb = host.find(']');
        if (rb == std::string::npos)
            return false;
        std::string v6 = host.substr(1, rb - 1);
        if (is_private_ipv6(v6))
            return false;
    } else {
        size_t colon = host.find(':');
        std::string h = (colon != std::string::npos) ? host.substr(0, colon) : host;
        if (h == "localhost")
            return false;
        if (is_private_ipv4_text(h))
            return false;
    }
    return true;
}

bool UrlGuard::is_private_ipv4_text(const std::string &s) {
    // P2 (Y23.7): use inet_pton for proper dotted-decimal validation. Non-dotted-decimal forms
    //   (decimal int 2852039166, octal 0177.0.0.1, hex 0x7f.0.0.1) are rejected as unsafe —
    //   curl may resolve them to private IPs, bypassing the guard.
    struct in_addr addr;
    if (inet_pton(AF_INET, s.c_str(), &addr) == 1) {
        // Valid dotted-decimal — check if private.
        unsigned int ip = ntohl(addr.s_addr);
        unsigned int a = (ip >> 24) & 0xFF, b = (ip >> 16) & 0xFF;
        if (a == 10 || a == 127 || a == 0)
            return true; // 10/8, 127/8, 0/8
        if (a == 169 && b == 254)
            return true; // 169.254/16 (metadata)
        if (a == 172 && b >= 16 && b <= 31)
            return true; // 172.16/12
        if (a == 192 && b == 168)
            return true; // 192.168/16
        if (a == 100 && b >= 64 && b <= 127)
            return true; // 100.64/10 (CGN)
        return false;    // public dotted-decimal IP
    }
    // Not dotted-decimal. If it looks like an IP in alternative notation (all hex/numeric/dots),
    // treat as UNSAFE — curl may resolve it to a private IP.
    bool looks_like_alt_ip = !s.empty();
    for (char c : s) {
        if (!std::isxdigit((unsigned char)c) && c != '.' && c != 'x' && c != 'X') {
            looks_like_alt_ip = false;
            break;
        }
    }
    return looks_like_alt_ip; // true = unsafe (reject); false = hostname (safe)
}

bool UrlGuard::is_private_ipv6(const std::string &s) {
    // P2 (Y23.7): use inet_pton + IN6_IS_ADDR_* macros for robust IPv6 checking (was prefix
    //   string-matching, which missed expanded forms like 0:0:0:0:0:0:0:1).
    struct in6_addr addr6;
    if (inet_pton(AF_INET6, s.c_str(), &addr6) == 1) {
        if (IN6_IS_ADDR_LOOPBACK(&addr6))
            return true;
        if (IN6_IS_ADDR_LINKLOCAL(&addr6))
            return true;
        if (IN6_IS_ADDR_SITELOCAL(&addr6))
            return true;
        if (IN6_IS_ADDR_V4MAPPED(&addr6)) {
            // IPv4-mapped — check the embedded v4.
            const uint8_t *v4 = addr6.s6_addr + 12;
            if (v4[0] == 10 || v4[0] == 127 || v4[0] == 0)
                return true;
            if (v4[0] == 169 && v4[1] == 254)
                return true;
            if (v4[0] == 172 && v4[1] >= 16 && v4[1] <= 31)
                return true;
            if (v4[0] == 192 && v4[1] == 168)
                return true;
        }
        // ULA fc00::/7
        if ((addr6.s6_addr[0] & 0xFE) == 0xFC)
            return true;
        return false;
    }
    return false; // not a valid IPv6 literal — treat as hostname
}

bool UrlGuard::reject(const std::string &url, const std::string &ctx) {
    if (!is_safe_url(url)) {
        LOG(fmt::format("[UrlGuard] Rejected unsafe URL ({}): {}", ctx, url));
        return true;
    }
    return false;
}

} // namespace panicast
