// The focus ring as the EDITOR actually gets it — parsed from the shipped
// stylesheets on disk, not from CSS a test wrote.
//
// FocusRingTests.cpp proves what `outline` does when a rule reaches it. It
// cannot prove which rules the editor ships, because every one of its cases
// supplies its own CSS. That blind spot is not hypothetical: the :focus ->
// :focus-visible migration landed in controls/Button.css while an identical
// `button:focus { outline: ... }` block stayed behind in theme/widgets.css.
// Button.css is a subtree sheet the Button control attaches; widgets.css is
// global. On a click :focus-visible does not match, so Button.css contributed
// nothing and the widgets.css rule applied unopposed — the ring came back for
// mouse focus and the migration was, for generic buttons, a no-op. Every
// inline-CSS test still passed.
//
// So these read the real files. The keyboard case is here as the control: an
// assertion that no ring appears on click is worthless on its own, because it
// also passes when the ring is broken everywhere.

#include "IsolatedUIFixture.h"

#include "UI/Parsers/CSSParser.h"
#include "UI/ResolvedStyle.h"
#include "UI/Selectors/SelectorTypes.h"
#include "UI/UIPrimitive.h"
#include "UI/UIStyle.h"

#include "Core/Application.h"

#include <gtest/gtest.h>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

using GameEngine::BorderStyle;
using GameEngine::CSSRule;
using GameEngine::PathUtils;
using GameEngine::PseudoClass;
using GameEngine::StylePropertyId;
using GameEngine::Stylesheet;
using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UIParsing::CSSParser;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;

namespace
{

// The stylesheets the build stages next to the test exe, at the same relative
// path the editor reads them from under its own exe. Anchored to the
// executable directory, never back into the source tree.
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

// tokens.css defines --ui_color_accent_blue on :root, which the ring's colour
// resolves through; widgets.css carries the global button rules; Button.css is
// the subtree sheet the Button control attaches, and comes last for the same
// reason it wins on sheet index in the editor.
constexpr const char* kShippedSheets[] = {
    "theme/tokens.css",
    "theme/widgets.css",
    "controls/Button.css",
};

// Only the root and the button's box are stated locally. Nothing here mentions
// outline or focus, so the shipped sheets remain the sole authority on the ring.
constexpr char kGeometryCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; padding: 40px; }
#target { width: 120px; height: 40px; }
#sibling { width: 120px; height: 40px; }
)";

constexpr char kXml[] = R"(<uielement id="root">
  <button id="target"/>
  <uielement id="sibling"/>
</uielement>)";

// Returns the concatenated shipped CSS, or an empty string when a file is
// missing — staging is part of the test, so absence is a failure, not a skip.
std::string BuildShippedCss(std::string& missing)
{
    std::string css;
    for (const char* rel : kShippedSheets)
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
    css += kGeometryCss;
    return css;
}

// The ring is the one Rect painting nothing but its stroke: fully transparent
// fill, stroked on all four edges. The button's own box is excluded by the fill
// test — widgets.css gives it an opaque background-color. Colour is
// deliberately not part of the predicate; the ring's colour comes from a token
// and retinting it is not this suite's business.
bool IsRing(const UIPrimitive& p)
{
    const bool transparentFill = (p.FillColor & 0xFF000000u) == 0u;
    const bool stroked = p.BorderWidths[0] > 0.0f && p.BorderWidths[1] > 0.0f &&
                         p.BorderWidths[2] > 0.0f && p.BorderWidths[3] > 0.0f;
    return GameEngine::UI::GetMode(p.ModeAndFlags) == PrimitiveMode::Rect && transparentFill &&
           stroked;
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

#define REQUIRE_SHIPPED_FIXTURE(fx, css)                                                           \
    do                                                                                             \
    {                                                                                              \
        std::string missing;                                                                       \
        (css) = BuildShippedCss(missing);                                                          \
        ASSERT_TRUE(missing.empty())                                                               \
            << "shipped stylesheet not staged next to the test exe: " << missing << " (expected "  \
            << "under " << ShippedCssRoot().string() << ")";                                       \
        if (!(fx).Build(1.0f, kXml, (css)))                                                        \
        {                                                                                          \
            if (!(fx).DeviceAvailable())                                                           \
                GTEST_SKIP() << (fx).Diagnostic();                                                 \
            FAIL() << (fx).Diagnostic();                                                           \
        }                                                                                          \
    } while (false)

} // namespace

// ---------------------------------------------------------------------------
// Behaviour, against the shipped sheets
// ---------------------------------------------------------------------------

// The control. Without it, the mouse test below passes just as happily when the
// shipped CSS has no ring at all, which is the other half of issue #749.
TEST(ShippedFocusRingTests, KeyboardFocusRingsWithTheShippedTheme)
{
    IsolatedUIFixture fx;
    std::string css;
    REQUIRE_SHIPPED_FIXTURE(fx, css);

    fx.FocusViaTab();
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "target") << "Tab must reach the button";
    ASSERT_TRUE(fx.Manager().IsFocusViaKeyboard()) << "Tab focus is keyboard focus";

    ASSERT_NE(fx.Style("target"), nullptr);
    EXPECT_EQ(fx.Style("target")->Visual.OutlineStyle, BorderStyle::Solid)
        << "the shipped theme must still ring for keyboard focus";

    const auto ring = FindRing(fx, "target");
    ASSERT_TRUE(ring.has_value()) << "the shipped :focus-visible rule must emit a ring primitive";

    // outline-offset 2px + outline-width 2px, at content scale 1.
    const PhysicalRect box = fx.BorderBox("target");
    constexpr float kInflate = 2.0f + 2.0f;
    EXPECT_FLOAT_EQ(ring->X, box.X - kInflate);
    EXPECT_FLOAT_EQ(ring->Y, box.Y - kInflate);
    EXPECT_FLOAT_EQ(ring->W, box.W + 2.0f * kInflate);
    EXPECT_FLOAT_EQ(ring->H, box.H + 2.0f * kInflate);
}

// The regression. This fails whenever any shipped sheet rings a plain button on
// a bare :focus — which is exactly what widgets.css did after the Button.css
// migration.
TEST(ShippedFocusRingTests, MouseFocusDoesNotRingWithTheShippedTheme)
{
    IsolatedUIFixture fx;
    std::string css;
    REQUIRE_SHIPPED_FIXTURE(fx, css);

    ASSERT_TRUE(fx.FocusViaClick("target"));
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "target") << "the click must focus the button";
    ASSERT_FALSE(fx.Manager().IsFocusViaKeyboard()) << "a click is not keyboard focus";

    ASSERT_NE(fx.Style("target"), nullptr);
    EXPECT_EQ(fx.Style("target")->Visual.OutlineStyle, BorderStyle::None)
        << "no shipped rule may give a mouse-focused button an outline";

    EXPECT_FALSE(FindRing(fx, "target").has_value())
        << "clicking a button must not draw a focus ring";
}

// ---------------------------------------------------------------------------
// Cost: what a focus change makes the engine re-solve
// ---------------------------------------------------------------------------
//
// The ring is drawn outside the border box and never reaches Yoga, so focusing
// something must repaint and nothing more. The instrument is UIManager's own
// per-frame update profile: YogaMs is accumulated by a ScopedSectionTimer that
// wraps every YGNodeCalculateLayout call site and stays exactly 0.0 on a frame
// that solved nothing. Each case below proves the instrument discriminates on
// this very fixture before it reads anything from it -- a quiescent tail that
// still solves would make every number here noise.

namespace
{

struct SolveTally
{
    int Frames = 0;
    int SolveFrames = 0;
};

size_t ProfiledFrameCount(const IsolatedUIFixture& fx)
{
    return fx.Manager().GetUpdateProfilingHistory().size();
}

SolveTally TallySolvesSince(const IsolatedUIFixture& fx, size_t firstFrame)
{
    SolveTally tally{};
    const auto& history = fx.Manager().GetUpdateProfilingHistory();
    for (size_t i = firstFrame; i < history.size(); ++i)
    {
        ++tally.Frames;
        if (history[i].YogaMs > 0.0)
            ++tally.SolveFrames;
    }
    return tally;
}

// Turns profiling on, then burns a settle cycle and requires the next one to be
// solve-free. Returns the history index the caller's action starts at.
size_t BeginQuiescentSolveMeasurement(IsolatedUIFixture& fx, SolveTally& outQuiet)
{
    fx.Manager().SetUpdateProfilingEnabled(true);
    fx.Settle();
    const size_t quietStart = ProfiledFrameCount(fx);
    fx.Settle();
    outQuiet = TallySolvesSince(fx, quietStart);
    return ProfiledFrameCount(fx);
}

#define REQUIRE_QUIESCENT(fx, actionStart)                                                         \
    SolveTally quiet{};                                                                            \
    const size_t actionStart = BeginQuiescentSolveMeasurement((fx), quiet);                        \
    ASSERT_GT(quiet.Frames, 0) << "profiling published no frames; the tally reads nothing";        \
    ASSERT_EQ(quiet.SolveFrames, 0)                                                                \
        << "the fixture never goes quiet (" << quiet.SolveFrames << "/" << quiet.Frames            \
        << " idle frames solved), so any solve counted below is unattributable"

// The geometry of the shipped cases, with a focus rule this file writes itself
// so the two control arms differ from each other in exactly one declaration.
constexpr char kProbeGeometryCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; padding: 40px; }
#target { width: 120px; height: 40px; }
#sibling { width: 120px; height: 40px; }
:root { --probe_accent: #3b82f6; }
)";

} // namespace

// Instrument check, positive arm: a focus rule that really does move the box
// must show up as a solve. Without this, "no solves" below is indistinguishable
// from an instrument that can only ever say zero.
TEST(ShippedFocusRingTests, SolveTallySeesALayoutAffectingFocusRule)
{
    IsolatedUIFixture fx;
    const std::string css = std::string(kProbeGeometryCss) +
                            "button:focus-visible { padding-left: var(--probe_pad); }\n"
                            ":root { --probe_pad: 12px; }\n";
    if (!fx.Build(1.0f, kXml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    REQUIRE_QUIESCENT(fx, actionStart);
    fx.FocusViaTab();
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "target");

    const SolveTally focus = TallySolvesSince(fx, actionStart);
    EXPECT_GT(focus.SolveFrames, 0)
        << "a var()-valued padding change on :focus-visible must re-solve layout";
}

// Instrument check, negative arm: same shape, same var() indirection, a
// paint-only property. This is what pins the defect on the shorthand rather
// than on deferred declarations in general.
TEST(ShippedFocusRingTests, SolveTallySeesNoSolveForAPaintOnlyFocusRule)
{
    IsolatedUIFixture fx;
    const std::string css = std::string(kProbeGeometryCss) +
                            "button:focus-visible { background-color: var(--probe_accent); }\n";
    if (!fx.Build(1.0f, kXml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    REQUIRE_QUIESCENT(fx, actionStart);
    fx.FocusViaTab();
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "target");

    const SolveTally focus = TallySolvesSince(fx, actionStart);
    EXPECT_EQ(focus.SolveFrames, 0)
        << "a var()-valued background-color change on :focus-visible is paint-only";
}

// The shipped ring is `outline: 2px solid var(--ui_color_accent_blue)`, a
// SHORTHAND held as a deferred declaration and classified by name. The
// classification decides only whether the focus mark is allowed to REQUEST a
// solve; two gates downstream (the pre-solve dirty probe, which only runs when
// virtualization or a scroll callback did, and the layout-signature comparison
// in BuildYogaRecursive) then decide whether one happens. This pins the end of
// that chain: whatever the request, focusing a button must not move any box.
//
// Which is why this test cannot stand in for the classification itself, and why
// FocusRingTests reads the analysis aggregate directly instead. Three sites
// consume the :focus-visible aggregate: UIManager_HoverAndEvents.cpp:1500 and
// UIManager.cpp:475 both union it with :focus, and against any sheet carrying a
// layout-affecting :focus rule -- as the shipped ones do, via border and
// border-width -- that union reports AffectsLayout however `outline` is
// classified. Only the modality-flip branch at
// UIManager_HoverAndEvents.cpp:1509-1512 reads :focus-visible alone.
TEST(ShippedFocusRingTests, KeyboardFocusWithTheShippedThemeDoesNotResolveLayout)
{
    IsolatedUIFixture fx;
    std::string css;
    REQUIRE_SHIPPED_FIXTURE(fx, css);

    REQUIRE_QUIESCENT(fx, actionStart);
    fx.FocusViaTab();
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "target");
    ASSERT_TRUE(fx.Manager().IsFocusViaKeyboard());
    ASSERT_TRUE(FindRing(fx, "target").has_value())
        << "the ring must still be drawn -- a cheap focus change that lost the ring is not a fix";

    const SolveTally focus = TallySolvesSince(fx, actionStart);
    EXPECT_EQ(focus.SolveFrames, 0)
        << "keyboard focus re-solved layout on " << focus.SolveFrames << " of " << focus.Frames
        << " frames; the only thing :focus-visible changes here is the outline";
}

// ---------------------------------------------------------------------------
// The whole shipped tree, not just the button path
// ---------------------------------------------------------------------------

namespace
{

bool SelectorUsesBareFocus(const CSSRule& rule)
{
    for (const auto& term : rule.Selector.Terms)
        for (const auto& pseudo : term.Selector.Pseudos)
            if (pseudo.PseudoKind == PseudoClass::Kind::Focus)
                return true;
    return false;
}

bool ContainsWord(const std::string& haystack, const char* word)
{
    const size_t n = std::strlen(word);
    for (size_t i = haystack.find(word); i != std::string::npos; i = haystack.find(word, i + 1))
    {
        const bool leftOk = i == 0 || !std::isalnum(static_cast<unsigned char>(haystack[i - 1]));
        const bool rightOk = i + n >= haystack.size() ||
                             !std::isalnum(static_cast<unsigned char>(haystack[i + n]));
        if (leftOk && rightOk)
            return true;
    }
    return false;
}

// A rule "draws a ring" when it sets outline-style to anything but none —
// whether written as the `outline` shorthand or the longhand. `outline: none`
// is the opt-out every editor sheet uses and stays legal on :focus, because
// suppressing on the wider state also suppresses on the narrower one.
//
// The typed OutlineStyle property is only half the story. A declaration
// containing var() is not parsed into typed properties at all: the stylesheet
// parser defers it verbatim as a DeferredDecl so the variable can be
// substituted at compute time. The shipped ring is written
// `outline: 2px solid var(--ui_color_accent_blue)`, so it lives ENTIRELY in
// that deferred form — a check that reads only typed properties passes on the
// exact defect it was written to catch. It did, until this branch was added.
bool RuleDrawsARing(const CSSRule& rule)
{
    for (const auto& prop : rule.Properties)
    {
        if (prop.PropertyId == StylePropertyId::OutlineStyle)
        {
            const auto* style = std::get_if<BorderStyle>(&prop.Value);
            if (style && *style != BorderStyle::None)
                return true;
            continue;
        }

        if (prop.PropertyId != StylePropertyId::DeferredDecl)
            continue;
        const auto* decl = std::get_if<GameEngine::DeferredDeclValue>(&prop.Value);
        if (!decl || (decl->Name != "outline" && decl->Name != "outline-style"))
            continue;

        std::string value = decl->Value;
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // Unresolvable without the cascade, so assume it rings unless it
        // explicitly says otherwise. A false positive here is a loud test
        // failure; a false negative is the bug shipping again.
        if (!ContainsWord(value, "none") && !ContainsWord(value, "hidden"))
            return true;
    }
    return false;
}

} // namespace

// The migration that prompted this suite missed a byte-identical duplicate in a
// second file. Grepping one file proves nothing about the other 45, so this
// walks every shipped sheet: a ring keyed on :focus fires for mouse focus too,
// which is the naive behaviour :focus-visible exists to replace.
TEST(ShippedFocusRingTests, NoShippedRuleDrawsARingOnBareFocus)
{
    const std::filesystem::path root = ShippedCssRoot();
    ASSERT_TRUE(std::filesystem::exists(root))
        << "shipped stylesheets are not staged next to the test exe: " << root.string();

    std::vector<std::string> offenders;
    size_t sheetsScanned = 0;
    size_t rulesScanned = 0;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".css")
            continue;

        std::ifstream in(entry.path(), std::ios::binary);
        ASSERT_TRUE(in) << "cannot read staged stylesheet " << entry.path().string();
        std::ostringstream ss;
        ss << in.rdbuf();

        Stylesheet sheet{};
        ASSERT_TRUE(CSSParser::ParseStylesFromString(ss.str(), sheet))
            << "shipped stylesheet failed to parse: " << entry.path().string();
        ++sheetsScanned;
        rulesScanned += sheet.Rules.size();

        for (const CSSRule& rule : sheet.Rules)
        {
            if (SelectorUsesBareFocus(rule) && RuleDrawsARing(rule))
            {
                offenders.push_back(
                    std::filesystem::relative(entry.path(), root).string() + " (rule #" +
                    std::to_string(rule.Order) + ")");
            }
        }
    }

    // A pass with nothing scanned is a staging failure wearing a green tick.
    ASSERT_GT(sheetsScanned, 0u) << "no stylesheets were scanned";
    ASSERT_GT(rulesScanned, 0u) << "stylesheets were found but parsed to zero rules";

    std::string report;
    for (const std::string& o : offenders)
        report += "\n  " + o;
    EXPECT_TRUE(offenders.empty())
        << "these shipped rules ring on mouse focus; they want :focus-visible:" << report;
}

// ---------------------------------------------------------------------------
// A dimmed icon button must not dim its own focus ring.
//
// theme/core.css rests every icon button at `opacity: 0.35` so its artwork
// reads as inactive — 67 rules do this. CSS `opacity` applies to the element's
// whole rendering INCLUDING its outline, so without a counter-rule the one
// indicator that must be legible when it appears inherits that 0.35. Measured
// in the editor before the fix: the ring composited to (42, 72, 111), which is
// the authored #3A8FFF at exactly 0.35 over the #222222 toolbar.
//
// This reads the shipped sheets, so it fails if the counter-rule is deleted OR
// merely out-cascaded — the failure mode this whole file exists for. The
// resting-opacity assertion is the control: without it the focus assertion
// would also pass on a button that was never dimmed.
// ---------------------------------------------------------------------------
namespace
{

constexpr const char* kIconSheets[] = {
    "theme/tokens.css",
    "theme/core.css",
    "theme/widgets.css",
    "controls/Button.css",
};

constexpr char kIconGeometryCss[] = R"(
#root { display: flex; flex-direction: row; width: 400px; height: 60px; padding: 10px; }
)";

constexpr char kIconXml[] = R"(<uielement id="root">
  <button id="target" class="small secondary icon-button save-icon"/>
  <uielement id="sibling"/>
</uielement>)";

std::string BuildIconCss(std::string& missing)
{
    std::string css;
    for (const char* rel : kIconSheets)
    {
        const std::string text = ReadShippedCss(rel);
        if (text.empty())
        {
            missing = rel;
            return {};
        }
        css += text;
        css += "\n";
    }
    css += kIconGeometryCss;
    return css;
}

} // namespace

TEST(ShippedFocusRingTests, DimmedIconButtonDoesNotDimItsFocusRing)
{
    IsolatedUIFixture fx;
    std::string missing;
    const std::string css = BuildIconCss(missing);
    ASSERT_TRUE(missing.empty()) << "shipped stylesheet not staged: " << missing;
    if (!fx.Build(1.0f, kIconXml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    // CONTROL: the button really is dimmed at rest. Without this the focus
    // assertion below would pass on a button that no rule ever dimmed.
    ASSERT_NE(fx.Style("target"), nullptr);
    const float resting = fx.Style("target")->Visual.Opacity;
    EXPECT_LT(resting, 0.9f)
        << "the shipped icon-button resting opacity is no longer dim (" << resting
        << "), so this test no longer exercises the case it was written for";

    fx.FocusViaTab();
    ASSERT_EQ(fx.Manager().GetFocusedElementId(), "target") << "Tab must reach the icon button";
    ASSERT_TRUE(fx.Manager().IsFocusViaKeyboard());

    const auto ring = FindRing(fx, "target");
    ASSERT_TRUE(ring.has_value()) << "keyboard focus must emit a ring primitive";

    EXPECT_FLOAT_EQ(ring->Opacity, 1.0f)
        << "the focus ring inherited the icon's resting opacity (" << resting
        << "); a focus indicator at that alpha is the faintest thing on the toolbar";
}
