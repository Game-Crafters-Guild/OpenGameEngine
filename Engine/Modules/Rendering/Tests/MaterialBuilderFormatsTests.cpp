#include <gtest/gtest.h>
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderMeta.h"

using namespace GameEngine::Rendering;

static ShaderMeta MakeBasicMeta(uint32_t pcSize = 64, uint32_t stagesMask = 3) {
    ShaderMeta m{}; m.Version = 1; m.EntryPoints["vs"] = "main"; m.EntryPoints["fs"] = "main";
    // One UBO in set 0
    DescriptorBindingMeta b{}; b.Binding = 0; b.Name = "UBO0"; b.Type = 0; b.Count = 1; b.StagesMask = stagesMask;
    DescriptorSetMeta s{}; s.Set = 0; s.Bindings.push_back(b); m.Sets.push_back(s);
    // Push constants
    PushConstantRangeMeta pc{}; pc.Name = "Globals"; pc.Size = pcSize; pc.StagesMask = stagesMask; m.PushConstants.push_back(pc);
    return m;
}

TEST(MaterialBuilderFormats, Auto_FillsFormatsHint_WhenMissing) {
    ShaderMeta meta = MakeBasicMeta();
    PipelineDesc pd{}; // leave formats empty

    MaterialBuilder::FormatsHint hint{};
    hint.ColorFormats = { /* VK_FORMAT_B8G8R8A8_UNORM */ 44u };
    hint.DepthFormat = /* VK_FORMAT_D32_SFLOAT */ 126u;

    std::string err;
    bool ok = MaterialBuilder::BuildPipelineDescFromMeta(meta, pd, hint, MaterialBuilder::MergeMode::Auto, {}, &err);
    ASSERT_TRUE(ok) << err;

    ASSERT_EQ(pd.colorAttachmentFormats.size(), 1u);
    EXPECT_EQ(pd.colorAttachmentFormats[0], 44u);
    EXPECT_EQ(pd.depthAttachmentFormat, 126u);
}

TEST(MaterialBuilderFormats, Off_DoesNotApplyFormatsHint) {
    ShaderMeta meta = MakeBasicMeta();
    PipelineDesc pd{}; // leave formats empty

    MaterialBuilder::FormatsHint hint{};
    hint.ColorFormats = { 44u };
    hint.DepthFormat = 126u;

    std::string err;
    bool ok = MaterialBuilder::BuildPipelineDescFromMeta(meta, pd, hint, MaterialBuilder::MergeMode::Off, {}, &err);
    ASSERT_TRUE(ok) << err;

    EXPECT_TRUE(pd.colorAttachmentFormats.empty());
    EXPECT_EQ(pd.depthAttachmentFormat, 0u);
}

