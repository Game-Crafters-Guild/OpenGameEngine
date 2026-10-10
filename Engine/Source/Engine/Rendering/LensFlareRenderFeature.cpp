#include "Engine/Rendering/LensFlareRenderFeature.h"

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <string>

namespace GameEngine::Engine::Renderer
{

using namespace ::GameEngine::Rendering;

namespace
{
constexpr uint32_t kVertexStage = kShaderStageVertex;
constexpr uint32_t kFragmentStage = kShaderStageFragment;
} // namespace

bool LensFlareRenderFeature::Initialize(IDevice* device)
{
    if (m_Initialized)
        return true;
    if (!device)
        return false;
    m_Device = device;

    // Canonical core-node shader path: load the prebuilt .shaderpkg staged next to
    // the executable (built by the Rendering module's CompileShaderPkgs target).
    ShaderPackage pkg{};
    std::string loadErr;
    if (!LoadShaderPkg("Shaders/lens_flare.shaderpkg", device->PreferredShaderSource(), pkg, &loadErr))
    {
        Logger::Log::Warning("LensFlareRenderFeature: failed to load shaderpkg: {}", loadErr);
        return false;
    }
    auto vs = pkg.stageBytes.find("vs");
    auto fsIt = pkg.stageBytes.find("fs");
    if (vs == pkg.stageBytes.end() || fsIt == pkg.stageBytes.end() || vs->second.empty() ||
        fsIt->second.empty())
    {
        Logger::Log::Warning("LensFlareRenderFeature: shaderpkg missing vs/fs stage bytes.");
        return false;
    }

    // Resolve each set0 binding index from the shader's reflected meta by name so
    // a layout edit in lens_flare.vert/frag stays in lockstep with the C++ binds.
    // Only the INDEX is taken from reflection; the descriptor type stays explicit
    // (the reflected type enum is unreliable for images). A miss falls back to the
    // GLSL literal with a warning. The SSBO reflects under its block name
    // "Instances", not the member "uInstances".
    auto resolveBinding = [&pkg](const char* name, uint32_t fallback) -> uint32_t {
        for (const auto& setMeta : pkg.meta.Sets)
        {
            if (setMeta.Set != 0)
                continue;
            for (const auto& b : setMeta.Bindings)
                if (b.Name == name)
                    return b.Binding;
            break;
        }
        Logger::Log::Warning("LensFlareRenderFeature: failed to resolve set0 binding '{}' from "
                             "reflection; falling back to literal {}",
                             name, fallback);
        return fallback;
    };
    m_AtlasBinding = resolveBinding("uAtlas", 0u);
    m_InstanceBinding = resolveBinding("Instances", 1u);
    m_DepthBinding = resolveBinding("uSceneDepth", 2u);

    m_Layout.debugName = "LensFlare.Set0";
    m_Layout.bindings = {
        {m_AtlasBinding, DescriptorType::CombinedImageSampler, 1, kFragmentStage}, // atlas
        {m_InstanceBinding, DescriptorType::StorageBuffer, 1, kVertexStage},       // instances
        {m_DepthBinding, DescriptorType::CombinedImageSampler, 1, kFragmentStage}, // scene depth
    };

    m_Pipeline = {};
    m_Pipeline.type = PipelineType::Graphics;
    m_Pipeline.vertexShader = vs->second;
    m_Pipeline.pixelShader = fsIt->second;
    m_Pipeline.rasterizationSamples = 1;
    m_Pipeline.EnableDepthTest(false);
    m_Pipeline.SetCullingMode(CullModeFlagBits::None);
    m_Pipeline.pushConstantSize = sizeof(int32_t);
    m_Pipeline.pushConstantStagesMask = kFragmentStage;
    m_Pipeline.EnableBlending(true, BlendFactor::One, BlendFactor::One); // additive color
    // Keep the flare light additive, but accumulate a proper coverage alpha for
    // transparent thumbnail/UI compositing instead of saturating every quad.
    auto& blend = m_Pipeline.colorBlendState.attachments[0];
    blend.srcAlphaBlendFactor = BlendFactor::One;
    blend.dstAlphaBlendFactor = BlendFactor::OneMinusSrcAlpha;
    m_Pipeline.AddDynamicState(DynamicState::Viewport);
    m_Pipeline.AddDynamicState(DynamicState::Scissor);
    m_Pipeline.descriptorSetLayouts.push_back(m_Layout);
    m_Pipeline.debugName = "LensFlare";

    m_PipelineId = PipelineDescTranslator::InternGraphics(*device, m_Pipeline);
    if (!m_PipelineId.IsValid())
    {
        Logger::Log::Warning("LensFlareRenderFeature: failed to intern pipeline.");
        return false;
    }

    m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("LensFlare_AtlasSampler"));
    if (!m_Sampler.IsValid())
        return false;

    // Ring depth = device frames-in-flight, so a slot is never overwritten while
    // the GPU may still be reading it (clamped to a sane minimum of 1).
    m_FramesInFlight = device->GetFramesInFlight();
    if (m_FramesInFlight == 0)
        m_FramesInFlight = 1;

    m_Initialized = true;
    return true;
}

void LensFlareRenderFeature::OnDeviceRebuilt(IDevice* device)
{
    if (!device || !m_Initialized)
        return;
    m_Device = device;
    // The per-view instance rings are persistently-mapped storage buffers the rebuild
    // freed — their cached mapped pointers now dangle, so UploadInstances' next
    // UpdateBuffer would write through freed VMA memory (a real UAF, which generational
    // handles do NOT guard). Drop the rings WITHOUT DestroyBuffer (the pool already
    // freed them) so UploadInstances re-creates + re-maps a fresh slot on next use.
    m_ViewRings.clear();
    // The atlas sampler is created eagerly in Initialize, whose m_Initialized guard
    // means it will not recreate it; do so here (the old dead handle is just
    // overwritten). The interned pipeline self-heals via warm recompile.
    m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("LensFlare_AtlasSampler"));
}

void LensFlareRenderFeature::SetFlares(std::vector<ResolvedFlare> flares)
{
    m_Flares = std::move(flares);
}

void LensFlareRenderFeature::ClearFrameData()
{
    m_Flares.clear();
}

BufferHandle LensFlareRenderFeature::UploadInstances(IDevice& device, uint32_t viewId,
                                                     uint32_t frameIndex,
                                                     const std::vector<FlareInstanceGPU>& instances,
                                                     uint32_t& outCount)
{
    outCount = 0;
    if (instances.empty())
        return {};

    ViewRing& ring = m_ViewRings[viewId];
    if (ring.Buffers.size() != m_FramesInFlight)
    {
        ring.Buffers.assign(m_FramesInFlight, {});
        ring.Capacity.assign(m_FramesInFlight, 0u);
    }

    const uint32_t slot = frameIndex % m_FramesInFlight;
    const uint32_t needed = static_cast<uint32_t>(instances.size());

    if (!ring.Buffers[slot].IsValid() || ring.Capacity[slot] < needed)
    {
        if (ring.Buffers[slot].IsValid())
            device.DestroyBuffer(ring.Buffers[slot]);
        // Round capacity up to reduce churn as flare counts fluctuate.
        uint32_t capacity = 64;
        while (capacity < needed)
            capacity *= 2;
        BufferDesc bd{};
        bd.size = static_cast<uint64_t>(capacity) * sizeof(FlareInstanceGPU);
        bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        // FrameSlotted: one element of a ring exactly as deep as the device paces.
        // UploadInstances is reached only from LensFlareRenderNode::DeclareForView,
        // which runs behind BeginFrame; an upload from anywhere earlier would race a
        // frame in flight.
        bd.flags = BufferCreateFlags::PersistentlyMapped | BufferCreateFlags::FrameSlotted;
        bd.debugName = "LensFlare_Instances";
        ring.Buffers[slot] = device.CreateBuffer(bd);
        ring.Capacity[slot] = ring.Buffers[slot].IsValid() ? capacity : 0;
    }
    if (!ring.Buffers[slot].IsValid())
        return {};

    device.UpdateBuffer(ring.Buffers[slot], 0, needed * sizeof(FlareInstanceGPU), instances.data());
    outCount = needed;
    return ring.Buffers[slot];
}

} // namespace GameEngine::Engine::Renderer
