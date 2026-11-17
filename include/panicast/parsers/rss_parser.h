// RSS/Atom parser: RSS 2.0 (with iTunes/Media RSS extensions) + Atom feed + YouTube Atom.
// RSSParser also handles YouTube Atom parsing (parse_youtube_atom/parse_youtube_entry);
//   not split out for now. Registered with ParserRegistry (URLType::RSS_PODCAST).
#pragma once

#include <string>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include "panicast/core/types.h"
#include "panicast/parsers/feed_parser.h"

namespace panicast
{

class RSSParser : public IFeedParser {
public:
    // Parse RSS/Atom XML into a podcast channel node. feed_url selects the YouTube RSS branch.
    static TreeNodePtr parse(std::string xml, const std::string &feed_url);

    // ── ParserRegistry self-registration ──
    static URLType type() {
        return URLType::RSS_PODCAST;
    }
    URLType supports() const override {
        return URLType::RSS_PODCAST;
    }
    TreeNodePtr parse(const ParseInput &in) override {
        return parse(in.data, in.url);
    }

private:
    static void parse_channel(xmlNodePtr ch, TreeNodePtr c);
    static TreeNodePtr parse_item(xmlNodePtr it);
    static void parse_atom_feed(xmlNodePtr feed, TreeNodePtr c);
    static TreeNodePtr parse_atom_entry(xmlNodePtr entry);
    static void parse_youtube_atom(xmlDocPtr doc, TreeNodePtr c);
    static TreeNodePtr parse_youtube_entry(xmlNodePtr entry);
};

} // namespace panicast
