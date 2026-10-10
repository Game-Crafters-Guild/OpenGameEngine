#pragma once

#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include <vulkan/vulkan.h>

namespace GameEngine
{
namespace Rendering
{

namespace VulkanMappings
{

// Translate engine-level PipelineStageMask to Vulkan flags
inline VkPipelineStageFlags TranslatePipelineStageMask(uint64_t maskVal)
{
    if (maskVal == 0)
        return static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

    VkPipelineStageFlags flags = 0;
    using StageMask = PipelineStageMask;

    // Note: Use explicit casts to check bits against the engine-level uint64_t
    auto has = [&](StageMask bit)
    { return (maskVal & static_cast<uint64_t>(bit)) != 0; };

    if (has(StageMask::GraphicsColor))
        flags |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (has(StageMask::GraphicsDepth))
        flags |= (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    if (has(StageMask::GraphicsVertex))
        flags |= (VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
    if (has(StageMask::GraphicsFragment))
        flags |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (has(StageMask::ComputeShader))
        flags |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (has(StageMask::Transfer))
        flags |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (has(StageMask::DrawIndirect))
        flags |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;

    return flags != 0
               ? flags
               : static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}

// Translate engine-level ResourceAccessMask to Vulkan flags
inline VkAccessFlags TranslateResourceAccessMask(uint64_t maskVal)
{
    if (maskVal == 0)
        return 0;

    VkAccessFlags flags = 0;
    using AccessMask = ResourceAccessMask;

    auto has = [&](AccessMask bit)
    { return (maskVal & static_cast<uint64_t>(bit)) != 0; };

    if (has(AccessMask::ColorAttachmentRead))
        flags |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    if (has(AccessMask::ColorAttachmentWrite))
        flags |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (has(AccessMask::DepthStencilRead))
        flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (has(AccessMask::DepthStencilWrite))
        flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (has(AccessMask::ShaderRead))
        flags |= VK_ACCESS_SHADER_READ_BIT;
    if (has(AccessMask::ShaderWrite))
        flags |= VK_ACCESS_SHADER_WRITE_BIT;
    if (has(AccessMask::VertexAttributeRead))
        flags |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    if (has(AccessMask::TransferRead))
        flags |= VK_ACCESS_TRANSFER_READ_BIT;
    if (has(AccessMask::TransferWrite))
        flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
    if (has(AccessMask::IndirectCommandRead))
        flags |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;

    return flags;
}

// The image layout a resource in this state is bound in.
inline VkImageLayout TranslateResourceStateToLayout(ResourceState state)
{
    switch (state)
    {
    case ResourceState::Undefined:
        return VK_IMAGE_LAYOUT_UNDEFINED;
    case ResourceState::RenderTarget:
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case ResourceState::DepthWrite:
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case ResourceState::DepthRead:
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    case ResourceState::DepthSampled:
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    case ResourceState::ShaderResource:
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case ResourceState::CopySource:
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case ResourceState::CopyDest:
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    default:
        return VK_IMAGE_LAYOUT_GENERAL;
    }
}

// The accesses a resource in this state is subject to. Paired with
// TranslateResourceStateToStage to derive a barrier's scopes when the caller
// supplies no explicit masks.
inline VkAccessFlags TranslateResourceStateToAccess(ResourceState state)
{
    switch (state)
    {
    case ResourceState::Undefined:
        return 0;
    case ResourceState::RenderTarget:
        return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case ResourceState::DepthWrite:
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case ResourceState::DepthRead:
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    case ResourceState::DepthSampled:
        return VK_ACCESS_SHADER_READ_BIT;
    case ResourceState::ShaderResource:
        return VK_ACCESS_SHADER_READ_BIT;
    case ResourceState::CopySource:
        return VK_ACCESS_TRANSFER_READ_BIT;
    case ResourceState::CopyDest:
        return VK_ACCESS_TRANSFER_WRITE_BIT;
    default:
        return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
}

// The stages at which a resource in this state is accessed.
//
// CONVENTION TRAP: depth attachment access spans BOTH fragment-test stages. The
// depth test and its early write happen at EARLY_FRAGMENT_TESTS, but the
// attachment STORE happens at LATE_FRAGMENT_TESTS. Naming only EARLY leaves a
// barrier that does not wait for the store, so the following layout transition
// is a WRITE_AFTER_WRITE hazard against a write nothing synchronised. This must
// stay equal to TranslatePipelineStageMask(GraphicsDepth), which is the same
// scope reached via explicit masks.
//
// DepthSampled and ShaderResource share one scope on purpose: they are a single
// state — "a shader samples this image" — that RGRecord splits by FORMAT alone,
// so a depth image transitions to the layout its descriptor claims. Format says
// nothing about WHICH stage samples the image, and depth images are sampled from
// compute (the cascade array in volumetric_fog_light.comp) as well as from
// fragment shaders.
inline VkPipelineStageFlags TranslateResourceStateToStage(ResourceState state)
{
    switch (state)
    {
    case ResourceState::Undefined:
        return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    case ResourceState::RenderTarget:
        return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case ResourceState::DepthWrite:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    case ResourceState::DepthRead:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    case ResourceState::DepthSampled:
    case ResourceState::ShaderResource:
        return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case ResourceState::CopySource:
        return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case ResourceState::CopyDest:
        return VK_PIPELINE_STAGE_TRANSFER_BIT;
    default:
        return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
}

// Map engine ResolveMode to Vulkan flag bit
inline VkResolveModeFlagBits TranslateResolveModeToVulkan(RenderPassDesc::ResolveMode mode)
{
    switch (mode)
    {
    case RenderPassDesc::ResolveMode::Average:
        return VK_RESOLVE_MODE_AVERAGE_BIT;
    case RenderPassDesc::ResolveMode::SampleZero:
        return VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    case RenderPassDesc::ResolveMode::Min:
        return VK_RESOLVE_MODE_MIN_BIT;
    case RenderPassDesc::ResolveMode::Max:
        return VK_RESOLVE_MODE_MAX_BIT;
    }
    return VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
}

// Map Vulkan resolve flags to engine capability bits (for IDevice::GetCapabilities)
inline uint32_t TranslateVulkanResolveModesToEngine(VkResolveModeFlags vkFlags)
{
    uint32_t engineFlags = 0;
    if (vkFlags & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)
        engineFlags |= ResolveModeToBit(RenderPassDesc::ResolveMode::SampleZero);
    if (vkFlags & VK_RESOLVE_MODE_AVERAGE_BIT)
        engineFlags |= ResolveModeToBit(RenderPassDesc::ResolveMode::Average);
    if (vkFlags & VK_RESOLVE_MODE_MIN_BIT)
        engineFlags |= ResolveModeToBit(RenderPassDesc::ResolveMode::Min);
    if (vkFlags & VK_RESOLVE_MODE_MAX_BIT)
        engineFlags |= ResolveModeToBit(RenderPassDesc::ResolveMode::Max);
    return engineFlags;
}

// Helper to check if a Vulkan capability flag matches an engine-level request (which is mapped to engine bits)
// This converts the Vulkan bit (e.g. VK_RESOLVE_MODE_AVERAGE_BIT) to its corresponding engine bit,
// then checks if that bit is present in the device's engine-level capability mask.
inline bool CheckResolveModeSupport(const RenderingDeviceCapabilities& caps, VkResolveModeFlagBits mode)
{
    uint32_t requiredEngineBit = 0;
    // Note: We use the engine bit conversion logic manually here because TranslateVulkanResolveModesToEngine takes a mask, not a single bit.
    if (mode == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)
        requiredEngineBit = ResolveModeToBit(RenderPassDesc::ResolveMode::SampleZero);
    else if (mode == VK_RESOLVE_MODE_AVERAGE_BIT)
        requiredEngineBit = ResolveModeToBit(RenderPassDesc::ResolveMode::Average);
    else if (mode == VK_RESOLVE_MODE_MIN_BIT)
        requiredEngineBit = ResolveModeToBit(RenderPassDesc::ResolveMode::Min);
    else if (mode == VK_RESOLVE_MODE_MAX_BIT)
        requiredEngineBit = ResolveModeToBit(RenderPassDesc::ResolveMode::Max);

    // Resolve mode NONE or unknown mode: treat as unsupported here and let callers
    // handle special cases explicitly.
    if (requiredEngineBit == 0)
        return false;

    // Check against both depth and stencil capabilities. Callers that care about a
    // specific aspect should use the more specific helpers below.
    uint32_t unionCaps = caps.supportedDepthResolveModes | caps.supportedStencilResolveModes;
    return (unionCaps & requiredEngineBit) != 0;
}

inline bool CheckDepthResolveSupport(const RenderingDeviceCapabilities& caps, VkResolveModeFlagBits mode)
{
    uint32_t requiredEngineBit = TranslateVulkanResolveModesToEngine(mode);
    return (caps.supportedDepthResolveModes & requiredEngineBit) != 0;
}

inline bool CheckStencilResolveSupport(const RenderingDeviceCapabilities& caps, VkResolveModeFlagBits mode)
{
    uint32_t requiredEngineBit = TranslateVulkanResolveModesToEngine(mode);
    return (caps.supportedStencilResolveModes & requiredEngineBit) != 0;
}

// Pipeline state conversions
inline VkCullModeFlags TranslateCullMode(CullModeFlags cullMode)
{
    VkCullModeFlags flags = VK_CULL_MODE_NONE;
    if (cullMode & CullModeFlagBits::Front)
        flags |= VK_CULL_MODE_FRONT_BIT;
    if (cullMode & CullModeFlagBits::Back)
        flags |= VK_CULL_MODE_BACK_BIT;
    return flags;
}

inline VkFrontFace TranslateFrontFace(FrontFace frontFace)
{
    return (frontFace == FrontFace::Clockwise) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

inline VkPolygonMode TranslatePolygonMode(PolygonMode polygonMode)
{
    switch (polygonMode)
    {
    case PolygonMode::Fill:
        return VK_POLYGON_MODE_FILL;
    case PolygonMode::Line:
        return VK_POLYGON_MODE_LINE;
    case PolygonMode::Point:
        return VK_POLYGON_MODE_POINT;
    default:
        return VK_POLYGON_MODE_FILL;
    }
}

inline VkBlendFactor TranslateBlendFactor(BlendFactor f)
{
    switch (f)
    {
    case BlendFactor::Zero:
        return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One:
        return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcColor:
        return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::DstColor:
        return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::OneMinusDstColor:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::SrcAlpha:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::ConstantColor:
        return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case BlendFactor::OneMinusConstantColor:
        return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case BlendFactor::ConstantAlpha:
        return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case BlendFactor::OneMinusConstantAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case BlendFactor::AlphaSaturate:
        return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case BlendFactor::Src1Color:
        return VK_BLEND_FACTOR_SRC1_COLOR;
    case BlendFactor::OneMinusSrc1Color:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case BlendFactor::Src1Alpha:
        return VK_BLEND_FACTOR_SRC1_ALPHA;
    case BlendFactor::OneMinusSrc1Alpha:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default:
        return VK_BLEND_FACTOR_ZERO;
    }
}

inline VkBlendOp TranslateBlendOp(BlendOp op)
{
    switch (op)
    {
    case BlendOp::Add:
        return VK_BLEND_OP_ADD;
    case BlendOp::Subtract:
        return VK_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract:
        return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::Min:
        return VK_BLEND_OP_MIN;
    case BlendOp::Max:
        return VK_BLEND_OP_MAX;
    default:
        return VK_BLEND_OP_ADD;
    }
}

inline VkCompareOp TranslateCompareOp(CompareOp compareOp)
{
    switch (compareOp)
    {
    case CompareOp::Never:
        return VK_COMPARE_OP_NEVER;
    case CompareOp::Less:
        return VK_COMPARE_OP_LESS;
    case CompareOp::Equal:
        return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessOrEqual:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater:
        return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual:
        return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterOrEqual:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always:
        return VK_COMPARE_OP_ALWAYS;
    default:
        return VK_COMPARE_OP_ALWAYS;
    }
}

inline VkComponentSwizzle TranslateSwizzle(TextureSwizzle s)
{
    switch (s)
    {
    case TextureSwizzle::Identity:
        return VK_COMPONENT_SWIZZLE_IDENTITY;
    case TextureSwizzle::Zero:
        return VK_COMPONENT_SWIZZLE_ZERO;
    case TextureSwizzle::One:
        return VK_COMPONENT_SWIZZLE_ONE;
    case TextureSwizzle::R:
        return VK_COMPONENT_SWIZZLE_R;
    case TextureSwizzle::G:
        return VK_COMPONENT_SWIZZLE_G;
    case TextureSwizzle::B:
        return VK_COMPONENT_SWIZZLE_B;
    case TextureSwizzle::A:
        return VK_COMPONENT_SWIZZLE_A;
    default:
        return VK_COMPONENT_SWIZZLE_IDENTITY;
    }
}

inline VkImageAspectFlags TranslateAspect(TextureAspect a)
{
    VkImageAspectFlags flags = 0;
    if ((uint32_t)a & (uint32_t)TextureAspect::Color)
        flags |= VK_IMAGE_ASPECT_COLOR_BIT;
    if ((uint32_t)a & (uint32_t)TextureAspect::Depth)
        flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((uint32_t)a & (uint32_t)TextureAspect::Stencil)
        flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return flags;
}

} // namespace VulkanMappings

} // namespace Rendering
} // namespace GameEngine
