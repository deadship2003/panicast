// EventBus — lightweight type-safe publish/subscribe bus (synchronous dispatch).
//   Thread-safe. subscribe<E>(handler) returns a token; unsubscribe(token) removes it.
//   publish<E>(evt) dispatches to all current E subscribers synchronously, on the
//   caller's thread (so handlers must be reentrant/thread-safe if published cross-thread).
//
//   This is the structural cure for the ad-hoc `pending_select_` + scattered-callback
//   signaling (AUDIT P1-4/P1-5/P1-8). Synchronous publish covers same-thread and
//   thread-safe-subscriber cases (e.g. EventLog). Cross-thread→UI delivery (replacing
//   pending_select_) will use post()/drain(), added when that migration is undertaken.
//
//   Header-only (template subscribe/publish); no .cpp needed.
#pragma once

#include <algorithm>
#include <functional>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace panicast
{

class EventBus {
public:
    static EventBus &instance() {
        static EventBus b;
        return b;
    }

    // Subscribe a handler to events of type E. Returns a token for unsubscribe().
    // E must be deducible/visible at the call site. Handler is invoked synchronously
    // on the thread that calls publish<E>().
    template <class E, class Fn>
    std::size_t subscribe(Fn &&handler) {
        std::function<void(const E &)> h(std::forward<Fn>(handler));
        std::lock_guard<std::mutex> lock(mtx_);
        const std::size_t id = next_id_++;
        subs_[std::type_index(typeid(E))].push_back(
            Handler{id, [h = std::move(h)](const void *p) { h(*static_cast<const E *>(p)); }});
        return id;
    }

    // Remove a subscription by its token. O(total handlers); called rarely.
    void unsubscribe(std::size_t id) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto &kv : subs_) {
            auto &vec = kv.second;
            auto it = std::remove_if(vec.begin(), vec.end(),
                                     [id](const Handler &h) { return h.id == id; });
            if (it != vec.end()) {
                vec.erase(it, vec.end());
                return;
            }
        }
    }

    // Synchronously dispatch evt to all current subscribers of E, on this thread.
    // Subscribers are snapshotted under the lock, then invoked outside it (a handler
    // may publish/unsubscribe without self-deadlock). Throws in a handler propagate.
    template <class E>
    void publish(const E &evt) {
        std::vector<Handler> copy;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            auto it = subs_.find(std::type_index(typeid(E)));
            if (it != subs_.end()) copy = it->second;
        }
        for (const auto &h : copy) h.fn(&evt);
    }

    EventBus(const EventBus &) = delete;
    EventBus &operator=(const EventBus &) = delete;

private:
    struct Handler {
        std::size_t id = 0;
        std::function<void(const void *)> fn;
    };

    EventBus() = default;

    std::mutex mtx_;
    std::unordered_map<std::type_index, std::vector<Handler>> subs_;
    std::size_t next_id_ = 0;
};

}  // namespace panicast
