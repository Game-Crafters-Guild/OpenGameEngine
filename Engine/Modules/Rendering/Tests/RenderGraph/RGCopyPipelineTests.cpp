#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Tests/TestUtils.h"

#include <gtest/gtest.h>

using namespace GameEngine::Rendering;

namespace
{
constexpr const char* kPipelineName = "Test.Copy";
constexpr const char* kLayoutName = "Test.Copy.Set0";
} // namespace

// AddCopyPass binds one combined-image-sampler at set 0 binding 0; the loaded
// desc must carry exactly that layout on canonical fullscreen state.
TEST(RGCopyPipeline, LoadsStockCopyOnFullscreenState)
{
    DescriptorSetLayoutDesc layout{};
    PipelineDesc pipeline{};
    if (!RenderGraph::LoadCopyPipelineDesc(ShaderSourceKind::SpirV, kPipelineName, kLayoutName,
                                           layout, pipeline))
        GTEST_SKIP() << "copy.shaderpkg unavailable in this environment";

    ASSERT_EQ(layout.bindings.size(), 1u);
    EXPECT_EQ(layout.bindings[0].binding, 0u);
    EXPECT_EQ(layout.bindings[0].type, DescriptorType::CombinedImageSampler);
    EXPECT_EQ(layout.bindings[0].shaderStages, kShaderStageFragment);
    EXPECT_STREQ(layout.debugName, kLayoutName);

    EXPECT_FALSE(pipeline.vertexShader.empty());
    EXPECT_FALSE(pipeline.pixelShader.empty());
    EXPECT_EQ(pipeline.type, PipelineType::Graphics);
    EXPECT_EQ(pipeline.rasterizationSamples, 1u);
    EXPECT_STREQ(pipeline.debugName, kPipelineName);
    ASSERT_EQ(pipeline.descriptorSetLayouts.size(), 1u);
    EXPECT_EQ(pipeline.descriptorSetLayouts[0].bindings.size(), 1u);
}
