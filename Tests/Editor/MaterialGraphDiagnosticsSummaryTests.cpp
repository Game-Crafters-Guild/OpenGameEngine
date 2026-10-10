// The shader graph panel's compile verdict, derived from the compiler's
// diagnostics. Pure, so the label text, tooltip and blamed-node set are pinned
// here without a panel (nothing instantiates ShaderGraphPanel in a test target).

#include <gtest/gtest.h>

#include "ShaderGraph/MaterialGraphDiagnosticsSummary.h"

#include <string>
#include <unordered_set>

using namespace GameEngine;

TEST(MaterialGraphDiagnosticsSummaryTests, CleanCompileReadsOkAndBlamesNoNode)
{
    const MaterialGraphDiagnosticsSummary summary = SummarizeMaterialGraphDiagnostics({});
    EXPECT_EQ(summary.LabelText, "OK");
    EXPECT_TRUE(summary.Tooltip.empty());
    EXPECT_TRUE(summary.ErrorNodeIds.empty());
}

TEST(MaterialGraphDiagnosticsSummaryTests, LabelCountsErrorsWithTheRightPlural)
{
    EXPECT_EQ(SummarizeMaterialGraphDiagnostics({{"a", "n1"}}).LabelText, "1 error");
    EXPECT_EQ(SummarizeMaterialGraphDiagnostics({{"a", "n1"}, {"b", "n2"}, {"c", {}}}).LabelText,
              "3 errors");
}

TEST(MaterialGraphDiagnosticsSummaryTests, TooltipJoinsEveryMessageInOrder)
{
    const MaterialGraphDiagnosticsSummary summary =
        SummarizeMaterialGraphDiagnostics({{"first", "n1"}, {"second", {}}});
    EXPECT_EQ(summary.Tooltip, "first; second");
}

TEST(MaterialGraphDiagnosticsSummaryTests, BlamedNodesAreTheNamedOnesDeduplicated)
{
    // One bad node cascades into several diagnostics; the graph-scoped one
    // (no NodeId) must not contribute an empty id.
    const MaterialGraphDiagnosticsSummary summary = SummarizeMaterialGraphDiagnostics(
        {{"unknown node type", "n_bogus"},
         {"no matching overload", "n_bogus"},
         {"missing SurfaceOutput node", {}}});
    EXPECT_EQ(summary.ErrorNodeIds, (std::unordered_set<std::string>{"n_bogus"}));
}

TEST(MaterialGraphDiagnosticsSummaryTests, OnDiskHintCountsAndPluralises)
{
    EXPECT_EQ(OnDiskFailureHintText(1), "file failing on disk (1 error) — see Shader Errors");
    EXPECT_EQ(OnDiskFailureHintText(6), "file failing on disk (6 errors) — see Shader Errors");
}

TEST(MaterialGraphDiagnosticsSummaryTests, CollapsedLineCountsFailuresAndQuotesTheFirst)
{
    EXPECT_TRUE(SummarizeMaterialGraphDiagnostics({}).CollapsedLineText.empty())
        << "no diagnostics: the strip hides rather than showing an empty line";

    EXPECT_EQ(SummarizeMaterialGraphDiagnostics({{"Undeclared identifier 'foo'", "n-mul"}})
                  .CollapsedLineText,
              "1 error \xC2\xB7 Undeclared identifier 'foo'");

    const std::string many = SummarizeMaterialGraphDiagnostics({{"first failure", "n-a"},
                                                                {"second failure", "n-b"},
                                                                {"third failure", {}}})
                                 .CollapsedLineText;
    EXPECT_EQ(many, "3 errors \xC2\xB7 first failure");
    /* One message only: the folded line is clipped at the strip's width, so a
       second message would be cut before the first finished reading. */
    EXPECT_EQ(many.find("second failure"), std::string::npos);
}
