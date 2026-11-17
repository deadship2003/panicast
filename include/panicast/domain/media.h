// Media — the single domain handle modules exchange. A media item is identified by its
// canonical absolute source URL (the real, resolved source URL — never a local file:// path,
// never a raw cache path, never a tree-node pointer). This makes the identity stable across
// the in-memory tree, the DB cache, history (H), favourites (F) and the remote-control
// protocol: they all key the same absolute URL.
//
// D4 introduced Media/MediaID as a pointer-backed adapter (never wired into production).
// D14-1 redefines the identity as logical (URL-based) BEFORE first real adoption, so the
// now-playing / persistence / network surfaces can converge on one identity model instead
// of today's split (in-memory TreeNodePtr vs played-path string vs source-URL string).
// Header-only; currently tests-only (see D14-2 onward for production adoption).
//
// (D4 — new-arch M0; D14-1 — identity model: pointer → logical absolute URL.)
#pragma once

#include <string>
#include <utility>

#include "panicast/core/types.h"  // TreeNode, TreeNodePtr

namespace panicast
{

// Opaque, comparable media identity. The identity IS the canonical absolute source URL.
// Two MediaIDs are equal iff their URLs are equal — i.e. the same logical media, even when
// reached through different in-memory tree nodes (e.g. an episode shown under both PODCAST
// and FAVOURITE). This is the key property the DB/history/remote surfaces already rely on
// and that a pointer-backed identity cannot provide (pointers die with the node / process).
class MediaID {
public:
    MediaID() = default;
    explicit MediaID(std::string url) : url_(std::move(url)) {}

    // The canonical absolute source URL (empty for a default-constructed / invalid id).
    const std::string &url() const noexcept { return url_; }

    // A non-empty URL is a valid identity.
    bool valid() const noexcept { return !url_.empty(); }

    bool operator==(const MediaID &o) const noexcept { return url_ == o.url_; }
    bool operator!=(const MediaID &o) const noexcept { return !(*this == o); }

private:
    std::string url_;  // canonical absolute source URL — the identity
};

// Read-only view onto a media item: identity plus the display/protocol fields every
// now-playing consumer needs. Modules exchange this instead of the full TreeNode (80+
// fields) or a bare URL string. `is_video` is not derivable from a TreeNode alone (it has
// no such field; it is computed from the URL/type at the PlaybackService layer via
// URLClassifier) and is therefore left false by media_from_node — the service fills it (D14-2).
struct Media {
    MediaID id;
    std::string title;
    std::string art_url;
    bool is_video = false;
};

// Build a Media view from a tree node (nullptr → empty / invalid Media). Copies the node's
// url (→ identity), title, art_url. Does not derive is_video (see struct note).
inline Media media_from_node(const TreeNodePtr &n) {
    Media m;
    if (n) {
        m.id = MediaID{n->url};
        m.title = n->title;
        m.art_url = n->art_url;
    }
    return m;
}

// Build a MediaID from a raw URL string (e.g. a DB row or a wire field already carrying the
// canonical absolute source URL).
inline MediaID media_id_from_url(std::string url) {
    return MediaID{std::move(url)};
}

}  // namespace panicast
