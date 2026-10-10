#include <gtest/gtest.h>
#include "Rendering/Materials/ShaderReflection.h"

using namespace GameEngine::Rendering;

TEST(PushConstantIds, AssignedDeterministicallyAndMerged) {
    StageReflectionResult vs{}; vs.Stage = ShaderStageKind::Vertex;
    PushConstantRangeMeta pc0{}; pc0.Name = "PC0"; pc0.Size = 16; pc0.StagesMask = (1u<<0);
    PushConstantRangeMeta pc1{}; pc1.Name = "PC1"; pc1.Size = 32; pc1.StagesMask = (1u<<0);
    vs.Pcr = { pc0, pc1 };

    StageReflectionResult fs{}; fs.Stage = ShaderStageKind::Fragment;
    PushConstantRangeMeta pc1b{}; pc1b.Name = "PC1"; pc1b.Size = 32; pc1b.StagesMask = (1u<<1);
    fs.Pcr = { pc1b };

    ShaderMeta meta = MergeStages({vs, fs});
    ASSERT_EQ(meta.PushConstants.size(), 2u);
    EXPECT_EQ(meta.PushConstants[0].Name, "PC0");
    EXPECT_EQ(meta.PushConstants[1].Name, "PC1");
    EXPECT_EQ(meta.PushConstants[0].Id, 0u);
    EXPECT_EQ(meta.PushConstants[1].Id, 1u);
    // Merged stage mask for PC1 should include both stages
    EXPECT_NE(meta.PushConstants[1].StagesMask & (1u<<0), 0u);
    EXPECT_NE(meta.PushConstants[1].StagesMask & (1u<<1), 0u);
}

