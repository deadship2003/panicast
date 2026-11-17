// Unified layout manager: couples window resize with scrolling display, includes safe-width calculation.
#pragma once

#include <map>

#include <ncurses.h>

#include "panicast/config/ini_config.h"

namespace panicast
{

// Safe layout constants (right-panel safe buffer)
constexpr int SAFE_BORDER_MARGIN =
    0; // Right border safe buffer columns (2b3 note: 0 = content sits 1 cell before border, capped at 1-cell gap, avoids wide-char + margin causing 2 cells)

// Event log timestamp display width
constexpr int TIMESTAMP_WIDTH = 14;

// =========================================================
// Unified layout manager - couples window resize with scrolling display
// =========================================================
// Design goals:
//   1. Centrally manage size calculations for all UI regions
//   2. Detect window size changes and auto-reset scroll offsets
//   3. Provide a standard available net-width interface
//   4. Ensure display consistency across modes (RADIO/PODCAST/FAVOURITE/HISTORY)
//   5. V0.05B6 addition: safe-margin mechanism to prevent emoji width mismatches from breaking borders

// Usage flow:
//   1. Call check_resize() at the start of each frame to detect size changes
//   2. If size changed, auto-reset all scroll offsets
//   3. Call the get_*_width() interfaces to get the available net width
//   4. Scrolling uses unified offset management

// Safe-margin design principles:
//   - Actual drawable area = window width - safe buffer columns
//   - Icon area fixed width = 3 columns (space + icon + space)
//   - Out-of-bounds clipping performed before every output
// =========================================================
class LayoutMetrics {
public:
    // Layout parameters
    static constexpr float DEFAULT_LAYOUT_RATIO = 0.25f;
    static constexpr int STATUS_BAR_HEIGHT = 3;
    static constexpr int MIN_CONTENT_WIDTH = 10;
    // Safe-buffer constants
    static constexpr int SAFE_MARGIN = SAFE_BORDER_MARGIN; // Right border safe buffer columns

    // Size struct
    struct WindowSize {
        int lines = 0;
        int cols = 0;
        bool changed = false; // Whether a change was detected this check
    };

    struct PanelMetrics {
        int left_w = 0;         // Left panel width
        int right_w = 0;        // Right panel width
        int top_h = 0;          // Top height (excluding status bar)
        int content_w = 0;      // Left panel content area width (minus border)
        int safe_content_w = 0; // Safe content area width (minus border and safe buffer)
        int right_inner_w = 0;  // Right panel inner width
        int safe_right_w = 0;   // Safe right panel width
        int status_w = 0;       // Status bar width
        int safe_status_w = 0;  // Safe status bar width
    };

private:
    WindowSize last_size_;
    PanelMetrics metrics_;
    float layout_ratio_ = DEFAULT_LAYOUT_RATIO;
    bool ratio_loaded_ = false; // Lazily load INI layout_ratio

    // Scroll offset management - centrally manages all scroll state
    std::map<int, int> line_scroll_offsets_; // Per-line scroll offset for the left panel
    int log_scroll_offset_ = 0;              // Log scroll offset
    int status_scroll_offset_ = 0;           // Status bar URL scroll offset

public:
    static LayoutMetrics &instance();

    // Detect window size change - core coupling entry point
    // Returns: true means size changed and a full redraw is needed
    bool check_resize() {
        int current_lines = LINES;
        int current_cols = COLS;

        last_size_.changed = (current_lines != last_size_.lines || current_cols != last_size_.cols);

        if (last_size_.changed) {
            last_size_.lines = current_lines;
            last_size_.cols = current_cols;

            // Key - reset all scroll offsets when size changes
            // Because the available net width changed, old scroll positions may be invalid
            reset_all_scroll_offsets();

            // Recompute layout
            recalculate_metrics();
        }

        return last_size_.changed;
    }

    // Recompute layout dimensions (including safe width)
    void recalculate_metrics() {
        // Lazily load the left/right panel ratio from INI on first use
        if (!ratio_loaded_) {
            float r =
                IniConfig::instance().get_float("display", "layout_ratio", DEFAULT_LAYOUT_RATIO);
            if (r < 0.2f)
                r = 0.2f;
            if (r > 0.8f)
                r = 0.8f;
            layout_ratio_ = r;
            ratio_loaded_ = true;
        }
        metrics_.status_w = last_size_.cols;
        metrics_.top_h = last_size_.lines - STATUS_BAR_HEIGHT;
        if (metrics_.top_h < 0)
            metrics_.top_h = 0; // Tiny-terminal guard
        // Use integer arithmetic to avoid floating-point precision issues (layout_ratio_ -> percentage)
        int ratio_pct = (int)(layout_ratio_ * 100.0f + 0.5f);
        if (ratio_pct < 20)
            ratio_pct = 20;
        if (ratio_pct > 80)
            ratio_pct = 80;
        metrics_.left_w = last_size_.cols * ratio_pct / 100;
        metrics_.right_w = last_size_.cols - metrics_.left_w;

        // Content width = window width - left/right borders (2)
        metrics_.content_w = metrics_.left_w - 2;
        if (metrics_.content_w < MIN_CONTENT_WIDTH) {
            metrics_.content_w = MIN_CONTENT_WIDTH;
        }

        // Safe content width = content width - safe buffer (1)
        // Used to prevent emoji width mismatches from breaking the border
        metrics_.safe_content_w = metrics_.content_w - SAFE_MARGIN;
        if (metrics_.safe_content_w < MIN_CONTENT_WIDTH) {
            metrics_.safe_content_w = MIN_CONTENT_WIDTH;
        }

        // Fix right-panel inner width calculation
        // Content width = right_w - 2 (left/right borders, 1 column each)
        // Printing starts at column 2, available width = right_w - 3
        metrics_.right_inner_w = metrics_.right_w - 3;
        if (metrics_.right_inner_w < MIN_CONTENT_WIDTH) {
            metrics_.right_inner_w = MIN_CONTENT_WIDTH;
        }

        // Safe right-panel width
        metrics_.safe_right_w = metrics_.right_inner_w - SAFE_MARGIN;
        if (metrics_.safe_right_w < MIN_CONTENT_WIDTH) {
            metrics_.safe_right_w = MIN_CONTENT_WIDTH;
        }

        // Safe status bar width
        metrics_.safe_status_w = metrics_.status_w - SAFE_MARGIN;
        if (metrics_.safe_status_w < 1)
            metrics_.safe_status_w = 1; // Clamp lower bound
    }

    // Reset all scroll offsets - called when size changes
    void reset_all_scroll_offsets() {
        line_scroll_offsets_.clear();
        log_scroll_offset_ = 0;
        status_scroll_offset_ = 0;
    }

    // Get layout dimensions
    const PanelMetrics &get_metrics() const {
        return metrics_;
    }
    const WindowSize &get_window_size() const {
        return last_size_;
    }

    // Removed set_layout_ratio (grep verified 0 call sites; layout_ratio_ is set during init)

    // Removed get_title_available_width (0 call sites)

    // Compute the available net width for the right-panel log area (uses safe width)
    // Parameter: timestamp_width - width occupied by the timestamp
    int get_log_available_width(int timestamp_width = TIMESTAMP_WIDTH) const {
        // Use safe right-panel width
        int available = metrics_.safe_right_w - timestamp_width - 1;
        return (available > 0) ? available : 1;
    }

    // Removed get_status_available_width (0 call sites, marked below)

    // Scroll offset management interfaces
    int get_line_scroll_offset(int line_y) const {
        auto it = line_scroll_offsets_.find(line_y);
        return (it != line_scroll_offsets_.end()) ? it->second : 0;
    }

    void increment_line_scroll_offset(int line_y) {
        line_scroll_offsets_[line_y]++;
    }

    int get_log_scroll_offset() const {
        return log_scroll_offset_;
    }
    void increment_log_scroll_offset() {
        log_scroll_offset_++;
    }

    // Removed get_status_scroll_offset / increment_status_scroll_offset (0 call sites)
    // Removed get_status_available_width (0 call sites)

    // Direct access to the scroll offset table (for compatibility with existing code)
    std::map<int, int> &get_line_scroll_offsets() {
        return line_scroll_offsets_;
    }
};

} // namespace panicast
