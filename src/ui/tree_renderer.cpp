// UI rendering layer — extracted implementation unit (Y24.33–Y24.36).
//   Methods remain UI members (they touch private UI state); only their
//   implementations live here. Declarations stay in ui.h.
#include "panicast/ui/ui.h"

#include <algorithm>
#include <string>

#include <ncurses.h>

namespace panicast
{

void UI::draw_line(WINDOW *win, int y, const DisplayItem &item, bool selected, bool in_visual,
                   int max_len, const std::string &current_url) {
    (void)max_len; // keep the parameter for future use (internally uses title_max_len)
    // V2.39-FF: null-pointer check
    if (!item.node)
        return;

    //Node-tree indentation computation detailed explanation
    // ============================================
    // Node hierarchy example (1 space before and after emoji: connector trailing space + icon trailing space):
    // depth=0 (root): no prefix, no connector, has icon
    // depth=1: prefix=" "(1) + connector="├─ "(3) + icon_field = ...
    // depth=N: prefix=N + connector=3 + icon_field
    // icon field width = measured emoji width + 1 (reserve per actual footprint, to keep titles aligned across rows):
    // - ▼/▶ (folder): width-1 symbol + 1 space padded to field width
    // - 📺/🎵/🎤 (emoji): width 2 + 1 space = emoji_width(2)+1
    // 🎙(glibc=1)->🎤(glibc=2), matches the terminal's measured width, cursor does not misalign -> rows aligned.

    //Net-space computation formula
    // available net width = content_width - fixed_width
    //          = (left_w - 2) - (depth*2 + 4 + icon_width)

    // where content_width = left_w - 2 (full content area width)

    std::string prefix;
    std::string connector;

    if (show_tree_lines_ && item.depth > 0) {
        //Each parent-node level takes 1 space of indentation
        for (int d = 0; d < item.depth; ++d) {
            prefix += " ";
        }
        // connector drops its trailing space. emoji is left-padded within its 2-cell width (glyph sits right),
        //   so no extra space is needed between ─ and emoji — the emoji's own left-padding cell provides 1 char of
        //   spacing, symmetrical with the 1 space on the right, avoiding a visual 2-cell gap from ─ to emoji.
        connector = item.is_last ? "└─" : "├─";
    } else {
        prefix = std::string(item.depth, ' ');
        connector = "";
    }

    std::string icon;
    std::string url = item.node->url.empty() ? "" : item.node->url;

    // Currently playing has the highest priority, highlighted in green
    bool is_currently_playing = !current_url.empty() && !url.empty() && url == current_url;

    //Distinguish full cache (downloaded) from partial cache (stream cache), using different colors
    // BUG6: a FEED (PODCAST_FEED) is "downloaded" (green) only if at least one of its children is
    //   downloaded; if its episode list is loaded but nothing is downloaded, show cyan ("metadata
    //   cached only"); if children aren't loaded, no special color. Leaf nodes keep the old rules.
    bool feed_has_downloads =
        (item.node->type == NodeType::PODCAST_FEED && item.node->children_loaded && [&] {
            for (auto &c : item.node->children)
                if (c->is_downloaded)
                    return true;
            return false;
        }());
    bool is_downloaded = (!url.empty() && (CacheManager::instance().is_downloaded(url) ||
                                           item.node->is_downloaded)) ||
                         feed_has_downloads;
    bool is_partial = !url.empty() && !is_downloaded && !feed_has_downloads &&
                      CacheManager::instance().is_partial(url);
    // "stream cached" = a feed whose episode list is loaded in memory (children_loaded) or
    //   explicitly flagged is_cached. Limited to PODCAST_FEED so episodes aren't mis-colored.
    //   (No CacheManager.is_cached / no bulk memory load — lazy, in-memory node flag.)
    bool is_stream_cached = (!url.empty() && !is_downloaded && !is_partial) &&
                            (item.node->is_cached || (item.node->type == NodeType::PODCAST_FEED &&
                                                      item.node->children_loaded));

    //Use IconManager to manage icons uniformly (unified Emoji style)
    // All icons occupy a fixed 3 columns (space + icon + space), preventing width inconsistency
    if (item.node->marked)
        icon = IconManager::get_marked();
    else if (item.node->parse_failed)
        icon = IconManager::get_failed();
    else if (item.node->loading)
        icon = IconManager::get_loading();
    else if (item.node->media_type_set && (item.node->type == NodeType::PODCAST_EPISODE ||
                                           item.node->type == NodeType::RADIO_STREAM)) {
        // N06: DB-driven leaf nodes (history / favourites) carry a stored MediaType — render
        //   its icon directly instead of re-inferring from the URL every frame.
        icon = IconManager::media_type_icon(item.node->media_type);
    } else if (item.node->is_yt_search_result) {
        // Y23.1: type-aware icon for search results (title no longer carries an emoji prefix —
        //   the icon is the single type indicator). Video reuses get_video() (🎬) for consistency
        //   with P-mode/downloaded videos; channel/UP=👤, playlist=📋, music=🎵.
        if (item.node->is_yt_playlist)
            icon = IconManager::get_playlist();
        else if (item.node->is_yt_music)
            icon = IconManager::get_music();
        else if (item.node->is_yt_channel)
            icon = IconManager::get_creator();
        else if (item.node->type == NodeType::PODCAST_EPISODE)
            icon = IconManager::get_video();
        else
            icon = item.node->expanded ? IconManager::get_folder_expanded()
                                       : IconManager::get_folder_collapsed();
    } else if (item.node->is_bili_up) {
        // Y23.2: a Bilibili UP-master node (followings / subscribed / search) → 👤.
        icon = IconManager::get_creator();
    } else if (item.node->is_iptv_channel) {
        // N04: IPTV channel leaf → 📺 (television).
        icon = IconManager::get_tv();
    } else if (item.node->type == NodeType::FOLDER || item.node->type == NodeType::PODCAST_FEED)
        icon = item.node->expanded ? IconManager::get_folder_expanded()
                                   : IconManager::get_folder_collapsed();
    else if (item.node->type == NodeType::RADIO_STREAM) {
        // V2.39-FF: radio nodes show different icons depending on whether they have children
        if (!item.node->children.empty() || !item.node->children_loaded) {
            // Has children or not yet loaded: show folder icon
            icon = item.node->expanded ? IconManager::get_folder_expanded()
                                       : IconManager::get_folder_collapsed();
        } else {
            // Leaf radio node: show music icon
            icon = IconManager::get_radio();
        }
    } else if (item.node->type == NodeType::PODCAST_EPISODE) {
        // V2.39: detect video episodes
        if (!url.empty()) {
            URLType url_type = URLClassifier::classify(url);
            // Y23.1: BILIBILI_VIDEO/DOUYIN_VIDEO are video too (previously fell through to mic).
            if (item.node->is_youtube || url_type == URLType::VIDEO_FILE ||
                url_type == URLType::BILIBILI_VIDEO || url_type == URLType::DOUYIN_VIDEO)
                icon = IconManager::get_video();
            else
                icon = IconManager::get_podcast();
        } else {
            icon = IconManager::get_podcast();
        }
    }

    //Fixed icon-area width
    // Whether ASCII or Emoji, the icon area occupies a fixed ICON_FIELD_WIDTH columns
    int icon_width = IconManager::get_icon_field_width();

    //Fixed part (prefix + connector + icon), only the title scrolls
    //icon width uses a fixed value, eliminating uncertainty
    // Y16/Y23.4: if node has_subtitle, append "📜 " after the type icon (before title).
    //   The icon already ends with a trailing space (separator), so 📜 follows with NO leading
    //   space (Y23.4 fix: was " 📜 " → double space between icon and 📜 + sub_emoji_w over-count).
    std::string fixed_part = prefix + connector + icon;
    int sub_emoji_w = 0;
    if (item.node->has_subtitle) {
        if (item.node->has_asr_srt) {
            fixed_part += "\xF0\x9F\x93\x9D "; // "📝 " (U+1F4DD, ASR SRT, glibc wcwidth=2, safe)
        } else {
            fixed_part +=
                "\xF0\x9F\x93\x9C "; // "📜 " (U+1F4DC, online transcript, glibc wcwidth=2, safe)
        }
        sub_emoji_w = get_emoji_width() + 1; // emoji + trailing space
    }
    int fixed_width = Utils::utf8_display_width(prefix + connector) + icon_width + sub_emoji_w;

    //Make full use of the content area width
    //Use the safe content area width to prevent Emoji overflow
    int safe_content_width = layout_.get_metrics().safe_content_w;
    int title_max_len = safe_content_width - fixed_width;
    if (title_max_len < 1)
        title_max_len = 1; // keep at least 1 char

    std::string title_display;
    int title_width = Utils::utf8_display_width(item.node->title);
    if (scroll_mode_ && title_width > title_max_len) {
        //Scroll mode on: use the industrial-grade scrolling engine
        //Use LayoutMetrics to manage scroll offsets uniformly
        layout_.increment_line_scroll_offset(y);
        int scroll_offset = layout_.get_line_scroll_offset(y);
        title_display =
            Utils::get_scrolling_text(item.node->title, title_max_len, scroll_offset / 5);
    } else {
        //Scroll mode off: use strict truncation display
        title_display = Utils::truncate_by_display_width_strict(item.node->title, title_max_len);
    }

    //Double protection - ensure the final display strictly does not exceed the safe content area width
    std::string final_display = fixed_part + title_display;
    int final_width = Utils::utf8_display_width(final_display);
    if (final_width > safe_content_width) {
        final_display = Utils::truncate_by_display_width_strict(final_display, safe_content_width);
    }

    // 1. Selected / Visual mode - reverse video (highest priority)
    // 2. Currently playing - green + bold (bold to distinguish from "downloaded" plain green; color configurable in [colors])
    // 3. Fully downloaded - green (default; conveys "cached and available", color configurable in [colors])
    // 4. Stream cache - cyan
    // 5. DB cache - yellow
    // 6. Parse failed - red
    if (selected || in_visual)
        wattron(win, A_REVERSE);
    else if (is_currently_playing)
        wattron(win, A_BOLD | COLOR_PAIR(11)); //bold green - currently playing
    else if (is_downloaded)
        wattron(win, COLOR_PAIR(15)); //green - fully downloaded / cached
    else if (is_partial)
        wattron(win, COLOR_PAIR(16)); //yellow - incomplete download (.part)
    else if (is_stream_cached)
        wattron(win, COLOR_PAIR(10)); //cyan - stream cache
    else if (item.node->is_db_cached)
        wattron(win, COLOR_PAIR(12)); //yellow - db cache
    else if (item.node->parse_failed)
        wattron(win, COLOR_PAIR(13)); //red - parse failed

    //Print logic
    // Window structure: column 0 = left border, columns 1 to left_w-2 = content area, column left_w-1 = right border
    //Use the safe content area width to prevent Emoji overflow

    // Step 1: move to line start (column 1, content area start)
    wmove(win, y, 1);

    // Step 2: clear the whole content area with spaces (prevent residue)
    std::string clear_str(safe_content_width, ' ');
    waddstr(win, clear_str.c_str());

    // Step 3: move back to line start, print the actual content
    wmove(win, y, 1);
    //Use waddstr directly, because final_display is already strictly truncated
    // waddnstr's second parameter is byte count not display width, which causes problems for UTF-8 strings
    waddstr(win, final_display.c_str());

    if (selected || in_visual)
        wattroff(win, A_REVERSE);
    else if (is_currently_playing)
        wattroff(win, A_BOLD | COLOR_PAIR(11));
    else if (is_downloaded)
        wattroff(win, COLOR_PAIR(15));
    else if (is_partial)
        wattroff(win,
                 COLOR_PAIR(16)); // F21: was missing → partial yellow bled to all subsequent lines
    else if (is_stream_cached)
        wattroff(win, COLOR_PAIR(10));
    else if (item.node->is_db_cached)
        wattroff(win, COLOR_PAIR(12));
    else if (item.node->parse_failed)
        wattroff(win, COLOR_PAIR(13));
}

} // namespace panicast
