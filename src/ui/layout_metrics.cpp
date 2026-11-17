#include "panicast/ui/layout_metrics.h"

namespace panicast
{

LayoutMetrics &LayoutMetrics::instance() {
    static LayoutMetrics lm;
    return lm;
}

} // namespace panicast
