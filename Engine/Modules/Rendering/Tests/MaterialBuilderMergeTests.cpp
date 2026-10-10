#include <gtest/gtest.h>
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderMeta.h"
#include <string>

using namespace GameEngine::Rendering;

static ShaderMeta MakeMeta(uint32_t pcSize, uint32_t stagesMask, int bindingType = 0) {
    ShaderMeta m{}; m.Version = 1; m.EntryPoints["vs"] = "main"; m.EntryPoints["fs"] = "main";
    // Push constants
    PushConstantRangeMeta pc{}; pc.Name = "Globals"; pc.Size = pcSize; pc.StagesMask = stagesMask; m.PushConstants.push_back(pc);
    // One set with one binding
    DescriptorBindingMeta b{}; b.Binding = 0; b.Name = "UBO0"; b.Type = bindingType; b.Count = 1; b.StagesMask = stagesMask;
    DescriptorSetMeta s{}; s.Set = 0; s.Bindings.push_back(b); m.Sets.push_back(s);
    return m;
}

TEST(MaterialBuilderMerge, Auto_FillsMissing_NoConflicts) {
    ShaderMeta meta = MakeMeta(64, /*stages*/3, /*UniformBuffer*/0);
    PipelineDesc pd{}; // leave layouts and push constants empty

    std::string err;
    bool ok = MaterialBuilder::BuildPipelineDescFromMeta(meta, pd, MaterialBuilder::MergeMode::Auto, {}, &err);
    ASSERT_TRUE(ok) << err;

    ASSERT_FALSE(pd.descriptorSetLayouts.empty());
    EXPECT_EQ(pd.descriptorSetLayouts[0].bindings.size(), 1u);
    EXPECT_EQ(pd.pushConstantSize, 64u);
    EXPECT_NE(pd.pushConstantStagesMask, 0u);
}

TEST(MaterialBuilderMerge, Auto_ConflictOnPushConstantSize) {
    ShaderMeta meta = MakeMeta(64, 3);
    PipelineDesc pd{}; pd.pushConstantSize = 32; // conflict

    std::string err;
    bool ok = MaterialBuilder::BuildPipelineDescFromMeta(meta, pd, MaterialBuilder::MergeMode::Auto, {}, &err);
    ASSERT_FALSE(ok);
    ASSERT_FALSE(err.empty());
}

TEST(MaterialBuilderMerge, Off_IgnoresMetadata) {
    ShaderMeta meta = MakeMeta(64, 3);
    PipelineDesc pd{}; // no layouts

    std::string err;
    bool ok = MaterialBuilder::BuildPipelineDescFromMeta(meta, pd, MaterialBuilder::MergeMode::Off, {}, &err);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(pd.descriptorSetLayouts.empty());
    EXPECT_EQ(pd.pushConstantSize, 0u);
}

TEST(MaterialBuilderMerge, Require_FailsWhenMetaEmpty) {
    ShaderMeta empty{};
    PipelineDesc pd{};
    std::string err;
    bool ok = MaterialBuilder::BuildPipelineDescFromMeta(empty, pd, MaterialBuilder::MergeMode::Require, {}, &err);
    ASSERT_FALSE(ok);
}

TEST(MaterialBuilderMerge, Policy_EnforceLimit) {
    ShaderMeta meta = MakeMeta(256, 3);
    PipelineDesc pd{};
    std::string err;
    // default policy: enforce limit 128 -> fail
    bool ok = MaterialBuilder::BuildPipelineDescFromMeta(meta, pd, MaterialBuilder::MergeMode::Auto, {}, &err);
    ASSERT_FALSE(ok);
    // allow higher limit -> pass
    MaterialBuilder::PushConstantPolicy policy{}; policy.Enforce = true; policy.Limit = 512;
    ok = MaterialBuilder::BuildPipelineDescFromMeta(meta, pd, MaterialBuilder::MergeMode::Auto, policy, &err);
    ASSERT_TRUE(ok) << err;
    EXPECT_EQ(pd.pushConstantSize, 256u);
}

