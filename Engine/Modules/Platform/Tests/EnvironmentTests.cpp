#include <gtest/gtest.h>

#include "Platform/Environment.h"

#include <cstdlib>

using GameEngine::Platform::EnvironmentSwitchEnabled;

namespace
{

constexpr const char* kSwitch = "GE_PLATFORM_ENVIRONMENT_TEST_SWITCH";

void SetSwitch(const char* value)
{
#if defined(_WIN32)
    _putenv_s(kSwitch, value);
#else
    setenv(kSwitch, value, 1);
#endif
}

void ClearSwitch()
{
#if defined(_WIN32)
    _putenv_s(kSwitch, "");
#else
    unsetenv(kSwitch);
#endif
}

struct ScopedSwitchClear
{
    ~ScopedSwitchClear() { ClearSwitch(); }
};

} // namespace

TEST(EnvironmentSwitch, UnsetAndEmptyReturnTheDefault)
{
    ScopedSwitchClear clear;
    ClearSwitch();
    EXPECT_FALSE(EnvironmentSwitchEnabled(kSwitch, false));
    EXPECT_TRUE(EnvironmentSwitchEnabled(kSwitch, true));

    SetSwitch("");
    EXPECT_FALSE(EnvironmentSwitchEnabled(kSwitch, false));
    EXPECT_TRUE(EnvironmentSwitchEnabled(kSwitch, true));
}

TEST(EnvironmentSwitch, OffSpellingsAreOffInAnyCase)
{
    ScopedSwitchClear clear;
    for (const char* off : {"0", "false", "FALSE", "False", "off", "OFF", "no", "No"})
    {
        SetSwitch(off);
        EXPECT_FALSE(EnvironmentSwitchEnabled(kSwitch, false)) << off;
        EXPECT_FALSE(EnvironmentSwitchEnabled(kSwitch, true)) << off << " must win over a true default";
    }
}

TEST(EnvironmentSwitch, AnyOtherValueIsOn)
{
    ScopedSwitchClear clear;
    for (const char* on : {"1", "true", "on", "yes", "2", "00", "nope"})
    {
        SetSwitch(on);
        EXPECT_TRUE(EnvironmentSwitchEnabled(kSwitch, false)) << on;
    }
}
