// Percentage `border-radius` resolves against the border box.
//
// css-backgrounds-3 §5.1: "Percentages: Refer to corresponding dimension of the
// border box" — the horizontal radius against the width, the vertical against
// the height. A percentage therefore names an ELLIPSE, and a corner whose box is
// not square has two different radii.
//
// A corner carries both semi-axes end to end: UIStyle.h CornerRadius{X,Y},
// UIPrimitive.h `Radii`/`RadiiY` (two GPU vec4s), and roundedRectSDF taking a
// per-corner (rx, ry). The percentage resolves per axis in UsedBorderRadius, and
// the same per-axis rule derives the inner border corner in both places that
// derive it — InnerClipRadii on the CPU and radInnerX/radInnerY in ui_sdf.frag —
// so the overflow clip contour and the painted ring still meet exactly.
//
// The expectations below are grounded outside this engine. Chrome 200x60 at
// `border-radius: 50%` paints the ellipse hr=100 / vr=30: its top edge is still
// curving at x=70 (measured top of ink 1px below the box top, where the ellipse
// predicts 1.29px and a 30px circular corner predicts a flat 0). Chrome 200x200
// at `10%` paints the 20px circle. Both measured off painted pixels, with a
// `border-radius: 30px` box in the same page as the instrument control. The
// page is Engine/Modules/UI/Tests/ChromeReference/border-radius/percent-border-radius-probe.html — serve it over
// http (Chrome blocks file://) and read the ink, not getComputedStyle.
// Chrome's getComputedStyle returns "50%" / "10%" — the COMPUTED value stays a
// percentage, which is why ResolvedStyle keeps the authored number plus a
// per-corner IsPercent flag and resolves at use, exactly like padding/margin.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <vector>

using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="square"/>
  <uielement id="wide"/>
  <uielement id="dot10"/>
  <uielement id="dot8a"/>
  <uielement id="dot8b"/>
</uielement>)";

// `square` is the sharp specimen: 200x200 with a 10% radius. The spec answer is
// 20px on both axes — circular, exactly representable — and 20 is a fifth of the
// 100px shader clamp, so nothing downstream can hide a wrong number.
//
// `wide` is 200x60 at 50%: the spec answer is a 100x30 ellipse, the case a
// single scalar cannot express.
//
// `dot10`/`dot8a`/`dot8b` reproduce the three shipped `50%` rules
// (BuildPanel.css 10x10, theme/node-graph.css 8x8, theme/views.css 8x8).
constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 600px; height: 400px; }
#square { width: 200px; height: 200px; border-radius: 10%; background-color: #ff0000; }
#wide   { width: 200px; height: 60px;  border-radius: 50%; background-color: #00ff00; }
#dot10  { width: 10px;  height: 10px;  border-radius: 50%; background-color: #0000ff; }
#dot8a  { width: 8px;   height: 8px;   border-radius: 50%; background-color: #00ffff; }
#dot8b  { width: 8px;   height: 8px;   border-radius: 50%; background-color: #ff00ff; }
)";

// css-backgrounds-3 §5.1 applied to the authored CSS, confirmed against Chrome.
constexpr float kSquareSpecRadius = 20.0f;          // 10% of 200
constexpr float kWideSpecRadiusHorizontal = 100.0f; // 50% of 200
constexpr float kWideSpecRadiusVertical = 30.0f;    // 50% of 60

// Returned by value: Primitives() hands back a fresh vector, so a pointer into
// one dangles the moment the call's full-expression ends.
bool TryFirstRect(const std::vector<UIPrimitive>& prims, UIPrimitive& out)
{
    for (const UIPrimitive& p : prims)
    {
        if (GameEngine::UI::GetMode(p.ModeAndFlags) == PrimitiveMode::Rect)
        {
            out = p;
            return true;
        }
    }
    return false;
}

// What ui_sdf.frag paints for a corner's semi-axes: each clamps to ITS OWN
// half-extent (`clamp(prim.radii, 0, halfW)` / `clamp(prim.radiiY, 0, halfH)`,
// and again inside roundedRectSDF). The shipped-dot pin below compares these,
// not the raw radii, because these are the numbers that reach the screen.
float ShaderClampedRadiusX(const UIPrimitive& p, int corner)
{
    return std::clamp(p.Radii[corner], 0.0f, std::max(0.0f, p.W) * 0.5f);
}

float ShaderClampedRadiusY(const UIPrimitive& p, int corner)
{
    return std::clamp(p.RadiiY[corner], 0.0f, std::max(0.0f, p.H) * 0.5f);
}

} // namespace

// 10% of a 200px box is 20px, and that is what reaches the GPU: the emitter
// applies the content scale to the resolved radius, nothing else.
TEST(PercentBorderRadius, SquareBoxResolvesPercentAgainstTheBorderBox)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    // Instrument check: the spec number is computed from this box, so the box
    // has to be what the CSS said before the comparison means anything.
    ASSERT_FLOAT_EQ(fx.BorderBox("square").W, 200.0f);
    ASSERT_FLOAT_EQ(fx.BorderBox("square").H, 200.0f);

    UIPrimitive rect{};
    ASSERT_TRUE(TryFirstRect(fx.Primitives("square"), rect)) << "no background rect emitted";

    // Physical px at contentScale 1.0, so the same number the spec names.
    EXPECT_FLOAT_EQ(rect.Radii[0], kSquareSpecRadius);
    EXPECT_FLOAT_EQ(rect.Radii[1], kSquareSpecRadius);
    EXPECT_FLOAT_EQ(rect.Radii[2], kSquareSpecRadius);
    EXPECT_FLOAT_EQ(rect.Radii[3], kSquareSpecRadius);
}

// The percentage is resolved against the CSS-logical box and the result is then
// scaled: at contentScale 2 the same 10% of the same 200px CSS box is 20 CSS px
// = 40 physical. A resolver that took the percentage of the PHYSICAL box and
// then scaled again would report 80.
TEST(PercentBorderRadius, PercentResolvesInLogicalSpaceThenScales)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(2.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    ASSERT_FLOAT_EQ(fx.BorderBox("square").W, 400.0f) << "border box is not physical px";

    UIPrimitive rect{};
    ASSERT_TRUE(TryFirstRect(fx.Primitives("square"), rect)) << "no background rect emitted";

    EXPECT_FLOAT_EQ(rect.Radii[0], kSquareSpecRadius * 2.0f);
}

// The non-square case — the one a square box cannot distinguish. The spec asks
// for a 100x30 ellipse on every corner: the horizontal semi-axis resolves 50%
// against the width, the vertical against the height (css-backgrounds-3 §5.1).
// Chrome paints hr=100 / vr=30 here, measured off painted pixels.
//
// The two semi-axes have to differ, and by the ratio the spec names. An engine
// that still collapsed to one scalar would report 30 on both axes — the
// inscribed circle — so this is the assertion that separates a circle from an
// ellipse, and the reason the specimen is 200x60 rather than square.
TEST(PercentBorderRadius, NonSquareBoxResolvesTheSpecEllipsePerAxis)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const PhysicalRect box = fx.BorderBox("wide");
    ASSERT_FLOAT_EQ(box.W, 200.0f);
    ASSERT_FLOAT_EQ(box.H, 60.0f);

    UIPrimitive rect{};
    ASSERT_TRUE(TryFirstRect(fx.Primitives("wide"), rect)) << "no background rect emitted";

    static_assert(kWideSpecRadiusVertical < kWideSpecRadiusHorizontal,
                  "the specimen must be non-square for this test to say anything");

    for (int corner = 0; corner < 4; ++corner)
    {
        EXPECT_FLOAT_EQ(rect.Radii[corner], kWideSpecRadiusHorizontal)
            << "corner " << corner << ": 50% of the 200px width";
        EXPECT_FLOAT_EQ(rect.RadiiY[corner], kWideSpecRadiusVertical)
            << "corner " << corner << ": 50% of the 60px height";
    }

    // The inscribed circle is what the one-scalar corner used to paint, on both
    // axes. Naming it keeps this test failing if the collapse ever comes back.
    EXPECT_NE(rect.Radii[0], kWideSpecRadiusVertical)
        << "the horizontal semi-axis must not collapse to the inscribed radius";
}

// The three shipped `50%` rules are all square dots, and on a square box the two
// spec semi-axes are equal — so the ellipse IS the circle these already painted,
// and both axes must report the same half-side. This is the regression arm for
// the change: every shipped percentage radius in the engine is one of these.
TEST(PercentBorderRadius, ShippedFiftyPercentDotsKeepTheirPaintedCorner)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    struct Dot { const char* Id; float Side; };
    const Dot dots[] = {{"dot10", 10.0f}, {"dot8a", 8.0f}, {"dot8b", 8.0f}};

    for (const Dot& dot : dots)
    {
        const PhysicalRect box = fx.BorderBox(dot.Id);
        ASSERT_FLOAT_EQ(box.W, dot.Side) << dot.Id;
        ASSERT_FLOAT_EQ(box.H, dot.Side) << dot.Id;

        UIPrimitive rect{};
        ASSERT_TRUE(TryFirstRect(fx.Primitives(dot.Id), rect)) << dot.Id;

        std::printf("  %-6s %gx%g : raw Radii[0]=%.3f/%.3f  painted (shader-clamped)=%.3f/%.3f\n",
                    dot.Id, static_cast<double>(dot.Side), static_cast<double>(dot.Side),
                    rect.Radii[0], rect.RadiiY[0],
                    ShaderClampedRadiusX(rect, 0), ShaderClampedRadiusY(rect, 0));

        for (int corner = 0; corner < 4; ++corner)
        {
            EXPECT_FLOAT_EQ(ShaderClampedRadiusX(rect, corner), dot.Side * 0.5f)
                << dot.Id << " corner " << corner
                << ": the painted corner of a shipped 50% dot moved (horizontal)";
            EXPECT_FLOAT_EQ(ShaderClampedRadiusY(rect, corner), dot.Side * 0.5f)
                << dot.Id << " corner " << corner
                << ": the painted corner of a shipped 50% dot moved (vertical)";
        }
    }
}
