// V0.05B9n3e5g3r P3-3: Unit test infrastructure
// Tests panicast's pure-function modules
// Build: see the BUILD_TESTING option in CMakeLists.txt
//   cmake -DBUILD_TESTING=ON -B build && cmake --build build && ctest --test-dir build

#include <gtest/gtest.h>
#include <algorithm> // std::find (parse_token_all membership checks)
#include <string>
#include <vector>
#include <cstring>

#include "panicast/app/playback_events.h"
#include "panicast/core/event_bus.h"
#include "panicast/core/event_log.h"
#include "panicast/net/proxy_manager.h"
#include "panicast/net/url_classifier.h" // D14-test: link real classify (was mirrored copy)
#include "panicast/playback/sleep_timer.h" // D14-test: link real parse_time_string (was mirrored copy)
#include "panicast/domain/media.h"
#include "panicast/parsers/feed_parser.h"
#include "panicast/app/keymap.h" // D44-test: parse_token ([keys] hotkey rebinding)

// ─── Since panicast.cpp is a single file, we mirror the pure-function logic for independent testing ───
// Real integration tests require splitting the modules (v0.6 work)

// D14-test: the mirrored URLType / UrlPattern / PATTERNS / classify copy was deleted — tests now
//   link the real URLClassifier::classify (src/net/url_classifier.cpp) so they track the production
//   classifier instead of a hand-maintained duplicate that silently drifts (the copy had already
//   diverged: no local-video extension branch, no TikTok, no suffix-aware table matching).
using namespace panicast;

// ─── Keymap::parse_token ([keys] INI hotkey rebinding, D44) ───
TEST(KeymapParseToken, NamesAndChars) {
    EXPECT_EQ(Keymap::parse_token("space"), ' ');
    EXPECT_EQ(Keymap::parse_token("enter"), '\n');
    EXPECT_EQ(Keymap::parse_token("esc"), 27);
    EXPECT_EQ(Keymap::parse_token("tab"), '\t');
    EXPECT_EQ(Keymap::parse_token("backspace"), 127);
    EXPECT_EQ(Keymap::parse_token("p"), 'p');
    EXPECT_EQ(Keymap::parse_token("+"), '+');
    EXPECT_EQ(Keymap::parse_token("R"), 'R');
}
TEST(KeymapParseToken, CaseSensitive) {
    // 'r' and 'R' are distinct keys (wget_wch returns exactly what's typed); rebinding is case-exact.
    EXPECT_NE(Keymap::parse_token("r"), Keymap::parse_token("R"));
}
TEST(KeymapParseToken, NumericTrimsAndInvalid) {
    EXPECT_EQ(Keymap::parse_token("25"), 25);     // numeric keycode (Ctrl+Y)
    EXPECT_EQ(Keymap::parse_token("  p  "), 'p'); // surrounding whitespace trimmed
    EXPECT_EQ(Keymap::parse_token(""), -1);       // empty → unparseable
    EXPECT_EQ(Keymap::parse_token("ctrl+y"), -1); // unsupported name → keeps default
}

TEST(KeymapParseToken, NumericBounds) {
    // D44-prof: std::atoi was UB on overflow; from_chars now rejects out-of-range/unparseable.
    EXPECT_EQ(Keymap::parse_token("0"), '0');          // single char wins → the '0' KEY, not keycode 0
    EXPECT_EQ(Keymap::parse_token("25"), 25);          // multi-digit → numeric keycode path
    EXPECT_EQ(Keymap::parse_token("99999999999"), -1); // overflow → rejected, no UB
    EXPECT_EQ(Keymap::parse_token("12x"), -1);         // trailing garbage → rejected
    EXPECT_EQ(Keymap::parse_token("-5"), -1);          // sign not a digit → unparseable
}

// D44-audit ⑤: alias names bind EVERY terminal encoding (backspace = 127/8/KEY_BACKSPACE,
//   enter = '\n'/'\r'/KEY_ENTER across terminfos — popups defend the same trio). Numeric-keycode
//   values are platform ncurses constants, so assert membership/count, not literals.
TEST(KeymapParseToken, AliasNamesBindAllEncodings) {
    const auto bs = Keymap::parse_token_all("backspace");
    EXPECT_EQ(bs.size(), 3u);
    EXPECT_NE(std::find(bs.begin(), bs.end(), 127), bs.end());
    EXPECT_NE(std::find(bs.begin(), bs.end(), 8), bs.end());
    const auto en = Keymap::parse_token_all("enter");
    EXPECT_EQ(en.size(), 3u);
    EXPECT_NE(std::find(en.begin(), en.end(), '\n'), en.end());
    EXPECT_NE(std::find(en.begin(), en.end(), '\r'), en.end());
}
TEST(KeymapParseToken, NonAliasTokensStaySingle) {
    EXPECT_EQ(Keymap::parse_token_all("p"), (std::vector<int>{'p'}));
    EXPECT_EQ(Keymap::parse_token_all("  bs  ").size(), 3u); // trim applies to aliases too
    EXPECT_EQ(Keymap::parse_token_all("25").size(), 1u);
    EXPECT_TRUE(Keymap::parse_token_all("ctrl+y").empty()); // unparseable → empty (build_keymap logs)
    EXPECT_TRUE(Keymap::parse_token_all("").empty());
}

// ─── URLClassifier test cases ───

TEST(URLClassifier, EmptyUrl) {
    EXPECT_EQ(URLClassifier::classify(""), URLType::UNKNOWN);
}

TEST(URLClassifier, LocalFile) {
    EXPECT_EQ(URLClassifier::classify("file:///home/user/music.mp3"), URLType::RADIO_STREAM);
    EXPECT_EQ(URLClassifier::classify("/abs/path/file.mp3"), URLType::RADIO_STREAM);
}

TEST(URLClassifier, YouTubeChannel) {
    EXPECT_EQ(URLClassifier::classify("https://youtube.com/@SomeChannel"), URLType::YOUTUBE_CHANNEL);
    EXPECT_EQ(URLClassifier::classify("https://youtube.com/channel/UC123456"), URLType::YOUTUBE_CHANNEL);
    EXPECT_EQ(URLClassifier::classify("https://youtube.com/c/SomeChannel"), URLType::YOUTUBE_CHANNEL);
}

TEST(URLClassifier, YouTubeVideo) {
    EXPECT_EQ(URLClassifier::classify("https://youtube.com/watch?v=dQw4w9WgXcQ"), URLType::YOUTUBE_VIDEO);
    EXPECT_EQ(URLClassifier::classify("https://youtu.be/dQw4w9WgXcQ"), URLType::YOUTUBE_VIDEO);
}

TEST(URLClassifier, YouTubePlaylist) {
    EXPECT_EQ(URLClassifier::classify("https://youtube.com/playlist?list=PL123"), URLType::YOUTUBE_PLAYLIST);
}

TEST(URLClassifier, YouTubeRSS) {
    EXPECT_EQ(URLClassifier::classify("https://youtube.com/feeds/videos.xml?channel_id=UC123"),
              URLType::YOUTUBE_RSS);
}

TEST(URLClassifier, ApplePodcast) {
    EXPECT_EQ(URLClassifier::classify("https://podcasts.apple.com/us/podcast/example/id123"),
              URLType::APPLE_PODCAST);
}

TEST(URLClassifier, VideoFiles) {
    EXPECT_EQ(URLClassifier::classify("https://example.com/video.mp4"), URLType::VIDEO_FILE);
    EXPECT_EQ(URLClassifier::classify("https://example.com/video.webm"), URLType::VIDEO_FILE);
    EXPECT_EQ(URLClassifier::classify("https://example.com/video.mkv"), URLType::VIDEO_FILE);
    EXPECT_EQ(URLClassifier::classify("https://example.com/video.avi"), URLType::VIDEO_FILE);
    EXPECT_EQ(URLClassifier::classify("https://example.com/video.mov"), URLType::VIDEO_FILE);
}

TEST(URLClassifier, AudioStreams) {
    EXPECT_EQ(URLClassifier::classify("https://radio.example.com/live.m3u8"), URLType::RADIO_STREAM);
    EXPECT_EQ(URLClassifier::classify("https://example.com/podcast.mp3"), URLType::RADIO_STREAM);
    EXPECT_EQ(URLClassifier::classify("https://example.com/audio.aac"), URLType::RADIO_STREAM);
}

TEST(URLClassifier, OPML) {
    EXPECT_EQ(URLClassifier::classify("https://opml.example.com/Browse.ashx"), URLType::OPML);
    EXPECT_EQ(URLClassifier::classify("https://example.com/feeds.opml"), URLType::OPML);
}

TEST(URLClassifier, RSSPodcast) {
    EXPECT_EQ(URLClassifier::classify("https://example.com/feed.xml"), URLType::RSS_PODCAST);
    EXPECT_EQ(URLClassifier::classify("https://example.com/feed"), URLType::RSS_PODCAST);
    EXPECT_EQ(URLClassifier::classify("https://example.com/rss"), URLType::RSS_PODCAST);
}

TEST(URLClassifier, TuneInBrowseVsStream) {
    EXPECT_EQ(URLClassifier::classify("https://tunein.com/Tune.ashx?id=123&c=pbrowse"), URLType::OPML);
    EXPECT_EQ(URLClassifier::classify("https://tunein.com/Tune.ashx?id=123&c=sbrowse"), URLType::OPML);
    EXPECT_EQ(URLClassifier::classify("https://tunein.com/Tune.ashx?id=123"), URLType::RADIO_STREAM);
}

TEST(URLClassifier, UnknownUrl) {
    EXPECT_EQ(URLClassifier::classify("https://example.com/some/page"), URLType::UNKNOWN);
    EXPECT_EQ(URLClassifier::classify("https://example.com/"), URLType::UNKNOWN);
}

TEST(URLClassifier, PatternPriority) {
    // youtube.com/feeds/videos.xml takes priority over youtube.com/
    EXPECT_EQ(URLClassifier::classify("https://youtube.com/feeds/videos.xml?channel_id=UC123"),
              URLType::YOUTUBE_RSS);
    // youtube.com/watch will not be mismatched by youtube.com/
    EXPECT_EQ(URLClassifier::classify("https://youtube.com/watch?v=abc"), URLType::YOUTUBE_VIDEO);
}

// ─── URLClassifier::is_local_file (D14-3b: single source of truth for ASR is_streaming) ───
TEST(URLClassifier, IsLocalFile) {
    // The boolean 3 ASR call sites share as `is_streaming = !is_local_file(url)` (app_input×2,
    //   app_remote). Locking the predicate directly: classify() routes through it too.
    EXPECT_TRUE(URLClassifier::is_local_file("/abs/path/file.mp3"));
    EXPECT_TRUE(URLClassifier::is_local_file("file:///home/user/music.mp3"));
    EXPECT_FALSE(URLClassifier::is_local_file("https://example.com/stream.mp3"));
    EXPECT_FALSE(URLClassifier::is_local_file("relative/path.mp3"));  // not absolute, not file://
    EXPECT_FALSE(URLClassifier::is_local_file(""));                   // empty
}

// ─── Time format parsing tests (link real SleepTimer::parse_time_string, src/playback/sleep_timer.cpp) ───

TEST(TimeParser, SuffixFormats) {
    EXPECT_EQ(SleepTimer::parse_time_string("5h"), 5 * 3600);
    EXPECT_EQ(SleepTimer::parse_time_string("30m"), 30 * 60);
    EXPECT_EQ(SleepTimer::parse_time_string("90s"), 90);
    EXPECT_EQ(SleepTimer::parse_time_string("5H"), 5 * 3600);
    EXPECT_EQ(SleepTimer::parse_time_string("30M"), 30 * 60);
}

TEST(TimeParser, HHMMSS) {
    EXPECT_EQ(SleepTimer::parse_time_string("1:25:15"), 1 * 3600 + 25 * 60 + 15);
    EXPECT_EQ(SleepTimer::parse_time_string("25:15"), 25 * 60 + 15);
    EXPECT_EQ(SleepTimer::parse_time_string("0:0:30"), 30);
}

TEST(TimeParser, PureNumber) {
    EXPECT_EQ(SleepTimer::parse_time_string("5"), 5 * 3600);
    EXPECT_EQ(SleepTimer::parse_time_string("100"), 100 * 60);
    EXPECT_EQ(SleepTimer::parse_time_string("99"), 99 * 3600);
}

TEST(TimeParser, InvalidInput) {
    EXPECT_EQ(SleepTimer::parse_time_string(""), 0);
    EXPECT_EQ(SleepTimer::parse_time_string("abc"), 0);
    EXPECT_EQ(SleepTimer::parse_time_string("h"), 0);
}

// ─── escape_sql tests: REMOVED (D14-test). DatabaseManager::escape_sql was dead code (0 production
//   callers — AccountsManager SQL now uses prepared statements via account_repo.cpp sqlite3_bind_*).
//   The mirrored copy had also drifted (real strips NUL at database.cpp:521, the copy didn't). ───

// ─── Y01: crypto primitives (token at-rest encryption) ───────────────────────
#include "panicast/core/crypto.h"
#include <string>

namespace
{
std::string hex(const panicast::bytes &b) {
    static const char *d = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t x : b) {
        s.push_back(d[x >> 4]);
        s.push_back(d[x & 0xf]);
    }
    return s;
}
} // namespace

TEST(Y01Crypto, Sha256KnownVector) {
    // NIST: SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    auto h = panicast::sha256(std::string("abc"));
    EXPECT_EQ(hex(h), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Y01Crypto, Base64RoundTrip) {
    std::string msg = "Google OAuth refresh_token #861556708454";
    panicast::bytes b(msg.begin(), msg.end());
    std::string enc = panicast::base64_encode(b.data(), b.size());
    panicast::bytes dec;
    ASSERT_TRUE(panicast::base64_decode(enc, dec));
    std::string back(dec.begin(), dec.end());
    EXPECT_EQ(back, msg);
}

TEST(Y01Crypto, TokenSealOpenRoundTrip) {
    panicast::Key32 k = panicast::machine_key();
    std::string plaintext = "{\"refresh_token\":\"ya29.secret\"}";
    std::string sealed = panicast::token_seal(k, plaintext);
    std::string back;
    ASSERT_TRUE(panicast::token_open(k, sealed, back));
    EXPECT_EQ(back, plaintext);
}

TEST(Y01Crypto, TokenTamperRejected) {
    panicast::Key32 k = panicast::machine_key();
    std::string sealed = panicast::token_seal(k, "secret");
    // Flip one character → HMAC tag must fail verification.
    std::string tampered = sealed;
    tampered[0] = (tampered[0] == 'A') ? 'B' : 'A';
    std::string back;
    EXPECT_FALSE(panicast::token_open(k, tampered, back));
}

TEST(Y01Crypto, WrongMachineKeyRejected) {
    // A different key cannot decrypt what machine_key() sealed.
    panicast::Key32 k0 = panicast::machine_key();
    panicast::Key32 k1 = k0;
    k1[0] ^= 0xff;
    std::string sealed = panicast::token_seal(k0, "secret");
    std::string back;
    EXPECT_FALSE(panicast::token_open(k1, sealed, back));
}

// ─── EventBus tests (D1: type-safe synchronous pub/sub) ───────────────────────
namespace {
struct PeEvent {
    int v;
};
}  // namespace

TEST(EventBus, SubscribeAndPublish) {
    panicast::EventBus &bus = panicast::EventBus::instance();
    int got = -1;
    auto tok = bus.subscribe<PeEvent>([&got](const PeEvent &e) { got = e.v; });
    bus.publish(PeEvent{42});
    EXPECT_EQ(got, 42);
    bus.unsubscribe(tok);
}

TEST(EventBus, MultipleSubscribersAndUnsubscribe) {
    auto &bus = panicast::EventBus::instance();
    int a = 0, b = 0;
    auto ta = bus.subscribe<PeEvent>([&a](const PeEvent &e) { a += e.v; });
    auto tb = bus.subscribe<PeEvent>([&b](const PeEvent &e) { b += e.v; });
    bus.publish(PeEvent{10});
    EXPECT_EQ(a, 10);
    EXPECT_EQ(b, 10);
    bus.unsubscribe(ta);
    bus.publish(PeEvent{5});
    EXPECT_EQ(a, 10);  // ta removed, a unchanged
    EXPECT_EQ(b, 15);  // tb still active
    bus.unsubscribe(tb);
}

TEST(EventBus, PublishWithNoSubscribersIsSafe) {
    auto &bus = panicast::EventBus::instance();
    bus.publish(PeEvent{7});  // no PeEvent subscribers here -> safe no-op
    SUCCEED();
}

TEST(EventBus, LogEventRouting) {
    // The production event type EventLog publishes (D1 first consumer).
    auto &bus = panicast::EventBus::instance();
    std::string received;
    auto tok = bus.subscribe<panicast::LogEvent>([&received](const panicast::LogEvent &e) {
        received = e.msg;
    });
    bus.publish(panicast::LogEvent{"hello-bus"});
    EXPECT_EQ(received, "hello-bus");
    bus.unsubscribe(tok);
}

// ─── Playback events (D9: output-side Core→App messages on the bus) ───────────
TEST(PlaybackEvents, DeliveredOnBus) {
    // The D9 event types PlaybackService publishes; App subscribes. A nullptr node is fine here —
    //   we verify bus delivery + payload, not node validity.
    auto &bus = panicast::EventBus::instance();
    panicast::AppMode got_mode = panicast::AppMode::RADIO;
    bool got_pending = false, track_hit = false;
    int history_hits = 0;

    auto t1 = bus.subscribe<panicast::PlaybackTrackChanged>(
        [&](const panicast::PlaybackTrackChanged &e) { track_hit = true; got_mode = e.mode; });
    auto t2 = bus.subscribe<panicast::PlaybackBufferingChanged>(
        [&](const panicast::PlaybackBufferingChanged &e) { got_pending = e.pending; });
    auto t3 = bus.subscribe<panicast::HistoryChanged>(
        [&](const panicast::HistoryChanged &) { ++history_hits; });

    bus.publish(panicast::PlaybackTrackChanged{panicast::TreeNodePtr{}, panicast::AppMode::IPTV});
    bus.publish(panicast::PlaybackBufferingChanged{true});
    bus.publish(panicast::HistoryChanged{});

    EXPECT_TRUE(track_hit);
    EXPECT_EQ(got_mode, panicast::AppMode::IPTV);
    EXPECT_TRUE(got_pending);
    EXPECT_EQ(history_hits, 1);

    bus.unsubscribe(t1);
    bus.unsubscribe(t2);
    bus.unsubscribe(t3);
}

// ─── ProxyManager tests (D2: Connectivity layer rule chain) ───────────────────
TEST(ProxyManager, GlobalSourceIsUsedWhenNoRules) {
    panicast::ProxyManager pm;
    pm.setGlobalSource([] { return panicast::ProxyConfig{"socks5h://global:1080"}; });
    EXPECT_EQ(pm.resolveProxy("https://anything.com").url, "socks5h://global:1080");
}

// Ctrl+N immediacy contract: the global source is re-read on EVERY resolve (production source
//   reads IniConfig::get_proxy() live), so a proxy edit applies to the next curl fetch /
//   download / yt-dlp run with no restart. Locks the mechanism against a future "cache the
//   resolved proxy" regression.
TEST(ProxyManager, GlobalSourceReadLivePerResolve) {
    panicast::ProxyManager pm;
    std::string current = "socks5h://old:1080";
    pm.setGlobalSource([&current] { return panicast::ProxyConfig{current}; });
    EXPECT_EQ(pm.resolveProxy("https://x.com").url, "socks5h://old:1080");
    current = "http://new:8080"; // Ctrl+N ENTER — value changes between resolves
    EXPECT_EQ(pm.resolveProxy("https://x.com").url, "http://new:8080");
    current.clear(); // proxy disabled — next resolve is direct
    EXPECT_FALSE(pm.resolveProxy("https://x.com").enabled());
}

TEST(ProxyManager, DirectWhenNoSource) {
    panicast::ProxyManager pm;
    EXPECT_FALSE(pm.resolveProxy("https://x.com").enabled());  // direct
}

TEST(ProxyManager, PlatformOverridesGlobal) {
    panicast::ProxyManager pm;
    pm.setGlobalSource([] { return panicast::ProxyConfig{"socks5h://global:1080"}; });
    pm.setPlatform("youtube", {"http://yt:8080"});
    EXPECT_EQ(pm.resolveProxy("https://x.com", "youtube").url, "http://yt:8080");
    EXPECT_EQ(pm.resolveProxy("https://x.com", "bilibili").url, "socks5h://global:1080");
    pm.clearPlatform("youtube");
    EXPECT_EQ(pm.resolveProxy("https://x.com", "youtube").url, "socks5h://global:1080");
}

TEST(ProxyManager, DomainRuleMatches) {
    panicast::ProxyManager pm;
    pm.setGlobalSource([] { return panicast::ProxyConfig{"socks5h://global:1080"}; });
    pm.setDomain("*.googlevideo.com", {"http://gv:8080"});
    EXPECT_EQ(pm.resolveProxy("https://r1.googlevideo.com/videoplayback").url, "http://gv:8080");
    EXPECT_EQ(pm.resolveProxy("https://googlevideo.com").url, "http://gv:8080");  // apex matches
    EXPECT_EQ(pm.resolveProxy("https://other.com").url, "socks5h://global:1080");  // no match -> global
}

TEST(ProxyManager, PlatformBeatsDomain) {
    panicast::ProxyManager pm;
    pm.setGlobalSource([] { return panicast::ProxyConfig{"g"}; });
    pm.setDomain("*.googlevideo.com", {"gv"});
    pm.setPlatform("youtube", {"yt"});
    EXPECT_EQ(pm.resolveProxy("https://r1.googlevideo.com/v", "youtube").url, "yt");  // platform wins
}

TEST(ProxyManager, DomainRuleForcesDirectOverGlobal) {
    // D45: a domain rule with an empty proxy (direct) overrides the global proxy — the
    //   bilibili→direct seed relies on this (CN-domestic site must stay direct under a proxy).
    panicast::ProxyManager pm;
    pm.setGlobalSource([] { return panicast::ProxyConfig{"socks5h://global:1080"}; });
    pm.setDomain("*.bilibili.com", panicast::ProxyConfig{""});  // empty url = direct
    EXPECT_FALSE(pm.resolveProxy("https://www.bilibili.com/video/x").enabled());  // direct
    EXPECT_FALSE(pm.resolveProxy("https://api.bilibili.com/x").enabled());        // direct
    EXPECT_EQ(pm.resolveProxy("https://youtube.com/watch?v=x").url,
              "socks5h://global:1080");  // non-bilibili still uses global
}

// ─── Media / MediaID tests (D14-1: logical identity = canonical absolute source URL) ──
TEST(MediaID, IdentityIsUrl) {
    panicast::MediaID a("https://example.com/feed/ep1.mp3");
    panicast::MediaID b("https://example.com/feed/ep1.mp3");
    panicast::MediaID c("https://example.com/feed/ep2.mp3");
    EXPECT_EQ(a, b);   // same absolute URL → same media
    EXPECT_NE(a, c);   // different URL → different media
    EXPECT_EQ(a.url(), "https://example.com/feed/ep1.mp3");
    EXPECT_TRUE(a.valid());
}

TEST(MediaID, EmptyIsInvalid) {
    panicast::MediaID e;
    EXPECT_FALSE(e.valid());
    EXPECT_TRUE(e.url().empty());
    EXPECT_EQ(e, panicast::MediaID{});  // two empty ids are equal
}

TEST(MediaID, SurvivesNodeDestruction) {
    // Identity is logical (URL), not a pointer into the tree: it outlives the node.
    panicast::MediaID id;
    const std::string url = "https://example.com/track";
    {
        auto n = std::make_shared<panicast::TreeNode>();
        n->url = url;
        id = panicast::media_from_node(n).id;
    }
    EXPECT_TRUE(id.valid());   // node gone, identity intact
    EXPECT_EQ(id.url(), url);
}

TEST(Media, FromNodeCopiesFields) {
    auto n = std::make_shared<panicast::TreeNode>();
    n->url = "https://example.com/track";
    n->title = "Track";
    n->art_url = "https://example.com/cover.png";
    auto m = panicast::media_from_node(n);
    EXPECT_EQ(m.id.url(), "https://example.com/track");
    EXPECT_EQ(m.title, "Track");
    EXPECT_EQ(m.art_url, "https://example.com/cover.png");
    EXPECT_FALSE(m.is_video);  // not derived at the domain layer

    auto empty = panicast::media_from_node(nullptr);
    EXPECT_FALSE(empty.id.valid());
    EXPECT_TRUE(empty.title.empty());
    EXPECT_TRUE(empty.art_url.empty());
}

TEST(MediaID, FromUrlFactory) {
    auto id = panicast::media_id_from_url("https://example.com/x");
    EXPECT_EQ(id.url(), "https://example.com/x");
    EXPECT_TRUE(id.valid());
}

// ─── ParserRegistry tests (D13: IFeedParser self-registering dispatch contract) ──
// The real feed parsers (RSS/OPML/YouTube) are NOT linked into the test binary, so
// ParserRegistry starts empty here. This isolates the reg()/create() dispatch contract
// from the heavy parser implementations (libxml2/network). Which parsers self-register in
// production is proven by their REGISTER_PARSER macros, not asserted here.
namespace {
class DummyFeedParser : public panicast::IFeedParser {
public:
    explicit DummyFeedParser(panicast::URLType t) : t_(t) {}
    panicast::URLType supports() const override { return t_; }
    panicast::TreeNodePtr parse(const panicast::ParseInput &) override {
        return std::make_shared<panicast::TreeNode>();
    }
private:
    panicast::URLType t_;
};
}  // namespace

TEST(ParserRegistry, SingletonIdentity) {
    auto &a = panicast::ParserRegistry::instance();
    auto &b = panicast::ParserRegistry::instance();
    EXPECT_EQ(&a, &b);
}

TEST(ParserRegistry, CreateIsNullWhenUnregistered) {
    // No real parsers linked -> registry empty. RSS_PODCAST / YOUTUBE_CHANNEL are never
    // registered by any test below, so they stay nullptr regardless of test order.
    EXPECT_EQ(panicast::ParserRegistry::instance().create(panicast::URLType::RSS_PODCAST).get(),
              nullptr);
    EXPECT_EQ(panicast::ParserRegistry::instance().create(panicast::URLType::YOUTUBE_CHANNEL).get(),
              nullptr);
}

TEST(ParserRegistry, RegisterThenDispatch) {
    auto &reg = panicast::ParserRegistry::instance();
    reg.reg(panicast::URLType::OPML,
            []() -> std::unique_ptr<panicast::IFeedParser> {
                return std::make_unique<DummyFeedParser>(panicast::URLType::OPML);
            });
    auto p = reg.create(panicast::URLType::OPML);
    ASSERT_NE(p.get(), nullptr);
    EXPECT_EQ(p->supports(), panicast::URLType::OPML);
    // Registering a second key does not evict the first; each key dispatches its own factory.
    reg.reg(panicast::URLType::YOUTUBE_RSS,
            []() -> std::unique_ptr<panicast::IFeedParser> {
                return std::make_unique<DummyFeedParser>(panicast::URLType::YOUTUBE_RSS);
            });
    EXPECT_EQ(reg.create(panicast::URLType::YOUTUBE_RSS)->supports(),
              panicast::URLType::YOUTUBE_RSS);
    EXPECT_NE(reg.create(panicast::URLType::OPML).get(), nullptr);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
