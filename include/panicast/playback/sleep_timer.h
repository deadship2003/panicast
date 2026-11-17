// Sleep timer: singleton; supports setting duration, checking expiry, and querying remaining time.
//   Supports multiple formats: "5h", "30m", "1:25:15", "90" (pure number: <100 treated as hours, >=100 as minutes).
#pragma once

#include <chrono>
#include <mutex>
#include <string>

namespace panicast
{

class SleepTimer {
public:
    static SleepTimer &instance();

    void set_duration(int seconds);
    // Supports multiple formats: "5h", "30m", "1:25:15", "90"
    void set_duration(const std::string &time_str);

    bool check_expired();
    int remaining_seconds();

    bool is_active() const;
    void cancel();

    // Static parse function, used by main
    static int parse_time_string(const std::string &time_str);

private:
    SleepTimer() : duration_seconds_(0), active_(false) {}
    mutable std::mutex mtx_; // Multi-core concurrency guard (mutable so const is_active can lock)
    int duration_seconds_;
    std::chrono::steady_clock::time_point start_time_;
    bool active_;
};

} // namespace panicast
