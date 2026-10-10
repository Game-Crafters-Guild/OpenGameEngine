#pragma once

#include "Logger/Logger.h"

#include <atomic>

namespace GameEngine
{
namespace Platform
{
namespace Web
{

/**
 * @brief Report a desktop-shell surface that the browser cannot provide.
 *
 * The web build reaches these entry points through the same seam desktop uses,
 * so a silent no-op would read to the caller as success. Each call site keeps
 * its own latch: the first call names the surface, later calls stay quiet.
 * Callers still get the API's failure value.
 */
inline void ReportUnavailable(std::atomic<bool>& reported, const char* surface)
{
    if (reported.exchange(true))
    {
        return;
    }
    LOG_WARNING("Platform: {} is not available on web; the call reports failure.", surface);
}

} // namespace Web
} // namespace Platform
} // namespace GameEngine

/// Latch + report for one surface, evaluated at most once per call site.
#define GE_PLATFORM_WEB_UNAVAILABLE(surface)                                        \
    do                                                                              \
    {                                                                               \
        static std::atomic<bool> s_geWebUnavailableReported{false};                 \
        ::GameEngine::Platform::Web::ReportUnavailable(s_geWebUnavailableReported,  \
                                                      (surface));                   \
    } while (false)
