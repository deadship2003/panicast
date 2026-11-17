#include "panicast/net/proxy_manager.h"

namespace panicast
{

ProxyManager &ProxyManager::instance() {
    static ProxyManager pm;
    return pm;
}

std::string ProxyManager::host_of(const std::string &url) {
    auto scheme_end = url.find("://");
    std::size_t start = (scheme_end == std::string::npos) ? 0 : scheme_end + 3;
    if (start >= url.size()) return "";
    std::size_t end = url.find_first_of(":/", start);
    if (end == std::string::npos) end = url.size();
    return url.substr(start, end - start);
}

bool ProxyManager::domain_matches(const std::string &host, const std::string &glob) {
    auto ends_with = [](const std::string &s, const std::string &suf) {
        return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };
    if (glob.rfind("*.", 0) == 0) {
        const std::string suffix = glob.substr(2);
        if (suffix.empty()) return false;
        return host == suffix || ends_with(host, "." + suffix);
    }
    return host == glob;
}

ProxyConfig ProxyManager::resolveProxy(const std::string &url, const std::string &platform) const {
    // 1. platform-specific rule (e.g. "youtube" → proxy, "bilibili" → direct)
    if (!platform.empty()) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = platform_.find(platform);
        if (it != platform_.end()) return it->second;
    }
    // 2. domain-specific rule (e.g. "*.googlevideo.com" → proxy)
    const std::string host = host_of(url);
    if (!host.empty()) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto &kv : domain_) {
            if (domain_matches(host, kv.first)) return kv.second;
        }
    }
    // 3. global source (production: [network] proxy via Ctrl+N; tests: fixed value); else direct
    std::function<ProxyConfig()> src;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        src = global_source_;
    }
    if (src) return src();
    return ProxyConfig{};
}

void ProxyManager::setPlatform(const std::string &platform, const ProxyConfig &c) {
    std::lock_guard<std::mutex> lock(mtx_);
    platform_[platform] = c;
}
void ProxyManager::setDomain(const std::string &domain_glob, const ProxyConfig &c) {
    std::lock_guard<std::mutex> lock(mtx_);
    domain_[domain_glob] = c;
}
void ProxyManager::clearPlatform(const std::string &platform) {
    std::lock_guard<std::mutex> lock(mtx_);
    platform_.erase(platform);
}
void ProxyManager::clearDomain(const std::string &domain_glob) {
    std::lock_guard<std::mutex> lock(mtx_);
    domain_.erase(domain_glob);
}
void ProxyManager::setGlobalSource(std::function<ProxyConfig()> src) {
    std::lock_guard<std::mutex> lock(mtx_);
    global_source_ = std::move(src);
}

}  // namespace panicast
