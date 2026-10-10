// ColorPickerScope: the owner-side lifetime of a colour picker. An opener whose
// picker callbacks capture state it tears down (a Settings page) closes its
// scope with that state; the pickers opened under it close and stop calling
// back. The presenter's close actions are stood in for by counters here.

#include <gtest/gtest.h>

#include "ColorPicker/ColorPickerScope.h"

#include <memory>

using namespace GameEngine;
using GameEngine::Editor::ColorPickerScope;

namespace
{
struct CallbackCounts
{
    int applied = 0;
    int cancelled = 0;
    int changing = 0;
};

ColorPickerCallbacks CountingCallbacks(CallbackCounts& counts)
{
    ColorPickerCallbacks callbacks;
    callbacks.onApply = [&counts](uint32_t, float) { ++counts.applied; };
    callbacks.onCancel = [&counts]() { ++counts.cancelled; };
    callbacks.onValueChanging = [&counts](uint32_t, float) { ++counts.changing; };
    return callbacks;
}

void InvokeAll(const ColorPickerCallbacks& callbacks)
{
    callbacks.onValueChanging(0xFF00FF00u, 1.0f);
    callbacks.onApply(0xFF00FF00u, 1.0f);
    callbacks.onCancel();
}
} // namespace

TEST(ColorPickerScope, BoundCallbacksRunWhileTheScopeIsOpen)
{
    ColorPickerScope scope;
    CallbackCounts counts;
    const ColorPickerCallbacks bound = scope.Bind(CountingCallbacks(counts));

    InvokeAll(bound);

    EXPECT_EQ(counts.changing, 1);
    EXPECT_EQ(counts.applied, 1);
    EXPECT_EQ(counts.cancelled, 1);
}

TEST(ColorPickerScope, CloseAllClosesEveryPickerOnceAndSilencesItsCallbacks)
{
    ColorPickerScope scope;
    CallbackCounts counts;
    const ColorPickerCallbacks first = scope.Bind(CountingCallbacks(counts));
    const ColorPickerCallbacks second = scope.Bind(CountingCallbacks(counts));
    int closes = 0;
    scope.AddCloseAction([&closes]() { ++closes; });
    scope.AddCloseAction([&closes]() { ++closes; });

    scope.CloseAll();
    InvokeAll(first);
    InvokeAll(second);

    EXPECT_EQ(closes, 2);
    EXPECT_EQ(counts.changing + counts.applied + counts.cancelled, 0);

    scope.CloseAll();
    EXPECT_EQ(closes, 2) << "a close action runs once, not on every later CloseAll";
}

TEST(ColorPickerScope, APickerOpenedAfterCloseAllStillCallsBack)
{
    ColorPickerScope scope;
    scope.CloseAll();
    CallbackCounts counts;
    const ColorPickerCallbacks bound = scope.Bind(CountingCallbacks(counts));

    InvokeAll(bound);

    EXPECT_EQ(counts.changing, 1);
    EXPECT_EQ(counts.applied, 1);
    EXPECT_EQ(counts.cancelled, 1);
}

TEST(ColorPickerScope, DestroyingTheScopeClosesItsPickers)
{
    CallbackCounts counts;
    ColorPickerCallbacks bound;
    int closes = 0;
    {
        ColorPickerScope scope;
        bound = scope.Bind(CountingCallbacks(counts));
        scope.AddCloseAction([&closes]() { ++closes; });
    }

    InvokeAll(bound);

    EXPECT_EQ(closes, 1);
    EXPECT_EQ(counts.changing + counts.applied + counts.cancelled, 0);
}

// A Settings page can rebuild itself from inside a picker callback (a setting
// whose change refreshes the page). The callback being run finishes; the
// picker's later calls do nothing.
TEST(ColorPickerScope, ClosingFromInsideACallbackLetsThatCallFinish)
{
    ColorPickerScope scope;
    int closes = 0;
    scope.AddCloseAction([&closes]() { ++closes; });
    auto afterClose = std::make_shared<int>(0);
    int changing = 0;
    ColorPickerCallbacks callbacks;
    callbacks.onValueChanging = [&scope, &changing, afterClose](uint32_t, float)
    {
        ++changing;
        scope.CloseAll();
        ++*afterClose;
    };
    const ColorPickerCallbacks bound = scope.Bind(std::move(callbacks));

    bound.onValueChanging(0xFF0000FFu, 1.0f);
    bound.onValueChanging(0xFF0000FFu, 1.0f);

    EXPECT_EQ(changing, 1);
    EXPECT_EQ(*afterClose, 1);
    EXPECT_EQ(closes, 1);
}

TEST(ColorPickerScope, BindKeepsAbsentCallbacksAbsent)
{
    ColorPickerScope scope;
    ColorPickerCallbacks callbacks;
    callbacks.onApply = [](uint32_t, float) {};

    const ColorPickerCallbacks bound = scope.Bind(std::move(callbacks));

    EXPECT_TRUE(bound.onApply);
    EXPECT_FALSE(bound.onCancel);
    EXPECT_FALSE(bound.onValueChanging);
}
