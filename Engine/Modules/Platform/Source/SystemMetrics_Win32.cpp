#include "Platform/SystemMetrics.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace GameEngine::Platform
{
namespace
{
// The two getters differ in what they promise on failure, so the zero guards
// below are not the same kind of guard. GetCaretBlinkTime documents zero as its
// failure return. GetDoubleClickTime documents no failure return at all, so its
// zero guard is a sanity floor for a value no user setting can legitimately
// take. Either way the answer is the Windows out-of-the-box value, so a bad
// read cannot be mistaken for "never blink" or "double-clicks are impossible".
constexpr std::chrono::milliseconds kDefaultCaretBlinkHalfPeriod{530};
constexpr std::chrono::milliseconds kDefaultDoubleClickInterval{500};
} // namespace

std::optional<std::chrono::milliseconds> GetCaretBlinkHalfPeriod()
{
    const UINT blinkMs = ::GetCaretBlinkTime();
    if (blinkMs == INFINITE)
        return std::nullopt;
    if (blinkMs == 0)
        return kDefaultCaretBlinkHalfPeriod;
    return std::chrono::milliseconds{blinkMs};
}

std::chrono::milliseconds GetDoubleClickInterval()
{
    const UINT intervalMs = ::GetDoubleClickTime();
    if (intervalMs == 0)
        return kDefaultDoubleClickInterval;
    return std::chrono::milliseconds{intervalMs};
}

} // namespace GameEngine::Platform

#endif // _WIN32
