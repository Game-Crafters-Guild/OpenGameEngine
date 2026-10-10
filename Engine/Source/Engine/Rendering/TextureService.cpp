// TextureService implementation. Extracted from RenderServices — see
// TextureService.h for the service contract.

#include "Engine/Rendering/TextureService.h"
#include "CpuTextureSources.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Core/CpuProfiler.h"
#include "Engine/Rendering/IRenderFeature.h"

#include "Core/DebugMetrics.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Assets/AssetManager.h"
#include "Assets/BinaryAsset.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/TextureAlphaDecodeProbe.h"
#include "Assets/TextureAlphaProbe.h"
#include "Assets/TextureAsset.h"

#include "Assets/TextureCook.h"
#include "Engine/Rendering/EmbeddedImageDecoder.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Engine/Rendering/RetargetRenderFeature.h"
#include "Engine/Rendering/Pipeline/Nodes/ShadowMapNode.h"
#include "Engine/Rendering/Pipeline/Nodes/IBLGenNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ComputeShaderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/DepthPrepassNode.h"
#include "Engine/Rendering/Pipeline/Nodes/DepthResolveNode.h"
#include "Engine/Rendering/Pipeline/Nodes/AONode.h"
#include "Engine/Rendering/Pipeline/Nodes/AutoExposureNode.h"
#include "Engine/Rendering/Pipeline/Nodes/LightUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ViewParamsUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/HeightFogParamsUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/FullscreenShaderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/SkyRenderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/LensFlareRenderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/VolumetricFogNode.h"
#include "Engine/Rendering/Pipeline/Nodes/TransmissivePassNode.h"
#include "Engine/Rendering/Pipeline/Nodes/WorldRenderNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/BindlessResourceManager.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/CameraDerivation.h"
#include "Rendering/Core/CullingStrategy.h"
#include "Rendering/Core/FrustumCullingStrategy.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/BufferHelpers.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include "Rendering/Utils/CubeLutFileParser.h"
#include "Rendering/Utils/CubeLutGpuUpload.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Types/StringId.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Common/MatrixUtils.h"

#include "Rendering/Core/ThreadingUtils.h"

#include <algorithm>
#include <array>
#include "AssetCore/SharedFileRead.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "RenderServicesDetail.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

TextureService::TextureService() : m_CpuTextureSources(std::make_unique<CpuTextureSources>()) {}

TextureService::~TextureService()
{
    // Backstop for a host that skipped Shutdown: a running job must not outlive the members.
    DrainUploadJobs();
}

CpuTextureSource TextureService::CreateCpuTextureSource(const CpuTextureSourceDesc& desc)
{
    return m_CpuTextureSources->Create(desc);
}

void TextureService::ObserveCpuTextureWorld(CpuTextureScope scope)
{
    m_CpuTextureSources->ObserveWorld(scope);
}

void TextureService::FlushCpuTextureUploads()
{
    if (!m_Device || m_Device->GetDeviceHealth() != Rendering::DeviceHealth::Healthy)
        return;
    assert(std::this_thread::get_id() == m_RenderThreadId &&
           "CPU texture drain reached from a non-render thread");
    m_CpuTextureSources->Flush(*m_Device);
}

CpuTextureBinding TextureService::GetCpuTextureForView(uint64_t worldId, StringId name) const
{
    if (!m_Device)
        return {};
    const auto health = m_Device->GetDeviceHealth();
    if (health != Rendering::DeviceHealth::Healthy && health != Rendering::DeviceHealth::Hung)
        return {};
    return m_CpuTextureSources->Lookup(worldId, name);
}

TextureService::MaterialIndexingMode
TextureService::SelectMaterialIndexingMode(const Rendering::RenderingDeviceCapabilities& caps)
{
    return caps.supportsBindlessResources ? MaterialIndexingMode::Bindless
                                          : MaterialIndexingMode::Classic;
}

bool TextureService::Initialize(IDevice* device, MaterialRegistry& materials,
                                const Rendering::RendererProfile& profile)
{
    m_Device = device;
    m_Materials = &materials;
    m_Profile = profile;
    m_RenderThreadId = std::this_thread::get_id();
    if (!m_Device)
        return false;
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        m_UploadJobsStopped = false;
    }

    m_CpuTextureSources->Open();

    // Material indexing mode is selected from device capabilities: Bindless
    // reaches textures through one global unbounded array indexed per-instance,
    // Classic through a per-material bind group (MaterialBindingCache) whose
    // layout the compat shader profile declares. Material params ride the
    // shared SSBO in both modes.
    const auto caps = m_Device->GetCapabilities();
    m_IndexingMode = SelectMaterialIndexingMode(caps);
    // Classic is a supported path, not a failure, but it is a different renderer from the
    // one a full-profile device runs — and nothing else in the boot log names it. Say which
    // mode this device took and what put it there, so a materials question asked about a
    // machine can be answered without reproducing on it.
    if (m_IndexingMode == MaterialIndexingMode::Classic)
        Logger::Log::Warning(
            "TextureService: this device reports no bindless descriptor indexing — materials "
            "bind through classic per-material descriptor sets instead of the global bindless "
            "texture array.");

    // Publish the device's block-compression support to the asset layer:
    // KTX2/BasisU decode (worker threads) picks its libktx transcode target
    // from this — BC7 when supported, RGBA32 otherwise (unchanged pipeline).
    TextureAsset::SetGpuTranscodeTarget(caps.supportsTextureCompressionBC
                                            ? TextureGpuTranscodeTarget::BC7
                                            : TextureGpuTranscodeTarget::RGBA32);

    return CreateDeviceScopedResources();
}

// Creates (or, after a device-lost rebuild, recreates) every device-scoped GPU
// resource this service owns: the material indexing state for the active mode
// (Bindless: manager + descriptor set + default indices pushed into the
// MaterialRegistry; Classic: the per-material bind-group cache), the shared
// sampler cache, and the default fallback textures + identity LUTs — which both
// modes need. Split out of Initialize so ReprovisionAfterDeviceRebuild can
// re-run exactly this on the rebuilt device without disturbing the retained
// CPU-side ref graph / decode cache. Assumes m_Device and m_Materials are set.
bool TextureService::CreateDeviceScopedResources()
{
    Rendering::IDevice* const dev = m_Device;

    if (m_IndexingMode == MaterialIndexingMode::Bindless)
        CreateBindlessResources();

    CreateDefaultTexturesAndLuts(dev);

    if (m_IndexingMode == MaterialIndexingMode::Bindless)
        PublishBindlessDefaults();
    else
        InitializeMaterialBindingCache();

    return true;
}

// Bindless-only provisioning: the descriptor-indexing manager and the single
// global texture/sampler set every material samples through on the full profile.
void TextureService::CreateBindlessResources()
{
    const auto caps = m_Device->GetCapabilities();

    m_Bindless = std::make_unique<Rendering::BindlessResourceManager>(m_Device);
    {
        const uint32_t maxTextures = caps.maxBindlessTextures;
        // Update the file-local bindless texture limit so pipeline layouts and
        // descriptor set layouts all use the device capability-clamped value.
        kMaxBindlessTextures = maxTextures;
        // Publish LAST: the manager is fully constructed above, so a reader that
        // observes enabled=true here (during a reprovision re-enable) finds a valid
        // m_Bindless. release/acquire is unnecessary — relaxed suffices because the
        // only cross-thread reader re-checks under m_BindlessMutex before use.
        m_BindlessEnabled.store(m_Bindless->Initialize(maxTextures, 4096),
                                std::memory_order_relaxed);
    }

    // Create the global bindless texture descriptor set (persistent, not per-frame).
    // On the DB path, transient=false routes through VulkanDescriptorBufferPool::
    // AllocatePersistent so the set's backing memory is never rewound by
    // BeginFrameReset. On the pool path, the persistent pool set keeps its
    // UpdateAfterBind flag so cross-frame writes stay legal — the Vulkan backend
    // strips UAB from the layout creation when it's routing through DB, so both
    // paths coexist cleanly.
    if (m_BindlessEnabled.load(std::memory_order_relaxed))
    {
        auto bindlessLayout = GetBindlessTextureSetLayout(std::max<uint32_t>(1u, caps.maxBindlessTextures));

        Rendering::DescriptorSetDesc bindlessDesc{};
        bindlessDesc.layout = bindlessLayout;
        bindlessDesc.debugName = "BindlessTextureSet";
        bindlessDesc.transient = false;
        bindlessDesc.poolFlags = Rendering::DescriptorPoolFlags::UpdateAfterBind;
        m_BindlessTextureSet = m_Device->CreateDescriptorSet(bindlessDesc);

        // Populate binding 1 (the shared sampler array) once — one entry per
        // SamplerPreset, selected per texture-slot via the material's packed
        // SamplerIndices. This binding is NOT update-after-bind, so it must be
        // fully written here, before the set is ever bound during recording.
        if (m_BindlessTextureSet.IsValid())
        {
            for (uint32_t i = 0; i < static_cast<uint32_t>(Rendering::SamplerPreset::kCount); ++i)
            {
                auto sampler = GetSampler(static_cast<Rendering::SamplerPreset>(i));
                if (sampler.IsValid())
                    m_Device->UpdateSamplerBinding(m_BindlessTextureSet, 1, sampler, i);
            }
        }
    }
}

// A 1x1 RGBA8 cube, every face opaque black, in ShaderResource. Its default view is a cube
// (CubeCompatible, six layers), the shape a textureCube slot declares.
static Rendering::TextureHandle CreateBlackCube1x1(Rendering::IDevice* dev)
{
    constexpr uint32_t kFaceCount = 6;
    Rendering::TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.mipLevels = 1;
    td.arrayLayers = kFaceCount;
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource) |
               static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
    td.flags = Rendering::TextureCreateFlags::CubeCompatible;
    td.persistent = true;
    td.debugName = "DefaultBlackCube1x1";
    const Rendering::TextureHandle cube = dev->CreateTexture(td);
    if (!cube.IsValid())
    {
        Logger::Log::Error("TextureService: the DefaultBlackCube1x1 slot default could not be created");
        return cube;
    }

    // One staging row serves all six faces; its pitch is padded to the device's copy alignment
    // (256 on WebGPU), and the padding is never read.
    const size_t rowPitch =
        std::max<size_t>(4u, dev->GetCapabilities().textureCopyRowPitchAlignment);
    std::vector<uint8_t> row(rowPitch, 0u);
    row[3] = 0xFFu; // opaque black
    const Rendering::BufferHandle staging = dev->CreateUploadBuffer(rowPitch, "DefaultBlackCubeStaging");
    auto cl = dev->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
    if (!staging.IsValid() || !cl)
    {
        Logger::Log::Error("TextureService: the DefaultBlackCube1x1 slot default could not be seeded");
        if (staging.IsValid())
            dev->DestroyBuffer(staging);
        dev->DestroyTexture(cube);
        return {};
    }
    dev->UpdateBuffer(staging, 0, rowPitch, row.data());
    cl->Begin();
    cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
        cube, Rendering::ResourceState::Undefined, Rendering::ResourceState::CopyDest, 0, 1, 0, kFaceCount));
    for (uint32_t face = 0; face < kFaceCount; ++face)
        cl->CopyBufferToTextureSubresource(staging, cube, 0, face, 1, 1, 0, rowPitch);
    cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
        cube, Rendering::ResourceState::CopyDest, Rendering::ResourceState::ShaderResource, 0, 1, 0, kFaceCount));
    cl->End();
    dev->ExecuteCommandLists(std::vector<Rendering::CommandList*>{cl.get()});
    dev->DestroyBuffer(staging);
    return cube;
}

// Default fallback textures and identity LUTs.
// Both indexing modes need these: Bindless registers them for stable indices,
// Classic writes them into every unassigned bind-group slot.
void TextureService::CreateDefaultTexturesAndLuts(Rendering::IDevice* dev)
{
    {
        auto createDefaultTexture = [&](const char* name, uint32_t width, uint32_t height,
                                        Rendering::TextureCreateFlags flags = {}) -> Rendering::TextureHandle
        {
            Rendering::TextureDesc td{};
            td.width = width;
            td.height = height;
            td.mipLevels = 1;
            td.arrayLayers = 1;
            td.format = static_cast<uint32_t>(Rendering::TextureFormat::RGBA8_UNORM);
            td.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource)
                     | static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
            td.flags = flags;
            td.persistent = true;
            td.debugName = name;
            return dev->CreateTexture(td);
        };

        m_DefaultWhiteTexture = createDefaultTexture("DefaultWhite1x1", 1, 1);
        m_DefaultBlackTexture = createDefaultTexture("DefaultBlack1x1", 1, 1);
        m_DefaultFlatNormalTexture = createDefaultTexture("DefaultFlatNormal1x1", 1, 1);
        m_DefaultLensFlareTexture = createDefaultTexture("DefaultLensFlare64", 64, 64);
        // ForceArrayView: the one layer is sampled through a 2D-array view.
        m_DefaultWhiteArrayTexture =
            createDefaultTexture("DefaultWhite1x1Array", 1, 1, Rendering::TextureCreateFlags::ForceArrayView);
        m_DefaultBlackCubeTexture = CreateBlackCube1x1(dev);

        // Batch-upload the default textures in a single command list submission.
        // Note: SubmitTextureUploads (async transfer queue) causes VUID errors because
        // the implementation transitions textures with shader-stage barriers that are
        // invalid on the transfer queue. Use the synchronous graphics-queue path instead.
        uint32_t whitePixel = 0xFFFFFFFFu;
        uint32_t blackPixel = 0xFF000000u;
        uint32_t normalPixel = 0xFFFF8080u;
        std::vector<uint32_t> lensFlarePixels(64u * 64u);
        for (uint32_t y = 0; y < 64u; ++y)
        {
            for (uint32_t x = 0; x < 64u; ++x)
            {
                const float nx = (static_cast<float>(x) + 0.5f) / 64.0f * 2.0f - 1.0f;
                const float ny = (static_cast<float>(y) + 0.5f) / 64.0f * 2.0f - 1.0f;
                const float r = std::sqrt(nx * nx + ny * ny);
                const float core = std::clamp(1.0f - r * 2.9f, 0.0f, 1.0f);
                const float halo = std::clamp(1.0f - r, 0.0f, 1.0f);
                const float alpha = std::clamp(core * core + halo * halo * halo * 0.45f,
                                               0.0f, 1.0f);
                const uint32_t a = static_cast<uint32_t>(alpha * 255.0f + 0.5f);
                lensFlarePixels[y * 64u + x] =
                    (a << 24u) | (255u << 16u) | (255u << 8u) | 255u;
            }
        }
        Rendering::TextureUploadEntry defaultUploads[] = {
            {m_DefaultWhiteTexture, &whitePixel, 1, 1, 1, 4, 0},
            {m_DefaultBlackTexture, &blackPixel, 1, 1, 1, 4, 0},
            {m_DefaultFlatNormalTexture, &normalPixel, 1, 1, 1, 4, 0},
            {m_DefaultLensFlareTexture, lensFlarePixels.data(), 64, 64, 1, 64u * 4u, 0},
            {m_DefaultWhiteArrayTexture, &whitePixel, 1, 1, 1, 4, 0},
        };
        Rendering::UploadTexturesBatched(dev, defaultUploads, std::size(defaultUploads), "DefaultTexStaging");

        // 2x2x2 identity 3D LUT + 2x3 1D strip (linear RGB knots) for always-valid
        // bindings. Float16: the knots are exactly 0 and 1, and the LUT samplers
        // filter — WebGPU only filters float32 behind the float32-filterable
        // feature, while float16 filters everywhere.
        {
            constexpr uint16_t kHalfZero = 0x0000u;
            constexpr uint16_t kHalfOne = 0x3C00u;
            constexpr uint32_t n3 = 2u;
            std::vector<uint16_t> rgba3(static_cast<size_t>(n3) * n3 * n3 * 4u);
            for (uint32_t b = 0; b < n3; ++b)
            {
                for (uint32_t g = 0; g < n3; ++g)
                {
                    for (uint32_t r = 0; r < n3; ++r)
                    {
                        const size_t idx = ((static_cast<size_t>(b) * n3 + g) * n3 + r) * 4u;
                        rgba3[idx + 0u] = r != 0u ? kHalfOne : kHalfZero;
                        rgba3[idx + 1u] = g != 0u ? kHalfOne : kHalfZero;
                        rgba3[idx + 2u] = b != 0u ? kHalfOne : kHalfZero;
                        rgba3[idx + 3u] = kHalfOne;
                    }
                }
            }

            Rendering::TextureDesc td3{};
            td3.width = n3;
            td3.height = n3;
            td3.depth = n3;
            td3.mipLevels = 1;
            td3.arrayLayers = 1;
            td3.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
            td3.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource)
                      | static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
            td3.persistent = true;
            td3.debugName = "IdentityLut3D";
            m_IdentityLut3D = dev->CreateTexture(td3);
            if (m_IdentityLut3D.IsValid())
            {
                const size_t rowPitch3 = static_cast<size_t>(n3) * 4u * sizeof(uint16_t);
                Rendering::UploadTexture3D(dev, m_IdentityLut3D, rgba3.data(), n3, n3, n3, rowPitch3, 0, "IdentityLut3DStaging");
            }

            constexpr uint32_t n1 = 2u;
            std::vector<uint16_t> rgba1(static_cast<size_t>(n1) * 3u * 4u, kHalfZero);
            for (uint32_t row = 0; row < 3u; ++row)
            {
                rgba1[(static_cast<size_t>(row) * n1 + 0u) * 4u + 3u] = kHalfOne;
                rgba1[(static_cast<size_t>(row) * n1 + 1u) * 4u + 0u] = kHalfOne;
                rgba1[(static_cast<size_t>(row) * n1 + 1u) * 4u + 3u] = kHalfOne;
            }

            Rendering::TextureDesc td1{};
            td1.width = n1;
            td1.height = 3u;
            td1.depth = 1;
            td1.mipLevels = 1;
            td1.arrayLayers = 1;
            td1.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
            td1.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource)
                      | static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
            td1.persistent = true;
            td1.debugName = "IdentityLut1DStrip";
            m_IdentityLut1DStrip = dev->CreateTexture(td1);
            if (m_IdentityLut1DStrip.IsValid())
            {
                const size_t rowPitch1 = static_cast<size_t>(n1) * 4u * sizeof(uint16_t);
                Rendering::UploadTexture2D(dev, m_IdentityLut1DStrip, rgba1.data(), n1, 3u, rowPitch1, "IdentityLut1DStaging");
            }
        }
    }
}

// Register the default textures in the bindless array so they have stable
// indices, and hand those indices to the registry as every material's
// cold-start slot values.
void TextureService::PublishBindlessDefaults()
{
    m_DefaultWhiteBindlessIndex = GetBindlessIndex(m_DefaultWhiteTexture);
    m_DefaultBlackBindlessIndex = GetBindlessIndex(m_DefaultBlackTexture);
    m_DefaultFlatNormalBindlessIndex = GetBindlessIndex(m_DefaultFlatNormalTexture);
    assert(m_DefaultWhiteBindlessIndex != 0u && m_DefaultBlackBindlessIndex != 0u
           && m_DefaultFlatNormalBindlessIndex != 0u
           && "Default bindless texture registration produced the sentinel "
              "index 0 — every Material::InitBindlessDefaults call will "
              "silently re-introduce the slot-0-aliasing bug.");
    m_Materials->SeedTextureSlotDefaults(
        m_DefaultWhiteBindlessIndex,
        m_DefaultFlatNormalBindlessIndex,
        m_DefaultBlackBindlessIndex);
}

// Classic mode: hand the bind-group cache the fallbacks it substitutes for
// unassigned slots.
void TextureService::InitializeMaterialBindingCache()
{
    // Registration still has to be seeded before any material exists, but the
    // index lane it seeds is unread on this profile — the bind group carries
    // the default textures instead.
    m_Materials->SeedTextureSlotDefaults(0u, 0u, 0u);

    MaterialBindingCache::SlotDefaults defaults{};
    defaults.White = m_DefaultWhiteTexture;
    defaults.Black = m_DefaultBlackTexture;
    defaults.FlatNormal = m_DefaultFlatNormalTexture;
    m_MaterialBindings.Initialize(m_Device, defaults,
                                  GetSampler(Rendering::SamplerPreset::LinearRepeat));
}

void TextureService::ReprovisionAfterDeviceRebuild()
{
    if (!m_Device || !m_Materials)
        return;

    m_CpuTextureSources->Reprovision();

    // Snapshot the retained material<->texture ref graph, then drop the stale
    // in-flight load state. The rebuild teardown already freed every VkImage /
    // view / sampler / descriptor set this service created, so the cached handles
    // are dead — forget them WITHOUT Destroy* below (a Destroy would double-free a
    // recycled slot). The ref graph + CPU decode cache are PRESERVED: they drive
    // the re-upload replay.
    struct RefEntry
    {
        GUID Mat;
        StringId Slot;
        GUID Tex;
    };
    std::vector<RefEntry> refSnapshot;
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        for (const auto& [matGuid, slots] : m_MaterialTextureRefs)
            for (const auto& [slotName, texGuid] : slots)
                refSnapshot.push_back({matGuid, slotName, texGuid});

        // In-flight loads referenced the dead device's staging; withdraw them so
        // the replay re-kicks cleanly. Clear the ref maps here — BindMaterialTextureRef
        // repopulates them during replay (no double-tracking).
        WithdrawTextureLoadsLocked();
        m_PendingGpuUploads.clear();
        m_PendingTextureBinds.clear();
        m_PendingTextureReupload.clear();
        m_TextureLoadsKicked.clear();
        m_MaterialTextureRefs.clear();
        m_TextureToMaterialBindings.clear();
    }

    // Forget the stale GPU caches (dead handles — no Destroy*). These are not
    // bindless-mutex-protected (GetBindlessIndex never touches them); the GPU
    // texture cache carries its own lock, so clear them outside m_BindlessMutex.
    m_TextureGPUCache.Clear();
    m_CubeLutGpuCache.clear();
    m_CubeLutWarnedFailureGuids.clear();

    // Bindless teardown hardening (3a rider). Disable bindless FIRST so a concurrent
    // off-render-thread GetBindlessIndex (terrain streaming) bails at its lockless
    // gate rather than dereferencing the manager mid-reset. Then take m_BindlessMutex
    // across the whole teardown so a reader inside a mutex-guarded section (cache
    // lookup/insert, GetSampler) observes an all-or-nothing swap — never a
    // half-cleared cache next to a live manager. The lock CANNOT extend across
    // CreateBindlessAndDefaults below: that path (and the material re-bake + ref-graph
    // replay) call GetBindlessIndex, which re-acquires m_BindlessMutex — holding it
    // here would self-deadlock. See the precondition note after the replay.
    m_BindlessEnabled.store(false, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_BindlessMutex);
        // F6: keyed on the handle .id, so a stale entry would COLLIDE with a
        // reissued id (serving the wrong texture) — a miss is not enough.
        m_BindlessTextureIndexCache.clear();
        m_DefaultWhiteTexture = {};
        m_DefaultBlackTexture = {};
        m_DefaultFlatNormalTexture = {};
        m_DefaultLensFlareTexture = {};
        m_DefaultWhiteArrayTexture = {};
        m_DefaultBlackCubeTexture = {};
        m_IdentityLut3D = {};
        m_IdentityLut1DStrip = {};
        m_DefaultWhiteBindlessIndex = 0u;
        m_DefaultBlackBindlessIndex = 0u;
        m_DefaultFlatNormalBindlessIndex = 0u;
        for (auto& s : m_SamplerCache)
            s = {};
        // Race losers still parked for retirement belong to the manager and the
        // device being torn down here — drop them rather than destroying handles
        // that no longer resolve.
        m_PendingGpuDiscards.clear();
        m_BindlessTextureSet = {};
        m_Bindless.reset();
    }

    // Recreate the active mode's material plumbing, samplers, defaults + LUTs,
    // and (Bindless) push the fresh default indices into the registry (F6:
    // defaults FIRST). Re-enables m_BindlessEnabled at the end of its bindless
    // init; readers see enabled=false from the teardown above until then.
    if (!CreateDeviceScopedResources())
    {
        Logger::Log::Error(
            "TextureService: material-indexing/default re-provision FAILED after device rebuild");
        return;
    }

    // Re-bake every registered material's default slots to the fresh default
    // indices — their previously-baked indices are dead. MUST run AFTER the
    // defaults exist (F6 ordering).
    m_Materials->ForEachMutable(
        [&](const GUID&, Material& mat)
        {
            mat.InitBindlessDefaults(m_DefaultWhiteBindlessIndex,
                                     m_DefaultFlatNormalBindlessIndex,
                                     m_DefaultBlackBindlessIndex);
        });

    // Replay the ref graph: re-upload each referenced texture and re-bind it onto
    // its material slot. Assigned slots overwrite the just-baked defaults;
    // unassigned slots keep them. BindMaterialTextureRef re-tracks into the
    // cleared maps, so the ref graph is rebuilt exactly.
    for (const auto& ref : refSnapshot)
    {
        if (Material* mat = m_Materials->Find(ref.Mat))
            BindMaterialTextureRef(mat, ref.Mat, ref.Slot, ref.Tex);
    }

    Logger::Log::Info(
        "TextureService: re-provisioned bindless + default textures; replayed {} material texture bindings",
        refSnapshot.size());

    // Precondition + residual (3a rider). The atomic enabled flag + the
    // mutex-guarded teardown above close the two concrete hazards the rider named
    // (a data race on m_BindlessEnabled, and mutating bindless state outside
    // m_BindlessMutex). One window remains open by construction: a reader that
    // read enabled=true just before the teardown store, then dereferences m_Bindless
    // in GetBindlessIndex's post-cache RegisterTextureBindless path — which runs
    // OUTSIDE m_BindlessMutex by design (nested-lock avoidance on the render hot
    // path). Fully closing it would require holding m_BindlessMutex across
    // RegisterTextureBindless, serializing the hot registration path — deferred.
    // Until then the precondition is: this runs on the render thread during the
    // AwaitingReprovision suppression window, and an off-render-thread bindless
    // consumer (terrain streaming) must not call GetBindlessIndex concurrently with
    // a rebuild — it either quiesces or synchronizes via the shared device-rebuild
    // lock. Nothing does so today (rendering is suppressed here).
}

void TextureService::WithdrawTextureLoadsLocked()
{
    for (auto& [guid, handle] : m_TextureLoadHandles)
        handle.Cancel();
    m_TextureLoadHandles.clear();
    for (auto& [guid, cancel] : m_UploadJobCancels)
        cancel->store(true, std::memory_order_relaxed);
    m_UploadJobCancels.clear();
}

void TextureService::CancelPendingLoads()
{
    // Withdrawing each request is what stops its completion callback from
    // running into now-tearing-down state; dropping the handles alone does not.
    // Worker callbacks that already started take the mutex briefly here; they
    // exit before set/Find run because we hold the lock until we erase the queues.
    std::lock_guard lock(m_PendingTexturesMutex);
    WithdrawTextureLoadsLocked();
    m_PendingGpuUploads.clear();
    m_PendingTextureBinds.clear();
    m_TextureLoadsKicked.clear();
    m_MaterialTextureRefs.clear();
    m_TextureToMaterialBindings.clear();
}

void TextureService::Shutdown()
{
    // Before anything the jobs could touch goes: no upload job outlives the service.
    DrainUploadJobs();
    CancelPendingLoads();
    m_CpuTextureSources->Shutdown(m_Device);

    if (m_Device)
    {
        if (m_DefaultWhiteTexture.IsValid())
            m_Device->DestroyTexture(m_DefaultWhiteTexture);
        if (m_DefaultBlackTexture.IsValid())
            m_Device->DestroyTexture(m_DefaultBlackTexture);
        if (m_DefaultFlatNormalTexture.IsValid())
            m_Device->DestroyTexture(m_DefaultFlatNormalTexture);
        if (m_DefaultLensFlareTexture.IsValid())
            m_Device->DestroyTexture(m_DefaultLensFlareTexture);
        if (m_DefaultWhiteArrayTexture.IsValid())
            m_Device->DestroyTexture(m_DefaultWhiteArrayTexture);
        if (m_DefaultBlackCubeTexture.IsValid())
            m_Device->DestroyTexture(m_DefaultBlackCubeTexture);
        for (auto& entry : m_CubeLutGpuCache)
        {
            CubeLutGpuBindingState& lut = entry.second;
            if (lut.Lut3D.IsValid() && lut.Lut3D != m_IdentityLut3D)
                m_Device->DestroyTexture(lut.Lut3D);
            if (lut.Lut1DStrip.IsValid() && lut.Lut1DStrip != m_IdentityLut1DStrip)
                m_Device->DestroyTexture(lut.Lut1DStrip);
        }
        m_CubeLutGpuCache.clear();
        m_CubeLutWarnedFailureGuids.clear();
        if (m_IdentityLut3D.IsValid())
            m_Device->DestroyTexture(m_IdentityLut3D);
        if (m_IdentityLut1DStrip.IsValid())
            m_Device->DestroyTexture(m_IdentityLut1DStrip);
        // Destroy cached preset samplers.
        for (auto& s : m_SamplerCache)
        {
            if (s.IsValid())
                m_Device->DestroySampler(s);
            s = {};
        }
        m_DefaultWhiteTexture = {};
        m_DefaultBlackTexture = {};
        m_DefaultFlatNormalTexture = {};
        m_DefaultLensFlareTexture = {};
        m_DefaultWhiteArrayTexture = {};
        m_DefaultBlackCubeTexture = {};
        m_IdentityLut3D = {};
        m_IdentityLut1DStrip = {};

        for (auto& [guid, texHandle] : m_TextureGPUCache.Take())
        {
            if (texHandle.IsValid())
                m_Device->DestroyTexture(texHandle);
        }

        if (m_BindlessTextureSet.IsValid())
        {
            m_Device->DestroyDescriptorSet(m_BindlessTextureSet);
            m_BindlessTextureSet = {};
        }
        // Classic bind groups. Inside the device-live scope: the cache calls
        // back into the device to release its sets.
        m_MaterialBindings.Shutdown();
        // Race losers parked by GetBindlessIndex/GetSampler/GetOrUpload that no
        // frame drained. Same reason as the cache sweep below: their views would
        // otherwise trip the device's stray-view warning, and a parked duplicate
        // texture is by definition absent from the cache swept above.
        for (const auto& d : m_PendingGpuDiscards)
        {
            if (d.View.IsValid())
                m_Device->DestroyTextureView(d.View);
            if (d.Sampler.IsValid())
                m_Device->DestroySampler(d.Sampler);
            if (d.Texture.IsValid())
                m_Device->DestroyTexture(d.Texture);
        }
        m_PendingGpuDiscards.clear();

        // Free any custom TextureViews held by cache entries before dropping the
        // map. Without this the device's stray-view sweep at shutdown logs a
        // "view escaped tracking" warning for each PCSS cascade (and any other
        // custom-aspect/slice/mip registrations).
        for (auto& [key, entry] : m_BindlessTextureIndexCache)
        {
            if (entry.bindlessView.IsValid())
                m_Device->DestroyTextureView(entry.bindlessView);
        }
    }
    m_DerivedTextureGuidsByParent.clear();
    m_EmbeddedContentRefs.clear();
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        m_EmbeddedSlotsByMaterial.clear();
    }
    m_EmbeddedDecodeCache.clear();
    m_BindlessTextureIndexCache.clear();
    m_BindlessEnabled.store(false, std::memory_order_relaxed);
    m_Bindless.reset();
    m_Materials = nullptr;
    m_Device = nullptr;
}

namespace
{
// Write a texture's color-space tag into the AssetDatabase unless an explicit one is already
// there, so the extension-based sRGB guess cannot corrupt non-color data. Returns true only when
// it wrote a NEW tag — the caller then refreshes any upload made under the old classification.
//
// TextureColorSpace::Unknown means "no opinion" and writes nothing. That is what a COLOUR texture
// wants: its extension guess is already right (sRGB for LDR, linear for HDR), and pinning sRGB
// here would mis-tag an HDR source.
bool EnsureTextureColorSpaceTagged(const GUID& texGuid, TextureColorSpace colorSpace)
{
    if (texGuid.IsNull() || colorSpace == TextureColorSpace::Unknown)
        return false;
    auto& registry = GameEngine::EngineCore::GetInstance().GetAssetManager().GetRegistry();
    // A packaged source's import inputs must stay identical to the build's
    // cooked artifact key, including when no explicit color-space tag exists.
    if (!registry.GetDerivedArtifactPolicy(texGuid).InfersTextureImportSettings)
        return false;
    AssetMetadata texMeta{};
    if (!registry.TryGetAssetMetadata(texGuid, texMeta) || texMeta.Path.empty())
        return false;
    std::string existing;
    TextureColorSpace parsed;
    if (registry.TryGetMetaValue(texMeta.Path, kTextureColorSpaceMetaKey, existing) &&
        ParseTextureColorSpaceMeta(existing, parsed))
    {
        return false; // explicit override already present (user or prior import) — respect it
    }
    // Report the write, not the intent: a mount that refuses kv (immutable
    // package) never grows the tag, and a caller told otherwise would evict
    // the upload on every bind waiting for a row that can never appear.
    return registry.SetMetaValue(texMeta.Path, kTextureColorSpaceMetaKey,
                                 TextureColorSpaceMetaValue(colorSpace));
}

// Auto-classify a data-slot texture (normal / metallic-roughness / AO / ...) as
// linear the first time a material binds it. Skips embedded textures (whose color space is
// already handled by the embedded upload path).
bool EnsureDataTextureColorSpaceTagged(const GUID& texGuid, StringId slotName)
{
    return EnsureTextureColorSpaceTagged(texGuid, IsLinearTextureSlot(slotName)
                                                      ? TextureColorSpace::Linear
                                                      : TextureColorSpace::Unknown);
}

// Who needs a usage the stored one cannot serve, for the conflict warning: the material slot
// binding the texture, or a declaration made without a material (DeclareTextureClassification).
std::string DescribeUsageBinding(const GUID& materialGuid, StringId slotName)
{
    if (materialGuid.IsNull())
        return "a declaration without a material";
    AssetMetadata materialMeta{};
    auto& registry = GameEngine::EngineCore::GetInstance().GetAssetManager().GetRegistry();
    const std::string material = registry.TryGetAssetMetadata(materialGuid, materialMeta) && !materialMeta.Path.empty()
                                     ? materialMeta.Path.string()
                                     : materialGuid.ToString();
    return "slot '" + std::string(TextureCookUsageSlotName(slotName)) + "' of material '" + material + "'";
}
} // namespace

// Widen a texture's cook usage in the AssetDatabase to serve one more binding
// (WidenTextureCookUsage). The stored tag is never narrowed; a binding no cook can
// serve together with it keeps the tag and warns once per texture. The warning names
// the binding that needs the new usage (materialGuid and slotName, or a declaration
// when materialGuid is null); the binding that set the stored usage cannot be named,
// because the tag does not record who wrote it. Returns true only when it wrote a NEW
// tag, the caller's signal to refresh the upload and re-cook the payload under the new
// usage. TextureCookUsage::Auto means "no opinion" and writes nothing — the cook then
// leaves the payload uncompressed, which is the right answer for a texture nobody has
// classified.
bool TextureService::EnsureTextureCookUsageTagged(const GUID& texGuid, TextureCookUsage usage,
                                                  const GUID& materialGuid, StringId slotName)
{
    if (texGuid.IsNull() || usage == TextureCookUsage::Auto)
        return false;
    auto& registry = GameEngine::EngineCore::GetInstance().GetAssetManager().GetRegistry();
    if (!registry.GetDerivedArtifactPolicy(texGuid).InfersTextureImportSettings)
        return false;
    AssetMetadata texMeta{};
    if (!registry.TryGetAssetMetadata(texGuid, texMeta) || texMeta.Path.empty())
        return false;
    std::string existing;
    TextureCookUsage current = TextureCookUsage::Auto;
    if (registry.TryGetMetaValue(texMeta.Path, kTextureUsageMetaKey, existing))
        ParseTextureCookUsageMeta(existing, current);
    const std::optional<TextureCookUsage> widened = WidenTextureCookUsage(current, usage);
    if (!widened)
    {
        {
            std::lock_guard lock(m_ReportedUsageConflictsMutex);
            if (!m_ReportedUsageConflicts.insert(texGuid).second)
                return false;
        }
        Logger::Log::Warning("Texture '{}' has cook usage '{}', and {} needs '{}': one cooked texture cannot serve "
                             "both, so it keeps '{}'. Use a separate texture there, or pick the usage it keeps in "
                             "the texture inspector.",
                             texMeta.Path.string(), TextureCookUsageMetaValue(current),
                             DescribeUsageBinding(materialGuid, slotName), TextureCookUsageMetaValue(usage),
                             TextureCookUsageMetaValue(current));
        return false;
    }
    if (*widened == current)
        return false;
    // Same rule as the colour-space tag: a refused write must not be reported
    // as a new tag, or the caller re-cooks on every bind.
    if (!registry.SetMetaValue(texMeta.Path, kTextureUsageMetaKey, TextureCookUsageMetaValue(*widened)))
        return false;
    if (current != TextureCookUsage::Auto)
        Logger::Log::Info("Texture '{}' cook usage widened from '{}' to '{}' for a slot that needs '{}'",
                          texMeta.Path.string(), TextureCookUsageMetaValue(current),
                          TextureCookUsageMetaValue(*widened), TextureCookUsageMetaValue(usage));
    return true;
}

void TextureService::DeclareTextureClassification(const GUID& texGuid,
                                                  TextureColorSpace colorSpace,
                                                  TextureCookUsage usage)
{
    const bool newColorTag = EnsureTextureColorSpaceTagged(texGuid, colorSpace);
    const bool newUsageTag = EnsureTextureCookUsageTagged(texGuid, usage, GUID::Null(), StringId{});
    if (!newColorTag && !newUsageTag)
        return;

    // Already-resident textures were uploaded under the OLD classification, so they have to come
    // back under the new one. RequestReupload, not Evict: this runs on ECS extraction workers,
    // where Evict's material-registry walk and immediate DestroyTexture are out of bounds. The
    // parked swap also keeps the old upload sampled until the new one is ready — no default-
    // texture frame.
    if (m_TextureGPUCache.Contains(texGuid))
        RequestReupload(texGuid);
    // A usage change moves the cooked artifact's format (BC5/BC7), which only a re-cook produces.
    if (newUsageTag)
        RequestRecook(texGuid);
}

// The texture asset's own filter choice (kTextureFilterMetaKey), applied to the
// slot it binds to; Inherit (or no metadata) hands the slot back to the
// material's TextureFilter. Both bind entry points call this so a texture set
// to Point in its inspector samples that way from every material, including
// after a recook re-upload.
static void ApplyTextureFilterMeta(Material* mat, StringId slotName, const GUID& texGuid)
{
    if (!mat || texGuid.IsNull())
        return;
    std::optional<Rendering::SamplerPreset> preset;
    AssetMetadata meta{};
    std::string value;
    auto& registry = GameEngine::EngineCore::GetInstance().GetAssetManager().GetRegistry();
    if (registry.TryGetAssetMetadata(texGuid, meta) && !meta.Path.empty() &&
        registry.TryGetMetaValue(meta.Path, kTextureFilterMetaKey, value))
    {
        TextureFilterMode mode = TextureFilterMode::Inherit;
        if (ParseTextureFilterMeta(value, mode))
        {
            switch (mode)
            {
            case TextureFilterMode::Point:     preset = Rendering::SamplerPreset::PointRepeat; break;
            case TextureFilterMode::Bilinear:  preset = Rendering::SamplerPreset::BilinearRepeat; break;
            case TextureFilterMode::Trilinear: preset = Rendering::SamplerPreset::LinearRepeat; break;
            default: break;
            }
        }
    }
    mat->SetSlotSamplerOverride(slotName, preset);
}

void TextureService::BindMaterialTextureRef(Material* mat, const GUID& matGuid,
                                            StringId slotName, const GUID& texGuid)
{
    BindMaterialTextureSlot(mat, matGuid, slotName, texGuid, TextureLoadRoute::AssetManagerLoad);
}

void TextureService::BindMaterialTextureSlot(Material* mat, const GUID& matGuid, StringId slotName,
                                             const GUID& texGuid, TextureLoadRoute route)
{
    // This bind supersedes any earlier one for the slot that is still waiting on its load:
    // left queued, that load would land later and overwrite the slot with the old texture.
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        m_PendingTextureBinds.erase(
            std::remove_if(m_PendingTextureBinds.begin(), m_PendingTextureBinds.end(),
                [&](const PendingTextureBind& b) { return b.MaterialGuid == matGuid && b.SlotName == slotName; }),
            m_PendingTextureBinds.end());
    }
    ApplyTextureFilterMeta(mat, slotName, texGuid);
    // Auto-classify on material bind (before the upload reads meta):
    // non-color data textures tag linear color space; every known slot widens
    // the cook usage to one that serves it, so the import cook's Auto path can
    // block-compress. A newly written tag evicts any stale upload; a new or
    // widened usage additionally re-cooks the already-decoded payload (the
    // cook key folds the usage).
    const bool newColorTag = EnsureDataTextureColorSpaceTagged(texGuid, slotName);
    const bool newUsageTag =
        EnsureTextureCookUsageTagged(texGuid, TextureCookUsageForMaterialSlot(slotName), matGuid, slotName);
    if ((newColorTag || newUsageTag) && m_TextureGPUCache.Contains(texGuid))
    {
        Evict(texGuid);
    }
    if (newUsageTag)
    {
        RequestRecook(texGuid);
    }

    // Fast path: already in GPU cache — bind immediately.
    if (Rendering::TextureHandle cached; m_TextureGPUCache.TryGet(texGuid, cached))
    {
        BindMaterialTexture(mat, slotName, cached);
        std::lock_guard lock(m_PendingTexturesMutex);
        TrackMaterialTextureRefLocked(matGuid, slotName, texGuid);
        return;
    }

    // Slow path: kick an async AssetManager-driven load. The slot samples its
    // awaited default until the load completes and FlushPendingUploads binds the
    // texture next frame; a load that fails leaves it there. The ref is
    // pre-registered so a hot-reload of texGuid before the load completes still
    // finds this material and rebinds.
    if (GameEngine::EngineCore::GetInstance().IsInitialized())
    {
        BindAwaitedTexture(mat, slotName);
        if (route == TextureLoadRoute::UploadJob)
            ScheduleTextureUploadJob(texGuid, matGuid, slotName);
        else
            ScheduleMaterialTextureLoad(texGuid, matGuid, slotName);
    }
    else
    {
        BindMaterialTexture(mat, slotName, GetOrUpload(texGuid));
    }
    std::lock_guard lock(m_PendingTexturesMutex);
    TrackMaterialTextureRefLocked(matGuid, slotName, texGuid);
}

void TextureService::UntrackMaterialTextureRef(const GUID& matGuid, StringId slotName)
{
    std::lock_guard lock(m_PendingTexturesMutex);
    UntrackMaterialTextureRefLocked(matGuid, slotName);
}

void TextureService::OnMaterialUnregistered(const GUID& matGuid)
{
    // Drop tracking entries so a future texture eviction doesn't try to
    // rebind a freed Material* (Find() would return nullptr but the
    // reverse-map walk would still cost cycles per evict).
    std::lock_guard lock(m_PendingTexturesMutex);
    m_EmbeddedSlotsByMaterial.erase(matGuid);
    UntrackAllMaterialTextureRefsLocked(matGuid);
}

void TextureService::ReloadEmbeddedForModel(const GUID& modelGuid, std::span<const EmbeddedImage> images)
{
    // Embedded images are cached under derived GUIDs (see UploadEmbeddedImage);
    // the parent model GUID itself is never a texture cache key. Take the
    // previous payload's list out first: the rebinds below re-track this model
    // from `images` alone, so content both payloads share gains a reference
    // before the loop at the end drops the old one, and survives.
    std::vector<GUID> previous;
    if (auto derivedIt = m_DerivedTextureGuidsByParent.find(modelGuid);
        derivedIt != m_DerivedTextureGuidsByParent.end())
    {
        previous = std::move(derivedIt->second);
        m_DerivedTextureGuidsByParent.erase(derivedIt);
    }
    std::vector<Rendering::TextureHandle> previousTextures;
    for (const GUID& childGuid : previous)
    {
        if (Rendering::TextureHandle tex{}; m_TextureGPUCache.TryGet(childGuid, tex))
            previousTextures.push_back(tex);
    }

    // Rebind before release, outside the lock (the binds upload). A slot binding
    // one of the previous payload's textures takes the reloaded image; so does a
    // slot still awaiting one (its image was missing or empty) unless an asset
    // texture bind has claimed it since. A slot that a later bind moved off this
    // model's textures is no longer this model's to rebind, and is not tracked
    // again.
    std::vector<std::pair<GUID, std::vector<EmbeddedSlot>>> boundMaterials;
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        for (auto it = m_EmbeddedSlotsByMaterial.begin(); it != m_EmbeddedSlotsByMaterial.end();)
        {
            if (it->second.ModelGuid != modelGuid)
            {
                ++it;
                continue;
            }
            boundMaterials.emplace_back(it->first, std::move(it->second.Slots));
            it = m_EmbeddedSlotsByMaterial.erase(it);
        }
    }
    for (const auto& [matGuid, slots] : boundMaterials)
    {
        Material* mat = (*m_Materials).Find(matGuid);
        if (!mat)
            continue;
        for (const EmbeddedSlot& slot : slots)
        {
            const bool bindsPrevious = std::find(previousTextures.begin(), previousTextures.end(),
                                                 mat->GetTexture(slot.SlotName)) != previousTextures.end();
            if (!bindsPrevious && !AwaitsEmbeddedImage(*mat, matGuid, slot.SlotName))
                continue;
            TrackEmbeddedSlot(matGuid, modelGuid, slot.SlotName, slot.ImageIndex);
            if (!BindEmbeddedImage(mat, slot.SlotName, modelGuid, slot.ImageIndex, images))
            {
                Logger::Log::Warning(
                    "ReloadEmbeddedForModel: reloaded model {} has no image {} (or it has no bytes); "
                    "material {} slot {:#x} awaits",
                    modelGuid.ToString(), slot.ImageIndex, matGuid.ToString(), slot.SlotName);
            }
        }
    }

    for (const GUID& childGuid : previous)
    {
        // Content-keyed entries are shared across models: only destroy with the
        // last referencing parent, otherwise a hot-reload of one model would
        // yank a texture other loaded models still sample.
        auto refIt = m_EmbeddedContentRefs.find(childGuid);
        if (refIt != m_EmbeddedContentRefs.end())
        {
            if (--refIt->second > 0)
                continue;
            m_EmbeddedContentRefs.erase(refIt);
        }
        Evict(childGuid);
        m_EmbeddedDecodeCache.erase(childGuid);
    }
}

Rendering::DescriptorSetLayoutDesc TextureService::GetBindlessTextureSetLayout()
{
    return GetBindlessTextureSetLayout(kMaxBindlessTextures);
}

Rendering::DescriptorSetLayoutDesc TextureService::GetBindlessTextureSetLayout(uint32_t maxTextures)
{
    Rendering::DescriptorSetLayoutDesc layout{};

    // Binding 0: the large bindless SAMPLED_IMAGE array (textures only — the
    // sampler is selected per-draw from binding 1). Split from the former
    // CombinedImageSampler array so the texture count is no longer bounded by
    // Metal's sampler limit (MoltenVK), and so a texture occupies one slot
    // regardless of filter.
    Rendering::DescriptorBinding texArrayBinding{};
    texArrayBinding.binding = 0;
    texArrayBinding.type = Rendering::DescriptorType::Texture;
    texArrayBinding.count = std::max<uint32_t>(1u, maxTextures);
    texArrayBinding.shaderStages = Rendering::kShaderStageVertex |
                                   Rendering::kShaderStageFragment |
                                   Rendering::kShaderStageCompute;
    texArrayBinding.debugName = "ge_BindlessTextures";
    // Keep bindless on the pool/UAB path. This descriptor array is intentionally
    // large and visible to terrain vertex shaders as well as fragment shaders;
    // descriptor-buffer layouts cannot carry UPDATE_AFTER_BIND here.
    // CreateVulkanDescriptorSetLayout detects the UAB flag and routes this layout
    // through the pool path even when the device is otherwise DB-enabled.
    texArrayBinding.flags = Rendering::kDescriptorBindingUpdateAfterBind
                          | Rendering::kDescriptorBindingPartiallyBound;
    layout.bindings.push_back(texArrayBinding);

    // Binding 1: the small shared sampler array — one entry per SamplerPreset,
    // written once at init and indexed per texture-slot via the material's packed
    // SamplerIndices. Not UPDATE_AFTER_BIND (fully written before first bind).
    // Must share binding 0's stage visibility: terrain constructs sampler2D in VS
    // and grass compaction samples terrain masks in CS.
    Rendering::DescriptorBinding samplerArrayBinding{};
    samplerArrayBinding.binding = 1;
    samplerArrayBinding.type = Rendering::DescriptorType::Sampler;
    samplerArrayBinding.count = static_cast<uint32_t>(Rendering::SamplerPreset::kCount);
    samplerArrayBinding.shaderStages = Rendering::kShaderStageVertex |
                                       Rendering::kShaderStageFragment |
                                       Rendering::kShaderStageCompute;
    samplerArrayBinding.debugName = "ge_BindlessSamplers";
    layout.bindings.push_back(samplerArrayBinding);

    layout.debugName = "BindlessTextureSetLayout";
    return layout;
}

namespace
{
void FillIdentityCubeLutBinding(const Rendering::TextureHandle id3d,
                                const Rendering::TextureHandle id1d,
                                CubeLutGpuBindingState& out)
{
    constexpr int32 kHas3DLut = 1;
    constexpr int32 kHas1DLut = 2;
    out.Lut3D = id3d;
    out.Lut1DStrip = id1d;
    out.Size3D = id3d.IsValid() ? 2 : 0;
    out.Size1D = id1d.IsValid() ? 2 : 0;
    out.Flags = (id3d.IsValid() ? kHas3DLut : 0) | (id1d.IsValid() ? kHas1DLut : 0);
    out.In1DMin = 0.0f;
    out.In1DMax = 1.0f;
    out.In3DMin = 0.0f;
    out.In3DMax = 1.0f;
}

uint64_t HashBytesFnv1a64(const uint8_t* data, size_t size)
{
    constexpr uint64_t kFnvOffset = 14695981039346656037ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;
    uint64_t hash = kFnvOffset;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= kFnvPrime;
    }
    return hash;
}

} // namespace

bool TextureService::TryGetCubeLutGpuBinding(const PostProcessSettings& pp, CubeLutGpuBindingState& out) const
{
    FillIdentityCubeLutBinding(m_IdentityLut3D, m_IdentityLut1DStrip, out);
    if (!m_Device)
        return false;

    if (!pp.IsLutActive())
        return true;

    GUID::Data raw{};
    static_assert(sizeof(raw) == sizeof(pp.LutAssetGuidWords));
    std::memcpy(raw.data(), pp.LutAssetGuidWords, sizeof(pp.LutAssetGuidWords));
    const GUID assetGuid(raw);
    if (assetGuid.IsNull())
        return true;

    // The LUT is sampled with a filtering sampler, so a float32 LUT needs the
    // device to filter float32 (WebGPU: the float32-filterable feature). Where
    // it cannot, clamp the authored format to float16 rather than producing a
    // bind group the device rejects.
    const bool filterableF32 = m_Device->GetCapabilities().supportsFilterableFloat32;
    const int32 lutTextureFormat = (pp.LutTextureFormat == 1 || !filterableF32) ? 1 : 0;
    const auto gpuTextureFormat = lutTextureFormat == 1
        ? Rendering::TextureFormat::R16G16B16A16_FLOAT
        : Rendering::TextureFormat::R32G32B32A32_FLOAT;
    const CubeLutGpuCacheKey cacheKey{assetGuid, lutTextureFormat};

    if (const auto it = m_CubeLutGpuCache.find(cacheKey); it != m_CubeLutGpuCache.end())
    {
        out = it->second;
        return true;
    }

    CubeLutGpuBindingState uploaded{};
    FillIdentityCubeLutBinding(m_IdentityLut3D, m_IdentityLut1DStrip, uploaded);

    AssetMetadata meta{};
    if (!GameEngine::EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(assetGuid, meta)
        || meta.Path.empty())
    {
        if (m_CubeLutWarnedFailureGuids.insert(assetGuid).second)
            Logger::Log::Warning("Cube LUT: no asset metadata for GUID {}", assetGuid.ToString());
        out = uploaded;
        return true;
    }

    std::string text;
    if (!ReadFileTextShared(meta.Path, text))
    {
        if (m_CubeLutWarnedFailureGuids.insert(assetGuid).second)
            Logger::Log::Warning("Cube LUT: failed to open '{}'", meta.Path.string());
        out = uploaded;
        return true;
    }

    Rendering::CubeLutParseResult parsed{};
    if (!Rendering::ParseCubeLutFromText(text, parsed) || !parsed.Ok)
    {
        if (m_CubeLutWarnedFailureGuids.insert(assetGuid).second)
            Logger::Log::Warning("Cube LUT: parse failed for '{}': {}", meta.Path.string(), parsed.Error);
        out = uploaded;
        return true;
    }

    Rendering::CubeLutGpuTextures gpu{};
    std::string gpuErr;
    if (!Rendering::CreateCubeLutGpuTextures(m_Device, parsed, gpuTextureFormat, gpu, &gpuErr))
    {
        if (m_CubeLutWarnedFailureGuids.insert(assetGuid).second)
            Logger::Log::Warning("Cube LUT: GPU create failed for '{}': {}", meta.Path.string(), gpuErr);
        out = uploaded;
        return true;
    }

    constexpr int32 kHas3DLut = 1;
    constexpr int32 kHas1DLut = 2;
    uploaded.Lut3D = gpu.Lut3D.IsValid() ? gpu.Lut3D : m_IdentityLut3D;
    uploaded.Lut1DStrip = gpu.Lut1DStrip.IsValid() ? gpu.Lut1DStrip : m_IdentityLut1DStrip;
    uploaded.Size3D = (parsed.Has3D && uploaded.Lut3D.IsValid()) ? static_cast<int32>(parsed.Size3D) : 0;
    uploaded.Size1D = (parsed.Has1D && uploaded.Lut1DStrip.IsValid()) ? static_cast<int32>(parsed.Size1D) : 0;
    uploaded.Flags = (uploaded.Size3D > 0 ? kHas3DLut : 0) | (uploaded.Size1D > 0 ? kHas1DLut : 0);
    uploaded.In1DMin = parsed.In1DMin;
    uploaded.In1DMax = parsed.In1DMax;
    uploaded.In3DMin = parsed.In3DMin;
    uploaded.In3DMax = parsed.In3DMax;

    m_CubeLutGpuCache[cacheKey] = uploaded;
    m_CubeLutWarnedFailureGuids.erase(assetGuid);
    out = uploaded;
    return true;
}

bool TextureService::TryWriteCubeLutPushMember(const std::string& name,
                                               const CubeLutGpuBindingState& lut,
                                               Rendering::NamedPushConstantWriter& pcw)
{
    if (name == "lutSize3d") return pcw.Add(name, lut.Size3D);
    if (name == "lutSize1d") return pcw.Add(name, lut.Size1D);
    if (name == "lutFlags") return pcw.Add(name, lut.Flags);
    if (name == "lut1dInMin") return pcw.Add(name, lut.In1DMin);
    if (name == "lut1dInMax") return pcw.Add(name, lut.In1DMax);
    if (name == "lut3dInMin") return pcw.Add(name, lut.In3DMin);
    if (name == "lut3dInMax") return pcw.Add(name, lut.In3DMax);
    return false;
}

Rendering::DescriptorSetHandle TextureService::MaterialTextureSet(const Material& material)
{
    if (m_IndexingMode != MaterialIndexingMode::Classic)
        return {};
    return m_MaterialBindings.GetOrBuild(material, GetSampler(material.GetSamplerPreset()));
}

Rendering::DescriptorSetHandle TextureService::DefaultMaterialTextureSet()
{
    if (m_IndexingMode != MaterialIndexingMode::Classic)
        return {};
    return m_MaterialBindings.DefaultSet();
}

uint32_t TextureService::DefaultBindlessIndex(SlotDefaultTexture texture) const
{
    switch (texture)
    {
    case SlotDefaultTexture::FlatNormal: return m_DefaultFlatNormalBindlessIndex;
    case SlotDefaultTexture::Black:      return m_DefaultBlackBindlessIndex;
    case SlotDefaultTexture::White:      break;
    }
    return m_DefaultWhiteBindlessIndex;
}

Rendering::TextureHandle TextureService::DefaultTexture(SlotDefaultTexture texture) const
{
    switch (texture)
    {
    case SlotDefaultTexture::FlatNormal: return m_DefaultFlatNormalTexture;
    case SlotDefaultTexture::Black:      return m_DefaultBlackTexture;
    case SlotDefaultTexture::White:      break;
    }
    return m_DefaultWhiteTexture;
}

uint32_t TextureService::UnassignedSlotBindlessIndex(uint32_t ordinal) const
{
    return DefaultBindlessIndex(UnassignedSlotDefault(static_cast<TextureSlot>(ordinal)));
}

uint32_t TextureService::ResolveDefaultBindlessIndex(StringId slotName) const
{
    // The rule Material::InitBindlessDefaults bakes, so cold start and a slot
    // cleared later (an edited document, an ended override) leave the material
    // in the same visual state. A name that is no well-known slot reads kCount:
    // white.
    return DefaultBindlessIndex(UnassignedSlotDefault(TextureSlotFromName(slotName)));
}

void TextureService::BindAwaitedTexture(Material* mat, StringId slotName)
{
    if (!mat)
        return;
    ApplyMaterialSlotBinding(mat, slotName, Rendering::TextureHandle{},
                             DefaultBindlessIndex(AwaitedSlotDefault(TextureSlotFromName(slotName))));
    mat->MarkTextureAwaited(slotName);
}

Rendering::TextureHandle TextureService::ResolveDefaultTexture(std::string_view bindingName) const
{
    using Engine::Renderer::TextureSlot;
    using Engine::Renderer::TextureSlotFromName;
    // The rule ResolveDefaultBindlessIndex reads: a slot must fall back to the
    // same pixel whichever indexing mode is active.
    const TextureSlot slot = TextureSlotFromName(HashStringId(bindingName));
    if (slot != TextureSlot::kCount)
        return DefaultTexture(UnassignedSlotDefault(slot));
    // The caller passes a raw shader binding name, which is often not one of the
    // well-known slots (a terrain kernel's 'cbt_Layer0Normal' resolves to kCount).
    // White decodes to a badly skewed normal, so name-match the one case where the
    // wrong default is visibly wrong rather than merely flat.
    if (bindingName.find("ormal") != std::string_view::npos)
        return m_DefaultFlatNormalTexture;
    return m_DefaultWhiteTexture;
}

// Resolve a texture's effective color space, honoring the per-asset color-space
// override stored in the AssetDatabase metadata (kv key assets.texture.colorSpace =
// srgb|linear|auto) over the asset's extension-based guess. Color space is a property
// of the texture (not the slot it's bound to), so the GUID-keyed GPU texture cache
// stays correct: one file -> one color space -> one upload.
static TextureColorSpace ResolveTextureColorSpace(const std::filesystem::path& path, TextureColorSpace guessed)
{
    if (path.empty())
        return guessed;
    auto& registry = GameEngine::EngineCore::GetInstance().GetAssetManager().GetRegistry();
    std::string value;
    TextureColorSpace overridden = guessed;
    if (registry.TryGetMetaValue(path, kTextureColorSpaceMetaKey, value) &&
        ParseTextureColorSpaceMeta(value, overridden))
    {
        return overridden;
    }
    return guessed;
}

// Read the per-asset swizzle override (AssetDatabase metadata) for `path`; returns
// true and fills src[4] (source channel per output R,G,B,A) for a valid non-identity swizzle.
static bool ResolveTextureSwizzle(const std::filesystem::path& path, char src[4])
{
    if (path.empty())
        return false;
    auto& registry = GameEngine::EngineCore::GetInstance().GetAssetManager().GetRegistry();
    std::string value;
    if (!registry.TryGetMetaValue(path, kTextureSwizzleMetaKey, value))
        return false;
    return ParseTextureSwizzleMeta(value, src);
}

// Read the per-asset cook settings that shape a mip chain (AssetDatabase metadata):
// "Generate Mips", "Mip Limit", and the usage category (normal maps renormalize after
// each downsample). The raw-decode upload path builds its chain through the cook's
// builder under these settings, so a first bind matches the cooked artifact that later
// replaces it. Requested compression is also read for coverage admission: an
// alpha-discarding format must not become accidentally valid only on raw upload.
static void ExpandRawLdrToRgba(const uint8_t* pixels, size_t count, uint32_t channels,
                               bool coverage, std::vector<uint8_t>& expanded)
{
    expanded.resize(count * 4);
    for (size_t i = 0; i < count; ++i)
    {
        if (coverage && (channels == 1 || channels == 2))
        {
            // stb's one/two-channel layouts are gray / gray+alpha. Match the
            // cook's forced RGBA decode while leaving disabled legacy bytes alone.
            expanded[i*4] = expanded[i*4+1] = expanded[i*4+2] = pixels[i*channels];
            expanded[i*4+3] = channels == 2 ? pixels[i*channels+1] : 255u;
        }
        else
        {
            expanded[i*4] = channels > 0 ? pixels[i*channels] : 255u;
            expanded[i*4+1] = channels > 1 ? pixels[i*channels+1] : 255u;
            expanded[i*4+2] = channels > 2 ? pixels[i*channels+2] : 255u;
            expanded[i*4+3] = 255u;
        }
    }
}

static bool ResolveTextureCookSettings(const std::filesystem::path& path, TextureCookSettings& settings)
{
    if (path.empty())
        return true;
    auto& registry = GameEngine::EngineCore::GetInstance().GetAssetManager().GetRegistry();
    std::string value;
    if (registry.TryGetMetaValue(path, kTextureUsageMetaKey, value))
        ParseTextureCookUsageMeta(value, settings.Usage);
    value.clear();
    if (registry.TryGetMetaValue(path, kTextureMipsMetaKey, value))
        settings.MipsEnabled = value != "0";
    value.clear();
    if (registry.TryGetMetaValue(path, kTextureMipLimitMetaKey, value) && !value.empty())
        settings.MipLimit = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    value.clear();
    if (registry.TryGetMetaValue(path, kTextureCompressionMetaKey, value))
        ParseTextureCookCompressionMeta(value, settings.Compression);
    std::string enabled, cutoff, error;
    registry.TryGetMetaValue(path, kTextureAlphaCoverageMetaKey, enabled);
    registry.TryGetMetaValue(path, kTextureAlphaCutoffMetaKey, cutoff);
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!ParseTextureAlphaCoverageMeta(enabled, cutoff, settings, error) ||
        !ValidateTextureAlphaCoverageSettings(settings, extension == ".hdr", error))
    {
        Logger::Log::Error("Texture upload '{}': invalid alpha coverage policy: {}", path.string(), error);
        return false;
    }
    if (settings.PreserveAlphaCoverage &&
        (extension == ".ktx" || extension == ".ktx2" || extension == ".dds" || extension == ".svg"))
    {
        Logger::Log::Error("Texture upload '{}': alpha coverage applies to decoded LDR source images, not authored containers or SVG", path.string());
        return false;
    }
    return true;
}

// Remap an RGBA8 buffer in place per the swizzle sources (see kTextureSwizzleMetaKey).
static void ApplyTextureSwizzleRGBA8(uint8_t* data, size_t pixelCount, const char src[4])
{
    for (size_t i = 0; i < pixelCount; ++i)
    {
        uint8_t* px = data + i * 4;
        const uint8_t r = px[0], g = px[1], b = px[2], a = px[3];
        for (int c = 0; c < 4; ++c)
        {
            switch (src[c])
            {
            case 'r': px[c] = r; break;
            case 'g': px[c] = g; break;
            case 'b': px[c] = b; break;
            case 'a': px[c] = a; break;
            case '0': px[c] = 0; break;
            case '1': px[c] = 255; break;
            default: break;
            }
        }
    }
}

// Container-driven upload: textures whose asset carries a mip chain (authored
// KTX2 or the import cook's cached artifact). Uploads the full chain —
// block-compressed (BCn) or pre-mipped RGBA8/RGBA32F — and picks the GPU
// format from the resolved color space. Returns true when the asset was
// handled here (out may still be invalid on device failure); false routes the
// caller to the legacy single-mip RGBA8 path. Block-compressed payloads are
// ALWAYS handled: their bytes are not per-pixel addressable, so the legacy
// expansion/swizzle path must never see them.
// Creates a persistent 2D RGBA8 texture from decoded pixels with a FULL mip
// chain and uploads every level. Every raw (non-KTX2) upload goes through
// here: a texture that arrives without a cooked container still has to
// minify through trilinear levels, or its surface aliases into sparkle at any
// distance. The chain is built by the cook's own builder under the asset's
// settings so the raw upload is byte-comparable with the cooked artifact that
// may later replace it (level count, filters, normal renormalization).
// Returns an invalid handle when the device refuses the texture; `outLevels`
// reports the levels uploaded (1 when no resampler is compiled in).
static Rendering::TextureHandle UploadRgba8WithMips(Rendering::IDevice* device,
                                                    const uint8_t* rgba, uint32_t w, uint32_t h,
                                                    Rendering::TextureFormat gpuFormat,
                                                    const TextureCookSettings& cookSettings,
                                                    bool srgb, const char* debugName,
                                                    const char* stagingName, uint32_t& outLevels)
{
    std::vector<TextureCookMipLevel> mipChain;
    {
        TextureCookMipLevel level0;
        level0.Width = w;
        level0.Height = h;
        level0.Bytes.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
        mipChain.push_back(std::move(level0));
    }
    std::string mipError;
    if (!BuildTextureCookMipChain(mipChain, ComputeTextureCookMipCount(w, h, cookSettings),
                                  /*isFloat*/ false, srgb, cookSettings, &mipError))
    {
        if (cookSettings.PreserveAlphaCoverage)
        {
            Logger::Log::Error("Texture upload '{}': alpha coverage mip build failed: {}", debugName, mipError);
            return {};
        }
        mipChain.resize(1);
    }
    outLevels = static_cast<uint32_t>(mipChain.size());

    Rendering::TextureDesc td{};
    td.width = w;
    td.height = h;
    td.mipLevels = outLevels;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(gpuFormat);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource)
             | static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = debugName;
    Rendering::TextureHandle tex = device->CreateTexture(td);
    if (!tex.IsValid())
        return {};
    std::vector<Rendering::TextureMipUploadEntry> mipEntries(mipChain.size());
    for (size_t level = 0; level < mipChain.size(); ++level)
        mipEntries[level] = {mipChain[level].Bytes.data(), mipChain[level].Bytes.size(),
                             mipChain[level].Width, mipChain[level].Height};
    Rendering::UploadTextureMips(device, tex, mipEntries.data(),
                                 static_cast<uint32_t>(mipEntries.size()), gpuFormat, stagingName);
    return tex;
}

// True when the asset's own mip chain can be uploaded as it is (TryUploadFromMipChain): a
// block-compressed payload, or a tightly packed RGBA8/RGBA32F one with no unbaked swizzle.
// Reads the asset only, so the CPU half of an upload can decide it on any thread.
static bool UsesContainerUpload(const TextureAsset& texAsset, const std::filesystem::path& path)
{
    if (texAsset.GetMipChain().empty())
        return false;
    if (texAsset.IsBlockCompressed())
        return true;
    const bool isFloat = texAsset.GetFormat() == GameEngine::TextureFormat::RGBA32F;
    // Only tightly-packed RGBA8/RGBA32F payloads can skip the expansion path.
    if ((texAsset.GetFormat() != GameEngine::TextureFormat::RGBA8 && !isFloat) || texAsset.GetChannels() != 4)
        return false;
    // An explicit per-asset swizzle can't be applied per-mip here (the cook bakes it instead);
    // honor an unbaked one via the expansion path rather than dropping it.
    char swz[4];
    return texAsset.IsSwizzleBaked() || isFloat || !ResolveTextureSwizzle(path, swz);
}

static bool TryUploadFromMipChain(Rendering::IDevice* device,
                                  const TextureAsset& texAsset,
                                  const std::filesystem::path& path,
                                  TextureColorSpace colorSpace,
                                  Rendering::TextureHandle& out)
{
    out = {};
    if (!UsesContainerUpload(texAsset, path))
        return false;
    const auto& chain = texAsset.GetMipChain();
    const bool isBlock = texAsset.IsBlockCompressed();
    const bool isFloat = texAsset.GetFormat() == GameEngine::TextureFormat::RGBA32F;

    const bool srgb = colorSpace == TextureColorSpace::SRGB;
    Rendering::TextureFormat gpuFormat;
    if (isBlock)
    {
        if (!device->GetCapabilities().supportsTextureCompressionBC)
        {
            // Decode-time transcode-target / cook-output selection makes this
            // unreachable; guard so a mismatch degrades to the slot's default
            // binding instead of a VK_FORMAT_UNDEFINED texture.
            Logger::Log::Error(
                "TextureService: BC payload for '{}' but the device lacks BC support; skipping upload",
                path.string());
            return true;
        }
        char swz[4];
        if (!texAsset.IsSwizzleBaked() && ResolveTextureSwizzle(path, swz))
        {
            Logger::Log::Warning(
                "TextureService: swizzle meta on block-compressed texture '{}' cannot be applied; ignoring",
                path.string());
        }
        switch (texAsset.GetFormat())
        {
        case GameEngine::TextureFormat::BC1:
            gpuFormat = srgb ? Rendering::TextureFormat::BC1_SRGB
                             : Rendering::TextureFormat::BC1_UNORM;
            break;
        case GameEngine::TextureFormat::BC4:
            gpuFormat = Rendering::TextureFormat::BC4_UNORM;
            break;
        case GameEngine::TextureFormat::BC5:
            gpuFormat = Rendering::TextureFormat::BC5_UNORM;
            break;
        case GameEngine::TextureFormat::BC6H:
            gpuFormat = Rendering::TextureFormat::BC6H_UF16;
            break;
        case GameEngine::TextureFormat::BC7:
        default:
            gpuFormat = srgb ? Rendering::TextureFormat::BC7_SRGB
                             : Rendering::TextureFormat::BC7_UNORM;
            break;
        }
    }
    else if (isFloat)
    {
        gpuFormat = Rendering::TextureFormat::R32G32B32A32_FLOAT;
    }
    else
    {
        gpuFormat = srgb ? Rendering::TextureFormat::RGBA8_SRGB
                         : Rendering::TextureFormat::RGBA8_UNORM;
    }

    Rendering::TextureDesc td{};
    td.width = texAsset.GetWidth();
    td.height = texAsset.GetHeight();
    td.mipLevels = static_cast<uint32_t>(chain.size());
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(gpuFormat);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource)
             | static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = "MaterialTexture";
    auto tex = device->CreateTexture(td);
    if (!tex.IsValid())
        return true;

    const uint8_t* pixels = texAsset.GetPixelData();
    std::vector<Rendering::TextureMipUploadEntry> mips(chain.size());
    for (size_t i = 0; i < chain.size(); ++i)
    {
        mips[i] = {pixels + chain[i].Offset, static_cast<size_t>(chain[i].Size),
                   chain[i].Width, chain[i].Height};
    }
    Rendering::UploadTextureMips(device, tex, mips.data(), static_cast<uint32_t>(mips.size()),
                                 gpuFormat, "MaterialTexStaging");
    out = tex;
    return true;
}

GUID TextureService::ResolveTextureRefGuid(const std::string& guidStr,
                                           const std::string& pathStr) const
{
    // A non-GUID sentinel ("__embedded:N") or empty string yields Null — there is
    // no real GUID to redirect-chase or re-derive, so route it to the caller's
    // embedded/default handling unchanged. Gating Path 2 on a real parsed GUID
    // also stops a stray "path" companion on an embedded slot from binding a
    // wrong texture.
    const GUID parsed = guidStr.empty() ? GUID::Null() : GUID(guidStr);
    if (parsed.IsNull() || !GameEngine::EngineCore::GetInstance().IsInitialized())
        return parsed; // null sentinel, or headless/unit test with no registry

    auto& registry = GameEngine::EngineCore::GetInstance().GetAssetManager().GetRegistry();

    // Path 1 — redirect-chase the authored GUID; accept it if it still resolves to
    // a known asset (covers the no-flip case and renames via the redirect table).
    const GUID canonical = registry.ResolveGuid(parsed);
    {
        AssetMetadata meta{};
        if (registry.TryGetAssetMetadata(canonical, meta) && !meta.Path.empty())
            return canonical;
    }

    // Path 2 — the GUID is unknown (e.g. re-keyed by the derived-identity flip):
    // re-derive it from the authored source-relative path. Accept the result ONLY
    // if it resolves to a real asset: GetOrCreateAssetGUID returns a throwaway
    // GUID::Generate() for a path it can't place under a source (and a non-derived
    // source with no store entry), which must NOT replace the authored GUID.
    if (!pathStr.empty())
    {
        const GUID derived = registry.GetOrCreateAssetGUID(std::filesystem::path(pathStr));
        AssetMetadata meta{};
        if (!derived.IsNull() && registry.TryGetAssetMetadata(derived, meta) && !meta.Path.empty())
            return derived;
    }

    // Path 3 — degraded: keep the redirect-resolved authored GUID. Downstream
    // GetOrUpload caches a null handle and the slot keeps its bindless
    // default — identical to the pre-fix stale-GUID behaviour (no crash, no
    // scene-load failure).
    return canonical;
}

bool TextureService::AlphaIsUniformlyOpaque(const GUID& textureGuid)
{
    if (textureGuid.IsNull())
        return false;

    uint32_t epoch = 0;
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        epoch = m_TextureEpochByGuid[textureGuid];
    }
    {
        std::lock_guard lock(m_AlphaOpacityMutex);
        if (auto it = m_AlphaOpacityCache.find(textureGuid);
            it != m_AlphaOpacityCache.end() && it->second.Epoch == epoch)
            return it->second.UniformlyOpaque;
    }

    auto& am = GameEngine::EngineCore::GetInstance().GetAssetManager();
    AssetMetadata meta{};
    if (!am.GetRegistry().TryGetAssetMetadata(textureGuid, meta) || meta.Path.empty())
        return false;
    std::filesystem::path path = meta.Path;
    if (path.is_relative())
        path = am.ResolveAssetPath(path);
    if (path.empty())
        return false;

    bool uniformlyOpaque = false;
    // Header tier first: a container with no alpha channel at all samples alpha 1 by construction,
    // which answers the question without paying a decode.
    if (ProbeTextureFileAlpha(path) == TextureAlphaContent::NoAlphaSource)
    {
        uniformlyOpaque = true;
    }
    else
    {
        const TextureAlphaProbeResult probe = ProbeTextureMinAlphaFromFile(path);
        // Deferred is a property of the MOMENT, not of the file — the decode cap was full at this
        // instant. Caching it would freeze one moment of contention into a permanent "has alpha"
        // for a texture that will probe fine, so this call keeps the alpha path and leaves the
        // cache alone; the next frame's extraction re-probes.
        if (probe.Status == TextureAlphaProbeStatus::Deferred)
            return false;
        uniformlyOpaque =
            probe.Status == TextureAlphaProbeStatus::Answered && probe.MinAlpha == 255u;
    }

    // Once per texture per load epoch (this is the cache-miss path), so the verdict behind a
    // consumer skipping its alpha path is auditable from the log alone — the decision leaves no
    // other artifact to inspect.
    Logger::Log::Info("Texture alpha probe '{}': {}", path.string(),
                      uniformlyOpaque ? "uniformly opaque — alpha path can be skipped"
                                      : "carries alpha — alpha path kept");

    std::lock_guard lock(m_AlphaOpacityMutex);
    m_AlphaOpacityCache[textureGuid] = AlphaOpacityEntry{epoch, uniformlyOpaque};
    return uniformlyOpaque;
}

// The CPU half of a texture upload. Exactly one of Container and MipChain carries the texels.
struct TextureService::PreparedTextureUpload
{
    enum class Outcome
    {
        Ready,   // upload it
        Refused, // terminal: publish a negative cache entry so callers stop retrying
        Failed,  // nothing to upload (no pixels); publish nothing
    };
    Outcome Result = Outcome::Failed;
    std::filesystem::path Path;
    SharedPtr<TextureAsset> Container;          // uploads its own mip chain (TryUploadFromMipChain)
    std::vector<TextureCookMipLevel> MipChain;  // raw levels, RGBA8 or RGBA32F
    Rendering::TextureFormat Format = Rendering::TextureFormat::RGBA8_UNORM;
    // The asset's color space with the per-asset override applied. Carried here rather than
    // written back to the asset, which may be the AssetManager's shared instance.
    TextureColorSpace ColorSpace = TextureColorSpace::Unknown;
    uint32_t Width = 0;
    uint32_t Height = 0;
    double LoadMs = 0.0;
    double MipMs = 0.0;
    std::thread::id PreparedOn; // reported in the upload log: which thread did the decode
    // Log the upload's timing (GetOrUpload, the texture assign); a scene's textures stay quiet.
    bool LogTiming = false;
};

Rendering::TextureHandle TextureService::GetOrUpload(const GUID& textureGuid)
{
    if (textureGuid.IsNull() || !m_Device)
        return {};

    if (Rendering::TextureHandle cached; m_TextureGPUCache.TryGet(textureGuid, cached))
        return cached;

    // Resolve GUID to an asset path and load.
    auto& am = GameEngine::EngineCore::GetInstance().GetAssetManager();
    AssetMetadata meta{};
    if (!am.GetRegistry().TryGetAssetMetadata(textureGuid, meta) || meta.Path.empty())
    {
        static std::mutex s_FailedTextureLogMutex;
        static std::unordered_set<GUID> s_LoggedFailedTextureGuids;
        bool shouldLog = false;
        {
            std::lock_guard lock(s_FailedTextureLogMutex);
            shouldLog = s_LoggedFailedTextureGuids.insert(textureGuid).second;
        }
        if (shouldLog)
        {
            Logger::Log::Warning("GetOrUpload: no metadata for GUID {}", textureGuid.ToString());
        }
        // Negative entry: stop this GUID re-resolving every frame.
        return m_TextureGPUCache.PublishOrAdopt(textureGuid, Rendering::TextureHandle{});
    }

    // Synchronous: the CPU half runs on this thread. The asynchronous paths run the same
    // PrepareUpload on a worker and leave only SubmitPreparedUpload to the render thread.
    const std::shared_ptr<const PreparedTextureUpload> prepared =
        PrepareUpload(textureGuid, nullptr, nullptr, /*logTiming=*/true);
    return SubmitPreparedUpload(textureGuid, *prepared);
}

std::shared_ptr<const TextureService::PreparedTextureUpload> TextureService::PrepareUpload(
    const GUID& textureGuid, SharedPtr<TextureAsset> resident, const std::atomic<bool>* cancel, bool logTiming)
{
    using Clock = std::chrono::high_resolution_clock;
    const auto cancelled = [cancel] { return cancel && cancel->load(std::memory_order_relaxed); };
    auto prepared = std::make_shared<PreparedTextureUpload>();
    prepared->PreparedOn = std::this_thread::get_id();
    prepared->LogTiming = logTiming;
    using Outcome = PreparedTextureUpload::Outcome;

    AssetMetadata meta{};
    const bool haveMeta = GameEngine::EngineCore::GetInstance().GetAssetManager().GetRegistry()
                              .TryGetAssetMetadata(textureGuid, meta) &&
                          !meta.Path.empty();
    if (haveMeta)
        prepared->Path = meta.Path;
    TextureCookSettings cookSettings;
    if (haveMeta && !ResolveTextureCookSettings(meta.Path, cookSettings))
    {
        prepared->Result = Outcome::Refused;
        return prepared;
    }

    const auto tLoad = Clock::now();
    SharedPtr<TextureAsset> texAsset = std::move(resident);
    if (!texAsset)
    {
        if (!haveMeta)
        {
            prepared->Result = Outcome::Refused;
            return prepared;
        }
        texAsset = MakeShared<TextureAsset>(textureGuid, meta.Path);
        // Adopt an existing cooked artifact (BCn + mips) from any thread. A miss decodes here:
        // cooking (seconds to minutes of encode) is worker-only through the AssetManager
        // (async loads, RequestRecook), never this path.
        texAsset->SetAdoptCookedArtifacts(true);
        if (!texAsset->Load())
        {
            Logger::Log::Warning("GetOrUpload: failed to load '{}'", meta.Path.string());
            prepared->Result = Outcome::Refused;
            return prepared;
        }
    }
    if (cancelled())
        return nullptr;
    // The per-asset color-space override (AssetDatabase metadata) over the extension guess.
    const TextureColorSpace colorSpace =
        haveMeta ? ResolveTextureColorSpace(meta.Path, texAsset->GetColorSpace()) : texAsset->GetColorSpace();
    prepared->ColorSpace = colorSpace;
    prepared->LoadMs = std::chrono::duration<double, std::milli>(Clock::now() - tLoad).count();

    const uint32_t w = texAsset->GetWidth();
    const uint32_t h = texAsset->GetHeight();
    const uint32_t channels = texAsset->GetChannels();
    const uint8_t* pixels = texAsset->GetPixelData();
    if (!pixels || w == 0 || h == 0)
        return prepared; // Failed
    prepared->Width = w;
    prepared->Height = h;

    // Container-driven textures (KTX2: BC7 or pre-mipped RGBA8) upload their authored mip chain
    // directly; everything else is expanded and mipped below.
    if (UsesContainerUpload(*texAsset, prepared->Path))
    {
        prepared->Container = std::move(texAsset);
        prepared->Result = Outcome::Ready;
        return prepared;
    }

    const bool isFloatTexture =
        texAsset->GetFormat() == GameEngine::TextureFormat::R32F ||
        texAsset->GetFormat() == GameEngine::TextureFormat::RG32F ||
        texAsset->GetFormat() == GameEngine::TextureFormat::RGB32F ||
        texAsset->GetFormat() == GameEngine::TextureFormat::RGBA32F;
    prepared->Format = isFloatTexture
        ? Rendering::TextureFormat::R32G32B32A32_FLOAT
        : ((colorSpace == TextureColorSpace::SRGB) ? Rendering::TextureFormat::RGBA8_SRGB
                                                                    : Rendering::TextureFormat::RGBA8_UNORM);

    // Level 0 in RGBA: a source with fewer than 4 channels is expanded.
    TextureCookMipLevel level0;
    level0.Width = w;
    level0.Height = h;
    const size_t texels = static_cast<size_t>(w) * h;
    if (isFloatTexture)
    {
        const auto* src = reinterpret_cast<const float*>(pixels);
        std::vector<float> expandedFloat(texels * 4);
        for (size_t i = 0; i < texels; ++i)
        {
            expandedFloat[i * 4 + 0] = (channels > 0) ? src[i * channels + 0] : 1.0f;
            expandedFloat[i * 4 + 1] = (channels > 1) ? src[i * channels + 1] : 1.0f;
            expandedFloat[i * 4 + 2] = (channels > 2) ? src[i * channels + 2] : 1.0f;
            expandedFloat[i * 4 + 3] = (channels > 3) ? src[i * channels + 3] : 1.0f;
        }
        const auto* bytes = reinterpret_cast<const uint8_t*>(expandedFloat.data());
        level0.Bytes.assign(bytes, bytes + texels * 4 * sizeof(float));
    }
    else if (channels < 4)
    {
        std::vector<uint8_t> expanded;
        ExpandRawLdrToRgba(pixels, texels, channels, cookSettings.PreserveAlphaCoverage, expanded);
        level0.Bytes = std::move(expanded);
    }
    else
    {
        level0.Bytes.assign(pixels, pixels + texels * 4);
    }

    // Apply the per-asset channel swizzle (AssetDatabase metadata) to the RGBA8 levels (not float).
    char swz[4];
    if (!isFloatTexture && haveMeta && ResolveTextureSwizzle(meta.Path, swz))
        ApplyTextureSwizzleRGBA8(level0.Bytes.data(), texels, swz);
    if (cancelled())
        return nullptr;

    // Mip chain for the raw-decode path (texture-tiling S1): a raw upload must still minify
    // through trilinear levels, or its surface aliases at distance. Built by the COOK's builder
    // under the SAME per-asset settings, so this upload is byte-comparable with the cooked
    // artifact that later replaces it (level count, filters, normal renormalization).
    const auto tMip = Clock::now();
    prepared->MipChain.push_back(std::move(level0));
    const bool srgbChain = !isFloatTexture && colorSpace == TextureColorSpace::SRGB;
    std::string mipError;
    if (!BuildTextureCookMipChain(prepared->MipChain, ComputeTextureCookMipCount(w, h, cookSettings),
                                  isFloatTexture, srgbChain, cookSettings, &mipError))
    {
        if (cookSettings.PreserveAlphaCoverage)
        {
            Logger::Log::Error("Texture upload '{}': alpha coverage mip build failed: {}",
                               prepared->Path.string(), mipError);
            prepared->MipChain.clear();
            prepared->Result = Outcome::Refused;
            return prepared;
        }
        // No resampler compiled in, or a downsample failed: single-mip upload.
        prepared->MipChain.resize(1);
    }
    prepared->MipMs = std::chrono::duration<double, std::milli>(Clock::now() - tMip).count();
    prepared->Result = Outcome::Ready;
    return prepared;
}

Rendering::TextureHandle TextureService::SubmitPreparedUpload(const GUID& textureGuid,
                                                              const PreparedTextureUpload& prepared)
{
    using Clock = std::chrono::high_resolution_clock;
    using Outcome = PreparedTextureUpload::Outcome;
    if (prepared.Result == Outcome::Refused)
        return m_TextureGPUCache.PublishOrAdopt(textureGuid, Rendering::TextureHandle{});
    if (prepared.Result != Outcome::Ready || !m_Device)
        return {};
    const char* preparedWhere = prepared.PreparedOn == m_RenderThreadId ? "render thread" : "worker";

    Rendering::TextureHandle tex;
    const auto tUpload = Clock::now();
    if (prepared.Container)
    {
        TryUploadFromMipChain(m_Device, *prepared.Container, prepared.Path, prepared.ColorSpace, tex);
        if (!tex.IsValid())
            return {};
        if (prepared.LogTiming)
            Logger::Log::Info("[ModelLoad]     Texture '{}' {}x{} ({} mips{}): load {:.1f}ms on the {}, upload {:.1f}ms",
                          prepared.Path.filename().string(), prepared.Width, prepared.Height,
                          prepared.Container->GetMipmapLevels(), prepared.Container->IsBlockCompressed() ? ", BC" : "",
                          prepared.LoadMs, preparedWhere,
                          std::chrono::duration<double, std::milli>(Clock::now() - tUpload).count());
    }
    else
    {
        Rendering::TextureDesc td{};
        td.width = prepared.Width;
        td.height = prepared.Height;
        td.mipLevels = static_cast<uint32_t>(prepared.MipChain.size());
        td.arrayLayers = 1;
        td.format = static_cast<uint32_t>(prepared.Format);
        td.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource)
                 | static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
        td.persistent = true;
        td.debugName = "MaterialTexture";
        tex = m_Device->CreateTexture(td);
        if (!tex.IsValid())
            return {};
        std::vector<Rendering::TextureMipUploadEntry> mipEntries(prepared.MipChain.size());
        for (size_t i = 0; i < prepared.MipChain.size(); ++i)
            mipEntries[i] = {prepared.MipChain[i].Bytes.data(), prepared.MipChain[i].Bytes.size(),
                             prepared.MipChain[i].Width, prepared.MipChain[i].Height};
        Rendering::UploadTextureMips(m_Device, tex, mipEntries.data(), static_cast<uint32_t>(mipEntries.size()),
                                     prepared.Format, "MaterialTexStaging");
        if (prepared.LogTiming)
            Logger::Log::Info(
            "[ModelLoad]     Texture '{}' {}x{} ({} mips, raw): load {:.1f}ms and mip {:.1f}ms on the {}, upload {:.1f}ms",
            prepared.Path.filename().string(), prepared.Width, prepared.Height, prepared.MipChain.size(),
            prepared.LoadMs, prepared.MipMs, preparedWhere,
            std::chrono::duration<double, std::milli>(Clock::now() - tUpload).count());
    }

    // Another extraction worker may have published this GUID while this thread was preparing and
    // uploading outside the lock; adopt its handle and retire the duplicate rather than orphaning
    // a texture callers already hold.
    const Rendering::TextureHandle published = m_TextureGPUCache.PublishOrAdopt(textureGuid, tex);
    if (published != tex)
        ParkDuplicateTexture(tex);
    return published;
}

void TextureService::ParkDuplicateTexture(Rendering::TextureHandle texture)
{
    if (!texture.IsValid())
        return;
    std::lock_guard lock(m_BindlessMutex);
    m_PendingGpuDiscards.push_back({Rendering::kInvalidBindlessTexture, {}, {}, texture});
}

void TextureService::ScheduleMaterialTextureLoad(const GUID& texGuid, const GUID& matGuid, StringId slotName)
{
    if (texGuid.IsNull())
        return;

    auto& engine = GameEngine::EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;
    auto& am = engine.GetAssetManager();

    // matGuid.IsNull() is the "just re-trigger the load" path used by
    // Evict after it's already pushed real bind entries
    // onto m_PendingTextureBinds. We skip the bind push to avoid leaving a
    // phantom (null, {}, texGuid) entry in the queue.
    const bool reTriggerOnly = matGuid.IsNull();
    uint32_t capturedEpoch = 0;
    bool needsLoad = false;
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        if (!reTriggerOnly)
            m_PendingTextureBinds.push_back({matGuid, slotName, texGuid});
        capturedEpoch = m_TextureEpochByGuid[texGuid];

        // Dedupe: many materials referencing the same texture share one load.
        if (m_TextureLoadsKicked.insert(texGuid).second)
            needsLoad = true;
    }

    if (!needsLoad)
        return;

    // Pass `this` raw — RenderServices outlives async loads (the engine owns
    // it, and Shutdown unsubscribes/joins before destruction). The captured
    // epoch protects against hot-reload races; the cache check inside the
    // callback's mutex protects against any duplicate uploads.
    AssetLoadHandle handle = am.LoadAsset(texGuid,
        [this, texGuid, capturedEpoch](Result<SharedPtr<Asset>, AssetError> r)
        {
            const bool ok = r.IsOk() && r.Value() != nullptr;
            if (!ok)
            {
                Logger::Log::Warning("[TexLoad] callback FAILED tex={} err={}",
                                     texGuid.ToCompactString(),
                                     r.IsOk() ? "asset null" : ToString(r.Error()));
            }
            // Worker thread: defer the upload to FlushPendingUploads, which prepares it on the
            // render thread. The asset is the AssetManager's shared instance, whose payload a reload
            // swaps in place on the main thread, so it is read only there.
            std::lock_guard lock(m_PendingTexturesMutex);
            m_TextureLoadsKicked.erase(texGuid);
            if (!ok)
            {
                // Drop the corresponding load handle and pending bind entries
                // so we don't keep stillPending growing forever. Without this
                // a missing/broken texture file leaves IsMaterialTextureBindingComplete
                // permanently false for every material referencing it. Also
                // drop the tracking maps so future eviction sweeps don't keep
                // re-triggering known-broken loads.
                m_TextureLoadHandles.erase(texGuid);
                m_PendingTextureBinds.erase(
                    std::remove_if(m_PendingTextureBinds.begin(), m_PendingTextureBinds.end(),
                        [&](const PendingTextureBind& b) { return b.TextureGuid == texGuid; }),
                    m_PendingTextureBinds.end());
                UntrackTextureFromAllMaterialsLocked(texGuid);
                return;
            }
            m_PendingGpuUploads.push_back({texGuid, std::move(r.Value()), capturedEpoch});
        });

    // The handle is retained so teardown and eviction can withdraw the request
    // (Cancel) before our callback runs into state they are clearing.
    //
    // Race: the worker callback can fire and erase texGuid from
    // m_TextureLoadsKicked between LoadAsset returning and us reaching this
    // store. The kicked-flag presence indicates an outstanding load for this
    // GUID (ours, or a later overlapping schedule). If our specific callback
    // already fired, storing this handle would orphan it until the next
    // eviction — gate the insert on the flag still being present.
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        if (m_TextureLoadsKicked.find(texGuid) != m_TextureLoadsKicked.end())
            m_TextureLoadHandles[texGuid] = std::move(handle);
    }
}

void TextureService::ScheduleTextureUploadJob(const GUID& texGuid, const GUID& matGuid, StringId slotName)
{
    if (texGuid.IsNull())
        return;
    auto& engine = GameEngine::EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;

    // A texture the AssetManager holds is queued for the drain, which prepares it on the render
    // thread as an AssetManager load's is: that instance is shared, and a reload swaps its payload
    // in place on the main thread. It is normally a cooked container, so that costs milliseconds.
    SharedPtr<Asset> resident = engine.GetAssetManager().GetAsset(texGuid);
    if (!std::dynamic_pointer_cast<TextureAsset>(resident))
        resident.reset();

    uint32_t epoch = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        if (m_UploadJobsStopped)
            return;
        if (!matGuid.IsNull())
            m_PendingTextureBinds.push_back({matGuid, slotName, texGuid});
        epoch = m_TextureEpochByGuid[texGuid];
        if (resident)
        {
            m_PendingGpuUploads.push_back({texGuid, std::move(resident), epoch, nullptr, /*LogTiming=*/true});
            return;
        }
        // One load per texture: a job or an AssetManager load already in flight lands the
        // upload that this bind waits on.
        if (!m_TextureLoadsKicked.insert(texGuid).second)
            return;
        cancel = std::make_shared<std::atomic<bool>>(false);
        m_UploadJobCancels[texGuid] = cancel;
        m_UploadJobCounter.Add(1);
        m_UploadJobsEverSubmitted = true;
    }

    // The job loads its own instance, adopting a cooked artifact or decoding. `this` is safe in
    // it: DrainUploadJobs (Shutdown, destructor) waits for the counter.
    engine.GetJobSystem().EnqueueWork([this, texGuid, epoch, cancel]()
                                      { RunTextureUploadJob(texGuid, epoch, cancel); });
}

void TextureService::RunTextureUploadJob(const GUID& texGuid, uint32_t epoch,
                                         const std::shared_ptr<std::atomic<bool>>& cancel)
{
    // Releases the counter on every exit, an exception included, after the last use of `this`.
    struct CounterRelease
    {
        JobSystem::JobCounter& Counter;
        ~CounterRelease() { Counter.Decrement(); }
    } release{m_UploadJobCounter};

    std::shared_ptr<const PreparedTextureUpload> prepared;
    if (!cancel->load(std::memory_order_relaxed))
    {
        try
        {
            prepared = PrepareUpload(texGuid, nullptr, cancel.get(), /*logTiming=*/true);
        }
        catch (const std::exception& error)
        {
            // An allocation the size of an 8K float texture can fail. Report the load as failed,
            // which releases the binds waiting on it, rather than leave the texture loading forever.
            Logger::Log::Error("TextureService: preparing texture {} failed: {}", texGuid.ToString(), error.what());
            auto failed = std::make_shared<PreparedTextureUpload>();
            failed->Result = PreparedTextureUpload::Outcome::Failed;
            prepared = std::move(failed);
        }
    }

    std::lock_guard lock(m_PendingTexturesMutex);
    if (auto it = m_UploadJobCancels.find(texGuid); it != m_UploadJobCancels.end() && it->second == cancel)
    {
        m_UploadJobCancels.erase(it);
        m_TextureLoadsKicked.erase(texGuid);
    }
    // Cancelled (Evict, CancelPendingLoads, Shutdown): whoever cancelled owns what follows.
    if (cancel->load(std::memory_order_relaxed) || !prepared)
        return;
    if (prepared->Result == PreparedTextureUpload::Outcome::Failed)
    {
        // Nothing to upload: release the binds waiting on it, as a failed AssetManager load does.
        m_PendingTextureBinds.erase(
            std::remove_if(m_PendingTextureBinds.begin(), m_PendingTextureBinds.end(),
                [&](const PendingTextureBind& b) { return b.TextureGuid == texGuid; }),
            m_PendingTextureBinds.end());
        return;
    }
    // The epoch it was scheduled under: an Evict since then makes the drain discard it.
    m_PendingGpuUploads.push_back({texGuid, nullptr, epoch, std::move(prepared)});
}

void TextureService::DrainUploadJobs()
{
    bool everSubmitted = false;
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        m_UploadJobsStopped = true;
        for (auto& [guid, cancel] : m_UploadJobCancels)
            cancel->store(true, std::memory_order_relaxed);
        everSubmitted = m_UploadJobsEverSubmitted;
    }
    // Never touch the engine when no job ever ran (bare render-services hosts and tests). Once
    // one did, the pool exists and outlives this service (engine teardown destroys it last).
    if (!everSubmitted)
        return;
    // The pool's Wait is the join point: a bare IsZero() read does not wait for the last job's
    // Decrement to leave the counter's lock, and the counter is destroyed right after this.
    GameEngine::EngineCore::GetInstance().GetJobSystem().Wait(m_UploadJobCounter);
    // Drained, and no job can start once m_UploadJobsStopped is set: a later drain (the
    // destructor's, after Shutdown's) returns above without touching the engine, whose job system
    // may already be gone.
    std::lock_guard lock(m_PendingTexturesMutex);
    m_UploadJobsEverSubmitted = false;
}

void TextureService::FlushPendingUploads()
{
    if (!m_Device)
        return;

    // Drain re-upload requests (e.g. texture import-setting edits from the
    // inspector) here, on the render thread — a safe point for GPU texture work.
    // HotSwapTexture uploads the new texture and repoints material bindings before
    // destroying the old, so the change applies with no default-texture flicker.
    // Copy under the lock, then swap without holding it. Done before the early-out
    // below so a reupload-only frame still runs.
    {
        std::vector<GUID> reupload;
        {
            std::lock_guard lock(m_PendingTexturesMutex);
            reupload.swap(m_PendingTextureReupload);
        }
        for (const GUID& g : reupload)
            HotSwapTexture(g);
    }

    // Retire duplicates left by concurrent registrations (GetBindlessIndex /
    // GetSampler / GetOrUpload). Those entry points run on ECS extraction workers
    // and — under GE_PARALLEL_RECORD — render-graph record workers; the losers
    // are retired here, on the render thread, because bindless slot parking
    // (DestroyBindlessTexture) reasons in frame boundaries and must stay ordered
    // with this frame's binding updates. The paired IDevice destroys ride along:
    // they defer through mutex-protected queues either way, but retiring the
    // whole discard on one thread keeps slot, view, sampler and texture in one
    // ordered batch. Ahead of the early-out below so a bind-idle frame still
    // drains.
    assert(std::this_thread::get_id() == m_RenderThreadId &&
           "TextureService discard drain reached from a non-render thread");
    {
        std::vector<PendingGpuDiscard> discards;
        {
            std::lock_guard lock(m_BindlessMutex);
            discards.swap(m_PendingGpuDiscards);
        }
        for (const auto& d : discards)
        {
            if (d.Bindless != Rendering::kInvalidBindlessTexture && m_Bindless)
                m_Bindless->DestroyBindlessTexture(d.Bindless);
            if (d.View.IsValid())
                m_Device->DestroyTextureView(d.View);
            if (d.Sampler.IsValid())
                m_Device->DestroySampler(d.Sampler);
            if (d.Texture.IsValid())
                m_Device->DestroyTexture(d.Texture);
        }
    }

    m_CpuTextureSources->Flush(*m_Device);

    // Snapshot worker-touched state under the mutex once. Drop completed
    // upload handles in a small batch instead of re-locking per-iteration.
    std::vector<PendingGpuUpload> uploads;
    std::vector<std::pair<GUID, uint32_t>> uploadEpochs;
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        if (m_PendingGpuUploads.empty() && m_PendingTextureBinds.empty())
            return;
        uploads = std::move(m_PendingGpuUploads);
        uploadEpochs.reserve(uploads.size());
        for (const auto& u : uploads)
            uploadEpochs.emplace_back(u.TextureGuid, m_TextureEpochByGuid[u.TextureGuid]);
    }

    for (size_t i = 0; i < uploads.size(); ++i)
    {
        auto& upload = uploads[i];

        // Stale-load guard: an Evict between schedule and
        // flush bumped m_TextureEpochByGuid[guid]. If our captured epoch no
        // longer matches the snapshot taken above, the loaded bytes belong
        // to a superseded version and must not be installed into the cache.
        const uint32_t currentEpoch = uploadEpochs[i].second;
        if (upload.Epoch != currentEpoch)
        {
            Logger::Log::Debug("Discarded stale texture load for {} (epoch {} != current {})",
                               upload.TextureGuid.ToString(), upload.Epoch, currentEpoch);
            continue;
        }

        // Idempotency: a hot-reload re-upload path may have already populated
        // the cache for this GUID since we drained the queue.
        if (m_TextureGPUCache.Contains(upload.TextureGuid))
            continue;

        // The CPU half ran on the loading worker (or an upload job); a queued load without it
        // runs it here. Either way this thread only submits the upload.
        std::shared_ptr<const PreparedTextureUpload> prepared = upload.Prepared;
        if (!prepared)
        {
            SharedPtr<TextureAsset> texAsset = std::dynamic_pointer_cast<TextureAsset>(upload.TextureAsset);
            if (!texAsset)
                continue;
            prepared = PrepareUpload(upload.TextureGuid, std::move(texAsset), nullptr, upload.LogTiming);
        }
        else if (AssetMetadata texMeta{};
                 GameEngine::EngineCore::GetInstance().GetAssetManager().GetRegistry()
                     .TryGetAssetMetadata(upload.TextureGuid, texMeta) &&
                 !texMeta.Path.empty())
        {
            // Metadata may have turned invalid after the worker prepared: a terminal refusal
            // still wins over the prepared bytes.
            if (TextureCookSettings settings; !ResolveTextureCookSettings(texMeta.Path, settings))
            {
                m_TextureGPUCache.PublishOrAdopt(upload.TextureGuid, Rendering::TextureHandle{});
                continue;
            }
        }
        if (prepared)
            (void)SubmitPreparedUpload(upload.TextureGuid, *prepared);
    }

    // Drop the load handles whose uploads landed in the cache, and pull out
    // the (material, slot, textureHandle) tuples that are now bindable —
    // under the lock so worker callbacks pushing into m_PendingTextureBinds
    // see a consistent view.
    //
    // Bind work itself happens AFTER we release the lock: BindMaterialTexture
    // calls into GetBindlessIndex / m_BindlessCache and the descriptor
    // backend, which is render-thread state. Holding the queue mutex over
    // descriptor work would let a flurry of binds stall worker callbacks
    // even though they touch unrelated state.
    struct ResolvedBind
    {
        Material* Mat;
        StringId SlotName;
        Rendering::TextureHandle Tex;
        GUID TexGuid;
    };
    std::vector<ResolvedBind> resolved;
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        for (const auto& [guid, _epoch] : uploadEpochs)
        {
            if (m_TextureGPUCache.Contains(guid))
                m_TextureLoadHandles.erase(guid);
        }

        std::vector<PendingTextureBind> stillPending;
        stillPending.reserve(m_PendingTextureBinds.size());
        resolved.reserve(m_PendingTextureBinds.size());
        for (auto& bind : m_PendingTextureBinds)
        {
            Rendering::TextureHandle bound{};
            if (!m_TextureGPUCache.TryGet(bind.TextureGuid, bound))
            {
                stillPending.push_back(bind);
                continue;
            }
            Material* mat = (*m_Materials).Find(bind.MaterialGuid);
            if (mat)
            {
                resolved.push_back({mat, bind.SlotName, bound, bind.TextureGuid});
                // Re-track in case eviction cleared this slot's ref while
                // we were waiting on the load. Idempotent on the steady-state
                // path (RegisterMaterialFromDocument already tracked).
                TrackMaterialTextureRefLocked(bind.MaterialGuid, bind.SlotName, bind.TextureGuid);
            }
        }
        m_PendingTextureBinds = std::move(stillPending);
    }

    for (const auto& r : resolved)
    {
        ApplyTextureFilterMeta(r.Mat, r.SlotName, r.TexGuid);
        BindMaterialTexture(r.Mat, r.SlotName, r.Tex);
    }
}

void TextureService::Evict(const GUID& textureGuid)
{
    // Bump the per-GUID load epoch FIRST. An AssetManager load whose result
    // hasn't been processed by FlushPendingUploads yet will be
    // discarded at flush time when the captured epoch no longer matches.
    // This is the key guard against the texture-load hot-reload race.
    //
    // Also collect the materials currently bound to this texture so we can
    // (a) reset their slot bindings to the bindless defaults before the GPU
    // handle is destroyed (otherwise samples from the parked bindless slot
    // read stale/garbage memory until the slot retires) and (b) re-queue
    // them onto m_PendingTextureBinds so FlushPendingUploads rebinds
    // once the new texture is uploaded.
    std::vector<std::pair<GUID, StringId>> affectedRefs;
    bool needsReload = false;
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        ++m_TextureEpochByGuid[textureGuid];
        // Drop any not-yet-uploaded entry for this GUID — bytes there are
        // pre-reload and would clobber the next live upload.
        m_PendingGpuUploads.erase(
            std::remove_if(m_PendingGpuUploads.begin(), m_PendingGpuUploads.end(),
                [&](const PendingGpuUpload& u) { return u.TextureGuid == textureGuid; }),
            m_PendingGpuUploads.end());
        m_TextureLoadsKicked.erase(textureGuid);
        if (auto itJob = m_UploadJobCancels.find(textureGuid); itJob != m_UploadJobCancels.end())
        {
            itJob->second->store(true, std::memory_order_relaxed); // its result is pre-reload too
            m_UploadJobCancels.erase(itJob);
        }
        if (auto itLoad = m_TextureLoadHandles.find(textureGuid); itLoad != m_TextureLoadHandles.end())
        {
            itLoad->second.Cancel(); // pre-reload bytes must not land in the queues
            m_TextureLoadHandles.erase(itLoad);
        }

        auto reverseIt = m_TextureToMaterialBindings.find(textureGuid);
        if (reverseIt != m_TextureToMaterialBindings.end())
        {
            affectedRefs = reverseIt->second;
            // Re-queue the bind requests so the next FlushPendingUploads
            // picks them up. Dedup: only push if there isn't already a pending
            // bind for this (material, slot, texGuid).
            for (const auto& [matGuid, slotName] : affectedRefs)
            {
                auto existing = std::find_if(m_PendingTextureBinds.begin(), m_PendingTextureBinds.end(),
                    [&](const PendingTextureBind& b) {
                        return b.MaterialGuid == matGuid && b.SlotName == slotName && b.TextureGuid == textureGuid;
                    });
                if (existing == m_PendingTextureBinds.end())
                    m_PendingTextureBinds.push_back({matGuid, slotName, textureGuid});
            }
            needsReload = !affectedRefs.empty();
        }
    }

    // Clear stale per-material bindings outside the lock — touches the
    // material registry (render-thread only) and bindless cache via
    // SetBindlessTextureIndex (no lock needed). The slots stay assigned and
    // their rebinds are queued above, so they await the reload.
    for (const auto& [matGuid, slotName] : affectedRefs)
    {
        Material* mat = (*m_Materials).Find(matGuid);
        if (!mat) continue;
        BindAwaitedTexture(mat, slotName);
    }

    if (Rendering::TextureHandle tex{}; m_TextureGPUCache.Remove(textureGuid, tex))
    {
        if (tex.IsValid())
        {
            // Drop bindless slots that point at the freed texture before the
            // GPU resource is destroyed; otherwise the slots dangle and the next
            // bind through GetBindlessIndex would return stale descriptor data.
            InvalidateBindless(tex);
            if (m_Device)
                m_Device->DestroyTexture(tex);
        }
    }

    // Re-trigger the async load so AssetManager re-reads the (now-updated)
    // texture file. The bind queue already holds entries from the loop above;
    // the load's completion lands a fresh PendingGpuUpload, and the next
    // FlushPendingUploads binds the new GPU texture into all the
    // affected materials.
    if (needsReload)
        ScheduleMaterialTextureLoad(textureGuid, GUID::Null(), StringId{});
}

void TextureService::RequestReupload(const GUID& textureGuid)
{
    if (textureGuid.IsNull())
        return;
    std::lock_guard lock(m_PendingTexturesMutex);
    if (std::find(m_PendingTextureReupload.begin(), m_PendingTextureReupload.end(), textureGuid)
        == m_PendingTextureReupload.end())
    {
        m_PendingTextureReupload.push_back(textureGuid);
    }
}

void TextureService::RequestRecook(const GUID& textureGuid)
{
    if (textureGuid.IsNull())
        return;
    // Kill switch: with GE_TEXTURE_COOK_DISABLE=1 the reload would run a full
    // async decode + evict + re-upload only for TryLoadViaCook to refuse the
    // cook anyway — skip the churn. Usage tagging stays (harmless meta,
    // forward-compatible when the switch is lifted).
    if (IsTextureCookDisabled())
        return;
    GameEngine::EngineCore::GetInstance().GetAssetManager().RequestAsyncReload(textureGuid);
}

void TextureService::HotSwapTexture(const GUID& textureGuid)
{
    if (textureGuid.IsNull() || !m_Device)
        return;

    // Capture the currently-bound GPU texture WITHOUT destroying it, and drop the
    // cache entry so the re-upload below produces a fresh texture for this GUID.
    Rendering::TextureHandle oldHandle{};
    m_TextureGPUCache.Remove(textureGuid, oldHandle);

    // Re-upload with the current metadata (color space + swizzle) into a new GPU
    // texture; GetOrUpload re-caches it under the same GUID.
    const Rendering::TextureHandle newHandle = GetOrUpload(textureGuid);
    if (!newHandle.IsValid())
    {
        // Upload failed (missing/broken file) — keep the old binding intact.
        // Set, not PublishOrAdopt: the failed GetOrUpload cached a negative entry
        // that has to be overwritten.
        if (oldHandle.IsValid())
            m_TextureGPUCache.Set(textureGuid, oldHandle);
        return;
    }
    if (newHandle == oldHandle)
        return; // nothing changed

    const uint32_t newIdx = GetBindlessIndex(newHandle);

    // Snapshot the materials bound to this texture (the reverse map is guarded by
    // the pending mutex), then atomically repoint each slot to the new handle/index.
    // Until this point every material still samples the OLD texture, so there is no
    // default-texture gap.
    std::vector<std::pair<GUID, StringId>> refs;
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        if (auto rev = m_TextureToMaterialBindings.find(textureGuid); rev != m_TextureToMaterialBindings.end())
            refs = rev->second;
    }
    for (const auto& [matGuid, slotName] : refs)
    {
        if (Material* mat = (*m_Materials).Find(matGuid))
        {
            ApplyTextureFilterMeta(mat, slotName, textureGuid);
            ApplyMaterialSlotBinding(mat, slotName, newHandle, newIdx);
        }
    }

    // No material references the old texture now: free its bindless slot and
    // destroy it (the device defers the VkImage teardown past frames-in-flight,
    // same as Evict).
    if (oldHandle.IsValid())
    {
        InvalidateBindless(oldHandle);
        m_Device->DestroyTexture(oldHandle);
    }
}

void TextureService::EvictCubeLut(const GUID& lutGuid)
{
    m_CubeLutWarnedFailureGuids.erase(lutGuid);
    for (auto it = m_CubeLutGpuCache.begin(); it != m_CubeLutGpuCache.end();)
    {
        if (it->first.AssetGuid != lutGuid)
        {
            ++it;
            continue;
        }

        CubeLutGpuBindingState lut = it->second;
        it = m_CubeLutGpuCache.erase(it);

        if (!m_Device)
            continue;
        if (lut.Lut3D.IsValid() && lut.Lut3D != m_IdentityLut3D)
            m_Device->DestroyTexture(lut.Lut3D);
        if (lut.Lut1DStrip.IsValid() && lut.Lut1DStrip != m_IdentityLut1DStrip)
            m_Device->DestroyTexture(lut.Lut1DStrip);
    }
}



Rendering::TextureHandle TextureService::UploadEmbeddedImage(
    const GUID& modelGuid, uint32_t imageIndex, const EmbeddedImage& image, bool linear)
{
    if (!m_Device || image.Data.empty())
        return {};

    // Key caches by CONTENT, not by (model, image index): asset packs routinely
    // embed the same texture atlas into every FBX (a Synty pack embeds one 4K
    // atlas ~800 times), and per-model keys turn that into hundreds of identical
    // GPU textures + decoded-pixel cache entries — enough to exhaust VRAM and
    // device-lose on a large scene load. Hashing the compressed bytes costs a
    // few ms; a duplicate decode+upload costs ~400ms and ~85MB of VRAM.
    const uint64_t contentHash = HashBytesFnv1a64(image.Data.data(), image.Data.size());
    const GUID contentBase = GUID::Derive(
        GUID{}, "embedded/" + std::to_string(contentHash) + "/" + std::to_string(image.Data.size()));

    // GPU texture key carries linearity (the format differs between albedo
    // and normal), so an image used as both still allocates two textures.
    // Kept distinct from the decode key (bare contentBase) so the GPU texture
    // and the CPU decode entry refcount independently — a model referencing
    // this content only as linear must not keep an orphaned sRGB texture alive.
    const GUID derivedGuid = GUID::Derive(contentBase, linear ? "linear" : "srgb");
    if (Rendering::TextureHandle shared{}; m_TextureGPUCache.TryGet(derivedGuid, shared))
    {
        // Shared hit from another model (or an earlier slot of this one):
        // record this parent's reference so hot-reload eviction refcounts
        // correctly instead of destroying a texture other models still use.
        TrackEmbeddedDerived(modelGuid, derivedGuid);
        TrackEmbeddedDerived(modelGuid, contentBase);
        return shared;
    }

    using Clock = std::chrono::high_resolution_clock;

    // The decode is linearity-agnostic — the same RGBA8 pixels feed either
    // GPU format. Cache by linearity-free key so a second material reusing
    // the same source image (with different linearity) only pays for the
    // GPU upload, not the stbi PNG/JPEG decode.
    const GUID decodeKey = contentBase;
    DecodedImage decoded;
    double decodeMs = 0.0;
    auto decodeIt = m_EmbeddedDecodeCache.find(decodeKey);
    if (decodeIt != m_EmbeddedDecodeCache.end())
    {
        decoded = decodeIt->second;
    }
    else
    {
        const auto tDecode = Clock::now();
        decoded = DecodeImageToRGBA(image.Data.data(), image.Data.size());
        decodeMs = std::chrono::duration<double, std::milli>(Clock::now() - tDecode).count();
        if (!decoded.valid)
        {
            Logger::Log::Warning("UploadEmbeddedImage: failed to decode image {} from model {}",
                                 imageIndex, modelGuid.ToString());
            return {};
        }
        m_EmbeddedDecodeCache[decodeKey] = decoded;
    }

    auto gpuFormat = linear ? Rendering::TextureFormat::RGBA8_UNORM
                            : Rendering::TextureFormat::RGBA8_SRGB;

    // An embedded image has no AssetDatabase entry, so it takes the cook
    // defaults (full chain, no normal renormalization: `linear` also covers
    // packed metallic-roughness data, which renormalizing would corrupt).
    const auto tUpload = Clock::now();
    uint32_t levels = 0;
    auto tex = UploadRgba8WithMips(m_Device, decoded.pixels.data(), decoded.width, decoded.height,
                                   gpuFormat, TextureCookSettings{}, !linear, "EmbeddedTexture",
                                   "EmbeddedTexStaging", levels);
    if (!tex.IsValid())
        return {};
    const double uploadMs = std::chrono::duration<double, std::milli>(Clock::now() - tUpload).count();
    Logger::Log::Info(
        "[ModelLoad]     EmbeddedImage[{}] {}x{} ({} mips): decode {:.1f}ms, mip+upload {:.1f}ms",
        imageIndex, decoded.width, decoded.height, levels, decodeMs, uploadMs);

    // This path is render-thread-only, so nothing else publishes this derived
    // GUID; adopt-on-collision still beats overwriting, which would orphan a
    // texture a caller already holds.
    const Rendering::TextureHandle publishedEmbedded =
        m_TextureGPUCache.PublishOrAdopt(derivedGuid, tex);
    if (publishedEmbedded != tex)
        ParkDuplicateTexture(tex);

    // Track parent->derived so a Model AssetReloaded can evict every
    // embedded-image cache entry derived from this model. The list also
    // carries the linearity-free `decodeKey` (added the first time we
    // decoded this image) so the eviction loop can drop the decode-cache
    // entry alongside the GPU textures.
    TrackEmbeddedDerived(modelGuid, derivedGuid);
    TrackEmbeddedDerived(modelGuid, decodeKey);

    return publishedEmbedded;
}

void TextureService::TrackEmbeddedDerived(const GUID& modelGuid, const GUID& derivedGuid)
{
    // Render-thread only (like every embedded resolve/evict path): content
    // keying makes unrelated models read-modify-write the same refcount
    // bucket, so off-thread model resolution would need a mutex here first.
    auto& derivedList = m_DerivedTextureGuidsByParent[modelGuid];
    if (std::find(derivedList.begin(), derivedList.end(), derivedGuid) != derivedList.end())
        return;
    derivedList.push_back(derivedGuid);
    // Content-keyed entries are shared across models; count distinct parents so
    // ReloadEmbeddedForModel only destroys an entry with its last referencing model.
    ++m_EmbeddedContentRefs[derivedGuid];
}

void TextureService::ResolveEmbeddedTextures(
    Material* mat, const MaterialDocument& doc,
    const GUID& modelGuid, const Vector<EmbeddedImage>& embeddedImages)
{
    if (!mat)
        return;

    if (embeddedImages.empty())
    {
        const bool wantsEmbedded = std::any_of(doc.textures.begin(), doc.textures.end(),
                                               [](const auto& e) { return IsEmbeddedTextureRef(e.second); });
        if (wantsEmbedded)
        {
            Logger::Log::Warning(
                "ResolveEmbeddedTextures: material '{}' references embedded images but model has none",
                mat->GetName());
            for (const auto& [texName, texRef] : doc.textures)
            {
                if (IsEmbeddedTextureRef(texRef) && mat->ValidateDocumentTextureKey(texName))
                    BindAwaitedTexture(mat, HashStringId(texName));
            }
        }
        return;
    }

    for (const auto& [texName, texRef] : doc.textures)
    {
        if (!IsEmbeddedTextureRef(texRef))
            continue;

        // Same gate as the registration bind loop (which already warned for
        // this key — the report dedupes per material+key).
        if (!mat->ValidateDocumentTextureKey(texName))
            continue;
        // A reference this model cannot satisfy leaves the slot at its awaited
        // default, as a failed load does.
        const StringId slotId = HashStringId(texName);

        const auto indexStr = texRef.substr(kEmbeddedTexturePrefixLen);
        uint32_t imgIdx = 0;
        try
        {
            imgIdx = static_cast<uint32_t>(std::stoul(indexStr));
        }
        catch (const std::exception&)
        {
            Logger::Log::Warning("ResolveEmbeddedTextures: malformed index '{}' in slot '{}'",
                                 indexStr, texName);
            BindAwaitedTexture(mat, slotId);
            continue;
        }
        // Recorded whether or not the image binds, so the model's next reload
        // knows which image this slot takes.
        TrackEmbeddedSlot(mat->GetGuid(), modelGuid, slotId, imgIdx);
        if (!BindEmbeddedImage(mat, slotId, modelGuid, imgIdx, embeddedImages))
        {
            Logger::Log::Warning(
                "ResolveEmbeddedTextures: embedded image {} is out of range (have {} images) or has no "
                "bytes; slot '{}' awaits",
                imgIdx, embeddedImages.size(), texName);
        }
    }
}

void TextureService::TrackEmbeddedSlot(const GUID& matGuid, const GUID& modelGuid, StringId slotName,
                                       uint32_t imageIndex)
{
    std::lock_guard lock(m_PendingTexturesMutex);
    EmbeddedMaterialSlots& tracked = m_EmbeddedSlotsByMaterial[matGuid];
    if (tracked.ModelGuid != modelGuid)
        tracked = EmbeddedMaterialSlots{modelGuid, {}};
    auto slotIt = std::find_if(tracked.Slots.begin(), tracked.Slots.end(),
                               [&](const EmbeddedSlot& s) { return s.SlotName == slotName; });
    if (slotIt == tracked.Slots.end())
        tracked.Slots.push_back({slotName, imageIndex});
    else
        slotIt->ImageIndex = imageIndex;
}

bool TextureService::AwaitsEmbeddedImage(const Material& mat, const GUID& matGuid, StringId slotName) const
{
    if (!mat.IsTextureAwaited(slotName))
        return false;
    std::lock_guard lock(m_PendingTexturesMutex);
    const auto matIt = m_MaterialTextureRefs.find(matGuid);
    return matIt == m_MaterialTextureRefs.end() || !matIt->second.contains(slotName);
}

bool TextureService::BindEmbeddedImage(Material* mat, StringId slotName, const GUID& modelGuid,
                                       uint32_t imageIndex, std::span<const EmbeddedImage> images)
{
    if (imageIndex >= images.size() || images[imageIndex].Data.empty())
    {
        BindAwaitedTexture(mat, slotName);
        return false;
    }
    const bool linear = IsLinearTextureSlot(slotName);
    BindMaterialTexture(mat, slotName, UploadEmbeddedImage(modelGuid, imageIndex, images[imageIndex], linear));
    return true;
}

TextureService::BindlessCacheEntry TextureService::RegisterTextureBindless(
    const Rendering::BindlessTextureDesc& desc)
{
    // Returns {0, INVALID, {}} on failure. BindlessResourceManager assigns descriptor
    // indices starting at 1; descriptor[0] in the bindless array is never written,
    // so index 0 is reserved as a "not set" sentinel. The adapter shader does NOT
    // guard against it — sampling ge_BindlessTextures[0] returns undefined data. Every
    // texture slot a material declares MUST be initialised to a valid bindless
    // index (Material::InitBindlessDefaults) and re-defaulted on hot-reload
    // (ResolveDefaultBindlessIndex) before draws can sample it. The bindless set is a
    // SAMPLED_IMAGE array (binding 0); the sampler is chosen per-draw shader-side from
    // the shared sampler array, so no SamplerHandle is needed here.
    if (!m_BindlessEnabled.load(std::memory_order_relaxed) || !desc.textureHandle.IsValid())
        return {};

    auto handle = m_Bindless->CreateBindlessTexture(desc);
    if (handle == Rendering::kInvalidBindlessTexture)
        return {};

    uint32_t idx = m_Bindless->GetTextureDescriptorIndex(handle);

    Rendering::TextureViewHandle view{};
    if (idx != 0u && m_BindlessTextureSet.IsValid() && m_Device)
    {
        // Auto-create a custom TextureView when arraySlice / mipLevel / mipCount /
        // non-Color aspect diverge from the whole-texture default (e.g. a single
        // cascade slice of a depth array texture for PCSS), or when a BC5 payload
        // needs the B->ONE safety swizzle. The view is owned by the cache entry and
        // is destroyed with the slot (InvalidateBindless / slot recycle), so a
        // registration can never leave a descriptor pointing at a view whose
        // owner released it independently.
        const bool needsSubresourceView = desc.arraySlice != 0
            || desc.mipLevel != 0
            || desc.mipCount != 1
            || desc.aspect != Rendering::TextureAspect::Color;

        // BC5 payloads are two-channel; Vulkan and Metal sample .b as 0, which
        // an un-migrated legacy `.rgb * 2 - 1` normal decode turns into z = -1
        // (inverted normal, catastrophic lighting). Swizzling B->ONE on the
        // sampled view makes such a site read z = +1 — a flattened but visually
        // plausible normal. GE_DecodeTangentNormal reads .rg only, so the
        // swizzle is inert for migrated surfaces. BC5 exists in this engine
        // solely as the texture cook's tangent-normal output.
        const bool bc5BlueOne =
            m_Device->GetTextureFormat(desc.textureHandle) == Rendering::TextureFormat::BC5_UNORM;

        if (needsSubresourceView || bc5BlueOne)
        {
            Rendering::TextureViewDesc viewDesc{};
            viewDesc.viewType = Rendering::TextureViewType::View2D;
            viewDesc.aspect = desc.aspect;
            if (needsSubresourceView)
            {
                viewDesc.baseMip = desc.mipLevel;
                // Both sides spell "all remaining levels" 0, so the request passes
                // straight through (TextureViewDesc::levelCount).
                viewDesc.levelCount = desc.mipCount;
                viewDesc.baseLayer = desc.arraySlice;
                viewDesc.layerCount = 1;
            }
            // else: whole-texture view (levelCount/layerCount 0 = all remaining)
            if (bc5BlueOne)
                viewDesc.b = Rendering::TextureSwizzle::One;
            viewDesc.debugName = desc.debugName;
            view = m_Device->CreateTextureView(desc.textureHandle, viewDesc);
        }

        // Concurrent-safe from workers: the device serializes the view
        // bookkeeping this reads, and descriptor writes to distinct array
        // elements are disjoint (see WriteDescriptorBufferUpdate). What is
        // NOT synchronized is the GPU still reading a slot, which the
        // generational park in BindlessResourceManager covers — idx here is
        // either virgin or has outlived framesInFlight frame boundaries.
        if (view.IsValid())
        {
            m_Device->UpdateImageBinding(
                m_BindlessTextureSet, 0, view, idx);
        }
        else
        {
            m_Device->UpdateImageBinding(
                m_BindlessTextureSet, 0, desc.textureHandle, idx);
        }
    }

    return {idx, handle, view};
}

// Thread contract: callable concurrently from ECS extraction workers. The
// extraction wave holds TerrainExtraction and RenderExtraction together (their
// dependencies levelize onto the same wave), and SystemManager dispatches a
// multi-system wave onto JobSystem workers — so two workers reach this on the
// same frame. TerrainExtractionSystem.cpp:56 and RenderExtractionSystem.cpp:2194
// (via BindMaterialTexture) are the two live entries. Registration therefore
// races by design; the cache insert below picks one winner and the loser is
// retired on the render thread's pending-discard drain, which keeps bindless
// slot parking frame-ordered (see the drain in Update).
uint32_t TextureService::GetBindlessIndex(
    const Rendering::BindlessTextureDesc& desc)
{
    if (!m_BindlessEnabled.load(std::memory_order_relaxed) || !desc.textureHandle.IsValid())
        return 0u;

    BindlessCacheKey key{desc.textureHandle.id, desc.arraySlice, desc.mipLevel, desc.mipCount,
                         desc.aspect};

    {
        std::lock_guard<std::mutex> lock(m_BindlessMutex);
        auto it = m_BindlessTextureIndexCache.find(key);
        if (it != m_BindlessTextureIndexCache.end())
            return it->second.bindlessIndex;
    }

    // Registration calls into BindlessResourceManager (which has its own mutex)
    // and the device — do this outside our lock to avoid nested locking.
    auto entry = RegisterTextureBindless(desc);
    if (entry.bindlessIndex == 0u)
        return 0u;

    uint32_t resultIdx = 0u;
    {
        std::lock_guard<std::mutex> lock(m_BindlessMutex);
        auto [it, inserted] = m_BindlessTextureIndexCache.try_emplace(key, entry);
        if (!inserted)
        {
            // Lost the concurrent-register race: both threads wrote a descriptor
            // into different bindless slots. The loser's index was never returned
            // to any caller, so no draw can sample it, and its slot is parked by
            // DestroyBindlessTexture for delayed reuse — but the retirement itself
            // touches IDevice, which this thread may not do (see the contract note
            // above). Hand it to the render thread instead.
            m_PendingGpuDiscards.push_back({entry.bindlessHandle, entry.bindlessView, {}});
            resultIdx = it->second.bindlessIndex;
        }
        else
        {
            resultIdx = entry.bindlessIndex;
        }
    }
    return resultIdx;
}

uint32_t TextureService::GetBindlessIndex(Rendering::TextureHandle tex)
{
    Rendering::BindlessTextureDesc desc{};
    desc.textureHandle = tex;
    desc.type = Rendering::BindlessResourceType::Texture2D;
    return GetBindlessIndex(desc);
}

// Thread contract: callable concurrently from render-graph record workers.
// MaterialBinder::BuildSetInternal (MaterialBinder.cpp:472) reaches this from a
// pass execute lambda, and RGRecord.cpp:367 invokes those lambdas on
// WorkStealingThreadPool workers for the seven RecordInSecondary() pass families
// when GE_PARALLEL_RECORD is set (default OFF since 3f6b81fc7). MaterialBinder's
// neighbouring scratch state is thread_local for the same reason.
Rendering::SamplerHandle TextureService::GetSampler(Rendering::SamplerPreset preset)
{
    if (preset >= Rendering::SamplerPreset::kCount)
    {
        assert(false && "Invalid SamplerPreset");
        return {};
    }
    int idx = static_cast<int>(preset);

    {
        std::lock_guard<std::mutex> lock(m_BindlessMutex);
        if (m_SamplerCache[idx].IsValid())
            return m_SamplerCache[idx];
    }

    // Create outside the lock (CreateSampler may be expensive).
    auto handle = m_Device->CreateSampler(Rendering::ResolveSamplerPreset(preset, m_Profile));
    if (!handle.IsValid())
        return {};

    {
        std::lock_guard<std::mutex> lock(m_BindlessMutex);
        if (!m_SamplerCache[idx].IsValid())
        {
            m_SamplerCache[idx] = handle;
        }
        else
        {
            // Another thread created it first — park our duplicate for the render
            // thread rather than calling DestroySampler from a record worker.
            m_PendingGpuDiscards.push_back({Rendering::kInvalidBindlessTexture, {}, handle});
        }
        return m_SamplerCache[idx];
    }
}

void TextureService::InvalidateBindless(Rendering::TextureHandle tex)
{
    if (!tex.IsValid())
        return;

    // Collect handles to free under the lock, then destroy outside it to avoid
    // holding m_BindlessMutex while calling into BindlessResourceManager.
    std::vector<Rendering::BindlessTextureHandle> toDestroy;
    std::vector<Rendering::TextureViewHandle> viewsToDestroy;
    {
        std::lock_guard<std::mutex> lock(m_BindlessMutex);
        for (auto it = m_BindlessTextureIndexCache.begin(); it != m_BindlessTextureIndexCache.end(); )
        {
            if (it->first.textureId == tex.id)
            {
                toDestroy.push_back(it->second.bindlessHandle);
                if (it->second.bindlessView.IsValid())
                    viewsToDestroy.push_back(it->second.bindlessView);
                it = m_BindlessTextureIndexCache.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    if (m_Bindless)
    {
        for (auto h : toDestroy)
            m_Bindless->DestroyBindlessTexture(h);
    }
    if (m_Device)
    {
        for (auto v : viewsToDestroy)
            m_Device->DestroyTextureView(v);
    }
}

void TextureService::ApplyMaterialSlotBinding(Material* mat, StringId slotName,
                                              Rendering::TextureHandle tex,
                                              uint32_t bindlessIndex)
{
    if (!mat)
        return;
    mat->SetTexture(slotName, tex);
    mat->SetBindlessTextureIndex(slotName, bindlessIndex);
    // Classic mode reads the slot's TextureHandle when it builds the material's
    // bind group, so the cached set is now stale. Bindless mode needs nothing:
    // the index it just took is live in the global set.
    if (m_IndexingMode == MaterialIndexingMode::Classic)
        m_MaterialBindings.Invalidate(mat->GetGuid());
}

void TextureService::DiscardTextureDeferred(Rendering::TextureHandle tex)
{
    if (!tex.IsValid())
        return;

    // Everything lands on m_PendingGpuDiscards for FlushPendingUploads to
    // retire on the render thread — no destroy calls from here, so any wave
    // worker may park a texture it owns.
    std::lock_guard<std::mutex> lock(m_BindlessMutex);
    for (auto it = m_BindlessTextureIndexCache.begin(); it != m_BindlessTextureIndexCache.end();)
    {
        if (it->first.textureId == tex.id)
        {
            m_PendingGpuDiscards.push_back({it->second.bindlessHandle, it->second.bindlessView, {}, {}});
            it = m_BindlessTextureIndexCache.erase(it);
        }
        else
        {
            ++it;
        }
    }
    m_PendingGpuDiscards.push_back({Rendering::kInvalidBindlessTexture, {}, {}, tex});
}

void TextureService::RestoreMaterialTextureBinding(Material* mat, const GUID& matGuid, StringId slotName)
{
    if (!mat)
        return;

    GUID authoredTexGuid{};
    {
        std::lock_guard lock(m_PendingTexturesMutex);
        if (auto matIt = m_MaterialTextureRefs.find(matGuid); matIt != m_MaterialTextureRefs.end())
        {
            if (auto slotIt = matIt->second.find(slotName); slotIt != matIt->second.end())
                authoredTexGuid = slotIt->second;
        }
    }

    // The override ends. With no authored texture the slot clears the same way the document
    // hot-reload sweep does. Otherwise the authored texture binds at once when it is resident (the
    // common case: it was uploaded at material registration), or the slot awaits it until its load
    // lands, never waiting for it here.
    if (authoredTexGuid.IsNull())
    {
        ApplyMaterialSlotBinding(mat, slotName, Rendering::TextureHandle{},
                                 ResolveDefaultBindlessIndex(slotName));
        return;
    }
    if (Rendering::TextureHandle authored; m_TextureGPUCache.TryGet(authoredTexGuid, authored))
    {
        BindMaterialTexture(mat, slotName, authored);
        return;
    }
    if (!GameEngine::EngineCore::GetInstance().IsInitialized())
    {
        // No asset manager to load on (a bare render-services host): upload in place. A failed
        // upload leaves the slot at its awaited default.
        BindMaterialTexture(mat, slotName, GetOrUpload(authoredTexGuid));
        return;
    }
    BindAwaitedTexture(mat, slotName);
    ScheduleTextureUploadJob(authoredTexGuid, matGuid, slotName);
}

void TextureService::BindMaterialTexture(Material* mat, StringId slotName, Rendering::TextureHandle tex)
{
    if (!mat)
        return;
    // Every caller hands the texture a document assigned: an invalid handle is
    // one that failed to upload.
    if (!tex.IsValid())
    {
        BindAwaitedTexture(mat, slotName);
        return;
    }
    // Sampler is selected shader-side from the material's packed SamplerIndices
    // (Bindless) or pre-selected per slot in the bind group (Classic); the
    // bindless index is texture-only. Classic registers nothing — GetBindlessIndex
    // returns the 0 sentinel there and the index lane goes unread.
    if (m_IndexingMode == MaterialIndexingMode::Classic)
    {
        ApplyMaterialSlotBinding(mat, slotName, tex, 0u);
        return;
    }

    const uint32_t bindlessIdx = GetBindlessIndex(tex);
    if (bindlessIdx == 0u)
    {
        // CPU side now claims a binding (m_Textures[slot] = tex) but bindless
        // registration failed — falling through to the previous index would
        // leave the material sampling whatever was there before, or the "not
        // set" sentinel. Repaint with the per-slot default so the GPU side
        // matches the CPU side's "no usable texture" state.
        Logger::Log::Warning(
            "BindMaterialTexture: bindless registration failed for material "
            "{} slotId={:#x} (texture handle {}); falling back to default index.",
            mat->GetGuid().ToString(), slotName, tex.id);
        ApplyMaterialSlotBinding(mat, slotName, tex,
                                 DefaultBindlessIndex(AwaitedSlotDefault(TextureSlotFromName(slotName))));
        return;
    }
    ApplyMaterialSlotBinding(mat, slotName, tex, bindlessIdx);
}

void TextureService::TrackMaterialTextureRefLocked(const GUID& matGuid, StringId slotName, const GUID& texGuid)
{
    if (matGuid.IsNull() || texGuid.IsNull())
        return;

    // Drop the previous (matGuid, slotName) → oldTexGuid edge if any. Without
    // this a rebind from texA to texB leaves a stale reverse entry, and a
    // future eviction of texA would try to "rebind" a slot that's already
    // moved on.
    auto& slotMap = m_MaterialTextureRefs[matGuid];
    auto slotIt = slotMap.find(slotName);
    if (slotIt != slotMap.end() && slotIt->second != texGuid)
    {
        auto reverseIt = m_TextureToMaterialBindings.find(slotIt->second);
        if (reverseIt != m_TextureToMaterialBindings.end())
        {
            auto& refs = reverseIt->second;
            refs.erase(std::remove_if(refs.begin(), refs.end(),
                                      [&](const std::pair<GUID, StringId>& p) {
                                          return p.first == matGuid && p.second == slotName;
                                      }),
                       refs.end());
            if (refs.empty())
                m_TextureToMaterialBindings.erase(reverseIt);
        }
    }

    slotMap[slotName] = texGuid;
    auto& refs = m_TextureToMaterialBindings[texGuid];
    // Linear-scan dedup — refs per texture is small in practice.
    auto refIt = std::find_if(refs.begin(), refs.end(),
                              [&](const std::pair<GUID, StringId>& p) {
                                  return p.first == matGuid && p.second == slotName;
                              });
    if (refIt == refs.end())
        refs.emplace_back(matGuid, slotName);
}

void TextureService::UntrackMaterialTextureRefLocked(const GUID& matGuid, StringId slotName)
{
    if (matGuid.IsNull())
        return;
    auto matIt = m_MaterialTextureRefs.find(matGuid);
    if (matIt == m_MaterialTextureRefs.end())
        return;
    auto slotIt = matIt->second.find(slotName);
    if (slotIt == matIt->second.end())
        return;
    const GUID texGuid = slotIt->second;
    matIt->second.erase(slotIt);
    if (matIt->second.empty())
        m_MaterialTextureRefs.erase(matIt);

    auto reverseIt = m_TextureToMaterialBindings.find(texGuid);
    if (reverseIt == m_TextureToMaterialBindings.end())
        return;
    auto& refs = reverseIt->second;
    refs.erase(std::remove_if(refs.begin(), refs.end(),
                              [&](const std::pair<GUID, StringId>& p) {
                                  return p.first == matGuid && p.second == slotName;
                              }),
               refs.end());
    if (refs.empty())
        m_TextureToMaterialBindings.erase(reverseIt);
}

void TextureService::UntrackAllMaterialTextureRefsLocked(const GUID& matGuid)
{
    auto matIt = m_MaterialTextureRefs.find(matGuid);
    if (matIt == m_MaterialTextureRefs.end())
        return;
    for (const auto& [slotName, texGuid] : matIt->second)
    {
        auto reverseIt = m_TextureToMaterialBindings.find(texGuid);
        if (reverseIt == m_TextureToMaterialBindings.end())
            continue;
        auto& refs = reverseIt->second;
        refs.erase(std::remove_if(refs.begin(), refs.end(),
                                  [&](const std::pair<GUID, StringId>& p) {
                                      return p.first == matGuid && p.second == slotName;
                                  }),
                   refs.end());
        if (refs.empty())
            m_TextureToMaterialBindings.erase(reverseIt);
    }
    m_MaterialTextureRefs.erase(matIt);
}

void TextureService::UntrackTextureFromAllMaterialsLocked(const GUID& texGuid)
{
    auto reverseIt = m_TextureToMaterialBindings.find(texGuid);
    if (reverseIt == m_TextureToMaterialBindings.end())
        return;
    for (const auto& [matGuid, slotName] : reverseIt->second)
    {
        auto matIt = m_MaterialTextureRefs.find(matGuid);
        if (matIt == m_MaterialTextureRefs.end())
            continue;
        matIt->second.erase(slotName);
        if (matIt->second.empty())
            m_MaterialTextureRefs.erase(matIt);
    }
    m_TextureToMaterialBindings.erase(reverseIt);
}

void TextureService::TrackMaterialTextureRefForTesting(const GUID& matGuid, StringId slotName, const GUID& texGuid)
{
    std::lock_guard lock(m_PendingTexturesMutex);
    TrackMaterialTextureRefLocked(matGuid, slotName, texGuid);
}

size_t TextureService::CountMaterialsReferencingTextureForTesting(const GUID& texGuid) const
{
    std::lock_guard lock(m_PendingTexturesMutex);
    auto it = m_TextureToMaterialBindings.find(texGuid);
    return it == m_TextureToMaterialBindings.end() ? 0u : it->second.size();
}

bool TextureService::MaterialTextureRefForTesting(const GUID& matGuid, StringId slotName, GUID& outTexGuid) const
{
    std::lock_guard lock(m_PendingTexturesMutex);
    auto matIt = m_MaterialTextureRefs.find(matGuid);
    if (matIt == m_MaterialTextureRefs.end())
        return false;
    auto slotIt = matIt->second.find(slotName);
    if (slotIt == matIt->second.end())
        return false;
    outTexGuid = slotIt->second;
    return true;
}

void TextureService::UpdateMaterialTextures(const GUID& materialGuid, const MaterialDocument& doc)
{
    Material* mat = (*m_Materials).Find(materialGuid);
    if (!mat)
        return;

    // Update sampler preset before rebinding so new bindless entries pick the
    // intended filter. Existing bindless entries keyed on the previous (tex,
    // sampler) pair remain valid cache hits but won't be used after this
    // function because BindMaterialTexture below re-resolves the slot indices.
    // Same-value guarded MarkDirty: the sampler nibble is packed into the
    // MaterialParams SSBO, so a change must invalidate the pack skip stamp.
    if (const auto samplerPreset = SamplerPresetFromMaterialFilter(doc.textureFilter);
        mat->m_SamplerPreset != samplerPreset)
    {
        mat->m_SamplerPreset = samplerPreset;
        mat->MarkDirty();
    }

    // Full-document apply, same as registration: prune warned entries for
    // keys the edited document no longer contains, so a repaired-then-relapsed
    // key reports again.
    mat->PruneTextureKeyWarnings(doc.textures);
    for (const auto& [texName, texGuidStr] : doc.textures)
    {
        // A key that names no slot is skipped whole — clearing it would plant
        // a phantom m_Textures entry under a name nothing can ever bind.
        if (!mat->ValidateDocumentTextureKey(texName))
            continue;
        StringId nameId = HashStringId(texName);
        if (texGuidStr.empty())
        {
            mat->SetTexture(nameId, Rendering::TextureHandle{});
            mat->SetBindlessTextureIndex(nameId, ResolveDefaultBindlessIndex(nameId));
            std::lock_guard lock(m_PendingTexturesMutex);
            UntrackMaterialTextureRefLocked(materialGuid, nameId);
            continue;
        }
        auto texPathIt = doc.texturePaths.find(texName);
        GUID texGuid = ResolveTextureRefGuid(
            texGuidStr, texPathIt != doc.texturePaths.end() ? texPathIt->second : std::string());
        if (texGuid.IsNull())
        {
            BindAwaitedTexture(mat, nameId);
            std::lock_guard lock(m_PendingTexturesMutex);
            UntrackMaterialTextureRefLocked(materialGuid, nameId);
            continue;
        }
        // The same bind as registration (tag, bind a resident upload at once), except that a
        // texture not on the GPU loads through an upload job: adopt its cooked artifact, else
        // decode and build mips, on a worker; FlushPendingUploads submits it. The editor's
        // texture assign comes through here, so it never decodes on the calling thread, and a
        // texture never loaded shows within its decode, not after an encode.
        BindMaterialTextureSlot(mat, materialGuid, nameId, texGuid, TextureLoadRoute::UploadJob);
    }
}

bool TextureService::IsMaterialTextureBindingComplete(const GUID& matGuid) const
{
    if (matGuid.IsNull())
        return true;
    // Worker callbacks mutate m_PendingTextureBinds under the same mutex
    // (see ScheduleMaterialTextureLoad). Reading without the lock would
    // race with vector mutations from those callbacks.
    std::lock_guard lock(m_PendingTexturesMutex);
    for (const auto& bind : m_PendingTextureBinds)
    {
        if (bind.MaterialGuid == matGuid)
            return false;
    }
    return true;
}

size_t TextureService::PendingMaterialTextureBindCount() const
{
    std::lock_guard lock(m_PendingTexturesMutex);
    return m_PendingTextureBinds.size();
}

} // namespace Engine::Renderer
} // namespace GameEngine
