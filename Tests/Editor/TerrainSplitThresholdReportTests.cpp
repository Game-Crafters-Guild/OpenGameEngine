#include "DebugServer/TerrainSplitThresholdReport.h"

#include <gtest/gtest.h>

#include <string>

using GameEngine::Editor::DescribeTerrainSplitThreshold;

// The response carries what the kernels were given, not only what was authored: on a 900-row view a
// target of 11 applies 9.17 px.
TEST(TerrainSplitThresholdReport, ReportsTheAppliedThresholdAndItsRenderHeight)
{
    const nlohmann::json r = DescribeTerrainSplitThreshold(11.0f, 9.1666667f, 900u);
    EXPECT_FLOAT_EQ(r.at("targetPixelError").get<float>(), 11.0f);
    EXPECT_FLOAT_EQ(r.at("splitThresholdPx").get<float>(), 9.1666667f);
    EXPECT_EQ(r.at("renderHeightPx").get<std::uint32_t>(), 900u);
    EXPECT_FLOAT_EQ(r.at("targetReferenceHeightPx").get<float>(), 1080.0f);
    const std::string summary = r.at("summary").get<std::string>();
    EXPECT_NE(summary.find("9.17 px"), std::string::npos) << summary;
    EXPECT_NE(summary.find("900 render rows"), std::string::npos) << summary;
    EXPECT_NE(summary.find("11 px at 1080 rows"), std::string::npos) << summary;
}

TEST(TerrainSplitThresholdReport, BeforeTheFirstFrameTheAppliedFieldsAreNull)
{
    const nlohmann::json r = DescribeTerrainSplitThreshold(11.0f, 0.0f, 0u);
    EXPECT_TRUE(r.at("splitThresholdPx").is_null());
    EXPECT_TRUE(r.at("renderHeightPx").is_null());
    EXPECT_NE(r.at("summary").get<std::string>().find("no frame"), std::string::npos);
}
