// Slim UDP discovery responder — see slim_discovery.cpp for the protocol contract.
#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace panicast
{

class SlimDiscovery {
public:
    static SlimDiscovery &instance();

    SlimDiscovery(const SlimDiscovery &) = delete;
    SlimDiscovery &operator=(const SlimDiscovery &) = delete;

    // Bind UDP :3483 and start responding to discovery broadcasts. `http_port` is
    //   the LMS HTTP port advertised in the JSON TLV field. Returns false when the
    //   port is taken (another instance on this host) — NOT fatal, the app finds
    //   us via manual IP entry.
    bool start(int http_port);
    void stop();

    bool is_running() const {
        return running_.load();
    }
    const std::string &display_name() const {
        return display_name_;
    }
    const std::string &uuid() const {
        return uuid_;
    }

private:
    SlimDiscovery();
    ~SlimDiscovery();
    void loop();

    std::atomic<bool> running_{false};
    int fd_ = -1;
    int http_port_ = 0;
    std::string display_name_;
    std::string uuid_;
    std::thread thread_;
};

} // namespace panicast
