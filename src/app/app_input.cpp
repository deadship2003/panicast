#include "panicast/app/app.h"
#include "panicast/app/actions.h"
#include "panicast/core/event_bus.h"

#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "panicast/net/tiktok_region.h" // Y24.11: T-mode region name in the T-key status line
#include "panicast/net/url_classifier.h" // D14-3b: is_local_file() for ASR streaming/local + offline-transcription gate

namespace panicast
{

// Y24.27: unified mode switch — single source of truth (was 4+ duplicated sites).
void App::switch_mode(AppMode new_mode) {
    mode = new_mode; // E: cur_items() derives from mode; no current_root to set.
    switch (new_mode) {
    case AppMode::RADIO:
        break;
    case AppMode::PODCAST:
        break;
    case AppMode::FAVOURITE:
        break;
    case AppMode::HISTORY:
        break;
    case AppMode::ONLINE:
        OnlineState::instance().load_search_history();
        break;
    case AppMode::ACCOUNT:
        library_.load_accounts_root();
        break;
    case AppMode::BILIBILI:
        library_.load_bilibili_root();
        break;
    case AppMode::TIKTOK:
        library_.load_tiktok_root();
        break;
    case AppMode::IPTV:
        library_.load_iptv_root();
        break;
    }
    reset_search();
    library_.selected_idx() = 0;
}

// Mouse event handling: left-click = select and expand/play node; wheel = page scroll
// Only responds to the left-panel content area; uses steady_clock debouncing to avoid CLICKED+PRESSED double triggers.
void App::handle_mouse_event() {
    MEVENT ev;
    if (getmouse(&ev) != OK)
        return;

    // Wheel paging
    if (ev.bstate & BUTTON4_PRESSED) {
        nav_page_up();
        return;
    }
#ifdef BUTTON5_PRESSED
    if (ev.bstate & BUTTON5_PRESSED) {
        nav_page_down();
        return;
    }
#endif

    // Left click: only respond to clicks in the left-panel content area
    if (!(ev.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)))
        return;
    int lw = frontend_->get_left_w();
    int th = frontend_->get_top_h();
    if (lw <= 0 || th <= 0)
        return;
    // Content row range: y in [1, top_h-2] (0 = border title, top_h-1 = bottom border)
    if (ev.y < 1 || ev.y >= th - 1)
        return;
    if (ev.x < 0 || ev.x >= lw)
        return;

    // Debounce: merge PRESSED+CLICKED within 250ms into a single activation
    static std::chrono::steady_clock::time_point last_click;
    auto now = std::chrono::steady_clock::now();
    if (now - last_click < std::chrono::milliseconds(250))
        return;
    last_click = now;

    int row = library_.view_start() + (ev.y - 1);
    if (row < 0 || row >= (int)library_.display_list().size())
        return;
    library_.selected_idx() = row;
    enter_node(0); // select and activate: expand folder / play episode
}

// Ctrl+N: open the network proxy configuration dialog. Format: socks5h://host:port / http://host:port etc.
// After normalization + validation, persists to [network] proxy. Scope: the app's network front ONLY
//   (curl downloads / yt-dlp resolves / parsers) — they resolve the proxy live per request via
//   ProxyManager, so an edit takes effect on the next request, no restart. mpv is deliberately
//   OUT of scope: it is playback-only and never gets a proxy (the network is the user's concern,
//   e.g. a transparent proxy).
// Default is no proxy ([network] proxy empty = direct/transparent proxy). The dialog preloads the current proxy value;
//   you can edit/delete/save: clear + Enter -> set("") (disable proxy, takes effect immediately), no restart needed.
void App::configure_proxy() {
    std::string cur =
        IniConfig::instance().get_proxy(); // normalized currently-effective proxy (empty = none)
    std::string input = frontend_->input_box("Proxy (empty = direct connection)", cur, /*prefill=*/true);
    if (UI::is_input_cancelled(input)) {
        EVENT_LOG("Proxy config cancelled");
        return;
    }

    if (input.empty()) {
        IniConfig::instance().set("network", "proxy", "");
        EVENT_LOG("Proxy disabled (direct)");
        return;
    }
    bool valid = false;
    std::string norm = IniConfig::normalize_proxy(input, &valid);
    if (!valid) {
        EVENT_LOG(fmt::format("Proxy invalid (not saved): {}", input));
        return;
    }
    IniConfig::instance().set("network", "proxy", norm);
    EVENT_LOG(fmt::format("Proxy set: {}", norm));
}

void App::configure_cookies() {
    // Y16/#5: Unified Ctrl+B — context-aware. Y mode → YouTube; B mode → Bilibili;
    //   T mode → TikTok/Douyin (Y24.13: CN region → douyin_cookies_file; else tiktok cookies_file).
    //   Same input_box UI, just targets the current mode's INI section + label.
    std::string section, key, label, def_name, cur;
    if (mode == AppMode::BILIBILI) {
        section = "bilibili";
        key = "cookies_file";
        label = "Bilibili";
        def_name = "bilibili_cookie.txt";
        cur = IniConfig::instance().get_bilibili_cookies_file();
    } else if (mode == AppMode::TIKTOK) {
        bool cn = (tiktok_region_ == "CN");
        section = "tiktok";
        if (cn) {
            key = "douyin_cookies_file";
            label = "Douyin";
            def_name = "douyin_cookie.txt";
            cur = IniConfig::instance().get_tiktok_douyin_cookies_file();
        } else {
            key = "cookies_file";
            label = "TikTok";
            def_name = "tiktok_cookie.txt";
            cur = IniConfig::instance().get_tiktok_cookies_file();
        }
    } else {
        section = "youtube";
        key = "cookies_file";
        label = "YouTube";
        def_name = "youtube_cookie.txt";
        cur = IniConfig::instance().get_youtube_cookies_file();
    }
    std::string input = frontend_->input_box(label + " cookies.txt path (bare/empty = <data_dir>/" +
                                         def_name + "; or absolute)",
                                     cur, /*prefill=*/true);
    if (UI::is_input_cancelled(input)) {
        EVENT_LOG("Cookies config cancelled");
        return;
    }
    IniConfig::instance().set(section, key, input);
    EVENT_LOG(fmt::format("{} cookies_file set: {}", label, input.empty() ? def_name : input));
}

// `:` command window. Two kinds of commands:
//   - Play mode (case-insensitive): r/s/c or repeat/shuffle/cycle → sets global play_mode.
//   - mpv interaction (case-SENSITIVE single char): forwarded to mpv via mpv_command_string.
//     Needed because terminal=no (F25) disables mpv terminal input and the wlshm video window
//     doesn't get keyboard focus on Wayland, so mpv's own keys (f/o/i/...) can't reach it.
//     The TUI owns input and forwards via the mpv API — the clean TUI+libmpv design.
void App::open_command_window() {
    std::string input =
        frontend_->input_box("Command (r/s/c; mpv hotkey — see ? for full list)", "", /*prefill=*/false);
    if (UI::is_input_cancelled(input)) {
        EVENT_LOG(": command cancelled");
        return;
    }

    // Trim (preserve case for mpv commands)
    std::string raw = input;
    raw.erase(0, raw.find_first_not_of(" \t\r\n"));
    raw.erase(raw.find_last_not_of(" \t\r\n") + 1);

    // mpv commands — case-sensitive single char, forwarded to mpv via mpv_command_string.
    //   Y11: aligned to mpv's NATIVE default input.conf bindings where a native binding exists
    //   (see man/?/README for the full table). Zoom (+/-/=) has no native plain key (mpv uses
    //   Alt++/Alt+-, un-capturable in the single-char `:` box) so plain +/- is the closest.
    static const std::map<std::string, std::string> mpv_cmds = {
        // video / zoom / aspect / deinterlace
        {"+", "add video-zoom 0.1"},  // zoom in  (no native plain key)
        {"-", "add video-zoom -0.1"}, // zoom out (no native plain key)
        {"=", "set video-zoom 0"},    // reset zoom
        {"f", "fullscreen"},          // toggle fullscreen      (native f)
        {"A", "cycle video-aspect"},  // cycle aspect ratio     (native A)
        {"d", "cycle deinterlace"},   // cycle deinterlace      (native d)
        // subtitle size / sync / position / visibility / track
        {"F", "add sub-scale -0.1"},   // shrink subtitles       (native F)
        {"G", "add sub-scale 0.1"},    // enlarge subtitles      (native G)
        {"z", "add sub-delay -0.1"},   // subtitles earlier      (native z)
        {"Z", "add sub-delay 0.1"},    // delay subtitles        (native Z)
        {"r", "add sub-pos -1"},       // subtitles up           (native r)
        {"R", "add sub-pos +1"},       // subtitles down         (native R)
        {"v", "cycle sub-visibility"}, // hide/show subtitles    (native v)
        {"j", "cycle sub"},            // next subtitle track    (native j)
        {"J", "cycle sub down"},       // prev subtitle track    (native J)
        // audio
        {"#", "cycle audio"}, // cycle audio track      (native #)
        {"m", "cycle mute"},  // mute                   (native m)
        // osd / stats
        {"o", "show-progress"},                             // show OSD progress      (native o)
        {"O", "cycle-values osd-level 3 1"},                // toggle OSD level       (native O)
        {"i", "script-binding stats/display-stats"},        // momentary stats        (native i)
        {"I", "script-binding stats/display-stats-toggle"}, // toggle persistent stats(native I)
        // loop / screenshot
        {"l", "ab-loop"},          // set/clear A-B loop     (native l)
        {"s", "screenshot"},       // screenshot w/ subtitles(native s)
        {"S", "screenshot video"}, // screenshot no subtitles(native S)
        // video EQ (native 1-8)
        {"1", "add contrast -1"},
        {"2", "add contrast 1"},
        {"3", "add brightness -1"},
        {"4", "add brightness 1"},
        {"5", "add gamma -1"},
        {"6", "add gamma 1"},
        {"7", "add saturation -1"},
        {"8", "add saturation 1"},
    };
    auto it = mpv_cmds.find(raw);
    if (it != mpv_cmds.end()) {
        mpv_handle *h = player.get_handle();
        if (h) {
            mpv_command_string(h, it->second.c_str());
            EVENT_LOG(fmt::format(": mpv → {}", it->second));
        } else {
            EVENT_LOG(": mpv not available");
        }
        return;
    }

    // Play mode — Y18: single char r/s/c conflicts with mpv hotkeys (r=sub-pos, s=screenshot).
    //   Use full word or prefix match: repeat/shuffle/cycle (or rep/shu/cyc prefix).
    std::string s = raw;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    // Y24.25: ":asr" = force ASR. D11-3a: this is the ONE entry point that intentionally does NOT
    //   call resolve_subtitle_source — it bypasses ALL local/online sources (embedded / local ASR
    //   SRT / online 📜) and goes straight to real-time transcription. L-key / remote asr_start do
    //   respect "local subtitle first"; :asr is the manual override when you want ASR regardless.
    if (s == "asr") {
        auto pst = player.get_state();
        if (pst.has_media && !subtitle_.transcription_engine().realtime_running()) {
            std::string url = pst.current_url;
            bool is_streaming = !URLClassifier::is_local_file(url);
            subtitle_.transcription_engine().start_realtime(playback_.playback_node(), url, is_streaming);
            EVENT_LOG("Force ASR (bypassing embedded/local-SRT/online sources)");
        } else {
            EVENT_LOG("ASR: nothing playing or already running");
        }
        return;
    }

    // N04: remote control PIN. ":pin" shows the current dynamic PIN in a popup (for pairing a
    //   phone/APK). The universal 6696 always works (headless pairing).
    //   N04-fix: ":newpin" removed — dynamic PIN is already random per restart, and rotating
    //   mid-session adds no security (6696 stays valid) while invalidating an APK's stored PIN.
    if (s == "pin") {
        if (remote_server_.is_running()) {
            frontend_->show_pin_popup(remote_server_.dynamic_pin(), remote_server_.universal_pin());
        } else {
            EVENT_LOG("Remote control is disabled ([remote] enable=false)");
        }
        return;
    }

    // ":secret" — import the user's Google OAuth client_secret*.json into <data_dir> so the
    //   runtime OAuth loader (google_oauth.cpp scans <data_dir>/client_secret*.json) uses it for
    //   Y-mode login, taking precedence over the build-time builtin client. This is the
    //   client_secret analog of Ctrl+B (cookies): prompt for a path, COPY the file into the data
    //   dir (the loader reads from there — there is no INI path for it). The client is cached at
    //   first OAuth use, so the new file is picked up on the next Y-mode login (restart if you
    //   already logged in this session).
    if (s == "secret") {
        std::string input =
            frontend_->input_box("Google client_secret.json path (~/... or absolute; copies into data dir)",
                         "", /*prefill=*/false);
        if (UI::is_input_cancelled(input)) {
            EVENT_LOG("client_secret import cancelled");
            return;
        }
        std::string p = input;
        p.erase(0, p.find_first_not_of(" \t\r\n"));
        p.erase(p.find_last_not_of(" \t\r\n") + 1);
        if (p.empty()) {
            EVENT_LOG(":secret — no path given");
            return;
        }
        std::string src = Utils::expand_path(p);
        std::string dir = Paths::get_data_dir();
        std::error_code ec;
        if (dir.empty()) {
            EVENT_LOG(":secret — data dir unavailable (HOME unset?)");
            return;
        }
        if (!std::filesystem::exists(src, ec)) {
            EVENT_LOG(fmt::format(":secret — file not found: {}", src));
            return;
        }
        std::filesystem::create_directories(dir, ec);
        std::string dst = dir + "/client_secret.json";
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            EVENT_LOG(fmt::format(":secret — copy failed: {}", ec.message()));
            return;
        }
        EVENT_LOG(fmt::format("client_secret imported -> {} (used on next Y-mode login)", dst));
        return;
    }

    PlayMode new_mode = play_mode;
    // Prefix matching: "rep"/"repeat" → REPEAT, "shu"/"shuffle" → SHUFFLE, "cyc"/"cycle" → CYCLE
    if (s.size() >= 2 && std::string("repeat").rfind(s, 0) == 0)
        new_mode = PlayMode::REPEAT;
    else if (s.size() >= 2 && std::string("shuffle").rfind(s, 0) == 0)
        new_mode = PlayMode::SHUFFLE;
    else if (s.size() >= 2 && std::string("cycle").rfind(s, 0) == 0)
        new_mode = PlayMode::CYCLE;
    else {
        EVENT_LOG(fmt::format(
            ": unknown play mode '{}' (try :re/:sh/:cy or :repeat/:shuffle/:cycle)", input));
        return;
    }

    play_mode = new_mode;
    IniConfig::instance().set_play_mode(play_mode);
    // REPEAT relies on loop_file; re-apply immediately if something is playing
    if (player.get_state().has_media) {
        player.set_loop_file(play_mode == PlayMode::REPEAT);
    }
    const char *name = (new_mode == PlayMode::REPEAT)    ? "Repeat"
                       : (new_mode == PlayMode::SHUFFLE) ? "Shuffle"
                                                         : "Cycle";
    EVENT_LOG(fmt::format(": mode set → {}", name));
}

// D7: bind the legacy default keys to Actions (the Keymap is the single source of key bindings;
//   a future [keys] INI override lands here). Bound keys skip the handle_input switch entirely.
// D7/D44: build the Keymap (key→Action). Defaults are the legacy hardcoded keys; an INI [keys]
//   section overrides them (rebindable hotkeys — D7's full goal, landed in D44). Each [keys] entry
//   maps an action name → one or more key tokens (comma-separated; e.g. play_pause = space,p).
//   Absent/unparseable entries keep the default, so an INI without [keys] behaves exactly as before.
//   Complex stateful flows (play 'l' / search '/' / download / mark) stay in the switch by design —
//   only stateless + mode-switch commands are rebindable.
void App::build_keymap() {
    struct Def {
        const char *name;
        const char *tok;
        Action act;
    };
    const Def defs[] = {
        {"play_pause", "space,p", PlayPauseAction{}},
        {"volume_up", "+", VolumeUpAction{}},
        {"volume_down", "-", VolumeDownAction{}},
        {"nav_up", "k", NavUpAction{}},
        {"nav_down", "j", NavDownAction{}},
        // D42: mode-switch keys → SwitchModeAction (homogeneous cluster: switch_mode + optional
        //   hint). M (cycle), N (jump-to-playing), b (region), T stay in the switch (not pure mode
        //   switches). The hint travels with the action: rebinding only changes the trigger key,
        //   the post-switch hint stays (it describes the mode, not the key).
        {"mode_radio", "R", SwitchModeAction{AppMode::RADIO, ""}},
        {"mode_podcast", "P", SwitchModeAction{AppMode::PODCAST, ""}},
        {"mode_favourite", "F", SwitchModeAction{AppMode::FAVOURITE, ""}},
        {"mode_history", "H", SwitchModeAction{AppMode::HISTORY, ""}},
        {"mode_online", "O",
         SwitchModeAction{AppMode::ONLINE, "Switched to ONLINE mode - press '/' to search"}},
        {"mode_account", "Y", SwitchModeAction{AppMode::ACCOUNT,
                                               "Switched to Y mode - 'a' login / 'A' login another / "
                                               "l enter account"}},
        {"mode_bilibili", "B", SwitchModeAction{AppMode::BILIBILI, "B mode (Bilibili)"}},
        {"mode_iptv", "I", SwitchModeAction{AppMode::IPTV,
                                            "I mode (IPTV) - browse All / Region / Country / "
                                            "Category / Language / Custom"}},
    };
    const IniConfig &ini = IniConfig::instance();
    std::map<int, const char *> bound_by; // D44-prof: key → owning action name (collision log)
    // D44-audit ①: keycodes owned by PRE-keymap intercepts in handle_input (Ctrl+Y clipboard,
    //   mouse) — a [keys] binding there can never fire (handle_input returns before the
    //   lookup). Skip + log instead of installing a dead binding.
    const std::set<int> intercepted_before_keymap = {25, KEY_MOUSE};
    // D44-audit ③: keys still handled by the legacy switch (stateful flows). The keymap lookup
    //   runs FIRST, so binding any of these silently kills that flow's key (play 'l' / search
    //   '/' / quit 'q' ...). The defaults table never touches them (verified), so a hit is
    //   always a user override — allow it (power users may want it) but WARN so a dead 'l' is
    //   diagnosable from panicast.log. KEEP IN SYNC with handle_input's switch.
    const std::set<int> legacy_switch_keys = {
        'q', 'Q', 27, '?', 'M', 'N', 'b', 14, 2, 'r', ':', 'l', '\n', 'h', ']', '[',
        KEY_BACKSPACE, '\\', 'g', 'G', KEY_PPAGE, KEY_NPAGE, KEY_RESIZE, 'a', 'A', 'e', 'f',
        'd', 'D', 'm', 'v', 'V', 'C', 'T', 'S', 'L', 'U', 12, 'o', '/', 'J', 'K'};
    for (const Def &d : defs) {
        // D44-fix (BUG-2): IniConfig::get returns "" for a present-but-empty value (NOT the
        //   default) — without this fallback the action would lose ALL its keys, contradicting
        //   the [keys] template's "empty = default" promise. Same empty→default fixup pattern
        //   as get_mpv_ao (F40).
        std::string tok = ini.get("keys", d.name, d.tok);
        if (tok.empty())
            tok = d.tok;
        std::string part;
        std::stringstream ss(tok);
        while (std::getline(ss, part, ',')) {
            // D44-audit ⑤: bind EVERY encoding the token can mean (backspace = 127/8/
            //   KEY_BACKSPACE, enter = '\n'/'\r'/KEY_ENTER across terminfos) — see keymap.h.
            std::vector<int> codes = Keymap::parse_token_all(part);
            if (codes.empty()) {
                // D44-prof: silent skip left typos ("ctrl+y") invisible — log so a dead rebind
                //   is diagnosable from panicast.log.
                LOG(fmt::format("[keys] ignored unparseable token '{}' for action '{}'", part,
                                d.name));
                continue;
            }
            for (int k : codes) {
                if (intercepted_before_keymap.count(k)) {
                    LOG(fmt::format("[keys] token '{}' for '{}' resolves to keycode {} which is "
                                    "intercepted before the keymap (Ctrl+Y clipboard / mouse) — "
                                    "binding skipped",
                                    part, d.name, k));
                    continue;
                }
                if (legacy_switch_keys.count(k))
                    LOG(fmt::format("[keys] WARNING: '{}' bound to keycode {} shadows a legacy "
                                    "flow key — that flow's original key stops working",
                                    d.name, k));
                // D44-prof: bind is map_[key]=a — two actions on one key previously overwrote
                //   silently (later defs win). Warn so the losing action is visible, not a
                //   dead key.
                if (auto it = bound_by.find(k); it != bound_by.end())
                    LOG(fmt::format("[keys] WARNING: key {} bound to both '{}' and '{}' — '{}' "
                                    "wins",
                                    k, it->second, d.name, d.name));
                bound_by[k] = d.name;
                keymap_.bind(k, d.act);
            }
        }
    }
}

void App::handle_input(int ch, int marked_count) {
    // ch is already filtered by the main loop's wget_wch: only ASCII (<128) and special keys
    //   (KEY_CODE_YES) reach here; non-ASCII IME input is dropped before dispatch. Text-input
    //   popups (input_box/search/`:`) accept full UTF-8 via their own wgetch, not here.
    // Ctrl+Y - copy the URL of the object under the CURSOR (feed / episode / local folder /
    //   online item) to the system clipboard. Falls back to the playing item's URL only when
    //   the cursor has none. Copies directly — no popup. If no clipboard tool is available,
    //   only logs (never pops up). Useful to inspect why an episode shows 📜 but loads no lyric.
    if (ch == 25) { // ASCII 25 = Ctrl+Y
        std::string url;
        TreeNodePtr cn = (library_.selected_idx() >= 0 && library_.selected_idx() < (int)library_.display_list().size())
                             ? library_.display_list()[library_.selected_idx()].node
                             : nullptr;
        if (cn && !cn->url.empty())
            url = cn->url; // cursor object (preferred)
        else {
            auto pn = playback_.playback_node();
            if (pn && !pn->url.empty())
                url = pn->url; // fallback
        }
        if (url.empty()) {
            EVENT_LOG("No URL to copy");
        } else if (Utils::copy_to_clipboard(url)) {
            EVENT_LOG(fmt::format("URL copied: {}", url));
        } else {
            EVENT_LOG(fmt::format("Clipboard unavailable; URL: {}", url)); // no popup
        }
        // Y24.4: when the cursor object claims to have a subtitle, also log its transcript URL
        //   so the user can investigate "📜 but no lyric loaded" cases (fetch it manually, etc.).
        if (cn && cn->has_subtitle && !cn->subtitle_url.empty())
            LOG(fmt::format("[Clipboard] cursor '{}' has transcript URL: {}", cn->title,
                            cn->subtitle_url));
        return;
    }

    if (ch == KEY_MOUSE) {
        handle_mouse_event();
        return;
    }

    // Help display handled directly, without using a flag
    if (visual_mode_) {
        // D44-audit ②: cursor moves go through the keymap — the old hardcoded j/k made an INI
        //   nav rebind silently dead inside visual mode (worked in normal lists, died here).
        //   Defaults unchanged: j/k still bind nav_down/nav_up, so behavior is identical unless
        //   the user rebound nav. Other actions stay swallowed (select mode owns the keyboard).
        if (const Action *ka = keymap_.lookup(ch)) {
            if (std::holds_alternative<NavUpAction>(*ka))
                nav_up();
            else if (std::holds_alternative<NavDownAction>(*ka))
                nav_down();
        } else if (ch == 'v')
            confirm_visual_selection();
        else if (ch == 'V') {
            visual_mode_ = false;
            clear_all_marks();
        } // uppercase V cancels all
        else if (ch == 27) {
            visual_mode_ = false;
            EVENT_LOG("Visual cancelled");
        }
        return;
    }

    // D7: Keymap — bound keys go through the message bus (UI→Action→handler), not direct calls.
    //   Unbound keys fall through to the switch (complex flows not yet migrated).
    if (const Action *_ka = keymap_.lookup(ch)) {
        publish_action(*_ka);
        return;
    }

    switch (ch) {
    case 'q': // exit requires confirmation
    case 'Q':
    case 27: // ESC key
        if (frontend_->confirm_box("Quit panicast?")) {
            running = false;
        }
        break;
    case '?':
        frontend_->show_help(player.get_state());
        break; // show help popup directly
    case 'M': {
        // Mode cycle covers all 9 modes (RADIO/PODCAST/FAVOURITE/HISTORY/ONLINE/ACCOUNT/BILIBILI/TIKTOK/IPTV).
        switch_mode((AppMode)(((int)mode + 1) % 9));
        break;
    }
    case 'N': { // Y24.54: jump to the currently playing node (Now playing)
        jump_to_playing();
        break;
    }
    case 'b': { // Y15: ONLINE mode → switch region (was 'B', moved to lowercase to free 'B' for Bilibili)
        if (mode == AppMode::ONLINE) {
            std::string new_region = OnlineState::instance().get_next_region();
            EVENT_LOG(fmt::format("Search region: {}", ITunesSearch::get_region_name(new_region)));
        }
        break;
    }
    // D42: R/P/F/H/O/Y/B/I mode-switch keys migrated to SwitchModeAction (build_keymap).
    case 14: { // Ctrl+N - open network proxy configuration dialog
        configure_proxy();
        break;
    }
    case 2: { // Ctrl+B - set YouTube cookies.txt path (Y02: yt-dlp requires cookies)
        configure_cookies();
        break;
    }
    case 'r': {
        // Y23.1: r on a 🔍 search record re-runs it; r on a search container (Search History /
        //   O-mode online_root) is a no-op (use l to expand).
        TreeNodePtr rn = (library_.selected_idx() >= 0 && library_.selected_idx() < (int)library_.display_list().size())
                             ? library_.display_list()[library_.selected_idx()].node
                             : nullptr;
        if (rn && (rn->is_search_parent)) {
            EVENT_LOG("Use l/Enter to expand search records");
            break;
        }
        if (rn && rn->is_yt_search && rn->url.rfind("search:", 0) == 0) {
            rerun_search_record(rn);
            break;
        }
        // Unified: 'r' = "refresh what's under the cursor", scoped by node type so the same
        //   key is consistent across all 9 modes. Previously Y mode silently re-synced the
        //   WHOLE account on any node (heavy + collapsed the tree), and B mode had no refresh
        //   path at all (silent no-op). Account nodes are disambiguated by mode: is_account
        //   is reused by T-mode creators, so it must NOT be matched bare.
        if (rn && rn->is_account && mode == AppMode::ACCOUNT) {
            resync_account_node(rn); // Y: full Google re-sync (subs + history)
        } else if (rn && rn->is_account && mode == AppMode::BILIBILI) {
            refresh_bilibili_account(rn); // B: re-expand account (recreate children)
        } else if (rn && rn->is_yt_subscriptions) {
            refresh_account_subs(rn); // Y: re-sync subscriptions subtree only
        } else if (rn && rn->is_yt_history) {
            refresh_account_history(rn); // Y: re-sync watch-history subtree only
        } else if (rn && rn->is_bili_followings) {
            refresh_bili_followings(rn); // B: re-fetch followings (WBI)
        } else if (rn && rn->is_bili_history) {
            refresh_bili_history(rn); // B: re-fetch watch history
        } else {
            reset_search();
            refresh_node(); // feeds / channels / T creators / etc.
        }
        break;
    }
    case ':': // command window — set global play_mode (r/s/c or full word)
        open_command_window();
        break;
    // 'k'/'j' (nav up/down) → Keymap → NavUp/NavDownAction (D7)
    case 'l':
    case '\n': { // Enter/l: play the selected episode (its peers become the list); expand folders
        // enter_node handles: marked→play first marked; folder/feed→expand;
        //   playable leaf→play_episode (snapshot peers, play)
        // Y15: B-mode node activation
        if (mode == AppMode::BILIBILI && library_.selected_idx() >= 0 &&
            library_.selected_idx() < (int)library_.display_list().size()) {
            auto bn = library_.display_list()[library_.selected_idx()].node;
            if (bn && bn->is_account) {
                expand_bilibili_account(bn);
                break;
            }
            // Y22: account children — Subscriptions / History (lazy fetch on first expand;
            //   once loaded, enter_node handles collapse/expand toggle, 'r' re-fetches)
            if (bn && bn->is_bili_followings && !bn->children_loaded) {
                expand_bili_followings(bn);
                break;
            }
            if (bn && bn->is_bili_history && !bn->children_loaded) {
                expand_bili_history(bn);
                break;
            }
            // UP master (is_yt_channel) → spawn_load_feed (WBI arc/search); video → enter_node → play
        }
        enter_node(marked_count);
        break;
    }
    case 'h':
        go_back();
        break;
    // Space/'p' (pause) → Keymap → PlayPauseAction (D6/D7)
    // '+'/'-' (volume) → Keymap → VolumeUp/DownAction (D7)
    case ']':
        player.adjust_speed(true);
        break;
    case '[':
        player.adjust_speed(false);
        break;
    case KEY_BACKSPACE:
    case '\\':
        player.reset_speed();
        break; // backslash key also supports speed reset
    case 'g':
        nav_top();
        break;
    case 'G':
        nav_bottom();
        break;
    case KEY_PPAGE:
        nav_page_up();
        break;
    case KEY_NPAGE:
        nav_page_down();
        break;
    case KEY_RESIZE: // terminal resize event handling
        // Handle resize actively, rather than relying on the next frame
        // Old code: break no-op; resize had no effect at all while a popup was open
        resizeterm(0, 0); // 0,0 = re-read actual size from SIGWINCH
        frontend_->handle_resize(); // notify UI to reset cached size, forcing wresize + redraw on the next frame
        break;
    case 'a': {
        // Y01: in Y mode, 'a' = log in one Google account (SmartTube-style QR).
        if (mode == AppMode::ACCOUNT) {
            // Y02: on a [C] YouTube search-result channel, 'a' subscribes it to the active
            //   account; everywhere else in Y mode, 'a' logs in.
            if (library_.selected_idx() >= 0 && library_.selected_idx() < (int)library_.display_list().size()) {
                auto n = library_.display_list()[library_.selected_idx()].node;
                if (n && n->is_yt_search_result && n->is_yt_channel && !n->channel_id.empty()) {
                    subscribe_youtube_channel(n);
                    break;
                }
            }
            start_account_login(false);
            break;
        }
        // Y15/Y23: B-mode — 'a' on a 👤 UP search result subscribes it; else QR login.
        if (mode == AppMode::BILIBILI) {
            if (library_.selected_idx() >= 0 && library_.selected_idx() < (int)library_.display_list().size()) {
                auto n = library_.display_list()[library_.selected_idx()].node;
                if (n && n->is_yt_search_result && n->is_yt_channel && !n->is_youtube &&
                    !n->channel_id.empty()) {
                    subscribe_bilibili_up(n);
                    break;
                }
            }
            start_bilibili_login();
            break;
        }
        // Y24.xx: T-mode — 'a' = Douyin QR scan login (terminal), consistent with B/Y 'a' = login.
        if (mode == AppMode::TIKTOK) {
            start_douyin_login();
            break;
        }
        // Support multi-select subscription in ONLINE/FAVOURITE mode
        if (mode == AppMode::PODCAST) {
            add_feed();
        } else if (mode == AppMode::ONLINE) {
            if (marked_count > 0) {
                // Multi-select batch subscribe
                subscribe_online_podcasts_batch(marked_count);
            } else {
                // Single subscribe
                subscribe_online_podcast();
            }
        } else if (mode == AppMode::FAVOURITE) {
            // 'a' in FAVOURITE mode: recursively scan a local folder and add its playable
            //   files as children of a new folder node (replaces the old subscribe binding).
            add_local_files();
        }
        break;
    }
    case 'A': {
        // Y01: in Y mode, 'A' = log in ANOTHER Google account (add additional).
        if (mode == AppMode::ACCOUNT) {
            start_account_login(true);
            break;
        }
        // Y18: in B mode, 'A' = login ANOTHER Bilibili account (QR, same as 'a').
        //   Cookie path is set via Ctrl+B (unified, context-aware) — NOT 'A'.
        if (mode == AppMode::BILIBILI) {
            start_bilibili_login();
            break;
        }
        // Y24.xx: in T mode, 'A' = Douyin QR scan login (terminal QR, mirrors B-mode 'A').
        if (mode == AppMode::TIKTOK) {
            start_douyin_login();
            break;
        }
        break;
    }
    case 'e':
        edit_node();
        break; // edit node title and URL
    case 'f': {
        // Support multi-select favourites
        if (marked_count > 0) {
            add_favourites_batch(marked_count);
        } else {
            add_favourite();
        }
        break;
    }
    case 'd': {
        // Y23.1: d on a 🔍 search record deletes it; d on a search container is a no-op.
        TreeNodePtr dn = (library_.selected_idx() >= 0 && library_.selected_idx() < (int)library_.display_list().size())
                             ? library_.display_list()[library_.selected_idx()].node
                             : nullptr;
        if (dn && (dn->is_search_parent)) {
            EVENT_LOG("Cannot delete the search-records container");
            break;
        }
        if (dn && dn->is_yt_search && dn->url.rfind("search:", 0) == 0) {
            delete_search_record(dn);
            break;
        }
        if (mode == AppMode::ACCOUNT) {
            if (dn)
                delete_account_node(dn);
        } else if (mode == AppMode::BILIBILI) {
            // DB-11: delete the Bilibili account under the cursor (account nodes only).
            if (dn && dn->is_account) {
                delete_bilibili_account_node(dn);
                break;
            }
            delete_node(marked_count);
        } else if (mode == AppMode::TIKTOK) {
            // Y24.11: delete the subscribed creator under the cursor (creator nodes only).
            if (dn && dn->is_account) {
                delete_tiktok_user_node(dn);
                break;
            }
            delete_node(marked_count);
        } else {
            delete_node(marked_count);
        }
        break;
    }
    case 'D':
        download_node(marked_count);
        break;
    case 'm':
        toggle_mark();
        break;
    case 'v':
        visual_mode_ = true;
        visual_start_ = library_.selected_idx();
        break;
    case 'V':
        clear_all_marks();
        break; // uppercase V cancels all selections
    case 'C':
        playback_.clear_playlist();
        break; // N04-fix: clear peer playlist, keep playing
    case 'T': { // Y24.11: T mode (TikTok/Douyin) — was tree-lines toggle (now default ON, no keybind)
        switch_mode(AppMode::TIKTOK);
        EVENT_LOG("T mode (TikTok/Douyin)");
        break;
    }
    case 'S':
        frontend_->toggle_scroll_mode();
        break;
    // N04-fix: z/Z direct subtitle-offset keys removed. Subtitle delay is now adjusted
    //   only via the `:` command window (:z / :Z → mpv sub-delay, video-window subtitles).
    //   The old z/Z adjusted a separate LYRIC-panel offset persisted to INI, which was
    //   redundant with :z/:Z and an odd global-persisted per-content setting. See sheet 60.
    case 'L': { // Y24.43: unified subtitle flow (local-first; L never stops ASR)
        auto pst = player.get_state();
        if (pst.has_media) {
            auto pn = playback_.playback_node();
            bool vo_open = player.is_video_window_open();
            std::string url = pst.current_url;
            bool is_streaming = !URLClassifier::is_local_file(url);

            // D11-3a: source resolution centralized in SubtitleService::resolve_subtitle_source
            //   (embedded > local SRT [unified find_local_subtitle] > online). ASR only when none.
            //   Resolved LAZILY per path (after the LYRIC gates) — find_local_subtitle stats the
            //   WSL2 /mnt/e mount, so skip it when the L-press just closes the panel.

            if (vo_open) {
                // VO open: ONE-SHOT ensure subtitles in the mpv video window (mpv controls display).
                // Priority (local first): embedded > local ASR SRT > online > video ASR.
                auto src = subtitle_.resolve_subtitle_source(pn);
                if (src.kind == ResolvedSubtitle::Embedded) {
                    EVENT_LOG("L: embedded subtitle present (English via slang) - no ASR, mpv "
                              "renders in video window");
                    break;
                }
                if (src.kind == ResolvedSubtitle::LocalSrt) {
                    pn->has_asr_srt = true;
                    pn->asr_srt_path = src.path;
                    player.sub_add(src.path);
                    EVENT_LOG(fmt::format("L: local ASR SRT -> video window: {}", src.path));
                    break;
                }
                if (src.kind == ResolvedSubtitle::Online) {
                    player.sub_add(pn->subtitle_url);
                    EVENT_LOG("L: online transcript -> video window");
                    break;
                }
                subtitle_.transcription_engine().start_realtime(pn, url, is_streaming,
                                                     /*is_video=*/true);
                player.show_osd("Starting ASR transcription...", 3000);
                break;
            }

            // !vo_open (audio, or video without VO): TOGGLE the bottom LYRIC panel via the
            //   per-track manual override (Y24.48). Active→Closed (suppress auto until track
            //   change); inactive→Open (show immediately, e.g. during ASR startup).
            if (frontend_->is_lyric_bar_active()) {
                frontend_->set_lyric_manual(LyricManual::Closed);
                break;
            }
            frontend_->set_lyric_manual(LyricManual::Open);
            // LYRIC-fix (2026-08-15): the INI master switch ([display] lyric_bar, default OFF)
            //   gates panel activation in app_run — pressing L is explicit user intent, so enable
            //   the master switch too. Without this, L was a silent no-op: the transcript loaded
            //   fine ("parsed 309 segments", "L: online transcript -> LYRIC" logged) but the
            //   panel never appeared (user report 2026-08-15).
            if (!frontend_->is_lyric_bar_requested())
                frontend_->set_lyric_bar_requested(true);
            // If ASR already running, just reveal the panel — don't restart.
            if (subtitle_.transcription_engine().realtime_running()) {
                break;
            }
            // Take a source, LOCAL FIRST (embedded implies video — feeds LYRIC via the fallback,
            //   nothing to load).
            //   video file: embedded > local ASR SRT > online > audio ASR
            //   audio file: local ASR SRT > online > audio ASR
            auto src = subtitle_.resolve_subtitle_source(pn);
            if (src.kind == ResolvedSubtitle::Embedded) {
                EVENT_LOG("L: embedded subtitle (English) -> LYRIC");
                break; // sub-text feeds LYRIC via the fallback; nothing to load
            }
            if (src.kind == ResolvedSubtitle::LocalSrt) {
                pn->has_asr_srt = true;
                pn->asr_srt_path = src.path;
                pn->subtitle_url = src.path;
                pn->subtitle_type = "srt";
                subtitle_.subtitle_mgr().load_async(pn, pool_);
                EVENT_LOG(fmt::format("L: local ASR SRT -> LYRIC: {}", src.path));
                break;
            }
            if (src.kind == ResolvedSubtitle::Online) {
                subtitle_.subtitle_mgr().load_async(pn, pool_);
                EVENT_LOG("L: online transcript -> LYRIC");
                break;
            }
            // No source -> audio ASR (is_video=false so it feeds LYRIC; video-without-VO reuses audio flow).
            subtitle_.transcription_engine().start_realtime(pn, url, is_streaming,
                                                 /*is_video=*/false);
            break;
        }

        // F mode: offline batch transcription (unchanged).
        if (mode == AppMode::FAVOURITE) {
            if (subtitle_.transcription_engine().busy()) {
                subtitle_.transcription_engine().stop_offline();
                break;
            }
            std::vector<TreeNodePtr> targets;
            if (marked_count > 0) {
                collect_playable_marked_current(targets);
            } else if (library_.selected_idx() >= 0 && library_.selected_idx() < (int)library_.display_list().size()) {
                auto n = library_.display_list()[library_.selected_idx()].node;
                bool is_local =
                    !n->local_file.empty() || URLClassifier::is_local_file(n->url);
                if (is_playable_node(n) && is_local) {
                    targets.push_back(n);
                }
            }
            if (!targets.empty()) {
                subtitle_.transcription_engine().enqueue_offline(targets);
                break;
            }
        }
        // Fallback (not playing, not F-batch): toggle the LYRIC panel.
        frontend_->toggle_lyric_bar();
        break;
    }
    case 'U': { // toggle icon style (ASCII/Emoji)
        // Extended to cover status-bar art + mode badges + persistence
        IconManager::toggle_style();
        // Persist to INI
        IniConfig::instance().set("display", "icon_style",
                                  IconManager::get_style() == IconStyle::EMOJI ? "emoji" : "ascii");
        EVENT_LOG(fmt::format("Icon style: {} (saved to config)", IconManager::get_style_name()));
        break;
    }
    case 12: // Ctrl+L - cycle through 22 color themes
        frontend_->toggle_theme();
        break;
    case 'o':
        toggle_sort_order();
        break; // toggle sort order
    case '/':
        if (mode == AppMode::ONLINE) {
            perform_online_search(); // Online mode search
        } else if (mode == AppMode::BILIBILI) {
            perform_bilibili_search();
        } else if (mode == AppMode::ACCOUNT) {
            perform_youtube_search(); // Y02: YouTube search (channels/videos/playlists)
        } else if (mode == AppMode::TIKTOK) {
            tiktok_direct_input(); // Y24.16: '/' = open @user/#tag/URL (no keyword search)
        } else if (mode == AppMode::FAVOURITE) {
            // In FAVOURITE mode, detect whether under an online_root LINK node
            bool under_online_link = false;
            if (library_.selected_idx() < (int)library_.display_list().size()) {
                auto node = library_.display_list()[library_.selected_idx()].node;
                // Check whether the current node or its parent is an online_root LINK
                for (auto &f : library_.fav_root()) {
                    if (f->is_link && f->url == "online_root") {
                        // Check whether the current node is f or a child of f
                        if (f.get() == node.get()) {
                            under_online_link = true;
                            break;
                        }
                        for (auto &child : f->children) {
                            if (child.get() == node.get()) {
                                under_online_link = true;
                                break;
                            }
                        }
                        if (under_online_link)
                            break;
                    }
                }
            }
            if (under_online_link) {
                // Under online_root LINK, perform an online search and sync
                perform_online_search_from_favourite();
            } else {
                perform_search();
            }
        } else {
            perform_search();
        }
        break;
    case 'J':
        jump_search(1);
        break;
    case 'K':
        jump_search(-1);
        break;
    }
}

} // namespace panicast
