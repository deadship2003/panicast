// Color pair number constants: centralizes pair number magic numbers scattered across the code.
//   10-15: Node tree status colors (init_state_color_pairs initializes)
//   20-21: Border colors (apply_theme initializes)
//   1-256: 256-color palette (init_pair(i+1, i, -1), generic, not individually named here)
#pragma once

namespace panicast
{

// ─── Node tree status color pairs (pair 10-15)──────────────────────────
constexpr int PAIR_STATUS_STREAM = 10;     // Stream cache cyan
constexpr int PAIR_STATUS_PLAYING = 11;    // Currently playing green (draw_line overlays A_BOLD)
constexpr int PAIR_STATUS_DB_CACHED = 12;  // Database cache yellow
constexpr int PAIR_STATUS_PARSE_FAIL = 13; // Parse failed red
constexpr int PAIR_STATUS_INFO = 14;       // Info blue
constexpr int PAIR_STATUS_DOWNLOADED = 15; // Downloaded green (default)

// ─── Border color pairs (pair 20-21)────────────────────────────────
constexpr int PAIR_BORDER_STD = 20;  // Standard border (foreground color)
constexpr int PAIR_BORDER_INFO = 21; // Info border (dark cyan / light blue)

} // namespace panicast
