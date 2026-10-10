#include "gtest/gtest.h"

#include <cmath>
#include <memory>

#include "UIRgTestHarness.h"

#include "UI/Controls/Slider.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Input/KeyCodes.h"
#include "Rendering/Core/Device.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/ResolvedStyle.h"

using namespace GameEngine;

namespace
{
    struct TestableSlider : public Slider
    {
        void SimulatePointerDown(float mouseX, float x, float y, float W, float H)
        {
            ResolvedStyle style{};
            OnPointerDown(mouseX, y + H * 0.5f, x, y, W, H, style, nullptr);
        }

        void SimulatePointerDrag(float mouseX, float x, float y, float W, float H)
        {
            ResolvedStyle style{};
            OnPointerDrag(mouseX, y + H * 0.5f, x, y, W, H, style, nullptr);
        }
    };

    UIElement* FindSliderChildWithClass(Slider& slider, const char* className)
    {
        for (const auto& child : slider.GetChildren())
        {
            if (child && child->HasClass(className))
                return child.get();
        }
        return nullptr;
    }
}

TEST(SliderTests, ADisabledSliderTakesNoPointerOrKeyInput)
{
    // Disabled itself, or inside a disabled row: a press, a drag and an arrow key all leave the value.
    for (const bool disableTheRow : {false, true})
    {
        auto row = std::make_unique<UIElement>();
        auto owned = std::make_unique<TestableSlider>();
        TestableSlider* slider = owned.get();
        row->AddChild(std::move(owned));
        slider->SetMin(0.0f);
        slider->SetMax(1.0f);
        slider->SetValueWithoutNotify(0.5f);
        UILayoutAccess::SetLastLayoutRect(*slider, 0.0f, 0.0f, 200.0f, 24.0f);
        if (disableTheRow)
            row->SetEnabled(false);
        else
            slider->SetEnabled(false);
        slider->SimulatePointerDown(190.0f, 0.0f, 0.0f, 200.0f, 24.0f);
        slider->SimulatePointerDrag(10.0f, 0.0f, 0.0f, 200.0f, 24.0f);
        EXPECT_FALSE(slider->OnKey(Input::kKeyCode_Right, 0, nullptr));
        EXPECT_EQ(slider->GetValue(), 0.5f) << (disableTheRow ? "the row disabled" : "the slider disabled");

        // The control: enabled again, the same press moves it.
        row->SetEnabled(true);
        slider->SetEnabled(true);
        slider->SimulatePointerDown(190.0f, 0.0f, 0.0f, 200.0f, 24.0f);
        EXPECT_GT(slider->GetValue(), 0.5f);
    }
}

TEST(SliderTests, CreatesThumbChildrenWithExpectedClasses)
{
    TestableSlider slider;

    UIElement* lowThumb = FindSliderChildWithClass(slider, "slider-thumb-low");
    UIElement* highThumb = FindSliderChildWithClass(slider, "slider-thumb-high");
    ASSERT_NE(lowThumb, nullptr);
    ASSERT_NE(highThumb, nullptr);

    EXPECT_TRUE(lowThumb->HasClass("slider-thumb"));
    EXPECT_TRUE(highThumb->HasClass("slider-thumb"));
    EXPECT_TRUE(lowThumb->HasClass("hidden"));
    EXPECT_FALSE(highThumb->HasClass("hidden"));
}

TEST(SliderTests, RangeModeTogglesLowThumbVisibilityAndLayout)
{
    TestableSlider slider;
    slider.SetMin(0.0f);
    slider.SetMax(100.0f);
    slider.SetRangeValues(20.0f, 80.0f);
    UILayoutAccess::SetLastLayoutRect(slider, 0.0f, 0.0f, 200.0f, 24.0f);

    UIElement* lowThumb = FindSliderChildWithClass(slider, "slider-thumb-low");
    UIElement* highThumb = FindSliderChildWithClass(slider, "slider-thumb-high");
    ASSERT_NE(lowThumb, nullptr);
    ASSERT_NE(highThumb, nullptr);

    slider.SetRangeMode(false);
    slider.OnPostLayout();
    EXPECT_TRUE(lowThumb->HasClass("hidden"));

    slider.SetRangeMode(true);
    slider.OnPostLayout();
    EXPECT_FALSE(lowThumb->HasClass("hidden"));
    // Thumb rects are committed via the full-layout path, so the committed
    // layout rect — what the renderer paints — is the observable, not the
    // override store.
    EXPECT_GT(lowThumb->GetLayoutWidth(), 0.0f);
    EXPECT_GT(lowThumb->GetLayoutHeight(), 0.0f);
    EXPECT_GT(highThumb->GetLayoutWidth(), 0.0f);
    EXPECT_GT(highThumb->GetLayoutHeight(), 0.0f);
}

TEST(SliderTests, PointerDownSetsValueWithinRange)
{
    TestableSlider slider;
    slider.SetMin(0.0f);
    slider.SetMax(100.0f);

    // Simulate a click in the middle of a 100px-wide element.
    const float elementX = 0.0f;
    const float elementY = 0.0f;
    const float elementW = 100.0f;
    const float elementH = 20.0f;
    slider.SimulatePointerDown(50.0f, elementX, elementY, elementW, elementH);

    EXPECT_NEAR(slider.GetValue(), 50.0f, 1.0f);
}

TEST(SliderTests, PointerDragPositionsThumbBeforeLiveCallback)
{
    TestableSlider slider;
    slider.SetMin(0.0f);
    slider.SetMax(100.0f);

    constexpr float elementX = 0.0f;
    constexpr float elementY = 0.0f;
    constexpr float elementW = 200.0f;
    constexpr float elementH = 24.0f;
    UILayoutAccess::SetLastLayoutRect(slider, elementX, elementY, elementW, elementH);
    slider.OnPostLayout();

    UIElement* highThumb = FindSliderChildWithClass(slider, "slider-thumb-high");
    ASSERT_NE(highThumb, nullptr);
    // The committed layout rect is what paints; thumbs bypass the override
    // store, so position is observed there.
    const float initialThumbX = highThumb->GetLayoutX();

    float thumbXObservedByCallback = initialThumbX;
    slider.SetOnValueChanging([&](const float&)
    {
        thumbXObservedByCallback = highThumb->GetLayoutX();
    });

    slider.SimulatePointerDown(150.0f, elementX, elementY, elementW, elementH);

    EXPECT_GT(thumbXObservedByCallback, initialThumbX);
    EXPECT_FLOAT_EQ(thumbXObservedByCallback, highThumb->GetLayoutX());
}

TEST(SliderTests, DiscreteStepSnapsToNearestIncrement)
{
    TestableSlider slider;
    slider.SetMin(0.0f);
    slider.SetMax(10.0f);
    slider.SetStep(1.0f);

    const float elementX = 0.0f;
    const float elementY = 0.0f;
    const float elementW = 100.0f;
    const float elementH = 20.0f;

    // Click near the middle; value should snap to an integer within [0,10].
    slider.SimulatePointerDown(53.0f, elementX, elementY, elementW, elementH);
    float v = slider.GetValue();
    EXPECT_GE(v, 0.0f);
    EXPECT_LE(v, 10.0f);
    EXPECT_NEAR(v, std::round(v), 1e-4f);
}

TEST(SliderTests, StepSnapLandsOnExactDecimalGridValues)
{
    // Float accumulation (lo + n*step) off a -5..5 / 0.1 grid used to store
    // -0.099999905 for the -0.1 detent — visible the moment a value display
    // is round-trip honest. The snap must land on the float nearest the
    // DECIMAL the step describes, i.e. exactly what parsing "-0.1" yields.
    TestableSlider slider;
    slider.SetMin(-5.0f);
    slider.SetMax(5.0f);
    slider.SetStep(0.1f);

    slider.SetValue(-0.0999999f);
    EXPECT_EQ(slider.GetValue(), -0.1f);

    slider.SetValue(1.6999999f);
    EXPECT_EQ(slider.GetValue(), 1.7f);

    slider.SetValue(4.86f);
    EXPECT_EQ(slider.GetValue(), 4.9f);
}

TEST(SliderTests, RangeModeMaintainsOrdering)
{
    TestableSlider slider;
    slider.SetMin(0.0f);
    slider.SetMax(100.0f);
    slider.SetRangeMode(true);
    slider.SetRangeValues(20.0f, 80.0f);

    EXPECT_LE(slider.GetRangeStart(), slider.GetValue());

    // Drag the low thumb toward the high end; ordering should be preserved.
    const float elementX = 0.0f;
    const float elementY = 0.0f;
    const float elementW = 100.0f;
    const float elementH = 20.0f;

    // Click near the lower thumb first to select it.
    slider.SimulatePointerDown(20.0f, elementX, elementY, elementW, elementH);
    // Drag beyond the current high value; low should not exceed high.
    slider.SimulatePointerDrag(95.0f, elementX, elementY, elementW, elementH);

    EXPECT_LE(slider.GetRangeStart(), slider.GetValue());
}

TEST(SliderTests, RangeClickNearLowThumbSelectsLow)
{
	TestableSlider slider;
	slider.SetMin(0.0f);
	slider.SetMax(100.0f);
	slider.SetRangeMode(true);
	slider.SetRangeValues(20.0f, 80.0f);

	const float elementX = 0.0f;
	const float elementY = 0.0f;
	const float elementW = 100.0f;
	const float elementH = 20.0f;

	const float lowBefore = slider.GetRangeStart();
	const float highBefore = slider.GetValue();

	// Click and drag near the low thumb; low end should move, high end should stay.
	slider.SimulatePointerDown(20.0f, elementX, elementY, elementW, elementH);
	slider.SimulatePointerDrag(25.0f, elementX, elementY, elementW, elementH);

	EXPECT_NE(slider.GetRangeStart(), lowBefore);
	EXPECT_NEAR(slider.GetValue(), highBefore, 0.5f);
}

TEST(SliderTests, RangeClickNearHighThumbSelectsHigh)
{
	TestableSlider slider;
	slider.SetMin(0.0f);
	slider.SetMax(100.0f);
	slider.SetRangeMode(true);
	slider.SetRangeValues(20.0f, 80.0f);

	const float elementX = 0.0f;
	const float elementY = 0.0f;
	const float elementW = 100.0f;
	const float elementH = 20.0f;

	const float lowBefore = slider.GetRangeStart();
	const float highBefore = slider.GetValue();

	// Click and drag near the high thumb; high end should move, low end should stay.
	slider.SimulatePointerDown(80.0f, elementX, elementY, elementW, elementH);
	slider.SimulatePointerDrag(75.0f, elementX, elementY, elementW, elementH);

	EXPECT_NE(slider.GetValue(), highBefore);
	EXPECT_NEAR(slider.GetRangeStart(), lowBefore, 0.5f);
}

TEST(SliderTests, RangeOverlapClickLeftSelectsLowThumb)
{
	TestableSlider slider;
	slider.SetMin(0.0f);
	slider.SetMax(100.0f);
	slider.SetRangeMode(true);

	// Collapsed range: both thumbs overlap at 50.
	slider.SetRangeValues(50.0f, 50.0f);

	const float elementX = 0.0f;
	const float elementY = 0.0f;
	const float elementW = 200.0f;
	const float elementH = 24.0f;

	const float lowBefore = slider.GetRangeStart();
	const float highBefore = slider.GetValue();
	ASSERT_NEAR(lowBefore, 50.0f, 1e-3f);
	ASSERT_NEAR(highBefore, 50.0f, 1e-3f);

	// Click slightly left of the overlapping thumb and drag further left.
	// With click-side overlap behavior, this should select/move the low thumb.
	slider.SimulatePointerDown(95.0f, elementX, elementY, elementW, elementH);
	slider.SimulatePointerDrag(70.0f, elementX, elementY, elementW, elementH);

	EXPECT_LT(slider.GetRangeStart(), lowBefore);
	EXPECT_NEAR(slider.GetValue(), highBefore, 0.5f);
	EXPECT_LE(slider.GetRangeStart(), slider.GetValue());
}

TEST(SliderTests, RangeOverlapClickRightSelectsHighThumb)
{
	TestableSlider slider;
	slider.SetMin(0.0f);
	slider.SetMax(100.0f);
	slider.SetRangeMode(true);

	// Collapsed range: both thumbs overlap at 50.
	slider.SetRangeValues(50.0f, 50.0f);

	const float elementX = 0.0f;
	const float elementY = 0.0f;
	const float elementW = 200.0f;
	const float elementH = 24.0f;

	const float lowBefore = slider.GetRangeStart();
	const float highBefore = slider.GetValue();
	ASSERT_NEAR(lowBefore, 50.0f, 1e-3f);
	ASSERT_NEAR(highBefore, 50.0f, 1e-3f);

	// Click slightly right of the overlapping thumb and drag further right.
	// This should select/move the high thumb.
	slider.SimulatePointerDown(105.0f, elementX, elementY, elementW, elementH);
	slider.SimulatePointerDrag(130.0f, elementX, elementY, elementW, elementH);

	EXPECT_GT(slider.GetValue(), highBefore);
	EXPECT_NEAR(slider.GetRangeStart(), lowBefore, 0.5f);
	EXPECT_LE(slider.GetRangeStart(), slider.GetValue());
}

TEST(SliderTests, RangeDragLowPastHighSwapsThumbs)
{
	TestableSlider slider;
	slider.SetMin(0.0f);
	slider.SetMax(100.0f);
	slider.SetRangeMode(true);
	slider.SetRangeValues(20.0f, 80.0f);

	const float elementX = 0.0f;
	const float elementY = 0.0f;
	const float elementW = 200.0f;
	const float elementH = 24.0f;

	// Select low thumb then drag well past the high thumb.
	slider.SimulatePointerDown(45.0f, elementX, elementY, elementW, elementH);
	slider.SimulatePointerDrag(180.0f, elementX, elementY, elementW, elementH);

	EXPECT_LE(slider.GetRangeStart(), slider.GetValue());
	// After swapping, range start should be near the old high end (80-ish) and
	// the high end should be pushed right.
	EXPECT_GT(slider.GetValue(), 80.0f);
	EXPECT_GT(slider.GetRangeStart(), 40.0f);
}

TEST(SliderTests, RangeDragHighPastLowSwapsThumbs)
{
	TestableSlider slider;
	slider.SetMin(0.0f);
	slider.SetMax(100.0f);
	slider.SetRangeMode(true);
	slider.SetRangeValues(20.0f, 80.0f);

	const float elementX = 0.0f;
	const float elementY = 0.0f;
	const float elementW = 200.0f;
	const float elementH = 24.0f;

	// Select high thumb then drag well past the low thumb.
	slider.SimulatePointerDown(155.0f, elementX, elementY, elementW, elementH);
	slider.SimulatePointerDrag(20.0f, elementX, elementY, elementW, elementH);

	EXPECT_LE(slider.GetRangeStart(), slider.GetValue());
	// After swapping, the low end should be pulled left and the high end should
	// be near the old low end (20-ish).
	EXPECT_LT(slider.GetRangeStart(), 60.0f);
	EXPECT_LT(slider.GetValue(), 80.0f);
}

// A field claims the PRIMARY button only. Slider is the BaseField that needs no
// font atlas to route a pointer, so it is the one that can drive the whole
// routing path in a unit test — the gate it exercises is BaseField's, shared by
// every field including TextInput.
TEST(SliderTests, RightButtonPressIsNotClaimedAsAFieldInteraction)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    UIManager ui(dev);
    auto owned = std::make_unique<TestableSlider>();
    TestableSlider* slider = owned.get();
    ui.SetRoot(std::move(owned));
    ui.Update(0.0f, /*interactive=*/false);
    // After the layout pass, so the pointer path sees a non-zero rect.
    UILayoutAccess::SetLastLayoutRect(*slider, 0.0f, 0.0f, 200.0f, 24.0f);
    ASSERT_EQ(slider->GetOwnerManager(), &ui) << "the pointer path bails without an owner";

    auto press = [slider](int button)
    {
        UIEvent e{};
        e.Id = kEventMouseDown;
        e.X = 100.0f;
        e.Y = 12.0f;
        e.Button = button;
        e.Target = slider;
        e.CurrentTarget = slider;
        slider->DispatchEvent(e);
        return e;
    };

    const UIEvent left = press(Input::kMouseButton_Left);
    EXPECT_TRUE(left.Handled) << "the primary button edits the field";
    EXPECT_EQ(left.CaptureRequested, slider);

    /* Button 1 is the RIGHT button. Claiming it here swallowed every
       right-button gesture that started over a field — the graph canvas's
       right-drag pan among them. */
    const UIEvent right = press(Input::kMouseButton_Right);
    EXPECT_FALSE(right.Handled)
        << "a right press must bubble past the field to its container";
    EXPECT_EQ(right.CaptureRequested, nullptr);

    const UIEvent middle = press(Input::kMouseButton_Middle);
    EXPECT_FALSE(middle.Handled);
    EXPECT_EQ(middle.CaptureRequested, nullptr);
}
