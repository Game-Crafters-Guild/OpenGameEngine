#include <gtest/gtest.h>
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Core/Device.h"

#include <iterator>
#include <string>
#include <utility>


using namespace GameEngine::Rendering;

// Every ShaderMetaBindingType must map to its own DescriptorType. The mapper's
// `default` returns UniformBuffer, so a missing case does not fail loudly — it
// binds a TLAS (or a storage image) as a uniform buffer and the GPU reads
// nonsense. Pin the whole table rather than only the type that motivated it.
TEST(MaterialBuilder, BindingTypesMapToDistinctDescriptorTypes) {
    const std::pair<uint32_t, DescriptorType> kExpected[] = {
        {ShaderMetaBindingType::kUniformBuffer,        DescriptorType::UniformBuffer},
        {ShaderMetaBindingType::kStorageBuffer,        DescriptorType::StorageBuffer},
        {ShaderMetaBindingType::kSampler,              DescriptorType::Sampler},
        {ShaderMetaBindingType::kSampledImage,         DescriptorType::Texture},
        {ShaderMetaBindingType::kStorageImage,         DescriptorType::StorageImage},
        {ShaderMetaBindingType::kCombinedImageSampler, DescriptorType::CombinedImageSampler},
        {ShaderMetaBindingType::kAccelerationStructure, DescriptorType::AccelerationStructure},
    };

    ShaderMeta m{};
    DescriptorSetMeta s0{};
    s0.Set = 0;
    for (uint32_t i = 0; i < std::size(kExpected); ++i) {
        DescriptorBindingMeta b{};
        b.Binding = i;
        b.Name = "b" + std::to_string(i);
        b.Type = kExpected[i].first;
        b.Count = 1;
        b.StagesMask = (1 << 2); // compute
        s0.Bindings.push_back(b);
    }
    m.Sets.push_back(s0);

    auto layouts = MaterialBuilder::BuildSetLayouts(m);
    ASSERT_EQ(layouts.size(), 1u);
    ASSERT_EQ(layouts[0].bindings.size(), std::size(kExpected));
    for (uint32_t i = 0; i < std::size(kExpected); ++i) {
        EXPECT_EQ(layouts[0].bindings[i].type, kExpected[i].second)
            << "ShaderMetaBindingType " << kExpected[i].first << " mapped wrong";
    }
}

TEST(MaterialBuilder, BuildLayoutsAndPushConstants) {
    ShaderMeta m{};
    // Set 0: UBO binding 0, used in VS|FS
    DescriptorBindingMeta b0{}; b0.Binding = 0; b0.Name = "PerFrame"; b0.Type = 0; b0.Count = 1; b0.StagesMask = (1<<0) | (1<<1);
    DescriptorSetMeta s0{}; s0.Set = 0; s0.Bindings = { b0 };
    m.Sets.push_back(s0);

    // Push constants: 48 bytes, VS|FS
    PushConstantRangeMeta p{}; p.Name = "Globals"; p.Size = 48; p.StagesMask = (1<<0) | (1<<1);
    m.PushConstants.push_back(p);

    auto layouts = MaterialBuilder::BuildSetLayouts(m);
    ASSERT_EQ(layouts.size(), 1u);
    ASSERT_EQ(layouts[0].bindings.size(), 1u);
    EXPECT_EQ(layouts[0].bindings[0].binding, 0u);
    EXPECT_EQ(layouts[0].bindings[0].count, 1u);
    EXPECT_EQ(layouts[0].bindings[0].shaderStages, (uint32_t)(kShaderStageVertex | kShaderStageFragment));

    // Push constant size policy
    EXPECT_EQ(MaterialBuilder::ComputeTotalPushConstantSizeOrThrow(m), 48u);

    // Exceed policy
    m.PushConstants[0].Size = 256;
    EXPECT_THROW(MaterialBuilder::ComputeTotalPushConstantSizeOrThrow(m), std::runtime_error);
}

