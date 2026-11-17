// Remote command bus implementation. See remote_command_bus.h for the threading contract.
#include "panicast/net/remote_command_bus.h"

#include <utility>

namespace panicast
{

void RemoteCommandBus::push(RemoteCommand cmd) {
    if (shutdown_.load())
        return; // dropping commands after shutdown is intended
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back(std::move(cmd));
    }
}

std::vector<RemoteCommand> RemoteCommandBus::drain_all() {
    std::vector<RemoteCommand> out;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        out.swap(queue_); // O(1) move; queue_ left empty
    }
    return out;
}

} // namespace panicast
