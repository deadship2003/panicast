#include <unistd.h> // _exit (skip destructors on clean exit)
#include "panicast/app/app.h"
#include "panicast/app/actions.h"
#include "panicast/app/playback_events.h"
#include "panicast/core/event_bus.h"
#include "panicast/net/bilibili_api.h"
#include "panicast/net/tiktok_region.h"
#include "panicast/parsers/bilibili_parser.h"
#include <cstdio>
#include <iostream>

namespace panicast
{

void App::flatten(const TreeNodePtr &node, int depth, bool is_last, int parent_idx) {
    // E: pure subtree recursion — always pushes the node. The caller passes only displayable
    //   items (the per-mode root is never passed), so the old `title=="Root"` magic is gone.
    int current_idx = library_.display_list().size();
    library_.display_list().push_back({node, depth, is_last, parent_idx});
    if ((node->type == NodeType::FOLDER || node->type == NodeType::PODCAST_FEED) &&
        node->expanded) {
        int count = node->children.size();
        for (int i = 0; i < count; ++i) {
            flatten(node->children[i], depth + 1, i == count - 1, current_idx);
        }
    }
}

// E: flatten the current mode's top-level item list (the root is not in the display domain).
void App::flatten_items(const std::vector<TreeNodePtr> &tops) {
    int count = (int)tops.size();
    for (int i = 0; i < count; ++i) {
        flatten(tops[i], 0, i == count - 1, -1);
    }
}

// E: the active mode's top-level items. (Step 1 bridge: backed by the per-mode root's children;
//   Step 2 migrates ownership to dedicated vectors and removes the root nodes.)
std::vector<TreeNodePtr> &App::items_for_mode(AppMode m) {
    switch (m) {
    case AppMode::RADIO:
        return library_.radio_root();
    case AppMode::PODCAST:
        return library_.podcast_root();
    case AppMode::FAVOURITE:
        return library_.fav_root();
    case AppMode::HISTORY:
        return library_.history_root();
    case AppMode::ONLINE:
        return OnlineState::instance().online_root->children;
    case AppMode::ACCOUNT:
        return library_.account_root();
    case AppMode::BILIBILI:
        return library_.bilibili_root();
    case AppMode::TIKTOK:
        return library_.tiktok_root();
    case AppMode::IPTV:
        return library_.iptv_root();
    }
    return library_.radio_root();
}

std::vector<TreeNodePtr> &App::cur_items() {
    return items_for_mode(mode);
}

} // namespace panicast
