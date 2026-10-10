#include <gtest/gtest.h>
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"

#include <cstring>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{

void ReflectComputeShader(const char* spvName, ShaderMeta& out)
{
    std::vector<uint8_t> bytes = Utils::LoadShaderFile(spvName);
    ASSERT_FALSE(bytes.empty()) << "Missing " << spvName;
    ASSERT_EQ(bytes.size() % 4, 0u);
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());

    ReflectionOptions opts{};
    StageReflectionResult stage{};
    std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, words.data(), words.size(), opts, stage, &err)) << err;
    out = MergeStages({stage});
}

const Member* FindBlockMember(const ShaderMeta& meta, const char* bindingName, const char* memberName)
{
    for (const DescriptorSetMeta& set : meta.Sets)
        for (const DescriptorBindingMeta& binding : set.Bindings)
            if (binding.Name == bindingName && binding.Block)
                for (const Member& m : binding.Block->Members)
                    if (m.Name == memberName)
                        return &m;
    return nullptr;
}

} // namespace

TEST(ShaderUBOReflection, UniformBufferBlockLayout) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#else
    std::vector<uint8_t> fsBytes = Utils::LoadShaderFile("ubo_sample.frag.spv");
    ASSERT_FALSE(fsBytes.empty()) << "Missing ubo_sample.frag.spv";
    ASSERT_EQ(fsBytes.size() % 4, 0u);
    std::vector<uint32_t> fs(fsBytes.size()/4);
    std::memcpy(fs.data(), fsBytes.data(), fsBytes.size());

    ReflectionOptions opts{}; StageReflectionResult rfs{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, fs.data(), fs.size(), opts, rfs, &err)) << err;
    ShaderMeta meta = MergeStages({rfs});

    // Find set=0 binding=0, expect UBO with a block of at least 64 bytes (mat4)
    bool found = false; bool hasBlock=false; uint32_t blkSize=0;
    for (const auto& set : meta.Sets) {
        if (set.Set != 0) continue;
        for (const auto& b : set.Bindings) {
            // Accept any UBO binding in set 0 with a reflected block
            if (b.Type == ShaderMetaBindingType::kUniformBuffer) {
                found = true;
                hasBlock = b.Block.has_value();
                if (hasBlock) blkSize = b.Block->Size;
                break;
            }
        }
        if (found) break;
    }
    ASSERT_TRUE(found);
    ASSERT_TRUE(hasBlock);
    EXPECT_GE(blkSize, 64u);
#endif
}


// runtime_array_stride.comp declares array members whose stride is not the
// element size rounded up to 16, so the reflected value is provably the
// shader's ArrayStride decoration and not a derivation from Size.
TEST(ShaderUBOReflection, ArrayMembersCarryStrideDimsAndElementType) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#else
    ShaderMeta meta{};
    ASSERT_NO_FATAL_FAILURE(ReflectComputeShader("runtime_array_stride.comp.spv", meta));

    // std430 runtime array of a 20-byte struct: 32-byte rows, element type kept.
    const Member* rows = FindBlockMember(meta, "Rows", "rows");
    ASSERT_NE(rows, nullptr);
    ASSERT_TRUE(rows->ArrayStride.has_value());
    EXPECT_EQ(*rows->ArrayStride, 32u);
    EXPECT_EQ(rows->Size, 20u);
    EXPECT_EQ(rows->Type.Kind, TypeKind::Struct);
    EXPECT_EQ(rows->Type.ArrayDims, (std::vector<uint32_t>{kRuntimeArrayDim}));
    ASSERT_EQ(rows->Type.StructMembers.size(), 2u);
    EXPECT_EQ(rows->Type.StructMembers[0].Name, "a");
    EXPECT_EQ(rows->Type.StructMembers[0].Type.Kind, TypeKind::Vector);
    EXPECT_EQ(rows->Type.StructMembers[0].Type.VecSize, 4u);
    EXPECT_EQ(rows->Type.StructMembers[1].Name, "b");
    EXPECT_EQ(rows->Type.StructMembers[1].Type.Kind, TypeKind::Scalar);
    EXPECT_EQ(rows->Type.StructMembers[1].Type.Base, BaseType::Float);

    // std430 runtime array of float: 4-byte rows.
    const Member* weights = FindBlockMember(meta, "Weights", "weights");
    ASSERT_NE(weights, nullptr);
    ASSERT_TRUE(weights->ArrayStride.has_value());
    EXPECT_EQ(*weights->ArrayStride, 4u);
    EXPECT_EQ(weights->Type.Kind, TypeKind::Scalar);
    EXPECT_EQ(weights->Type.Base, BaseType::Float);
    EXPECT_EQ(weights->Type.ArrayDims, (std::vector<uint32_t>{kRuntimeArrayDim}));

    // Sized std430 array of the same struct: same stride, the count as its dimension.
    const Member* fixedRows = FindBlockMember(meta, "Fixed", "fixedRows");
    ASSERT_NE(fixedRows, nullptr);
    ASSERT_TRUE(fixedRows->ArrayStride.has_value());
    EXPECT_EQ(*fixedRows->ArrayStride, 32u);
    EXPECT_EQ(fixedRows->Size, 96u);
    EXPECT_EQ(fixedRows->Type.Kind, TypeKind::Struct);
    EXPECT_EQ(fixedRows->Type.ArrayDims, (std::vector<uint32_t>{3u}));

    // Sized std140 array of float: 16-byte rows.
    const Member* weights140 = FindBlockMember(meta, "Std140", "weights140");
    ASSERT_NE(weights140, nullptr);
    ASSERT_TRUE(weights140->ArrayStride.has_value());
    EXPECT_EQ(*weights140->ArrayStride, 16u);
    EXPECT_EQ(weights140->Size, 64u);
    EXPECT_EQ(weights140->Type.ArrayDims, (std::vector<uint32_t>{4u}));

    // A scalar member is no array and keeps its unsigned base type.
    const Member* count = FindBlockMember(meta, "Rows", "count");
    ASSERT_NE(count, nullptr);
    EXPECT_FALSE(count->ArrayStride.has_value());
    EXPECT_TRUE(count->Type.ArrayDims.empty());
    EXPECT_EQ(count->Type.Kind, TypeKind::Scalar);
    EXPECT_EQ(count->Type.Base, BaseType::UInt);
#endif
}
