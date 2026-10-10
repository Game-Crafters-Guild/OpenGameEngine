// GitHub #792 — the focus ring #749 added is correct at the element level and
// still does not reach the screen on two shipped surfaces.
//
// ShippedFocusRingTests.cpp next door asks "did the focused element emit a ring
// primitive". Both defects here answer yes to that and are invisible from it,
// because neither is a property of the focused element: one belongs to an
// ANCESTOR's clip and one to a SIBLING painted afterwards. These tests therefore
// assert over what survives compositing, which needs two things the
// element-local view does not have — the clip chain the ring is masked against,
// and the draw order it is painted in.
//
// THE ENGINE IS NOT AT FAULT IN EITHER CASE, and that was checked before these
// were written rather than assumed:
//
//   * EmitOutlineRing (UIManager_PrimitiveGen.cpp) is handed the AMBIENT clip —
//     `clipIdx` is read at the top of the element's visit, before the element
//     pushes any clip of its own — so an element cannot clip its own ring away,
//     and an ancestor's overflow does clip it. That is what browsers do.
//   * CSS 2.1 paints outlines in a final pass over the stacking context, after
//     every background; this engine paints in tree order. That difference is
//     only observable when two focusable siblings sit closer together than the
//     ring inflate, which one shipped rule does.
//
// So both fixes are CSS, and these tests are written against the SHIPPED
// stylesheets read off disk for the same reason ShippedFocusRingTests is: CSS a
// test wrote proves nothing about the CSS the editor loads.
//
// FIDELITY LIMIT, stated rather than papered over: the editor attaches
// Button.css and EditorTopToolbar.css as SUBTREE sheets
// (UIManager::AttachStyleToSubtreeFromAsset, EditorTopToolbar.cpp:196) while
// these tests concatenate every sheet into one global stylesheet. Cascade order
// is (Specificity, SheetIndex, Order), so a rule that wins on specificity wins
// either way and the flattening does not change these outcomes — but a defect
// that consisted ONLY of a sheet failing to reach a subtree would be invisible
// here, exactly as it is in ShippedFocusRingTests.

#include "IsolatedUIFixture.h"

#include "Core/Application.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIPrimitive.h"
#include "UI/UIStyle.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using GameEngine::PathUtils;
using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIClipRect;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

std::filesystem::path ShippedCssRoot()
{
    return PathUtils::GetExecutableDirectory() / "Assets" / "UI";
}

std::string ReadShippedCss(const std::filesystem::path& relative)
{
    std::ifstream in(ShippedCssRoot() / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Concatenates the named shipped sheets, then the test's own geometry. Returns
// empty and names the first missing file in `missing` — staging is part of the
// test, so an absent sheet is a failure and never a skip.
std::string BuildShippedCss(const std::vector<const char*>& sheets, const char* geometryCss,
                            std::string& missing)
{
    std::string css;
    for (const char* rel : sheets)
    {
        const std::string text = ReadShippedCss(rel);
        if (text.empty())
        {
            missing = rel;
            return {};
        }
        css += text;
        css += '\n';
    }
    css += geometryCss;
    return css;
}

// The ring is identified by the CASCADE's answer for outline-color and
// outline-width, never by where it sits — a test about the ring's geometry
// cannot use that geometry to find it.
//
// "Transparent fill and stroked on all four edges" is NOT enough here, and the
// difference is not academic: it is what ShippedFocusRingTests can afford
// because its button is opaque. Every button under test in this file is
// `.secondary`, whose background-color is transparent (Button.css), so the
// button's OWN bordered box satisfies that predicate and is emitted FIRST. A
// search on it returns the border box — which sits exactly inside every clip
// and never reaches a sibling, so both defects in this file report clean.
std::optional<UIPrimitive> FindRing(const IsolatedUIFixture& fx, const std::string& id)
{
    const GameEngine::ResolvedStyle* style = fx.Style(id);
    if (!style || style->Visual.OutlineStyle == GameEngine::BorderStyle::None)
        return std::nullopt;

    const uint32_t argb =
        style->Visual.HasOutlineColor ? style->Visual.OutlineColor : style->Visual.Color;
    const uint32_t wanted = GameEngine::UI::PackFromARGB(argb);
    // Content scale is 1.0 in every fixture here, so the resolved logical width
    // is also the physical one.
    const float width = style->Visual.OutlineWidth;

    for (const UIPrimitive& p : fx.Primitives(id))
    {
        if (GameEngine::UI::GetMode(p.ModeAndFlags) != PrimitiveMode::Rect)
            continue;
        if ((p.FillColor & 0xFF000000u) != 0u)
            continue; // the ring paints its stroke and nothing else
        if (p.BorderColor != wanted)
            continue; // outline-color, from the cascade
        const bool allEdges = std::abs(p.BorderWidths[0] - width) < 0.01f &&
                              std::abs(p.BorderWidths[1] - width) < 0.01f &&
                              std::abs(p.BorderWidths[2] - width) < 0.01f &&
                              std::abs(p.BorderWidths[3] - width) < 0.01f;
        if (!allEdges)
            continue; // outline-width, from the cascade
        return p;
    }
    return std::nullopt;
}

// A filled Rect with any visible alpha — what a sibling's background paints,
// and what therefore covers or tints a ring drawn before it. Alpha is NOT part
// of the predicate beyond "not fully transparent": the shipped view buttons are
// `.secondary`, whose resting background is transparent, and the defect belongs
// to the selected one, whose background is not.
std::optional<UIPrimitive> FindBackgroundFill(const IsolatedUIFixture& fx, const std::string& id)
{
    for (const UIPrimitive& p : fx.Primitives(id))
    {
        // Alpha is the high byte in both the engine's ARGB form and the
        // primitive's RGBA one, so the mask reads it either way.
        if (GameEngine::UI::GetMode(p.ModeAndFlags) == PrimitiveMode::Rect &&
            (p.FillColor & 0xFF000000u) != 0u)
            return p;
    }
    return std::nullopt;
}

PhysicalRect RectOf(const UIPrimitive& p) { return {p.X, p.Y, p.W, p.H}; }

bool Intersects(const PhysicalRect& a, const PhysicalRect& b)
{
    return a.X < b.X + b.W && b.X < a.X + a.W && a.Y < b.Y + b.H && b.Y < a.Y + a.H;
}

// The mask a primitive is actually composited against: its own clip slot
// intersected with every ancestor slot up the ParentIndex chain, which is the
// walk the shader performs. A primitive with no clip is masked by nothing.
PhysicalRect EffectiveClip(const IsolatedUIFixture& fx, const UIPrimitive& p)
{
    PhysicalRect out{-1e9f, -1e9f, 2e9f, 2e9f};
    uint16_t slot = GameEngine::UI::GetClipIndex(p.ModeAndFlags);
    // The chain is short and acyclic by construction; the bound only stops a
    // corrupt ParentIndex from hanging the suite.
    for (int guard = 0; guard < 64 && slot != GameEngine::UI::kNoClip; ++guard)
    {
        const UIClipRect* cr = fx.Manager().PeekClipRectForTesting(slot);
        if (!cr)
            break;
        const float l = std::max(out.X, cr->Rect[0]);
        const float t = std::max(out.Y, cr->Rect[1]);
        const float r = std::min(out.X + out.W, cr->Rect[0] + cr->Rect[2]);
        const float b = std::min(out.Y + out.H, cr->Rect[1] + cr->Rect[3]);
        out = {l, t, std::max(0.0f, r - l), std::max(0.0f, b - t)};
        if (cr->ParentIndex == GameEngine::UI::kNoClip)
            break;
        slot = static_cast<uint16_t>(cr->ParentIndex);
    }
    return out;
}

#define REQUIRE_SHIPPED_FIXTURE(fx, sheets, geometry, xml)                                         \
    do                                                                                             \
    {                                                                                              \
        std::string missing;                                                                       \
        const std::string css = BuildShippedCss((sheets), (geometry), missing);                    \
        ASSERT_TRUE(missing.empty())                                                               \
            << "shipped stylesheet not staged next to the test exe: " << missing << " (expected "  \
            << "under " << ShippedCssRoot().string() << ")";                                       \
        if (!(fx).Build(1.0f, (xml), css))                                                         \
        {                                                                                          \
            if (!(fx).DeviceAvailable())                                                           \
                GTEST_SKIP() << (fx).Diagnostic();                                                 \
            FAIL() << (fx).Diagnostic();                                                           \
        }                                                                                          \
    } while (false)

// ---------------------------------------------------------------------------
// 1. Ancestor overflow — the top toolbar
// ---------------------------------------------------------------------------

// tokens.css carries the accent colour the ring resolves through, widgets.css
// the global button rules, Button.css the ring itself, EditorTopToolbar.css the
// sections and their clip.
const std::vector<const char*> kToolbarSheets = {
    "theme/tokens.css",
    "theme/widgets.css",
    "controls/Button.css",
    "controls/EditorTopToolbar.css",
};

// Only #root is stated locally. The toolbar, its sections and the button are all
// sized by the shipped sheets, which is the point — the clip under test is the
// shipped one.
constexpr char kToolbarGeometryCss[] = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 200px; }
)";

// One button per section, mirroring EditorTopToolbar.uxml's structure and
// classes. A single button per section is enough: the clip that erases the
// horizontal bands is the section's, and it is the same for all 22.
constexpr char kToolbarXml[] = R"(<uielement id="root">
  <uielement id="toolbar" class="editor-top-toolbar">
    <uielement id="left" class="top-toolbar-left">
      <button id="leftBtn" class="small secondary icon-button"/>
    </uielement>
    <uielement id="center" class="top-toolbar-center">
      <button id="centerBtn" class="small secondary icon-button"/>
    </uielement>
    <uielement id="right" class="top-toolbar-right">
      <button id="rightBtn" class="small secondary icon-button"/>
    </uielement>
  </uielement>
</uielement>)";

} // namespace

// The specimen. Tab lands on the first focusable, which is the left section's
// button; the ring it emits must survive its ancestors' clip chain.
TEST(ShippedFocusRingComposite, ToolbarSectionClipDoesNotEraseTheRing)
{
    IsolatedUIFixture fx;
    REQUIRE_SHIPPED_FIXTURE(fx, kToolbarSheets, kToolbarGeometryCss, kToolbarXml);

    fx.FocusViaTab();
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "leftBtn") << "Tab must reach the first button";
    ASSERT_TRUE(fx.Manager().IsFocusViaKeyboard()) << "Tab focus is keyboard focus";

    const auto ring = FindRing(fx, "leftBtn");
    ASSERT_TRUE(ring.has_value()) << "the shipped :focus-visible rule must emit a ring primitive";

    // Instrument: a section that does not clip at all would make this test pass
    // for the wrong reason. The section carries overflow:hidden in the shipped
    // sheet, so the ring must be masked by something narrower than the viewport.
    const PhysicalRect clip = EffectiveClip(fx, *ring);
    const PhysicalRect section = fx.BorderBox("left");
    ASSERT_GT(clip.W, 0.0f) << "instrument: the ring must carry a resolvable clip chain";
    ASSERT_LE(clip.H, section.H + 0.5f)
        << "instrument: the section's overflow clip must be in the ring's chain, or this test "
           "cannot observe the defect";

    const PhysicalRect r = RectOf(*ring);
    EXPECT_GE(r.Y, clip.Y - 0.01f)
        << "the ring's top band is clipped away by an ancestor's overflow";
    EXPECT_LE(r.Y + r.H, clip.Y + clip.H + 0.01f)
        << "the ring's bottom band is clipped away by an ancestor's overflow";
    EXPECT_GE(r.X, clip.X - 0.01f) << "the ring's left band is clipped away";
    EXPECT_LE(r.X + r.W, clip.X + clip.W + 0.01f) << "the ring's right band is clipped away";
}

// The centre section holds play/pause/stop — the buttons the issue names. It is
// flex: 0 0 auto, so unlike left and right it can never shrink and its clip can
// never fire on the main axis; the ring is the only thing it masks.
TEST(ShippedFocusRingComposite, ToolbarCentreSectionClipDoesNotEraseTheRing)
{
    IsolatedUIFixture fx;
    REQUIRE_SHIPPED_FIXTURE(fx, kToolbarSheets, kToolbarGeometryCss, kToolbarXml);

    // Two tabs: left section's button, then the centre section's.
    fx.FocusViaTab();
    fx.FocusViaTab();
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "centerBtn") << "two tabs must reach the centre";
    ASSERT_TRUE(fx.Manager().IsFocusViaKeyboard());

    const auto ring = FindRing(fx, "centerBtn");
    ASSERT_TRUE(ring.has_value()) << "the centre section's button must ring on keyboard focus";

    const PhysicalRect clip = EffectiveClip(fx, *ring);
    const PhysicalRect r = RectOf(*ring);
    EXPECT_GE(r.Y, clip.Y - 0.01f) << "centre section clips the ring's top band";
    EXPECT_LE(r.Y + r.H, clip.Y + clip.H + 0.01f) << "centre section clips the ring's bottom band";
}

// ---------------------------------------------------------------------------
// 2. Sibling occlusion — the animation window's view switch
// ---------------------------------------------------------------------------

namespace
{

const std::vector<const char*> kViewButtonSheets = {
    "theme/tokens.css",
    "theme/widgets.css",
    "controls/Button.css",
    "panels/AnimationWindowPanel.css",
};

constexpr char kViewButtonGeometryCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 200px; }
)";

// The two view buttons carry no text on purpose. `.animationwindow-view-button`
// has min-width: 58px and no width, so an empty button is exactly 58px wide at
// every font and the horizontal geometry is decided by the shipped rule rather
// than by whichever face the atlas resolved. The defect is about the GAP between
// the two boxes, which text cannot change.
//
// `second` carries `active`, and that is the SHIPPED STATE rather than a
// contrivance: this is a view switch, so exactly one of Dope Sheet / Curves is
// selected at all times. It also matters which one — a `.secondary` button's
// resting background is transparent (Button.css), so a ring can only be covered
// by the SELECTED sibling, and only when the selected one is the LATER one in
// tree order. Focusing Curves while Dope Sheet is selected is safe for the same
// reason this case is not: DrawOrder is DFS pre-order.
constexpr char kViewButtonXml[] = R"(<uielement id="root">
  <uielement id="group" class="animationwindow-view-buttons">
    <button id="first" class="small secondary animationwindow-view-button"/>
    <button id="second" class="small secondary animationwindow-view-button active"/>
  </uielement>
</uielement>)";

} // namespace

TEST(ShippedFocusRingComposite, AdjacentViewButtonDoesNotPaintOverTheRing)
{
    IsolatedUIFixture fx;
    REQUIRE_SHIPPED_FIXTURE(fx, kViewButtonSheets, kViewButtonGeometryCss, kViewButtonXml);

    fx.FocusViaTab();
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "first") << "Tab must reach the first button";

    const auto ring = FindRing(fx, "first");
    ASSERT_TRUE(ring.has_value()) << "the focused view button must emit a ring";

    const auto neighbourFill = FindBackgroundFill(fx, "second");
    ASSERT_TRUE(neighbourFill.has_value())
        << "instrument: the selected sibling must paint a background, or it cannot cover "
           "anything and this test proves nothing";

    // Instrument: the occlusion only matters because the sibling paints LATER.
    // DrawOrder is DFS pre-order, so the sibling follows the focused element.
    const int firstPos = fx.Manager().FindDrawOrderPosForTesting(*fx.Element("first"));
    const int secondPos = fx.Manager().FindDrawOrderPosForTesting(*fx.Element("second"));
    ASSERT_GE(firstPos, 0);
    ASSERT_GE(secondPos, 0);
    ASSERT_GT(secondPos, firstPos)
        << "instrument: the sibling must paint after the ring for occlusion to be possible";

    EXPECT_FALSE(Intersects(RectOf(*ring), RectOf(*neighbourFill)))
        << "the selected sibling's background is painted over the focus ring: this engine "
           "paints outlines in tree order, so a gap smaller than the ring inflate lets the "
           "next box cover the band. Sibling fill alpha 0x" << std::hex
        << ((neighbourFill->FillColor >> 24) & 0xFFu) << std::dec
        << " (0xFF would erase the band outright; anything lower tints it)";
}
