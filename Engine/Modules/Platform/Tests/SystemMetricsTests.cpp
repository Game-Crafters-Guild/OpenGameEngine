#include <gtest/gtest.h>

#include "Platform/SystemMetrics.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <optional>
#include <string>
#endif

using namespace GameEngine;

#if defined(_WIN32)
namespace
{
// The accessors under test call GetCaretBlinkTime / GetDoubleClickTime. Reading
// the same numbers back through those functions would only prove the compiler
// works, so the oracle here is the registry the Control Panel actually writes
// to — a different mechanism reaching the same user preference.
std::optional<long> ReadPreferenceValue(const wchar_t* subKey, const wchar_t* valueName)
{
    wchar_t buffer[64] = {};
    DWORD size = sizeof(buffer);
    const LSTATUS status =
        ::RegGetValueW(HKEY_CURRENT_USER, subKey, valueName, RRF_RT_REG_SZ, nullptr, buffer, &size);
    if (status != ERROR_SUCCESS)
        return std::nullopt;
    try
    {
        return std::stol(std::wstring(buffer));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}
} // namespace

TEST(SystemMetricsTests, DoubleClickIntervalMatchesTheUserPreferenceTheControlPanelWrote)
{
    const std::optional<long> preference = ReadPreferenceValue(L"Control Panel\\Mouse", L"DoubleClickSpeed");
    if (!preference.has_value())
        GTEST_SKIP() << "No DoubleClickSpeed preference on this account.";

    EXPECT_EQ(Platform::GetDoubleClickInterval().count(), *preference);
}

TEST(SystemMetricsTests, CaretBlinkHalfPeriodMatchesTheUserPreferenceTheControlPanelWrote)
{
    const std::optional<long> preference = ReadPreferenceValue(L"Control Panel\\Desktop", L"CursorBlinkRate");
    if (!preference.has_value())
        GTEST_SKIP() << "No CursorBlinkRate preference on this account.";

    const std::optional<std::chrono::milliseconds> halfPeriod = Platform::GetCaretBlinkHalfPeriod();

    // A negative CursorBlinkRate is how the Control Panel records "never blink",
    // and it must survive as nullopt rather than as some very large duration.
    if (*preference < 0)
    {
        EXPECT_FALSE(halfPeriod.has_value());
        return;
    }

    ASSERT_TRUE(halfPeriod.has_value());
    EXPECT_EQ(halfPeriod->count(), *preference);
}
#endif // _WIN32

TEST(SystemMetricsTests, DoubleClickIntervalIsWithinTheRangeTheOsCanReport)
{
    const std::chrono::milliseconds interval = Platform::GetDoubleClickInterval();

    // The lower bound is the accessor's own contract rather than a guess at
    // what a user would pick: every platform path substitutes a real product
    // default rather than passing zero through, so zero means the accessor
    // returned something uninitialised. Windows documents 5000 ms as the
    // maximum GetDoubleClickTime can return, and the macOS and GTK defaults sit
    // far below that.
    EXPECT_GT(interval, std::chrono::milliseconds::zero());
    EXPECT_LE(interval, std::chrono::milliseconds{5000});
}

TEST(SystemMetricsTests, CaretBlinkHalfPeriodIsEitherAbsentOrAUsableDuration)
{
    const std::optional<std::chrono::milliseconds> halfPeriod = Platform::GetCaretBlinkHalfPeriod();
    if (!halfPeriod.has_value())
        return; // "never blink" is a valid answer, not a failure.

    EXPECT_GT(*halfPeriod, std::chrono::milliseconds::zero());
    EXPECT_LE(*halfPeriod, std::chrono::milliseconds{10000});
}
