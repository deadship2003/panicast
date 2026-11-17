#include "panicast/core/paths.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace panicast
{
namespace fs = std::filesystem;

std::string Paths::get_data_dir() {
    const char *h = std::getenv("HOME");
    return h ? std::string(h) + DATA_DIR : "";
}

std::string Paths::get_db_file() {
    std::string d = get_data_dir();
    return d.empty() ? "" : d + "/panicast.db";
}

std::string Paths::get_tmp_dir() {
    const char *h = std::getenv("HOME");
    if (!h)
        h = "/tmp"; // Fall back to /tmp when HOME is unset (preserve old behavior)
    return std::string(h) + DATA_DIR + "/tmp";
}

// ── legacy (podradio → panicast) one-shot migration ──────────────────────────
namespace
{
std::string home_dir() {
    const char *h = std::getenv("HOME");
    return h ? h : "";
}

// Rename oldp → newp only when newp is absent and oldp exists. Atomic on the same filesystem;
//   returns false (no-op) otherwise. Never throws.
bool rename_if_needed(const std::string &oldp, const std::string &newp) {
    if (oldp.empty() || newp.empty())
        return false;
    std::error_code ec;
    if (fs::exists(newp, ec))
        return false; // already migrated (or in use)
    if (!fs::exists(oldp, ec))
        return false;           // nothing to migrate
    fs::rename(oldp, newp, ec); // same-fs → rename(2); cross-fs → ec set
    return !ec;
}

// Rewrite stored path fragments inside the migrated config.ini so absolute paths the code reads
//   (e.g. [transcription] model = ~/.local/share/podradio/...) point at the new panicast dirs.
void fix_config_paths(const std::string &cfg) {
    std::ifstream in(cfg);
    if (!in)
        return;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    struct Rep {
        const char *from;
        const char *to;
    };
    static const Rep reps[] = {
        {"/.local/share/podradio", "/.local/share/panicast"},
        {"/.config/podradio", "/.config/panicast"},
        {"/Downloads/PodRadio", "/Downloads/panicast"},
        {"/Downloads/PaniCast", "/Downloads/panicast"},
        {"/Downloads/Panicast", "/Downloads/panicast"},
        {"podradio-", "panicast-"},
    };
    bool changed = false;
    for (const auto &r : reps) {
        const size_t flen = std::strlen(r.from), tlen = std::strlen(r.to);
        for (size_t p = 0; (p = content.find(r.from, p)) != std::string::npos;) {
            content.replace(p, flen, r.to);
            p += tlen;
            changed = true;
        }
    }
    if (!changed)
        return;
    std::ofstream out(cfg, std::ios::trunc);
    if (out)
        out << content;
}
} // namespace

void Paths::migrate_legacy() {
    std::string h = home_dir();
    if (h.empty())
        return;
    bool moved = false;
    // Data dir: DB / models / cookies / logs / tmp / transcripts.
    if (rename_if_needed(h + "/.local/share/podradio", h + "/.local/share/panicast")) {
        moved = true;
        // Fresh DB per user request: drop the legacy DB files so panicast.db is created empty.
        std::error_code ec;
        for (const char *f : {"/podradio.db", "/podradio.db-wal", "/podradio.db-shm"})
            fs::remove(h + "/.local/share/panicast" + f, ec);
    }
    // Config dir: config.ini + backups.
    if (rename_if_needed(h + "/.config/podradio", h + "/.config/panicast"))
        moved = true;
    // Downloads dir (best-effort; fs::rename is atomic same-fs, fails harmlessly cross-fs).
    rename_if_needed(h + "/Downloads/PodRadio", h + "/Downloads/panicast");
    // Also migrate the older camelCase "PaniCast" and capital-P "Panicast" downloads
    //   dirs → the canonical lowercase "panicast" name.
    rename_if_needed(h + "/Downloads/PaniCast", h + "/Downloads/panicast");
    rename_if_needed(h + "/Downloads/Panicast", h + "/Downloads/panicast");
    if (moved)
        fix_config_paths(h + "/.config/panicast/config.ini");
}

} // namespace panicast
