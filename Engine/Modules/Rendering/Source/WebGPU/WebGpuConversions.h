#pragma once

// Backend-neutral engine enums -> WebGPU enums. Pure functions, no state: every
// translation the device, resources and command list need lives here so the
// mapping table has exactly one home.

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <webgpu/webgpu.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace GameEngine::Rendering::WebGpu
{

// WGPUStringView over a NUL-terminated literal / std::string. WGPU_STRLEN tells
// wgpu to call strlen itself, so the pointer must outlive the call only.
inline WGPUStringView MakeStringView(const char* text)
{
    WGPUStringView view{};
    view.data = text;
    view.length = (text != nullptr) ? WGPU_STRLEN : 0;
    return view;
}

inline WGPUStringView MakeStringView(std::string_view text)
{
    WGPUStringView view{};
    view.data = text.data();
    view.length = text.size();
    return view;
}

// Copies a wgpu-owned string view into an owned std::string (views are not
// NUL-terminated in general).
std::string ToString(WGPUStringView view);

// TextureFormat::Unknown and any format WebGPU has no equivalent for map to
// WGPUTextureFormat_Undefined; callers treat that as "unsupported".
WGPUTextureFormat ToWgpuTextureFormat(TextureFormat format);
TextureFormat FromWgpuTextureFormat(WGPUTextureFormat format);

// Vertex-attribute formats (engine `Format`, not `TextureFormat`).
WGPUVertexFormat ToWgpuVertexFormat(Format format);

WGPUBufferUsage ToWgpuBufferUsage(uint32_t engineBufferUsage, BufferMemoryUsage memoryUsage);
WGPUTextureUsage ToWgpuTextureUsage(uint32_t engineTextureUsage);

WGPUCompareFunction ToWgpuCompareFunction(CompareOp op);
WGPUBlendFactor ToWgpuBlendFactor(BlendFactor factor);
WGPUBlendOperation ToWgpuBlendOperation(BlendOp op);
WGPUPrimitiveTopology ToWgpuPrimitiveTopology(PrimitiveTopology topology);
WGPUFrontFace ToWgpuFrontFace(FrontFace frontFace);
WGPUCullMode ToWgpuCullMode(CullModeFlags cullMode);
WGPULoadOp ToWgpuLoadOp(RenderPassDesc::LoadOp op);
WGPUStoreOp ToWgpuStoreOp(RenderPassDesc::StoreOp op);
WGPUAddressMode ToWgpuAddressMode(uint32_t engineAddressMode);
WGPUFilterMode ToWgpuFilterMode(uint32_t engineFilter);
WGPUMipmapFilterMode ToWgpuMipmapFilterMode(uint32_t engineFilter);
WGPUTextureViewDimension ToWgpuTextureViewDimension(TextureViewType type);
WGPUTextureAspect ToWgpuTextureAspect(TextureAspect aspect);
WGPUIndexFormat ToWgpuIndexFormat(IndexType type);
WGPUShaderStage ToWgpuShaderStage(uint32_t engineStageMask);

// Colour-write mask: the engine uses Vulkan's RGBA bit order, which happens to
// match WGPUColorWriteMask bit for bit; the conversion exists so the assumption
// is stated in one place rather than assumed at each call site.
WGPUColorWriteMask ToWgpuColorWriteMask(uint32_t engineMask);

// True for formats whose only legal render-pass role is a depth/stencil
// attachment.
bool IsDepthStencilFormat(WGPUTextureFormat format);
/// Core WebGPU's fixed list of formats a texture may carry STORAGE_BINDING with:
/// RGBA 8-bit, RGBA 16-bit uint/sint/float, and the 32-bit r/rg/rgba family.
/// bgra8unorm joins it only behind the BGRA8UnormStorage feature, which the
/// device checks separately.
bool IsCoreStorageCapableFormat(WGPUTextureFormat format);

} // namespace GameEngine::Rendering::WebGpu
