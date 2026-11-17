// App persistence lifecycle — extracted implementation unit (D23 god-object split).
//   Startup/exit DB persistence: load_data / load_persistent_data /
//   save_persistent_data / restore_player_state. They remain App members
//   (declarations in app.h:119,341-343); only their implementations live here.
//   Mechanical verbatim move from app_run.cpp.
#include "panicast/app/app.h"

#include <iostream>

namespace panicast
{

// Public method for loading persistent data in command-line mode
void App::load_data() {
    Persistence::load_cache(library_.radio_root(), library_.podcast_root());
    load_persistent_data();
    // Load region preference from INI at startup
    OnlineState::instance().load_region_from_config();
    std::cout << "Loaded " << library_.podcast_root().size() << " podcasts from cache" << std::endl;
}

// D11-3c: load_history_to_root relocated to LibraryService (the history_root_ owner).

void App::load_persistent_data() {
    std::vector<TreeNodePtr> podcasts, favs;
    Persistence::load_data(podcasts, favs);
    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
    if (!library_.podcast_loaded()) {
        for (auto &n : podcasts) {
            n->parent.reset(); // set parent pointer
            library_.podcast_root().push_back(n);
        }
        library_.podcast_loaded() = true;
    }
    for (auto &n : favs) {
        n->parent.reset(); // set parent pointer
        library_.fav_root().push_back(n);
    }
}

void App::save_persistent_data() {
    std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
    Persistence::save_data(library_.podcast_root(), library_.fav_root());
}

// Restore last playback state
// Extended to restore UI state
void App::restore_player_state() {
    auto saved_state = DatabaseManager::instance().load_player_state();

    // Restore volume
    if (saved_state.volume >= 0 && saved_state.volume <= 100) {
        player.set_volume(saved_state.volume);
        EVENT_LOG(fmt::format("Restored volume: {}%", saved_state.volume));
    }

    // Restore playback speed
    if (saved_state.speed >= 0.5 && saved_state.speed <= 2.0) {
        player.set_speed(saved_state.speed);
        EVENT_LOG(fmt::format("Restored speed: {:.2f}x", saved_state.speed));
    }

    // Restore playback position (if there is a media that was playing)
    if (!saved_state.current_url.empty() && saved_state.position > 0) {
        // Try to restore playback
        // Note: do not auto-start playback; only restore position info
        EVENT_LOG(fmt::format("Last played: {} (position: {:.1f}s)", saved_state.current_url,
                              saved_state.position));

        // Save to the progress table so playback can resume from this position next time
        DatabaseManager::instance().save_progress(saved_state.current_url, saved_state.position,
                                                  false);
    }

    // Restore UI state
    frontend_->set_scroll_mode(saved_state.scroll_mode);
    frontend_->set_show_tree_lines(saved_state.show_tree_lines);
    EVENT_LOG(fmt::format("Restored UI: scroll_mode={}, tree_lines={}",
                          saved_state.scroll_mode ? "ON" : "OFF",
                          saved_state.show_tree_lines ? "ON" : "OFF"));

    // Restore mode
    // Upper bound validated against the enum element count, to avoid hardcoded 4 breaking after enum extension
    constexpr int APP_MODE_COUNT = 8; // Y24.27: 8 modes (was 7, TIKTOK added in Y24.11)
    if (saved_state.current_mode >= 0 && saved_state.current_mode < APP_MODE_COUNT) {
        mode = static_cast<AppMode>(saved_state.current_mode);
        switch_mode(mode); // Y24.27: delegate to unified switch_mode
        EVENT_LOG(fmt::format("Restored mode: {}", static_cast<int>(mode)));
    }

    // Restore the last played title
    if (!saved_state.current_title.empty()) {
        EVENT_LOG(fmt::format("Last played: {}", saved_state.current_title));
    }
}
} // namespace panicast
