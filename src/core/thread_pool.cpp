#include "panicast/core/thread_pool.h"

#include <fmt/format.h>

#include "panicast/core/logger.h"

namespace panicast
{

ThreadPool::ThreadPool(size_t n) : stop_(false) {
    for (size_t i = 0; i < n; ++i)
        workers_.emplace_back([this] { worker_loop(); });
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    // Fire-and-forget: DETACH workers (was: join). A worker stuck in a long curl/yt-dlp call
    //   would block join indefinitely → panicast hangs on exit → no terminal restore → no prompt.
    //   The caller _exit()s after ui.cleanup(), so the detached workers die with the process.
    for (auto &t : workers_)
        if (t.joinable())
            t.detach();
}

void ThreadPool::wait_idle() {
    // P2-C8: also wake when stop_ is set, otherwise shutdown during wait_idle deadlocks.
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return stop_ || (tasks_.empty() && active_ == 0); });
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty())
                return;
            task = std::move(tasks_.front());
            tasks_.pop();
            ++active_;
        }
        try {
            task();
        } catch (const std::exception &e) {
            LOG(fmt::format("[ThreadPool] task threw: {}",
                            e.what())); // Log instead of silently swallowing
        } catch (...) {
            LOG("[ThreadPool] task threw unknown exception");
        }
        {
            std::lock_guard<std::mutex> lock(mtx_);
            --active_;
        }
        cv_.notify_all(); // Wake up wait_idle waiters
    }
}

} // namespace panicast
