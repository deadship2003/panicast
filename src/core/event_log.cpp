#include "panicast/core/event_log.h"

#include <chrono>
#include <ctime>
#include <fmt/format.h>

#include "panicast/core/logger.h"
#include "panicast/core/platform.h"

namespace panicast
{

EventLog &EventLog::instance() {
    static EventLog el;
    return el;
}

void EventLog::push(const std::string &msg) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    struct tm tm_local;
    struct tm *tmp = localtime_local(&t, &tm_local); // P2-C10: guard nullptr
    if (tmp)
        std::strftime(buf, sizeof(buf), "%H:%M:%S", tmp);
    else
        buf[0] = '\0';

    LogEntry entry;
    entry.timestamp = fmt::format("[{}.{:03d}]", buf, ms.count());
    entry.message = msg;

    logs_.push_front(entry);
    while (logs_.size() > MAX_EVENT_LOG_ENTRIES)
        logs_.pop_back();

    LOG(fmt::format("[{}.{:03d}] {}", buf, ms.count(), msg));

    // D1: also publish on the EventBus so future subscribers (remote log push, debug
    // overlay) receive log lines without polling EventLog. No-op when there are none.
    EventBus::instance().publish(LogEvent{msg});
}

std::deque<LogEntry> EventLog::get() {
    std::lock_guard<std::mutex> lock(mtx_);
    return logs_;
}

size_t EventLog::size() {
    std::lock_guard<std::mutex> lock(mtx_);
    return logs_.size();
}

} // namespace panicast
