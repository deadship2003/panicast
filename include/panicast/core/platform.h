// Platform-related inline utilities: thread-safe local time conversion.
// Uses localtime_r (POSIX); never degenerate to localtime() — it returns a pointer to a shared
// static buffer, which gets overwritten on multicore concurrency.
#pragma once

#include <ctime>

namespace panicast
{

inline struct tm *localtime_local(const std::time_t *t, struct tm *out) {
    return localtime_r(t, out);
}

} // namespace panicast
