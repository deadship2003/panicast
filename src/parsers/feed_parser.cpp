// Feed parser registry implementation.
#include "panicast/parsers/feed_parser.h"

namespace panicast
{

ParserRegistry &ParserRegistry::instance() {
    // Meyers singleton: function-local static, avoids initialization-order problems across translation units (SIOF).
    static ParserRegistry inst;
    return inst;
}

void ParserRegistry::reg(URLType t, Factory f) {
    regs_[t] = f;
}

std::unique_ptr<IFeedParser> ParserRegistry::create(URLType t) const {
    auto it = regs_.find(t);
    if (it == regs_.end())
        return nullptr;
    return it->second();
}

ParserRegistrar::ParserRegistrar(URLType t, std::unique_ptr<IFeedParser> (*f)()) {
    ParserRegistry::instance().reg(t, f);
}

} // namespace panicast
