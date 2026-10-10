// The mark-ups' unseen state, as the editor ships it: the Scene View pill's indicator and the
// Mark-ups panel row's dot are one look, a filled dot in the same color, so the world and the
// panel say "unseen" alike. Read from the staged sheets.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/UIStyle.h"

#include "Core/Application.h"

#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

using GameEngine::PathUtils;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

std::string ReadShippedCss(const std::string& relative)
{
    std::ifstream in(PathUtils::GetExecutableDirectory() / "Assets" / "UI" / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="pill" class="markup-label-unread"/>
  <uielement id="row" class="markups-update-dot"/>
</uielement>)";

constexpr char kGeometryCss[] = R"(
#root { display: flex; flex-direction: column; width: 200px; height: 100px; }
)";

} // namespace

TEST(ShippedMarkupUnseenDotTests, ThePillsUnseenDotIsTheRowsDotFilled)
{
    std::string css;
    for (const char* sheet : {"theme/tokens.css", "controls/Markups/MarkupLabelOverlay.css", "panels/MarkupsPanel.css"})
    {
        const std::string text = ReadShippedCss(sheet);
        ASSERT_FALSE(text.empty()) << "shipped stylesheet not staged next to the test exe: " << sheet;
        css += text;
        css += '\n';
    }
    css += kGeometryCss;

    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kXml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }
    ASSERT_NE(fx.Style("pill"), nullptr);
    ASSERT_NE(fx.Style("row"), nullptr);
    EXPECT_EQ(fx.Style("pill")->Visual.BackgroundColor >> 24, 0xFFu) << "the pill's dot is filled";
    EXPECT_EQ(fx.Style("pill")->Visual.BackgroundColor, fx.Style("row")->Visual.BackgroundColor);
}
