#include "panicast/app/app.h"

namespace panicast
{

// D43: the download EXECUTION ENGINE (start_one_download / ytdlp_download / pump + the verify
//   helpers capture_exec / probe_media_duration / verify_downloaded_file / VerifyResult) moved to
//   DownloadService (src/app/download_service.cpp). App keeps download_node — the thin orchestrator:
//   gather marked or selected items (via App's mark methods, which stay here because navigation/
//   input/subscriptions also use them), enqueue + pump on the DownloadService, clear marks, and
//   persist the cache. Behaviour unchanged.
void App::download_node(int marked_count) {
    std::vector<TreeNodePtr> items;
    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        if (marked_count > 0)
            collect_playable_marked_current(items);
        else if (library_.selected_idx() < (int)library_.display_list().size()) {
            auto n = library_.display_list()[library_.selected_idx()].node;
            if (n->type == NodeType::RADIO_STREAM || n->type == NodeType::PODCAST_EPISODE)
                items.push_back(n);
        }
    }

    if (items.empty()) {
        EVENT_LOG("No downloadable items");
        return;
    }

    EVENT_LOG(fmt::format("Downloading {} items...", items.size()));
    // Enqueue all items; start_one_download is throttled to MAX_CONCURRENT_DOWNLOADS slots
    //   so the download list never overflows the INFO panel. Pending items are promoted as
    //   slots free (pump is also called from the render loop's prepare_frame).
    download_.enqueue(items);
    download_.pump(ProgressManager::instance().get_all().size());

    {
        std::lock_guard<std::recursive_mutex> lock(library_.tree_mutex());
        clear_marks_current();
    }
    Persistence::save_cache(library_.radio_root(), library_.podcast_root());
}

} // namespace panicast
