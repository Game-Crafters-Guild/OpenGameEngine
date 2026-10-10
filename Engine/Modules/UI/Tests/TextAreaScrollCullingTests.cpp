// A TextArea inside a ScrollView draws only the lines the viewport shows. The
// visible band is measured from where the area's text sits against the clip
// viewport of the scroll view that moves it: one area of several stacked in the
// scroll content still draws its first lines once they are scrolled into view,
// and an area the scroll view does not move is not culled by its offset.

#include "IsolatedUIFixture.h"

#include "UI/Controls/ScrollView.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using GameEngine::ScrollView;
using GameEngine::UIElement;
using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{
// Thirty lines of 20px above three lines: the second area starts 600px down the
// content. `&#10;` is a newline that survives XML attribute parsing.
constexpr char kStackedXml[] = R"(<uielement id="root">
  <scrollview id="scroll">
    <textarea id="above" value="H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H&#10;H"/>
    <textarea id="below" value="A&#10;B&#10;C"/>
  </scrollview>
</uielement>)";

constexpr char kStackedCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 200px; }
#scroll { width: 400px; height: 200px; }
#above, #below {
  width: 300px;
  font-family: Roboto;
  font-size: 16px;
  line-height: 20px;
  white-space: nowrap;
  padding: 0px;
  border-width: 0px;
  flex-shrink: 0;
}
#above { height: 600px; }
#below { height: 60px; }
)";

// The same areas, the lower one painting a background: it owns primitives at
// every scroll offset, so a scroll re-emits it in place and only the line band
// decides what it draws.
constexpr char kPaintedBelowCss[] = R"(
#below { background-color: #202020; }
)";

// The painted lower area with a 4px top padding and room to scroll past its
// top: its text starts at 604 in the content, so its first line spans 604..624.
constexpr char kPaddedBelowCss[] = R"(
#below { background-color: #202020; padding-top: 4px; height: 400px; }
)";

// Ten lines in an area appended beside the scroll row: a child of the scroll
// view that its scroll offset neither moves nor clips. The spacer is the
// scrolled content.
constexpr char kBesideXml[] = R"(<uielement id="root">
  <scrollview id="scroll">
    <uielement id="spacer"/>
    <textarea id="beside" value="0&#10;1&#10;2&#10;3&#10;4&#10;5&#10;6&#10;7&#10;8&#10;9"/>
  </scrollview>
</uielement>)";

constexpr char kBesideCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 400px; }
#scroll { width: 400px; height: 400px; }
#spacer { width: 300px; height: 1000px; flex-shrink: 0; }
#beside {
  width: 300px;
  height: 200px;
  font-family: Roboto;
  font-size: 16px;
  line-height: 20px;
  white-space: nowrap;
  padding: 0px;
  border-width: 0px;
  flex-shrink: 0;
}
)";

constexpr float kRowEpsilon = 0.5f;

// Distinct glyph quad tops: one per drawn line.
size_t DrawnLines(const std::vector<UIPrimitive>& glyphs)
{
    std::vector<float> tops;
    for (const UIPrimitive& g : glyphs)
    {
        if (g.W <= 0.0f || g.H <= 0.0f)
            continue;
        const bool known = std::any_of(tops.begin(), tops.end(),
                                       [&](float t) { return std::fabs(t - g.Y) < kRowEpsilon; });
        if (!known)
            tops.push_back(g.Y);
    }
    return tops.size();
}

// XML children of a ScrollView are appended beside its scroll row; the
// scrolled content is what AddContent holds.
void MoveIntoScrollContent(IsolatedUIFixture& fx, ScrollView& scroll, const char* id)
{
    UIElement* element = fx.Element(id);
    ASSERT_NE(element, nullptr) << id;
    std::unique_ptr<UIElement> owned = element->GetParent()->TakeChild(element);
    ASSERT_NE(owned, nullptr) << id;
    scroll.AddContent(std::move(owned));
}

// Builds the stacked specimen with both areas in the scroll content, settles
// at offset 0, then scrolls so the viewport shows 440..640 of the content: the
// upper area's last eight lines and the lower area's first two (600..640).
void BuildStackedAndScroll(IsolatedUIFixture& fx, const std::string& css)
{
    const bool built = fx.Build(1.0f, kStackedXml, css);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    auto* scroll = dynamic_cast<ScrollView*>(fx.Element("scroll"));
    ASSERT_NE(scroll, nullptr);
    MoveIntoScrollContent(fx, *scroll, "above");
    MoveIntoScrollContent(fx, *scroll, "below");
    fx.Settle();

    scroll->SetScrollY(440.0f);
    fx.Settle();
    ASSERT_NEAR(scroll->GetScrollY(), 440.0f, 0.5f) << "the content is shorter than the specimen assumes";
}
} // namespace

TEST(TextAreaScrollCulling, ATextAreaBelowAnotherDrawsItsFirstLinesOnceScrolledIntoView)
{
    IsolatedUIFixture fx;
    BuildStackedAndScroll(fx, std::string(kStackedCss) + kPaintedBelowCss);
    if (::testing::Test::IsSkipped() || ::testing::Test::HasFatalFailure())
        return;

    EXPECT_GE(DrawnLines(fx.Primitives("below", PrimitiveMode::Slug)), 2u)
        << "the area 600px down the content culled its lines as if it sat at the content's top";
    EXPECT_LT(DrawnLines(fx.Primitives("above", PrimitiveMode::Slug)), 30u)
        << "the area scrolled mostly out of view no longer culls the lines above the viewport";
}

// Below the viewport at offset 0, the lower area culls every line and, with no
// background of its own, emits nothing at all. Scrolling it into view is a
// drain-only frame, which must still find out that it now has lines to draw.
TEST(TextAreaScrollCulling, ATextAreaThatDrewNothingDrawsOnceScrolledIntoView)
{
    IsolatedUIFixture fx;
    BuildStackedAndScroll(fx, kStackedCss);
    if (::testing::Test::IsSkipped() || ::testing::Test::HasFatalFailure())
        return;

    EXPECT_GE(DrawnLines(fx.Primitives("below", PrimitiveMode::Slug)), 2u)
        << "the area that emitted nothing at its last full regen stayed blank after scrolling into view";
}

TEST(TextAreaScrollCulling, ATextAreaTheScrollViewDoesNotMoveIgnoresItsOffset)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kBesideXml, kBesideCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    auto* scroll = dynamic_cast<ScrollView*>(fx.Element("scroll"));
    ASSERT_NE(scroll, nullptr);
    MoveIntoScrollContent(fx, *scroll, "spacer");
    fx.Settle();

    scroll->SetScrollY(500.0f);
    fx.Settle();
    ASSERT_NEAR(scroll->GetScrollY(), 500.0f, 0.5f) << "the content is shorter than the specimen assumes";
    // Any visual change re-emits the area at the current offset.
    fx.Element("beside")->MarkDirty(UIElement::VisualDirty);
    fx.Settle();

    EXPECT_EQ(DrawnLines(fx.Primitives("beside", PrimitiveMode::Slug)), 10u)
        << "an area beside the scroll row culled its lines against an offset that does not move it";
}

// The band starts at the area's text, below its padding and border. At offset 621
// the viewport (621..821) shows the last 3px of the lower area's first line, which
// a band measured from the border box would start one line too late.
TEST(TextAreaScrollCulling, APaddedAreaDrawsTheLineJustInsideTheViewportTop)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kStackedXml, std::string(kStackedCss) + kPaddedBelowCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    auto* scroll = dynamic_cast<ScrollView*>(fx.Element("scroll"));
    ASSERT_NE(scroll, nullptr);
    MoveIntoScrollContent(fx, *scroll, "above");
    MoveIntoScrollContent(fx, *scroll, "below");
    fx.Settle();

    scroll->SetScrollY(621.0f);
    fx.Settle();
    ASSERT_NEAR(scroll->GetScrollY(), 621.0f, 0.5f) << "the content is shorter than the specimen assumes";

    EXPECT_EQ(DrawnLines(fx.Primitives("below", PrimitiveMode::Slug)), 3u)
        << "the band ignored the area's top padding and dropped the line just inside the viewport";
}
