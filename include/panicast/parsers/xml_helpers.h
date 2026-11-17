// XML parsing helpers: libxml2 attribute access + xmlDoc RAII guard + XML error state query.
//   reset/get_last_xml_error and the libxml2 error callback implementation live in xml_helpers.cpp.
#pragma once

#include <string>
#include <vector>
#include <mutex>

#include <libxml/parser.h>
#include <libxml/tree.h>

namespace panicast
{

// Return the value of the first xml attribute that exists, by candidate name order.
std::string get_xml_prop_any(xmlNodePtr n, std::vector<std::string> ns);

// libxml2 global error callback (hooked in main at startup via xmlSetGenericErrorFunc /
//   xmlSetStructuredErrorFunc), redirecting errors to LOG/EVENT_LOG instead of stderr.
extern std::string g_last_xml_error; // Last XML error message
extern int g_xml_error_count;        // Cumulative error count
void xml_error_handler(void *ctx, const char *msg, ...);
void xml_structured_error_handler(
    void *ctx,
    xmlError *error); // Signature strictly matches xmlStructuredErrorFunc, avoiding C-style casts

// xmlDoc RAII guard: auto xmlFreeDoc on exception/early return, avoiding leaks.
// Provides operator xmlDocPtr() implicit conversion, so existing libxml2 call sites need zero changes.
struct XmlDocGuard {
    xmlDocPtr doc;
    explicit XmlDocGuard(xmlDocPtr d) : doc(d) {}
    ~XmlDocGuard() {
        if (doc)
            xmlFreeDoc(doc);
    }
    XmlDocGuard(const XmlDocGuard &) = delete;
    XmlDocGuard &operator=(const XmlDocGuard &) = delete;
    operator xmlDocPtr() const {
        return doc;
    }
};

// Reset / query the libxml2 global error state.
void reset_xml_error_state();
std::string get_last_xml_error();

} // namespace panicast
