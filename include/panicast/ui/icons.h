// UI icon style system: dual-mode (ASCII/Emoji) icons and widths.
#pragma once

#include <string>

#include "panicast/core/terminal.h" // get_emoji_width()
#include "panicast/core/types.h"    // MediaType

namespace panicast
{

// =========================================================
// UI icon style system - dual-mode support (ASCII/Emoji)
// =========================================================
// Background:
//   Emoji display width varies across terminals (1 or 2 columns), breaking borders
//   wcwidth() return value != actual display width (often mismatches for Emoji)
//   Press U to toggle icon style
// =========================================================

enum class IconStyle { ASCII, EMOJI };
// Global icon style (defined in icons.cpp, singleton-style shared)
extern IconStyle g_icon_style;

// Safe layout constants (icon-related)
constexpr int ICON_FIELD_WIDTH =
    3; // Fixed width of icon area (icon + space, emoji width 2 + space 1 = 3)
constexpr int EMOJI_LOGICAL_WIDTH = 2; // Emoji logical width
constexpr int ASCII_LOGICAL_WIDTH = 1; // ASCII logical width

// Icon manager - supports ASCII/Emoji dual mode
// Icon format: <icon><space>, keep 1 space separation from connector
class IconManager {
public:
    static void set_style(IconStyle style) {
        g_icon_style = style;
    }
    static IconStyle get_style() {
        return g_icon_style;
    }
    static void toggle_style() {
        g_icon_style = (g_icon_style == IconStyle::EMOJI) ? IconStyle::ASCII : IconStyle::EMOJI;
    }
    static std::string get_style_name() {
        return (g_icon_style == IconStyle::EMOJI) ? "Emoji" : "ASCII";
    }

    // Node state icons
    static std::string get_folder_expanded() {
        return (g_icon_style == IconStyle::EMOJI) ? "▼ " : "v ";
    }
    static std::string get_folder_collapsed() {
        return (g_icon_style == IconStyle::EMOJI) ? "▶ " : "> ";
    }
    static std::string get_marked() {
        return (g_icon_style == IconStyle::EMOJI) ? "🔖 " : "* ";
    }
    static std::string get_failed() {
        return (g_icon_style == IconStyle::EMOJI) ? "✗ " : "x ";
    }
    static std::string get_loading() {
        return (g_icon_style == IconStyle::EMOJI) ? "⏳ " : "~ ";
    }

    // Node type icons
    static std::string get_radio() {
        return (g_icon_style == IconStyle::EMOJI) ? "🎵 " : "[R]";
    }
    static std::string get_podcast() {
        // 🎙(U+1F399) glibc wcwidth=1 but terminal renders as emoji(2) -> cursor misalignment
        //   causing the right border │ of that line to misalign. Switched to 🎤(U+1F3A4, glibc=2),
        //   also a microphone and glibc width=2, matching the terminal, no misalignment.
        return (g_icon_style == IconStyle::EMOJI) ? "🎤 " : "[P]";
    }
    static std::string get_video() {
        return (g_icon_style == IconStyle::EMOJI) ? "🎬 " : "[V]";
    }
    // N04: IPTV channel leaf — television icon (IPTV channels are TV stations).
    static std::string get_tv() {
        return (g_icon_style == IconStyle::EMOJI) ? "📺 " : "[T]";
    }
    // Y23.1: search-result type icons (all glibc wcwidth=2, consistent with the project's emoji set).
    //   Video results reuse get_video() (📺) so a search-result video looks the same as a P-mode video.
    static std::string
    get_creator() { // YouTube channel / Bilibili UP (a person/channel, not a video)
        return (g_icon_style == IconStyle::EMOJI) ? "👤 " : "[U]";
    }
    static std::string get_playlist() { // YouTube playlist
        return (g_icon_style == IconStyle::EMOJI) ? "📋 " : "[L]";
    }
    static std::string get_music() { // YouTube music-category video
        return (g_icon_style == IconStyle::EMOJI) ? "🎵 " : "[M]";
    }
    // ===== CONVENTION: every icon/emoji glyph below MUST have glibc wcwidth == 2. =====
    // ncurses renders via libc wcwidth (it advances its cursor by glibc's per-codepoint width).
    // If a glyph is glibc-width-1 but the terminal draws it width-2 (e.g. 🅱 U+1F171, 🅈 U+1F148,
    // ▶ U+25B6), ncurses's cursor desynchronizes from the terminal → the row's content shifts and
    // the border goes misaligned. The simple, uniform fix is to ONLY use width-2 glyphs:
    //   - standard emoji that glibc already reports as 2 (📻 📹 📺 🎤 🎬 🎵 🎶 🎥 👤 📋 …), or
    //   - CJK fullwidth letters (Ｂ U+FF22, Ｙ U+FF39 — always glibc-width-2, render width-2 everywhere).
    // Do NOT use width-1 emoji; pick a width-2 alternative instead. (Bilibili therefore uses the
    // fullwidth Ｂ — there is no glibc-width-2 "🅱" emoji.)
    static std::string media_type_icon(MediaType t) {
        if (g_icon_style != IconStyle::EMOJI) {
            switch (t) {
            case MediaType::Radio:
                return "[R] ";
            case MediaType::Youtube:
                return "[Y] ";
            case MediaType::Bilibili:
                return "[B] ";
            case MediaType::Tiktok:
                return "[K] ";
            case MediaType::Iptv:
                return "[T] ";
            case MediaType::OnlineAudio:
                return "[A] ";
            case MediaType::OnlineVideo:
                return "[V] ";
            case MediaType::LocalAudio:
                return "[a] ";
            case MediaType::LocalVideo:
                return "[v] ";
            }
        }
        switch (t) {
        case MediaType::Radio:
            return "📻 ";
        case MediaType::Youtube:
            return "📹 ";
        case MediaType::Bilibili:
            return "Ｂ ";
        case MediaType::Tiktok:
            return "🎵 ";
        case MediaType::Iptv:
            return "📺 ";
        case MediaType::OnlineAudio:
            return "🎤 ";
        case MediaType::OnlineVideo:
            return "🎬 ";
        case MediaType::LocalAudio:
            return "🎶 ";
        case MediaType::LocalVideo:
            return "🎥 ";
        }
        return "🎵 ";
    }
    // Note: get_music/get_playing/get_paused/get_stopped/get_buffering/get_cached/get_downloading
    // removed (zero call sites, dead code). Playback status symbols ▶/‖ etc. are used directly in draw_info/draw_status.

    // Icon display width
    static int get_icon_display_width() {
        if (g_icon_style == IconStyle::EMOJI) {
            return get_emoji_width();
        }
        return ASCII_LOGICAL_WIDTH;
    }

    // Total width of icon area (including trailing space; the 1 space before and after emoji is provided by connector and this space)
    //   Field width = measured emoji width + 1, reserved by actual occupied width to ensure title alignment across rows.
    static int get_icon_field_width() {
        if (g_icon_style == IconStyle::EMOJI) {
            return get_emoji_width() + 1; // Icon (actual width) + 1 space
        }
        return 2; // ASCII: 1 char + space = 2
    }
};

} // namespace panicast
