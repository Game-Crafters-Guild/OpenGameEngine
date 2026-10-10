// A correct flex solve drawn at the wrong coordinates is still wrong.
//
// The parity families assert LAYOUT rects. Nothing in them proves the emitter
// turns those rects into primitives at the same place: a generator that
// re-derived a child's origin, dropped a wrap line's cross offset, or wrote the
// border ring in logical px would satisfy every layout assertion and still
// paint wrong. These four specimens -- one justify/align, one wrap, one
// bordered container, one fractional distribution -- read the EMITTED
// UIPrimitives and the emitted clip rect instead, following
// BorderPaintPrimitiveTests.cpp.
//
// GROUND TRUTH. Real Chrome, same run as FlexDefaultsParityTests:
//   "C:\Program Files\Google\Chrome\Application\chrome.exe" --headless=new
//     --disable-gpu --no-sandbox --force-device-scale-factor=1
//     --virtual-time-budget=4000 --dump-dom file:///<scratch>/defaults.html
// getBoundingClientRect, relative to each specimen root. Raw values per test.
//
// Both content scales run. Primitives are PHYSICAL px (logical * contentScale,
// UIManager_PrimitiveGen.cpp) while Yoga solves in LOGICAL px, so a coordinate
// produced in the wrong space is invisible at 1.0 and only 1.5 catches it.
// The first three specimens' Chrome numbers are integers, and 1.5x of each is
// exact in binary floating point, so those expectations stay exact at both
// scales. FractionalDistributionSurvivesToThePrimitives is the one that is
// deliberately not integral -- see its own note for why it needs both arms.
//
// The elements carry a background-color purely so they emit a Rect primitive
// at all; it does not enter the layout. The specimen roots spell out their own
// flex-item properties for the reason FlexDefaultsParityTests documents: #root
// is a flex column and its CSS-default shrink would otherwise resize them.

#include "IsolatedUIFixture.h"

#include "UI/UIElement.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

constexpr float kEps = 0.01f;

// For the fractional specimen only. A 1/64-LOGICAL-px grid -- the regression
// this file has to be able to see -- misses a device-grid third by 0.0078
// physical px at content scale 1.5, which kEps would pass. Every value it
// guards is either an exact multiple of 1/64 or 100/3 scaled, so 1e-4 is still
// four decimal orders above the float noise.
constexpr float kSubUnitEps = 1e-4f;

constexpr char kWrapperCss[] =
    "#root { display: flex; flex-direction: column; flex-wrap: nowrap;"
    " align-items: flex-start; align-content: flex-start;"
    " justify-content: flex-start; width: 800px; height: 600px; }\n";

#define SPEC_ROOT_ITEM "flex-grow: 0; flex-shrink: 0; flex-basis: auto; align-self: flex-start;"

// The element's one Rect primitive. Fails the calling test when the element did
// not emit exactly one, which is itself the finding worth reporting.
const UIPrimitive* SoleRect(const IsolatedUIFixture& fx, const char* id,
                            std::vector<UIPrimitive>& storage)
{
    storage = fx.Primitives(id, PrimitiveMode::Rect);
    if (storage.size() != 1u)
    {
        ADD_FAILURE() << id << " emitted " << storage.size() << " Rect primitives, expected 1";
        return nullptr;
    }
    return &storage[0];
}

// Asserts the emitted quad sits at `expect` (physical px, relative to the
// specimen root's OWN primitive origin) and is `w` x `h` physical px. The
// tolerance is explicit because the fractional specimen needs a tighter one
// than a divergence-sized kEps to stay discriminating.
void ExpectPrimAt(const UIPrimitive& prim, const UIPrimitive& rootPrim, float x, float y, float w,
                  float h, float eps, const char* what)
{
    EXPECT_NEAR(prim.X - rootPrim.X, x, eps) << what << " prim x";
    EXPECT_NEAR(prim.Y - rootPrim.Y, y, eps) << what << " prim y";
    EXPECT_NEAR(prim.W, w, eps) << what << " prim w";
    EXPECT_NEAR(prim.H, h, eps) << what << " prim h";
}

// The emitted quad must be the layout rect snapped to the device grid per
// edge (SnapPaintRect; BorderEdgeSnapTests pins the snap rules) -- the two are
// computed by different code and only agree when the emitter used the solve.
void ExpectPrimIsLayoutRect(const IsolatedUIFixture& fx, const UIPrimitive& prim, const char* id)
{
    const PhysicalRect bb = fx.BorderBox(id);
    const float x0 = std::round(bb.X);
    const float y0 = std::round(bb.Y);
    EXPECT_NEAR(prim.X, x0, kEps) << id << " prim vs snapped layout x";
    EXPECT_NEAR(prim.Y, y0, kEps) << id << " prim vs snapped layout y";
    EXPECT_NEAR(prim.W, std::round(bb.X + bb.W) - x0, kEps) << id << " prim vs snapped layout w";
    EXPECT_NEAR(prim.H, std::round(bb.Y + bb.H) - y0, kEps) << id << " prim vs snapped layout h";
}

bool BuildOrSkipReason(IsolatedUIFixture& fx, float cs, const std::string& xml,
                       const std::string& css, std::string& outSkip)
{
    if (fx.Build(cs, xml, kWrapperCss + css))
        return true;
    if (!fx.DeviceAvailable())
    {
        outSkip = fx.Diagnostic();
        return false;
    }
    ADD_FAILURE() << fx.Diagnostic();
    outSkip.clear();
    return false;
}

#define BUILD_OR_SKIP(fx, cs, xml, css)                                                            \
    do                                                                                             \
    {                                                                                              \
        std::string skipReason;                                                                    \
        if (!BuildOrSkipReason(fx, cs, xml, css, skipReason))                                      \
        {                                                                                          \
            if (!skipReason.empty())                                                               \
                GTEST_SKIP() << skipReason;                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (false)

} // namespace

class FlexPrimitiveSpotCheck : public ::testing::TestWithParam<float>
{
};

INSTANTIATE_TEST_SUITE_P(Scales, FlexPrimitiveSpotCheck, ::testing::Values(1.0f, 1.5f));

// --- justify-content: space-between + align-items: center -------------------
//
// Chrome: p1root 300x100, p1a (0,40,60,20), p1b (120,40,60,20),
//         p1c (240,40,60,20)
// -- 300 - 3*60 = 120 of free space becomes two 60px gaps, and the 20px items
// centre in the 100px cross axis.
//
// Both distributed offsets are asserted on the PRIMITIVE. An emitter that
// walked children in tree order and stacked them itself would put p1b at 60
// and p1c at 120 -- layout untouched, paint wrong.
TEST_P(FlexPrimitiveSpotCheck, SpaceBetweenAndCenterEmitAtTheLayoutRects)
{
    const float cs = GetParam();

    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="p1root"><uielement id="p1a"/><uielement id="p1b"/><uielement id="p1c"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#p1root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: center; align-content: flex-start; justify-content: space-between; width: 300px; height: 100px; background-color: #202020; )" SPEC_ROOT_ITEM R"( }
#p1a { width: 60px; height: 20px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; background-color: #0000ff; }
#p1b { width: 60px; height: 20px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; background-color: #0000ff; }
#p1c { width: 60px; height: 20px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; background-color: #0000ff; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, cs, kXml, kCss);

    std::vector<UIPrimitive> rootStore, aStore, bStore, cStore;
    const UIPrimitive* rootPrim = SoleRect(fx, "p1root", rootStore);
    const UIPrimitive* a = SoleRect(fx, "p1a", aStore);
    const UIPrimitive* b = SoleRect(fx, "p1b", bStore);
    const UIPrimitive* c = SoleRect(fx, "p1c", cStore);
    ASSERT_TRUE(rootPrim && a && b && c);

    EXPECT_NEAR(rootPrim->W, 300.0f * cs, kEps);
    EXPECT_NEAR(rootPrim->H, 100.0f * cs, kEps);

    ExpectPrimAt(*a, *rootPrim, 0.0f * cs, 40.0f * cs, 60.0f * cs, 20.0f * cs, kEps, "p1a");
    ExpectPrimAt(*b, *rootPrim, 120.0f * cs, 40.0f * cs, 60.0f * cs, 20.0f * cs, kEps, "p1b");
    ExpectPrimAt(*c, *rootPrim, 240.0f * cs, 40.0f * cs, 60.0f * cs, 20.0f * cs, kEps, "p1c");

    ExpectPrimIsLayoutRect(fx, *a, "p1a");
    ExpectPrimIsLayoutRect(fx, *b, "p1b");
    ExpectPrimIsLayoutRect(fx, *c, "p1c");

    // No border authored: the ring must be zero on all four edges, or the
    // shader insets a fill that Chrome does not inset.
    for (int e = 0; e < 4; ++e)
        EXPECT_NEAR(a->BorderWidths[e], 0.0f, kEps) << "p1a border edge " << e;
}

// --- flex-wrap: wrap --------------------------------------------------------
//
// Chrome: p2root 200x300, p2a (0,0,100,40), p2b (100,0,100,40),
//         p2c (0,40,100,40), p2d (100,40,100,40)
// -- two lines of two, align-content:flex-start so the second line starts at
// the first line's cross size.
//
// The second line's cross offset is the thing under test on the paint side: an
// emitter that ignored it would draw all four quads at y=0, overlapping.
TEST_P(FlexPrimitiveSpotCheck, WrappedLinesEmitAtTheirCrossOffsets)
{
    const float cs = GetParam();

    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="p2root"><uielement id="p2a"/><uielement id="p2b"/><uielement id="p2c"/><uielement id="p2d"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#p2root { display: flex; flex-direction: row; flex-wrap: wrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 200px; height: 300px; background-color: #202020; )" SPEC_ROOT_ITEM R"( }
#p2a { width: 100px; height: 40px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; background-color: #0000ff; }
#p2b { width: 100px; height: 40px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; background-color: #0000ff; }
#p2c { width: 100px; height: 40px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; background-color: #0000ff; }
#p2d { width: 100px; height: 40px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; background-color: #0000ff; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, cs, kXml, kCss);

    std::vector<UIPrimitive> rootStore, aStore, bStore, cStore, dStore;
    const UIPrimitive* rootPrim = SoleRect(fx, "p2root", rootStore);
    const UIPrimitive* a = SoleRect(fx, "p2a", aStore);
    const UIPrimitive* b = SoleRect(fx, "p2b", bStore);
    const UIPrimitive* c = SoleRect(fx, "p2c", cStore);
    const UIPrimitive* d = SoleRect(fx, "p2d", dStore);
    ASSERT_TRUE(rootPrim && a && b && c && d);

    EXPECT_NEAR(rootPrim->W, 200.0f * cs, kEps);
    EXPECT_NEAR(rootPrim->H, 300.0f * cs, kEps);

    ExpectPrimAt(*a, *rootPrim, 0.0f, 0.0f, 100.0f * cs, 40.0f * cs, kEps, "p2a");
    ExpectPrimAt(*b, *rootPrim, 100.0f * cs, 0.0f, 100.0f * cs, 40.0f * cs, kEps, "p2b");
    ExpectPrimAt(*c, *rootPrim, 0.0f, 40.0f * cs, 100.0f * cs, 40.0f * cs, kEps, "p2c");
    ExpectPrimAt(*d, *rootPrim, 100.0f * cs, 40.0f * cs, 100.0f * cs, 40.0f * cs, kEps, "p2d");

    ExpectPrimIsLayoutRect(fx, *c, "p2c");
    ExpectPrimIsLayoutRect(fx, *d, "p2d");

    // Second line strictly below the first: the assertion that survives even if
    // someone retunes the specimen's sizes.
    EXPECT_GE(c->Y, a->Y + a->H - kEps) << "wrap line 2 must not overlap line 1";
}

// --- bordered flex container ------------------------------------------------
//
// Chrome: p3root 300x120 with border:4px and padding:6px,
//         p3a (10,10,80,30), p3b (90,10,80,30)
// -- children start at border + padding = 10 in.
//
// Three separate claims, all on emitted data:
//   1. the container's quad IS the border box (not the padding or content box),
//   2. BorderWidths carries the CSS widths in PHYSICAL px,
//   3. the children's quads land at the content box the flex solve placed them
//      in, inside the ring.
//
// The clip rect is asserted too: the overflow clip region is the PADDING box,
// matching Chrome (css-overflow-3). It also catches a clip emitted in the wrong
// SPACE (logical px would be 1/1.5 of it) or against the wrong element.
TEST_P(FlexPrimitiveSpotCheck, BorderedContainerRingAndChildQuads)
{
    const float cs = GetParam();

    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="p3root"><uielement id="p3a"/><uielement id="p3b"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#p3root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 300px; height: 120px; border: 4px solid #ff0000; padding: 6px; background-color: #00ff00; overflow: hidden; )" SPEC_ROOT_ITEM R"( }
#p3a { width: 80px; height: 30px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; background-color: #0000ff; }
#p3b { width: 80px; height: 30px; flex-grow: 0; flex-shrink: 0; flex-basis: auto; background-color: #0000ff; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, cs, kXml, kCss);

    std::vector<UIPrimitive> rootStore, aStore, bStore;
    const UIPrimitive* rootPrim = SoleRect(fx, "p3root", rootStore);
    const UIPrimitive* a = SoleRect(fx, "p3a", aStore);
    const UIPrimitive* b = SoleRect(fx, "p3b", bStore);
    ASSERT_TRUE(rootPrim && a && b);

    // 1. The container's quad is the border box.
    EXPECT_NEAR(rootPrim->W, 300.0f * cs, kEps);
    EXPECT_NEAR(rootPrim->H, 120.0f * cs, kEps);
    ExpectPrimIsLayoutRect(fx, *rootPrim, "p3root");

    // 2. Ring widths, physical px, L/T/R/B.
    for (int e = 0; e < 4; ++e)
        EXPECT_NEAR(rootPrim->BorderWidths[e], 4.0f * cs, kEps) << "p3root border edge " << e;

    // 3. Children at border + padding in, and strictly inside the ring.
    ExpectPrimAt(*a, *rootPrim, 10.0f * cs, 10.0f * cs, 80.0f * cs, 30.0f * cs, kEps, "p3a");
    ExpectPrimAt(*b, *rootPrim, 90.0f * cs, 10.0f * cs, 80.0f * cs, 30.0f * cs, kEps, "p3b");
    ExpectPrimIsLayoutRect(fx, *a, "p3a");
    ExpectPrimIsLayoutRect(fx, *b, "p3b");

    EXPECT_GE(a->X, rootPrim->X + rootPrim->BorderWidths[0] - kEps);
    EXPECT_GE(a->Y, rootPrim->Y + rootPrim->BorderWidths[1] - kEps);

    // Clip: same slot the children reference, sized in physical px.
    const GameEngine::UIElement* clipOwner = fx.Element("p3root");
    ASSERT_NE(clipOwner, nullptr);
    ASSERT_NE(clipOwner->m_ClipSlotIdx, GameEngine::UI::kNoClip)
        << "overflow:hidden owner must own a clip slot";
    const GameEngine::UI::UIClipRect* clip =
        fx.Manager().PeekClipRectForTesting(clipOwner->m_ClipSlotIdx);
    ASSERT_NE(clip, nullptr);

    // The overflow clip region is the PADDING box (css-overflow-3): content is
    // cut at the inner border edge, so a child overlapping the ring cannot
    // paint over it. The 4px ring shrinks the slot by 4 on each side.
    EXPECT_NEAR(clip->Rect[0], rootPrim->X + 4.0f * cs, kEps) << "clip x (padding box)";
    EXPECT_NEAR(clip->Rect[1], rootPrim->Y + 4.0f * cs, kEps) << "clip y (padding box)";
    EXPECT_NEAR(clip->Rect[2], (300.0f - 8.0f) * cs, kEps) << "clip w (padding box, physical px)";
    EXPECT_NEAR(clip->Rect[3], (120.0f - 8.0f) * cs, kEps) << "clip h (padding box, physical px)";

    // And the children are actually clipped by it, not left unclipped.
    const uint16_t childClip = GameEngine::UI::GetClipIndex(a->ModeAndFlags);
    EXPECT_NE(childClip, GameEngine::UI::kNoClip) << "child of overflow:hidden must carry a clip";
}

// --- a FRACTIONAL solve, into the layout and the snapped quad ---------------
//
// The three specimens above distribute into integers, so they are byte-
// identical on any layout grid finer than a pixel and cannot see the grid at
// all. This one is three equal grow factors over 100px: the thirds are
// fractional, so the solve has something to lose. The LAYOUT assertions carry
// the sub-unit discrimination; the primitives are that layout snapped per
// edge to the device grid (paint snaps — SnapPaintRect), which is also where
// a coordinate produced in the wrong space still lands a whole pixel off.
//
// Chrome (dpr 1): p4root 100x60, p4a (0,0,33.3281,20),
//                 p4b (33.3281,0,33.3438,20), p4c (66.6719,0,33.3281,20).
// The geometry is FlexDefaultsParity's d21 specimen exactly -- same wrapper,
// same rule minus the background-color, which does not enter layout -- so that
// fixture's measurement is the reference here rather than a second Chrome run.
//
// THE TWO ARMS DO NOT SCALE FROM ONE ANOTHER, and that is the point. The grid
// is 1/64 of a DEVICE pixel (YogaAdapter::SetContentScale multiplies the factor
// by the content scale), so which thirds are representable depends on the
// scale:
//   cs 1.0 -- grid 1/64 logical px. 100/3 is not on it; the solve lands on
//             2133/2134/2133 units, Chrome's own uneven split.
//   cs 1.5 -- grid 1/96 logical px, and 100/3 IS on it (3200/96), so the split
//             is even and the physical widths are exactly 50 device px.
// Neither arm subsumes the other. A grid keyed to LOGICAL px gives 33.328125 at
// both scales, so it passes at 1.0 and only the 1.5 arm rejects it (49.9922
// physical). A whole-pixel grid is the mirror image: at 1.5 it still lands on
// 50/50/50 and only the 1.0 arm rejects it. Both stay.
TEST_P(FlexPrimitiveSpotCheck, FractionalDistributionSurvivesToThePrimitives)
{
    const float cs = GetParam();

    constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="p4root"><uielement id="p4a"/><uielement id="p4b"/><uielement id="p4c"/></uielement>
</uielement>)";
    constexpr char kCss[] = R"(
#p4root { display: flex; flex-direction: row; flex-wrap: nowrap; align-items: flex-start; align-content: flex-start; justify-content: flex-start; width: 100px; height: 60px; background-color: #202020; )" SPEC_ROOT_ITEM R"( }
#p4a { flex: 1; height: 20px; background-color: #0000ff; }
#p4b { flex: 1; height: 20px; background-color: #0000ff; }
#p4c { flex: 1; height: 20px; background-color: #0000ff; }
)";

    IsolatedUIFixture fx;
    BUILD_OR_SKIP(fx, cs, kXml, kCss);

    std::vector<UIPrimitive> rootStore, aStore, bStore, cStore;
    const UIPrimitive* rootPrim = SoleRect(fx, "p4root", rootStore);
    const UIPrimitive* a = SoleRect(fx, "p4a", aStore);
    const UIPrimitive* b = SoleRect(fx, "p4b", bStore);
    const UIPrimitive* c = SoleRect(fx, "p4c", cStore);
    ASSERT_TRUE(rootPrim && a && b && c);

    EXPECT_NEAR(rootPrim->W, 100.0f * cs, kEps);
    EXPECT_NEAR(rootPrim->H, 60.0f * cs, kEps);

    // The LAYOUT keeps the fractional solve — this is where the grid
    // regression this file has to see stays visible, at sub-unit epsilon.
    // (Paint snaps below, so the primitive can no longer carry it.)
    const PhysicalRect bbRoot = fx.BorderBox("p4root");
    const PhysicalRect bbA = fx.BorderBox("p4a");
    const PhysicalRect bbB = fx.BorderBox("p4b");
    const PhysicalRect bbC = fx.BorderBox("p4c");
    if (cs == 1.0f)
    {
        EXPECT_NEAR(bbA.X - bbRoot.X, 0.0f, kSubUnitEps) << "p4a layout x";
        EXPECT_NEAR(bbA.W, 33.328125f, kSubUnitEps) << "p4a layout w";
        EXPECT_NEAR(bbB.X - bbRoot.X, 33.328125f, kSubUnitEps) << "p4b layout x";
        EXPECT_NEAR(bbB.W, 33.34375f, kSubUnitEps) << "p4b layout w";
        EXPECT_NEAR(bbC.X - bbRoot.X, 66.671875f, kSubUnitEps) << "p4c layout x";
        EXPECT_NEAR(bbC.W, 33.328125f, kSubUnitEps) << "p4c layout w";
    }
    else
    {
        EXPECT_NEAR(bbA.X - bbRoot.X, 0.0f, kSubUnitEps) << "p4a layout x";
        EXPECT_NEAR(bbA.W, 50.0f, kSubUnitEps) << "p4a layout w";
        EXPECT_NEAR(bbB.X - bbRoot.X, 50.0f, kSubUnitEps) << "p4b layout x";
        EXPECT_NEAR(bbB.W, 50.0f, kSubUnitEps) << "p4b layout w";
        EXPECT_NEAR(bbC.X - bbRoot.X, 100.0f, kSubUnitEps) << "p4c layout x";
        EXPECT_NEAR(bbC.W, 50.0f, kSubUnitEps) << "p4c layout w";
    }

    // Physical px, relative to the container's own quad — the layout above,
    // snapped per edge like Chrome's own raster of this specimen: adjacent
    // children share a snapped edge, so the thirds paint as 33/34/33 at cs 1
    // and 50/50/50 at cs 1.5. Spelled out per arm because the two solves
    // genuinely differ, not just by a factor.
    if (cs == 1.0f)
    {
        ExpectPrimAt(*a, *rootPrim, 0.0f, 0.0f, 33.0f, 20.0f, kSubUnitEps, "p4a");
        ExpectPrimAt(*b, *rootPrim, 33.0f, 0.0f, 34.0f, 20.0f, kSubUnitEps, "p4b");
        ExpectPrimAt(*c, *rootPrim, 67.0f, 0.0f, 33.0f, 20.0f, kSubUnitEps, "p4c");
    }
    else
    {
        ExpectPrimAt(*a, *rootPrim, 0.0f, 0.0f, 50.0f, 30.0f, kSubUnitEps, "p4a");
        ExpectPrimAt(*b, *rootPrim, 50.0f, 0.0f, 50.0f, 30.0f, kSubUnitEps, "p4b");
        ExpectPrimAt(*c, *rootPrim, 100.0f, 0.0f, 50.0f, 30.0f, kSubUnitEps, "p4c");
    }

    // The emitted quad is the solve times the content scale -- the mapping the
    // fractional value is here to stress. BorderBox() reads the layout and
    // scales it; the primitive came from the generator. They agree only if the
    // generator used the solve rather than re-deriving anything.
    ExpectPrimIsLayoutRect(fx, *a, "p4a");
    ExpectPrimIsLayoutRect(fx, *b, "p4b");
    ExpectPrimIsLayoutRect(fx, *c, "p4c");

    // Gapless and exactly filling the container in PHYSICAL px: sub-pixel
    // distribution must not open a seam the user can see.
    EXPECT_NEAR(b->X, a->X + a->W, kEps) << "no seam between p4a and p4b";
    EXPECT_NEAR(c->X, b->X + b->W, kEps) << "no seam between p4b and p4c";
    EXPECT_NEAR(c->X + c->W - rootPrim->X, 100.0f * cs, kEps) << "children fill the container";

    // Every emitted edge on the device-pixel quantum.
    constexpr float kUnitsPerDevicePx = 64.0f;
    for (const float v : {a->W, b->W, c->W, b->X - a->X, c->X - a->X})
        EXPECT_NEAR(v * kUnitsPerDevicePx, std::round(v * kUnitsPerDevicePx), kEps)
            << "primitive edge must land on the 1/64 device px grid";
}
