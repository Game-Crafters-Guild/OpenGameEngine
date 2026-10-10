#include "Platform/SystemMetrics.h"

#if defined(__APPLE__)

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

namespace GameEngine::Platform
{
namespace
{
// AppKit's behaviour when the corresponding user default is absent.
constexpr std::chrono::milliseconds kDefaultCaretBlinkHalfPeriod{500};
constexpr std::chrono::milliseconds kDefaultDoubleClickInterval{500};

std::chrono::milliseconds SecondsToMilliseconds(double seconds)
{
    return std::chrono::milliseconds{static_cast<long long>((seconds * 1000.0) + 0.5)};
}
} // namespace

std::optional<std::chrono::milliseconds> GetCaretBlinkHalfPeriod()
{
    // AppKit takes the two halves of the insertion-point blink from these user
    // defaults, both expressed in SECONDS. Absent is the common case, and means
    // the AppKit default. objectForKey distinguishes absent from a written
    // zero, which is what makes "off half is zero" readable as "never blinks"
    // instead of collapsing into the default.
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];

    if ([defaults objectForKey:@"NSTextInsertionPointBlinkPeriodOff"] != nil &&
        [defaults doubleForKey:@"NSTextInsertionPointBlinkPeriodOff"] <= 0.0)
    {
        return std::nullopt;
    }

    if ([defaults objectForKey:@"NSTextInsertionPointBlinkPeriodOn"] != nil)
    {
        const double onSeconds = [defaults doubleForKey:@"NSTextInsertionPointBlinkPeriodOn"];
        if (onSeconds > 0.0)
            return SecondsToMilliseconds(onSeconds);
    }

    return kDefaultCaretBlinkHalfPeriod;
}

std::chrono::milliseconds GetDoubleClickInterval()
{
    // NSEvent.doubleClickInterval is in SECONDS and already reflects the
    // Mouse / Trackpad preference pane's double-click speed.
    const double seconds = [NSEvent doubleClickInterval];
    if (seconds <= 0.0)
        return kDefaultDoubleClickInterval;
    return SecondsToMilliseconds(seconds);
}

} // namespace GameEngine::Platform

#endif // __APPLE__
