// OPML parser implementation.
#include "panicast/parsers/opml_parser.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#include <libxml/parser.h>

#include <fmt/format.h>

#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/parsers/xml_helpers.h"

namespace panicast
{

namespace fs = std::filesystem;

TreeNodePtr OPMLParser::parse(const std::string &xml) {
    // Reset XML error state
    reset_xml_error_state();

    XmlDocGuard doc(xmlReadMemory(xml.c_str(), xml.size(), "opml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING));
    if (!doc) {
        // Log XML error
        std::string last_err = get_last_xml_error();
        if (!last_err.empty()) {
            LOG(fmt::format("[OPML] Parse failed: {}", last_err));
        }
        return nullptr;
    }

    auto root = std::make_shared<TreeNode>();
    root->title = "Root";
    root->type = NodeType::FOLDER;

    xmlNodePtr rootNode = xmlDocGetRootElement(doc);
    if (rootNode) {
        for (xmlNodePtr n = rootNode->children; n; n = n->next) {
            if (xmlStrcmp(n->name, (const xmlChar *)"body") == 0) {
                parse_outline(n->children, root, true);
            }
        }
    }

    return root; // doc is auto-released by XmlDocGuard
}

std::vector<TreeNodePtr> OPMLParser::import_opml_file(const std::string &filepath) {
    std::vector<TreeNodePtr> feeds;

    if (!fs::exists(filepath))
        return feeds;

    std::ifstream f(filepath);
    std::stringstream buffer;
    buffer << f.rdbuf();
    std::string xml = buffer.str();

    if (xml.empty())
        return feeds;

    // Parse OPML and extract all RSS subscriptions
    extract_feeds(xml, feeds);

    return feeds;
}

void OPMLParser::parse_outline(xmlNodePtr node, TreeNodePtr parent, bool is_top_level) {
    (void)is_top_level; // Parameter kept for future use
    for (; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE || xmlStrcmp(node->name, (const xmlChar *)"outline"))
            continue;

        auto child = std::make_shared<TreeNode>();
        child->title = get_xml_prop_any(node, {"text", "title"});
        child->parent = parent; // Set parent node pointer

        std::string type = get_xml_prop_any(node, {"type"});
        std::string url = get_xml_prop_any(node, {"URL", "url"});
        std::string item = get_xml_prop_any(node, {"item"});
        std::string key = get_xml_prop_any(node, {"key"});
        std::string subtext = get_xml_prop_any(node, {"subtext"});
        std::string image = get_xml_prop_any(node, {"image", "art"}); // station logo
        std::string duration_str = get_xml_prop_any(node, {"topic_duration"});
        std::string stream_type = get_xml_prop_any(node, {"stream_type"});

        child->subtext = subtext;
        child->art_url = image; // remote browse / now-playing artwork
        if (!duration_str.empty()) {
            try {
                child->duration = std::stoi(duration_str);
            } catch (...) {
            }
        }

        // Check whether downloadable (kept for future feature expansion)
        bool is_downloadable = (stream_type == "download");
        (void)is_downloadable; // Variable kept for future use

        if (item == "topic" && type == "audio") {
            child->type = NodeType::PODCAST_EPISODE;
            child->url = url;
            child->children_loaded = true;
            parent->children.push_back(child);
        } else if (type == "audio" && !url.empty()) {
            child->type = NodeType::RADIO_STREAM;
            child->url = url;
            child->children_loaded = true;
            parent->children.push_back(child);
        } else if (type == "link" || type == "search" ||
                   (!url.empty() && url.find("Browse.ashx") != std::string::npos)) {
            child->type = NodeType::FOLDER;
            child->url = url;
            parent->children.push_back(child);
        } else if (node->children != nullptr) {
            child->type = NodeType::FOLDER;
            if (!key.empty()) {
                child->title = child->title.empty() ? key : child->title;
            }
            parent->children.push_back(child);
            parse_outline(node->children, child, false);
            child->children_loaded = true;
        } else if (!url.empty()) {
            child->type = NodeType::RADIO_STREAM;
            child->url = url;
            child->children_loaded = true;
            parent->children.push_back(child);
        }
    }
}

void OPMLParser::extract_feeds(const std::string &xml, std::vector<TreeNodePtr> &feeds) {
    // Reset XML error state
    reset_xml_error_state();

    XmlDocGuard doc(xmlReadMemory(xml.c_str(), xml.size(), "opml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING));
    if (!doc) {
        std::string last_err = get_last_xml_error();
        if (!last_err.empty()) {
            LOG(fmt::format("[OPML] Extract feeds failed: {}", last_err));
        }
        return;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root) {
        return;
    } // doc is auto-released by XmlDocGuard

    std::function<void(xmlNodePtr)> parse_outlines = [&](xmlNodePtr node) {
        for (; node; node = node->next) {
            if (node->type != XML_ELEMENT_NODE)
                continue;

            if (xmlStrcmp(node->name, (const xmlChar *)"outline") == 0) {
                std::string type = get_xml_prop_any(node, {"type"});
                std::string url = get_xml_prop_any(node, {"xmlUrl", "URL", "url"});
                std::string title = get_xml_prop_any(node, {"text", "title"});

                // RSS subscription type
                if ((type == "rss" || !url.empty()) && url.find("http") == 0) {
                    auto feed = std::make_shared<TreeNode>();
                    feed->title = title.empty() ? "Imported Feed" : title;
                    feed->url = url;
                    feed->type = NodeType::PODCAST_FEED;
                    feeds.push_back(feed);
                }

                // Recursively process child nodes
                if (node->children) {
                    parse_outlines(node->children);
                }
            } else if (node->children) {
                parse_outlines(node->children);
            }
        }
    };

    parse_outlines(root);
    // doc is auto-released by XmlDocGuard
}

// -- ParserRegistry self-registration --
REGISTER_PARSER(OPMLParser)

} // namespace panicast
