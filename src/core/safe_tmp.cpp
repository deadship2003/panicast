#include "panicast/core/safe_tmp.h"

#include <cerrno>
#include <cstring> // strerror
#include <filesystem>
#include <unistd.h> // close
#include <vector>

#include <fmt/format.h>

#include "panicast/core/logger.h"
#include "panicast/core/paths.h"

namespace panicast
{

namespace fs = std::filesystem;

std::string SafeTmpFile::create(const std::string &suffix) {
    ensure_tmp_dir();
    std::string tmpl = tmp_dir() + "/panicast_XXXXXX" + suffix;
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemps(buf.data(), (int)suffix.size());
    if (fd < 0) {
        LOG(fmt::format("[SafeTmpFile] mkstemps failed: {}", strerror(errno)));
        return "";
    }
    close(fd);
    return std::string(buf.data());
}

void SafeTmpFile::remove(const std::string &path) {
    if (!path.empty())
        std::filesystem::remove(path);
}

std::string SafeTmpFile::tmp_dir() {
    return Paths::get_tmp_dir();
}

void SafeTmpFile::ensure_tmp_dir() {
    // Private directory uses 0700 permissions, narrowing the TOCTOU attack surface
    std::error_code ec;
    fs::create_directories(tmp_dir(), ec);
    if (!ec) {
        fs::permissions(tmp_dir(), fs::perms::owner_all, ec);
    }
}

} // namespace panicast
