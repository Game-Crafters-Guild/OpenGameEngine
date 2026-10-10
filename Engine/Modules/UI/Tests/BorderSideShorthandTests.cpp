// Two suites, one subject: a CSS declaration the engine drops on the floor.
//
// BorderSideShorthand — `border-top` / `border-right` / `border-bottom` /
// `border-left` (#875).
//
// These four per-side shorthands were unregistered, so every declaration using
// one was dropped whole — not the width, not the colour — with no diagnostic.
// The pins below are composited pixels rather than resolved-style fields: a
// resolved width proves the cascade stored a number, not that a band was
// painted in it, and the defect this file guards is specifically "the sheet
// says there is a border and the screen has none".
//
// Band samples are 5x5-uniform patches inside a 10px border, so a patch that
// drifts onto an AA edge fails loudly instead of averaging into a pass.
//
// The engine's `border-style` is a WHOLE-BOX property, which is the one place
// this family cannot be CSS-faithful: a per-side <style> has nowhere per-side
// to live. The expansion honours it only where it is expressible per side —
// `none`/`hidden` zero THAT side's width, leaving its colour untouched — and
// never writes the box's border-style, so one side can never restyle the other
// three. The NoneSuppressesOnlyItsOwnSide pin holds that decision in place.
//
// UnknownPropertyDiagnostic — the instrument that would have made #875 visible
// the day it was written, plus the sweeps that keep it honest: an unregistered
// name is recorded once per sheet rather than per declaration, every shipped
// sheet is walked instead of a hand-picked few, and the boot log is pinned at
// zero so the next dropped declaration fails a test instead of scrolling past.
// It lives beside the border family because that family is its worked example
// of what a silent drop costs.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "IsolatedUIFixture.h"
#include "UIPixelReadback.h"

#include "Core/Application.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UITargetSpace.h"

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;
using GameEngine::UITesting::RenderUiToBytes;
using GameEngine::UITesting::Rgb;
using GameEngine::UITesting::UniformCentre;
using GameEngine::UITesting::UniformPatch;
using namespace GameEngine;

namespace
{

// Three 200x120 probes in a row, 20px margins, on an opaque #272727 root, so
// every probe lands on integer pixel edges and the band patches carry no
// partial coverage. Probe x origins: 20, 260, 500.
//
//   sh  - the shorthand under test, top side only
//   lh  - the longhand twin of `sh`; the two must agree to the byte
//   box  - all four sides from `border`, with one side taken back by `none`
constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="row1">
    <uielement id="sh"/><uielement id="lh"/><uielement id="box"/>
  </uielement>
  <uielement id="row2">
    <uielement id="bot"/><uielement id="botlh"/>
  </uielement>
</uielement>)";

constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; background-color: #272727; }
#row1 { display: flex; flex-direction: row; }
#row2 { display: flex; flex-direction: row; }
#sh    { width: 200px; height: 120px; margin: 20px; background-color: #202020; border-top: 10px solid #ff0000; }
#lh    { width: 200px; height: 120px; margin: 20px; background-color: #202020; border-top-width: 10px; border-top-color: #ff0000; }
#box   { width: 200px; height: 120px; margin: 20px; background-color: #202020; border: 10px solid #00ff00; border-bottom: none; }
#bot   { width: 200px; height: 120px; margin: 20px; background-color: #202020; border-bottom: 10px solid #ff0000; }
#botlh { width: 200px; height: 120px; margin: 20px; background-color: #202020; border-bottom-width: 10px; border-bottom-color: #ff0000; }
)";

constexpr float kBorderPx = 10.0f;

// Raw bytes of the fixture's opaque colours under EncodedSrgb readback.
constexpr int kFillR = 32, kFillG = 32, kFillB = 32;       // #202020
constexpr int kRedR = 255, kRedG = 0, kRedB = 0;           // #ff0000
constexpr int kGreenR = 0, kGreenG = 255, kGreenB = 0;     // #00ff00

bool BuildOrSkip(IsolatedUIFixture& fx, std::string& why)
{
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
    {
        why = "No Vulkan device available";
        return false;
    }
    EXPECT_TRUE(built) << fx.Diagnostic();
    return built;
}

// The patch offsets below assume this layout; a drift would silently move
// every sample, so pin the geometry before reading a single pixel.
void AssertGeometry(const IsolatedUIFixture& fx)
{
    const PhysicalRect sh = fx.BorderBox("sh");
    ASSERT_FLOAT_EQ(sh.X, 20.0f);
    ASSERT_FLOAT_EQ(sh.Y, 20.0f);
    ASSERT_FLOAT_EQ(sh.W, 200.0f);
    ASSERT_FLOAT_EQ(sh.H, 120.0f);
    ASSERT_FLOAT_EQ(fx.BorderBox("lh").X, 260.0f);
    ASSERT_FLOAT_EQ(fx.BorderBox("box").X, 500.0f);
    const PhysicalRect bot = fx.BorderBox("bot");
    ASSERT_FLOAT_EQ(bot.X, 20.0f);
    ASSERT_FLOAT_EQ(bot.Y, 180.0f);
    ASSERT_FLOAT_EQ(fx.BorderBox("botlh").X, 260.0f);
}

// Centre of the top band: 5px down, patch spans y+3..y+7 inside the 10px band.
bool TopBand(const std::vector<uint8_t>& px, const PhysicalRect& b, Rgb& out, std::string& why)
{
    return UniformPatch(px, static_cast<uint32_t>(b.X + b.W * 0.5f),
                        static_cast<uint32_t>(b.Y + kBorderPx * 0.5f), out, why);
}

bool BottomBand(const std::vector<uint8_t>& px, const PhysicalRect& b, Rgb& out, std::string& why)
{
    return UniformPatch(px, static_cast<uint32_t>(b.X + b.W * 0.5f),
                        static_cast<uint32_t>(b.Y + b.H - kBorderPx * 0.5f), out, why);
}

bool LeftBand(const std::vector<uint8_t>& px, const PhysicalRect& b, Rgb& out, std::string& why)
{
    return UniformPatch(px, static_cast<uint32_t>(b.X + kBorderPx * 0.5f),
                        static_cast<uint32_t>(b.Y + b.H * 0.5f), out, why);
}

bool RightBand(const std::vector<uint8_t>& px, const PhysicalRect& b, Rgb& out, std::string& why)
{
    return UniformPatch(px, static_cast<uint32_t>(b.X + b.W - kBorderPx * 0.5f),
                        static_cast<uint32_t>(b.Y + b.H * 0.5f), out, why);
}

void ExpectRgb(const Rgb& p, int r, int g, int b, const char* what)
{
    EXPECT_EQ(p.R, r) << what;
    EXPECT_EQ(p.G, g) << what;
    EXPECT_EQ(p.B, b) << what;
}

} // namespace

// The #875 defect itself: the declaration is in the sheet and nothing is drawn.
TEST(BorderSideShorthand, TopShorthandPaintsATopBandOfThatWidthAndColour)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }
    AssertGeometry(fx);

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty()) << "render/readback produced no pixels";

    // Instrument: the backdrop must hold its raw byte before any pin is read.
    // Sampled below both probe rows (row1 0..160, row2 160..320).
    Rgb p{};
    ASSERT_TRUE(UniformPatch(px, 400, 500, p, why)) << why;
    ASSERT_EQ(p.R, 39) << "backdrop control (#272727 raw bytes)";

    const PhysicalRect b = fx.BorderBox("sh");
    ASSERT_TRUE(TopBand(px, b, p, why)) << why;
    ExpectRgb(p, kRedR, kRedG, kRedB, "border-top: 10px solid #ff0000 must paint a red top band");

    ASSERT_TRUE(UniformCentre(px, b, p, why)) << why;
    ExpectRgb(p, kFillR, kFillG, kFillB, "interior stays the element fill");
}

// A per-side shorthand must not touch the other three sides. Guards the
// obvious wrong expansion (reusing ExpandBorderValue's four-side append).
TEST(BorderSideShorthand, TopShorthandDoesNotLeakIntoTheOtherThreeSides)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }
    AssertGeometry(fx);

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty()) << "render/readback produced no pixels";

    const PhysicalRect b = fx.BorderBox("sh");
    Rgb p{};
    ASSERT_TRUE(LeftBand(px, b, p, why)) << why;
    ExpectRgb(p, kFillR, kFillG, kFillB, "left edge must stay unpainted");
    ASSERT_TRUE(RightBand(px, b, p, why)) << why;
    ExpectRgb(p, kFillR, kFillG, kFillB, "right edge must stay unpainted");
    ASSERT_TRUE(BottomBand(px, b, p, why)) << why;
    ExpectRgb(p, kFillR, kFillG, kFillB, "bottom edge must stay unpainted");

    // Same statement in the resolved style, where a leak would show as width.
    const ResolvedStyle* rs = fx.Style("sh");
    ASSERT_NE(rs, nullptr);
    EXPECT_FLOAT_EQ(rs->Layout.BorderWidth.Top, 10.0f);
    EXPECT_FLOAT_EQ(rs->Layout.BorderWidth.Right, 0.0f);
    EXPECT_FLOAT_EQ(rs->Layout.BorderWidth.Bottom, 0.0f);
    EXPECT_FLOAT_EQ(rs->Layout.BorderWidth.Left, 0.0f);

    // The declared side takes the declared colour; the other three keep the
    // initial BorderColorsTRBL value (0x000000FF — alpha 0, i.e. transparent),
    // which is what "the shorthand never wrote here" looks like.
    const BorderColorsTRBL initial{};
    EXPECT_EQ(rs->Visual.BorderColor.Top, 0xFFFF0000u);
    EXPECT_EQ(rs->Visual.BorderColor.Right, initial.Right);
    EXPECT_EQ(rs->Visual.BorderColor.Bottom, initial.Bottom);
    EXPECT_EQ(rs->Visual.BorderColor.Left, initial.Left);
}

// Half the shipped declarations #875 unblocks are `border-bottom`, and the
// paint path picks ONE colour for the whole box, preferring Top and skipping
// only sides whose colour is exactly 0 (UIManager_PrimitiveGen.cpp). Top's
// untouched initial value is 0x000000FF — non-zero, and alpha 0 — so a
// bottom-only border is the case where that heuristic can silently win and
// paint the band transparent. Pinned against the longhand twin, which this
// change does not touch: if both are wrong the defect is in the paint path,
// not in the expansion.
TEST(BorderSideShorthand, BottomOnlyShorthandPaintsItsDeclaredColour)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }
    AssertGeometry(fx);

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty()) << "render/readback produced no pixels";

    Rgb sh{}, lh{};
    ASSERT_TRUE(BottomBand(px, fx.BorderBox("bot"), sh, why)) << why;
    ASSERT_TRUE(BottomBand(px, fx.BorderBox("botlh"), lh, why)) << why;
    ExpectRgb(sh, lh.R, lh.G, lh.B, "shorthand must match its longhand twin");
    ExpectRgb(sh, kRedR, kRedG, kRedB, "border-bottom: 10px solid #ff0000 must paint red");

    // The mechanism, pinned one layer down: the single colour the rect
    // primitive carries must come from a side that is actually drawn.
    const std::vector<UI::UIPrimitive> prims = fx.Primitives("bot");
    ASSERT_FALSE(prims.empty());
    EXPECT_FLOAT_EQ(prims[0].BorderWidths[3], 10.0f) << "B of BorderWidths[L,T,R,B]";
    EXPECT_FLOAT_EQ(prims[0].BorderWidths[1], 0.0f) << "T of BorderWidths[L,T,R,B]";
    EXPECT_EQ(prims[0].BorderColor, UI::PackFromARGB(0xFFFF0000u))
        << "primitive took a colour from a zero-width side";
}

// The shorthand is defined as its longhands; if the two ever disagree, one of
// them is wrong and the sheet author cannot tell which.
TEST(BorderSideShorthand, ShorthandAndLonghandsAgree)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }
    AssertGeometry(fx);

    const ResolvedStyle* sh = fx.Style("sh");
    const ResolvedStyle* lh = fx.Style("lh");
    ASSERT_NE(sh, nullptr);
    ASSERT_NE(lh, nullptr);
    EXPECT_EQ(sh->Layout.BorderWidth, lh->Layout.BorderWidth);
    EXPECT_EQ(sh->Visual.BorderColor, lh->Visual.BorderColor);
    EXPECT_EQ(sh->Visual.BorderStyle, lh->Visual.BorderStyle);

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty()) << "render/readback produced no pixels";

    Rgb a{}, c{};
    ASSERT_TRUE(TopBand(px, fx.BorderBox("sh"), a, why)) << why;
    ASSERT_TRUE(TopBand(px, fx.BorderBox("lh"), c, why)) << why;
    ExpectRgb(a, c.R, c.G, c.B, "shorthand and longhand must composite identically");
    ExpectRgb(a, kRedR, kRedG, kRedB, "...and both must be the declared red");
}

// `border-bottom: none` after `border: 10px solid` must take back exactly one
// side. Setting the box's border-style instead would blank all four, which is
// the failure this pin exists to catch.
TEST(BorderSideShorthand, NoneSuppressesOnlyItsOwnSide)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }
    AssertGeometry(fx);

    const std::vector<uint8_t> px = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(px.empty()) << "render/readback produced no pixels";

    const PhysicalRect b = fx.BorderBox("box");
    Rgb p{};
    ASSERT_TRUE(BottomBand(px, b, p, why)) << why;
    ExpectRgb(p, kFillR, kFillG, kFillB, "border-bottom: none must remove the bottom band");

    ASSERT_TRUE(TopBand(px, b, p, why)) << why;
    ExpectRgb(p, kGreenR, kGreenG, kGreenB, "top band survives");
    ASSERT_TRUE(LeftBand(px, b, p, why)) << why;
    ExpectRgb(p, kGreenR, kGreenG, kGreenB, "left band survives");
    ASSERT_TRUE(RightBand(px, b, p, why)) << why;
    ExpectRgb(p, kGreenR, kGreenG, kGreenB, "right band survives");

    const ResolvedStyle* rs = fx.Style("box");
    ASSERT_NE(rs, nullptr);
    EXPECT_FLOAT_EQ(rs->Layout.BorderWidth.Bottom, 0.0f);
    EXPECT_FLOAT_EQ(rs->Layout.BorderWidth.Top, 10.0f);
    // The whole-box style must be untouched by the per-side `none`; had it been
    // written, ResolveUsedBorderWidths would have zeroed all four widths.
    EXPECT_EQ(rs->Visual.BorderStyle, BorderStyle::Solid);
}

// All four names are registered, each expanding to its own side only. Parser
// level, so it runs on hosts with no Vulkan device.
TEST(BorderSideShorthand, EverySideNameIsRegisteredAndExpandsToItsOwnSide)
{
    struct Case
    {
        const char* Decl;
        StylePropertyId Width;
        StylePropertyId Color;
    };
    const Case cases[] = {
        {"border-top: 3px solid #010203", StylePropertyId::BorderTopWidth, StylePropertyId::BorderTopColor},
        {"border-right: 3px solid #010203", StylePropertyId::BorderRightWidth, StylePropertyId::BorderRightColor},
        {"border-bottom: 3px solid #010203", StylePropertyId::BorderBottomWidth, StylePropertyId::BorderBottomColor},
        {"border-left: 3px solid #010203", StylePropertyId::BorderLeftWidth, StylePropertyId::BorderLeftColor},
    };

    for (const Case& c : cases)
    {
        Stylesheet sheet{};
        const std::string css = std::string("#e { ") + c.Decl + "; }";
        ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(css, sheet)) << c.Decl;
        ASSERT_EQ(sheet.Rules.size(), 1u) << c.Decl;

        bool sawWidth = false;
        bool sawColor = false;
        for (const StyleProperty& p : sheet.Rules[0].Properties)
        {
            // An unregistered name yields exactly one Unknown-id row.
            EXPECT_NE(p.PropertyId, StylePropertyId::Unknown)
                << c.Decl << " produced an Unknown-id row (name not registered)";
            if (p.PropertyId == c.Width)
            {
                sawWidth = true;
                EXPECT_FLOAT_EQ(std::get<float>(p.Value), 3.0f) << c.Decl;
            }
            if (p.PropertyId == c.Color)
                sawColor = true;
            // Nothing but this side's two longhands may be emitted; in
            // particular never BorderStyle, which is a whole-box property.
            EXPECT_NE(p.PropertyId, StylePropertyId::BorderStyle) << c.Decl;
        }
        EXPECT_TRUE(sawWidth) << c.Decl << " did not emit its width longhand";
        EXPECT_TRUE(sawColor) << c.Decl << " did not emit its colour longhand";
    }
}

// --- unknown-property diagnostic -------------------------------------------

// The general failure #875 is one instance of: an unregistered declaration is
// dropped in silence. The parser records the names it could not place so the
// sheet's loader can name them alongside the source path.
TEST(UnknownPropertyDiagnostic, RecordsUnregisteredNamesOnce)
{
    Stylesheet sheet{};
    const char* css = R"(
        .a { colour: red; border-top: 1px solid #333; }
        .b { colour: blue; -webkit-box-shadow: 0 0 1px #000; }
        .c { color: red; }
    )";
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(css, sheet));

    // `colour` appears twice but is reported once; `border-top` and `color`
    // are registered and must not appear at all.
    EXPECT_EQ(std::count(sheet.UnknownProperties.begin(), sheet.UnknownProperties.end(), "colour"), 1)
        << "unknown names must be deduped per sheet";
    EXPECT_NE(std::find(sheet.UnknownProperties.begin(), sheet.UnknownProperties.end(),
                        "-webkit-box-shadow"),
              sheet.UnknownProperties.end());
    EXPECT_EQ(std::find(sheet.UnknownProperties.begin(), sheet.UnknownProperties.end(), "border-top"),
              sheet.UnknownProperties.end())
        << "border-top is registered and must not be reported unknown";
    EXPECT_EQ(std::find(sheet.UnknownProperties.begin(), sheet.UnknownProperties.end(), "color"),
              sheet.UnknownProperties.end());
}

// background-origin and background-clip are unimplemented, and the engine's
// fixed behaviour is the pair of CSS initial values: the image measures against
// the padding box and paints to the border box. A sheet asking for anything
// else — content-box, notably — silently gets neither, so the names must stay
// OFF kKnownUnsupportedProperties: suppressing the warning would turn a real
// authoring mistake into a layout that quietly does nothing. Registering them
// for real is what retires this test.
TEST(UnknownPropertyDiagnostic, BackgroundOriginAndClipStillWarn)
{
    Stylesheet sheet{};
    const char* css = R"(
        .a { background-origin: content-box; background-clip: content-box; }
    )";
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(css, sheet));
    sheet.SourceName = "BackgroundOriginAndClipStillWarn";

    EXPECT_NE(std::find(sheet.UnknownProperties.begin(), sheet.UnknownProperties.end(),
                        "background-origin"),
              sheet.UnknownProperties.end());
    EXPECT_NE(std::find(sheet.UnknownProperties.begin(), sheet.UnknownProperties.end(),
                        "background-clip"),
              sheet.UnknownProperties.end());
    EXPECT_EQ(UIParsing::WarnUnknownStylesheetProperties(sheet), 2u)
        << "both names must reach the boot warning; neither is a deliberate no-op";
}

// The diagnostic must be silent on a clean sheet, or it is noise nobody reads.
// Custom properties are declarations too and are never "unknown".
TEST(UnknownPropertyDiagnostic, SilentForRegisteredNamesAndCustomProperties)
{
    Stylesheet sheet{};
    const char* css = R"(
        :root { --brand: #123456; }
        .a { color: var(--brand); border-left: 2px dashed var(--brand); padding: 4px; display: flex; }
        .b { border-top: none; border-bottom: 0; background-color: #111; }
    )";
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(css, sheet));
    EXPECT_TRUE(sheet.UnknownProperties.empty())
        << "first unexpected name: " << (sheet.UnknownProperties.empty()
                                             ? std::string{}
                                             : sheet.UnknownProperties.front());
}

// The sweep that would have caught #875 the day it was written: every shipped
// sheet, not a hand-picked few — the gap was invisible precisely because nobody
// looked at all of them at once. Walks the tree the build stages next to the
// test exe, the same bytes the editor loads.
TEST(UnknownPropertyDiagnostic, NoShippedStylesheetDeclaresABorderSideShorthandAsUnknown)
{
    const std::filesystem::path root = PathUtils::GetExecutableDirectory() / "Assets" / "UI";
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
        ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(ss.str(), sheet))
            << "shipped stylesheet failed to parse: " << entry.path().string();
        ++sheetsScanned;
        rulesScanned += sheet.Rules.size();

        for (const std::string& name : sheet.UnknownProperties)
        {
            if (name == "border-top" || name == "border-right" || name == "border-bottom" ||
                name == "border-left")
            {
                offenders.push_back(std::filesystem::relative(entry.path(), root).string() + ": " +
                                    name);
            }
        }
    }

    // A pass with nothing scanned is a staging failure wearing a green tick.
    ASSERT_GT(sheetsScanned, 0u) << "no stylesheets were scanned";
    ASSERT_GT(rulesScanned, 0u) << "stylesheets were found but parsed to zero rules";

    std::string joined;
    for (const std::string& o : offenders)
        joined += "\n  " + o;
    EXPECT_TRUE(offenders.empty())
        << offenders.size() << " shipped declaration(s) still dropped:" << joined;
}

// The diagnostic only stays useful while a clean tree is silent: a warning that
// fires on 25 of the shipped sheets every boot is one nobody reads, and the
// next `border-top`-class gap would land in that stream unnoticed. Everything
// the shipped sheets currently declare-and-drop is a deliberate gap, so this
// pins the boot log at zero. A NEW unregistered name — a typo, or a property
// someone assumed worked — fails here instead of scrolling past in a log.
TEST(UnknownPropertyDiagnostic, ShippedStylesheetsWarnAboutNothing)
{
    const std::filesystem::path root = PathUtils::GetExecutableDirectory() / "Assets" / "UI";
    ASSERT_TRUE(std::filesystem::exists(root))
        << "shipped stylesheets are not staged next to the test exe: " << root.string();

    std::vector<std::string> offenders;
    size_t sheetsScanned = 0;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".css")
            continue;

        std::ifstream in(entry.path(), std::ios::binary);
        ASSERT_TRUE(in) << "cannot read staged stylesheet " << entry.path().string();
        std::ostringstream ss;
        ss << in.rdbuf();

        Stylesheet sheet{};
        ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(ss.str(), sheet));
        sheet.SourceName = entry.path().string();
        ++sheetsScanned;

        if (UIParsing::WarnUnknownStylesheetProperties(sheet) > 0)
            offenders.push_back(std::filesystem::relative(entry.path(), root).string());
    }

    ASSERT_GT(sheetsScanned, 0u) << "no stylesheets were scanned";

    std::string joined;
    for (const std::string& o : offenders)
        joined += "\n  " + o;
    EXPECT_TRUE(offenders.empty())
        << offenders.size()
        << " shipped sheet(s) warn at boot. Three ways out: delete the declaration if the sheet"
           " does not need it, register the property if it should work, or — if the engine"
           " deliberately does not implement it — add it to kKnownUnsupportedProperties"
           " (CSSStylesheetParser.cpp):"
        << joined;
}
