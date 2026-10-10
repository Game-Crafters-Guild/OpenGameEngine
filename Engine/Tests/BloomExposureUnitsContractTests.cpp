// Structural graph contracts complement the production GPU pixel tests.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>

TEST(BloomExposureUnitsContract, HighlightExtractorsBindTheSameExposureAsTonemap)
{
    std::ifstream f(std::filesystem::path(GE_RENDERER_REPO_ROOT) / "Assets/RenderPipelines/ForwardPlus.rendergraph");
    ASSERT_TRUE(f.is_open());
    const auto graph = nlohmann::json::parse(f);
    int extracts = 0, linearDownsamples = 0;
    for (const auto& pass : graph["passes"])
    {
        const auto shader = pass.value("shaderPkg", std::string());
        if (shader == "Shaders/bloom_threshold.shaderpkg" || shader == "Shaders/halation_prefilter.shaderpkg")
        {
            ++extracts;
            EXPECT_EQ(pass["buffers"]["uExposure"], "ExposureHistory");
            EXPECT_EQ(pass["bufferUsages"]["uExposure"], "Storage");
            EXPECT_TRUE(pass["pushConstants"].contains("exposure"));
            EXPECT_TRUE(pass["pushConstants"].contains("useAutoExposure"));
        }
        if (shader == "Shaders/bloom_downsample.shaderpkg")
        {
            ++linearDownsamples;
            EXPECT_FALSE(pass.contains("buffers")) << "linear filtering has no exposure dependency";
        }
    }
    EXPECT_EQ(extracts, 3);
    EXPECT_EQ(linearDownsamples, 14);
}

TEST(BloomExposureUnitsContract, HalationReadsOriginalHighlightsNotScatteringOrBloom)
{
    std::ifstream f(std::filesystem::path(GE_RENDERER_REPO_ROOT) / "Assets/RenderPipelines/ForwardPlus.rendergraph");
    ASSERT_TRUE(f.is_open());
    const auto graph = nlohmann::json::parse(f);
    int checked = 0;
    for (const auto& pass : graph["passes"])
    {
        const auto id = pass.value("id", std::string());
        if (id == "HalationHighlights")
        {
            ++checked;
            EXPECT_EQ(pass["inputs"]["uHDR"], "HDRUpscaled");
            EXPECT_TRUE(pass["skipWhen"].contains("halationActive"));
        }
        if (id == "HalationComposite")
        {
            ++checked;
            EXPECT_EQ(pass["inputs"]["uHighlights"], "HalationHorizontal");
            EXPECT_EQ(pass["inputs"]["uHDR"], "HDRBloomWithDirt");
        }
        if (id == "BloomCombine")
        {
            ++checked;
            EXPECT_EQ(pass["inputs"]["uBloom"], "BloomNormalized");
            EXPECT_EQ(pass["inputs"]["uScattering"], "ScatteringNormalized");
            EXPECT_FALSE(pass["pushConstants"].contains("halationIntensity"));
        }
    }
    EXPECT_EQ(checked, 3);
}
