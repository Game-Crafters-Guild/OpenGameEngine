#include <gtest/gtest.h>

#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Vulkan/VulkanMappings.h"

using namespace GameEngine::Rendering;

namespace
{
constexpr VkPipelineStageFlags kBothFragmentTestStages =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
}

// A depth attachment is written at BOTH fragment-test stages: the depth test and
// its early write land at EARLY_FRAGMENT_TESTS, but the attachment STORE lands at
// LATE_FRAGMENT_TESTS. A barrier that names only EARLY therefore does not wait for
// the store, and transitioning the image afterwards is a WRITE_AFTER_WRITE hazard
// against a write the barrier never covered.
//
// This is the mapping BarrierBatch uses whenever a caller supplies no explicit
// masks (ResourceBarrier::CreateTextureBarrier leaves them 0), which is every
// hand-built barrier outside the render graph.
TEST(VulkanResourceStateMapping, DepthWriteCoversLateFragmentTests)
{
    const VkPipelineStageFlags stage =
        VulkanMappings::TranslateResourceStateToStage(ResourceState::DepthWrite);

    EXPECT_TRUE(stage & VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
        << "DepthWrite omits LATE_FRAGMENT_TESTS, so a barrier out of a depth "
           "attachment does not wait for the attachment store: WRITE_AFTER_WRITE";
    EXPECT_TRUE(stage & VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT)
        << "DepthWrite must still cover the early-fragment-test write";
    EXPECT_EQ(stage & kBothFragmentTestStages, kBothFragmentTestStages);
}

// Depth testing reads the attachment at both fragment-test stages for the same
// reason, so a read-scope barrier that names only EARLY under-covers it too.
TEST(VulkanResourceStateMapping, DepthReadCoversLateFragmentTests)
{
    const VkPipelineStageFlags stage =
        VulkanMappings::TranslateResourceStateToStage(ResourceState::DepthRead);

    EXPECT_EQ(stage & kBothFragmentTestStages, kBothFragmentTestStages)
        << "DepthRead must cover both fragment-test stages";
}

// The two ways a barrier's stages get derived must agree. The render graph sets
// explicit masks (PipelineStageMask::GraphicsDepth -> TranslatePipelineStageMask);
// every hand-built barrier derives them from ResourceState. GraphicsDepth is a
// single engine-level bit meaning "both fragment-test stages", so a ResourceState
// translation that resolves to less than that is a divergence, and a hazard the
// render-graph path would not have.
TEST(VulkanResourceStateMapping, StateDerivedDepthStagesMatchTheExplicitMaskPath)
{
    const VkPipelineStageFlags explicitPath = VulkanMappings::TranslatePipelineStageMask(
        static_cast<uint64_t>(PipelineStageMask::GraphicsDepth));

    EXPECT_EQ(VulkanMappings::TranslateResourceStateToStage(ResourceState::DepthWrite), explicitPath)
        << "DepthWrite's derived stages diverge from GraphicsDepth";
    EXPECT_EQ(VulkanMappings::TranslateResourceStateToStage(ResourceState::DepthRead), explicitPath)
        << "DepthRead's derived stages diverge from GraphicsDepth";
}

// Pins the rest of the depth triple so a future edit cannot quietly widen the
// stage fix into an access or layout change.
TEST(VulkanResourceStateMapping, DepthStatesKeepTheirAccessAndLayout)
{
    EXPECT_EQ(VulkanMappings::TranslateResourceStateToAccess(ResourceState::DepthWrite),
              static_cast<VkAccessFlags>(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));
    EXPECT_EQ(VulkanMappings::TranslateResourceStateToLayout(ResourceState::DepthWrite),
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    EXPECT_EQ(VulkanMappings::TranslateResourceStateToAccess(ResourceState::DepthRead),
              static_cast<VkAccessFlags>(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT));
    EXPECT_EQ(VulkanMappings::TranslateResourceStateToLayout(ResourceState::DepthRead),
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    // The state the cascade fallback ends in: sampled through a comparison sampler.
    EXPECT_EQ(VulkanMappings::TranslateResourceStateToAccess(ResourceState::DepthSampled),
              static_cast<VkAccessFlags>(VK_ACCESS_SHADER_READ_BIT));
    EXPECT_EQ(VulkanMappings::TranslateResourceStateToLayout(ResourceState::DepthSampled),
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
}

// DepthSampled and ShaderResource are the same state — "a shader samples this
// image" — split only by format: RGRecord's textureState() substitutes
// DepthSampled for ShaderResource when the resource carries a depth format, so
// that the transition names the layout the descriptor writer binds depth views
// in. Nothing about that substitution narrows WHICH shader stage samples the
// image, so the two must resolve to the same stage scope. A depth image whose
// scope is fragment-only silently loses the compute consumers the color state
// covers.
TEST(VulkanResourceStateMapping, DepthSampledStagesMatchShaderResource)
{
    EXPECT_EQ(VulkanMappings::TranslateResourceStateToStage(ResourceState::DepthSampled),
              VulkanMappings::TranslateResourceStateToStage(ResourceState::ShaderResource))
        << "DepthSampled's stages diverge from ShaderResource, so a format change "
           "alone narrows a barrier's visibility scope";
}

// The cascade shadow array is sampled from COMPUTE — volumetric_fog_light.comp
// binds it as ge_shadowMapArray and the fog's lighting dispatch reads it. The
// 1x1 cascade fallback that stands in for it reaches DepthSampled through a
// hand-built barrier (RenderServices' ClearDepthArrayToFar), which supplies no
// explicit masks and therefore derives its scopes from this table. A
// fragment-only scope makes that fill visible to fragment shaders alone and
// leaves the dispatch reading data no barrier made visible to it.
TEST(VulkanResourceStateMapping, DepthSampledCoversTheComputeStage)
{
    const VkPipelineStageFlags stage =
        VulkanMappings::TranslateResourceStateToStage(ResourceState::DepthSampled);

    EXPECT_TRUE(stage & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
        << "DepthSampled omits COMPUTE_SHADER: a depth image transitioned to it by a "
           "hand-built barrier is not visible to a compute dispatch that samples it";
    EXPECT_TRUE(stage & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
        << "DepthSampled must still cover the fragment consumers (world lighting, "
           "the shadow debug overlay, the ocean god-ray pass)";
}

// Undefined is a discard: nothing to wait for, nothing to make available.
TEST(VulkanResourceStateMapping, UndefinedIsADiscard)
{
    EXPECT_EQ(VulkanMappings::TranslateResourceStateToStage(ResourceState::Undefined),
              static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT));
    EXPECT_EQ(VulkanMappings::TranslateResourceStateToAccess(ResourceState::Undefined),
              static_cast<VkAccessFlags>(0));
    EXPECT_EQ(VulkanMappings::TranslateResourceStateToLayout(ResourceState::Undefined),
              VK_IMAGE_LAYOUT_UNDEFINED);
}
