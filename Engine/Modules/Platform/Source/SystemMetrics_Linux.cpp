#include "Platform/SystemMetrics.h"

#if !defined(_WIN32) && !defined(__APPLE__)

namespace GameEngine::Platform
{
namespace
{
// These are GTK's documented defaults, returned as constants because this
// module deliberately takes no GTK/GLib dependency for two scalar settings —
// Platform links X11 and optionally WebKitGTK, never GTK itself. If a GTK
// dependency ever lands, read them live from GtkSettings instead:
//   gtk-cursor-blink       (bool; FALSE means return std::nullopt here)
//   gtk-cursor-blink-time  (the full on+off CYCLE, in milliseconds)
//   gtk-double-click-time  (milliseconds)

// gtk-cursor-blink-time defaults to 1200 ms and describes a FULL cycle, so the
// half-period this accessor is contracted to return is half of that.
constexpr std::chrono::milliseconds kGtkDefaultCaretBlinkHalfPeriod{600};

// gtk-double-click-time defaults to 400 ms.
constexpr std::chrono::milliseconds kGtkDefaultDoubleClickInterval{400};
} // namespace

std::optional<std::chrono::milliseconds> GetCaretBlinkHalfPeriod()
{
    // gtk-cursor-blink defaults to TRUE, so the default answer is "it blinks".
    return kGtkDefaultCaretBlinkHalfPeriod;
}

std::chrono::milliseconds GetDoubleClickInterval()
{
    return kGtkDefaultDoubleClickInterval;
}

} // namespace GameEngine::Platform

#endif // !_WIN32 && !__APPLE__
