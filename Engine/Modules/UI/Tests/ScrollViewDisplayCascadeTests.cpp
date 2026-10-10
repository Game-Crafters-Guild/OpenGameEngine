// GitHub #1382 — a stylesheet must be able to hide a ScrollView.
//
// ScrollView used to set `display` as an inline style override in its
// constructor. Overrides land on the resolved style AFTER the cascade
// (UIManager_StyleResolve.cpp), so no rule could outrank them and every
// `... scrollview { display: none; }` in the shipped theme was dead. The
// graph panel's collapsed palette is the specimen: double-clicking the title
// hid the search row and the content, and left the scroller behind.
//
// The default now arrives through the cascade instead, from the control's own
// sheet (controls/ScrollView.css) at type-selector specificity, so any
// class-bearing rule wins. Both halves are asserted: the default still resolves
// to `flex` when nothing else claims it, and a state class hides it. Without
// the first, the second also passes when ScrollView renders nothing at all.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/UIStyle.h"

#include "Core/Application.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

using GameEngine::DisplayMode;
using GameEngine::PathUtils;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

// The stylesheets the build stages next to the test exe, at the same relative
// path the editor reads them from under its own exe. Anchored to the
// executable directory, never back into the source tree.
std::string ReadShippedCss(const std::string& relative)
{
    std::ifstream in(PathUtils::GetExecutableDirectory() / "Assets" / "UI" / relative,
                     std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="host">
    <scrollview id="sv" class="node-graph-palette-scroll"/>
  </uielement>
</uielement>)";

// Geometry only. Nothing here mentions display on the scrollview, so the sheet
// under test stays the sole authority on it.
constexpr char kGeometryCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#host { display: flex; flex-direction: column; width: 200px; height: 200px; }
)";

// The fixture takes one CSS string, so the sheets are concatenated in the order
// the editor cascades them: the control's subtree sheet is the lowest-priority
// one that mentions the scrollview, and specificity decides the rest.
std::string ShippedCss(std::string& missing)
{
    std::string css;
    for (const char* rel : {"controls/ScrollView.css", "theme/node-graph.css"})
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

// Builds the fixture with `hostClass` on the scroller's parent and reports the
// display the cascade resolved for the scroller. Returns false when the fixture
// could not be built at all; Diagnostic() then says whether that was a missing
// device (skip) or the test's own CSS/XML (failure).
bool ResolveScrollViewDisplay(IsolatedUIFixture& fx, const std::string& css,
                              const std::string& hostClass, DisplayMode& outDisplay)
{
    std::string xml = kXml;
    if (!hostClass.empty())
    {
        const std::string anchor = R"(<uielement id="host">)";
        xml.replace(xml.find(anchor), anchor.size(),
                    R"(<uielement id="host" class=")" + hostClass + R"(">)");
    }
    if (!fx.Build(1.0f, xml, css))
        return false;
    const GameEngine::ResolvedStyle* style = fx.Style("sv");
    if (!style)
        return false;
    outDisplay = style->Layout.DisplayMode;
    return true;
}

#define REQUIRE_SCROLLVIEW_DISPLAY(fx, css, hostClass, out)                                        \
    do                                                                                             \
    {                                                                                              \
        if (!ResolveScrollViewDisplay((fx), (css), (hostClass), (out)))                            \
        {                                                                                          \
            if (!(fx).DeviceAvailable())                                                           \
                GTEST_SKIP() << (fx).Diagnostic();                                                 \
            FAIL() << (fx).Diagnostic();                                                           \
        }                                                                                          \
    } while (false)

} // namespace

// The regression itself, stated in the smallest CSS that shows it: a descendant
// rule naming the element type must be able to take the scroller out of layout.
TEST(ScrollViewDisplayCascadeTests, StateClassRuleHidesTheScrollView)
{
    IsolatedUIFixture fx;
    constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#host { display: flex; flex-direction: column; width: 200px; height: 200px; }
scrollview { display: flex; }
.collapsed scrollview { display: none; }
)";
    DisplayMode display{};
    REQUIRE_SCROLLVIEW_DISPLAY(fx, kCss, "collapsed", display);
    EXPECT_EQ(display, DisplayMode::None);
}

// The control for the case above: with no state class the scroller keeps the
// default its own sheet declares, so an assertion that it can be hidden is not
// passing because it was never displayed.
TEST(ScrollViewDisplayCascadeTests, DefaultDisplayIsFlexWithoutAStateClass)
{
    IsolatedUIFixture fx;
    constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#host { display: flex; flex-direction: column; width: 200px; height: 200px; }
scrollview { display: flex; }
.collapsed scrollview { display: none; }
)";
    DisplayMode display{};
    REQUIRE_SCROLLVIEW_DISPLAY(fx, kCss, "", display);
    EXPECT_EQ(display, DisplayMode::Flex);
}

// Against the sheets the editor actually ships, so the graph panel's collapsed
// palette is covered by its own rule rather than by CSS a test wrote.
TEST(ScrollViewDisplayCascadeTests, ShippedPaletteCollapsedRuleHidesTheScrollView)
{
    IsolatedUIFixture fx;
    std::string missing;
    const std::string css = ShippedCss(missing);
    ASSERT_TRUE(missing.empty()) << "shipped stylesheet not staged next to the test exe: "
                                 << missing;
    DisplayMode display{};
    REQUIRE_SCROLLVIEW_DISPLAY(fx, css, "node-graph-palette node-graph-palette-collapsed", display);
    EXPECT_EQ(display, DisplayMode::None);
}

// Its control: the same shipped sheets with the palette expanded.
TEST(ScrollViewDisplayCascadeTests, ShippedExpandedPaletteKeepsTheScrollView)
{
    IsolatedUIFixture fx;
    std::string missing;
    const std::string css = ShippedCss(missing);
    ASSERT_TRUE(missing.empty()) << "shipped stylesheet not staged next to the test exe: "
                                 << missing;
    DisplayMode display{};
    REQUIRE_SCROLLVIEW_DISPLAY(fx, css, "node-graph-palette", display);
    EXPECT_EQ(display, DisplayMode::Flex);
}
