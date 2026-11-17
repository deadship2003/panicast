// NullFrontend — the headless IFrontend implementation for `panicast -d` (N09/S1).
//   The daemon runs the full engine (playback, queue, downloads, transcription, the
//   LMS/PRP remote-control servers) with NO terminal: every render/dialog call is a
//   no-op and state-bearing queries return the same defaults UI starts with. This is
//   the D12-3b "swappable renderer" contract used as designed — App never knows there
//   is no screen.
//
// Quit semantics: confirm_box() returns false (nothing to confirm without a display);
//   daemon exit paths are the termination signals (SIGTERM from `systemctl stop` /
//   `panicast stop` → the existing flush-and-exit path in check_exit_requests) and the
//   sleep timer, both of which bypass the interactive confirm.
#pragma once

#include <string>
#include <vector>

#include "panicast/ui/frontend.h"

namespace panicast
{

class NullFrontend : public IFrontend {
public:
    // ── lifecycle ──
    void init(float ratio = 0.4f) override { (void)ratio; }
    void cleanup() override {}
    void handle_resize() override {}

    // ── main render entry ── (no screen — nothing to draw)
    void draw(AppMode, const std::vector<DisplayItem> &, int, const MPVController::State &, int,
              AppState, int, const std::string &, int, int, TreeNodePtr,
              const std::vector<DownloadProgress> &, bool, int,
              const std::vector<PlaylistItem> & = {}, int = -1, PlayMode = PlayMode::CYCLE,
              const std::vector<std::string> & = {}, const std::vector<int> & = {},
              const DisplayContext & = {}) override {}

    // ── input / popups / dialogs ──
    std::string input_box(const std::string &prompt, const std::string &default_val = "",
                          bool prefill = false) override {
        (void)prompt;
        (void)prefill;
        return default_val; // remote-driven flows get the default (usually cancel-ish)
    }
    std::string dialog(const std::string &msg) override {
        (void)msg;
        return "";
    }
    bool confirm_box(const std::string &prompt = "Quit?") override {
        (void)prompt;
        return false; // no interactive confirm in a daemon; signals still exit cleanly
    }
    void show_pin_popup(const std::string &dynamic_pin, const std::string &universal_pin) override {
        (void)dynamic_pin;
        (void)universal_pin; // PIN pairing stays available via the log line remote_server emits
    }
    void show_help(const MPVController::State &state) override { (void)state; }

    // ── per-frame state push ──
    void update_lyric_history(const MPVController::State &state) override { (void)state; }
    void set_transcript(const std::vector<TranscriptSegment> &segs, const std::string &url) override {
        (void)segs;
        (void)url;
    }
    void set_lyric_bar_active(bool active) override { lyric_bar_active_ = active; }

    // ── toggles + queries (defaults mirror UI's startup state) ──
    void toggle_lyric_bar() override { lyric_bar_requested_ = !lyric_bar_requested_; }
    bool is_lyric_bar_requested() const override { return lyric_bar_requested_; }
    void set_lyric_bar_requested(bool requested) override { lyric_bar_requested_ = requested; }
    bool is_lyric_bar_active() const override { return lyric_bar_active_; }
    LyricManual lyric_manual() const override { return lyric_manual_; }
    void set_lyric_manual(LyricManual m) override { lyric_manual_ = m; }
    void toggle_scroll_mode() override { scroll_mode_ = !scroll_mode_; }
    void set_scroll_mode(bool mode) override { scroll_mode_ = mode; }
    bool is_scroll_mode() const override { return scroll_mode_; }
    void set_show_tree_lines(bool show) override { show_tree_lines_ = show; }
    bool is_show_tree_lines() const override { return show_tree_lines_; }
    bool embedded_sub_confirmed() const override { return false; }
    void toggle_theme() override {}

    // ── geometry ── (no screen → zero-sized; only used for mouse hit testing)
    int get_left_w() const override { return 0; }
    int get_top_h() const override { return 0; }

private:
    bool lyric_bar_requested_ = false;
    bool lyric_bar_active_ = false;
    LyricManual lyric_manual_ = LyricManual::Auto;
    bool scroll_mode_ = false;
    bool show_tree_lines_ = true;
};

} // namespace panicast
