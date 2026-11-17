// Download progress management implementation.
#include "panicast/app/progress.h"

#include <cmath>     // std::isnan/std::isinf
#include <algorithm> // std::sort

#include <fmt/format.h>

#include "panicast/core/logger.h"

namespace panicast
{

// Format download speed (internal helper)
static std::string format_speed(double bytes_per_sec) {
    if (bytes_per_sec < 0 || std::isnan(bytes_per_sec) || std::isinf(bytes_per_sec)) {
        return "...";
    }
    if (bytes_per_sec < 1024) {
        return fmt::format("{:.0f}B/s", bytes_per_sec);
    } else if (bytes_per_sec < 1024 * 1024) {
        return fmt::format("{:.1f}KB/s", bytes_per_sec / 1024);
    } else if (bytes_per_sec < 1024 * 1024 * 1024) {
        return fmt::format("{:.1f}MB/s", bytes_per_sec / (1024 * 1024));
    } else {
        return fmt::format("{:.1f}GB/s", bytes_per_sec / (1024 * 1024 * 1024));
    }
}

// Format ETA time (internal helper)
static std::string format_eta(int seconds) {
    if (seconds <= 0 || seconds > 86400)
        return "--:--";
    int hours = seconds / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;
    if (hours > 0) {
        return fmt::format("{}:{:02d}:{:02d}", hours, mins, secs);
    } else {
        return fmt::format("{}:{:02d}", mins, secs);
    }
}

ProgressManager &ProgressManager::instance() {
    static ProgressManager pm;
    return pm;
}

std::string ProgressManager::start_download(const std::string &title, const std::string &url,
                                            bool is_youtube, StartResult *result) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (result)
        *result = StartResult::NEW;
    // Dedup by URL: never add a second line for the same program. Reuse / reset the existing entry.
    if (!url.empty()) {
        for (auto &[id, p] : downloads_) {
            if (p.url != url)
                continue;
            if (p.active) {
                // Same program is already downloading — keep the existing line, don't spawn a duplicate.
                if (result)
                    *result = StartResult::REUSED_ACTIVE;
                return id;
            }
            // A previous (failed/succeeded) entry for this URL exists — reset it in place for a retry,
            //   so the UI reuses the same line instead of stacking duplicates. Slot count unchanged.
            p.active = true;
            p.complete = false;
            p.failed = false;
            p.percent = 0;
            p.status = "Retrying...";
            p.speed = "";
            p.eta_seconds = 0;
            p.downloaded_bytes = 0;
            p.total_bytes = 0;
            p.title = title; // refresh (title may have changed)
            p.is_youtube = is_youtube;
            p.complete_time = std::chrono::steady_clock::time_point{};
            if (result)
                *result = StartResult::RESET_EXISTING;
            return id;
        }
    }
    std::string id = fmt::format("dl_{}", counter_++);
    downloads_[id] = {id,
                      title,
                      url,
                      "Starting...",
                      0,
                      "",
                      0,
                      0,
                      0,
                      true,
                      false,
                      false,
                      is_youtube,
                      ++seq_counter_,
                      std::chrono::steady_clock::now()};
    return id;
}

void ProgressManager::update(const std::string &id, int percent, const std::string &status,
                             const std::string &speed, int eta_seconds, int64_t downloaded_bytes,
                             int64_t total_bytes) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (downloads_.count(id)) {
        downloads_[id].percent = percent;
        downloads_[id].status = status;
        downloads_[id].speed = speed;
        downloads_[id].eta_seconds = eta_seconds;
        downloads_[id].downloaded_bytes = downloaded_bytes;
        downloads_[id].total_bytes = total_bytes;
    }
}

void ProgressManager::complete(const std::string &id, bool success) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (downloads_.count(id)) {
        downloads_[id].active = false;
        downloads_[id].complete = true;
        downloads_[id].failed = !success;
        downloads_[id].percent = 100;
        downloads_[id].status = success ? "Complete" : "Failed";
        downloads_[id].eta_seconds = 0;
        downloads_[id].complete_time = std::chrono::steady_clock::now();
        downloads_[id].speed = success ? "Done" : "Error";
    }
}

std::vector<DownloadProgress> ProgressManager::get_all() {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<DownloadProgress> result;
    auto now = std::chrono::steady_clock::now();

    for (auto &[id, p] : downloads_) {
        result.push_back(p);
    }
    // Order by creation sequence (not by id string) so the list is chronological: new downloads
    //   append at the end, retries stay in their original position. Avoids the lexicographic
    //   dl_0/dl_1/dl_10/dl_2 jumble that looked random.
    std::sort(result.begin(), result.end(),
              [](const DownloadProgress &a, const DownloadProgress &b) { return a.seq < b.seq; });

    // Clean up completed downloads after a specified duration.
    //   Failures are kept much longer than successes so the user can see [FAIL] (a 5s flash is useless);
    //   a failed entry is also reclaimed as soon as the same URL is retried (see start_download dedup).
    for (auto it = downloads_.begin(); it != downloads_.end();) {
        if (it->second.complete && !it->second.active) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(now - it->second.complete_time)
                    .count();
            int keep_sec =
                it->second.failed ? DOWNLOAD_FAILED_DISPLAY_SEC : DOWNLOAD_COMPLETE_DISPLAY_SEC;
            if (elapsed > keep_sec) {
                it = downloads_.erase(it);
                continue;
            }
        }
        ++it;
    }
    return result;
}

int curl_progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                           curl_off_t ulnow) {
    (void)ultotal; // Standard libcurl parameter, currently unused
    (void)ulnow;   // Standard libcurl parameter, currently unused
    CurlProgressData *data = static_cast<CurlProgressData *>(clientp);
    if (!data || data->dl_id.empty())
        return 0;

    auto now = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - data->last_time).count();

    // Update progress every 200ms to avoid frequent updates
    if (elapsed < 200 && dlnow < dltotal)
        return 0;

    // Overall percentage = (resume offset + downloaded this session) / (resume offset + total this session)
    curl_off_t overall_total = data->resume_offset + dltotal;
    curl_off_t overall_done = data->resume_offset + dlnow;
    int percent = 0;
    if (overall_total > 0) {
        percent = static_cast<int>((overall_done * 100) / overall_total);
        if (percent > 100)
            percent = 100;
    }

    // Calculate download speed
    double bytes_per_sec = 0;
    int eta_seconds = 0;
    if (elapsed > 0) {
        int64_t bytes_diff = dlnow > data->last_bytes
                                 ? dlnow - data->last_bytes
                                 : 0; // Clamp negative values (redirect / resume rollback)
        bytes_per_sec = (bytes_diff * 1000.0) / elapsed;

        // Calculate ETA
        if (bytes_per_sec > 0 && dltotal > dlnow) {
            int64_t remaining_bytes = dltotal - dlnow;
            eta_seconds = static_cast<int>(remaining_bytes / bytes_per_sec);
        }
    }

    // Update progress
    std::string speed_str = format_speed(bytes_per_sec);
    ProgressManager::instance().update(data->dl_id, percent,
                                       fmt::format("{}", format_eta(eta_seconds)), speed_str,
                                       eta_seconds, dlnow, dltotal);

    // Update previous state
    data->last_bytes = dlnow;
    data->last_time = now;

    return 0;
}

} // namespace panicast
