// A search field's placeholder as the editor ships it: a hint that cannot be mistaken for
// typed text. SearchFieldWithFilter (and every panel search bar built on it) draws its
// placeholder as a label with the shared .search-field-with-filter-placeholder class, so the
// rule in theme/widgets.css is the default every search field takes. Read from the staged
// sheets, so a panel sheet cannot be what makes the case pass.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/UIStyle.h"

#include "Core/Application.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

using GameEngine::FontStyle;
using GameEngine::PathUtils;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

// The stylesheets the build stages next to the test exe, at the relative path the editor
// reads them from under its own exe.
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
  <label id="placeholder" class="search-field-with-filter-placeholder">Search</label>
  <label id="typed">Search</label>
</uielement>)";

// The typed text takes the theme's text color, as the search field's input does.
constexpr char kTestCss[] = R"(
#root { display: flex; flex-direction: column; width: 300px; height: 100px; }
#typed { color: var(--ui_color_text); }
)";

// One 8-bit channel of an ARGB color, `shift` bits up.
std::uint32_t Channel(std::uint32_t argb, int shift)
{
    return (argb >> shift) & 0xFFu;
}

} // namespace

TEST(ShippedSearchPlaceholderTests, ThePlaceholderIsDimmerThanTypedTextAndItalic)
{
    std::string css;
    for (const char* sheet : {"theme/tokens.css", "theme/widgets.css"})
    {
        const std::string text = ReadShippedCss(sheet);
        ASSERT_FALSE(text.empty()) << "shipped stylesheet not staged next to the test exe: " << sheet;
        css += text;
        css += '\n';
    }
    css += kTestCss;

    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kXml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }
    const GameEngine::ResolvedStyle* placeholder = fx.Style("placeholder");
    const GameEngine::ResolvedStyle* typed = fx.Style("typed");
    ASSERT_NE(placeholder, nullptr);
    ASSERT_NE(typed, nullptr);

    EXPECT_EQ(placeholder->Visual.FontStyle, FontStyle::Italic);
    EXPECT_EQ(typed->Visual.FontStyle, FontStyle::Normal);
    // A clear step down on every channel, not a near-equal grey.
    constexpr std::uint32_t kMinStep = 0x20;
    for (const int shift : {16, 8, 0})
    {
        EXPECT_GE(Channel(typed->Visual.Color, shift), Channel(placeholder->Visual.Color, shift) + kMinStep)
            << "channel at bit " << shift << ": typed " << std::hex << typed->Visual.Color << ", placeholder "
            << placeholder->Visual.Color;
    }
}
