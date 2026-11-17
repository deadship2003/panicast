// D12-3b: IFrontend — the abstract frontend contract (ncurses-free).
//   The ncurses UI implements it (class UI : public IFrontend); App owns the UI through this
//   interface so the renderer is swappable (a Qt/other frontend could implement the same contract).
//   This header owns the ncurses-free VIEW-MODEL types the contract speaks in (DisplayItem /
//   DisplayContext) plus the LyricManual enum, so neither the interface nor its callers need to
//   include <ncurses.h>. Dependency direction: frontend.h (no ncurses) ← ui.h (ncurses).
//
//   The method set is exactly what App + the subtitle Application Service call on the frontend
//   (render entry, input/dialogs, per-frame state push, lyric/scroll/tree-line toggles + queries,
//   geometry). UI's private rendering helpers (draw_line/draw_status/draw_lyric_* — which take
//   WINDOW*) stay concrete on UI and are NOT part of the contract. is_input_cancelled is a
//   static ncurses-input-contract utility (the CANCELLED marker) and stays on UI, not here.
#pragma once

#include <string>
#include <vector>

#include "panicast/core/types.h"               // AppMode, AppState, PlayMode, TreeNodePtr, PlaylistItem
#include "panicast/playback/mpv_controller.h"  // MPVController::State
#include "panicast/app/progress.h"             // DownloadProgress
#include "panicast/subtitle/subtitle_parser.h" // TranscriptSegment

namespace panicast
{

// Display entry the renderer renders a tree row from (index into App/LibraryService display_list).
struct DisplayItem {
    TreeNodePtr node;
    int depth;
    bool is_last;
    int parent_idx;
};

// D12-1: ambient runtime display state pushed IN by App each frame. The UI must NOT query these
//   singletons itself — they hold runtime/business state (sleep timer, selected regions), which is
//   exactly the "UI queries runtime state instead of receiving a view-model" coupling D11-4 flagged
//   as the D12 frontier (see docs/ARCHITECTURE.md §2.1). App reads them once per frame and resolves
//   region codes to display names, so the UI only ever renders plain values. Pure functions
//   (Utils::*, URLClassifier::classify) remain direct calls — they are stateless cross-cutting infra.
struct DisplayContext {
    bool sleep_active = false;
    int sleep_remaining = 0;        // seconds (valid only when sleep_active)
    std::string online_region_name; // resolved name for the ONLINE title
    std::string tiktok_region;      // current TikTok region code (e.g. "CN")
    std::string now_playing_url;    // D14-3: canonical now-playing source URL (the real absolute
                                    //   source URL; NOT state.current_url, which holds the played
                                    //   mpv/cache path). Read-side converges on this identity.
    std::string now_playing_title;  // D15: now-playing display title (from
                                    //   PlaybackService::now_playing()). Pairs with now_playing_url so
                                    //   the UI renders now-playing identity from this view-model bag
                                    //   instead of a domain TreeNodePtr (see draw() contract).
};

// Y24.48: per-track manual LYRIC override (L key). Auto = follow auto-detection (open only when a
//   displayable subtitle source exists); Open = user opened (show even before content, e.g. ASR
//   startup); Closed = user closed (suppress auto-open until track change).
enum class LyricManual { Auto, Open, Closed };

// The swappable frontend contract. App and the subtitle Application Service program against this
//   interface; the ncurses UI (class UI) is the current implementation.
class IFrontend {
public:
    virtual ~IFrontend() = default;

    // ── lifecycle ──
    virtual void init(float ratio = 0.4f) = 0;
    virtual void cleanup() = 0;
    virtual void handle_resize() = 0;

    // ── main render entry ──
    // D15: now-playing identity is no longer passed as a domain TreeNodePtr — the UI reads it
    //   from DisplayContext (now_playing_url + now_playing_title), so the render contract speaks
    //   only view-model types for the playing track. selected_node stays (it renders the
    //   cursor-node block, which legitimately needs node fields).
    virtual void draw(AppMode mode, const std::vector<DisplayItem> &list, int selected,
                      const MPVController::State &state, int view_start, AppState app_state,
                      int marked_count, const std::string &search_query,
                      int current_match, int total_matches, TreeNodePtr selected_node,
                      const std::vector<DownloadProgress> &downloads, bool visual_mode,
                      int visual_start, const std::vector<PlaylistItem> &playlist = {},
                      int playlist_index = -1,
                      // Play mode + INFO play-context (7-line: 3 history + current + 3 next)
                      PlayMode play_mode = PlayMode::CYCLE,
                      const std::vector<std::string> &history_titles = {},
                      const std::vector<int> &next_indices = {},
                      // D12-1: ambient runtime display state (sleep timer + regions).
                      const DisplayContext &dctx = {}) = 0;

    // ── input / popups / dialogs ──
    virtual std::string input_box(const std::string &prompt, const std::string &default_val = "",
                                  bool prefill = false) = 0;
    virtual std::string dialog(const std::string &msg) = 0;
    virtual bool confirm_box(const std::string &prompt = "Quit?") = 0;
    virtual void show_pin_popup(const std::string &dynamic_pin, const std::string &universal_pin) = 0;
    virtual void show_help(const MPVController::State &state) = 0;

    // ── per-frame state pushed in by App / the subtitle Application Service ──
    virtual void update_lyric_history(const MPVController::State &state) = 0;
    virtual void set_transcript(const std::vector<TranscriptSegment> &segs, const std::string &url) = 0;
    virtual void set_lyric_bar_active(bool active) = 0;

    // ── lyric-bar / scroll / tree-line toggles + queries ──
    virtual void toggle_lyric_bar() = 0;
    virtual bool is_lyric_bar_requested() const = 0;
    // LYRIC-fix (2026-08-15): the L key enables the INI master switch live (persisted) — pressing
    //   L is explicit intent and must not be a silent no-op when [display] lyric_bar is off.
    virtual void set_lyric_bar_requested(bool requested) = 0;
    virtual bool is_lyric_bar_active() const = 0;
    virtual LyricManual lyric_manual() const = 0;
    virtual void set_lyric_manual(LyricManual m) = 0;
    virtual void toggle_scroll_mode() = 0;
    virtual void set_scroll_mode(bool mode) = 0;
    virtual bool is_scroll_mode() const = 0;
    virtual void set_show_tree_lines(bool show) = 0;
    virtual bool is_show_tree_lines() const = 0;
    virtual bool embedded_sub_confirmed() const = 0;
    virtual void toggle_theme() = 0;

    // ── geometry (mouse-click hit testing) ──
    virtual int get_left_w() const = 0;
    virtual int get_top_h() const = 0;
};

} // namespace panicast
