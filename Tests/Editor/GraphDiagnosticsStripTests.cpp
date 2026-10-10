// The diagnostics strip's stylesheet: that it folds to one row, that its
// opened list is bounded, and that every rule stays a single flat selector.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
std::string ReadNodeGraphCss()
{
    const std::filesystem::path path =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets" / "UI" / "theme" / "node-graph.css";
    std::ifstream in(path, std::ios::binary);
    if (!in.good())
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string RuleBody(const std::string& css, const std::string& selector)
{
    const size_t rule = css.find(selector + " {");
    if (rule == std::string::npos)
        return {};
    const size_t end = css.find('}', rule);
    if (end == std::string::npos)
        return {};
    return css.substr(rule, end - rule);
}
} // namespace

TEST(GraphDiagnosticsStripTests, StripFoldsToOneRowAndOpensToABoundedList)
{
    const std::string css = ReadNodeGraphCss();
    ASSERT_FALSE(css.empty()) << "node-graph.css did not read; this test is vacuous";

    const std::string summary = RuleBody(css, ".node-graph-diagnostic-summary");
    ASSERT_FALSE(summary.empty());
    EXPECT_NE(summary.find("max-height: 22px"), std::string::npos)
        << "the folded strip is one text row; an unclamped line can grow a second";
    EXPECT_NE(summary.find("cursor: pointer"), std::string::npos)
        << "the whole line is the click target";

    const std::string summaryText = RuleBody(css, ".node-graph-diagnostic-summary-text");
    ASSERT_FALSE(summaryText.empty());
    /* The CSS ellipsis recipe: nowrap + overflow hidden + text-overflow. All
       three, or the folded line either wraps to a second row or cuts a glyph
       mid-stroke with nothing to say it was cut. */
    EXPECT_NE(summaryText.find("white-space: nowrap"), std::string::npos);
    EXPECT_NE(summaryText.find("overflow: hidden"), std::string::npos)
        << "a long first message is truncated, never wrapped onto a second row";
    EXPECT_NE(summaryText.find("text-overflow: ellipsis"), std::string::npos);

    /* The list's visibility is one modifier class, so GraphPanel toggles a
       class and sets no style of its own. */
    const std::string list = RuleBody(css, ".node-graph-diagnostic-list");
    ASSERT_FALSE(list.empty());
    EXPECT_NE(list.find("display: none"), std::string::npos) << "collapsed is the default";
    EXPECT_NE(list.find("max-height:"), std::string::npos)
        << "the opened list is bounded and scrolls; it must not push the canvas off-panel";
    EXPECT_NE(RuleBody(css, ".node-graph-diagnostic-list.expanded").find("display: flex"),
              std::string::npos);

    /* One term for one state: `expanded` on every part, so the chevron's
       default is the folded arrow rather than the opposite word. */
    EXPECT_NE(RuleBody(css, ".node-graph-diagnostic-chevron").find("arrow-right"),
              std::string::npos)
        << "folded is the chevron's default, matching the list's display:none";
    EXPECT_NE(RuleBody(css, ".node-graph-diagnostic-chevron.expanded").find("arrow-down"),
              std::string::npos)
        << "the chevron's icon is a CSS class, not a C++ assignment";
    EXPECT_EQ(css.find(".node-graph-diagnostic-chevron.collapsed"), std::string::npos)
        << "no second name for the same state";

    /* The on-disk-failure hint is not a compile failure of anything in this
       panel, and it is muted rather than error-red to say so. */
    EXPECT_NE(RuleBody(css, ".node-graph-diagnostic-summary-text.no-toggle")
                  .find("var(--graph-diagnostic-muted-text)"),
              std::string::npos);
    EXPECT_NE(css.find("--graph-diagnostic-muted-text: #C89A9A"), std::string::npos)
        << "the muted tone #1230 shipped the hint in";
}

TEST(GraphDiagnosticsStripTests, DiagnosticsRulesStayFlatAndAboveTheTypographyFloor)
{
    const std::string css = ReadNodeGraphCss();
    ASSERT_FALSE(css.empty()) << "node-graph.css did not read; this test is vacuous";

    /* One selector per rule: no ancestor chain into the strip's own children,
       which a shorter row height makes unnecessary. */
    EXPECT_EQ(css.find(".node-graph-diagnostics ."), std::string::npos);

    /* The "+N more" row and the separate hint label are gone: the opened list
       is complete, and the on-disk hint is the summary line's message. */
    EXPECT_EQ(css.find("node-graph-diagnostic-more"), std::string::npos);
    EXPECT_EQ(css.find("node-graph-diagnostic-hint"), std::string::npos);

    for (const char* rule : {".node-graph-diagnostic-summary-text",
                             ".node-graph-diagnostic-row-text"})
    {
        const std::string body = RuleBody(css, rule);
        ASSERT_FALSE(body.empty()) << rule;
        EXPECT_NE(body.find("font-size: 12px"), std::string::npos)
            << rule << ": editor UI does not go below the 12px floor";
        EXPECT_NE(body.find("text-overflow: ellipsis"), std::string::npos)
            << rule << ": a truncated diagnostic must show that it was truncated";
    }
}
