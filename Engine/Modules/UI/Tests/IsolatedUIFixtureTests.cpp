// The instrument, checked before anything is read off it.
//
// IsolatedUIFixture exists because several UI investigations reached wrong
// conclusions from confounded instruments rather than from misread code: a
// browser's emulated device scale that never re-instantiated fonts, an editor
// screenshot dominated by a runtime !important injection, an A/B where one
// capture silently had nothing on screen. A fixture that claims to remove those
// confounders has to demonstrate it, so these tests pin the four properties the
// fixture's readings depend on: the CSS the test wrote is the only CSS in play,
// the cascade actually ran, the content scale reached the whole chain, and the
// same inputs give the same numbers.

#include "DefaultStylesheet.h"
#include "IsolatedUIFixture.h"

#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <cmath>

#include <cstring>
#include <string>
#include <vector>

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

constexpr char kTwoBoxes[] = R"(<uielement id="root">
  <uielement id="box"/>
</uielement>)";

// `box` carries a :hover rule that would be unmissable if it applied, and an
// id rule that would be unmissable if the sheet did not.
constexpr char kTwoBoxesCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#box { width: 100px; height: 50px; background-color: #ff0000; }
#box:hover { width: 320px; }
)";

constexpr float kEpsilon = 0.01f;

} // namespace

// The whole point of a CSS-string harness: the string has to drive the real
// cascade, not merely parse. An unapplied sheet leaves `box` at its default
// (auto) size, so any width assertion at all separates the two outcomes.
TEST(IsolatedUIFixture, CssStringDrivesTheCascade)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kTwoBoxes, kTwoBoxesCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const GameEngine::ResolvedStyle* style = fx.Style("box");
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->Visual.BackgroundColor, 0xFFFF0000u);

    const PhysicalRect box = fx.BorderBox("box");
    EXPECT_NEAR(box.W, 100.0f, kEpsilon);
    EXPECT_NEAR(box.H, 50.0f, kEpsilon);
}

// Exactly two sheets: the UI module's default stylesheet, which every UIManager loads, then the
// test's own. Nothing about the editor's 46 sheets.
TEST(IsolatedUIFixture, OnlyTheDefaultAndTheTestsOwnStylesheetAreAttached)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kTwoBoxes, kTwoBoxesCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const auto& sheets = fx.Manager().GetStylesheets();
    ASSERT_EQ(sheets.size(), 2u);
    EXPECT_EQ(sheets[0], GameEngine::UI::GetDefaultStylesheet());
    ASSERT_NE(sheets[1], nullptr);
    EXPECT_EQ(sheets[1]->SourceName, "IsolatedUIFixture.css");
}

// No pointer or focus event is ever delivered, so no element is in a dynamic
// state. The `#box:hover` rule above would triple the box width if it were.
TEST(IsolatedUIFixture, NoHoverOrFocusStateByDefault)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kTwoBoxes, kTwoBoxesCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    EXPECT_EQ(fx.Manager().GetHoveredElement(), nullptr);
    EXPECT_TRUE(fx.Manager().GetFocusedElementId().empty());
    EXPECT_NEAR(fx.BorderBox("box").W, 100.0f, kEpsilon);
}

// Content scale is the parameter every DPI question turns on, so it has to
// reach the manager and come back out in the emitted geometry. The logical
// viewport is held constant across scales, so the same CSS must give a border
// box that is exactly `scale` times as large — and the primitive the element
// emits must land on that box, not on the logical one.
TEST(IsolatedUIFixture, ContentScaleReachesTheManagerAndTheEmittedGeometry)
{
    constexpr float kScales[] = {1.0f, 1.25f, 1.5f, 2.0f};
    for (float scale : kScales)
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(scale, kTwoBoxes, kTwoBoxesCss);
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic() << " scale=" << scale;

        EXPECT_NEAR(fx.Manager().GetContentScale(), scale, kEpsilon) << "scale=" << scale;

        const PhysicalRect box = fx.BorderBox("box");
        EXPECT_NEAR(box.W, 100.0f * scale, kEpsilon) << "scale=" << scale;
        EXPECT_NEAR(box.H, 50.0f * scale, kEpsilon) << "scale=" << scale;

        // The emitted primitive is the border box SNAPPED to the device grid
        // per edge (SnapPaintRect; BorderEdgeSnapTests pins the rules), so at
        // fractional scales its extent is the difference of rounded edges —
        // 62.5 paints as 63 — while the layout box above keeps the fraction.
        const auto rects = fx.Primitives("box", GameEngine::UI::PrimitiveMode::Rect);
        ASSERT_FALSE(rects.empty()) << "scale=" << scale;
        const float paintW = std::round(box.X + box.W) - std::round(box.X);
        const float paintH = std::round(box.Y + box.H) - std::round(box.Y);
        EXPECT_NEAR(rects[0].W, paintW, kEpsilon) << "scale=" << scale;
        EXPECT_NEAR(rects[0].H, paintH, kEpsilon) << "scale=" << scale;
    }
}

// Two independently built fixtures over the same inputs must agree byte for
// byte. A fixture whose numbers drift between runs cannot support any of the
// comparisons the other suites make with it.
TEST(IsolatedUIFixture, SameInputsProduceIdenticalPrimitives)
{
    IsolatedUIFixture first;
    const bool builtFirst = first.Build(1.5f, kTwoBoxes, kTwoBoxesCss);
    if (!first.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(builtFirst) << first.Diagnostic();

    IsolatedUIFixture second;
    ASSERT_TRUE(second.Build(1.5f, kTwoBoxes, kTwoBoxesCss)) << second.Diagnostic();

    const auto a = first.Primitives("box");
    const auto b = second.Primitives("box");
    ASSERT_FALSE(a.empty());
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i)
    {
        EXPECT_EQ(std::memcmp(&a[i], &b[i], sizeof(GameEngine::UI::UIPrimitive)), 0)
            << "primitive " << i;
    }
}

// The shared device frees a released buffer only at a retirement point. A manager
// built as a local of the test body has no fixture to retire it, so its releases
// stay queued until the test ends: this is the instrument the next test reads, and
// without it that test proves nothing.
TEST(IsolatedUIFixture, ATestBodysOwnManagerQueuesItsBuffersUntilTheTestEnds)
{
    GameEngine::Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        GameEngine::UIManager ui(device);
        std::unique_ptr<GameEngine::UIElement> root;
        ASSERT_TRUE(GameEngine::UIParsing::XMLParser::ParseLayoutFromString(kTwoBoxes, root));
        ui.SetRoot(std::move(root));
        UiRgHarness harness(device);
        ui.Update(0.016f, /*interactive=*/true);
        DriveUiRender(ui, harness);
    }
    EXPECT_GT(device->GetResourcePoolStats().deferredBuffers, 0u);
}

// Declared after the test above so it runs next: whatever a test released is
// destroyed before the next test starts, so a suite's GPU memory stays at one
// test's worth instead of growing with every test.
TEST(IsolatedUIFixture, NoTestStartsWithAnEarlierTestsReleasesPending)
{
    GameEngine::Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    EXPECT_EQ(device->GetResourcePoolStats().deferredBuffers, 0u);
    EXPECT_EQ(device->GetResourcePoolStats().deferredTextures, 0u);
}

// A test that builds one fixture per case holds one fixture's buffers at a time:
// each fixture retires its own releases when it is destroyed. Every fixture's
// SDF rings are about 113 MB, so a loop that kept them all would grow wired GPU
// memory by that much per case.
TEST(IsolatedUIFixture, FixturesBuiltInOneTestHoldOneFixturesBuffersAtATime)
{
    GameEngine::Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    constexpr int kFixtures = 4;
    size_t liveWhileBuilt = 0;
    size_t liveAfterFirst = 0;
    for (int i = 0; i < kFixtures; ++i)
    {
        {
            IsolatedUIFixture fx;
            ASSERT_TRUE(fx.Build(1.0f, kTwoBoxes, kTwoBoxesCss)) << fx.Diagnostic();
            if (i == 0)
                liveWhileBuilt = device->GetResourcePoolStats().liveBuffers;
        }
        const auto stats = device->GetResourcePoolStats();
        EXPECT_EQ(stats.deferredBuffers, 0u) << "after fixture " << i;
        if (i == 0)
            liveAfterFirst = stats.liveBuffers;
        else
            EXPECT_LE(stats.liveBuffers, liveAfterFirst) << "after fixture " << i;
    }
    // The fixture's own buffers are gone once it is destroyed, so the reading
    // above measures them rather than an empty device.
    EXPECT_LT(liveAfterFirst, liveWhileBuilt);
}
