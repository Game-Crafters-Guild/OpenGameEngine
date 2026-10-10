// TextArea's public metrics: the line advance and the visual line count a
// container reads to size itself around the text (the AI Assistant's input
// grows with its prompt). Both must match what the control draws.

#include "IsolatedUIFixture.h"

#include "UI/Controls/TextArea.h"

#include <gtest/gtest.h>

#include <string>

using GameEngine::TextArea;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{
// `&#10;` is a newline that survives XML attribute parsing. The wrapped area is
// 60px wide, so the sentence takes several lines at 16px.
constexpr char kXml[] = R"(<uielement id="root">
  <textarea id="lines" value="HH&#10;HH&#10;HH"/>
  <textarea id="wrapped" value="one two three four five six seven eight"/>
  <textarea id="empty" value=""/>
</uielement>)";

constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 600px; height: 400px; }
#lines, #wrapped, #empty {
  height: 100px;
  font-family: Roboto;
  font-size: 16px;
  line-height: 24px;
  padding: 0px;
  border-width: 0px;
}
#lines { width: 300px; white-space: nowrap; }
#wrapped { width: 60px; }
#empty { width: 300px; }
)";

const TextArea* Area(const IsolatedUIFixture& fx, const char* id)
{
    return dynamic_cast<const TextArea*>(fx.Element(id));
}
} // namespace

TEST(TextAreaMetrics, LineAdvanceIsTheAuthoredLineHeightAndNewlinesCountAsLines)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const TextArea* lines = Area(fx, "lines");
    const TextArea* empty = Area(fx, "empty");
    ASSERT_NE(lines, nullptr);
    ASSERT_NE(empty, nullptr);
    EXPECT_FLOAT_EQ(lines->GetResolvedLineAdvancePx(), 24.0f);
    EXPECT_EQ(lines->GetVisualLineCount(), 3u);
    EXPECT_EQ(empty->GetVisualLineCount(), 1u);
}

TEST(TextAreaMetrics, WrappedLinesCountAtTheElementsWidth)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const TextArea* wrapped = Area(fx, "wrapped");
    ASSERT_NE(wrapped, nullptr);
    EXPECT_GT(wrapped->GetVisualLineCount(), 2u)
        << "eight words at 16px cannot share fewer than three 60px lines";
}
