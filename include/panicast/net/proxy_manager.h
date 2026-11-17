// IProxyManager — Connectivity layer: the single network/proxy frontend for ALL network
// consumers (Parser, Downloader, subtitle fetch, ASR/TTS/LLM cloud). mpv playback stays
// direct — its stream URLs are usually CDN addresses and bypassing the proxy is faster.
//
// resolveProxy(url, platform) applies a rule chain:
//   platform-specific → domain-specific → global → direct (empty)
// This is the seam that replaces every consumer reading `[network] proxy` directly, and
// enables future per-platform / per-domain proxy routing (e.g. youtube→proxy, bilibili→direct,
// *.googlevideo.com→proxy) without touching call sites.
//
// The global layer is an injectable source (setGlobalSource) so this file has NO dependency on
// the config system — production wires it to [network] proxy (network.cpp), unit tests set a
// fixed value. (D2 — new-arch M0.)
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace panicast
{

// A resolved proxy. `url` is the normalized proxy URL that curl/yt-dlp accept
// (e.g. "socks5h://1.2.3.4:1080", "http://host:8080"); empty means direct (no proxy).
struct ProxyConfig {
    std::string url;
    bool enabled() const { return !url.empty(); }
};

class IProxyManager {
public:
    virtual ~IProxyManager() = default;
    // Resolve the proxy for a request URL. `platform` is an optional id ("youtube",
    // "bilibili", "podcast", ...) selecting a platform-specific rule.
    virtual ProxyConfig resolveProxy(const std::string &url,
                                     const std::string &platform = "") const = 0;
    virtual void setPlatform(const std::string &platform, const ProxyConfig &c) = 0;
    virtual void setDomain(const std::string &domain_glob, const ProxyConfig &c) = 0;
    virtual void clearPlatform(const std::string &platform) = 0;
    virtual void clearDomain(const std::string &domain_glob) = 0;
};

// Default implementation. Constructible (for unit tests with isolated state) plus a
// process-wide instance() for the app. Thread-safe (resolveProxy is called from worker
// threads via apply_network_proxy).
class ProxyManager : public IProxyManager {
public:
    ProxyManager() = default;

    static ProxyManager &instance();

    ProxyConfig resolveProxy(const std::string &url,
                             const std::string &platform = "") const override;
    void setPlatform(const std::string &platform, const ProxyConfig &c) override;
    void setDomain(const std::string &domain_glob, const ProxyConfig &c) override;
    void clearPlatform(const std::string &platform) override;
    void clearDomain(const std::string &domain_glob) override;

    // Set the global-proxy source (the chain's global layer). Production wires this to
    // [network] proxy so Ctrl/N stays live; tests set a fixed value. Unset → global layer
    // returns direct.
    void setGlobalSource(std::function<ProxyConfig()> src);

private:
    // Extract the host portion of a request URL ("scheme://host[:port]/..." → "host").
    static std::string host_of(const std::string &url);
    // Glob match: "*.example.com" matches "a.b.example.com" and "example.com" itself.
    static bool domain_matches(const std::string &host, const std::string &glob);

    mutable std::mutex mtx_;
    std::map<std::string, ProxyConfig> platform_;
    std::map<std::string, ProxyConfig> domain_;
    std::function<ProxyConfig()> global_source_;
};

}  // namespace panicast
