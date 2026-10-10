// Shader modules, bind-group layouts, pipeline layouts and pipeline creation
// for the WebGPU backend.

#include "WebGpuConversions.h"
#include "WebGpuDevice.h"
#include "WebGpuUnsupported.h"

#include "Rendering/Core/PipelineTypes.h"

#include "Logger/Logger.h"

#if !defined(__EMSCRIPTEN__)
// wgpu-native extras (immediates, DevicePoll); Dawn/emdawnwebgpu has no wgpu.h.
#include <webgpu/wgpu.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace GameEngine::Rendering
{

namespace
{
// SPIR-V and WGSL both name the engine's entry point "main".
constexpr const char* kShaderEntryPoint = "main";

// Distinguishes the depth-write-disabled twin in Dawn's validation messages.
constexpr const char* kDepthReadOnlyPipelineSuffix = ".DepthReadOnly";

// PipelineDesc offers two spellings of the same block: the single
// Size/StageMask pair, and the named-range list that wins whenever it is
// non-empty (Device.h, PipelineDesc::pushConstantRanges). `PushConstants.Size`
// is therefore the shorthand's size, not the block's — a desc that used only
// the range list reports zero there. Everything WebGPU derives from the block
// (the emulation group in the pipeline layout, and the UBO a touching draw
// uploads) needs the byte count that actually spans it, so take the widest
// declaration either spelling produces. Ranges are 4-byte aligned.
uint32_t PushConstantBlockBytes(const PushConstantRange& single,
                                const std::vector<NamedPushConstantRange>& ranges)
{
    constexpr uint32_t kPushConstantAlignment = 4u;
    uint32_t bytes = single.Size;
    for (const NamedPushConstantRange& range : ranges)
    {
        const uint32_t end = range.Offset + range.Size;
        const uint32_t aligned = (end + kPushConstantAlignment - 1u) & ~(kPushConstantAlignment - 1u);
        bytes = std::max(bytes, aligned);
    }
    return bytes;
}

WGPUBufferBindingType ResolveBufferBindingType(DescriptorType type, uint32_t engineStageMask,
                                               uint32_t bindingFlags)
{
    if (type == DescriptorType::UniformBuffer)
    {
        return WGPUBufferBindingType_Uniform;
    }
    // The shader module's own access class wins: a var<storage, read> module
    // rejects a read_write layout (and vice versa).
    if ((bindingFlags & kDescriptorBindingReadOnlyStorage) != 0)
    {
        return WGPUBufferBindingType_ReadOnlyStorage;
    }
    // Writable storage in the vertex stage needs a native feature that core
    // WebGPU does not have; a vertex-visible storage buffer is therefore
    // declared read-only unless compute also sees it.
    const bool vertexVisible = (engineStageMask & kShaderStageVertex) != 0;
    const bool computeVisible = (engineStageMask & kShaderStageCompute) != 0;
    return (vertexVisible && !computeVisible) ? WGPUBufferBindingType_ReadOnlyStorage
                                              : WGPUBufferBindingType_Storage;
}
} // namespace

WGPUShaderModule WebGpuDevice::CreateShaderModule(const std::vector<uint8_t>& bytes, const char* debugName)
{
    if (bytes.empty())
    {
        Logger::Log::Error("WebGpuDevice: shader '{}' is empty", debugName ? debugName : "");
        return nullptr;
    }

    // The shader loaders serve the form PreferredShaderSource() asked for, so
    // the blob should already be a SPIR-V word stream or UTF-8 WGSL to match.
    // The SPIR-V magic in the first word tells them apart and validates that:
    // a package cooked without WGSL chunks arrives here as SPIR-V and is
    // rejected by name below instead of reaching wgpu as garbage WGSL.
    constexpr uint32_t kSpirvMagic = 0x07230203u;
    uint32_t firstWord = 0;
    if (bytes.size() >= sizeof(uint32_t))
    {
        std::memcpy(&firstWord, bytes.data(), sizeof(uint32_t));
    }
    const bool isSpirv = firstWord == kSpirvMagic && (bytes.size() % sizeof(uint32_t)) == 0;

    WGPUShaderModuleDescriptor desc{};
    desc.label = WebGpu::MakeStringView(debugName);

    WGPUShaderSourceSPIRV spirvSource{};
    WGPUShaderSourceWGSL wgslSource{};
    if (isSpirv)
    {
        if (!m_SupportsSpirv)
        {
            WebGpuLogUnsupportedOnce("SPIR-V shader ingestion (cook to WGSL)");
            return nullptr;
        }
        spirvSource.chain.sType = WGPUSType_ShaderSourceSPIRV;
        spirvSource.codeSize = static_cast<uint32_t>(bytes.size() / sizeof(uint32_t));
        spirvSource.code = reinterpret_cast<const uint32_t*>(bytes.data());
        desc.nextInChain = &spirvSource.chain;
    }
    else
    {
        wgslSource.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgslSource.code.data = reinterpret_cast<const char*>(bytes.data());
        wgslSource.code.length = bytes.size();
        desc.nextInChain = &wgslSource.chain;
    }
    return wgpuDeviceCreateShaderModule(m_Device, &desc);
}

namespace
{
WGPUTextureViewDimension ViewDimensionFromBinding(const DescriptorBinding& binding)
{
    switch (binding.imageDim)
    {
    case 1: return WGPUTextureViewDimension_1D;
    case 3: return WGPUTextureViewDimension_3D;
    case 4: return binding.imageArrayed ? WGPUTextureViewDimension_CubeArray : WGPUTextureViewDimension_Cube;
    case 2:
    default: return binding.imageArrayed ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
    }
}

WGPUTextureSampleType SampleTypeFromBinding(const DescriptorBinding& binding)
{
    // A shadow sampler is a depth texture: the cook's WGSL declares
    // texture_depth_* against sampler_comparison, and Dawn rejects the bind if
    // the layout claims a filterable float instead.
    if (binding.imageIsDepth)
        return WGPUTextureSampleType_Depth;
    // Integer images are never filtered and never pair with a sampler in WGSL;
    // texelFetch is the only access, so Uint is the whole contract.
    if (binding.imageIsUnsignedInteger)
        return WGPUTextureSampleType_Uint;
    // A multisampled image is read with textureLoad only; WebGPU rejects a
    // Float (filterable) sample type on a multisampled layout entry.
    if (binding.imageMultisample)
        return WGPUTextureSampleType_UnfilterableFloat;
    // A separate sampled image that reflection saw no stage filter is read with
    // textureLoad only, on any stage. UnfilterableFloat accepts every float
    // format Float does and depth32float besides, which a second, raw-depth
    // binding of a shadow map needs (shadow_sampling.glsl's ge_shadowMapRaw).
    if (binding.type == DescriptorType::Texture && binding.imageUsageReflected &&
        !binding.imageFilterableFloat)
        return WGPUTextureSampleType_UnfilterableFloat;
    // Reflection records actual usage: imageFilterableFloat is true exactly
    // when some stage samples this binding through a filtering-capable op
    // (ImageInfo.Filtered from the SPIR-V walk). For compute the bit is
    // authoritative — a texelFetch-only binding stays UnfilterableFloat,
    // which also accepts depth/R32F views that Float would reject. Graphics
    // stages keep the permissive Float default when the bit is unset:
    // hand-synthesized layouts predate the usage walk and say nothing, and
    // Float matches their historical behavior.
    constexpr uint32_t kComputeStageBit = 0x00000020; // VK_SHADER_STAGE_COMPUTE_BIT
    if (binding.shaderStages == kComputeStageBit && !binding.imageFilterableFloat)
        return WGPUTextureSampleType_UnfilterableFloat;
    return WGPUTextureSampleType_Float;
}

// A depth texture's split sampler is the comparison sampler the shadow tap
// needs; every other split sampler stays filtering.
WGPUSamplerBindingType SamplerTypeFromBinding(const DescriptorBinding& binding)
{
    return binding.imageIsDepth ? WGPUSamplerBindingType_Comparison
                                : WGPUSamplerBindingType_Filtering;
}
} // namespace

WGPUBindGroupLayout WebGpuDevice::CreateBindGroupLayout(const DescriptorSetLayoutDesc& desc)
{
    std::vector<WGPUBindGroupLayoutEntry> entries;
    entries.reserve(desc.bindings.size());

    for (const DescriptorBinding& binding : desc.bindings)
    {
        WGPUBindGroupLayoutEntry entry{};
        entry.binding = binding.binding;
        entry.visibility = WebGpu::ToWgpuShaderStage(binding.shaderStages);
        entry.bindingArraySize = binding.count > 1 ? binding.count : 0;
        if (binding.count > 1)
        {
            WebGpuLogUnsupportedOnce("descriptor arrays (binding arrays need a wgpu native feature)");
        }

        switch (binding.type)
        {
        case DescriptorType::UniformBuffer:
        case DescriptorType::StorageBuffer:
            entry.buffer.type = ResolveBufferBindingType(binding.type, binding.shaderStages, binding.flags);
            break;
        case DescriptorType::Texture:
            entry.texture.sampleType = SampleTypeFromBinding(binding);
            entry.texture.viewDimension = ViewDimensionFromBinding(binding);
            entry.texture.multisampled = binding.imageMultisample;
            break;
        case DescriptorType::StorageImage:
            // GLSL image2D without writeonly reflects as read_write access in
            // wgsl-land only when actually read; WriteOnly matches the
            // engine's imageStore-style passes. Format comes from the
            // shader's declared texel format via reflection meta.
            entry.storageTexture.access = binding.storageReadOnly
                                              ? WGPUStorageTextureAccess_ReadOnly
                                              : WGPUStorageTextureAccess_WriteOnly;
            entry.storageTexture.format =
                binding.storageTexelFormat != 0
                    ? WebGpu::ToWgpuTextureFormat(static_cast<TextureFormat>(binding.storageTexelFormat))
                    : WGPUTextureFormat_RGBA8Unorm;
            entry.storageTexture.viewDimension = ViewDimensionFromBinding(binding);
            break;
        case DescriptorType::Sampler:
            entry.sampler.type = SamplerTypeFromBinding(binding);
            break;
        case DescriptorType::AccelerationStructure:
            // WebGPU exposes no ray-tracing acceleration structure. Nothing on this backend can
            // declare one, so reaching here means a pipeline was built for a device that cannot
            // run it — say so once rather than emitting a silently wrong layout.
            WebGpuLogUnsupportedOnce("acceleration-structure descriptors (no WebGPU equivalent)");
            break;
        case DescriptorType::CombinedImageSampler:
        {
            // WebGPU has no combined image/sampler. The WGSL cook splits the
            // pair: the texture keeps the original binding, the sampler moves
            // to binding + kCombinedSamplerBindingOffset (the same renumber
            // Tools/ShaderCook applies to the SPIR-V before naga).
            entry.texture.sampleType = SampleTypeFromBinding(binding);
            entry.texture.viewDimension = ViewDimensionFromBinding(binding);
            entry.texture.multisampled = binding.imageMultisample;
            entries.push_back(entry);
            // A MULTISAMPLED image has no sampler half. WGSL cannot sample one — it is read with
            // textureLoad — so the cook emits the texture alone, and a sampler entry here would be
            // a layout binding the shader module never declares, which invalidates the pipeline.
            // GLSL still spells it sampler2DMS, so the split is where the two conventions part.
            if (binding.imageMultisample)
                continue;
            WGPUBindGroupLayoutEntry samplerEntry{};
            samplerEntry.binding = binding.binding + kCombinedSamplerBindingOffset;
            samplerEntry.visibility = entry.visibility;
            samplerEntry.sampler.type = SamplerTypeFromBinding(binding);
            entries.push_back(samplerEntry);
            continue;
        }
        }
        entries.push_back(entry);
    }

    WGPUBindGroupLayoutDescriptor layoutDesc{};
    layoutDesc.label = WebGpu::MakeStringView(desc.debugName);
    layoutDesc.entryCount = entries.size();
    layoutDesc.entries = entries.empty() ? nullptr : entries.data();
    return wgpuDeviceCreateBindGroupLayout(m_Device, &layoutDesc);
}

WGPUBindGroupLayout WebGpuDevice::GetOrCreateBindGroupLayout(DescriptorSetLayoutId id)
{
    if (!id.IsValid() || m_Device == nullptr)
    {
        return nullptr;
    }

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    auto existing = m_BindGroupLayouts.find(id.Value);
    if (existing != m_BindGroupLayouts.end())
    {
        return existing->second;
    }

    DescriptorSetLayoutDesc desc{};
    if (!CopyDescriptorSetLayout(id, desc))
    {
        return nullptr;
    }

    WGPUBindGroupLayout layout = CreateBindGroupLayout(desc);
    if (layout == nullptr)
    {
        return nullptr;
    }
    m_BindGroupLayouts.emplace(id.Value, layout);
    return layout;
}

WGPUBindGroupLayout WebGpuDevice::GetOrCreateEmptyBindGroupLayout()
{
    if (m_EmptyBindGroupLayout == nullptr && m_Device != nullptr)
    {
        WGPUBindGroupLayoutDescriptor desc{};
        desc.label = WebGpu::MakeStringView("Empty");
        m_EmptyBindGroupLayout = wgpuDeviceCreateBindGroupLayout(m_Device, &desc);
    }
    return m_EmptyBindGroupLayout;
}

WGPUBindGroup WebGpuDevice::GetOrCreateEmptyBindGroup()
{
    if (m_EmptyBindGroup == nullptr && m_Device != nullptr)
    {
        WGPUBindGroupDescriptor desc{};
        desc.label = WebGpu::MakeStringView("Empty");
        desc.layout = GetOrCreateEmptyBindGroupLayout();
        m_EmptyBindGroup = wgpuDeviceCreateBindGroup(m_Device, &desc);
    }
    return m_EmptyBindGroup;
}

WGPUBindGroupLayout WebGpuDevice::GetOrCreatePushConstantBindGroupLayout()
{
    if (m_PushConstantBindGroupLayout == nullptr && m_Device != nullptr)
    {
        m_PushConstantSlotBytes = std::max(256u, m_DeviceLimits.minUniformBufferOffsetAlignment);
        WGPUBindGroupLayoutEntry entry{};
        entry.binding = 0;
        entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
        entry.buffer.type = WGPUBufferBindingType_Uniform;
        entry.buffer.hasDynamicOffset = true;
        entry.buffer.minBindingSize = m_PushConstantSlotBytes;
        WGPUBindGroupLayoutDescriptor desc{};
        desc.label = WebGpu::MakeStringView("PushConstantEmulation");
        desc.entryCount = 1;
        desc.entries = &entry;
        m_PushConstantBindGroupLayout = wgpuDeviceCreateBindGroupLayout(m_Device, &desc);
    }
    return m_PushConstantBindGroupLayout;
}

void WebGpuDevice::ReleasePushConstantRing()
{
    if (m_PushConstantBindGroup != nullptr)
    {
        wgpuBindGroupRelease(m_PushConstantBindGroup);
        m_PushConstantBindGroup = nullptr;
    }
    if (m_PushConstantRing != nullptr)
    {
        wgpuBufferDestroy(m_PushConstantRing);
        wgpuBufferRelease(m_PushConstantRing);
        m_PushConstantRing = nullptr;
    }
    m_PushConstantSliceBytes = 0;
    m_PushConstantOffset = 0;
}

bool WebGpuDevice::EnsurePushConstantRing()
{
    if (m_PushConstantRing != nullptr && m_PushConstantBindGroup != nullptr)
    {
        return true;
    }
    if (m_Device == nullptr || m_Queue == nullptr)
    {
        return false;
    }
    WGPUBindGroupLayout layout = GetOrCreatePushConstantBindGroupLayout();
    if (layout == nullptr)
    {
        return false;
    }
    m_PushConstantSliceBytes =
        static_cast<uint64_t>(kPushConstantSlotsPerFrame) * m_PushConstantSlotBytes;
    const uint64_t capacity = m_PushConstantSliceBytes * kFramesInFlight;
    WGPUBufferDescriptor bd{};
    bd.label = WebGpu::MakeStringView("PushConstantRing");
    bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    bd.size = capacity;
    m_PushConstantRing = wgpuDeviceCreateBuffer(m_Device, &bd);
    if (m_PushConstantRing == nullptr)
    {
        Logger::Log::Error("WebGpuDevice: push-constant ring allocation failed ({} bytes)", capacity);
        return false;
    }
    WGPUBindGroupEntry entry{};
    entry.binding = 0;
    entry.buffer = m_PushConstantRing;
    entry.offset = 0;
    entry.size = m_PushConstantSlotBytes;
    WGPUBindGroupDescriptor gd{};
    gd.label = WebGpu::MakeStringView("PushConstantRing");
    gd.layout = layout;
    gd.entryCount = 1;
    gd.entries = &entry;
    m_PushConstantBindGroup = wgpuDeviceCreateBindGroup(m_Device, &gd);
    if (m_PushConstantBindGroup == nullptr)
    {
        Logger::Log::Error("WebGpuDevice: push-constant ring bind group failed");
        ReleasePushConstantRing();
        return false;
    }
    m_PushConstantOffset = static_cast<uint64_t>(m_FrameIndex) * m_PushConstantSliceBytes;
    return true;
}

bool WebGpuDevice::BindEmulatedPushConstants(WGPURenderPassEncoder renderPass, WGPUComputePassEncoder computePass,
                                            const void* data, uint32_t size, const std::string& passLabel,
                                            const std::string& pipelineName)
{
    if (data == nullptr || size == 0 || !EnsurePushConstantRing())
    {
        return false;
    }
    if (size > m_PushConstantSlotBytes)
    {
        Logger::Log::Error("WebGpuDevice: push-constant block {} exceeds ring slot {}", size,
                           m_PushConstantSlotBytes);
        return false;
    }
    const uint64_t sliceEnd =
        static_cast<uint64_t>(m_FrameIndex) * m_PushConstantSliceBytes + m_PushConstantSliceBytes;
    if (m_PushConstantOffset + m_PushConstantSlotBytes > sliceEnd)
    {
        if (m_PushConstantRefusals++ == 0)
        {
            m_PushConstantFirstRefusalPass = passLabel;
            m_PushConstantFirstRefusalPipeline = pipelineName;
        }
        return false;
    }

    uint8_t slot[512];
    const uint32_t writeBytes = m_PushConstantSlotBytes;
    if (writeBytes > sizeof(slot))
    {
        return false;
    }
    std::memset(slot, 0, writeBytes);
    std::memcpy(slot, data, size);
    wgpuQueueWriteBuffer(m_Queue, m_PushConstantRing, m_PushConstantOffset, slot, writeBytes);

    const uint32_t dynamicOffset = static_cast<uint32_t>(m_PushConstantOffset);
    m_PushConstantOffset += m_PushConstantSlotBytes;
    if (renderPass != nullptr)
    {
        wgpuRenderPassEncoderSetBindGroup(renderPass, kPushConstantEmulationGroup, m_PushConstantBindGroup, 1,
                                          &dynamicOffset);
    }
    else if (computePass != nullptr)
    {
        wgpuComputePassEncoderSetBindGroup(computePass, kPushConstantEmulationGroup, m_PushConstantBindGroup, 1,
                                           &dynamicOffset);
    }
    return true;
}

void WebGpuDevice::ReportPushConstantRefusals()
{
    const uint32_t refused = m_PushConstantRefusals;
    m_PushConstantRefusals = 0;
    if (refused == 0)
    {
        m_PushConstantRefusalsReported = 0;
        return;
    }
    if (refused <= m_PushConstantRefusalsReported)
    {
        return;
    }
    m_PushConstantRefusalsReported = refused;
    Logger::Log::Error("WebGpuDevice: refused {} draws or dispatches last frame: the push-constant ring holds {} "
                       "slots of {} bytes per frame and each draw of a pipeline with push constants takes one; "
                       "the first refused was pipeline '{}' in pass '{}'. Draw fewer, larger instanced "
                       "batches to stay under the limit.",
                       refused, kPushConstantSlotsPerFrame, m_PushConstantSlotBytes,
                       m_PushConstantFirstRefusalPipeline, m_PushConstantFirstRefusalPass);
}

WGPUPipelineLayout WebGpuDevice::CreatePipelineLayout(const std::vector<DescriptorSetLayoutId>& layouts,
                                                      uint32_t immediateSize, const char* debugName)
{
    std::vector<WGPUBindGroupLayout> bindGroupLayouts;
    bindGroupLayouts.reserve(layouts.size());
    for (DescriptorSetLayoutId id : layouts)
    {
        WGPUBindGroupLayout layout = GetOrCreateBindGroupLayout(id);
        if (layout == nullptr)
        {
            Logger::Log::Error("WebGpuDevice: pipeline '{}' references an unresolvable set layout",
                               debugName ? debugName : "");
            return nullptr;
        }
        bindGroupLayouts.push_back(layout);
    }

    // Push-constant emulation (no immediates): the cooked WGSL reads the
    // block as a UBO at group kPushConstantEmulationGroup; pad any unused
    // groups in between with the shared empty layout.
    if (!m_SupportsImmediates && immediateSize > 0)
    {
        while (bindGroupLayouts.size() < kPushConstantEmulationGroup)
        {
            bindGroupLayouts.push_back(GetOrCreateEmptyBindGroupLayout());
        }
        bindGroupLayouts.push_back(GetOrCreatePushConstantBindGroupLayout());
    }

    WGPUPipelineLayoutDescriptor desc{};
    desc.label = WebGpu::MakeStringView(debugName);
    desc.bindGroupLayoutCount = bindGroupLayouts.size();
    desc.bindGroupLayouts = bindGroupLayouts.empty() ? nullptr : bindGroupLayouts.data();
    desc.immediateSize = m_SupportsImmediates ? immediateSize : 0;
    return wgpuDeviceCreatePipelineLayout(m_Device, &desc);
}

PipelineHandle WebGpuDevice::CreateConcreteGraphicsPipeline(const GraphicsPipelineDesc& gd, const PipelineFormatKey& fk)
{
    if (m_Device == nullptr)
    {
        return INVALID_PIPELINE_HANDLE;
    }
    if (gd.Kind == GraphicsPipelineKind::MeshFragment)
    {
        WebGpuLogUnsupportedOnce("mesh-shading pipelines");
        return INVALID_PIPELINE_HANDLE;
    }
    if (!gd.VertexShader || gd.VertexShader->empty())
    {
        Logger::Log::Error("WebGpuDevice: graphics pipeline '{}' has no vertex shader", gd.DebugName);
        return INVALID_PIPELINE_HANDLE;
    }

    WGPUShaderModule vertexModule = CreateShaderModule(*gd.VertexShader, gd.DebugName.c_str());
    if (vertexModule == nullptr)
    {
        return INVALID_PIPELINE_HANDLE;
    }
    WGPUShaderModule fragmentModule = nullptr;
    if (gd.PixelShader && !gd.PixelShader->empty())
    {
        fragmentModule = CreateShaderModule(*gd.PixelShader, gd.DebugName.c_str());
        if (fragmentModule == nullptr)
        {
            wgpuShaderModuleRelease(vertexModule);
            return INVALID_PIPELINE_HANDLE;
        }
    }

    const uint32_t pushConstantBytes =
        PushConstantBlockBytes(gd.PushConstants, gd.NamedPushConstantRanges);
    WGPUPipelineLayout pipelineLayout =
        CreatePipelineLayout(gd.DescriptorSetLayouts, pushConstantBytes, gd.DebugName.c_str());
    if (pipelineLayout == nullptr)
    {
        wgpuShaderModuleRelease(vertexModule);
        if (fragmentModule != nullptr) wgpuShaderModuleRelease(fragmentModule);
        return INVALID_PIPELINE_HANDLE;
    }

    // Vertex layout: one WGPUVertexBufferLayout per engine binding, in the
    // order the desc lists them, attributes grouped by the binding they read
    // from. Buffer slot i is gd.VertexBindings[i].binding, which the wrapper
    // records for the command list (WebGpuPipeline::vertexBindings).
    std::vector<std::vector<WGPUVertexAttribute>> attributesPerBinding(gd.VertexBindings.size());
    for (const VertexInputAttribute& attribute : gd.VertexAttributes)
    {
        auto bindingIndex = std::find_if(gd.VertexBindings.begin(), gd.VertexBindings.end(),
                                         [&attribute](const VertexInputBinding& b) { return b.binding == attribute.binding; });
        if (bindingIndex == gd.VertexBindings.end())
        {
            continue;
        }
        WGPUVertexAttribute wgpuAttribute{};
        wgpuAttribute.format = WebGpu::ToWgpuVertexFormat(attribute.format);
        wgpuAttribute.offset = attribute.offset;
        wgpuAttribute.shaderLocation = attribute.location;
        attributesPerBinding[static_cast<size_t>(bindingIndex - gd.VertexBindings.begin())].push_back(wgpuAttribute);
    }

    std::vector<WGPUVertexBufferLayout> vertexBuffers;
    vertexBuffers.reserve(gd.VertexBindings.size());
    for (size_t i = 0; i < gd.VertexBindings.size(); ++i)
    {
        WGPUVertexBufferLayout layout{};
        layout.arrayStride = gd.VertexBindings[i].stride;
        layout.stepMode = gd.VertexBindings[i].inputRate == 1 ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex;
        layout.attributeCount = attributesPerBinding[i].size();
        layout.attributes = attributesPerBinding[i].empty() ? nullptr : attributesPerBinding[i].data();
        vertexBuffers.push_back(layout);
    }

    // Colour targets, one per attachment in the format key.
    std::vector<WGPUColorTargetState> colorTargets;
    std::vector<WGPUBlendState> blendStates(fk.ColorCount);
    colorTargets.reserve(fk.ColorCount);
    for (uint8_t i = 0; i < fk.ColorCount && i < PipelineFormatKey::kMaxColors; ++i)
    {
        const ColorBlendAttachmentState* blend =
            i < gd.ColorBlend.attachments.size() ? &gd.ColorBlend.attachments[i] : nullptr;

        WGPUColorTargetState target{};
        target.format = WebGpu::ToWgpuTextureFormat(fk.ColorFormats[i]);
        // Blend attachments describe blending, not shader outputs. Opaque and
        // unblended fullscreen pipelines leave the vector empty and still write
        // location 0 — defaulting those to a zero write mask blacks the canvas
        // with no validation error. Surplus MRT slots (a 1-output variant in
        // SSSR's 3-target world pass) are the opposite case: WebGPU rejects a
        // target with no fragment output unless writeMask is None.
        if (blend != nullptr)
            target.writeMask = WebGpu::ToWgpuColorWriteMask(blend->colorWriteMask);
        else if (gd.ColorBlend.attachments.empty() && i == 0)
            target.writeMask = WGPUColorWriteMask_All;
        else
            target.writeMask = WGPUColorWriteMask_None;
        if (blend != nullptr && blend->blendEnable)
        {
            blendStates[i].color.operation = WebGpu::ToWgpuBlendOperation(blend->colorBlendOp);
            blendStates[i].color.srcFactor = WebGpu::ToWgpuBlendFactor(blend->srcColorBlendFactor);
            blendStates[i].color.dstFactor = WebGpu::ToWgpuBlendFactor(blend->dstColorBlendFactor);
            blendStates[i].alpha.operation = WebGpu::ToWgpuBlendOperation(blend->alphaBlendOp);
            blendStates[i].alpha.srcFactor = WebGpu::ToWgpuBlendFactor(blend->srcAlphaBlendFactor);
            blendStates[i].alpha.dstFactor = WebGpu::ToWgpuBlendFactor(blend->dstAlphaBlendFactor);
            target.blend = &blendStates[i];
        }
        colorTargets.push_back(target);
    }

    // A masked-off target is a legal pipeline that silently stores nothing, so
    // an MRT slice that should have been written looks identical to one the
    // pass never had. Name it once per pipeline rather than leaving the reader
    // to infer it from the absence of pixels.
    if (fk.ColorCount > gd.ColorBlend.attachments.size() && !gd.ColorBlend.attachments.empty())
    {
        Logger::Log::Warning(
            "WebGpuDevice: pipeline '{}' — render pass has {} colour targets but the pipeline "
            "describes {}; targets {}+ are write-masked off and store nothing",
            gd.DebugName, static_cast<uint32_t>(fk.ColorCount),
            static_cast<uint32_t>(gd.ColorBlend.attachments.size()),
            static_cast<uint32_t>(gd.ColorBlend.attachments.size()));
    }

    WGPUFragmentState fragmentState{};
    fragmentState.module = fragmentModule;
    fragmentState.entryPoint = WebGpu::MakeStringView(kShaderEntryPoint);
    fragmentState.targetCount = colorTargets.size();
    fragmentState.targets = colorTargets.empty() ? nullptr : colorTargets.data();

    WGPUDepthStencilState depthStencil{};
    const WGPUTextureFormat depthFormat = WebGpu::ToWgpuTextureFormat(fk.DepthFormat);
    if (depthFormat != WGPUTextureFormat_Undefined)
    {
        depthStencil.format = depthFormat;
        depthStencil.depthWriteEnabled =
            gd.DepthStencil.depthWriteEnable ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        depthStencil.depthCompare = gd.DepthStencil.depthTestEnable
                                        ? WebGpu::ToWgpuCompareFunction(gd.DepthStencil.depthCompareOp)
                                        : WGPUCompareFunction_Always;
        depthStencil.stencilFront.compare = WGPUCompareFunction_Always;
        depthStencil.stencilFront.failOp = WGPUStencilOperation_Keep;
        depthStencil.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
        depthStencil.stencilFront.passOp = WGPUStencilOperation_Keep;
        depthStencil.stencilBack = depthStencil.stencilFront;
        depthStencil.depthBias = gd.Rasterization.depthBiasEnable
                                     ? static_cast<int32_t>(gd.Rasterization.depthBiasConstantFactor)
                                     : 0;
        depthStencil.depthBiasSlopeScale = gd.Rasterization.depthBiasEnable ? gd.Rasterization.depthBiasSlopeFactor : 0.0f;
        depthStencil.depthBiasClamp = gd.Rasterization.depthBiasEnable ? gd.Rasterization.depthBiasClamp : 0.0f;
    }

    WGPURenderPipelineDescriptor pipelineDesc{};
    pipelineDesc.label = WebGpu::MakeStringView(gd.DebugName.c_str());
    pipelineDesc.layout = pipelineLayout;
    pipelineDesc.vertex.module = vertexModule;
    pipelineDesc.vertex.entryPoint = WebGpu::MakeStringView(kShaderEntryPoint);
    pipelineDesc.vertex.bufferCount = vertexBuffers.size();
    pipelineDesc.vertex.buffers = vertexBuffers.empty() ? nullptr : vertexBuffers.data();
    pipelineDesc.primitive.topology = WebGpu::ToWgpuPrimitiveTopology(gd.Topology);
    pipelineDesc.primitive.frontFace = WebGpu::ToWgpuFrontFace(gd.Rasterization.frontFace);
    pipelineDesc.primitive.cullMode = WebGpu::ToWgpuCullMode(gd.Rasterization.cullMode);
    // Only when the device actually took depth-clip-control: asking without it
    // invalidates the pipeline outright rather than degrading.
    pipelineDesc.primitive.unclippedDepth =
        (gd.Rasterization.depthClampEnable && m_SupportsDepthClipControl) ? 1u : 0u;
    if (gd.Topology == PrimitiveTopology::TriangleStrip || gd.Topology == PrimitiveTopology::LineStrip)
    {
        // Strip topologies must declare the index format the pipeline restarts on.
        pipelineDesc.primitive.stripIndexFormat = WGPUIndexFormat_Uint32;
    }
    pipelineDesc.multisample.count = std::max<uint32_t>(1u, fk.RasterizationSamples);
    pipelineDesc.multisample.mask = 0xFFFFFFFFu;
    pipelineDesc.depthStencil = depthFormat != WGPUTextureFormat_Undefined ? &depthStencil : nullptr;
    pipelineDesc.fragment = fragmentModule != nullptr ? &fragmentState : nullptr;

    WGPURenderPipeline renderPipeline = wgpuDeviceCreateRenderPipeline(m_Device, &pipelineDesc);

    // A depth-writing pipeline is illegal inside a read-only-depth pass, and the
    // engine relies on the pass overriding the pipeline (Vulkan dynamic state,
    // Metal read-only depth-stencil state). WebGPU bakes the flag in, so build
    // the override as a twin now, while the compiled modules and layout are hot.
    WGPURenderPipeline renderPipelineDepthReadOnly = nullptr;
    if (renderPipeline != nullptr && pipelineDesc.depthStencil != nullptr &&
        depthStencil.depthWriteEnabled == WGPUOptionalBool_True)
    {
        const std::string readOnlyName = gd.DebugName + kDepthReadOnlyPipelineSuffix;
        WGPUDepthStencilState readOnlyDepth = depthStencil;
        readOnlyDepth.depthWriteEnabled = WGPUOptionalBool_False;
        WGPURenderPipelineDescriptor readOnlyDesc = pipelineDesc;
        readOnlyDesc.label = WebGpu::MakeStringView(readOnlyName.c_str());
        readOnlyDesc.depthStencil = &readOnlyDepth;
        renderPipelineDepthReadOnly = wgpuDeviceCreateRenderPipeline(m_Device, &readOnlyDesc);
        if (renderPipelineDepthReadOnly == nullptr)
        {
            Logger::Log::Error("WebGpuDevice: depth-read-only twin of graphics pipeline '{}' failed to "
                               "create; a read-only-depth pass drawing with it will be rejected",
                               gd.DebugName);
        }
    }

    wgpuShaderModuleRelease(vertexModule);
    if (fragmentModule != nullptr)
    {
        wgpuShaderModuleRelease(fragmentModule);
    }

    if (renderPipeline == nullptr)
    {
        wgpuPipelineLayoutRelease(pipelineLayout);
        SetLastGraphicsPipelineFailure("wgpuDeviceCreateRenderPipeline failed for " + gd.DebugName);
        return INVALID_PIPELINE_HANDLE;
    }

    WebGpuPipeline wrapper{};
    wrapper.renderPipeline = renderPipeline;
    wrapper.renderPipelineDepthReadOnly = renderPipelineDepthReadOnly;
    wrapper.pipelineLayout = pipelineLayout;
    wrapper.type = PipelineType::Graphics;
    wrapper.formatKey = fk;
    wrapper.pushConstantSize = pushConstantBytes;
    wrapper.pushConstantStagesMask = gd.PushConstants.StageMask;
    wrapper.descriptorSetCount = static_cast<uint32_t>(gd.DescriptorSetLayouts.size());
    wrapper.vertexBindings.reserve(gd.VertexBindings.size());
    for (const VertexInputBinding& binding : gd.VertexBindings)
    {
        wrapper.vertexBindings.push_back(binding.binding);
    }
    wrapper.debugName = gd.DebugName;
    for (const NamedPushConstantRange& range : gd.NamedPushConstantRanges)
    {
        wrapper.pushRanges.push_back({range.Name, range.Offset, range.Size, range.StageMask});
    }

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<PipelineTag>(m_Pipelines.Create(std::move(wrapper)));
}

PipelineHandle WebGpuDevice::CreateConcreteComputePipeline(const ComputePipelineDesc& cd)
{
    if (m_Device == nullptr || !cd.ComputeShader || cd.ComputeShader->empty())
    {
        return INVALID_PIPELINE_HANDLE;
    }

    WGPUShaderModule module = CreateShaderModule(*cd.ComputeShader, cd.DebugName.c_str());
    if (module == nullptr)
    {
        return INVALID_PIPELINE_HANDLE;
    }

    const uint32_t pushConstantBytes =
        PushConstantBlockBytes(cd.PushConstants, cd.NamedPushConstantRanges);
    WGPUPipelineLayout pipelineLayout =
        CreatePipelineLayout(cd.DescriptorSetLayouts, pushConstantBytes, cd.DebugName.c_str());
    if (pipelineLayout == nullptr)
    {
        wgpuShaderModuleRelease(module);
        return INVALID_PIPELINE_HANDLE;
    }

    WGPUComputePipelineDescriptor desc{};
    desc.label = WebGpu::MakeStringView(cd.DebugName.c_str());
    desc.layout = pipelineLayout;
    desc.compute.module = module;
    desc.compute.entryPoint = WebGpu::MakeStringView(kShaderEntryPoint);

    WGPUComputePipeline computePipeline = wgpuDeviceCreateComputePipeline(m_Device, &desc);
    wgpuShaderModuleRelease(module);

    if (computePipeline == nullptr)
    {
        wgpuPipelineLayoutRelease(pipelineLayout);
        Logger::Log::Error("WebGpuDevice: compute pipeline '{}' creation failed", cd.DebugName);
        return INVALID_PIPELINE_HANDLE;
    }

    WebGpuPipeline wrapper{};
    wrapper.computePipeline = computePipeline;
    wrapper.pipelineLayout = pipelineLayout;
    wrapper.type = PipelineType::Compute;
    wrapper.pushConstantSize = pushConstantBytes;
    wrapper.pushConstantStagesMask = cd.PushConstants.StageMask;
    wrapper.descriptorSetCount = static_cast<uint32_t>(cd.DescriptorSetLayouts.size());
    wrapper.debugName = cd.DebugName;
    for (const NamedPushConstantRange& range : cd.NamedPushConstantRanges)
    {
        wrapper.pushRanges.push_back({range.Name, range.Offset, range.Size, range.StageMask});
    }

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<PipelineTag>(m_Pipelines.Create(std::move(wrapper)));
}

void WebGpuDevice::DestroyPipeline(PipelineHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    WebGpuPipeline* pipeline = m_Pipelines.Get(ToGeneric(handle));
    if (pipeline == nullptr)
    {
        return;
    }
    if (pipeline->renderPipeline != nullptr)  wgpuRenderPipelineRelease(pipeline->renderPipeline);
    if (pipeline->renderPipelineDepthReadOnly != nullptr)
        wgpuRenderPipelineRelease(pipeline->renderPipelineDepthReadOnly);
    if (pipeline->computePipeline != nullptr) wgpuComputePipelineRelease(pipeline->computePipeline);
    if (pipeline->pipelineLayout != nullptr)  wgpuPipelineLayoutRelease(pipeline->pipelineLayout);
    pipeline->renderPipeline = nullptr;
    pipeline->renderPipelineDepthReadOnly = nullptr;
    pipeline->computePipeline = nullptr;
    pipeline->pipelineLayout = nullptr;
    m_Pipelines.Destroy(ToGeneric(handle));
}

WebGpuPipeline* WebGpuDevice::GetPipeline(PipelineHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Pipelines.Get(ToGeneric(handle));
}

// ---------------------------------------------------------------------------
// Push-constant introspection
// ---------------------------------------------------------------------------

uint32_t WebGpuDevice::GetPipelinePushConstantRangeCount(PipelineHandle pipeline) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const WebGpuPipeline* entry = m_Pipelines.Get(ToGeneric(pipeline));
    return entry != nullptr ? static_cast<uint32_t>(entry->pushRanges.size()) : 0u;
}

bool WebGpuDevice::GetPipelinePushConstantRangeInfo(PipelineHandle pipeline, uint32_t id,
                                                    PushConstantRangeInfo& outInfo) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const WebGpuPipeline* entry = m_Pipelines.Get(ToGeneric(pipeline));
    if (entry == nullptr || id >= entry->pushRanges.size())
    {
        return false;
    }
    const WebGpuPipeline::PushRange& range = entry->pushRanges[id];
    outInfo.id = id;
    outInfo.name = range.name.c_str();
    outInfo.offset = range.offset;
    outInfo.size = range.size;
    outInfo.stagesMask = range.stagesMask;
    return true;
}

bool WebGpuDevice::FindPipelinePushConstantRangeId(PipelineHandle pipeline, const char* name, uint32_t& outId) const
{
    if (name == nullptr)
    {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const WebGpuPipeline* entry = m_Pipelines.Get(ToGeneric(pipeline));
    if (entry == nullptr)
    {
        return false;
    }
    for (size_t i = 0; i < entry->pushRanges.size(); ++i)
    {
        if (entry->pushRanges[i].name == name)
        {
            outId = static_cast<uint32_t>(i);
            return true;
        }
    }
    return false;
}

bool WebGpuDevice::GetPipelinePushConstantInfo(PipelineHandle pipeline, PipelinePushConstantInfo& outInfo) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const WebGpuPipeline* entry = m_Pipelines.Get(ToGeneric(pipeline));
    if (entry == nullptr)
    {
        return false;
    }
    outInfo.size = entry->pushConstantSize;
    outInfo.stagesMask = entry->pushConstantStagesMask;
    return true;
}

} // namespace GameEngine::Rendering
