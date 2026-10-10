// What `outline` does once it exists: draw a ring for keyboard focus, stay out
// of the box model, and stay out of the element's own overflow clip.
//
// Every assertion here runs against the emitted primitives, not against the
// cascade alone. A property that parses and resolves but never reaches a
// primitive is exactly the failure this suite exists to catch — the reverse of
// it (a declaration silently dropped before the cascade) is what left the
// editor with no focus indicator at all.
//
// Two spaces, as always: CSS and Yoga are LOGICAL px, primitives are PHYSICAL
// px. The fixture's BorderBox() returns physical, so it compares directly
// against primitive geometry.

#include "IsolatedUIFixture.h"

#include "Input/KeyCodes.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIPrimitive.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using GameEngine::BorderStyle;
using GameEngine::kEventKeyDown;
using GameEngine::UIEvent;
using GameEngine::UIManager;
namespace Input = GameEngine::Input;
using GameEngine::UI::kNoClip;
using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

// A device-less host cannot build the manager at all; that is a skip, not a
// failure. An XML/CSS authoring error inside the test IS a failure.
#define REQUIRE_FIXTURE(fx, built)                                                                 \
    do                                                                                             \
    {                                                                                              \
        if (!(built))                                                                              \
        {                                                                                          \
            if (!(fx).DeviceAvailable())                                                           \
                GTEST_SKIP() << (fx).Diagnostic();                                                 \
            FAIL() << (fx).Diagnostic();                                                           \
        }                                                                                          \
    } while (false)

constexpr uint32_t kRingArgb = 0xFF3F8FEFu;
// UIPrimitive packs RGBA8 with R in the low byte and A in the high byte.
constexpr uint32_t kRingRgba = 0xFFEF8F3Fu;

// A button is focusable out of the box (Button.cpp calls SetFocusable(true)),
// which is what makes Tab reach it. The sibling exists so a layout test has
// something whose position a ring could push.
constexpr char kXml[] = R"(<uielement id="root">
  <button id="target"/>
  <uielement id="sibling"/>
</uielement>)";

constexpr char kCssKeyboardRing[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; padding: 40px; }
#target { width: 120px; height: 40px; background-color: #202020; border-width: 0px; }
#sibling { width: 120px; height: 40px; background-color: #303030; }
#target:focus-visible { outline: 2px solid #3F8FEF; outline-offset: 4px; }
)";

// The element's own overflow must not clip its own ring; an ancestor's still
// applies, which is what browsers do.
constexpr char kXmlClipped[] = R"(<uielement id="root">
  <uielement id="clipper">
    <button id="target"/>
  </uielement>
</uielement>)";

constexpr char kCssClipped[] = R"(
#root { display: flex; width: 400px; height: 300px; padding: 40px; }
#clipper { width: 200px; height: 100px; overflow: hidden; background-color: #101010; }
#target { width: 120px; height: 40px; overflow: hidden; background-color: #202020; border-width: 0px; }
#target:focus-visible { outline: 2px solid #3F8FEF; outline-offset: 4px; }
)";

bool IsRing(const UIPrimitive& p)
{
    // The ring is the one Rect that paints nothing but its stroke: fully
    // transparent fill, a border on all four edges, in the ring colour.
    const bool transparentFill = (p.FillColor & 0xFF000000u) == 0u;
    const bool stroked = p.BorderWidths[0] > 0.0f && p.BorderWidths[1] > 0.0f &&
                         p.BorderWidths[2] > 0.0f && p.BorderWidths[3] > 0.0f;
    return GameEngine::UI::GetMode(p.ModeAndFlags) == PrimitiveMode::Rect && transparentFill &&
           stroked && p.BorderColor == kRingRgba;
}

std::optional<UIPrimitive> FindRing(const IsolatedUIFixture& fx, const std::string& id)
{
    for (const UIPrimitive& p : fx.Primitives(id))
    {
        if (IsRing(p))
            return p;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Keyboard vs mouse focus
// ---------------------------------------------------------------------------

TEST(FocusRingTests, KeyboardFocusEmitsTheRing)
{
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXml, kCssKeyboardRing));

    ASSERT_FALSE(FindRing(fx, "target").has_value())
        << "nothing is focused yet, so no ring may exist";

    fx.FocusViaTab();
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "target") << "Tab must reach the button";
    ASSERT_TRUE(fx.Manager().IsFocusViaKeyboard()) << "Tab focus is keyboard focus";

    const auto ring = FindRing(fx, "target");
    ASSERT_TRUE(ring.has_value()) << ":focus-visible outline must emit a ring primitive";

    // Outer edge sits (offset + width) beyond the border edge; the stroke
    // fills inward from there, so its inner edge lands exactly `offset` out.
    const PhysicalRect box = fx.BorderBox("target");
    constexpr float kInflate = 4.0f + 2.0f; // outline-offset + outline-width, scale 1
    EXPECT_FLOAT_EQ(ring->X, box.X - kInflate);
    EXPECT_FLOAT_EQ(ring->Y, box.Y - kInflate);
    EXPECT_FLOAT_EQ(ring->W, box.W + 2.0f * kInflate);
    EXPECT_FLOAT_EQ(ring->H, box.H + 2.0f * kInflate);
    EXPECT_FLOAT_EQ(ring->BorderWidths[0], 2.0f);
    EXPECT_FLOAT_EQ(ring->BorderWidths[1], 2.0f);
    EXPECT_FLOAT_EQ(ring->BorderWidths[2], 2.0f);
    EXPECT_FLOAT_EQ(ring->BorderWidths[3], 2.0f);
}

TEST(FocusRingTests, MouseFocusEmitsNoRing)
{
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXml, kCssKeyboardRing));

    ASSERT_TRUE(fx.FocusViaClick("target"));
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "target") << "the click must focus the button";
    ASSERT_FALSE(fx.Manager().IsFocusViaKeyboard()) << "a click is not keyboard focus";

    EXPECT_FALSE(FindRing(fx, "target").has_value())
        << ":focus-visible must not match pointer-assigned focus";
}

// A shortcut that moves focus somewhere (the AI Assistant's Ctrl+Enter onto a
// waiting call's Allow) is keyboard navigation: the user must see where the
// next Enter lands. The source is focused by a click first, so a ring can only
// come from the move the key handler makes. Both buttons draw a ring when
// keyboard-focused, so a ring left on the wrong one is seen too.
constexpr char kXmlShortcut[] = R"(<uielement id="root">
  <button id="source"/>
  <button id="target"/>
</uielement>)";

constexpr char kCssShortcutRing[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; padding: 40px; }
#source { width: 120px; height: 40px; background-color: #303030; border-width: 0px; }
#target { width: 120px; height: 40px; background-color: #202020; border-width: 0px; }
#source:focus-visible { outline: 2px solid #3F8FEF; outline-offset: 4px; }
#target:focus-visible { outline: 2px solid #3F8FEF; outline-offset: 4px; }
)";

// Focuses `target` from source's handler for F2, as a shortcut does.
void FocusTargetOnF2(IsolatedUIFixture& fx)
{
    UIManager& ui = fx.Manager();
    fx.Element("source")->RegisterEventHandler(kEventKeyDown, [&fx, &ui](UIEvent& event)
    {
        if (event.Key == Input::kKeyCode_F2)
            ui.FocusElement(fx.Element("target"));
    });
}

TEST(FocusRingTests, FocusMovedByAKeyHandlerEmitsTheRing)
{
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXmlShortcut, kCssShortcutRing));
    UIManager& ui = fx.Manager();
    FocusTargetOnF2(fx);

    ASSERT_TRUE(fx.FocusViaClick("source"));
    ASSERT_FALSE(ui.IsFocusViaKeyboard()) << "the source is focused by the pointer";
    ui.OnKey(Input::kKeyCode_F2, 1 /*press*/, 0);
    fx.Settle();

    ASSERT_EQ(ui.GetFocusedElementId(), "target") << "the key handler must move focus";
    EXPECT_TRUE(ui.IsFocusViaKeyboard());
    EXPECT_TRUE(FindRing(fx, "target").has_value()) << "focus a key moved must draw :focus-visible";
}

// Focus code moves after a key's dispatch has ended (after a click, from work
// Update drains later) keeps the origin it had: here pointer focus, so no ring.
// A key nobody handles goes through first, so the dispatch window must close
// behind it.
TEST(FocusRingTests, FocusMovedOutsideAKeyDispatchEmitsNoRing)
{
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXmlShortcut, kCssShortcutRing));
    UIManager& ui = fx.Manager();

    ui.OnKey(Input::kKeyCode_F2, 1 /*press*/, 0);
    ASSERT_TRUE(fx.FocusViaClick("source"));
    ui.FocusElement(fx.Element("target"));
    fx.Settle();

    ASSERT_EQ(ui.GetFocusedElementId(), "target");
    EXPECT_FALSE(ui.IsFocusViaKeyboard());
    EXPECT_FALSE(FindRing(fx, "target").has_value()) << "focus moved outside a key event must not draw :focus-visible";
}

// A key handler that targets a disabled control moves nothing: focus stays on
// the clicked source, still pointer focus, with no ring on either button.
TEST(FocusRingTests, ARefusedKeyFocusMoveLeavesPointerFocusWithoutARing)
{
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXmlShortcut, kCssShortcutRing));
    UIManager& ui = fx.Manager();
    FocusTargetOnF2(fx);
    fx.Element("target")->SetEnabled(false);

    ASSERT_TRUE(fx.FocusViaClick("source"));
    ui.OnKey(Input::kKeyCode_F2, 1 /*press*/, 0);
    fx.Settle();

    EXPECT_EQ(ui.GetFocusedElementId(), "source") << "a disabled control takes no focus";
    EXPECT_FALSE(ui.IsFocusViaKeyboard());
    EXPECT_FALSE(FindRing(fx, "source").has_value());
    EXPECT_FALSE(FindRing(fx, "target").has_value());
}

// The ring is a state, not a one-way door: focusing something else takes it
// away again. Without this, a stale ring would be indistinguishable from a
// correct one in the test above.
TEST(FocusRingTests, RingDisappearsWhenFocusLeaves)
{
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXml, kCssKeyboardRing));

    fx.FocusViaTab();
    ASSERT_TRUE(FindRing(fx, "target").has_value());

    fx.Manager().SetFocusById("");
    fx.Settle();

    EXPECT_FALSE(FindRing(fx, "target").has_value()) << "blur must retire the ring";
}

// ---------------------------------------------------------------------------
// Outline is not in the box model
// ---------------------------------------------------------------------------

TEST(FocusRingTests, RingDoesNotMoveOrResizeAnything)
{
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXml, kCssKeyboardRing));

    const PhysicalRect targetBefore = fx.BorderBox("target");
    const PhysicalRect siblingBefore = fx.BorderBox("sibling");

    fx.FocusViaTab();
    ASSERT_TRUE(FindRing(fx, "target").has_value()) << "the ring must actually be up";

    const PhysicalRect targetAfter = fx.BorderBox("target");
    const PhysicalRect siblingAfter = fx.BorderBox("sibling");

    EXPECT_FLOAT_EQ(targetAfter.X, targetBefore.X);
    EXPECT_FLOAT_EQ(targetAfter.Y, targetBefore.Y);
    EXPECT_FLOAT_EQ(targetAfter.W, targetBefore.W);
    EXPECT_FLOAT_EQ(targetAfter.H, targetBefore.H);

    // The sibling is the one that would move if the ring had been folded into
    // the border box or into any Yoga edge.
    EXPECT_FLOAT_EQ(siblingAfter.X, siblingBefore.X);
    EXPECT_FLOAT_EQ(siblingAfter.Y, siblingBefore.Y);
    EXPECT_FLOAT_EQ(siblingAfter.W, siblingBefore.W);
    EXPECT_FLOAT_EQ(siblingAfter.H, siblingBefore.H);
}

TEST(FocusRingTests, OutlinePropertiesAreClassifiedPaintOnly)
{
    using GameEngine::GetStylePropertyImpact;
    using GameEngine::StylePropertyId;
    for (StylePropertyId id : {StylePropertyId::OutlineWidth, StylePropertyId::OutlineStyle,
                               StylePropertyId::OutlineColor, StylePropertyId::OutlineOffset})
    {
        const auto impact = GetStylePropertyImpact(id);
        EXPECT_FALSE(impact.Layout) << "outline must never escalate to a Yoga solve";
        EXPECT_TRUE(impact.Paint);
    }
}

// The four longhands above are not the form the editor ships. Every shipped
// ring is the `outline` SHORTHAND holding a var(), which the stylesheet parser
// defers by name -- so the classification the analysis actually performs is the
// one below, and it is the only one that decides whether a focus change is
// allowed to ask for a Yoga solve.
TEST(FocusRingTests, TheOutlineShorthandIsClassifiedPaintOnly)
{
    const auto impact = GameEngine::UIParsing::CSSParser::PropertyImpactForName("outline");
    EXPECT_FALSE(impact.Layout)
        << "the `outline` shorthand expands only to outline-width/style/color, none of which "
           "reach Yoga";
    EXPECT_TRUE(impact.Paint) << "the ring still has to be painted";
}

// Asking the classifier directly, as the two tests above do, skips the code that
// consumes it. RefreshDynamicStyleAnalysis is where a deferred declaration's
// name is turned into the per-pseudo aggregate the engine acts on, and it is
// where an `outline` reported as "no information" got escalated to
// layout-affecting. The two tests below go through a real UIManager and read
// that aggregate back.
//
// The stylesheet must be written here rather than taken from the editor's. The
// aggregate is per-STYLESHEET: the shipped :focus rules also set border and
// border-width, which genuinely are {layout, paint}, so a shipped sheet reports
// a layout-affecting :focus-visible however `outline` is classified.
//
// Only a var()-holding declaration takes this path at all -- the parser defers
// by value, not by name (CSSStylesheetParser.cpp, ContainsVarCall). Across the
// 55 shipped editor sheets that is 874 declarations (counted 2026-08-05), of
// which the ring accounts for two: `outline:` is written 47 times and exactly
// two of those hold a var() (controls/Button.css and theme/widgets.css, both
// `outline: 2px solid var(--ui_color_accent_blue)`). An `outline` without a
// var() is parsed into typed longhands and never classified by name.

// Every DeferredDecl name the parser produced, in rule order. The sheets below
// are written to contain exactly one, so this also proves the declaration
// reached the analysis under the name the test names -- a parser that expanded
// the shorthand into longhands instead would classify something else, and the
// unknown-name test in particular would then pass for the wrong reason.
std::vector<std::string> DeferredNamesIn(const std::string& css)
{
    std::vector<std::string> names;
    GameEngine::Stylesheet sheet{};
    if (!GameEngine::UIParsing::CSSParser::ParseStylesFromString(css, sheet))
        return names;
    for (const GameEngine::CSSRule& rule : sheet.Rules)
    {
        for (const GameEngine::StyleProperty& prop : rule.Properties)
        {
            if (prop.PropertyId == GameEngine::StylePropertyId::DeferredDecl)
                names.push_back(std::get<GameEngine::DeferredDeclValue>(prop.Value).Name);
        }
    }
    return names;
}

// Geometry, one custom property, and one :focus-visible declaration supplied by
// the caller. Nothing else in the sheet holds a var(), and no other rule carries
// a focus pseudo, so the FocusVisible aggregate is attributable to `declaration`
// alone.
std::string FocusVisibleProbeCss(const char* declaration)
{
    return std::string(R"(
:root { --probe_accent: #3b82f6; }
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#target { width: 120px; height: 40px; }
#sibling { width: 120px; height: 40px; }
button:focus-visible { )") +
           declaration + " }\n";
}

// The shipped ring's exact form, through the consumer. `outline` owns no
// StylePropertyId of its own, so the classification has to come from the
// longhands it expands to; reading the aggregate back is what separates that
// from the invalidation gates downstream, which reject the escalated solve
// request anyway and so make every solve-counting probe blind to it.
TEST(FocusRingTests, TheAnalysisClassifiesAFocusVisibleOutlineRulePaintOnly)
{
    const std::string css = FocusVisibleProbeCss("outline: 2px solid var(--probe_accent);");
    const std::vector<std::string> deferred = DeferredNamesIn(css);
    ASSERT_EQ(deferred.size(), 1u)
        << "the probe sheet must defer exactly one declaration; the analysis reads names, so a "
           "second one would blur what this measures";
    ASSERT_EQ(deferred[0], "outline")
        << "the shorthand must reach the analysis unexpanded -- that is the whole defect";

    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXml, css));
    EXPECT_FALSE(fx.Manager().FocusVisibleAffectsLayoutForTesting())
        << "a :focus-visible rule whose only declaration is `outline` cannot move a box, so the "
           "modality flip it drives must not be allowed to request a Yoga solve";
    EXPECT_TRUE(fx.Manager().FocusVisibleAffectsPaintForTesting())
        << "the ring still has to be painted -- classifying it as affecting nothing would drop it";
}

// The other half of the same decision, and the one that must NOT move. A name
// the property table does not know at all still reports {false,false}, and the
// consumer has to keep reading that as "no information" and escalating to
// layout+paint. A deliberately unregistered probe keeps this independent of
// newly supported CSS features. Trading the escalation away would swap a
// redundant solve for a missed one.
TEST(FocusRingTests, TheAnalysisStaysConservativeForAnUnclassifiableFocusVisibleProperty)
{
    const auto impact = GameEngine::UIParsing::CSSParser::PropertyImpactForName("test-unknown-focus-property");
    ASSERT_FALSE(impact.Layout) << "this test probes the unknown-name escalation and needs a name "
                                   "absent from the property table; `test-unknown-focus-property` got registered -- "
                                   "move the test to a name that is still unregistered";
    ASSERT_FALSE(impact.Paint) << "see above";

    const std::string css = FocusVisibleProbeCss("test-unknown-focus-property: 0 1px var(--probe_accent);");
    const std::vector<std::string> deferred = DeferredNamesIn(css);
    ASSERT_EQ(deferred.size(), 1u);
    ASSERT_EQ(deferred[0], "test-unknown-focus-property");

    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXml, css));
    EXPECT_TRUE(fx.Manager().FocusVisibleAffectsLayoutForTesting())
        << "an unclassifiable property must stay conservative";
    EXPECT_TRUE(fx.Manager().FocusVisibleAffectsPaintForTesting())
        << "an unclassifiable property must stay conservative";
}

// ---------------------------------------------------------------------------
// Clipping
// ---------------------------------------------------------------------------

// The element sets overflow:hidden on ITSELF. Its ring is drawn outside its
// border box, so an own-overflow clip would erase the whole thing.
TEST(FocusRingTests, OwnOverflowHiddenDoesNotClipTheRing)
{
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXmlClipped, kCssClipped));

    fx.FocusViaTab();
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "target");

    const auto ring = FindRing(fx, "target");
    ASSERT_TRUE(ring.has_value());

    // Whatever clip the ring wears, none of the rects in its chain may be the
    // element's own box — that is the clip overflow:hidden installs, and it
    // would cut the ring back to the border edge.
    const PhysicalRect box = fx.BorderBox("target");
    uint16_t idx = GameEngine::UI::GetClipIndex(ring->ModeAndFlags);
    int guard = 0;
    while (idx != kNoClip && guard++ < 16)
    {
        const GameEngine::UI::UIClipRect* cr = fx.Manager().PeekClipRectForTesting(idx);
        ASSERT_NE(cr, nullptr);
        const bool isOwnBox = cr->Rect[0] == box.X && cr->Rect[1] == box.Y &&
                              cr->Rect[2] == box.W && cr->Rect[3] == box.H;
        EXPECT_FALSE(isOwnBox) << "the element's own overflow clip must not apply to its outline";
        idx = static_cast<uint16_t>(cr->ParentIndex);
    }
}

// An ancestor's overflow:hidden is a different matter — browsers clip a
// descendant's outline against it, so the ring must arrive wearing that clip
// rather than being dropped, unclipped, or culled away.
TEST(FocusRingTests, RingSurvivesAnAncestorOverflowClip)
{
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXmlClipped, kCssClipped));

    fx.FocusViaTab();
    const auto ring = FindRing(fx, "target");
    ASSERT_TRUE(ring.has_value()) << "an ancestor clip must not suppress the ring";

    const uint16_t idx = GameEngine::UI::GetClipIndex(ring->ModeAndFlags);
    ASSERT_NE(idx, kNoClip) << "the ring inherits the ancestor clip, like the rest of the element";

    const GameEngine::UI::UIClipRect* cr = fx.Manager().PeekClipRectForTesting(idx);
    ASSERT_NE(cr, nullptr);
    const PhysicalRect clipper = fx.BorderBox("clipper");
    EXPECT_FLOAT_EQ(cr->Rect[0], clipper.X);
    EXPECT_FLOAT_EQ(cr->Rect[1], clipper.Y);
    EXPECT_FLOAT_EQ(cr->Rect[2], clipper.W);
    EXPECT_FLOAT_EQ(cr->Rect[3], clipper.H);

    // The ring geometry itself still reaches outside the focused element, so
    // what the ancestor clip trims is a real ring and not an empty one.
    const PhysicalRect box = fx.BorderBox("target");
    EXPECT_LT(ring->X, box.X);
    EXPECT_GT(ring->W, box.W);
}

// ---------------------------------------------------------------------------
// The property surface
// ---------------------------------------------------------------------------

TEST(FocusRingTests, LineWidthKeywordsResolveLikeBlink)
{
    constexpr char kXmlWidths[] = R"(<uielement id="root">
  <uielement id="thin"/><uielement id="medium"/><uielement id="thick"/><uielement id="initial"/>
</uielement>)";
    constexpr char kCssWidths[] = R"(
#root { width: 400px; height: 300px; }
#thin { outline-width: thin; }
#medium { outline-width: medium; }
#thick { outline-width: thick; }
)";
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXmlWidths, kCssWidths));

    ASSERT_NE(fx.Style("thin"), nullptr);
    EXPECT_FLOAT_EQ(fx.Style("thin")->Visual.OutlineWidth, 1.0f);
    EXPECT_FLOAT_EQ(fx.Style("medium")->Visual.OutlineWidth, 3.0f);
    EXPECT_FLOAT_EQ(fx.Style("thick")->Visual.OutlineWidth, 5.0f);
    // CSS initial for outline-width is `medium`, whether or not anyone says so.
    EXPECT_FLOAT_EQ(fx.Style("initial")->Visual.OutlineWidth, 3.0f);
    // ...and initial outline-style is `none`, which is why an undeclared
    // outline-width paints nothing.
    EXPECT_EQ(fx.Style("initial")->Visual.OutlineStyle, BorderStyle::None);
}

// `outline: none` is the opt-out every editor stylesheet already uses. It has
// to beat a width the shorthand does not mention, or the opt-out leaks a ring.
TEST(FocusRingTests, OutlineNoneSuppressesAnEarlierRing)
{
    constexpr char kCssOptOut[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; padding: 40px; }
#target { width: 120px; height: 40px; background-color: #202020; border-width: 0px; }
#sibling { width: 120px; height: 40px; }
button:focus-visible { outline: 2px solid #3F8FEF; outline-offset: 4px; }
#target:focus-visible { outline: none; }
)";
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXml, kCssOptOut));

    fx.FocusViaTab();
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "target");
    ASSERT_TRUE(fx.Manager().IsFocusViaKeyboard());

    ASSERT_NE(fx.Style("target"), nullptr);
    EXPECT_EQ(fx.Style("target")->Visual.OutlineStyle, BorderStyle::None);
    EXPECT_FALSE(FindRing(fx, "target").has_value()) << "`outline: none` must suppress the ring";
}

// An outline with no colour of its own is currentColor, so it tracks `color`.
TEST(FocusRingTests, UndeclaredOutlineColorFollowsCurrentColor)
{
    constexpr char kCssCurrent[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; padding: 40px; }
#target { width: 120px; height: 40px; color: #3F8FEF; border-width: 0px; }
#sibling { width: 120px; height: 40px; }
#target:focus-visible { outline-style: solid; outline-width: 2px; }
)";
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXml, kCssCurrent));

    fx.FocusViaTab();
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "target");

    ASSERT_NE(fx.Style("target"), nullptr);
    EXPECT_FALSE(fx.Style("target")->Visual.HasOutlineColor);
    EXPECT_EQ(fx.Style("target")->Visual.Color, kRingArgb);

    const auto ring = FindRing(fx, "target");
    ASSERT_TRUE(ring.has_value()) << "the ring takes its colour from `color`";
}

// Widths and offsets are authored in logical px and emitted in physical px.
// At scale 1 a bug that forgets the conversion is invisible.
TEST(FocusRingTests, RingGeometryScalesWithContentScale)
{
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(2.0f, kXml, kCssKeyboardRing));

    fx.FocusViaTab();
    const auto ring = FindRing(fx, "target");
    ASSERT_TRUE(ring.has_value());

    const PhysicalRect box = fx.BorderBox("target");
    constexpr float kInflatePhysical = (4.0f + 2.0f) * 2.0f;
    EXPECT_FLOAT_EQ(ring->X, box.X - kInflatePhysical);
    EXPECT_FLOAT_EQ(ring->Y, box.Y - kInflatePhysical);
    EXPECT_FLOAT_EQ(ring->W, box.W + 2.0f * kInflatePhysical);
    EXPECT_FLOAT_EQ(ring->H, box.H + 2.0f * kInflatePhysical);
    EXPECT_FLOAT_EQ(ring->BorderWidths[0], 4.0f);
}

// A rounded element gets a concentric ring: radii grow by the same amount the
// box did. A square one keeps square corners — CSS does not invent a radius.
TEST(FocusRingTests, RingRadiiTrackTheBorderRadius)
{
    constexpr char kCssRadii[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; padding: 40px; }
#target { width: 120px; height: 40px; border-radius: 6px; background-color: #202020; border-width: 0px; }
#sibling { width: 120px; height: 40px; }
#target:focus-visible { outline: 2px solid #3F8FEF; outline-offset: 4px; }
)";
    IsolatedUIFixture fx;
    REQUIRE_FIXTURE(fx, fx.Build(1.0f, kXml, kCssRadii));

    fx.FocusViaTab();
    const auto ring = FindRing(fx, "target");
    ASSERT_TRUE(ring.has_value());
    for (float r : ring->Radii)
        EXPECT_FLOAT_EQ(r, 6.0f + 4.0f + 2.0f);

    IsolatedUIFixture square;
    REQUIRE_FIXTURE(square, square.Build(1.0f, kXml, kCssKeyboardRing));
    square.FocusViaTab();
    const auto squareRing = FindRing(square, "target");
    ASSERT_TRUE(squareRing.has_value());
    for (float r : squareRing->Radii)
        EXPECT_FLOAT_EQ(r, 0.0f);
}

} // namespace
