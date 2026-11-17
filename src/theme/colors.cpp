#include "panicast/theme/colors.h"

#include <chrono>
#include <map>
#include <mutex>
#include <random>

#include "panicast/core/platform.h"

namespace panicast
{

int StatusBarColorRenderer::get_color(const StatusBarColorConfig &config, int offset) {
    static std::mutex
        mtx; // P3-8: Multi-core concurrency protection for in-function static state (hue/last_update)
    std::lock_guard<std::mutex> lk(mtx);
    static int hue = 0;
    static auto last_update = std::chrono::steady_clock::now();

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update).count();

    if (elapsed >= config.update_interval_ms) {
        hue = (hue + config.rainbow_speed) % 360;
        last_update = now;
    }

    float brightness = calculate_brightness(config);

    switch (config.mode) {
    case StatusBarColorMode::RAINBOW:
        return get_rainbow_color(hue + offset, brightness);
    case StatusBarColorMode::RANDOM:
        return get_random_color(brightness);
    case StatusBarColorMode::TIME_BRIGHTNESS:
        return get_time_adjusted_color(brightness);
    case StatusBarColorMode::FIXED:
        return get_fixed_color(config.fixed_color, brightness);
    case StatusBarColorMode::CUSTOM:
        // Cycle through the user-defined color sequence
        return get_custom_color(config, offset);
    default:
        return get_rainbow_color(hue + offset, brightness);
    }
}

float StatusBarColorRenderer::calculate_brightness(const StatusBarColorConfig &config) {
    float brightness = config.brightness_max;

    if (config.time_adjust) {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_local;
        localtime_local(&t, &tm_local); // Thread-safe
        int hour = tm_local.tm_hour;

        if (hour >= 22 || hour < 6) {
            brightness = config.brightness_min;
        } else if (hour >= 18 || hour < 8) {
            brightness = (config.brightness_min + config.brightness_max) / 2;
        }
    }

    return brightness;
}

int StatusBarColorRenderer::get_rainbow_color(int hue, float brightness) {
    float h = hue / 60.0f;
    int i = static_cast<int>(h) % 6;
    float f = h - static_cast<int>(h);
    float v = brightness;
    float s = 1.0f;

    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    float r, g, b;
    switch (i) {
    case 0:
        r = v;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    default:
        r = v;
        g = p;
        b = q;
        break;
    }

    return 16 + static_cast<int>(r * 5) * 36 + static_cast<int>(g * 5) * 6 +
           static_cast<int>(b * 5);
}

int StatusBarColorRenderer::get_random_color(float brightness) {
    (void)brightness;      // Parameter reserved for future use
    static std::mutex mtx; // P3-8: Multi-core concurrency protection for mt19937/distributor
    std::lock_guard<std::mutex> lk(mtx);
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(16, 231);
    return dis(gen);
}

int StatusBarColorRenderer::get_time_adjusted_color(float brightness) {
    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    int hue = (seconds / 10) % 360;
    return get_rainbow_color(hue, brightness);
}

int StatusBarColorRenderer::get_fixed_color(const std::string &color_name, float brightness) {
    (void)brightness; // Parameter reserved for future use
    static std::map<std::string, int> color_map = {
        {"black", COLOR_BLACK},   {"red", COLOR_RED},     {"green", COLOR_GREEN},
        {"yellow", COLOR_YELLOW}, {"blue", COLOR_BLUE},   {"magenta", COLOR_MAGENTA},
        {"cyan", COLOR_CYAN},     {"white", COLOR_WHITE},
    };

    auto it = color_map.find(color_name);
    if (it != color_map.end())
        return it->second;
    return COLOR_CYAN;
}

int StatusBarColorRenderer::get_custom_color(const StatusBarColorConfig &config, int offset) {
    if (config.custom_colors.empty()) {
        // Fall back to CYAN when no color sequence is configured
        return COLOR_CYAN;
    }
    int group = offset / std::max(1, config.custom_speed);
    int idx = group % static_cast<int>(config.custom_colors.size());
    return config.custom_colors[idx];
}

} // namespace panicast
