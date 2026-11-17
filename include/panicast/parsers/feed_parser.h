// Feed parser registry (self-registering, Linux module_init style).
// IFeedParser is the unified parser interface; ParserRegistry dispatches factories by URLType.
// Each parser .cpp registers itself at the end via the REGISTER_PARSER(XxxParser) macro, no manual switch maintenance needed.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "panicast/core/types.h"

namespace panicast
{

// Parse input: data=already-fetched body (may be empty for parsers that parse directly from URL, e.g. YouTube), url=feed source URL.
struct ParseInput {
    std::string data;
    std::string url;
};

// Unified feed parser interface.
class IFeedParser {
public:
    virtual ~IFeedParser() = default;
    virtual URLType supports() const = 0;
    virtual TreeNodePtr parse(const ParseInput &) = 0;
};

// Parser registry (Meyers singleton, function-local static, avoids SIOF).
class ParserRegistry {
    using Factory = std::unique_ptr<IFeedParser> (*)();
    std::map<URLType, Factory> regs_;
    ParserRegistry() = default;

public:
    static ParserRegistry &instance();
    void reg(URLType t, Factory f);
    std::unique_ptr<IFeedParser> create(URLType t) const; // nullptr if none
};

// Static registrar: registers a factory with ParserRegistry on construction.
struct ParserRegistrar {
    ParserRegistrar(URLType t, std::unique_ptr<IFeedParser> (*f)());
};

#define REGISTER_PARSER(cls)                                                                       \
    static ::panicast::ParserRegistrar _reg_##cls(                                                 \
        cls::type(),                                                                               \
        []() -> std::unique_ptr<::panicast::IFeedParser> { return std::make_unique<cls>(); });

} // namespace panicast
