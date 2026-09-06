// N10.6 client-mode TUI — implementation. See client_tui.h for the contract.
//   A deliberately small ncurses controller over the daemon's LMS plane:
//     - view = the daemon's CURRENT display list (fetched via `panicast browse root`);
//       navigation moves the daemon's cursor, so phone/terminal/web always agree;
//     - keys map onto the same slim commands Squeeze Client sends;
//     - 300 ms polling of `status` keeps the now-playing footer live and refetches
//       the list whenever browse_sig changes (mode switch / navigation from phone).
#include "panicast/app/client_tui.h"

#include <ncurses.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "panicast/config/ini_config.h"

namespace panicast
{

namespace
{

// ── minimal LMS-loopback client (POST /cometd, one request per call) ──────────
struct LmsClient {
    int port = 9090;
    std::string auth;

    bool init() {
        IniConfig::instance().load();
        port = IniConfig::instance().get_remote_lms_port();
        auth = IniConfig::instance().get_remote_lms_user() + ":" +
               IniConfig::instance().get_remote_lms_pass();
        return probe();
    }

    bool probe() const {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return false;
        timeval tv{1, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool ok = ::connect(fd, (sockaddr *)&a, sizeof(a)) == 0;
        ::close(fd);
        return ok;
    }

    // Executes one slim command; returns the result object (empty on failure).
    //   `tmo_sec` — polling uses a short budget so a busy daemon can never freeze
    //   the UI; user-initiated actions (search/mode) get the full 6s.
    nlohmann::json slim(const std::vector<std::string> &cmd, int tmo_sec = 6) const {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return nlohmann::json::object();
        timeval tv{tmo_sec, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, (sockaddr *)&a, sizeof(a)) != 0) {
            ::close(fd);
            return nlohmann::json::object();
        }
        // Basic auth from the INI (same credentials the phone uses).
        static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        int val = 0, bits = 0;
        for (unsigned char c : auth) {
            val = (val << 8) | c;
            bits += 8;
            while (bits >= 6) {
                bits -= 6;
                b64 += tbl[(val >> bits) & 0x3F];
            }
        }
        if (bits > 0)
            b64 += tbl[(val << (6 - bits)) & 0x3F];
        while (b64.size() % 4)
            b64 += '=';

        nlohmann::json inner = nlohmann::json::array({"00:00:00:00:84:21", cmd});
        std::string body = "[{\"channel\":\"/slim/request\",\"data\":{\"response\":\"/ctui\","
                           "\"request\":" +
                           inner.dump() + "},\"id\":\"1\"}]";
        std::string req =
            "POST /cometd HTTP/1.1\r\nHost: localhost\r\nAuthorization: Basic " + b64 +
            "\r\nContent-Type: text/json\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) <= 0) {
            ::close(fd);
            return nlohmann::json::object();
        }
        std::string resp;
        char buf[8192];
        ssize_t n;
        while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
            resp.append(buf, (size_t)n);
        ::close(fd);
        size_t sep = resp.find("\r\n\r\n");
        if (sep == std::string::npos)
            return nlohmann::json::object();
        try {
            nlohmann::json msgs = nlohmann::json::parse(resp.substr(sep + 4), nullptr, false);
            if (msgs.is_array())
                for (const auto &m : msgs)
                    if (m.value("channel", std::string()) == "/ctui" && m.contains("data"))
                        return m["data"];
        } catch (...) {
        }
        return nlohmann::json::object();
    }
};

// ── view state ────────────────────────────────────────────────────────────────
struct Row {
    std::string text; // already depth-indented, branches carry "▸ "
    bool branch = false;
};

struct ClientState {
    LmsClient lms;
    std::vector<Row> rows;
    std::string browse_sig;    // daemon-side list signature ("" until first fetch)
    std::string last_sig_seen; // sig as of the last rows fetch
    // now-playing footer (from `status - 1`)
    std::string mode = "?", play_state = "stop", title, artist;
    double pos = 0, dur = 0;
    int volume = 0, queue_count = 0, queue_idx = -1;
    bool queue_view = false; // Tab toggles tree ↔ queue
    std::vector<Row> queue_rows;
};

std::vector<Row> rows_from_browse(const nlohmann::json &r) {
    std::vector<Row> out;
    for (const auto &it : r.value("item_loop", nlohmann::json::array())) {
        std::string text = it.value("text", std::string());
        // first line only (subtext second line is nice-to-have; keep rows single-line)
        size_t nl = text.find('\n');
        if (nl != std::string::npos)
            text.resize(nl);
        // META-5: the virtual search-input row is dropped — the client has its own
        //   's' key, and keeping it would shift every activation index by one.
        if (text.rfind("🔍", 0) == 0)
            continue;
        bool branch = text.find("▸") != std::string::npos;
        out.push_back({text, branch});
    }
    return out;
}

std::vector<Row> rows_from_playlist(const nlohmann::json &r) {
    std::vector<Row> out;
    for (const auto &it : r.value("item_loop", nlohmann::json::array()))
        out.push_back({it.value("track", std::string()), false});
    return out;
}

void fetch_status(ClientState &st) {
    nlohmann::json s = st.lms.slim({"status", "-", "1"});
    st.mode = s.value("mode_name", s.value("player_name", std::string("?")));
    // mode name is not in status; derive from the menu below instead. Keep player state:
    st.play_state = s.value("mode", "stop");
    st.pos = s.value("time", 0.0);
    st.dur = s.value("duration", 0.0);
    st.volume = s.value("mixer volume", 0);
    st.queue_count = s.value("playlist_tracks", 0);
    st.queue_idx = s.value("playlist_cur_index", -1);
    const auto &item =
        s.contains("item_loop") && s["item_loop"].is_array() && !s["item_loop"].empty()
            ? s["item_loop"][0]
            : nlohmann::json::object();
    st.title = item.value("track", std::string());
    st.artist = item.value("artist", std::string());
    st.browse_sig = s.value("browse_sig", st.browse_sig);
}

void fetch_rows(ClientState &st) {
    nlohmann::json r = st.lms.slim({"panicast", "browse", "root", "0", "400"});
    st.rows = rows_from_browse(r);
    st.last_sig_seen = st.browse_sig;
}

void fetch_queue(ClientState &st) {
    nlohmann::json r = st.lms.slim({"status", "0", "400", "menu:menu"});
    st.queue_rows = rows_from_playlist(r);
}

// The current mode label (for the header) — read from the home menu's ▶ entry.
std::string fetch_mode_label(ClientState &st) {
    nlohmann::json m = st.lms.slim({"menu", "0", "32", "direct:1"});
    for (const auto &it : m.value("item_loop", nlohmann::json::array())) {
        std::string t = it.value("text", std::string());
        if (t.rfind("▶ ", 0) == 0)
            return t.substr(4); // "▶ " is U+25B6 (3 bytes) + space
    }
    return "?";
}

// small local sprintf wrapper (kept tiny; snprintf with up to 3 ints)
std::string fmt_s(const char *f, int a, int b = 0, int c = 0) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), f, a, b, c);
    return buf;
}

std::string fmt_time(double t) {
    if (t < 0)
        t = 0;
    int s = (int)t;
    return s >= 3600 ? fmt_s("%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60)
                     : fmt_s("%d:%02d", s / 60, s % 60);
}

} // namespace

int run_client_tui() {
    ClientState st;
    if (!st.lms.init()) {
        std::fprintf(stderr,
                     "panicast: the background service is running but its mini-LMS port "
                     "(:%d) is not reachable.\nCheck [remote] lms_enable / lms_port, or "
                     "stop the service to run the standalone TUI.\n",
                     IniConfig::instance().get_remote_lms_port());
        return 1;
    }

    // Non-interactive smoke mode for tests: one poll cycle, dump state, exit.
    if (std::getenv("PANICAST_CLIENT_TEST")) {
        fetch_status(st);
        fetch_rows(st);
        std::printf("mode=%s state=%s vol=%d queue=%d rows=%zu sig=%s\n",
                    fetch_mode_label(st).c_str(), st.play_state.c_str(), st.volume, st.queue_count,
                    st.rows.size(), st.browse_sig.substr(0, 12).c_str());
        return 0;
    }

    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(300); // poll tick

    fetch_status(st);
    fetch_rows(st);
    std::string mode_label = fetch_mode_label(st);
    size_t cursor = 0, view_start = 0;
    static const char *HELP =
        "Enter:open/play  Backspace:back  1-9:mode  Tab:queue  space:pause  n/p:track  "
        "+/-:vol  m:mute  ,/.:seek  q:quit";

    bool running = true;
    auto run_search = [&]() {
        // 's' — local text input → search_query on the daemon → replace the list
        //   with the reply (same page shape the Squeezer input row produces).
        echo();
        curs_set(1);
        int rows_n, cols_n;
        getmaxyx(stdscr, rows_n, cols_n);
        (void)rows_n;
        mvaddstr(0, 0, " Search: ");
        clrtoeol();
        char buf[256] = {0};
        timeout(-1); // blocking read while typing
        mvgetnstr(0, 9, buf, sizeof(buf) - 1);
        timeout(300);
        noecho();
        curs_set(0);
        std::string q(buf);
        while (!q.empty() && (q.back() == ' '))
            q.pop_back();
        if (q.empty())
            return;
        nlohmann::json r = st.lms.slim({"panicast", "search", q, "0", "400"});
        if (r.contains("item_loop")) {
            st.rows = rows_from_browse(r);
            st.last_sig_seen = st.browse_sig; // the reply already reflects them
            cursor = 0;
            view_start = 0;
        }
        st.queue_view = false;
    };

    while (running) {
        int rows_n, cols_n;
        getmaxyx(stdscr, rows_n, cols_n);
        int list_h = std::max(1, rows_n - 4);

        erase();
        // header: mode + view
        attron(A_REVERSE);
        std::string head =
            " panicast·client · " + mode_label + (st.queue_view ? " · Queue" : " · List");
        mvaddnstr(0, 0, head.c_str(), cols_n - 1);
        for (int x = (int)head.size(); x < cols_n; ++x)
            addch(' ');
        attroff(A_REVERSE);

        const auto &items = st.queue_view ? st.queue_rows : st.rows;
        if (cursor >= items.size())
            cursor = items.empty() ? 0 : items.size() - 1;
        if (cursor < view_start)
            view_start = cursor;
        if (cursor >= view_start + (size_t)list_h)
            view_start = cursor - list_h + 1;

        for (size_t i = view_start; i < items.size() && i < view_start + (size_t)list_h; ++i) {
            int y = (int)(1 + i - view_start);
            bool cur = i == cursor;
            bool playing = st.queue_view && (int)i == st.queue_idx;
            if (cur)
                attron(A_BOLD);
            if (playing)
                attron(A_REVERSE);
            std::string line = (cur ? "▶ " : "  ") + items[i].text;
            mvaddnstr(y, 0, line.c_str(), cols_n - 1);
            attroff(A_BOLD | A_REVERSE);
        }
        if (items.empty())
            mvaddstr(1, 2, "(empty — switch mode with 1-9)");

        // now-playing footer + two help lines
        std::string np = " " + (st.title.empty() ? std::string("—") : st.title);
        if (!st.artist.empty() && st.artist != "panicast")
            np += " — " + st.artist;
        if (st.dur > 0)
            np += "  [" + fmt_time(st.pos) + "/" + fmt_time(st.dur) + "]";
        np += "  vol:" + std::to_string(st.volume) + "%";
        attron(A_REVERSE);
        mvaddnstr(rows_n - 3, 0, np.c_str(), cols_n - 1);
        attroff(A_REVERSE);
        mvaddnstr(rows_n - 2, 0,
                  "Enter:open/play  BS:back  1-9:mode  Tab:queue  space:pause  n/p:track  "
                  "+/-:vol  m:mute  ,/.:seek10s",
                  cols_n - 1);
        mvaddnstr(rows_n - 1, 0,
                  "s:search  r:cycle R:repeat S:shuffle  x/z:X speed  L:subs  f:fav  "
                  "d:download  q:quit",
                  cols_n - 1);
        refresh();

        // ── input: keys NEVER wait on the network (fire-and-forget commands);
        //   state catches up on the next idle poll tick (≤300ms).
        int ch = getch();
        switch (ch) {
        case 'q':
        case 'Q':
            running = false;
            break;
        case KEY_UP:
            if (cursor > 0)
                --cursor;
            break;
        case KEY_DOWN:
            if (cursor + 1 < items.size())
                ++cursor;
            break;
        case KEY_PPAGE:
            cursor = cursor > (size_t)list_h ? cursor - list_h : 0;
            break;
        case KEY_NPAGE:
            cursor = std::min(items.size() - 1, cursor + list_h);
            break;
        case KEY_HOME:
            cursor = 0;
            break;
        case KEY_END:
            cursor = items.empty() ? 0 : items.size() - 1;
            break;
        case '\n':
        case KEY_ENTER: {
            if (st.queue_view) {
                st.lms.slim({"playlist", "index", std::to_string(cursor)});
            } else {
                nlohmann::json r =
                    st.lms.slim({"panicast", "browse", std::to_string(cursor), "0", "400"});
                if (r.contains("item_loop"))
                    st.rows = rows_from_browse(r); // branch nav reply = new page
            }
            break;
        }
        case KEY_BACKSPACE:
        case 127:
        case 'b': {
            if (!st.queue_view) {
                nlohmann::json r = st.lms.slim({"panicast", "browse", "back", "0", "400"});
                if (r.contains("item_loop")) {
                    st.rows = rows_from_browse(r);
                    st.last_sig_seen = st.browse_sig;
                    cursor = 0;
                    view_start = 0;
                }
            }
            break;
        }
        case '\t':
            st.queue_view = !st.queue_view;
            if (st.queue_view)
                fetch_queue(st);
            cursor = 0;
            view_start = 0;
            break;
        case ' ':
            st.lms.slim({"pause"});
            break;
        case 'n':
            st.lms.slim({"next"});
            break;
        case 'p':
            st.lms.slim({"prev"});
            break;
        case '+':
            st.lms.slim({"mixer", "volume", "+5"});
            break;
        case '-':
            st.lms.slim({"mixer", "volume", "-5"});
            break;
        case 'm':
            st.lms.slim({"mixer", "muting", "toggle"});
            break;
        case '.':
            st.lms.slim({"time", std::to_string((int)st.pos + 10)});
            break;
        case ',':
            st.lms.slim({"time", std::to_string(std::max(0, (int)st.pos - 10))});
            break;
        case 's':
            run_search();
            break;
        case 'r':
            st.lms.slim({"panicast", "playmode", "cycle"});
            break;
        case 'R':
            st.lms.slim({"panicast", "playmode", "repeat"});
            break;
        case 'S':
            st.lms.slim({"panicast", "playmode", "shuffle"});
            break;
        case 'x':
            st.lms.slim({"panicast", "speed", "up"});
            break;
        case 'z':
            st.lms.slim({"panicast", "speed", "down"});
            break;
        case 'X':
            st.lms.slim({"panicast", "speed", "reset"});
            break;
        case 'L':
            st.lms.slim({"subtitle_toggle"});
            break;
        case 'f':
            st.lms.slim({"favourite_toggle"});
            break;
        case 'd':
            st.lms.slim({"download"});
            break;
        default:
            if (ch >= '1' && ch <= '9') {
                static const char *modes[] = {"RADIO",    "PODCAST", "FAVOURITE",
                                              "HISTORY",  "ONLINE",  "ACCOUNT",
                                              "BILIBILI", "TIKTOK",  "IPTV"};
                const char *want = modes[ch - '1'];
                nlohmann::json r = st.lms.slim({"panicast", "mode", want, "0", "400"});
                if (r.contains("item_loop"))
                    st.rows = rows_from_browse(r);
                mode_label = fetch_mode_label(st);
                cursor = 0;
                view_start = 0;
            }
            break;
        }

        // idle poll tick (getch timed out): refresh footer; refetch the list only
        // when the daemon's signature moved (mode switch / navigation elsewhere).
        if (ch == ERR) {
            nlohmann::json s = st.lms.slim({"status", "-", "1"}, 2);
            if (!s.empty()) {
                st.play_state = s.value("mode", st.play_state);
                st.pos = s.value("time", st.pos);
                st.dur = s.value("duration", st.dur);
                st.volume = s.value("mixer volume", st.volume);
                st.queue_count = s.value("playlist_tracks", st.queue_count);
                st.queue_idx = s.value("playlist_cur_index", st.queue_idx);
                const auto &item =
                    s.contains("item_loop") && s["item_loop"].is_array() && !s["item_loop"].empty()
                        ? s["item_loop"][0]
                        : nlohmann::json::object();
                st.title = item.value("track", st.title);
                st.artist = item.value("artist", st.artist);
                st.browse_sig = s.value("browse_sig", st.browse_sig);
            }
            if (!st.queue_view && !st.browse_sig.empty() && st.browse_sig != st.last_sig_seen) {
                fetch_rows(st);
                cursor = std::min(cursor, st.rows.empty() ? (size_t)0 : st.rows.size() - 1);
            }
            if (st.queue_view)
                fetch_queue(st);
        }
    }

    endwin();
    return 0;
}

} // namespace panicast
