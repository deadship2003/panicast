// RSS/Atom parser implementation.
#include <filesystem>
#include "panicast/parsers/rss_parser.h"

#include <cstring>

#include <fmt/format.h>

#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/net/url_classifier.h"
#include "panicast/parsers/xml_helpers.h"
#include "panicast/subtitle/subtitle_manager.h" // Y24.7: centralized transcript detection

namespace panicast
{

TreeNodePtr RSSParser::parse(std::string xml, const std::string &feed_url) {
    //Reset XML error state
    reset_xml_error_state();

    size_t p = xml.find('<');
    if (p != std::string::npos && p > 0)
        xml = xml.substr(p);
    if (xml.compare(0, 3, "\xEF\xBB\xBF") == 0)
        xml = xml.substr(3);

    // Prevent parser-error output from HTML content reaching the terminal and causing display corruption
    XmlDocGuard doc(xmlReadMemory(xml.c_str(), xml.size(), "feed", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING));
    if (!doc) {
        //Show more detailed XML errors
        std::string last_err = get_last_xml_error();
        if (!last_err.empty()) {
            EVENT_LOG(fmt::format("XML parse failed: {}", last_err.substr(0, 50)));
        } else {
            EVENT_LOG("XML parse failed: invalid document");
        }
        return nullptr;
    }

    auto channel = std::make_shared<TreeNode>();
    channel->type = NodeType::PODCAST_FEED;
    channel->children_loaded = true;

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root) {
        return nullptr;
    } // doc is released automatically by XmlDocGuard

    bool is_youtube_rss = (feed_url.find("youtube.com/feeds/videos.xml") != std::string::npos);

    if (xmlStrcmp(root->name, (const xmlChar *)"rss") == 0 ||
        xmlStrcmp(root->name, (const xmlChar *)"rdf:RDF") == 0) {
        for (xmlNodePtr n = root->children; n; n = n->next) {
            if (xmlStrcmp(n->name, (const xmlChar *)"channel") == 0) {
                parse_channel(n, channel);
            }
        }
    } else if (xmlStrcmp(root->name, (const xmlChar *)"feed") == 0) {
        if (is_youtube_rss) {
            parse_youtube_atom(doc, channel);
        } else {
            parse_atom_feed(root, channel);
        }
    }

    // Episodes without their own artwork inherit the feed cover — the remote browse
    //   rows and the now-playing screen then show one consistent logo per podcast.
    for (auto &e : channel->children)
        if (e->art_url.empty())
            e->art_url = channel->art_url;
    return channel; // doc is released automatically by XmlDocGuard
}

void RSSParser::parse_channel(xmlNodePtr ch, TreeNodePtr c) {
    for (xmlNodePtr p = ch->children; p; p = p->next) {
        if (p->type != XML_ELEMENT_NODE)
            continue; // Skip text/comment/PI nodes to avoid strcmp(NULL)
        const char *local_name = (const char *)p->name;

        // RSS 2.0 standard field - title
        if (strcmp(local_name, "title") == 0) {
            xmlChar *t = xmlNodeGetContent(p);
            if (t) {
                c->title = (char *)t;
                xmlFree(t);
            }
        }
        // RSS 2.0 standard field - description (podcast summary)
        else if (strcmp(local_name, "description") == 0) {
            // Could store the podcast summary; currently ignored
        }
        // RSS 2.0 standard field - item (episode)
        else if (strcmp(local_name, "item") == 0) {
            auto e = parse_item(p);
            if (e) {
                e->parent = c;
                c->children.push_back(e);
            }
        }
        // iTunes extension - author
        else if (strcmp(local_name, "itunes:author") == 0) {
            xmlChar *t = xmlNodeGetContent(p);
            if (t) {
                // Could store author information
                xmlFree(t);
            }
        }
        // iTunes extension - image (podcast cover) → feed logo (remote browse artwork)
        else if (strcmp(local_name, "itunes:image") == 0) {
            xmlChar *href = xmlGetProp(p, (const xmlChar *)"href");
            if (href) {
                c->art_url = (const char *)href;
                xmlFree(href);
            }
        }
        // Atom-style logo inside a channel (some feeds embed it)
        else if (strcmp(local_name, "image") == 0 || strcmp(local_name, "logo") == 0) {
            xmlChar *href = xmlGetProp(p, (const xmlChar *)"href");
            if (href && c->art_url.empty()) {
                c->art_url = (const char *)href;
            }
            if (href)
                xmlFree(href);
        }
        // iTunes extension - category
        else if (strcmp(local_name, "itunes:category") == 0) {
            xmlChar *txt = xmlGetProp(p, (const xmlChar *)"text");
            if (txt) {
                // Could store category information
                xmlFree(txt);
            }
        }
        // Atom extension - link (pagination/self-reference)
        // Namespace: xmlns:atom="http://www.w3.org/2005/Atom"
        else if (strcmp(local_name, "link") == 0 || strcmp(local_name, "atom:link") == 0) {
            xmlChar *rel = xmlGetProp(p, (const xmlChar *)"rel");
            xmlChar *href = xmlGetProp(p, (const xmlChar *)"href");
            if (rel && href) {
                std::string rel_val = (char *)rel;
                std::string href_val = (char *)href;
                // atom:link rel="next" - pagination support
                if (rel_val == "next" && !href_val.empty()) {
                    // Could store the next-page URL for paginated loading
                    LOG(fmt::format("[RSS] Found next page: {}", href_val));
                }
                xmlFree(rel);
                xmlFree(href);
            } else {
                if (rel)
                    xmlFree(rel);
                if (href)
                    xmlFree(href);
            }
        }
    }
}

TreeNodePtr RSSParser::parse_item(xmlNodePtr it) {
    auto ep = std::make_shared<TreeNode>();
    ep->type = NodeType::PODCAST_EPISODE;
    std::string url;       // Audio/video URL
    std::string media_url; // Media RSS URL (fallback)

    for (xmlNodePtr i = it->children; i; i = i->next) {
        if (i->type != XML_ELEMENT_NODE)
            continue; // Skip text/comment/PI nodes to avoid strcmp(NULL)
        const char *local_name = (const char *)i->name;

        // RSS 2.0 standard fields
        if (strcmp(local_name, "title") == 0) {
            xmlChar *t = xmlNodeGetContent(i);
            if (t) {
                ep->title = (char *)t;
                xmlFree(t);
            }
        }
        // RSS 2.0 enclosure - highest priority
        else if (strcmp(local_name, "enclosure") == 0) {
            std::string u = get_xml_prop_any(i, {"url"});
            if (!u.empty())
                url = u;
        }
        // Media RSS support (media:content) - fallback for enclosure
        // Namespace: xmlns:media="http://search.yahoo.com/mrss/"
        else if (strcmp(local_name, "content") == 0 || strcmp(local_name, "media:content") == 0) {
            xmlChar *media_url_attr = xmlGetProp(i, (const xmlChar *)"url");
            xmlChar *type_attr = xmlGetProp(i, (const xmlChar *)"type");
            if (media_url_attr) {
                std::string murl = (char *)media_url_attr;
                std::string mime = type_attr ? (char *)type_attr : "";
                // Only accept audio/video media:content
                if (mime.find("audio") != std::string::npos ||
                    mime.find("video") != std::string::npos ||
                    murl.find(".mp3") != std::string::npos ||
                    murl.find(".mp4") != std::string::npos ||
                    murl.find(".m4a") != std::string::npos ||
                    murl.find(".ogg") != std::string::npos ||
                    murl.find(".webm") != std::string::npos) {
                    if (media_url.empty())
                        media_url = murl;
                }
                xmlFree(media_url_attr);
            }
            if (type_attr)
                xmlFree(type_attr); // Must be freed whether or not url exists, to avoid leaks
        }
        // Media RSS group - nested media:content
        else if (strcmp(local_name, "group") == 0 || strcmp(local_name, "media:group") == 0) {
            for (xmlNodePtr mc = i->children; mc; mc = mc->next) {
                const char *mc_name = (const char *)mc->name;
                if (strcmp(mc_name, "content") == 0 || strcmp(mc_name, "media:content") == 0) {
                    xmlChar *media_url_attr = xmlGetProp(mc, (const xmlChar *)"url");
                    xmlChar *type_attr = xmlGetProp(mc, (const xmlChar *)"type");
                    if (media_url_attr) {
                        std::string murl = (char *)media_url_attr;
                        std::string mime = type_attr ? (char *)type_attr : "";
                        if ((mime.find("audio") != std::string::npos ||
                             mime.find("video") != std::string::npos ||
                             murl.find(".mp3") != std::string::npos ||
                             murl.find(".m4a") != std::string::npos) &&
                            media_url.empty()) {
                            media_url = murl;
                        }
                        xmlFree(media_url_attr);
                        if (type_attr)
                            xmlFree(type_attr);
                    }
                }
            }
        }
        // link field - last fallback
        else if (strcmp(local_name, "link") == 0 && url.empty() && media_url.empty()) {
            xmlChar *t = xmlNodeGetContent(i);
            if (t) {
                std::string l = (char *)t;
                xmlFree(t);
                if (l.find(".mp3") != std::string::npos || l.find(".mp4") != std::string::npos ||
                    l.find(".m4a") != std::string::npos)
                    url = l;
            }
        }
        // pubDate / dc:date - publish date
        else if (strcmp(local_name, "pubDate") == 0 || strcmp(local_name, "dc:date") == 0) {
            xmlChar *t = xmlNodeGetContent(i);
            if (t) {
                ep->subtext = (char *)t;
                xmlFree(t);
            }
        }
        // iTunes extension - duration
        else if (strcmp(local_name, "duration") == 0 ||
                 strcmp(local_name, "itunes:duration") == 0) {
            xmlChar *t = xmlNodeGetContent(i);
            if (t) {
                std::string dur = (char *)t;
                xmlFree(t);
                int d = 0;
                size_t colon1 = dur.find(':');
                // Colon branch is also wrapped in try/catch — malformed durations
                //   ("1:30:abc"/empty segment/out-of-range) throw invalid_argument/out_of_range
                try {
                    if (colon1 != std::string::npos) {
                        size_t colon2 = dur.find(':', colon1 + 1);
                        if (colon2 != std::string::npos) {
                            d = std::stoi(dur.substr(0, colon1)) * 3600 +
                                std::stoi(dur.substr(colon1 + 1, colon2 - colon1 - 1)) * 60 +
                                std::stoi(dur.substr(colon2 + 1));
                        } else {
                            d = std::stoi(dur.substr(0, colon1)) * 60 +
                                std::stoi(dur.substr(colon1 + 1));
                        }
                    } else {
                        d = std::stoi(dur);
                    }
                } catch (...) {
                    d = 0;
                }
                ep->duration = d;
            }
        }
        // iTunes extension - author (stored in extra field, currently unused)
        else if (strcmp(local_name, "author") == 0 || strcmp(local_name, "itunes:author") == 0) {
            // Could store author information; currently ignored
        }
        // iTunes extension - image (episode artwork; often the feed cover). libxml
        //   strips the namespace prefix from ->name for xmlns-declared elements, so
        //   match the bare "image" too (href attribute in both spellings).
        else if (strcmp(local_name, "itunes:image") == 0 || strcmp(local_name, "image") == 0) {
            xmlChar *href = xmlGetProp(i, (const xmlChar *)"href");
            if (href) {
                ep->art_url = (const char *)href;
                xmlFree(href);
            }
        }
        // Podcast 2.0 extension - chapters (chapter file)
        else if (strcmp(local_name, "chapters") == 0 ||
                 strcmp(local_name, "podcast:chapters") == 0) {
            xmlChar *ch_url = xmlGetProp(i, (const xmlChar *)"url");
            if (ch_url) {
                // Could store the chapters URL; currently ignored
                xmlFree(ch_url);
            }
        }
        // Podcast 2.0 extension - transcript (subtitle/transcript)
        // Y16: detect and store the transcript URL — used for 📜 emoji + sub-file loading.
        else if (strcmp(local_name, "transcript") == 0 ||
                 strcmp(local_name, "podcast:transcript") == 0) {
            xmlChar *tr_url = xmlGetProp(i, (const xmlChar *)"url");
            xmlChar *tr_type =
                xmlGetProp(i, (const xmlChar *)"type"); // Y24.7: application/json etc.
            if (tr_url) {
                std::string url_str = (char *)tr_url;
                std::string type_str = tr_type ? (char *)tr_type : "";
                if (!url_str.empty()) {
                    // Y24.7: centralized detection (sets subtitle_url/has_subtitle/subtitle_type + LOG).
                    SubtitleManager::detect_from_rss(ep, url_str, type_str);
                }
                xmlFree(tr_url);
            }
            if (tr_type)
                xmlFree(tr_type);
        }
    }

    // URL resolution priority: enclosure > media:content > link
    std::string final_url = url.empty() ? media_url : url;

    if (!final_url.empty()) {
        ep->url = final_url;
        ep->children_loaded = true;
        // Detect video URL
        URLType url_type = URLClassifier::classify(final_url);
        if (url_type == URLType::VIDEO_FILE) {
            ep->is_youtube = false; // Not YouTube, but is video
        }
        // Y16: if no RSS transcript URL found, check for local sidecar (.srt/.vtt/.lrc)
        //   alongside a downloaded file. If the episode has a local_file, check for same-basename
        //   subtitle files. This catches user-placed sidecar subtitles.
        if (!ep->has_subtitle && !ep->local_file.empty()) {
            std::string base = ep->local_file;
            // Strip extension
            size_t dot = base.find_last_of('.');
            if (dot != std::string::npos)
                base = base.substr(0, dot);
            for (const char *ext : {".srt", ".vtt", ".lrc"}) {
                std::string sidecar = base + ext;
                std::error_code ec;
                if (std::filesystem::exists(sidecar, ec)) {
                    ep->subtitle_url = sidecar; // local path
                    ep->has_subtitle = true;
                    LOG(fmt::format("[RSS] transcript sidecar detected for '{}': {}", ep->title,
                                    sidecar));
                    break;
                }
            }
        }
        return ep;
    }
    return nullptr;
}

void RSSParser::parse_atom_feed(xmlNodePtr feed, TreeNodePtr c) {
    for (xmlNodePtr n = feed->children; n; n = n->next) {
        if (xmlStrcmp(n->name, (const xmlChar *)"title") == 0) {
            xmlChar *t = xmlNodeGetContent(n);
            if (t) {
                c->title = (char *)t;
                xmlFree(t);
            }
        } else if (xmlStrcmp(n->name, (const xmlChar *)"entry") == 0) {
            auto e = parse_atom_entry(n);
            if (e) {
                e->parent = c;
                c->children.push_back(e);
            }
        }
    }
}

TreeNodePtr RSSParser::parse_atom_entry(xmlNodePtr entry) {
    auto ep = std::make_shared<TreeNode>();
    ep->type = NodeType::PODCAST_EPISODE;

    for (xmlNodePtr child = entry->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE)
            continue;

        const char *name = (const char *)child->name;
        if (strcmp(name, "title") == 0) {
            xmlChar *t = xmlNodeGetContent(child);
            if (t) {
                ep->title = (char *)t;
                xmlFree(t);
            }
        } else if (strcmp(name, "link") == 0) {
            xmlChar *href = xmlGetProp(child, (const xmlChar *)"href");
            xmlChar *type = xmlGetProp(child, (const xmlChar *)"type");
            if (href) {
                std::string link = (char *)href;
                std::string mime = type ? (char *)type : "";
                if (mime.find("audio") != std::string::npos ||
                    mime.find("video") != std::string::npos ||
                    link.find(".mp3") != std::string::npos ||
                    link.find(".mp4") != std::string::npos ||
                    link.find(".m4a") != std::string::npos) {
                    ep->url = link;
                }
                xmlFree(href);
            }
            if (type)
                xmlFree(type); // Must be freed whether or not href exists, to avoid leaks
        } else if (strcmp(name, "published") == 0 || strcmp(name, "updated") == 0) {
            xmlChar *t = xmlNodeGetContent(child);
            if (t) {
                ep->subtext = (char *)t;
                xmlFree(t);
            }
        }
    }

    if (!ep->url.empty())
        return ep;
    return nullptr;
}

void RSSParser::parse_youtube_atom(xmlDocPtr doc, TreeNodePtr c) {
    xmlXPathContextPtr xpath_ctx = xmlXPathNewContext(doc);
    if (!xpath_ctx)
        return;

    xmlXPathRegisterNs(xpath_ctx, (const xmlChar *)"atom",
                       (const xmlChar *)"http://www.w3.org/2005/Atom");
    xmlXPathRegisterNs(xpath_ctx, (const xmlChar *)"yt",
                       (const xmlChar *)"http://www.youtube.com/xml/schemas/2015");
    xmlXPathRegisterNs(xpath_ctx, (const xmlChar *)"media",
                       (const xmlChar *)"http://search.yahoo.com/mrss/");

    xmlXPathObjectPtr title_result = xmlXPathNodeEval(
        xmlDocGetRootElement(doc), (const xmlChar *)".//*[local-name()='title'][1]", xpath_ctx);

    if (title_result && title_result->nodesetval && title_result->nodesetval->nodeNr > 0) {
        xmlChar *t = xmlNodeGetContent(title_result->nodesetval->nodeTab[0]);
        if (t) {
            c->title = (char *)t;
            xmlFree(t);
        }
    }
    if (title_result)
        xmlXPathFreeObject(title_result);

    xmlXPathObjectPtr entries = xmlXPathNodeEval(
        xmlDocGetRootElement(doc), (const xmlChar *)".//*[local-name()='entry']", xpath_ctx);

    if (entries && entries->nodesetval) {
        for (int i = 0; i < entries->nodesetval->nodeNr; ++i) {
            auto ep = parse_youtube_entry(entries->nodesetval->nodeTab[i]);
            if (ep) {
                ep->parent = c;
                c->children.push_back(ep);
            }
        }
    }

    if (entries)
        xmlXPathFreeObject(entries);
    xmlXPathFreeContext(xpath_ctx);
}

TreeNodePtr RSSParser::parse_youtube_entry(xmlNodePtr entry) {
    auto ep = std::make_shared<TreeNode>();
    ep->type = NodeType::PODCAST_EPISODE;
    ep->is_youtube = true;
    std::string video_id, title, link_url;

    for (xmlNodePtr child = entry->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE)
            continue;
        const char *local_name = (const char *)child->name;

        if (strcmp(local_name, "title") == 0 && title.empty()) {
            xmlChar *t = xmlNodeGetContent(child);
            if (t) {
                title = (char *)t;
                xmlFree(t);
            }
        } else if (strcmp(local_name, "videoId") == 0) {
            xmlChar *vid = xmlNodeGetContent(child);
            if (vid) {
                std::string vid_str = (char *)vid;
                if (vid_str.length() == 11)
                    video_id = vid_str;
                xmlFree(vid);
            }
        } else if (strcmp(local_name, "link") == 0) {
            xmlChar *href = xmlGetProp(child, (const xmlChar *)"href");
            xmlChar *rel = xmlGetProp(child, (const xmlChar *)"rel");
            if (href && (!rel || strcmp((char *)rel, "alternate") == 0))
                link_url = (char *)href;
            if (href)
                xmlFree(href);
            if (rel)
                xmlFree(rel);
        }
    }

    if (!video_id.empty())
        ep->url = fmt::format("https://www.youtube.com/watch?v={}", video_id);
    else if (!link_url.empty())
        ep->url = link_url;
    ep->title = title;

    if (!ep->title.empty() && !ep->url.empty())
        return ep;
    return nullptr;
}

// ── ParserRegistry self-registration ──
REGISTER_PARSER(RSSParser)
// YOUTUBE_RSS (YouTube Atom feed) is also parsed by RSSParser; registered separately.
static ::panicast::ParserRegistrar _reg_rss_youtube(
    ::panicast::URLType::YOUTUBE_RSS,
    []() -> std::unique_ptr<::panicast::IFeedParser> { return std::make_unique<RSSParser>(); });

} // namespace panicast
