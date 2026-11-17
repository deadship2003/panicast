// Path utilities: expand_path, get_download_dir, get_log_file, sanitize_filename.
// Y24.38: split out of utils.cpp. Methods remain Utils:: static members
//   (declarations stay in utils.h); only implementations live here.
#include "panicast/core/utils.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring> // std::string
#include <ctime>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sstream>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#include <fmt/format.h>

#include "panicast/core/paths.h"
#include "panicast/core/terminal.h"

extern char **environ; // Required by posix_spawnp

namespace panicast
{

std::string Utils::expand_path(const std::string &path) {
    if (path.empty() || path[0] != '~')
        return path;
    const char *home = std::getenv("HOME");
    return home ? std::string(home) + path.substr(1) : path;
}

std::string Utils::get_download_dir() {
    return expand_path("~" + std::string(DOWNLOAD_DIR));
}
// Y24.17: daily log file (panicast-YYYYMMDD.log); logs older than 365 days are auto-deleted on init.
std::string Utils::get_log_file() {
    std::time_t t = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m%d", std::localtime(&t));
    return expand_path("~" + std::string(DATA_DIR) + "/panicast-" + buf + ".log");
}

std::string Utils::sanitize_filename(const std::string &name) {
    std::string result = name;
    std::replace_if(
        result.begin(), result.end(),
        [](char c) {
            return c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
                   c == '<' || c == '>' || c == '|' || (unsigned char)c < 0x20 ||
                   c == '\x7f'; // Strip control characters
        },
        '_');
    if (result.length() > 200)
        result = result.substr(0, 200);
    // Prevent dot-prefixed filenames (hidden files / path escape) and dot-only names
    size_t first_real = result.find_first_not_of('.');
    if (first_real == std::string::npos)
        return "unnamed"; // All dots
    if (first_real > 0)
        result = result.substr(first_real);
    if (result == "." || result == "..")
        result = "unnamed";
    return result;
}
} // namespace panicast
