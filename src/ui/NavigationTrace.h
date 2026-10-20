// SUMMIT_NAV_TRACE=1 prints "Summit nav" lines for navigations started from
// the address field: when Enter was pressed, when the window handled it, the
// call into the engine and the state changes the engine reports for the tab,
// stamped with system_time() in seconds and the milliseconds since the key.
// The engine prints its own "Summit nav" lines on the same clock (the load
// on its main loop, policy decisions, the provisional start, the commit, the
// first frame), so one log shows where a navigation's time went.
#pragma once
#include <OS.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>

namespace summit {

inline bool NavigationTraceEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_NAV_TRACE");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

// When the traced navigation's key was pressed (0: none in progress).
inline bigtime_t& NavigationTraceStart()
{
    static bigtime_t start = 0;
    return start;
}

inline void NavigationTrace(const char* format, ...) __attribute__((format(printf, 1, 2)));
inline void NavigationTrace(const char* format, ...)
{
    if (!NavigationTraceEnabled()) return;
    const bigtime_t now = system_time();
    const bigtime_t start = NavigationTraceStart();
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    std::fprintf(stderr, "Summit nav %.3f pid=%d +%.1f ms %s\n", now / 1e6, static_cast<int>(getpid()),
        start ? (now - start) / 1000.0 : 0.0, text);
}

// A navigation begins: the key's own time stamp (input_server's "when"), or
// now when there is none. The wall clock goes with it, to line the trace up
// with the page's performance.timeOrigin.
inline void NavigationTraceBegin(bigtime_t pressed, const char* what)
{
    if (!NavigationTraceEnabled()) return;
    const bigtime_t now = system_time();
    NavigationTraceStart() = pressed > 0 && pressed <= now ? pressed : now;
    timeval wall { };
    gettimeofday(&wall, nullptr);
    const double wallMs = wall.tv_sec * 1000.0 + wall.tv_usec / 1000.0 - (now - NavigationTraceStart()) / 1000.0;
    NavigationTrace("%s (key at %.3f, wall clock %.1f ms)", what, NavigationTraceStart() / 1e6, wallMs);
}

}
