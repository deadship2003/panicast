// Logging system: singleton Logger, writes to ~/.local/share/panicast/panicast.log
#pragma once

#include <fstream>
#include <mutex>
#include <string>

namespace panicast
{

class Logger {
public:
    static Logger &instance();

    void init();
    void log(const std::string &msg);
    // LIF-002 (--debug): mirror every log line to stderr so a foreground debug
    //   run shows the production log stream on the console. Off by default.
    void set_console_echo(bool on) { echo_stderr_ = on; }
    ~Logger();

private:
    Logger() = default;
    std::ofstream file_;
    std::mutex mtx_;
    bool echo_stderr_ = false;
};

} // namespace panicast

#define LOG(msg) ::panicast::Logger::instance().log(msg)
