// Download progress management: DownloadProgress data items + ProgressManager singleton + libcurl progress callback.
//   - ProgressManager tracks percent/speed/ETA/byte count of multi-task downloads; keeps completed items for display before cleaning up
//   - CurlProgressData is the user-data carrier for libcurl's CURLOPT_XFERINFOFUNCTION
//   - curl_progress_callback computes rate/ETA and writes back to ProgressManager
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "panicast/core/constants.h"

namespace panicast
{

// Download progress item
struct DownloadProgress {
    std::string id;
    std::string title;
    std::string url;
    std::string status;
    int percent = 0;
    std::string speed;
    int eta_seconds = 0;          // Remaining time estimate (seconds)
    int64_t downloaded_bytes = 0; // Downloaded byte count
    int64_t total_bytes = 0;      // Total byte count
    bool active = true;
    bool complete = false;
    bool failed = false;
    bool is_youtube = false;
    uint64_t seq = 0; // Monotonic creation order; get_all() sorts by this so the list is
                      //   chronological (new downloads append at the end), not lexicographic by id.
    std::chrono::steady_clock::time_point complete_time; // Completion timestamp
};

class ProgressManager {
public:
    static ProgressManager &instance();

    // Start (or retry) a download. Dedup by URL: if an entry for the same URL already exists,
    //   - active   → REUSED_ACTIVE: reuse it, caller must NOT spawn a duplicate task (slot unchanged)
    //   - finished → RESET_EXISTING: reset it in place to "retrying" (same id; slot unchanged)
    //   - none     → NEW: create a new entry (slot +1)
    enum class StartResult { NEW, REUSED_ACTIVE, RESET_EXISTING };
    std::string start_download(const std::string &title, const std::string &url,
                               bool is_youtube = false, StartResult *result = nullptr);

    // Enhanced update, supports ETA and byte counting
    void update(const std::string &id, int percent, const std::string &status,
                const std::string &speed = "", int eta_seconds = 0, int64_t downloaded_bytes = 0,
                int64_t total_bytes = 0);

    // Record timestamp on completion
    void complete(const std::string &id, bool success);

    // Improved cleanup logic: completed downloads are kept for 5 seconds before being removed
    std::vector<DownloadProgress> get_all();

private:
    ProgressManager() {}
    std::map<std::string, DownloadProgress> downloads_;
    std::mutex mtx_;
    int counter_ = 0;
    uint64_t seq_counter_ = 0; // Monotonic creation sequence (drives get_all ordering)
};

// Progress callback user-data structure
struct CurlProgressData {
    std::string dl_id;      // ProgressManager download ID
    std::string title;      // Download title
    int64_t last_bytes = 0; // Last downloaded byte count (this transfer)
    curl_off_t resume_offset =
        0; // Resume offset (existing file size), used to compute overall percentage
    std::chrono::steady_clock::time_point last_time; // Last update time
};

// libcurl progress callback function (CURLOPT_XFERINFOFUNCTION)
int curl_progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                           curl_off_t ulnow);

} // namespace panicast
