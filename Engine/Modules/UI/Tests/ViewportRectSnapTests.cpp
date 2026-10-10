// The 1:1 invariant for elements that host a render target.
//
// Yoga quantises every used length to 1/64 of a DEVICE pixel, which is what
// lets a length Chrome reports as 20.8px survive the solve. For chrome that is
// exactly right; for an element whose background IS a render target it is not,
// because the render target extent is a whole number of pixels and a `cover`
// fit into a fractional rect resolves to a non-unit scale — every destination
// pixel then samples a blend of two source texels, with the phase drifting
// across the pane.
//
// UIElement::SetSnapRectToDevicePixels opts an element out of the 1/64 grid.
// These tests pin what that buys: an integral rect, edges that do not drift
// away from unsnapped siblings, and the same guarantee at fractional DPI.

#include "IsolatedUIFixture.h"

#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIStyle.h"

#include <gtest/gtest.h>

#include <cmath>
#include <initializer_list>
#include <string>

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

// Three flex-1 panes across a container that does not divide by three: the
// solve lands them on 1/64-px boundaries, which is the fractional rect the snap
// exists to remove. Widths are logical px; the fixture's viewport is 800x600.
constexpr const char* kQuadXml = R"(
<panel id="root">
  <panel id="row">
    <panel id="paneA"/>
    <panel id="paneB"/>
    <panel id="paneC"/>
  </panel>
</panel>
)";

constexpr const char* kQuadCss = R"(
#root { width: 800px; height: 600px; flex-direction: column; }
#row { width: 605px; height: 401px; flex-direction: row; }
#paneA, #paneB, #paneC { flex-grow: 1; flex-basis: 0; height: 401px; }
)";

// The width the row is re-solved at. 611/3 does not divide, so the panes stay
// on 1/64 boundaries and the snap has something to do.
constexpr float kResolvedRowWidthPx = 611.0f;

bool IsWholePx(float v)
{
    return std::fabs(v - std::round(v)) < 1e-3f;
}

// Re-solve the row at kResolvedRowWidthPx, after optionally opting panes into
// the snap.
//
// The snap is applied where the layout solve is COMMITTED, and that commit
// prunes subtrees whose relative layout and origin are both unchanged — so a
// flag flipped on a fully settled tree reaches nothing until something
// re-solves. That is how production works: the editor sets the flag from
// SceneViewPanel/GameViewPanel::RefreshElementPointers (mount and hot-reload
// reconciliation, both followed by a solve), and every later change of a
// viewport's rect — splitter drag, dock, window resize — is a solve too. Every
// test below measures at the SAME re-solved geometry, snapped or not, so the
// only difference between two arms is the snap itself.
void Resolve(IsolatedUIFixture& fx, std::initializer_list<const char*> snapIds)
{
    for (const char* id : snapIds)
        fx.Element(id)->SetSnapRectToDevicePixels(true);
    fx.Element("row")->Overrides().Set(GameEngine::Style::Width,
                                       GameEngine::StyleLength::Px(kResolvedRowWidthPx));
    fx.Settle();
}

TEST(ViewportRectSnap, UnsnappedPaneRectIsFractional)
{
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kQuadXml, kQuadCss))
    {
        GTEST_SKIP() << fx.Diagnostic();
    }
    Resolve(fx, {});

    // The control: without the opt-in, the panes land off the whole-pixel grid.
    // If this ever stops holding, every test below passes for free — so it is
    // asserted rather than assumed.
    const PhysicalRect a = fx.BorderBox("paneA");
    const PhysicalRect b = fx.BorderBox("paneB");
    const PhysicalRect c = fx.BorderBox("paneC");
    EXPECT_FALSE(IsWholePx(a.W) && IsWholePx(b.X) && IsWholePx(b.W) && IsWholePx(c.X))
        << "the re-solved row divided evenly; pick a width that does not";
}

TEST(ViewportRectSnap, SnappedPaneRectIsWholeDevicePixels)
{
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kQuadXml, kQuadCss))
    {
        GTEST_SKIP() << fx.Diagnostic();
    }
    Resolve(fx, {"paneA", "paneB", "paneC"});

    for (const char* id : {"paneA", "paneB", "paneC"})
    {
        const PhysicalRect r = fx.BorderBox(id);
        EXPECT_TRUE(IsWholePx(r.X)) << id << " x=" << r.X;
        EXPECT_TRUE(IsWholePx(r.Y)) << id << " y=" << r.Y;
        EXPECT_TRUE(IsWholePx(r.W)) << id << " w=" << r.W;
        EXPECT_TRUE(IsWholePx(r.H)) << id << " h=" << r.H;
    }
}

TEST(ViewportRectSnap, SnappedEdgesStayWhereTheSolvePutThem)
{
    IsolatedUIFixture unsnapped;
    IsolatedUIFixture snapped;
    if (!unsnapped.Build(1.0f, kQuadXml, kQuadCss) || !snapped.Build(1.0f, kQuadXml, kQuadCss))
    {
        GTEST_SKIP() << unsnapped.Diagnostic();
    }
    Resolve(unsnapped, {});
    Resolve(snapped, {"paneA", "paneB", "paneC"});

    for (const char* id : {"paneA", "paneB", "paneC"})
    {
        const PhysicalRect before = unsnapped.BorderBox(id);
        const PhysicalRect after = snapped.BorderBox(id);
        // Edges are rounded, never the size: each edge moves by at most half a
        // device pixel, so a snapped pane cannot drift away from the splitter it
        // shares with its neighbours.
        EXPECT_NEAR(after.X, std::round(before.X), 1e-3f) << id;
        EXPECT_NEAR(after.X + after.W, std::round(before.X + before.W), 1e-3f) << id;
        EXPECT_NEAR(after.Y + after.H, std::round(before.Y + before.H), 1e-3f) << id;
    }

    // No gap and no overlap across the row: a rounded shared edge is one number,
    // so neighbours still meet exactly.
    const PhysicalRect a = snapped.BorderBox("paneA");
    const PhysicalRect b = snapped.BorderBox("paneB");
    const PhysicalRect c = snapped.BorderBox("paneC");
    EXPECT_NEAR(a.X + a.W, b.X, 1e-3f);
    EXPECT_NEAR(b.X + b.W, c.X, 1e-3f);
}

TEST(ViewportRectSnap, SnapIsInDeviceSpaceAtFractionalDpi)
{
    IsolatedUIFixture fx;
    if (!fx.Build(1.25f, kQuadXml, kQuadCss))
    {
        GTEST_SKIP() << fx.Diagnostic();
    }
    Resolve(fx, {"paneA", "paneB", "paneC"});

    // The whole-pixel guarantee is in DEVICE px — the space the render target
    // extent and the background quad are both derived in. The logical rect is
    // free to be fractional there, and at 1.25 it usually is.
    for (const char* id : {"paneA", "paneB", "paneC"})
    {
        const PhysicalRect r = fx.BorderBox(id);
        EXPECT_TRUE(IsWholePx(r.X)) << id << " x=" << r.X;
        EXPECT_TRUE(IsWholePx(r.W)) << id << " w=" << r.W;
        EXPECT_TRUE(IsWholePx(r.H)) << id << " h=" << r.H;
    }
}

TEST(ViewportRectSnap, SnapDoesNotLeakToUnsnappedSiblings)
{
    IsolatedUIFixture unsnapped;
    IsolatedUIFixture partial;
    if (!unsnapped.Build(1.0f, kQuadXml, kQuadCss) || !partial.Build(1.0f, kQuadXml, kQuadCss))
    {
        GTEST_SKIP() << unsnapped.Diagnostic();
    }
    Resolve(unsnapped, {});
    Resolve(partial, {"paneA"});

    // Snapping is per element. paneA becomes integral; paneB, which did not ask
    // for it, keeps the rect the solve gave it.
    EXPECT_TRUE(IsWholePx(partial.BorderBox("paneA").W));
    const PhysicalRect ref = unsnapped.BorderBox("paneB");
    const PhysicalRect got = partial.BorderBox("paneB");
    EXPECT_NEAR(got.X, ref.X, 1e-3f) << "unsnapped neighbour moved";
    EXPECT_NEAR(got.W, ref.W, 1e-3f) << "unsnapped neighbour resized";
    EXPECT_FALSE(IsWholePx(got.W) && IsWholePx(got.X))
        << "unsnapped neighbour became integral — the opt-in leaked";
}

// The commit prune's boundary, pinned in BOTH directions so the documented
// limitation cannot quietly stop being true.
//
// SetSnapRectToDevicePixels marks the element layout-dirty, but the snap is
// applied where the solve is COMMITTED, and that commit prunes any subtree whose
// relative layout and origin are both unchanged. On a fully settled tree the
// viewport's ANCESTORS are clean, so the walk never reaches the element and the
// flag has no effect yet. Production never depends on it doing so: the flag is
// set from SceneViewPanel/GameViewPanel::RefreshElementPointers, which run
// immediately after the layout subtree is bound or re-bound, and every later
// rect change (splitter, dock, resize, DPI change) is itself a solve.
//
// If this test starts failing on the first assertion, the prune stopped blocking
// the flip and UIElement::SetSnapRectToDevicePixels' comment is the thing to fix.
TEST(ViewportRectSnap, FlagFlipOnASettledTreeLandsAtTheNextSolve)
{
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kQuadXml, kQuadCss))
    {
        GTEST_SKIP() << fx.Diagnostic();
    }

    // Build() settles at the stylesheet's 605px row, which does not divide by
    // three — so the panes start fractional and the snap has something to do.
    ASSERT_FALSE(IsWholePx(fx.BorderBox("paneA").W))
        << "settled row divided evenly; pick a stylesheet width that does not";

    // Flip the flag and settle with NO geometry change.
    fx.Element("paneA")->SetSnapRectToDevicePixels(true);
    fx.Settle();
    EXPECT_FALSE(IsWholePx(fx.BorderBox("paneA").W))
        << "the flag took effect without a re-solve — the commit prune no longer "
           "blocks a flip on a settled tree, so SetSnapRectToDevicePixels' "
           "documented limitation is stale";

    // The other direction: any real solve lands it. This is what every
    // production path supplies.
    Resolve(fx, {});
    EXPECT_TRUE(IsWholePx(fx.BorderBox("paneA").W))
        << "a re-solve did not apply the snap — the flag is not merely deferred, "
           "it is inert";
}

} // namespace
