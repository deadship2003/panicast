// Program entry point: command-line parsing (TUI / headless daemon / import subscriptions
//   / export OPML / purge cache / version help).
#include <iostream>
#include <string>
#include <vector>
#include <getopt.h>

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include "panicast/core/constants.h"
#include "panicast/core/paths.h"
#include "panicast/core/safe_tmp.h"
#include "panicast/config/ini_config.h"
#include "panicast/playback/sleep_timer.h"
#include "panicast/parsers/xml_helpers.h"
#include "panicast/ui/ui.h"
#include "panicast/app/app.h"
#include "panicast/app/cli_commands.h"
#include "panicast/app/client_tui.h"
#include "panicast/app/daemon_mode.h"

#if __has_include("version.h")
#include "version.h"
#endif

static void print_usage() {
    // Use global constants to dynamically update version info, including build time and email
    std::cout << panicast::VERSION << " - Terminal Media Player\n";
    std::cout << "By " << panicast::AUTHOR << " <" << panicast::EMAIL << "> @"
              << panicast::BUILD_TIME << "\n\n";
    std::cout << "Usage:\n";
    std::cout << "  panicast                  TUI: client controller when the background\n";
    std::cout << "                            service is running (it stays untouched);\n";
    std::cout << "                            standalone engine TUI otherwise\n";
    std::cout << "  panicast --full           Force the FULL engine TUI (takes over the\n";
    std::cout << "                            running service; hands it back on exit)\n";
    std::cout << "  panicast start|stop|restart|enable|disable   Manage the background\n";
    std::cout << "                            service (user systemd unit; no sudo needed)\n";
    std::cout << "  panicast status           Service + playback status\n";
    std::cout << "  panicast log [-n N]       Tail today's log and FOLLOW it (Ctrl+C exits)\n";
    std::cout << "  panicast -a <url>         Add feed from URL\n";
    std::cout << "  panicast -i <file>        Import OPML subscriptions\n";
    std::cout << "  panicast -e <file>        Export to OPML file\n";
    std::cout << "  panicast -t <time>        Sleep timer (see below)\n";
    std::cout << "  panicast --purge          Clear all cached data\n";
    std::cout << "  panicast --quiet          Pure audio mode (vo=null, vid=no)\n";
    std::cout << "  panicast --vid <val>      Override video track (auto/no)\n";
    std::cout << "  panicast --vo <val>       Override video output (auto/null/gpu/wlshm)\n";
    std::cout << "  panicast --ao <val>       Override audio output (default pulse,alsa; or "
                 "pulse/alsa/pipewire/auto)\n";
    std::cout << "  panicast -h, -?           Show this help\n\n";
    std::cout << "Sleep Timer Formats (-t):\n";
    std::cout << "  5h, 30m, 90s    With suffix (hours/minutes/seconds)\n";
    std::cout << "  1:30:00         HH:MM:SS format\n";
    std::cout << "  5               Auto: <100 = hours (5 hours)\n";
    std::cout << "  100             Auto: >=100 = minutes (100 min)\n\n";
    std::cout << "Data Paths:\n";
    std::cout << "  Database:   ~/.local/share/panicast/panicast.db (SQLite3)\n";
    std::cout << "  Config:     ~/.config/panicast/config.ini\n";
    std::cout << "  Downloads:  ~/Downloads/panicast/\n";
    std::cout
        << "  Log:        ~/.local/share/panicast/panicast-YYYYMMDD.log (daily, kept 365 days)\n\n";
    std::cout << "Database Tables:\n";
    std::cout << "  history       - Playback history (max "
              << panicast::IniConfig::instance().get_history_max_days() << " days / "
              << panicast::IniConfig::instance().get_history_max_records() << " records)\n";
    std::cout << "  nodes         - Podcast subscription tree\n";
    std::cout << "  progress      - Resume positions\n";
    std::cout << "  favourites    - Favourite items\n";
    std::cout << "  player_state  - Player state snapshot\n";
    std::cout << "  radio_cache   - Radio directory cache\n";
    std::cout << "  youtube_cache - YouTube channel cache\n";
    std::cout << "  url_cache     - URL cache / download state\n";
    std::cout << "  search_cache  - iTunes search cache\n";
    std::cout << "  podcast_cache - Podcast info cache\n";
    std::cout << "  episode_cache - Episode list cache\n";
    std::cout << "  cache         - General cache\n\n";
    std::cout << "YouTube config (in ~/.config/panicast/config.ini [youtube]):\n";
    std::cout << "  cookies_file        Netscape cookies.txt (Ctrl+B). Default "
                 "<data_dir>/youtube_cookie.txt\n";
    std::cout << "  player_client       Default tv_downgraded,web (least bot-check)\n";
    std::cout << "  js_runtime          Default quickjs (nsig solver; also deno / quickjs:/path)\n";
    std::cout
        << "  play_format_video   Default bestvideo[height<=1080]+bestaudio/best (1080p DASH)\n";
    std::cout << "  play_format_audio   Default bestaudio/best (highest audio)\n";
    std::cout << "  Modes: P (cookies, no login) and Y (Google OAuth) work independently; see man "
                 "panicast.\n\n";
    std::cout << "Compile:\n";
    std::cout << "  g++ -std=c++17 -o panicast \\\n";
    std::cout << "      -I/usr/include/libxml2 \\\n";
    std::cout << "      -lmpv -lncurses -lcurl -lxml2 -lfmt -lpthread -lsqlite3\n";
}

int main(int argc, char *argv[]) {
    using namespace panicast;

    // One-shot legacy data migration (podradio → panicast) BEFORE anything touches the config/DB,
    //   so IniConfig / DatabaseManager see the new (~/.local/share/panicast) location. Idempotent.
    Paths::migrate_legacy();

    // N09/S1→N10: service subcommands (status/start/stop/restart/enable/disable/log) are
    //   handled and exited here, before any TUI/daemon machinery starts.
    if (int rc = run_cli_command(argc, argv); rc >= 0)
        return rc;

    int opt;
    std::string import_url, export_file, import_opml_file, sleep_time;
    bool purge = false;
    bool quiet_mode = false; /* --quiet = pure audio (vid=no, vo=null) */

    /* CLI long options: --purge, --quiet, --vid, --vo, --ao, --help, --version.
       (--daemon stays as an INTERNAL long option: the user service unit's ExecStart
       uses it — nobody types it. The -d short form is gone per N10.3.) */
    static struct option long_options[] = {
        {"daemon", no_argument, 0, 'd'},    {"full", no_argument, 0, 'F'},
        {"purge", no_argument, 0, 'P'},     {"quiet", no_argument, 0, 'q'},
        {"vid", required_argument, 0, 'V'}, {"vo", required_argument, 0, 'O'},
        {"ao", required_argument, 0, 'A'},  {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},   {0, 0, 0, 0}};

    std::string cli_vo, cli_vid, cli_ao; /* CLI overrides (empty = use defaults) */
    bool daemon_mode = false;    /* --daemon (internal): headless daemon for the service unit */
    bool force_full_tui = false; /* --full: engine TUI even when the service runs */

    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "a:i:e:t:Fh?v", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'd': // internal (--daemon only; reached from the systemd unit's ExecStart)
            daemon_mode = true;
            break;
        case 'a':
            import_url = optarg;
            break;
        case 'i':
            import_opml_file = optarg;
            break;
        case 'e':
            export_file = optarg;
            break;
        case 't':
            sleep_time = optarg;
            break;
        case 'h':
        case '?':
            print_usage();
            return 0;
        case 'v':
            std::cout << VERSION << "\n";
            std::cout << "  Author: " << AUTHOR << " <" << EMAIL << ">\n";
            std::cout << "  Build:  " << BUILD_TIME << "\n";
            std::cout << "  License: MIT\n";
            return 0;
        case 'P':
            purge = true;
            break;
        case 'F': /* --full: force the standalone engine TUI (takeover) even when
                       the background service is running */
            force_full_tui = true;
            break;
        case 'q': /* --quiet: pure audio mode */
            quiet_mode = true;
            break;
        case 'V': /* --vid <val> */
            cli_vid = optarg;
            break;
        case 'O': /* --vo <val> */
            cli_vo = optarg;
            break;
        case 'A': /* --ao <val> */
            cli_ao = optarg;
            break;
        }
    }

    /* N10: -d / --daemon → headless foreground daemon (same engine, NullFrontend).
       Takes precedence over the TUI-only options; the double-instance guard lives in
       run_daemon(). */
    if (daemon_mode)
        return run_daemon();

    /* --quiet = --vid=no --vo=null */
    if (quiet_mode) {
        cli_vid = "no";
        cli_vo = "null";
    }

    /* Store CLI overrides for MPVController */
    MPVController::set_cli_overrides(cli_vo, cli_vid, cli_ao);

    // Handle --purge to clear cache (cache tables only, user data preserved)
    if (purge) {
        std::string data_dir = Paths::get_data_dir();
        if (!data_dir.empty()) {
            std::cout << "Purging cache (user data preserved): " << data_dir << std::endl;
            try {
                DatabaseManager::instance().init();
                DatabaseManager::instance().purge_cache_only();
                std::string tmp_dir = SafeTmpFile::tmp_dir();
                if (fs::exists(tmp_dir)) {
                    fs::remove_all(tmp_dir);
                    fs::create_directories(tmp_dir);
                }
                std::cout << "Cache cleared successfully." << std::endl;
                std::cout << "  Preserved: subscriptions, history, favourites, progress"
                          << std::endl;
                std::cout << "  Cleared:   search_cache, podcast_cache, episode_cache, tmp files"
                          << std::endl;
            } catch (const std::exception &e) {
                std::cerr << "Error: " << e.what() << std::endl;
            }
        }
        return 0;
    }

    curl_global_init(CURL_GLOBAL_ALL);
    xmlInitParser();

    // Set the XML error handler immediately to prevent libxml2 from writing to stderr by default
    // Must be set after xmlInitParser() and before any XML parsing
    xmlSetGenericErrorFunc(NULL, xml_error_handler);
    xmlSetStructuredErrorFunc(
        NULL, (xmlStructuredErrorFunc)
                  xml_structured_error_handler); // Compatible with older libxml2 const signature

    // Set up sleep timer
    if (!sleep_time.empty()) {
        int seconds = SleepTimer::parse_time_string(sleep_time);
        if (seconds > 0) {
            SleepTimer::instance().set_duration(seconds);
            std::cout << "Sleep timer set for " << seconds << " seconds" << std::endl;
        } else {
            std::cerr << "Invalid sleep time format: '" << sleep_time
                      << "'. Use e.g. 30m / 1h / 90s / HH:MM:SS / <minutes>." << std::endl;
        }
    }

    bool took_over = false; // N09/S1-4: set when the TUI stops the daemon on launch

    // Save original terminal state, register signal handlers and atexit cleanup
    save_terminal_state();
    setup_signal_handlers();
    atexit(tui_cleanup);

    if (!import_url.empty() || !export_file.empty() || !import_opml_file.empty()) {
        App app;
        // In CLI mode, persistent data must also be loaded; otherwise -e export is empty
        app.load_data();

        try {
            if (!import_url.empty()) {
                std::cout << "Importing: " << import_url << std::endl;
                app.import_feed(
                    import_url); // Internally waits idle for loading to complete; no fixed sleep needed
            }

            if (!import_opml_file.empty()) {
                app.import_opml(import_opml_file); // Internally waits idle
            }

            if (!export_file.empty()) {
                std::cout << "Exporting to: " << export_file << std::endl;
                app.export_podcasts(export_file);
            }
        } catch (const std::exception &e) {
            std::cerr << "Error: " << e.what() << std::endl;
            curl_global_cleanup();
            xmlCleanupParser();
            return 1;
        } catch (...) {
            std::cerr << "Error: unknown exception" << std::endl;
            curl_global_cleanup();
            xmlCleanupParser();
            return 1;
        }
    } else {
        // N10.6: the service IS the engine. When it's already running, `panicast`
        //   opens a local CLIENT TUI over its control plane — the service process
        //   is not touched at all (no takeover, no restart, phone keeps streaming).
        //   The standalone engine TUI (below) only boots when nothing is running.
        if (panicast::daemon_pid_alive() && !force_full_tui) {
            return panicast::run_client_tui();
        }
        // N10.3: FIRST-RUN AUTO-SERVICE — install/refresh the user-space unit before
        //   anything else (sudo-free; ExecStart = this binary). The handover-restore
        //   on exit then starts it, so one `panicast` run leaves the background
        //   service installed AND running for Squeeze Client.
        panicast::ensure_user_unit();
        // N10.2: single-instance engine ownership. One TUI session at a time — the
        //   engine (mpv + queue + DB) has exactly one owner; a second TUI would race
        //   the first exactly like a second daemon would.
        if (int tui_pid = 0; panicast::tui_pid_alive(&tui_pid)) {
            std::cerr << "panicast: another TUI session is already running (pid " << tui_pid
                      << ") — exit it first." << std::endl;
            return 1;
        }
        App app;
        // N09/S1-4 + N10.2: single-instance session handover — stop the daemon (it
        //   persists player state on its clean exit), run the TUI, restart the daemon
        //   afterwards. App::run() exits via _exit(0) (App::shutdown), which SKIPS
        //   main's epilogue — so the restore (+ pidfile removal) rides the exit hook,
        //   the same mechanism the daemon uses for its pidfile.
        app.set_exit_hook([]() {
            panicast::remove_tui_pidfile();
            panicast::service_handover_restore();
        });
        took_over = panicast::service_handover_takeover();
        panicast::write_tui_pidfile();
        // Wrap exceptions — any exception thrown by run() that reaches main triggers
        //   std::terminate/abort, skipping atexit(tui_cleanup) and leaving the terminal stuck in curses mode,
        //   and the App destructor won't run (thread pool not joined). Catch here, restore terminal, then exit.
        try {
            app.run();
        } catch (const std::exception &e) {
            panicast::remove_tui_pidfile();
            panicast::service_handover_restore(); // give the phone its remote back
            tui_cleanup();
            std::cerr << "Fatal error: " << e.what() << std::endl;
            curl_global_cleanup();
            xmlCleanupParser();
            return 1;
        } catch (...) {
            panicast::remove_tui_pidfile();
            panicast::service_handover_restore();
            tui_cleanup();
            std::cerr << "Fatal error: unknown exception" << std::endl;
            curl_global_cleanup();
            xmlCleanupParser();
            return 1;
        }
    }

    // Unreachable for the TUI path (App::run ends in _exit(0) — the exit hook covers
    //   the handover restore); kept for the import/export CLI branch's fall-through.
    if (took_over)
        service_handover_restore();

    curl_global_cleanup();
    xmlCleanupParser();

    return 0;
}
