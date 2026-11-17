// OPML parser: parses OPML subscription directory tree + imports RSS subscriptions from OPML files.
// Registered to ParserRegistry (URLType::OPML).
#pragma once

#include <string>
#include <vector>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "panicast/core/types.h"
#include "panicast/parsers/feed_parser.h"

namespace panicast
{

class OPMLParser : public IFeedParser {
public:
    // Parse OPML XML into a directory tree.
    static TreeNodePtr parse(const std::string &xml);

    // Import subscriptions from an OPML file (extract all RSS feeds).
    static std::vector<TreeNodePtr> import_opml_file(const std::string &filepath);

    // ── ParserRegistry self-registration ──
    static URLType type() {
        return URLType::OPML;
    }
    URLType supports() const override {
        return URLType::OPML;
    }
    TreeNodePtr parse(const ParseInput &in) override {
        return parse(in.data);
    }

private:
    static void parse_outline(xmlNodePtr node, TreeNodePtr parent, bool is_top_level = false);
    static void extract_feeds(const std::string &xml, std::vector<TreeNodePtr> &feeds);
};

} // namespace panicast
