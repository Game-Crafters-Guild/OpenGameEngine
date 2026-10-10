#pragma once

// Engine enum -> Metal enum translation tables. Header-only, included by the
// Metal backend TUs. Keep in sync with the engine enums in Device.h and the
// Vulkan tables in VulkanMappings.h.

#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"

#include <Metal/Metal.hpp>

namespace GameEngine::Rendering::MetalMappings
{

inline MTL::PixelFormat ToMTLPixelFormat(TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::RGBA8_UNORM:        return MTL::PixelFormatRGBA8Unorm;
    case TextureFormat::RGBA8_SRGB:         return MTL::PixelFormatRGBA8Unorm_sRGB;
    case TextureFormat::BGRA8_UNORM:        return MTL::PixelFormatBGRA8Unorm;
    case TextureFormat::BGRA8_SRGB:         return MTL::PixelFormatBGRA8Unorm_sRGB;
    case TextureFormat::R8_UINT:            return MTL::PixelFormatR8Uint;
    case TextureFormat::R8_SINT:            return MTL::PixelFormatR8Sint;
    case TextureFormat::R8G8_UINT:          return MTL::PixelFormatRG8Uint;
    case TextureFormat::R8G8_SINT:          return MTL::PixelFormatRG8Sint;
    case TextureFormat::RGBA8_UINT:         return MTL::PixelFormatRGBA8Uint;
    case TextureFormat::RGBA8_SINT:         return MTL::PixelFormatRGBA8Sint;
    case TextureFormat::R16_UINT:           return MTL::PixelFormatR16Uint;
    case TextureFormat::R16_SINT:           return MTL::PixelFormatR16Sint;
    case TextureFormat::R16G16_UINT:        return MTL::PixelFormatRG16Uint;
    case TextureFormat::R16G16_SINT:        return MTL::PixelFormatRG16Sint;
    case TextureFormat::RGBA16_UINT:        return MTL::PixelFormatRGBA16Uint;
    case TextureFormat::RGBA16_SINT:        return MTL::PixelFormatRGBA16Sint;
    case TextureFormat::R32_UINT:           return MTL::PixelFormatR32Uint;
    case TextureFormat::R32_SINT:           return MTL::PixelFormatR32Sint;
    case TextureFormat::R32G32_UINT:        return MTL::PixelFormatRG32Uint;
    case TextureFormat::R32G32_SINT:        return MTL::PixelFormatRG32Sint;
    // Metal has no RGB32 formats; callers must use RGBA32 textures.
    case TextureFormat::R32G32B32_UINT:     return MTL::PixelFormatInvalid;
    case TextureFormat::R32G32B32_SINT:     return MTL::PixelFormatInvalid;
    case TextureFormat::RGBA32_UINT:        return MTL::PixelFormatRGBA32Uint;
    case TextureFormat::RGBA32_SINT:        return MTL::PixelFormatRGBA32Sint;
    case TextureFormat::R32G32B32A32_FLOAT: return MTL::PixelFormatRGBA32Float;
    case TextureFormat::R16G16B16A16_FLOAT: return MTL::PixelFormatRGBA16Float;
    case TextureFormat::R16G16B16A16_UNORM: return MTL::PixelFormatRGBA16Unorm;
    case TextureFormat::R11G11B10_FLOAT:    return MTL::PixelFormatRG11B10Float;
    case TextureFormat::RGB10A2_UNORM:      return MTL::PixelFormatRGB10A2Unorm;
    case TextureFormat::R16_FLOAT:          return MTL::PixelFormatR16Float;
    case TextureFormat::R16G16_FLOAT:       return MTL::PixelFormatRG16Float;
    case TextureFormat::R32_FLOAT:          return MTL::PixelFormatR32Float;
    case TextureFormat::R32G32_FLOAT:       return MTL::PixelFormatRG32Float;
    case TextureFormat::R8_UNORM:           return MTL::PixelFormatR8Unorm;
    case TextureFormat::R8G8_UNORM:         return MTL::PixelFormatRG8Unorm;
    case TextureFormat::D32_FLOAT:          return MTL::PixelFormatDepth32Float;
    // Apple GPUs have no D24; promote to D32+S8 (size differs, callers use
    // IsTextureFormatSupported / preferredDepthAndStencilFormat to avoid it).
    case TextureFormat::D24_UNORM_S8_UINT:  return MTL::PixelFormatDepth32Float_Stencil8;
    case TextureFormat::D32_SFLOAT_S8_UINT: return MTL::PixelFormatDepth32Float_Stencil8;
    case TextureFormat::BC1_UNORM:          return MTL::PixelFormatBC1_RGBA;
    case TextureFormat::BC1_SRGB:           return MTL::PixelFormatBC1_RGBA_sRGB;
    case TextureFormat::BC3_UNORM:          return MTL::PixelFormatBC3_RGBA;
    case TextureFormat::BC4_UNORM:          return MTL::PixelFormatBC4_RUnorm;
    case TextureFormat::BC5_UNORM:          return MTL::PixelFormatBC5_RGUnorm;
    case TextureFormat::BC6H_UF16:          return MTL::PixelFormatBC6H_RGBUfloat;
    case TextureFormat::BC7_UNORM:          return MTL::PixelFormatBC7_RGBAUnorm;
    case TextureFormat::BC7_SRGB:           return MTL::PixelFormatBC7_RGBAUnorm_sRGB;
    case TextureFormat::D16_UNORM:          return MTL::PixelFormatDepth16Unorm;
    case TextureFormat::S8_UINT:            return MTL::PixelFormatStencil8;
    case TextureFormat::X8_D24_UNORM_PACK32: return MTL::PixelFormatDepth32Float;
    case TextureFormat::Unknown:
    default:                                return MTL::PixelFormatInvalid;
    }
}

inline MTL::VertexFormat ToMTLVertexFormat(Format format)
{
    switch (format)
    {
    case Format::R8G8B8A8_UNORM:     return MTL::VertexFormatUChar4Normalized;
    case Format::R16G16B16A16_FLOAT: return MTL::VertexFormatHalf4;
    case Format::R32G32B32A32_FLOAT: return MTL::VertexFormatFloat4;
    case Format::R32G32B32_FLOAT:    return MTL::VertexFormatFloat3;
    case Format::R32G32_FLOAT:       return MTL::VertexFormatFloat2;
    case Format::R16G16B16A16_UINT:  return MTL::VertexFormatUShort4;
    default:                         return MTL::VertexFormatInvalid;
    }
}

inline MTL::LoadAction ToMTLLoadAction(RenderPassDesc::LoadOp op)
{
    switch (op)
    {
    case RenderPassDesc::LoadOp::Load:     return MTL::LoadActionLoad;
    case RenderPassDesc::LoadOp::Clear:    return MTL::LoadActionClear;
    case RenderPassDesc::LoadOp::DontCare: return MTL::LoadActionDontCare;
    }
    return MTL::LoadActionDontCare;
}

inline MTL::StoreAction ToMTLStoreAction(RenderPassDesc::StoreOp op)
{
    switch (op)
    {
    case RenderPassDesc::StoreOp::Store:    return MTL::StoreActionStore;
    case RenderPassDesc::StoreOp::DontCare: return MTL::StoreActionDontCare;
    // Metal has no write-free store action; Store is the closest — it keeps the
    // contents, which is the half of None's contract that affects pixels. Metal
    // tracks hazards itself, so the write-freedom half costs nothing here.
    case RenderPassDesc::StoreOp::None:     return MTL::StoreActionStore;
    }
    return MTL::StoreActionDontCare;
}

inline MTL::CompareFunction ToMTLCompareFunction(CompareOp op)
{
    switch (op)
    {
    case CompareOp::Never:          return MTL::CompareFunctionNever;
    case CompareOp::Less:           return MTL::CompareFunctionLess;
    case CompareOp::Equal:          return MTL::CompareFunctionEqual;
    case CompareOp::LessOrEqual:    return MTL::CompareFunctionLessEqual;
    case CompareOp::Greater:        return MTL::CompareFunctionGreater;
    case CompareOp::NotEqual:       return MTL::CompareFunctionNotEqual;
    case CompareOp::GreaterOrEqual: return MTL::CompareFunctionGreaterEqual;
    case CompareOp::Always:         return MTL::CompareFunctionAlways;
    }
    return MTL::CompareFunctionAlways;
}

inline MTL::BlendFactor ToMTLBlendFactor(BlendFactor factor)
{
    switch (factor)
    {
    case BlendFactor::Zero:                  return MTL::BlendFactorZero;
    case BlendFactor::One:                   return MTL::BlendFactorOne;
    case BlendFactor::SrcColor:              return MTL::BlendFactorSourceColor;
    case BlendFactor::OneMinusSrcColor:      return MTL::BlendFactorOneMinusSourceColor;
    case BlendFactor::DstColor:              return MTL::BlendFactorDestinationColor;
    case BlendFactor::OneMinusDstColor:      return MTL::BlendFactorOneMinusDestinationColor;
    case BlendFactor::SrcAlpha:              return MTL::BlendFactorSourceAlpha;
    case BlendFactor::OneMinusSrcAlpha:      return MTL::BlendFactorOneMinusSourceAlpha;
    case BlendFactor::DstAlpha:              return MTL::BlendFactorDestinationAlpha;
    case BlendFactor::OneMinusDstAlpha:      return MTL::BlendFactorOneMinusDestinationAlpha;
    case BlendFactor::ConstantColor:         return MTL::BlendFactorBlendColor;
    case BlendFactor::OneMinusConstantColor: return MTL::BlendFactorOneMinusBlendColor;
    case BlendFactor::ConstantAlpha:         return MTL::BlendFactorBlendAlpha;
    case BlendFactor::OneMinusConstantAlpha: return MTL::BlendFactorOneMinusBlendAlpha;
    case BlendFactor::AlphaSaturate:         return MTL::BlendFactorSourceAlphaSaturated;
    case BlendFactor::Src1Color:             return MTL::BlendFactorSource1Color;
    case BlendFactor::OneMinusSrc1Color:     return MTL::BlendFactorOneMinusSource1Color;
    case BlendFactor::Src1Alpha:             return MTL::BlendFactorSource1Alpha;
    case BlendFactor::OneMinusSrc1Alpha:     return MTL::BlendFactorOneMinusSource1Alpha;
    }
    return MTL::BlendFactorOne;
}

inline MTL::BlendOperation ToMTLBlendOperation(BlendOp op)
{
    switch (op)
    {
    case BlendOp::Add:             return MTL::BlendOperationAdd;
    case BlendOp::Subtract:        return MTL::BlendOperationSubtract;
    case BlendOp::ReverseSubtract: return MTL::BlendOperationReverseSubtract;
    case BlendOp::Min:             return MTL::BlendOperationMin;
    case BlendOp::Max:             return MTL::BlendOperationMax;
    }
    return MTL::BlendOperationAdd;
}

inline MTL::PrimitiveType ToMTLPrimitiveType(PrimitiveTopology topology)
{
    switch (topology)
    {
    case PrimitiveTopology::PointList:     return MTL::PrimitiveTypePoint;
    case PrimitiveTopology::LineList:      return MTL::PrimitiveTypeLine;
    case PrimitiveTopology::LineStrip:     return MTL::PrimitiveTypeLineStrip;
    case PrimitiveTopology::TriangleList:  return MTL::PrimitiveTypeTriangle;
    case PrimitiveTopology::TriangleStrip: return MTL::PrimitiveTypeTriangleStrip;
    // Metal has no adjacency/patch primitive types in the render pipeline.
    default:                               return MTL::PrimitiveTypeTriangle;
    }
}

inline MTL::CullMode ToMTLCullMode(CullModeFlags cullMode)
{
    if (cullMode & CullModeFlagBits::Back)
    {
        return MTL::CullModeBack;
    }
    if (cullMode & CullModeFlagBits::Front)
    {
        return MTL::CullModeFront;
    }
    return MTL::CullModeNone;
}

inline MTL::Winding ToMTLWinding(FrontFace frontFace)
{
    // The engine renders Y-up via a negative-height Vulkan viewport. That
    // flip mirrors the framebuffer, which REVERSES the screen-space winding
    // Vulkan evaluates front faces in. Metal's NDC is natively Y-up (no
    // flip), so the same clip-space triangles arrive with the opposite
    // on-screen winding — the mapping must invert to keep culling parity.
    // (Verified empirically: with a 1:1 mapping every back-face-culled
    // pipeline rasterized only back faces.)
    return frontFace == FrontFace::CounterClockwise ? MTL::WindingClockwise
                                                    : MTL::WindingCounterClockwise;
}

inline MTL::TriangleFillMode ToMTLFillMode(PolygonMode mode)
{
    // Metal has no point fill mode; Line is the closest.
    return mode == PolygonMode::Fill ? MTL::TriangleFillModeFill : MTL::TriangleFillModeLines;
}

// Engine masks use the Vulkan bit layout (R=1,G=2,B=4,A=8); Metal reverses it
// (R=8,G=4,B=2,A=1).
inline MTL::ColorWriteMask ToMTLColorWriteMask(uint32_t mask)
{
    MTL::ColorWriteMask result = MTL::ColorWriteMaskNone;
    if (mask & 0x1) { result |= MTL::ColorWriteMaskRed; }
    if (mask & 0x2) { result |= MTL::ColorWriteMaskGreen; }
    if (mask & 0x4) { result |= MTL::ColorWriteMaskBlue; }
    if (mask & 0x8) { result |= MTL::ColorWriteMaskAlpha; }
    return result;
}

inline MTL::IndexType ToMTLIndexType(IndexType type)
{
    return type == IndexType::Uint16 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32;
}

inline MTL::SamplerMinMagFilter ToMTLMinMagFilter(uint32_t filter)
{
    return filter == 0 ? MTL::SamplerMinMagFilterNearest : MTL::SamplerMinMagFilterLinear;
}

inline MTL::SamplerMipFilter ToMTLMipFilter(uint32_t filter)
{
    return filter == 0 ? MTL::SamplerMipFilterNearest : MTL::SamplerMipFilterLinear;
}

// SamplerDesc address modes follow the Vulkan numeric convention:
// 0 = repeat, 1 = mirrored repeat, 2 = clamp to edge, 3 = clamp to border.
inline MTL::SamplerAddressMode ToMTLAddressMode(uint32_t mode)
{
    switch (mode)
    {
    case 0:  return MTL::SamplerAddressModeRepeat;
    case 1:  return MTL::SamplerAddressModeMirrorRepeat;
    case 2:  return MTL::SamplerAddressModeClampToEdge;
    case 3:  return MTL::SamplerAddressModeClampToBorderColor;
    default: return MTL::SamplerAddressModeRepeat;
    }
}

} // namespace GameEngine::Rendering::MetalMappings
