// XML parsing helpers: libxml2 attribute access + global error callbacks (redirected to LOG/EVENT_LOG).
#include "panicast/parsers/xml_helpers.h"

#include <cstdarg>
#include <cstring>

#include <fmt/format.h>

#include "panicast/core/logger.h"
#include "panicast/core/event_log.h"

namespace panicast
{

std::string g_last_xml_error; // Stores the last XML error message
int g_xml_error_count = 0;    // Error count
static std::mutex
    g_xml_err_mtx; // Protects the two above (libxml2 callbacks may come from worker threads)

std::string get_xml_prop_any(xmlNodePtr n, std::vector<std::string> ns) {
    for (auto &name : ns) {
        xmlChar *p = xmlGetProp(n, (const xmlChar *)name.c_str());
        if (p) {
            std::string s = (char *)p;
            xmlFree(p);
            return s;
        }
    }
    return "";
}

void xml_error_handler(void *ctx, const char *msg, ...) {
    (void)ctx; // libxml2 standard parameter, currently unused
    char buffer[1024];
    va_list args;
    va_start(args, msg);
    vsnprintf(buffer, sizeof(buffer), msg, args);
    va_end(args);

    // Log to LOG file
    std::string err_msg = buffer;
    // Remove trailing newline
    while (!err_msg.empty() && (err_msg.back() == '\n' || err_msg.back() == '\r')) {
        err_msg.pop_back();
    }

    if (!err_msg.empty()) {
        LOG(fmt::format("[XML] {}", err_msg));
        bool first = false;
        {
            std::lock_guard<std::mutex> lock(g_xml_err_mtx);
            g_last_xml_error = err_msg;
            g_xml_error_count++;
            first = (g_xml_error_count == 1);
        }
        // Only show the first line of error to EVENT_LOG (avoid flooding)
        if (first) {
            EVENT_LOG(fmt::format("XML Error: {}", err_msg.substr(0, 60)));
        }
    }
}

// Fix parameter type to const xmlError* (matches libxml2's xmlStructuredErrorFunc definition)
void xml_structured_error_handler(void *ctx, xmlError *error) {
    (void)ctx; // libxml2 standard parameter, currently unused
    if (error && error->message) {
        std::string msg = error->message;
        // Remove trailing newline
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
            msg.pop_back();
        }

        LOG(fmt::format("[XML] Line {}: {}", error->line, msg));
        bool first = false;
        {
            std::lock_guard<std::mutex> lock(g_xml_err_mtx);
            g_last_xml_error = msg;
            g_xml_error_count++;
            first = (g_xml_error_count == 1);
        }
        if (first) {
            EVENT_LOG(fmt::format("XML Error (L{}): {}", error->line, msg.substr(0, 50)));
        }
    }
}

void reset_xml_error_state() {
    std::lock_guard<std::mutex> lock(g_xml_err_mtx);
    g_last_xml_error.clear();
    g_xml_error_count = 0;
}

std::string get_last_xml_error() {
    std::lock_guard<std::mutex> lock(g_xml_err_mtx);
    return g_last_xml_error;
}

} // namespace panicast
