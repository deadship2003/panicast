// Actions — UI → Core commands (the INPUT direction of the message bus). UI emits via
// publish_action (usually through the Keymap: key → Action → publish). App / Services subscribe
// to each action type + handle. This is the seam that lets the UI be a pure interaction layer
// (no direct Core calls) — see docs/DESIGN.md "target architecture".
// (D6 seed: PlayPause. D7: + Volume/Nav, + Keymap.)
#pragma once

#include <string>
#include <variant>

#include "panicast/core/event_bus.h"
#include "panicast/core/types.h" // AppMode (SwitchModeAction)

namespace panicast
{

struct PlayPauseAction {};
struct VolumeUpAction {};
struct VolumeDownAction {};
struct NavUpAction {};
struct NavDownAction {};
// D42: switch to a mode. `hint` is an optional one-line EVENT_LOG message shown after the
//   switch (empty = none). It is key-specific, not mode-specific — switch_mode() is also called
//   from remote commands / the M-cycle / programmatic paths where no hint is wanted, so the hint
//   travels with the key binding (the Action), not the mode switch itself.
struct SwitchModeAction {
    AppMode target;
    std::string hint;
};

// A key binding's target: one of the actions above. The Keymap maps a key (int) → Action.
using Action = std::variant<PlayPauseAction, VolumeUpAction, VolumeDownAction,
                            NavUpAction, NavDownAction, SwitchModeAction>;

// Publish whichever action the variant holds onto the EventBus (lets handle_input be a generic
// Keymap lookup instead of a per-key switch).
inline void publish_action(const Action &a) {
    std::visit([](const auto &x) { EventBus::instance().publish(x); }, a);
}

}  // namespace panicast
