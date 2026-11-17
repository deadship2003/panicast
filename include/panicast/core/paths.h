// Path helper: unified getenv("HOME")+DATA_DIR+... concatenation.
// Reused by all sites such as Logger/SafeTmpFile/Utils/main. Returns empty string when HOME is unset (tmp falls back to /tmp).
#pragma once

#include <cstdlib>
#include <string>

namespace panicast
{

// Path constants (constexpr implies inline linkage; safe to place in a header)
constexpr const char *DATA_DIR = "/.local/share/panicast";
constexpr const char *CONFIG_DIR = "/.config/panicast";
constexpr const char *DOWNLOAD_DIR = "/Downloads/panicast";

struct Paths {
    static std::string get_data_dir();
    static std::string get_db_file();
    static std::string get_tmp_dir();
    // One-shot legacy migration: if the new panicast data/config dirs are absent but the old
    //   podradio dirs exist (rename from an older build), move them to the panicast names so the
    //   user keeps config/cookies/models/downloads. The DB is intentionally NOT migrated — it is
    //   rebuilt fresh (legacy podradio.db is dropped, panicast.db is created empty on first run).
    //   Also rewrites stored path fragments in the migrated config.ini. Idempotent (runs only while
    //   the new data dir is absent). Best-effort; never throws.
    static void migrate_legacy();
};

} // namespace panicast
