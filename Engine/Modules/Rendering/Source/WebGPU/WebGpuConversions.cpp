#include "WebGpuConversions.h"

#include "Logger/Logger.h"

#include <cstring>

namespace GameEngine::Rendering::WebGpu
{

std::string ToString(WGPUStringView view)
{
    if (view.data == nullptr)
    {
        return {};
    }
    const size_t length = (view.length == WGPU_STRLEN) ? std::strlen(view.data) : view.length;
    return std::string(view.data, length);
}

WGPUTextureFormat ToWgpuTextureFormat(TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::RGBA8_UNORM:          return WGPUTextureFormat_RGBA8Unorm;
    case TextureFormat::RGBA8_SRGB:           return WGPUTextureFormat_RGBA8UnormSrgb;
    case TextureFormat::BGRA8_UNORM:          return WGPUTextureFormat_BGRA8Unorm;
    case TextureFormat::BGRA8_SRGB:           return WGPUTextureFormat_BGRA8UnormSrgb;
    case TextureFormat::R8_UINT:              return WGPUTextureFormat_R8Uint;
    case TextureFormat::R8_SINT:              return WGPUTextureFormat_R8Sint;
    case TextureFormat::R8G8_UINT:            return WGPUTextureFormat_RG8Uint;
    case TextureFormat::R8G8_SINT:            return WGPUTextureFormat_RG8Sint;
    case TextureFormat::RGBA8_UINT:           return WGPUTextureFormat_RGBA8Uint;
    case TextureFormat::RGBA8_SINT:           return WGPUTextureFormat_RGBA8Sint;
    case TextureFormat::R16_UINT:             return WGPUTextureFormat_R16Uint;
    case TextureFormat::R16_SINT:             return WGPUTextureFormat_R16Sint;
    case TextureFormat::R16G16_UINT:          return WGPUTextureFormat_RG16Uint;
    case TextureFormat::R16G16_SINT:          return WGPUTextureFormat_RG16Sint;
    case TextureFormat::RGBA16_UINT:          return WGPUTextureFormat_RGBA16Uint;
    case TextureFormat::RGBA16_SINT:          return WGPUTextureFormat_RGBA16Sint;
    case TextureFormat::R32_UINT:             return WGPUTextureFormat_R32Uint;
    case TextureFormat::R32_SINT:             return WGPUTextureFormat_R32Sint;
    case TextureFormat::R32G32_UINT:          return WGPUTextureFormat_RG32Uint;
    case TextureFormat::R32G32_SINT:          return WGPUTextureFormat_RG32Sint;
    case TextureFormat::RGBA32_UINT:          return WGPUTextureFormat_RGBA32Uint;
    case TextureFormat::RGBA32_SINT:          return WGPUTextureFormat_RGBA32Sint;
    case TextureFormat::R32G32B32A32_FLOAT:   return WGPUTextureFormat_RGBA32Float;
    case TextureFormat::R16G16B16A16_FLOAT:   return WGPUTextureFormat_RGBA16Float;
    case TextureFormat::R16G16B16A16_UNORM:   return WGPUTextureFormat_RGBA16Unorm;
    case TextureFormat::R11G11B10_FLOAT:      return WGPUTextureFormat_RG11B10Ufloat;
    case TextureFormat::RGB10A2_UNORM:        return WGPUTextureFormat_RGB10A2Unorm;
    case TextureFormat::R16_FLOAT:            return WGPUTextureFormat_R16Float;
    case TextureFormat::R16G16_FLOAT:         return WGPUTextureFormat_RG16Float;
    case TextureFormat::R32_FLOAT:            return WGPUTextureFormat_R32Float;
    case TextureFormat::R32G32_FLOAT:         return WGPUTextureFormat_RG32Float;
    case TextureFormat::R8_UNORM:             return WGPUTextureFormat_R8Unorm;
    case TextureFormat::R8G8_UNORM:           return WGPUTextureFormat_RG8Unorm;
    case TextureFormat::D32_FLOAT:            return WGPUTextureFormat_Depth32Float;
    case TextureFormat::D24_UNORM_S8_UINT:    return WGPUTextureFormat_Depth24PlusStencil8;
    case TextureFormat::D32_SFLOAT_S8_UINT:   return WGPUTextureFormat_Depth32FloatStencil8;
    case TextureFormat::D16_UNORM:            return WGPUTextureFormat_Depth16Unorm;
    case TextureFormat::S8_UINT:              return WGPUTextureFormat_Stencil8;
    case TextureFormat::X8_D24_UNORM_PACK32:  return WGPUTextureFormat_Depth24Plus;
    case TextureFormat::BC1_UNORM:            return WGPUTextureFormat_BC1RGBAUnorm;
    case TextureFormat::BC1_SRGB:             return WGPUTextureFormat_BC1RGBAUnormSrgb;
    case TextureFormat::BC3_UNORM:            return WGPUTextureFormat_BC3RGBAUnorm;
    case TextureFormat::BC4_UNORM:            return WGPUTextureFormat_BC4RUnorm;
    case TextureFormat::BC5_UNORM:            return WGPUTextureFormat_BC5RGUnorm;
    case TextureFormat::BC6H_UF16:            return WGPUTextureFormat_BC6HRGBUfloat;
    case TextureFormat::BC7_UNORM:            return WGPUTextureFormat_BC7RGBAUnorm;
    case TextureFormat::BC7_SRGB:             return WGPUTextureFormat_BC7RGBAUnormSrgb;
    // WebGPU has no three-component texture formats.
    case TextureFormat::R32G32B32_UINT:
    case TextureFormat::R32G32B32_SINT:
    case TextureFormat::Unknown:
    default:
        return WGPUTextureFormat_Undefined;
    }
}

TextureFormat FromWgpuTextureFormat(WGPUTextureFormat format)
{
    switch (format)
    {
    case WGPUTextureFormat_RGBA8Unorm:            return TextureFormat::RGBA8_UNORM;
    case WGPUTextureFormat_RGBA8UnormSrgb:        return TextureFormat::RGBA8_SRGB;
    case WGPUTextureFormat_BGRA8Unorm:            return TextureFormat::BGRA8_UNORM;
    case WGPUTextureFormat_BGRA8UnormSrgb:        return TextureFormat::BGRA8_SRGB;
    case WGPUTextureFormat_R8Uint:                return TextureFormat::R8_UINT;
    case WGPUTextureFormat_R8Sint:                return TextureFormat::R8_SINT;
    case WGPUTextureFormat_RG8Uint:               return TextureFormat::R8G8_UINT;
    case WGPUTextureFormat_RG8Sint:               return TextureFormat::R8G8_SINT;
    case WGPUTextureFormat_RGBA8Uint:             return TextureFormat::RGBA8_UINT;
    case WGPUTextureFormat_RGBA8Sint:             return TextureFormat::RGBA8_SINT;
    case WGPUTextureFormat_R16Uint:               return TextureFormat::R16_UINT;
    case WGPUTextureFormat_R16Sint:               return TextureFormat::R16_SINT;
    case WGPUTextureFormat_RG16Uint:              return TextureFormat::R16G16_UINT;
    case WGPUTextureFormat_RG16Sint:              return TextureFormat::R16G16_SINT;
    case WGPUTextureFormat_RGBA16Uint:            return TextureFormat::RGBA16_UINT;
    case WGPUTextureFormat_RGBA16Sint:            return TextureFormat::RGBA16_SINT;
    case WGPUTextureFormat_R32Uint:               return TextureFormat::R32_UINT;
    case WGPUTextureFormat_R32Sint:               return TextureFormat::R32_SINT;
    case WGPUTextureFormat_RG32Uint:              return TextureFormat::R32G32_UINT;
    case WGPUTextureFormat_RG32Sint:              return TextureFormat::R32G32_SINT;
    case WGPUTextureFormat_RGBA32Uint:            return TextureFormat::RGBA32_UINT;
    case WGPUTextureFormat_RGBA32Sint:            return TextureFormat::RGBA32_SINT;
    case WGPUTextureFormat_RGBA32Float:           return TextureFormat::R32G32B32A32_FLOAT;
    case WGPUTextureFormat_RGBA16Float:           return TextureFormat::R16G16B16A16_FLOAT;
    case WGPUTextureFormat_RGBA16Unorm:           return TextureFormat::R16G16B16A16_UNORM;
    case WGPUTextureFormat_RG11B10Ufloat:         return TextureFormat::R11G11B10_FLOAT;
    case WGPUTextureFormat_RGB10A2Unorm:          return TextureFormat::RGB10A2_UNORM;
    case WGPUTextureFormat_R16Float:              return TextureFormat::R16_FLOAT;
    case WGPUTextureFormat_RG16Float:             return TextureFormat::R16G16_FLOAT;
    case WGPUTextureFormat_R32Float:              return TextureFormat::R32_FLOAT;
    case WGPUTextureFormat_RG32Float:             return TextureFormat::R32G32_FLOAT;
    case WGPUTextureFormat_R8Unorm:               return TextureFormat::R8_UNORM;
    case WGPUTextureFormat_RG8Unorm:              return TextureFormat::R8G8_UNORM;
    case WGPUTextureFormat_Depth32Float:          return TextureFormat::D32_FLOAT;
    case WGPUTextureFormat_Depth24PlusStencil8:   return TextureFormat::D24_UNORM_S8_UINT;
    case WGPUTextureFormat_Depth32FloatStencil8:  return TextureFormat::D32_SFLOAT_S8_UINT;
    case WGPUTextureFormat_Depth16Unorm:          return TextureFormat::D16_UNORM;
    case WGPUTextureFormat_Stencil8:              return TextureFormat::S8_UINT;
    case WGPUTextureFormat_Depth24Plus:           return TextureFormat::X8_D24_UNORM_PACK32;
    case WGPUTextureFormat_BC1RGBAUnorm:          return TextureFormat::BC1_UNORM;
    case WGPUTextureFormat_BC1RGBAUnormSrgb:      return TextureFormat::BC1_SRGB;
    case WGPUTextureFormat_BC3RGBAUnorm:          return TextureFormat::BC3_UNORM;
    case WGPUTextureFormat_BC4RUnorm:             return TextureFormat::BC4_UNORM;
    case WGPUTextureFormat_BC5RGUnorm:            return TextureFormat::BC5_UNORM;
    case WGPUTextureFormat_BC6HRGBUfloat:         return TextureFormat::BC6H_UF16;
    case WGPUTextureFormat_BC7RGBAUnorm:          return TextureFormat::BC7_UNORM;
    case WGPUTextureFormat_BC7RGBAUnormSrgb:      return TextureFormat::BC7_SRGB;
    default:                                      return TextureFormat::Unknown;
    }
}

WGPUVertexFormat ToWgpuVertexFormat(Format format)
{
    switch (format)
    {
    // WebGPU has no sRGB vertex formats; the sRGB variants describe the same
    // bytes and the shader owns any decode.
    case Format::R8G8B8A8_UNORM:
    case Format::R8G8B8A8_SRGB:        return WGPUVertexFormat_Unorm8x4;
    case Format::B8G8R8A8_UNORM:
    case Format::B8G8R8A8_SRGB:        return WGPUVertexFormat_Unorm8x4BGRA;
    case Format::R16G16B16A16_FLOAT:   return WGPUVertexFormat_Float16x4;
    case Format::R32G32B32A32_FLOAT:   return WGPUVertexFormat_Float32x4;
    case Format::R32G32B32_FLOAT:      return WGPUVertexFormat_Float32x3;
    case Format::R32G32_FLOAT:         return WGPUVertexFormat_Float32x2;
    case Format::R16G16B16A16_UINT:    return WGPUVertexFormat_Uint16x4;
    case Format::D32_FLOAT:
    case Format::D24_UNORM_S8_UINT:
    case Format::Unknown:
    default:
        return WGPUVertexFormat_Force32;
    }
}

WGPUBufferUsage ToWgpuBufferUsage(uint32_t engineBufferUsage, BufferMemoryUsage memoryUsage)
{
    // MapRead is exclusive with everything except CopyDst, so a readback buffer
    // takes that pair verbatim and ignores the rest of the request.
    if (memoryUsage == BufferMemoryUsage::Readback)
    {
        // Anything else the caller asked for is unreachable here, and silently
        // dropping it surfaces later as an unattributable bind-group rejection.
        // A readback slot a shader writes directly needs a device-local buffer
        // plus a copy into the mapped one; report that instead of guessing.
        const auto kUnsatisfiable = BufferUsage::Storage | BufferUsage::Uniform |
                                    BufferUsage::Vertex | BufferUsage::Index |
                                    BufferUsage::Indirect;
        if ((static_cast<BufferUsage>(engineBufferUsage) & kUnsatisfiable) != BufferUsage::None)
        {
            Logger::Log::Error("WebGpuDevice: buffer usage 0x{:x} cannot combine with Readback memory "
                               "(WebGPU allows MapRead only with CopyDst); the buffer is created "
                               "readback-only and any shader binding of it will be rejected",
                               engineBufferUsage);
        }
        return WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    }

    const auto usage = static_cast<BufferUsage>(engineBufferUsage);
    WGPUBufferUsage result = WGPUBufferUsage_None;
    auto has = [usage](BufferUsage bit) { return (usage & bit) != BufferUsage::None; };

    if (has(BufferUsage::Vertex))      result |= WGPUBufferUsage_Vertex;
    if (has(BufferUsage::Index))       result |= WGPUBufferUsage_Index;
    if (has(BufferUsage::Uniform))     result |= WGPUBufferUsage_Uniform;
    if (has(BufferUsage::Storage))     result |= WGPUBufferUsage_Storage;
    if (has(BufferUsage::Indirect))    result |= WGPUBufferUsage_Indirect;
    if (has(BufferUsage::TransferSrc)) result |= WGPUBufferUsage_CopySrc;

    // Every non-mappable buffer is writable through wgpuQueueWriteBuffer, which
    // the backend uses for UpdateBuffer and for flushing shadowed uploads.
    result |= WGPUBufferUsage_CopyDst;
    return result;
}

WGPUTextureUsage ToWgpuTextureUsage(uint32_t engineTextureUsage)
{
    const auto usage = static_cast<TextureUsage>(engineTextureUsage);
    WGPUTextureUsage result = WGPUTextureUsage_None;
    auto has = [usage](TextureUsage bit) { return (usage & bit) != TextureUsage::None; };

    if (has(TextureUsage::ShaderResource))  result |= WGPUTextureUsage_TextureBinding;
    if (has(TextureUsage::RenderTarget))    result |= WGPUTextureUsage_RenderAttachment;
    if (has(TextureUsage::DepthStencil))    result |= WGPUTextureUsage_RenderAttachment;
    if (has(TextureUsage::UnorderedAccess)) result |= WGPUTextureUsage_StorageBinding;
    if (has(TextureUsage::TransferSrc))     result |= WGPUTextureUsage_CopySrc;
    if (has(TextureUsage::TransferDst))     result |= WGPUTextureUsage_CopyDst;
    return result;
}

WGPUCompareFunction ToWgpuCompareFunction(CompareOp op)
{
    switch (op)
    {
    case CompareOp::Never:          return WGPUCompareFunction_Never;
    case CompareOp::Less:           return WGPUCompareFunction_Less;
    case CompareOp::Equal:          return WGPUCompareFunction_Equal;
    case CompareOp::LessOrEqual:    return WGPUCompareFunction_LessEqual;
    case CompareOp::Greater:        return WGPUCompareFunction_Greater;
    case CompareOp::NotEqual:       return WGPUCompareFunction_NotEqual;
    case CompareOp::GreaterOrEqual: return WGPUCompareFunction_GreaterEqual;
    case CompareOp::Always:         return WGPUCompareFunction_Always;
    }
    return WGPUCompareFunction_Always;
}

WGPUBlendFactor ToWgpuBlendFactor(BlendFactor factor)
{
    switch (factor)
    {
    case BlendFactor::Zero:                  return WGPUBlendFactor_Zero;
    case BlendFactor::One:                   return WGPUBlendFactor_One;
    case BlendFactor::SrcColor:              return WGPUBlendFactor_Src;
    case BlendFactor::OneMinusSrcColor:      return WGPUBlendFactor_OneMinusSrc;
    case BlendFactor::DstColor:              return WGPUBlendFactor_Dst;
    case BlendFactor::OneMinusDstColor:      return WGPUBlendFactor_OneMinusDst;
    case BlendFactor::SrcAlpha:              return WGPUBlendFactor_SrcAlpha;
    case BlendFactor::OneMinusSrcAlpha:      return WGPUBlendFactor_OneMinusSrcAlpha;
    case BlendFactor::DstAlpha:              return WGPUBlendFactor_DstAlpha;
    case BlendFactor::OneMinusDstAlpha:      return WGPUBlendFactor_OneMinusDstAlpha;
    case BlendFactor::ConstantColor:         return WGPUBlendFactor_Constant;
    case BlendFactor::OneMinusConstantColor: return WGPUBlendFactor_OneMinusConstant;
    // WebGPU carries a single blend constant shared by colour and alpha, so the
    // alpha-only constant factors resolve to the same value.
    case BlendFactor::ConstantAlpha:         return WGPUBlendFactor_Constant;
    case BlendFactor::OneMinusConstantAlpha: return WGPUBlendFactor_OneMinusConstant;
    case BlendFactor::AlphaSaturate:         return WGPUBlendFactor_SrcAlphaSaturated;
    case BlendFactor::Src1Color:             return WGPUBlendFactor_Src1;
    case BlendFactor::OneMinusSrc1Color:     return WGPUBlendFactor_OneMinusSrc1;
    case BlendFactor::Src1Alpha:             return WGPUBlendFactor_Src1Alpha;
    case BlendFactor::OneMinusSrc1Alpha:     return WGPUBlendFactor_OneMinusSrc1Alpha;
    }
    return WGPUBlendFactor_One;
}

WGPUBlendOperation ToWgpuBlendOperation(BlendOp op)
{
    switch (op)
    {
    case BlendOp::Add:             return WGPUBlendOperation_Add;
    case BlendOp::Subtract:        return WGPUBlendOperation_Subtract;
    case BlendOp::ReverseSubtract: return WGPUBlendOperation_ReverseSubtract;
    case BlendOp::Min:             return WGPUBlendOperation_Min;
    case BlendOp::Max:             return WGPUBlendOperation_Max;
    }
    return WGPUBlendOperation_Add;
}

WGPUPrimitiveTopology ToWgpuPrimitiveTopology(PrimitiveTopology topology)
{
    switch (topology)
    {
    case PrimitiveTopology::PointList:     return WGPUPrimitiveTopology_PointList;
    case PrimitiveTopology::LineList:      return WGPUPrimitiveTopology_LineList;
    case PrimitiveTopology::LineStrip:     return WGPUPrimitiveTopology_LineStrip;
    case PrimitiveTopology::TriangleList:  return WGPUPrimitiveTopology_TriangleList;
    case PrimitiveTopology::TriangleStrip: return WGPUPrimitiveTopology_TriangleStrip;
    // Patches and adjacency topologies have no WebGPU equivalent.
    default:                               return WGPUPrimitiveTopology_TriangleList;
    }
}

WGPUFrontFace ToWgpuFrontFace(FrontFace frontFace)
{
    return frontFace == FrontFace::Clockwise ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
}

WGPUCullMode ToWgpuCullMode(CullModeFlags cullMode)
{
    switch (cullMode)
    {
    case CullModeFlagBits::Front: return WGPUCullMode_Front;
    case CullModeFlagBits::Back:  return WGPUCullMode_Back;
    // WebGPU has no FrontAndBack; rasterizerDiscard is the equivalent and is a
    // pipeline-level concept the engine expresses separately.
    default:                      return WGPUCullMode_None;
    }
}

WGPULoadOp ToWgpuLoadOp(RenderPassDesc::LoadOp op)
{
    switch (op)
    {
    case RenderPassDesc::LoadOp::Clear: return WGPULoadOp_Clear;
    case RenderPassDesc::LoadOp::Load:  return WGPULoadOp_Load;
    // WebGPU has no DontCare load; clearing is the safe reading of "contents
    // are undefined" and avoids consuming stale attachment data.
    case RenderPassDesc::LoadOp::DontCare: return WGPULoadOp_Clear;
    }
    return WGPULoadOp_Clear;
}

WGPUStoreOp ToWgpuStoreOp(RenderPassDesc::StoreOp op)
{
    switch (op)
    {
    case RenderPassDesc::StoreOp::Store: return WGPUStoreOp_Store;
    case RenderPassDesc::StoreOp::DontCare: return WGPUStoreOp_Discard;
    // "Preserve, write nothing" is expressible only as the attachment's
    // readOnly flag, which BeginRenderPass sets instead of a store op; the op
    // itself falls back to Store, since the contract is that the contents
    // survive and Discard would destroy them.
    case RenderPassDesc::StoreOp::None: return WGPUStoreOp_Store;
    }
    return WGPUStoreOp_Store;
}

WGPUAddressMode ToWgpuAddressMode(uint32_t engineAddressMode)
{
    // Engine sampler address modes follow Vulkan's VkSamplerAddressMode order.
    switch (engineAddressMode)
    {
    case 0: return WGPUAddressMode_Repeat;
    case 1: return WGPUAddressMode_MirrorRepeat;
    case 2: return WGPUAddressMode_ClampToEdge;
    // ClampToBorder / MirrorClampToEdge need native features; clamp-to-edge is
    // the closest core behaviour.
    default: return WGPUAddressMode_ClampToEdge;
    }
}

WGPUFilterMode ToWgpuFilterMode(uint32_t engineFilter)
{
    return engineFilter == 0 ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear;
}

WGPUMipmapFilterMode ToWgpuMipmapFilterMode(uint32_t engineFilter)
{
    return engineFilter == 0 ? WGPUMipmapFilterMode_Nearest : WGPUMipmapFilterMode_Linear;
}

WGPUTextureViewDimension ToWgpuTextureViewDimension(TextureViewType type)
{
    switch (type)
    {
    case TextureViewType::View2D:        return WGPUTextureViewDimension_2D;
    case TextureViewType::View2DArray:   return WGPUTextureViewDimension_2DArray;
    case TextureViewType::ViewCube:      return WGPUTextureViewDimension_Cube;
    case TextureViewType::ViewCubeArray: return WGPUTextureViewDimension_CubeArray;
    case TextureViewType::View3D:        return WGPUTextureViewDimension_3D;
    }
    return WGPUTextureViewDimension_2D;
}

WGPUTextureAspect ToWgpuTextureAspect(TextureAspect aspect)
{
    if (aspect == TextureAspect::Depth)   return WGPUTextureAspect_DepthOnly;
    if (aspect == TextureAspect::Stencil) return WGPUTextureAspect_StencilOnly;
    return WGPUTextureAspect_All;
}

WGPUIndexFormat ToWgpuIndexFormat(IndexType type)
{
    return type == IndexType::Uint32 ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16;
}

WGPUShaderStage ToWgpuShaderStage(uint32_t engineStageMask)
{
    WGPUShaderStage stages = WGPUShaderStage_None;
    if ((engineStageMask & kShaderStageVertex) != 0)   stages |= WGPUShaderStage_Vertex;
    if ((engineStageMask & kShaderStageFragment) != 0) stages |= WGPUShaderStage_Fragment;
    if ((engineStageMask & kShaderStageCompute) != 0)  stages |= WGPUShaderStage_Compute;
    return stages;
}

WGPUColorWriteMask ToWgpuColorWriteMask(uint32_t engineMask)
{
    WGPUColorWriteMask mask = WGPUColorWriteMask_None;
    if ((engineMask & 0x1u) != 0) mask |= WGPUColorWriteMask_Red;
    if ((engineMask & 0x2u) != 0) mask |= WGPUColorWriteMask_Green;
    if ((engineMask & 0x4u) != 0) mask |= WGPUColorWriteMask_Blue;
    if ((engineMask & 0x8u) != 0) mask |= WGPUColorWriteMask_Alpha;
    return mask;
}

bool IsCoreStorageCapableFormat(WGPUTextureFormat format)
{
    switch (format)
    {
    case WGPUTextureFormat_RGBA8Unorm:
    case WGPUTextureFormat_RGBA8Snorm:
    case WGPUTextureFormat_RGBA8Uint:
    case WGPUTextureFormat_RGBA8Sint:
    case WGPUTextureFormat_RGBA16Uint:
    case WGPUTextureFormat_RGBA16Sint:
    case WGPUTextureFormat_RGBA16Float:
    case WGPUTextureFormat_R32Float:
    case WGPUTextureFormat_R32Uint:
    case WGPUTextureFormat_R32Sint:
    case WGPUTextureFormat_RG32Float:
    case WGPUTextureFormat_RG32Uint:
    case WGPUTextureFormat_RG32Sint:
    case WGPUTextureFormat_RGBA32Float:
    case WGPUTextureFormat_RGBA32Uint:
    case WGPUTextureFormat_RGBA32Sint:
        return true;
    default:
        return false;
    }
}

bool IsDepthStencilFormat(WGPUTextureFormat format)
{
    switch (format)
    {
    case WGPUTextureFormat_Stencil8:
    case WGPUTextureFormat_Depth16Unorm:
    case WGPUTextureFormat_Depth24Plus:
    case WGPUTextureFormat_Depth24PlusStencil8:
    case WGPUTextureFormat_Depth32Float:
    case WGPUTextureFormat_Depth32FloatStencil8:
        return true;
    default:
        return false;
    }
}

} // namespace GameEngine::Rendering::WebGpu
