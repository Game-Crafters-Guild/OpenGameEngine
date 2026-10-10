#include <gtest/gtest.h>
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/Core/Device.h"

using namespace GameEngine::Rendering;

TEST(MaterialBuilderStageMaskMerge, MergeMetaIntoPipelineDesc_WhenUnset) {
    ShaderMeta meta{};
    // Two ranges, union the stage masks
    PushConstantRangeMeta r1{}; r1.Name = "A"; r1.Size = 32u; r1.StagesMask = (1u<<0);
    PushConstantRangeMeta r2{}; r2.Name = "B"; r2.Size = 64u; r2.StagesMask = (1u<<1);
    meta.PushConstants = { r1, r2 };

    PipelineDesc pd{};
    std::vector<DescriptorSetLayoutDesc> outSets; uint32_t outSize=0, outStages=0;
    MaterialBuilder::BuildPipelineLayoutInputs(meta, outSets, outSize, outStages);

    EXPECT_EQ(outSize, 64u); // max size across ranges
    EXPECT_NE(outStages, 0u);
    EXPECT_TRUE((outStages & kShaderStageVertex) && (outStages & kShaderStageFragment));

    // If desc has size unset, BuildPipelineDescFromMeta should fill it
    PipelineDesc pd2{};
    MaterialBuilder::BuildPipelineDescFromMeta(meta, pd2);
    EXPECT_EQ(pd2.pushConstantSize, 64u);
    EXPECT_TRUE((pd2.pushConstantStagesMask & kShaderStageVertex) && (pd2.pushConstantStagesMask & kShaderStageFragment));
}

