// Sleep timer implementation.
#include "panicast/playback/sleep_timer.h"

#include <cctype>
#include <sstream>
#include <vector>

#include <fmt/format.h>

#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"

namespace panicast
{

SleepTimer &SleepTimer::instance() {
    static SleepTimer st;
    return st;
}

void SleepTimer::set_duration(int seconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    duration_seconds_ = seconds;
    start_time_ = std::chrono::steady_clock::now();
    active_ = true;
    EVENT_LOG(fmt::format("Sleep timer set: {}s", seconds));
}

void SleepTimer::set_duration(const std::string &time_str) {
    int seconds = parse_time_string(time_str);
    if (seconds > 0)
        set_duration(seconds);
}

bool SleepTimer::check_expired() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!active_ || duration_seconds_ <= 0)
        return false;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    if (elapsed >= duration_seconds_) {
        active_ = false;
        EVENT_LOG("Sleep timer expired!");
        return true;
    }
    return false;
}

int SleepTimer::remaining_seconds() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!active_ || duration_seconds_ <= 0)
        return 0;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    int remaining = duration_seconds_ - elapsed;
    return remaining > 0 ? remaining : 0;
}

bool SleepTimer::is_active() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return active_;
}
void SleepTimer::cancel() {
    std::lock_guard<std::mutex> lk(mtx_);
    active_ = false;
    EVENT_LOG("Sleep timer cancelled");
}

int SleepTimer::parse_time_string(const std::string &time_str) {
    // Accumulate using long long and clamp, to prevent "999999h"/"999:99:99" from overflowing int (UB)
    constexpr long long MAX_TIMER_SEC = 365LL * 24 * 3600; // Upper limit 1 year
    auto clamp_ret = [](long long v) -> int {
        if (v <= 0)
            return 0;
        if (v > MAX_TIMER_SEC)
            v = MAX_TIMER_SEC;
        return (int)v;
    };
    long long seconds = 0;
    std::string s = time_str;

    // Remove possible leading minus sign (e.g. -5h)
    if (!s.empty() && s[0] == '-') {
        s = s.substr(1);
    }

    // Check suffix format: 5h, 30m, 90s
    if (s.size() >= 2) {
        char suffix = std::tolower(s.back());
        std::string num_part = s.substr(0, s.size() - 1);
        try {
            long long val = std::stoll(num_part);
            if (suffix == 'h')
                return clamp_ret(val * 3600);
            if (suffix == 'm')
                return clamp_ret(val * 60);
            if (suffix == 's')
                return clamp_ret(val);
        } catch (const std::exception &e) {
            LOG(fmt::format("[Exception] {}", e.what()));
        }
    }

    // Check HH:MM:SS format
    if (s.find(':') != std::string::npos) {
        std::vector<long long> parts;
        std::stringstream ss(s);
        std::string part;
        while (std::getline(ss, part, ':')) {
            try {
                parts.push_back(std::stoll(part));
            } catch (...) {
                parts.push_back(0);
            }
        }
        if (parts.size() >= 3)
            seconds = parts[0] * 3600 + parts[1] * 60 + parts[2];
        else if (parts.size() == 2)
            seconds = parts[0] * 60 + parts[1];
        else if (parts.size() == 1)
            seconds = parts[0];
        return clamp_ret(seconds);
    }

    // Pure number auto-detection logic
    // Rule: val < 100 -> hours, val >= 100 -> minutes
    try {
        long long val = std::stoll(s);
        if (val >= 100) {
            seconds = val * 60; // >=100 treated as minutes
        } else {
            seconds = val * 3600; // <100 treated as hours
        }
    } catch (const std::exception &e) {
        LOG(fmt::format("[Exception] {}", e.what()));
    }
    return clamp_ret(seconds);
}

} // namespace panicast
