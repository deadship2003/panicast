// Event log: singleton EventLog, a stream of recent events shown in the UI right panel.
#pragma once

#include <deque>
#include <mutex>
#include <string>

#include "panicast/core/event_bus.h"

namespace panicast
{

constexpr int MAX_EVENT_LOG_ENTRIES = 1024;

struct LogEntry {
    std::string timestamp;
    std::string message;
};

// D1: an event published on EventBus for every log line. EventLog::push() is the
// producer; future subscribers (remote log push, debug overlay) subscribe to LogEvent
// instead of polling EventLog. Carries the raw message; subscribers format/timestamp.
struct LogEvent {
    std::string msg;
};

class EventLog {
public:
    static EventLog &instance();

    void push(const std::string &msg);
    // Returns a copy (copied while holding the lock), to avoid returning an internal reference
    //   and then unlocking causing use-after-free — another thread's push_front/pop_back can invalidate the reference.
    std::deque<LogEntry> get();
    size_t size();

private:
    EventLog() {}
    std::deque<LogEntry> logs_;
    std::mutex mtx_;
};

} // namespace panicast

#define EVENT_LOG(msg) ::panicast::EventLog::instance().push(msg)
