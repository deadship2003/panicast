// UI rendering layer — extracted implementation unit (Y24.33–Y24.36).
//   Methods remain UI members (they touch private UI state); only their
//   implementations live here. Declarations stay in ui.h.
#include "panicast/ui/ui.h"

#include <algorithm>
#include <string>

#include <ncurses.h>

namespace panicast
{

void UI::draw_info(WINDOW *win, const MPVController::State &state, AppState app_state,
                   const DisplayContext &dctx, int marked_count, const std::string &search_query,
                   int current_match, int total_matches, TreeNodePtr selected_node,
                   const std::vector<DownloadProgress> &downloads, bool visual_mode, int cw,
                   const std::vector<PlaylistItem> &playlist, int playlist_index,
                   PlayMode play_mode, const std::vector<std::string> &history_titles,
                   const std::vector<int> &next_indices) {
    //Truncation width computation
    // Right panel structure (window width right_w):
    //   column 0 = left border │
    //   column 1 = left margin (space)
    //   columns 2 ~ right_w-3 = content area
    //   column right_w-2 = right margin (space)
    //   column right_w-1 = right border │

    // Input param: cw = right_w - 3 (content area width + right margin)
    // Print starts at column 2, last usable column is right_w-3
    // Available width = (right_w-3) - 2 + 1 = right_w - 4 = cw - 1

    // Actual content area goes to column right_w-3, right margin at column right_w-2
    // so truncation width = cw (content area + right margin, fully utilized)
    int safe_cw = cw;
    if (safe_cw < 1)
        safe_cw = 1; // minimum width protection

    int border_bottom = top_h - 1;
    // Use LayoutGuard::safe_split_y, which always returns a valid range or -1
    // Old code: int log_height = std::max(6, (int)((top_h-2)*0.4)); int split_y = border_bottom - log_height;
    // When top_h ≤ 7, split_y ≤ 0, causing mvwaddch(split_y, 0, ACS_LTEE) to overwrite the top-left corner ┌
    // Note: log_height is not directly used afterwards (the EVENT LOG area uses current_log_y = border_bottom - 1 with split_y boundary checks)
    int split_y = LayoutGuard::safe_split_y(top_h);

    int y = 1;

    if (visual_mode) {
        wattron(win, A_BOLD);
        mvwprintw(win, y++, 2, "-- VISUAL MODE --");
        wattroff(win, A_BOLD);
        mvwprintw(win, y++, 2, "j/k: extend | v: confirm | Esc: cancel | V: clear all");
        y++;
    }

    if (!search_query.empty()) {
        wattron(win, A_BOLD);
        std::string search_info =
            fmt::format("Search: \"{}\" ({}/{})", search_query,
                        total_matches > 0 ? current_match + 1 : 0, total_matches);
        mvwprintw(win, y++, 2, "%s",
                  Utils::truncate_by_display_width(search_info, safe_cw).c_str());
        wattroff(win, A_BOLD);
        y++;
    }

    // V0.03: improved playback state display, including volume and speed
    std::string state_str;
    std::string state_icon;
    // State icon switches with icon_style: true-emoji (⏳🎯📋, glibc=2) only used in EMOJI style,
    // ASCII style falls back to symbols, to avoid emoji width misalignment on narrow terminals. ▶/‖/● are
    // geometric/symbol chars (glibc=1=terminal 1), safe in both styles.
    const bool emoji_icons = (IconManager::get_style() == IconStyle::EMOJI);
    switch (app_state) {
    case AppState::LOADING:
        state_icon = emoji_icons ? "⏳" : "~";
        state_str = "Loading...";
        break;
    case AppState::PLAYING:
        state_icon = "▶";
        state_str = "Playing";
        break;
    case AppState::PAUSED:
        state_icon = "⏸️";
        state_str = "Paused";
        break;
    case AppState::BUFFERING:
        state_icon = emoji_icons ? "⏳" : "~";
        state_str = "Buffering...";
        break;
    case AppState::BROWSING:
        state_icon = emoji_icons ? "🎯" : "*";
        state_str = "Navigating";
        break;
    default:
        state_icon = "●";
        state_str = "Idle";
        break;
    }

    //Do not show this status line during play/pause (the PLAYER STATUS area already shows the same info)
    if (app_state != AppState::PLAYING && app_state != AppState::PAUSED) {
        // V0.03: status line shows: state | volume | speed
        //Precisely compute the truncation width

        //Precisely compute the truncation width
        // Right panel structure (window width right_w):
        //   column 0 = left border │
        //   column 1 = left margin (space)
        //   columns 2 ~ right_w-3 = content area
        //   column right_w-2 = right margin (space)
        //   column right_w-1 = right border │

        // Input param: cw = right_w - 3 (content area width)
        // Print starts at column 2, available width = cw

        // Truncation width = safe_cw (safe protection of the right border)
        std::string status_line = fmt::format("{} {} | Vol:{}% | Spd:{:.1f}x", state_icon,
                                              state_str, state.volume, state.speed);
        wattron(win, A_BOLD);
        mvwprintw(win, y++, 2, "%s",
                  Utils::truncate_by_display_width(status_line, safe_cw).c_str());
        wattroff(win, A_BOLD);
    }

    // Always show the current play mode in the INFO area (R/S/C or : command can change it)
    {
        const char *m_icon;
        const char *m_name;
        switch (play_mode) {
        case PlayMode::REPEAT:
            m_icon = "🔂";
            m_name = "Repeat";
            break;
        case PlayMode::SHUFFLE:
            m_icon = "🔀";
            m_name = "Shuffle";
            break;
        default:
            m_icon = "🔁";
            m_name = "Cycle";
            break;
        }
        std::string mode_line = fmt::format("Mode: {} {}", m_icon, m_name);
        mvwprintw(win, y++, 2, "%s", Utils::truncate_by_display_width(mode_line, safe_cw).c_str());
    }

    if (marked_count > 0) {
        mvwprintw(win, y++, 2, "Marked: %d items", marked_count);
        mvwprintw(win, y++, 2, "Enter: Play | d: Del | D: DL");
        y++;
    }

    if (!downloads.empty()) {
        y++;
        wattron(win, A_BOLD);
        mvwprintw(win, y++, 2, "--- Downloads ---");
        wattroff(win, A_BOLD);

        int dl_index =
            0; // download item index (starts from 1, for locating during parallel multi-task)
        for (const auto &dl : downloads) {
            if (y >= split_y - 3)
                break;
            ++dl_index;

            //Enhanced download status display: index + title + status
            std::string status_line = fmt::format("{}. {}", dl_index, dl.title);
            if (dl.active) {
                status_line += fmt::format(" [{}%]", dl.percent);
            } else if (dl.failed) {
                status_line += " [FAIL]";
            } else if (dl.complete) {
                status_line += " [OK]";
            }

            mvwprintw(win, y++, 2, "%s",
                      Utils::truncate_by_display_width(status_line, safe_cw).c_str());

            // Download progress bar: speed+ETA right-aligned, the bar fills the remaining width (full width, consistent with the playback progress bar)
            if (dl.active && y < split_y - 2 && dl.percent > 0) {
                // First compute the speed+ETA text and its display width
                std::string speed_eta;
                if (!dl.speed.empty())
                    speed_eta = dl.speed;
                else
                    speed_eta = "..."; // no-speed placeholder
                if (dl.eta_seconds > 0) {
                    int eta_mins = dl.eta_seconds / 60;
                    int eta_secs = dl.eta_seconds % 60;
                    speed_eta += fmt::format(" ETA:{}:{:02d}", eta_mins, eta_secs);
                } else if (dl.percent < 100) {
                    speed_eta += " ETA:--:--";
                }
                int se_w = Utils::utf8_display_width(speed_eta);
                int available = std::max(1, cw - 3); // left margin 2 + right margin 1
                // bar width = available - (speed_eta + 1 space) - 2 (brackets); the rest goes to the bar, no cap
                int bar_width = available - se_w - 1 - 2;
                if (bar_width < 1)
                    bar_width = 1; // extremely narrow: at least 1 cell, prioritize speed/ETA
                int filled = (dl.percent * bar_width) / 100;
                std::string bar = "[";
                for (int i = 0; i < bar_width; ++i)
                    bar += (i < filled) ? "█" : "░";
                bar += "]";
                wattron(win, COLOR_PAIR(11));
                mvwprintw(win, y++, 2, "%s %s", bar.c_str(), speed_eta.c_str());
                wattroff(win, COLOR_PAIR(11));
            }
        }
        y++;
    }

    if (app_state == AppState::PLAYING || app_state == AppState::PAUSED) {
        // V2.39-FF: add player status title
        y++;
        // F35: use the theme foreground pair (PAIR_BORDER_STD) instead of A_BOLD.
        //   A_BOLD renders as a bright color (index 8-15) on many terminals, which the
        //   15 themes do NOT redefine (they only redefine normal colors 0-7 via init_color),
        //   so A_BOLD looked fixed across themes. PAIR_BORDER_STD = init_pair(20, fg, bg)
        //   where fg is a normal color index redefined by the theme → follows the theme.
        wattron(win, COLOR_PAIR(PAIR_BORDER_STD));
        mvwprintw(win, y++, 2, "=== PLAYER STATUS ===");
        wattroff(win, COLOR_PAIR(PAIR_BORDER_STD));

        // V2.39-FF: show play status and speed/volume
        // A-3 palette — play ▶ green / pause ⏸️ blue
        std::string play_state = (app_state == AppState::PLAYING) ? "▶ Playing" : "⏸️ Paused";
        int ps_pair = (app_state == AppState::PLAYING) ? 11 : 14; // 11=GREEN, 14=BLUE
        wattron(win, A_BOLD | COLOR_PAIR(ps_pair));
        mvwprintw(win, y++, 2, "%s", play_state.c_str());
        wattroff(win, A_BOLD | COLOR_PAIR(ps_pair));
        mvwprintw(win, y++, 2, "Speed:  %.1fx", state.speed);
        mvwprintw(win, y++, 2, "Volume: %d%%", state.volume);

        //ASCII-art timeline progress bar (width adaptive)
        if (state.media_duration > 0) {
            int cur_mins = (int)state.time_pos / 60;
            int cur_secs = (int)state.time_pos % 60;
            int tot_mins = (int)state.media_duration / 60;
            int tot_secs = (int)state.media_duration % 60;

            // Compute progress percentage
            double progress = state.time_pos / state.media_duration;
            if (progress > 1.0)
                progress = 1.0;
            if (progress < 0.0)
                progress = 0.0;

            //Timeline width adapts to the window — the bar fills the remaining space
            // The duration display width is dynamic (" 0:30/0:45" vs " 0:00/120:00"); compute the actual display width first
            std::string time_str =
                fmt::format(" {}:{:02d}/{}:{:02d}", cur_mins, cur_secs, tot_mins, tot_secs);
            int time_w = Utils::utf8_display_width(time_str);
            // Playhead ▶(U+25B6) is 2 columns on wide terminals, 1 column on narrow/‖ terminals; count by measured width
            std::string ph_glyph = (app_state == AppState::PLAYING) ? "▶" : "⏸️";
            int ph_w = Utils::utf8_display_width(ph_glyph);
            // Full-row available display width (consistent with the cw-3 truncation target below: left margin 2 + right margin 1)
            int available = std::max(1, cw - 3);
            // bar display width = 1(left bracket) + (bar_width-1)(▓/░ cells) + ph_w(playhead) + 1(right bracket) = bar_width + 1 + ph_w
            // Let bar + time_w = available -> bar_width = available - time_w - 1 - ph_w
            int bar_width = available - time_w - 1 - ph_w;
            if (bar_width < 1)
                bar_width =
                    1; // extremely narrow window: at least 1 cell, prioritize duration display
            // No longer cap at 40 — let the bar fill the window's remaining space
            int filled = static_cast<int>(progress * bar_width);

            std::string timeline = "[";
            for (int i = 0; i < bar_width; ++i) {
                if (i < filled) {
                    timeline += "▓"; // played portion
                } else if (i == filled) {
                    timeline += ph_glyph; // playhead (‖ replaces ⏸)
                } else {
                    timeline += "░"; // unplayed portion
                }
            }
            timeline += "]";

            // Time right-aligned to the row end (time_w/available computed above) — the bar fills the left remaining —
            //   avoids leaving space to the right of the time (previously timeline+time left-aligned + playhead width error left trailing spaces).
            int time_x =
                2 + available - time_w; // time start column (right-aligned, ending at col cw-2)
            if (time_x < 3)
                time_x = 3;
            // Clear the row (prevent old char residue / border embedding)
            wmove(win, y, 2);
            for (int i = 2; i < cw; ++i)
                waddch(win, ' ');
            // Bar (left, playhead segmented colored) + time (right-aligned to row end)
            // F32: removed the old hardcoded green(11)/blue(14) playhead color — play/pause
            //   is conveyed by the glyph (▶/⏸) and the "▶ Playing"/"⏸️ Paused" line above.
            // F35: use PAIR_BORDER_STD (theme fg) instead of A_BOLD — A_BOLD renders as a
            //   bright color (index 8-15) which themes don't redefine, so it looked fixed.
            wattron(win, COLOR_PAIR(PAIR_BORDER_STD));
            size_t ph_pos = timeline.find(ph_glyph);
            if (ph_pos != std::string::npos) {
                mvwaddstr(win, y, 2, timeline.substr(0, ph_pos).c_str());
                waddstr(win, ph_glyph.c_str());
                waddstr(win, timeline.substr(ph_pos + ph_glyph.size()).c_str());
            } else {
                mvwaddstr(win, y, 2, timeline.c_str());
            }
            mvwaddstr(win, y, time_x, time_str.c_str()); // time right-aligned to row end
            wattroff(win, COLOR_PAIR(PAIR_BORDER_STD));
            y++;
        } else if (state.time_pos > 0) {
            // No total duration: show only the current time
            int cur_mins = (int)state.time_pos / 60;
            int cur_secs = (int)state.time_pos % 60;
            mvwprintw(win, y++, 2, "Time:   %d:%02d", cur_mins, cur_secs);
        }

        //Video/Audio info truncation to prevent overflow
        if (!state.audio_codec.empty()) {
            mvwprintw(win, y++, 2, "Audio:  %s",
                      Utils::truncate_by_display_width(state.audio_codec, safe_cw - 8).c_str());
        }
        if (!state.video_codec.empty()) {
            // F27: append decode method — hwdec-current value, or "software" when empty/"no".
            std::string vline = state.video_codec;
            vline += fmt::format(" [{}]",
                                 state.hwdec_current.empty() ? "software" : state.hwdec_current);
            mvwprintw(win, y++, 2, "Video:  %s",
                      Utils::truncate_by_display_width(vline, safe_cw - 8).c_str());
        }
        // F22: VO/AO device + bitrate info (only when playing)
        if (state.has_media) {
            // VO: show device + video codec + resolution + bitrate + hwdec (Y16: add decoder)
            if (!state.current_vo.empty() && state.current_vo != "null") {
                std::string vo_line = fmt::format("VO: {} {}x{}", state.current_vo,
                                                  state.video_width, state.video_height);
                if (state.video_bitrate > 0)
                    vo_line += fmt::format(" {}kbps", state.video_bitrate);
                // Y16: append decoder — hwdec-current (vaapi-copy/nvdec/...) or [software]
                vo_line += fmt::format(" [{}]", state.hwdec_current.empty() ? "software"
                                                                            : state.hwdec_current);
                if (y < split_y)
                    mvwprintw(win, y++, 2, "%s",
                              Utils::truncate_by_display_width(vo_line, safe_cw - 2).c_str());
            } else {
                if (y < split_y)
                    mvwprintw(win, y++, 2, "VO: null (audio only)");
            }
            // AO: show device + audio channels + samplerate + bitrate + codec (Y16: add decoder)
            std::string ao_line =
                fmt::format("AO: {}", state.current_ao.empty() ? "null" : state.current_ao);
            if (!state.audio_channels.empty())
                ao_line += fmt::format(" {}ch", state.audio_channels);
            if (!state.audio_samplerate.empty())
                ao_line += fmt::format(" {}Hz", state.audio_samplerate);
            if (state.audio_bitrate > 0)
                ao_line += fmt::format(" {}kbps", state.audio_bitrate);
            // Y16: append audio codec as "decoder" (audio has no hwdec; codec = which decoder)
            if (!state.audio_codec.empty())
                ao_line += fmt::format(" [{}]", state.audio_codec);
            if (y < split_y)
                mvwprintw(win, y++, 2, "%s",
                          Utils::truncate_by_display_width(ao_line, safe_cw - 2).c_str());
            // Y11: network/stream health. cache-speed=download rate; demuxer-cache-duration
            //   =seconds buffered ahead; cache-buffering-state=0-100 while buffering (>0).
            //   Plain text label (theme fg, no emoji/⚠ — adapts to any theme). No [LOW] marker
            //   (a low number is self-evident). Label "Buffering:" per user (was "Buf:").
            std::string nspeed;
            double bps = state.net_speed_bps;
            if (bps >= 1000000.0)
                nspeed = fmt::format("{:.1f} MB/s", bps / 1000000.0);
            else if (bps >= 1000.0)
                nspeed = fmt::format("{} KB/s", (int)(bps / 1000.0));
            else
                nspeed = "--";
            std::string nbuf = (state.buffering_pct > 0)
                                   ? fmt::format("Buffering: {}%", state.buffering_pct)
                                   : fmt::format("Buffering: {:.1f}s", state.buffering_sec);
            std::string net_line = fmt::format("Network: {} | {}", nspeed, nbuf);
            if (y < split_y)
                mvwprintw(win, y++, 2, "%s",
                          Utils::truncate_by_display_width(net_line, safe_cw - 2).c_str());
        }
        y++;

        // D15: now-playing title/url arrive via the DisplayContext view-model (the canonical
        //   source identity from PlaybackService::now_playing()), not a domain TreeNodePtr — the
        //   UI render path no longer holds the playback node. Empty when nothing is playing →
        //   fall back to mpv's state (state.title / state.current_url played path).
        std::string title_display =
            !dctx.now_playing_title.empty() ? dctx.now_playing_title : state.title;
        if (!title_display.empty()) {
            mvwprintw(win, y++, 2, "Title: %s",
                      Utils::truncate_by_display_width(title_display, safe_cw - 7).c_str());
        }
        // F32: pair the playing program's Streaming URL with its Title (both in the playing
        //   block, on top). Previously the Streaming URL was wedged inside the cursor-node
        //   block, mixing the playing stream URL with the cursor node's own URL.
        // D14-3/D15: display the canonical source URL (dctx.now_playing_url, fed by
        //   PlaybackService::now_playing()), not the played mpv/cache path (state.current_url) —
        //   a cached item shows its real source URL.
        std::string stream_url =
            !dctx.now_playing_url.empty() ? dctx.now_playing_url : state.current_url;
        if (state.has_media && !stream_url.empty()) {
            if (y < split_y)
                mvwprintw(win, y++, 2, "Streaming URL:");
            int su_width = safe_cw - 2;
            if (su_width < 10)
                su_width = 10;
            std::string full_url = Utils::http_to_https(stream_url);
            std::vector<std::string> su_lines = Utils::wrap_text(full_url, su_width, 12);
            bool osc8 = url_hyperlink_;
            int by = 0, bx = 0;
            getbegyx(win, by, bx); // abs origin for OSC 8 positioning
            for (size_t li = 0; li < su_lines.size() && y < split_y - 1; ++li) {
                int line_y = y;
                mvwprintw(win, y++, 2, "  %s", su_lines[li].c_str());
                // Y24.12/15: each wrapped line → OSC 8 link to the FULL url. Same id="u" on
                //   every line → terminal treats them as ONE link → hovering any line
                //   underlines ALL lines synchronously (not just the hovered one).
                if (osc8)
                    pending_osc8_.push_back(
                        {by + line_y + 1, bx + 4 + 1, su_lines[li], full_url, "u"});
            }
        }
    }

    if (selected_node && y < split_y) {
        // Y01: account / YouTube-history / YouTube-subscription nodes get a dedicated INFO block
        //   (account email/channel/token expiry/sync). Falls through to the generic block otherwise.
        if (selected_node->is_account || selected_node->is_yt_history ||
            selected_node->is_yt_subscriptions || selected_node->is_yt_channel) {
            if (selected_node->is_account) {
                if (y < split_y)
                    mvwprintw(win, y++, 2, "Type:  Google Account");
                if (y < split_y)
                    mvwprintw(win, y++, 2, "Label: %s",
                              Utils::truncate_by_display_width(selected_node->title, safe_cw - 7)
                                  .c_str());
                if (y < split_y)
                    mvwprintw(
                        win, y++, 2, "Email: %s",
                        Utils::truncate_by_display_width(selected_node->account_email, safe_cw - 7)
                            .c_str());
                if (y < split_y)
                    mvwprintw(win, y++, 2, "Subs:  %d", selected_node->account_sub_count);
                if (selected_node->account_token_expires > 0 && y < split_y) {
                    time_t exp = (time_t)selected_node->account_token_expires;
                    struct tm tmv;
                    localtime_r(&exp, &tmv);
                    char buf[32];
                    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
                    mvwprintw(win, y++, 2, "Token: %s", buf);
                }
                if (y < split_y)
                    mvwprintw(win, y++, 2, "Active:%s",
                              selected_node->account_id == 0
                                  ? ""
                                  : (selected_node->is_cached ? " yes" : " no"));
            } else {
                if (y < split_y)
                    mvwprintw(win, y++, 2, "Type:  %s",
                              selected_node->is_yt_history         ? "YouTube History"
                              : selected_node->is_yt_subscriptions ? "YouTube Subscriptions"
                                                                   : "YouTube Channel");
                if (y < split_y)
                    mvwprintw(win, y++, 2, "Title: %s",
                              Utils::truncate_by_display_width(selected_node->title, safe_cw - 7)
                                  .c_str());
                if (!selected_node->url.empty() && y < split_y) {
                    if (y < split_y)
                        mvwprintw(win, y++, 2, "URL:");
                    std::vector<std::string> ul =
                        Utils::wrap_text(Utils::http_to_https(selected_node->url), safe_cw - 2, 12);
                    for (size_t li = 0; li < ul.size() && y < split_y - 1; ++li)
                        mvwprintw(win, y++, 2, "  %s", ul[li].c_str());
                }
            }
        } else {
            y++;
            std::string type_str;
            switch (selected_node->type) {
            case NodeType::FOLDER:
                type_str = "Folder";
                break;
            case NodeType::RADIO_STREAM:
                type_str = "Radio";
                break;
            case NodeType::PODCAST_FEED:
                type_str = "Feed";
                break;
            case NodeType::PODCAST_EPISODE:
                type_str = "Episode";
                break;
            }

            URLType url_type = URLClassifier::classify(selected_node->url);

            if (y < split_y)
                mvwprintw(win, y++, 2, "Type: %s", type_str.c_str());
            if (y < split_y)
                mvwprintw(
                    win, y++, 2, "Title: %s",
                    Utils::truncate_by_display_width(selected_node->title, safe_cw - 7).c_str());

            // Parse-failure diagnostics (B9n3f11): show error_msg in red, for troubleshooting network/proxy/yt-dlp issues
            if (selected_node->parse_failed && !selected_node->error_msg.empty()) {
                if (y < split_y) {
                    wattron(win, COLOR_PAIR(13)); // red
                    mvwprintw(
                        win, y++, 2, "Error: %s",
                        Utils::truncate_by_display_width(selected_node->error_msg, safe_cw - 7)
                            .c_str());
                    wattroff(win, COLOR_PAIR(13));
                }
            }

            if (!selected_node->url.empty()) {
                //HTTP-to-HTTPS display - safety first
                std::string url = Utils::http_to_https(selected_node->url);

                //URL display format optimization - label on its own line, URL wrapped with 2-space indent
                // Format:
                //   URL:
                //     https://...
                int url_width = safe_cw - 2; // 2-space indent + 2-space margin
                if (url_width < 10)
                    url_width = 10;

                // Label on its own line
                if (y < split_y)
                    mvwprintw(win, y++, 2, "URL:");

                // URL wrapped, indented 2 spaces
                std::vector<std::string> url_lines = Utils::wrap_text(url, url_width, 12);
                for (size_t li = 0; li < url_lines.size() && y < split_y - 1; ++li) {
                    mvwprintw(win, y++, 2, "  %s", url_lines[li].c_str());
                }
                // F32: Streaming URL moved to the playing block (paired with the playing Title).
            }

            //Fix subtext display - distinguish node types, always truncate
            if (!selected_node->subtext.empty()) {
                // PODCAST_FEED type shows "Podcast:", other types show "Date:"
                std::string label =
                    (selected_node->type == NodeType::PODCAST_FEED) ? "Podcast:" : "Date:";
                // Label takes 8-9 chars, starts at column 2, needs truncation
                int subtext_max_width = safe_cw - 9;
                if (subtext_max_width < 10)
                    subtext_max_width = 10;
                if (y < split_y)
                    mvwprintw(
                        win, y++, 2, "%s %s", label.c_str(),
                        Utils::truncate_by_display_width(selected_node->subtext, subtext_max_width)
                            .c_str());
            }
            if (selected_node->duration > 0) {
                if (y < split_y)
                    mvwprintw(win, y++, 2, "Dur:   %s",
                              Utils::format_duration(selected_node->duration).c_str());
            }

            if (selected_node->is_cached ||
                (selected_node->type == NodeType::PODCAST_FEED && selected_node->children_loaded)) {
                wattron(win,
                        COLOR_PAIR(
                            10)); // P2 (Y23.4): cyan (stream-cached) — was pair 11 (playing green)
                if (y < split_y)
                    mvwprintw(win, y++, 2, " [CACHED]");
                wattroff(win, COLOR_PAIR(10));
            }
            if (selected_node->is_downloaded ||
                CacheManager::instance().is_downloaded(selected_node->url)) {
                wattron(win, COLOR_PAIR(15)); // P2 (Y23.4): green (downloaded) — was pair 11
                std::string local = CacheManager::instance().get_local_file(selected_node->url);
                if (!local.empty()) {
                    if (y < split_y)
                        mvwprintw(win, y++, 2, " [DOWNLOADED: %s]",
                                  Utils::truncate_by_display_width(local, safe_cw - 15).c_str());
                } else {
                    if (y < split_y)
                        mvwprintw(win, y++, 2, " [DOWNLOADED]");
                }
                wattroff(win, COLOR_PAIR(15));
            }

            if (URLClassifier::is_youtube(url_type)) {
                wattron(win, COLOR_PAIR(12));
                mvwprintw(win, y++, 2, " [YouTube]");
                wattroff(win, COLOR_PAIR(12));
            } else if (url_type == URLType::VIDEO_FILE) {
                wattron(win, COLOR_PAIR(12));
                mvwprintw(win, y++, 2, " [Video]");
                wattroff(win, COLOR_PAIR(12));
            }
        } // Y01: close else (non-account generic INFO block)
    }

    // 7-line play context: 3 history (global) + current + 3 next (per play_mode).
    //   - history_titles: up to 3 most-recent global history titles (excludes current)
    //   - current: playlist[playlist_index].title (highlighted)
    //   - next_indices: up to 3 upcoming indices into `playlist`
    //       REPEAT → current x3, CYCLE → current+1..+3, SHUFFLE → pre-generated lookahead
    if (!playlist.empty() && playlist_index >= 0 &&
        playlist_index < static_cast<int>(playlist.size()) && y < split_y - 3) {
        y++;
        // F35: theme-fg pair (not A_BOLD — see PLAYER STATUS note) + rename to "Play Index".
        //   [%s] stays DYNAMIC: reflects the current play mode (Repeat/Shuffle/Cycle).
        wattron(win, COLOR_PAIR(PAIR_BORDER_STD));
        const char *m_name = (play_mode == PlayMode::REPEAT)    ? "Repeat"
                             : (play_mode == PlayMode::SHUFFLE) ? "Shuffle"
                                                                : "Cycle";
        mvwprintw(win, y++, 2, "--- Playlist Index [%s] ---", m_name);
        wattroff(win, COLOR_PAIR(PAIR_BORDER_STD));

        // 3 history (most recent first → print top-down oldest→newest so it reads
        //   upward toward the current item)
        for (int k = static_cast<int>(history_titles.size()) - 1; k >= 0 && y < split_y - 2; --k) {
            std::string line = "  ↑ " + history_titles[k];
            wattron(win, A_DIM);
            mvwprintw(win, y++, 2, "%s", Utils::truncate_by_display_width(line, safe_cw).c_str());
            wattroff(win, A_DIM);
        }

        // current — prefix padded to 4 display cols (" 🔊 " == "  ↑ " == "  ↓ ") so the
        //   TITLE column is left-aligned across all 7 rows.
        {
            std::string line = " 🔊 " + playlist[playlist_index].title;
            wattron(win, COLOR_PAIR(11) | A_BOLD); // green + bold
            mvwprintw(win, y++, 2, "%s", Utils::truncate_by_display_width(line, safe_cw).c_str());
            wattroff(win, COLOR_PAIR(11) | A_BOLD);
        }

        // 3 next
        for (size_t k = 0; k < next_indices.size() && y < split_y - 2; ++k) {
            int idx = next_indices[k];
            std::string title =
                (idx >= 0 && idx < static_cast<int>(playlist.size())) ? playlist[idx].title : "?";
            std::string line = "  ↓ " + title;
            mvwprintw(win, y++, 2, "%s", Utils::truncate_by_display_width(line, safe_cw).c_str());
        }
        y++;
    }

    // When split_y == -1, skip the separator line (not drawn for small windows).
    // The separator line itself (├─┤) is drawn uniformly by protect_border; here we write the title.
    // Y24.43: the INFO/LOG embedded LYRIC region was REMOVED — subtitles now show only in the
    //   bottom LYRIC panel (L key) or the mpv video window. This separator is always "Event Log".
    int lyric_h =
        0; // EventLog upper-bound offset (0 = no lyric region; kept for the log loop bound)
    if (split_y > 0) {
        mvwprintw(win, split_y, 2, " Event Log ");
    }

    // V2.39: EVENT LOG scrolling display, no truncation
    auto logs = EventLog::instance().get();
    int current_log_y = border_bottom - 1;

    static auto last_scroll_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (scroll_mode_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_scroll_time).count() >
            200) {
        //Use LayoutMetrics to manage scroll offsets uniformly
        layout_.increment_log_scroll_offset();
        last_scroll_time = now;
    }

    //Use LayoutMetrics to compute the net available width of the log area
    int timestamp_width = 14;
    int log_msg_width = layout_.get_log_available_width(timestamp_width);
    bool osc8 = url_hyperlink_;
    int lby = 0, lbx = 0;
    getbegyx(win, lby, lbx); // abs origin for OSC 8 positioning
    int log_link_id = 0;     // Y24.15: distinct id per log URL (underline sync within a link)

    for (size_t i = 0; i < logs.size() && current_log_y > split_y + lyric_h && split_y > 0; ++i) {
        // When split_y<=0 (small window, no separator line) skip log drawing,
        //   otherwise current_log_y > -1 is always true and logs would draw from bottom to top, covering the whole info area
        const LogEntry &entry = logs[i];

        // V2.39: scroll-display the log message
        std::string msg_display;
        int msg_len = Utils::utf8_display_width(entry.message);

        if (scroll_mode_ && msg_len > log_msg_width) {
            //Fix Chinese scrolling hitting the border
            //Use LayoutMetrics to manage scroll offsets uniformly
            msg_display = Utils::get_scrolling_text(entry.message, log_msg_width,
                                                    layout_.get_log_scroll_offset() / 5);
        } else {
            msg_display = Utils::truncate_by_display_width(entry.message, log_msg_width);
        }

        int line_row = current_log_y;
        // V2.39: timestamp + space + message, with extra space for alignment
        mvwprintw(win, current_log_y--, 2, "%s %s", entry.timestamp.c_str(), msg_display.c_str());

        // Y24.12: OSC 8 link for a URL inside the log message. The message is printed at
        //   col 2 + timestamp_width + 1; locate the URL within msg_display and link that span
        //   to the FULL url (from entry.message, so truncation doesn't break the link target).
        //   Skip in scroll_mode (the URL's on-screen position shifts each tick — too fragile).
        if (osc8 && !scroll_mode_) {
            size_t sp = entry.message.find("https://");
            if (sp == std::string::npos)
                sp = entry.message.find("http://");
            if (sp != std::string::npos) {
                size_t end = entry.message.find_first_of(" \t\r\n", sp);
                std::string full_url = entry.message.substr(
                    sp, end == std::string::npos ? std::string::npos : end - sp);
                size_t mp = msg_display.find("https://");
                if (mp == std::string::npos)
                    mp = msg_display.find("http://");
                if (mp != std::string::npos) {
                    int prefix_w = Utils::utf8_display_width(msg_display.substr(0, mp));
                    std::string visible =
                        msg_display.substr(mp); // may be truncated — that's the drawn text
                    // abs col = lbx + 2(indent) + timestamp_width + 1(space) + prefix_w ; CUP is 1-based
                    pending_osc8_.push_back({lby + line_row + 1,
                                             lbx + 2 + timestamp_width + 1 + prefix_w + 1, visible,
                                             full_url, std::to_string(++log_link_id)});
                }
            }
        }
    }

    //Right panel border protection
    // Title embed region: columns 2 to 14 (" INFO & LOG " = 12 chars + 2 spaces)
    // Separator embed region: columns 2 to 13 (" Event Log " = 11 chars + 2 spaces)
    int right_ww = getmaxx(win);
    int right_wh = getmaxy(win);
    protect_border(win, right_ww, right_wh, TITLE_EMBED_START, TITLE_EMBED_END, split_y,
                   TITLE_EMBED_START, SPLIT_TITLE_EMBED_END);
}

} // namespace panicast
