// Status bar art character rendering: returns art characters based on IconStyle.
#pragma once

#include "panicast/ui/icons.h" // IconStyle, g_icon_style

namespace panicast
{

// =========================================================
// Status bar art effect modular configuration
// Switched to function-style access, supports ASCII/Emoji switching
// Users can customize art characters for personalization
// =========================================================
// Left art structure: ART_OUTER_LEFT + version + ART_OUTER_RIGHT
// Right art structure: ART_RIGHT_PREFIX + author/time + ART_RIGHT_SUFFIX
// Middle structure: ART_BRACKET_LEFT + content + ART_BRACKET_RIGHT

// ArtRenderer returns art characters based on IconStyle
// Emoji mode: 🎵 ♫ (original visuals)
// ASCII mode: *~ (no emoji, avoids WSL2 width issues)
class ArtRenderer {
public:
    static const char *outer_left() {
        return (g_icon_style == IconStyle::EMOJI) ? "🎵 ♫ " : "*~ ";
    }
    static const char *outer_right() {
        return (g_icon_style == IconStyle::EMOJI) ? " ♫ 🎵" : " ~*";
    }
    static const char *right_prefix() {
        return (g_icon_style == IconStyle::EMOJI) ? "🎵 ♫ " : "*~ ";
    }
    static const char *right_suffix() {
        return (g_icon_style == IconStyle::EMOJI) ? " ♫ 🎵" : " ~*";
    }
    static const char *bracket_left() {
        return "[ ";
    }
    static const char *bracket_right() {
        return " ]";
    }
};

// Keep legacy constant names for compatibility (point to Emoji versions, used only by unmigrated code paths)
constexpr const char *ART_OUTER_LEFT = "🎵 ♫ ";
constexpr const char *ART_OUTER_RIGHT = " ♫ 🎵";
constexpr const char *ART_BRACKET_LEFT = "[ ";
constexpr const char *ART_BRACKET_RIGHT = " ]";
constexpr const char *ART_RIGHT_PREFIX = "🎵 ♫ ";
constexpr const char *ART_RIGHT_SUFFIX = " ♫ 🎵";

} // namespace panicast
