// Thread pool: a fixed number of workers consume a task queue; supports wait_idle to wait for idle.
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace panicast
{

class ThreadPool {
public:
    explicit ThreadPool(size_t n = 2);
    ~ThreadPool();

    template <class F> void submit(F &&task) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stop_)
                return; // Do not accept new tasks after shutdown (avoids lost tasks and pointless queuing)
            // P2-C9: the queue is intentionally unbounded and submit is non-blocking. A blocking
            //   producer would deadlock when a worker itself submits (on_playback_ended does), and
            //   dropping tasks would lose user-initiated loads. The pool is sized to
            //   MAX_CONCURRENT_DOWNLOADS and tasks are driven by discrete user actions, so in
            //   practice the queue stays small; this is the documented contract.
            tasks_.emplace(std::forward<F>(task));
        }
        cv_.notify_one();
    }

    void shutdown();
    // Wait for all submitted tasks to complete, but do not stop the pool (it can still accept new tasks).
    // Used by CLI import/export to join background loading before serializing the tree, avoiding torn reads/crashes.
    void wait_idle();

private:
    void worker_loop();
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_;
    int active_ =
        0; // Number of tasks currently executing, used by wait_idle to determine true idle
};

} // namespace panicast
