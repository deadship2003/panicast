// UI rendering layer — extracted implementation unit (D21 god-object split).
//   View-state toggles/setters (tree-lines / transcript / scroll-mode / lyric-bar).
//   They remain UI members (they mutate private UI view state + emit EVENT_LOG / persist
//   to INI); only their implementations live here. Declarations stay in ui.h.
//   Mechanical verbatim move from ui.cpp.
#include "panicast/ui/ui.h"

#include <string>
#include <vector>

#include <fmt/format.h>

#include "panicast/config/ini_config.h"
#include "panicast/core/event_log.h"

namespace panicast
{

void UI::toggle_tree_lines() {
    show_tree_lines_ = !show_tree_lines_;
    EVENT_LOG(fmt::format("Tree lines: {}", show_tree_lines_ ? "ON" : "OFF"));
}

void UI::set_transcript(const std::vector<TranscriptSegment> &segs, const std::string &url) {
    current_transcript_ = segs;
    current_transcript_url_ = url;
    lyric_history_.clear(); // new track → fresh lyric history
}

void UI::toggle_scroll_mode() {
    scroll_mode_ = !scroll_mode_;
    EVENT_LOG(fmt::format("Scroll mode: {}", scroll_mode_ ? "ON" : "OFF"));
}

void UI::toggle_lyric_bar() {
    lyric_bar_requested_ = !lyric_bar_requested_;
    IniConfig::instance().set("display", "lyric_bar", lyric_bar_requested_ ? "true" : "false");
    EVENT_LOG(fmt::format("LYRIC bar: {}", lyric_bar_requested_ ? "requested" : "off"));
}

void UI::set_lyric_bar_requested(bool requested) {
    if (lyric_bar_requested_ == requested)
        return;
    lyric_bar_requested_ = requested;
    IniConfig::instance().set("display", "lyric_bar", requested ? "true" : "false");
    EVENT_LOG(fmt::format("LYRIC bar: {}", requested ? "requested" : "off"));
}
} // namespace panicast
