// yt-dlp safe runner: posix_spawn + argv, no shell parsing, eliminates command injection.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace panicast
{

class YtdlpRunner {
public:
    struct Result {
        int exit_code = -1;
        std::string stdout_output;
        std::string stderr_output;
        bool launched = false;
    };

    // Safely run yt-dlp, passing URL/path as an argv element, eliminating command injection.
    // line_cb: invoked once per line of stdout (for download progress parsing).
    // timeout_sec: timeout in seconds (<=0 means 600); on timeout the child is sent SIGTERM/SIGKILL.
    // source_url: the URL being fetched, used ONLY to resolve the proxy via the Connectivity layer
    //   (IProxyManager) so domain rules match its host (e.g. *.bilibili.com → direct). "" = global
    //   proxy only (legacy behavior). (D45 — url-aware yt-dlp proxy routing.)
    static Result run(const std::vector<std::string> &args,
                      std::function<void(const std::string &)> line_cb = nullptr,
                      int timeout_sec = 600,
                      const std::string &source_url = "");

private:
    static std::string find_ytdlp();
};

} // namespace panicast
