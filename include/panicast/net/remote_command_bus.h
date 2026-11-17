// Remote command bus: thread-safe queue bridging the network server thread and the TUI main loop.
//   The UI (ncurses) is NOT thread-safe — all control actions must execute on the TUI main thread.
//   So the network server never calls App/MPVController methods directly; instead it pushes a
//   RemoteCommand here, and App::run() drains the queue once per frame and dispatches on the UI
//   thread. This is the single sanctioned crossing point between the network and UI threads.
//
//   action: a stable, descriptive verb naming the local-terminal operation to replicate
//           (e.g. "play_pause", "nav_down", "switch_mode", "seek"). No internal codenames.
//   args:   optional string arguments (mode name, seek seconds, search query, ...).
//   client_id: which remote client issued the command (logging / per-client rate limits).
//
// Concurrency: mutex-protected vector, single consumer (the main loop). push() is non-blocking
//   and a no-op after shutdown(); drain_all() is non-blocking and swaps the queue out. No
//   condition_variable is needed — the main loop polls every frame at ~30 fps.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace panicast
{

struct RemoteCommand {
    std::string action;
    std::vector<std::string> args;
    int64_t client_id = 0;
};

class RemoteCommandBus {
public:
    // Called from the network server thread(s). No-op after shutdown().
    void push(RemoteCommand cmd);

    // Called once per frame from the TUI main thread. Non-blocking; moves out all pending
    //   commands. Returns an empty vector when nothing is pending or after shutdown.
    std::vector<RemoteCommand> drain_all();

    void shutdown() {
        shutdown_.store(true);
    }
    bool is_shutdown() const {
        return shutdown_.load();
    }

private:
    std::mutex mtx_;
    std::vector<RemoteCommand> queue_;
    std::atomic<bool> shutdown_{false};
};

} // namespace panicast
