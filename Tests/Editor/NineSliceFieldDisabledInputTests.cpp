// A 9-slice field on a texture whose import settings refuse writes offers
// nothing on its canvas: the cut handles take no hover under the cursor, so
// none of them lights up, and a press on one starts no drag. Resizing the
// preview is a view change rather than a slice edit and stays live either way,
// which is why the hover below the canvas is asserted alive here too.

#include <gtest/gtest.h>

#include "UI/Controls/NineSliceField.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UIEvents.h"

#include <memory>
#include <optional>

using namespace GameEngine;

namespace
{

constexpr float kFieldWidthPx = 320.0f;
constexpr float kFieldHeightPx = 480.0f;
constexpr uint32_t kSourceWidthTexels = 64;
constexpr uint32_t kSourceHeightTexels = 64;
// The source canvas is the top half of the element and the resizable preview
// the bottom half. Each sweep stays inside its own half so neither can pick up
// the other's handles.
constexpr float kCanvasSweepEndFrac = 0.45f;
constexpr float kPreviewSweepStartFrac = 0.55f;
constexpr float kSweepStepPx = 2.0f;

struct Point
{
    float X = 0.0f;
    float Y = 0.0f;
};

NineSlice AuthoredSlice()
{
    NineSlice slice;
    slice.Enabled = true;
    slice.X[0] = slice.X[1] = 16;
    slice.X[2] = slice.X[3] = 48;
    slice.Y[0] = slice.Y[1] = 16;
    slice.Y[2] = slice.Y[3] = 48;
    return slice;
}

// The field reads its own layout rect when it hit-tests, so a test that never
// runs a layout pass writes the rect the solver would have committed.
void PlaceField(NineSliceField& field)
{
    UILayoutAccess::SetLastLayoutRect(field, 0.0f, 0.0f, kFieldWidthPx, kFieldHeightPx);
    field.SetTexture(GUID{}, kSourceWidthTexels, kSourceHeightTexels);
    field.SetSlice(AuthoredSlice());
    field.ClearDirty(UIElement::VisualDirty);
}

void SendMouse(NineSliceField& field, EventId id, Point at)
{
    UIEvent e{};
    e.Id = id;
    e.X = at.X;
    e.Y = at.Y;
    e.Button = 0;
    e.Target = &field;
    e.CurrentTarget = &field;
    field.DispatchEvent(e);
}

// Where a handle sits is layout-derived, so find one the way a pointer does:
// sweep the band until the field takes a hover and repaints.
std::optional<Point> SweepForHover(NineSliceField& field, float startY, float endY)
{
    for (float y = startY; y < endY; y += kSweepStepPx)
    {
        for (float x = 0.0f; x < kFieldWidthPx; x += kSweepStepPx)
        {
            const Point at{x, y};
            SendMouse(field, kEventMouseMove, at);
            if (field.IsDirty(UIElement::VisualDirty))
                return at;
            field.ClearDirty(UIElement::VisualDirty);
        }
    }
    return std::nullopt;
}

// A cut handle's position, located on a field that accepts edits.
std::optional<Point> FindCutHandlePoint()
{
    NineSliceField probe;
    PlaceField(probe);
    return SweepForHover(probe, 0.0f, kFieldHeightPx * kCanvasSweepEndFrac);
}

TEST(NineSliceFieldDisabledInput, AnEnabledFieldTakesTheHoverAndThePressOnACutHandle)
{
    const std::optional<Point> handle = FindCutHandlePoint();
    ASSERT_TRUE(handle.has_value()) << "no cut handle found on the canvas";

    NineSliceField field;
    PlaceField(field);
    int changing = 0;
    field.SetOnChanging([&changing](const NineSlice&) { ++changing; });

    SendMouse(field, kEventMouseMove, *handle);
    EXPECT_TRUE(field.IsDirty(UIElement::VisualDirty));

    SendMouse(field, kEventMouseDown, *handle);
    EXPECT_EQ(changing, 1);
}

TEST(NineSliceFieldDisabledInput, ADisabledFieldTakesNoHoverAnywhereOnItsCanvas)
{
    NineSliceField field;
    PlaceField(field);
    field.SetDisabled(true);
    field.ClearDirty(UIElement::VisualDirty);

    EXPECT_FALSE(SweepForHover(field, 0.0f, kFieldHeightPx * kCanvasSweepEndFrac).has_value());
}

TEST(NineSliceFieldDisabledInput, ADisabledFieldRefusesThePressOnACutHandle)
{
    const std::optional<Point> handle = FindCutHandlePoint();
    ASSERT_TRUE(handle.has_value()) << "no cut handle found on the canvas";

    NineSliceField field;
    PlaceField(field);
    field.SetDisabled(true);
    int changing = 0;
    int changed = 0;
    field.SetOnChanging([&changing](const NineSlice&) { ++changing; });
    field.SetOnChanged([&changed](const NineSlice&) { ++changed; });

    SendMouse(field, kEventMouseDown, *handle);
    SendMouse(field, kEventMouseUp, *handle);

    EXPECT_EQ(changing, 0);
    EXPECT_EQ(changed, 0);
}

TEST(NineSliceFieldDisabledInput, AFieldInsideADisabledGroupRefusesTheHoverAndThePress)
{
    const std::optional<Point> handle = FindCutHandlePoint();
    ASSERT_TRUE(handle.has_value()) << "no cut handle found on the canvas";

    UIElement group;
    auto fieldOwned = std::make_unique<NineSliceField>();
    NineSliceField* field = fieldOwned.get();
    group.AddChild(std::move(fieldOwned));
    PlaceField(*field);
    group.SetDisabled(true);
    field->ClearDirty(UIElement::VisualDirty);

    int changing = 0;
    field->SetOnChanging([&changing](const NineSlice&) { ++changing; });

    SendMouse(*field, kEventMouseMove, *handle);
    SendMouse(*field, kEventMouseDown, *handle);

    EXPECT_FALSE(field->IsDirty(UIElement::VisualDirty));
    EXPECT_EQ(changing, 0);
}

TEST(NineSliceFieldDisabledInput, ADisabledFieldStillHoversItsPreviewResizeGrip)
{
    NineSliceField field;
    PlaceField(field);
    field.SetDisabled(true);
    field.ClearDirty(UIElement::VisualDirty);

    EXPECT_TRUE(
        SweepForHover(field, kFieldHeightPx * kPreviewSweepStartFrac, kFieldHeightPx).has_value());
}

} // namespace
