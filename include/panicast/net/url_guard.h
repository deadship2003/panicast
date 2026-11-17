// URL safety guard: protocol whitelist + host validation, blocking SSRF (private network/loopback/link-local/cloud metadata endpoints).
#pragma once

#include <string>

namespace panicast
{

class UrlGuard {
public:
    // Adds host validation on top of the protocol whitelist, blocking access to private network/loopback/link-local addresses and cloud metadata endpoints
    // (169.254.169.254). Note: may still be bypassed by DNS rebinding — the current threat model is a single-user local application.
    static bool is_safe_url(const std::string &url);
    // Whether a dotted-decimal IPv4 text is a private network/loopback/link-local/metadata endpoint
    static bool is_private_ipv4_text(const std::string &s);
    static bool is_private_ipv6(const std::string &s);
    // Returns true when rejected (and logged); false means passed
    static bool reject(const std::string &url, const std::string &ctx = "");
};

} // namespace panicast
