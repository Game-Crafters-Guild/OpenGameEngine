// The mark-up inspector's description and comment fields as the editor ships them: like other
// editable text, they take the accent border while they hold the focus. These read the staged
// sheets, so the panel sheet's own rule is what is under test.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/UIStyle.h"

#include "Core/Application.h"

#include <cstdint>
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
  <textarea id="description" class="markup-inspector-description"/>
  <textarea id="comment" class="markup-inspector-comment"/>
</uielement>)";

constexpr char kGeometryCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#description { width: 300px; height: 60px; }
#comment { width: 300px; height: 40px; }
)";

// --ui_color_accent in theme/tokens.css.
constexpr std::uint32_t kAccentArgb = 0xFF006FFFu;

} // namespace

TEST(ShippedMarkupTextAreaFocusTests, TheDescriptionAndCommentTakeTheAccentWhileFocused)
{
    std::string css;
    for (const char* sheet : {"theme/tokens.css", "theme/widgets.css", "panels/MarkupsPanel.css"})
    {
        const std::string text = ReadShippedCss(sheet);
        ASSERT_FALSE(text.empty()) << "shipped stylesheet not staged next to the test exe: " << sheet;
        css += text;
        css += '\n';
    }
    css += kGeometryCss;

    for (const char* id : {"description", "comment"})
    {
        IsolatedUIFixture fx;
        if (!fx.Build(1.0f, kXml, css))
        {
            if (!fx.DeviceAvailable())
                GTEST_SKIP() << fx.Diagnostic();
            FAIL() << fx.Diagnostic();
        }
        ASSERT_NE(fx.Style(id), nullptr) << id;
        EXPECT_NE(fx.Style(id)->Visual.BorderColor.Top, kAccentArgb) << id << " at rest keeps its grey";
        ASSERT_TRUE(fx.FocusViaClick(id)) << id;
        EXPECT_EQ(fx.Style(id)->Visual.BorderColor.Top, kAccentArgb) << id << " focused";
    }
}
