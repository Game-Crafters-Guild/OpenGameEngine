// RenderServices.cpp
// Part of the RenderServices implementation — split by concern from the
// former single RenderServices.cpp. All files define members of the same
// RenderServices class; shared file-scope helpers live in RenderServicesDetail.h.
#include "Engine/Rendering/RenderServices.h"
#include "Core/CpuProfiler.h"
#include "Engine/Rendering/AntiAliasingEnvOverrides.h"
#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/RTShadowMaskService.h"
#include "Engine/Rendering/SceneAccelerationStructureService.h"
#include "Engine/Rendering/SceneColorGrab.h"
#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowPasses.h"

#include "Core/DebugMetrics.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Engine/Rendering/MeshReprovisionSource.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Assets/AssetManager.h"
#include "Assets/BinaryAsset.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/Packages/PackageResolver.h"
#include "Assets/TextureAsset.h"
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/EmbeddedImageDecoder.h"
#include "Engine/Rendering/HlodRuntime.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Engine/Rendering/RetargetRenderFeature.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Logger/Logger.h"
#include "Platform/Environment.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/BindlessResourceManager.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/PipelineCache.h"
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
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "RenderServicesDetail.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;



RenderServices::RenderServices()
    : m_DepthUnderDraw(std::make_unique<DepthUnderDrawTracker>()),
      m_HlodRuntime(std::make_unique<Hlod::HlodRuntime>())
{
}

// The prewarm-drain backstop moved to ~MaterialSystem with the prewarm state
// (A1.3); the facade member's destruction runs it during ~RenderServices.
RenderServices::~RenderServices()
{
    // Publish death BEFORE any member tears down. A destructor body runs ahead
    // of member destruction, so a subscriber destroyed later (ECS systems are
    // torn down with the world, after RenderDeviceContext destroys this) sees
    // the token already false and skips its call back in.
    m_Alive->store(false, std::memory_order_release);
}

bool RenderServices::IsPackageAvailable(std::string_view packageName) const
{
    if (m_PackageAvailabilityQuery)
        return m_PackageAvailabilityQuery(packageName);

    // Standalone render tests and tooling can construct RenderServices without
    // an EngineCore asset manager. Preserve that host shape unless it opts into
    // an explicit package query above.
    GameEngine::AssetManager* assetManager = GameEngine::EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
        return true;

    const std::string alias = GameEngine::SanitizePackageAlias(packageName);
    return !assetManager->GetSourceRoot(alias).empty();
}

std::optional<Rendering::MaterialKeyword> RenderServices::GetWorldPassKeywords(Rendering::ViewId viewId) const
{
    const auto* pv = m_ViewRegistry.FindPerView(viewId);
    if (!pv)
        return std::nullopt;
    return pv->WorldPassKeywords;
}

namespace
{
static std::vector<uint8_t> LoadDefaultWorldShaderBytes(const char* name)
{
    return GameEngine::Rendering::Utils::LoadShaderFile(name);
}

static std::vector<uint8_t> LoadCullingShaderFromAssets(const char* name)
{
    if (!name || !name[0])
        return {};

    // Shader packages are runtime artifacts, not registered assets.
    {
        std::filesystem::path p(name);
        auto ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".shaderpkg")
            return GameEngine::Rendering::Utils::LoadShaderFile(name);
    }

    using GameEngine::Asset;
    using GameEngine::AssetManager;
    using GameEngine::BinaryAsset;
    using GameEngine::GUID;

    AssetManager& am = GameEngine::EngineCore::GetInstance().GetAssetManager();
    const auto abs = am.ResolveAssetPath(name);
    GUID guid = am.GetRegistry().GetAssetGUID(abs);
    if (!guid.IsNull())
    {
        SharedPtr<Asset> asset = am.GetAsset(guid);
        if (!asset)
            asset = am.LoadAssetAsync(guid).get();
        auto bin = std::dynamic_pointer_cast<BinaryAsset>(asset);
        if (bin && bin->HasData())
        {
            auto vec = bin->GetDataCopy();
            return std::vector<uint8_t>(vec.begin(), vec.end());
        }
    }

    return LoadDefaultWorldShaderBytes(name);
}

} // namespace

bool RenderServices::Initialize(Rendering::IDevice* device)
{
    m_Device = device;
    if (!m_Device)
        return false;
    // Stage shaders asked for by .spv name resolve to the form this device ingests.
    Rendering::Utils::SetPreferredShaderSource(m_Device->PreferredShaderSource());
    // The registry mirrors RS's device (A2): set here, nulled at the SAME point
    // as m_Device in Shutdown so a ReleaseView during the teardown window still
    // frees per-view GPU buffers through a live device.
    m_ViewRegistry.SetDevice(m_Device);
    // The one cross-subsystem lifecycle edge (sec 3.3): ReleaseView clears the
    // view's draw-builder submissions and temporal history after tearing
    // down its own state.
    m_ViewRegistry.SetViewReleasedCallback(
        [this](Rendering::ViewId id)
        {
            m_WorldDrawBuilder.ClearView(id);
            m_ViewTemporalHistory.ReleaseView(id);
        });
    Rendering::IDevice* const dev = m_Device;

    // Feature policy, derived once. Everything downstream — texture indexing
    // mode, draw submission, advanced-feature passes, shader defines — gates on
    // this, never on raw caps at the call site.
    m_Profile = Rendering::RendererProfile::FromCapabilities(m_Device->GetCapabilities());
    m_Profile.LogResolved();
    Rendering::ApplyShaderCompileProfile(m_Profile);

    // The sample count of the best AA rung this device can run. Hosts that
    // apply settings (the editor per project, the Player from game.config)
    // overwrite it; hosts that do not render at this count wherever the
    // resolved AA mode is MSAA.
    m_AA.MsaaSamples = ResolveDefaultAntiAliasing(GetAntiAliasingDeviceCaps()).SampleCount;

    auto MsSince = [](const auto& t) {
        return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t).count();
    };
    const auto tRSStart = std::chrono::high_resolution_clock::now();

    // Default render-scale override. The project's persisted setting applies
    // later (project open) and per-view overrides beat both; all three are fully
    // live — the pipeline pre-pass re-derives the internal extent each frame,
    // and the material texture mip bias rides ViewParams per view
    // (ViewParamsUploadNode), never the samplers.
    if (const char* env = std::getenv("GE_TAA_RENDER_SCALE"))
    {
        const float v = static_cast<float>(std::atof(env));
        if (v >= kMinRenderScale && v <= kMaxRenderScale)
            SetDefaultRenderScale(v);
        else
            Logger::Log::Warning("GE_TAA_RENDER_SCALE '{}' out of range [{}, 1.0]; keeping {}", env,
                                 kMinRenderScale, m_DefaultRenderScale);
    }

    // Which producer arm writes deforming motion vectors. Three independent
    // switches, every one off by default, so a run that sets none records
    // nothing and the frame is the frame it was before this work. They select
    // between the options the producer comparison measures; enabling more than
    // one is refused (they share one target). The measurement slice replaces
    // the switches with the winner.
    SetDeformationMotionArm(ResolveDeformationMotionArm(
        Platform::EnvironmentSwitchEnabled("GE_DEFORMATION_MOTION_COLOR_PASS_TARGET", false),
        Platform::EnvironmentSwitchEnabled("GE_DEFORMATION_MOTION_PREPASS_TARGET", false),
        Platform::EnvironmentSwitchEnabled("GE_DEFORMATION_MOTION_SEPARATE_PASS", false)));
    // Stated once, so a run that enabled a switch and sees no motion can tell
    // "the arm is off" from "the arm ran and recorded nothing" without a build.
    if (m_DeformationMotionArm != DeformationMotionArm::None)
        Logger::Log::Info("Deformation motion: producing through the {} arm.",
                          DeformationMotionArmName(m_DeformationMotionArm));

    // Texture residency + binding service: bindless array, GPU texture cache,
    // samplers, defaults, LUTs, async upload pump.
    m_Textures = std::make_unique<TextureService>();
    m_Textures->Initialize(m_Device, m_MaterialSystem.Registry(), m_Profile);

    m_GpuScene = std::make_unique<Rendering::GPUScene>(m_Device);

    // GPU mesh registry: owns deduplicated GPU buffers for model submeshes.
    m_MeshGPURegistry.Initialize(m_Device);
    m_MeshGPURegistry.SetGPUScene(m_GpuScene.get());

    // Register the shader loader BEFORE creating the GPUDrawStreamBuilder so
    // its lazy CreatePipeline call (during Initialize) has a valid loader. The
    // same loader serves the other compute loaders registered further down.
    Rendering::GPUDrawStreamBuilder::SetShaderLoader(&LoadCullingShaderFromAssets);

    // Bucketer host driver. 8192 drawCmds/stream = ~200KB per slot
    // (drawCmd + count + indirection). 80-100 batches in a busy scene
    // -> ~16-20MB total. Larger scenes can bump this if needed.
    try
    {
        m_DrawStreamBuilder = std::make_unique<Rendering::GPUDrawStreamBuilder>(m_Device);
        if (!m_DrawStreamBuilder->Initialize())
        {
            Logger::Log::Warning(
                "RenderServices::Initialize: GPUDrawStreamBuilder initialization failed; GPU-driven draw streams will retry lazily");
        }
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning(
            "RenderServices::Initialize: GPUDrawStreamBuilder initialization exception: {}",
            e.what());
        m_DrawStreamBuilder.reset();
    }
    catch (...)
    {
        Logger::Log::Warning(
            "RenderServices::Initialize: GPUDrawStreamBuilder initialization unknown exception");
        m_DrawStreamBuilder.reset();
    }

    // The material stack (A1.3): prewarm/binder/variants construction, the eager
    // compiler creation, and the registry init + pre-unregister callback wiring
    // all live behind the facade now. Placed here (the old registry-init point)
    // so TextureService::Initialize above still binds the registry reference
    // before the registry is initialized, exactly as before; the binder/variants/
    // prewarm construction moved down from the top of Initialize (nothing between
    // touches them). See MaterialSystem::Initialize.
    m_MaterialSystem.Initialize(m_Device, m_PerFrameWritePool, *m_Textures, *this,
                                [this] { return ResolveWorldPassKeywordsForPrewarm(); });

    // Per-frame double-buffered SSBO pools for material params and bone palettes.
    InitializePerFrameWritePool();

    // Initialize GPU animation data store for Tier 2 compute skinning.
    // The compute pass and shader are loaded lazily in BuildFrameGraph
    // (the shader path resolver isn't configured yet at this point).
    m_GPUAnimDataStore.Initialize(m_Device);

    // Create minimal fallback buffers for Forward+ bindings so composed/material shaders
    // that declare clustered resources don't cause us to skip binding set0 when the
    // active pipeline doesn't provide those buffers.
    CreateFallbackBuffers();

    // Developer AA env overrides (GE_MSAA_SAMPLES / GE_AA_MODE / GE_TAA_SAMPLES;
    // parsed by AntiAliasingEnvOverrides). Applied in that order, so an explicit
    // GE_AA_MODE wins over the mode a legacy GE_MSAA_SAMPLES > 1 implies.
    {
        const AntiAliasingEnvOverrides envAA = AntiAliasingEnvOverrides::Read();
        if (envAA.MsaaSamples)
        {
            // Through the setter, which rejects anything that is not a sample
            // count and clamps to what the device can create targets with.
            SetDefaultMSAASampleCount(*envAA.MsaaSamples);
            if (m_AA.MsaaSamples > 1u)
                m_AA.Mode = AntiAliasingMode::MSAA;
        }
        if (envAA.Mode)
            m_AA.Mode = *envAA.Mode;
        if (envAA.TaaSequenceLength)
            m_AA.TaaSequenceLength = *envAA.TaaSequenceLength;
    }

    // Register engine-level culling shader loader so GPUScene can lazily
    // create its internal GPU culling compute pipeline on first use
    // without knowing about AssetManager paths or doing direct file I/O.
    Rendering::GPUScene::SetCullingShaderLoader(&LoadCullingShaderFromAssets);

    // Phase 6-ii: visibility-union compute shader loader. Same package-based
    // pattern as the other compute shader loaders above.
    Rendering::GPUCullingPipeline::SetVisibilityUnionShaderLoader(&LoadCullingShaderFromAssets);

    // Phase 6-iii: runtime visibility aggregate shader loader. Same loader.
    Rendering::GPUCullingPipeline::SetRuntimeVisibilityShaderLoader(&LoadCullingShaderFromAssets);

    // R2.1 P2: two-phase HZB occlusion cull shader loader. Same loader.
    Rendering::GPUCullingPipeline::SetHzbCullingShaderLoader(&LoadCullingShaderFromAssets);

    // Device-aware defaults.
    // The criterion is the viewport transform, not the API: a backend whose
    // Y-up path inverts screen-space winding needs Clockwise. Vulkan does it
    // with a negative viewport height; WebGPU's clip space is Y-up against a
    // top-left framebuffer, which is the same inversion with no flag to set.
    const Rendering::GraphicsAPI api = dev->GetAPI();
    if (api == Rendering::GraphicsAPI::Vulkan || api == Rendering::GraphicsAPI::WebGPU)
    {
        m_FrontFace = Rendering::FrontFace::Clockwise;
    }
    else
    {
        m_FrontFace = Rendering::FrontFace::CounterClockwise;
    }

    // Reverse-Z requires float depth — D24_UNORM precision is uniformly distributed
    // and discards the precision win that float32 + reverse-Z gives near the camera.
    // Pin to D32_FLOAT regardless of device "preferred" format.
    m_DepthFormat = Rendering::TextureFormat::D32_FLOAT;

    // Defaults; callers can reconfigure
    m_GpuScene->Initialize();

    // The eager MaterialCompiler creation moved into MaterialSystem::Initialize (A1.3).

    // Create a default GPU culling pipeline used by ScheduleViewCullingDispatches.
    // If creation fails we log once and per-view culling becomes a no-op (the
    // bucketer falls back to running every instance through the indirect path,
    // which still renders — just without frustum-rejected work being skipped).
    // Only on profiles that dispatch it. The compatibility tier draws through
    // CpuDrawStreamBuilder and never consumes GPU culling output, so creating
    // the pipeline there only manufactures per-frame failures on backends
    // whose shader set cannot express it (WebGPU).
    if (m_Profile.UseGpuDrivenDraws)
    {
        m_GpuCullingPipeline = Rendering::GPUCullingFactory::CreateBalanced(dev);
        if (!m_GpuCullingPipeline)
        {
            Logger::Log::Warning("RenderServices::Initialize: failed to create GPUCullingPipeline; GPU-driven culling will be disabled.");
        }
    }
    else
    {
        Logger::Log::Info("RenderServices: compatibility profile — GPU culling pipeline not created (CPU draw path culls)");
    }

    // Default identity-bones UBO + unwired-binding placeholder buffer.
    CreateDeviceDefaultBuffers();

    // Shadow fallback textures (bound when a view has no casters of a type).
    CreateShadowFallbackResources();
    // Created up front, not on first use: MaterialBinder binds it with the depth slot defaults
    // from parallel record workers, which must not race its lazy creation.
    GetCascadeShadowSampler();

    // Initialize the data-driven render pipeline system (node registry + compiler,
    // engine node-type registrations, default-path seed, env override). Consolidated
    // onto the frame orchestrator (A1.4); built-in modules register their node types
    // into Spine().GetPipelineNodeRegistry() from EngineCore::EnableRenderingLoop,
    // and anything registering later goes through Spine().RegisterPipelineNodeType.
    m_FrameOrchestrator.Initialize(*this);

    InstallAssetReloadInvalidators();

    // Q6 device-lost re-provision (design §8): recreate the RenderServices-owned
    // GPU resources after an in-place device rebuild. Fires on the render thread
    // inside RebuildDevice while the device is AwaitingReprovision (usable). The
    // registration is idempotent per id and persists across rebuilds; the callback
    // guards on m_Device so a stray fire after Shutdown is a no-op.
    m_Device->RegisterDeviceRebuiltCallback(
        "RenderServicesReprovision", [this](Rendering::IDevice*) { OnDeviceRebuilt(); });

    Logger::Log::Info("[Startup] RenderServices::Initialize TOTAL: {:.1f}ms", MsSince(tRSStart));
    return true;
}

// Creates (or, after a device-lost rebuild, recreates) the minimal Forward+
// fallback buffers so composed/material shaders that declare clustered resources
// never skip binding set0 when the active pipeline doesn't provide them. These
// encode "0 lights" and safe cluster params — functional, not correct lighting.
void RenderServices::CreateFallbackBuffers()
{
    Rendering::IDevice* const dev = m_Device;
    if (dev)
    {
        auto createUpload = [&](Rendering::BufferHandle& out,
                                Rendering::BufferUsage usage,
                                size_t bytes,
                                const char* debugName,
                                const void* initData)
        {
            Rendering::BufferDesc bd{};
            bd.size = bytes;
            bd.usage = static_cast<uint32_t>(usage);
            bd.memoryUsage = Rendering::BufferMemoryUsage::Upload;
            bd.flags = Rendering::BufferCreateFlags::PersistentlyMapped;
            bd.debugName = debugName;
            out = dev->CreateBuffer(bd);
            if (out.IsValid() && initData && bytes)
            {
                void* ptr = dev->MapBuffer(out);
                if (ptr)
                {
                    std::memcpy(ptr, initData, bytes);
                    dev->UnmapBuffer(out);
                }
            }
        };

        // ClusterBuffer header layout is uvec4 (clustersX, clustersY, depthSlices, maxLightsPerCluster).
        // Use 1,1,1,1 so shaders clamp to valid non-zero values.
        struct UVec4
        {
            uint32_t x, y, z, w;
        };
        const UVec4 clusterHeader{1u, 1u, 1u, 1u};
        const uint32_t lightIndex0 = 0u;
        const UVec4 lightHeader{0u, 0u, 0u, 0u}; // LightBuffer.header.x = lightCount = 0
        const UVec4 clusterParams{16u, 16u, 24u, 64u};

        // ViewParams identity fallback (bound when a view publishes no buffer).
        // Layout is the shared Rendering::ViewParamsUBO (ViewParamsLayout.h).
        // Composed forward adapter light block: shared ForwardLightUBO mirror
        // (RenderServices.h) so the zero fallback grows with the real writer.
        Rendering::ViewParamsUBO vp{};
        ForwardLightUBO lightUbo{};
        std::memcpy(vp.ge_invProj, kIdentity4x4.Data(), 64);
        std::memcpy(vp.ge_view, kIdentity4x4.Data(), 64);
        std::memcpy(vp.ge_viewProj, kIdentity4x4.Data(), 64);
        std::memcpy(vp.ge_proj, kIdentity4x4.Data(), 64);
        std::memcpy(lightUbo.uLightVP, kIdentity4x4.Data(), 64);
        vp.ge_nearFar[0] = 0.1f;
        vp.ge_nearFar[1] = 1000.0f;
        vp.ge_nearFar[2] = 0.0f;
        vp.ge_nearFar[3] = 0.0f;
        vp.ge_cameraPosWS[0] = 0.0f;
        vp.ge_cameraPosWS[1] = 0.0f;
        vp.ge_cameraPosWS[2] = 0.0f;
        vp.ge_cameraPosWS[3] = 0.0f;
        vp.ge_screenSize[0] = 1.0f;
        vp.ge_screenSize[1] = 1.0f;
        vp.ge_screenSize[2] = 1.0f;
        vp.ge_screenSize[3] = 1.0f;
        vp.ge_exposureParams[0] = 1.0f; // neutral exposure, no metering
        // No default light — scenes should submit their own lights via SubmitLight().

        auto registerFallback = [&](const char* name, Rendering::BufferUsage usage,
                                    size_t bytes, const char* debugName, const void* initData)
        {
            Rendering::BufferHandle buf{};
            createUpload(buf, usage, bytes, debugName, initData);
            if (buf.IsValid())
                m_FallbackBuffers[HashStringId(name)] = buf;
        };
        registerFallback("ClusterBuffer", Rendering::BufferUsage::Storage,
                         sizeof(clusterHeader), "RenderServices.Fallback.ClusterBuffer", &clusterHeader);
        registerFallback("LightIndexBuffer", Rendering::BufferUsage::Storage,
                         sizeof(lightIndex0), "RenderServices.Fallback.LightIndexBuffer", &lightIndex0);
        registerFallback("LightBuffer", Rendering::BufferUsage::Storage,
                         sizeof(lightHeader), "RenderServices.Fallback.LightBuffer", &lightHeader);
        registerFallback("ClusterParams", Rendering::BufferUsage::Uniform,
                         sizeof(clusterParams), "RenderServices.Fallback.ClusterParams", &clusterParams);
        registerFallback("ViewParams", Rendering::BufferUsage::Uniform,
                         sizeof(Rendering::ViewParamsUBO), "RenderServices.Fallback.ViewParams", &vp);
        registerFallback("LightUBO", Rendering::BufferUsage::Uniform,
                         sizeof(ForwardLightUBO), "RenderServices.Fallback.ComposedLightUBO", &lightUbo);
        m_FallbackBuffers[HashStringId("Light")] = m_FallbackBuffers[HashStringId("LightUBO")];
        // CompatInstanceList: a view with nothing to draw still binds the name
        // its compat entity variants reflect. One entry, scene instance 0; no
        // draw reads it, because a view without a list issues no entity draw.
        if (m_Profile.IsCompat())
        {
            const uint32_t noInstance = 0u;
            registerFallback(CpuDrawStreamBuilder::kIndexListBindingName,
                             Rendering::BufferUsage::Storage, sizeof(noInstance),
                             "RenderServices.Fallback.CompatInstanceList", &noInstance);
        }
        // ExposureHistory: nothing metered, so GE_ViewExposureScale falls back to the view's
        // static scale where the render graph has no metering or has not materialized it.
        const ExposureReadbackFeature::State unmetered{};
        registerFallback("ExposureHistory", Rendering::BufferUsage::Storage, sizeof(unmetered),
                         "RenderServices.Fallback.ExposureHistory", &unmetered);

        // ShadowData fallback: zero-filled means numCascades=0, so GE_SampleShadow returns 1.0 (no shadow).
        {
            ShadowDataGPU shadowFallback{};
            registerFallback("ShadowData", Rendering::BufferUsage::Uniform,
                             sizeof(ShadowDataGPU), "RenderServices.Fallback.ShadowData", &shadowFallback);
        }

        // AreaShadowData fallback: enabled=0, so GE_SampleAreaShadow returns 1.0.
        {
            AreaShadowDataGPU areaShadowFallback{};
            registerFallback("AreaShadowData", Rendering::BufferUsage::Uniform,
                             sizeof(AreaShadowDataGPU), "RenderServices.Fallback.AreaShadowData", &areaShadowFallback);
        }

        // SpotShadowData fallback: enabled=0, so GE_SampleSpotShadow returns 1.0.
        {
            SpotShadowDataGPU spotShadowFallback{};
            registerFallback("SpotShadowData", Rendering::BufferUsage::Uniform,
                             sizeof(SpotShadowDataGPU), "RenderServices.Fallback.SpotShadowData", &spotShadowFallback);
        }

        // PointShadowData fallback (M1): a single disabled slot as a STORAGE buffer
        // (binding 25 is now a std430 slot array). Bound when a view publishes no
        // atlas; every light's shadowSlot is -1 then, so GE_SamplePointShadow
        // returns 1.0 before it ever indexes ge_pointShadowSlots.
        {
            PointShadowSlotGPU pointShadowFallback{}; // params[0] (enabled) = 0
            registerFallback("PointShadowData", Rendering::BufferUsage::Storage,
                             sizeof(PointShadowSlotGPU), "RenderServices.Fallback.PointShadowData", &pointShadowFallback);
        }

        // MotionParams fallback: the motion variant's per-view block, zeroed. A
        // view whose producer arm did not run this frame must bind zeros
        // rather than another view's previous view-projection — a stale matrix
        // would be a wrong motion vector, which is worse than none. Nothing
        // draws with it: a producer that cannot fill the block records no
        // deforming draw at all.
        {
            DeformationMotionParamsGPU motionFallback{};
            registerFallback(kDeformationMotionParamsName, Rendering::BufferUsage::Uniform,
                             sizeof(motionFallback), "RenderServices.Fallback.MotionParams",
                             &motionFallback);
        }

        // EnvData fallback (full std140 block, sized from the feature's constant so
        // every field the shader reads — including the ambient-floor tail — is backed
        // by the buffer): zero-filled => iblIntensity=0 (no env radiance) and
        // ambientFloorParams mode 0 (floor off), so an IBL variant bound before the
        // feature initializes adds no ambient. The reflected UBO name is the instance
        // "Env"; keep "EnvDataUBO" as an alias.
        {
            const uint8_t envFallback[ImageBasedLightingFeature::kEnvDataBytes] = {};
            registerFallback("Env", Rendering::BufferUsage::Uniform,
                             sizeof(envFallback), "RenderServices.Fallback.EnvData", envFallback);
            m_FallbackBuffers[HashStringId("EnvDataUBO")] = m_FallbackBuffers[HashStringId("Env")];
        }

        // MaterialParamsSSBO fallback: persistent zero-filled buffer so binding 13
        // is always valid, even on frames before PackMaterialSSBO first runs.
        {
            uint8_t zeros[kMaterialEntryStride]{};
            Rendering::BufferHandle buf{};
            createUpload(buf, Rendering::BufferUsage::Storage, kMaterialEntryStride,
                         "RenderServices.Fallback.MaterialParamsSSBO", zeros);
            if (buf.IsValid())
            {
                // RS owns the physical fallback buffer (m_FallbackBuffers, torn
                // down in Shutdown); the facade holds the handle for binding 13
                // and its own state reset (§0a-A5).
                m_MaterialSystem.SeedMaterialParamsFallback(buf, kMaterialEntryStride);
                m_FallbackBuffers[HashStringId("MaterialParams")] = buf;
            }
        }

    }
}

// Creates (or, after a device-lost rebuild, recreates) the two device-owned
// default buffers: the identity-bones UBO (bind-pose palette for static/safety
// skinned binds) and the zero-filled placeholder bound to any unwired set0
// buffer descriptor (design §8 "device default buffers", RS.h:1131-1135).
void RenderServices::CreateDeviceDefaultBuffers()
{
    Rendering::IDevice* const dev = m_Device;
    if (!dev)
        return;

    // Zero-filled placeholder bound to any reflected set0 buffer descriptor a
    // render-graph node leaves unwired (see GetDefaultPlaceholderBuffer). Both
    // Uniform and Storage usage so it satisfies UBO and SSBO bindings; sized to
    // cover small ranges (large SSBOs are always explicitly provided).
    {
        constexpr size_t kDefaultPlaceholderBufferBytes = 4096;
        BufferDesc d{};
        d.size = kDefaultPlaceholderBufferBytes;
        d.usage = static_cast<uint32_t>(BufferUsage::Uniform | BufferUsage::Storage);
        d.memoryUsage = BufferMemoryUsage::Upload;
        d.flags = BufferCreateFlags::PersistentlyMapped;
        d.debugName = "DefaultPlaceholderBuffer_4K";
        m_DefaultPlaceholderBuffer = dev->CreateBuffer(d);
        if (m_DefaultPlaceholderBuffer.IsValid())
        {
            // Zero the contents so an unwired binding reads predictable zeros
            // rather than uninitialized memory.
            if (void* ptr = dev->MapBuffer(m_DefaultPlaceholderBuffer))
            {
                std::memset(ptr, 0, kDefaultPlaceholderBufferBytes);
                dev->UnmapBuffer(m_DefaultPlaceholderBuffer);
            }
        }
    }
}

// Fills every layer of a depth array with the reverse-Z FAR value, 0.0.
//
// CONVENTION TRAP: this engine is reverse-Z and its shadow comparison sampler is
// GreaterOrEqual, so a STORED depth of 0.0 makes every comparison pass — the
// texel reads as "nothing occludes here", i.e. fully lit. That is the correct
// no-op for a shadow map that does not exist. (1.0 would shadow everything.) The
// same rule sets the shadow sampler's border to 0 in Device.cpp with the note
// "reverse-Z far depth -> OOB considered lit".
//
// The fill is a clear through a depth-only render pass per layer, the route
// every backend supports without a staging buffer, and it runs once per device
// (creation + rebuild) on a 1x1 image.
//
// Assumes dynamic rendering (DeviceDesc::enableDynamicRendering, default true).
// Without it each BeginRenderPass caches a framebuffer + render pass that
// ~VulkanCommandList destroys as soon as `cl` leaves scope below — immediately
// after submit, while the GPU may still be executing them. Unreachable today;
// a backend that turns dynamic rendering off must fence before that destructor.
//
// No path here may fail silently. The texture is created Undefined, so
// CreateTexture skips its initial-state transition; if this clear does not run,
// the image both keeps undefined contents AND never leaves
// VK_IMAGE_LAYOUT_UNDEFINED, while its descriptor claims
// DEPTH_STENCIL_READ_ONLY_OPTIMAL. A provisioning failure therefore logs an
// error and asserts; a latched device loss logs a warning instead, because the
// post-rebuild reprovision re-runs this. `label` names the fallback in those
// diagnostics — every depth fallback shares this path, so an unlabelled message
// cannot say which one is undefined.
static void ClearDepthArrayToFar(Rendering::IDevice* dev, Rendering::TextureHandle tex,
                                 uint32_t layerCount, const char* label)
{
    if (!dev || !tex.IsValid() || layerCount == 0)
    {
        Logger::Log::Error(
            "[Shadows] {} clear skipped: device={} texture={} layerCount={}. The "
            "image keeps undefined contents and is never transitioned out of UNDEFINED",
            label, dev ? "ok" : "NULL", tex.IsValid() ? "ok" : "INVALID", layerCount);
        assert(false && "a depth shadow fallback could not be cleared");
        return;
    }

    auto cl = dev->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
    if (!cl)
    {
        Logger::Log::Error(
            "[Shadows] {} clear skipped: no graphics command list. The image keeps "
            "undefined contents and is never transitioned out of UNDEFINED",
            label);
        assert(false && "a depth shadow fallback could not be cleared");
        return;
    }
    cl->Begin();
    cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
        tex, Rendering::ResourceState::Undefined, Rendering::ResourceState::DepthWrite,
        /*baseMip*/ 0, /*levelCount*/ 1, /*baseLayer*/ 0, layerCount));

    for (uint32_t layer = 0; layer < layerCount; ++layer)
    {
        Rendering::RenderPassDesc rp{};
        rp.colorTargetCount = 0;
        rp.clearColor[0] = false;
        rp.colorLoadOp[0] = Rendering::RenderPassDesc::LoadOp::DontCare;
        rp.colorStoreOp[0] = Rendering::RenderPassDesc::StoreOp::DontCare;
        rp.depthTarget = tex;
        rp.clearDepth = true;
        rp.clearDepthValue = 0.0f; // reverse-Z far == lit
        rp.depthLoadOp = Rendering::RenderPassDesc::LoadOp::Clear;
        rp.depthStoreOp = Rendering::RenderPassDesc::StoreOp::Store;
        // Dynamic rendering records one layer per pass, so each layer gets its
        // own single-layer attachment view.
        rp.useDepthView = true;
        rp.depthViewDesc.aspect = Rendering::TextureAspect::Depth;
        rp.depthViewDesc.viewType = Rendering::TextureViewType::View2D;
        rp.depthViewDesc.baseLayer = layer;
        rp.depthViewDesc.layerCount = 1;
        rp.depthViewDesc.levelCount = 1;
        cl->BeginRenderPass(rp);
        cl->EndRenderPass();
    }

    cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
        tex, Rendering::ResourceState::DepthWrite, Rendering::ResourceState::DepthSampled,
        /*baseMip*/ 0, /*levelCount*/ 1, /*baseLayer*/ 0, layerCount));
    cl->End();
    Rendering::CommandList* raw = cl.get();
    dev->ExecuteCommandLists({raw});

    // ExecuteCommandLists returns without submitting when a device loss is
    // already latched (VulkanDevice::ExecuteCommandLists), which would leave the
    // image in UNDEFINED while the descriptor claims DEPTH_STENCIL_READ_ONLY.
    // That case is a transient the recovery path re-runs — CreateShadowFallbackResources
    // runs again on rebuild — so it warns rather than asserting.
    const Rendering::DeviceHealth health = dev->GetDeviceHealth();
    if (health != Rendering::DeviceHealth::Healthy)
    {
        Logger::Log::Warning(
            "[Shadows] {} clear may not have been submitted (device {}); the "
            "post-rebuild reprovision re-runs it",
            label, Rendering::DeviceHealthToString(health));
    }
}

// Creates (or, after a device-lost rebuild, recreates) the 1x1 shadow fallback
// textures bound when a view has no casters of a given type (design §8 "device
// default buffers"). The area/spot/point comparison samplers are created lazily
// by Get*ShadowSampler and are reset separately by OnDeviceRebuilt.
void RenderServices::CreateShadowFallbackResources()
{
    Rendering::IDevice* const dev = m_Device;
    if (!dev)
        return;

    // Shadow fallback textures (bound when a view has no casters of a type).
    {
        // Base desc for every DEPTH fallback below (area/spot, point, cascade).
        // Its texels must be DEFINED, not merely allocated: each one is sampled
        // through a GreaterOrEqual comparison sampler, and two are also read RAW
        // (ge_areaShadowMapRaw's PCSS blocker search, and ge_sceneDepth). An
        // allocated-but-unwritten depth image is exactly the read these
        // fallbacks exist to replace, so all three are created Undefined and
        // filled by ClearDepthArrayToFar. TransferSrc keeps that fill
        // observable: a value no test can read is a value nothing holds to.
        Rendering::TextureDesc areaShadowFallbackDesc{};
        areaShadowFallbackDesc.width = 1;
        areaShadowFallbackDesc.height = 1;
        areaShadowFallbackDesc.mipLevels = 1;
        areaShadowFallbackDesc.arrayLayers = 1;
        areaShadowFallbackDesc.format = static_cast<uint32_t>(Rendering::TextureFormat::D32_FLOAT);
        areaShadowFallbackDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil)
                                     | static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource)
                                     | static_cast<uint32_t>(Rendering::TextureUsage::TransferSrc);
        areaShadowFallbackDesc.initialState = Rendering::ResourceState::Undefined;
        areaShadowFallbackDesc.persistent = true;
        areaShadowFallbackDesc.debugName = "AreaShadowFallback1x1";
        m_AreaShadowFallbackTexture = dev->CreateTexture(areaShadowFallbackDesc);
        // Bound for ge_areaShadowMap AND ge_spotShadowMap (both comparison), for
        // ge_areaShadowMapRaw, and as the ge_sceneDepth fallback. 0.0 is the
        // no-op on all four: GreaterOrEqual with a caller-clamped Dref in [0,1]
        // passes against a stored 0.0 (fully lit); the raw blocker search tests
        // `stored > refDepth`, false for every refDepth in [0,1], so it finds no
        // blocker and returns lit; and for ge_sceneDepth 0.0 is reverse-Z FAR,
        // i.e. "the opaque scene is infinitely distant", which is what every
        // consumer of that binding treats as "nothing in front of me".
        ClearDepthArrayToFar(dev, m_AreaShadowFallbackTexture, 1, "area/spot shadow fallback");

        // White 2D-ARRAY fallback for ge_transmittanceShadowArray (sampler2DArray), bound
        // when a view has no glass casters. ForceArrayView makes the 1-layer texture a
        // valid array source; the cascade layer index clamps to layer 0.
        Rendering::TextureDesc tintFallbackDesc{};
        tintFallbackDesc.width = 1;
        tintFallbackDesc.height = 1;
        tintFallbackDesc.mipLevels = 1;
        tintFallbackDesc.arrayLayers = 1;
        tintFallbackDesc.format = static_cast<uint32_t>(Rendering::TextureFormat::RGBA8_UNORM);
        tintFallbackDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource)
                               | static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
        tintFallbackDesc.flags = Rendering::TextureCreateFlags::ForceArrayView;
        tintFallbackDesc.persistent = true;
        tintFallbackDesc.debugName = "GlassShadowTintFallback1x1";
        m_TransmittanceShadowFallback = dev->CreateTexture(tintFallbackDesc);

        // ge_pointShadowMap (sampler2DArrayShadow). Receivers index
        // baseLayer + face, so every one of the six face layers is reachable and
        // each must carry the far value. ForceArrayView makes it a valid array
        // source at any layer count.
        Rendering::TextureDesc pointShadowFallbackDesc = areaShadowFallbackDesc;
        pointShadowFallbackDesc.arrayLayers = kPointShadowFaceCount;
        pointShadowFallbackDesc.flags = Rendering::TextureCreateFlags::ForceArrayView;
        pointShadowFallbackDesc.debugName = "PointShadowFallback1x1Array";
        m_PointShadowFallbackTexture = dev->CreateTexture(pointShadowFallbackDesc);
        ClearDepthArrayToFar(dev, m_PointShadowFallbackTexture, kPointShadowFaceCount,
                             "point shadow fallback");

        // Directional cascade array (ge_shadowMapArray), bound when a view has no
        // live cascade array for the frame being recorded — sampled by EVERY
        // shadowed material. No ForceArrayView — arrayLayers > 1 already selects
        // a 2D_ARRAY view.
        Rendering::TextureDesc cascadeFallbackDesc = areaShadowFallbackDesc;
        cascadeFallbackDesc.arrayLayers = kMaxShadowCascades;
        cascadeFallbackDesc.debugName = "CascadeShadowFallback1x1Array";
        m_CascadeShadowFallbackTexture = dev->CreateTexture(cascadeFallbackDesc);
        ClearDepthArrayToFar(dev, m_CascadeShadowFallbackTexture, kMaxShadowCascades,
                             "cascade shadow fallback");

        // RGB white (no tint), A=0 (no glass) — the tint fallback's single texel.
        uint32_t whiteNoPresence = 0x00FFFFFFu;
        Rendering::TextureUploadEntry fallbackUploads[] = {
            {m_TransmittanceShadowFallback, &whiteNoPresence, 1, 1, 1, 4, 0},
        };
        Rendering::UploadTexturesBatched(dev, fallbackUploads, 1, "ShadowFallbackStaging");
    }
}

void RenderServices::InitializePerFrameWritePool()
{
    // Idempotent: Shutdown() is a safe no-op on an uninitialized pool, so this
    // both initializes (from Initialize) and re-initializes over dead buffers
    // (from OnDeviceRebuilt) with the same config.
    m_PerFrameWritePool.Shutdown();

    PerFrameWritePoolConfig poolCfg{};
    poolCfg.FramesInFlight = std::max(1u, m_Device->GetFramesInFlight());

    // All SSBO usages must respect the device's minimum storage buffer offset alignment.
    const auto& caps = m_Device->GetCapabilities();
    const size_t ssboAlign = std::max<size_t>(16, caps.minStorageBufferOffsetAlignment);
    for (auto& usage : poolCfg.usages)
    {
        if (usage.bufferUsage == Rendering::BufferUsage::Storage)
            usage.alignment = std::max(usage.alignment, ssboAlign);
    }

    // Growth ceilings are budget numbers, not device promises: a ring that
    // grows past what one storage-buffer descriptor may cover produces an
    // unbindable buffer. Clamp every growable usage to the granted limit and
    // say so once — for BonePalette the clamp is a crowd-size cap.
    if (caps.maxStorageBufferBindingSize != 0)
    {
        for (size_t i = 0; i < poolCfg.usages.size(); ++i)
        {
            auto& usage = poolCfg.usages[i];
            if (usage.maxCapacityBytes <= caps.maxStorageBufferBindingSize)
                continue;
            Logger::Log::Info(
                "PerFrameWritePool: usage {} growth ceiling clamped {} MB -> {} MB "
                "(device maxStorageBufferBindingSize)",
                i, usage.maxCapacityBytes >> 20, caps.maxStorageBufferBindingSize >> 20);
            usage.maxCapacityBytes = caps.maxStorageBufferBindingSize;
        }
    }

    m_PerFrameWritePool.Initialize(m_Device, poolCfg);
}

void RenderServices::OnDeviceRebuilt()
{
    // Fires on the render thread inside VulkanDevice::RebuildDevice, once the
    // device is functional and AwaitingReprovision (usable for GPU ops; rendering
    // stays suppressed). Guard a stray fire after Shutdown — the device-rebuilt
    // callback persists on the device, which can outlive this RenderServices in
    // teardown ordering.
    if (!m_Device)
        return;

    const auto tReprovStart = std::chrono::steady_clock::now();
    Logger::Log::Warning(
        "RenderServices::OnDeviceRebuilt: re-provisioning RenderServices-owned GPU resources (Q6 slice 3a+4)");

    // 1. Drop the stale concrete VkPipeline map. The VkPipelines were destroyed by
    //    the rebuild teardown, so this only forgets dead handles (no DestroyPipeline);
    //    interned descs + SPIR-V survive so PSOs recompile warm on next use.
    m_Device->GetMutablePipelineCache().ClearConcreteOnly();
    //    The variant cache's concrete-warm tracking mirrors the map just cleared:
    //    a (pipeline, format) build that failed because the device was dying must
    //    not stay suppressed on the healthy device, and in-flight markers for
    //    builds the teardown invalidated must not block re-requests.
    m_MaterialSystem.Variants().OnDeviceRebuilt();

    // 2. Device-owned default / fallback / placeholder resources. The old handles
    //    are dead (teardown freed every VkBuffer/VkImage); the Create* helpers
    //    overwrite the members with fresh handles. They skip Destroy on the old ones
    //    not to dodge a double-free (a stale Destroy* is a generational no-op) but
    //    because the physical is already gone and some of these (default buffers) are
    //    mapped — the point is that the fresh handle replaces the dangling one.
    CreateDeviceDefaultBuffers();
    CreateFallbackBuffers();
    CreateShadowFallbackResources();
    // Lazily-created shadow comparison samplers are dead; drop the handles so
    // Get*ShadowSampler recreates them on next use.
    m_AreaShadowSampler = {};
    m_AreaShadowRawSampler = {};
    m_SpotShadowSampler = {};
    m_PointShadowSampler = {};
    m_CascadeShadowSampler = {};
    GetCascadeShadowSampler(); // eager for the binder's depth slot defaults, as in Initialize

    // 3. TextureService / bindless (design §8 bindless row / F6): recreate default
    //    textures + bindless set FIRST, CLEAR the id-keyed bindless cache (a stale
    //    entry would COLLIDE with a reissued handle id), re-bake every material's
    //    default bindless indices, then replay the retained ref graph re-uploading
    //    real textures from the RAM-resident AssetManager.
    if (m_Textures)
        m_Textures->ReprovisionAfterDeviceRebuild();

    // 3b. Per-view GPU buffers: forget the handles so the lazy create can fire
    //     again. WriteViewLightBuffer is guarded on `!buf.IsValid()`, and a handle
    //     stays non-null once its device is gone, so without this the light UBO is
    //     never recreated: every material-set write drops ("buffer handle did not
    //     resolve") and lit surfaces render black for the life of the process.
    //     Forget rather than destroy — the buffers died with the old device, and
    //     DestroyBuffer here would hand its handles to the live one.
    m_ViewRegistry.ForgetPerViewGpuResourcesAfterDeviceRebuild();

    // 4. MeshGPURegistry (Q6 slice 4): the teardown freed every bucket-pool
    //    VkBuffer, but the registry TABLE survives, so every component-resident
    //    MeshGPUHandle and its GPUScene mesh-row index must stay valid. Recreate the
    //    pools and re-upload each live entry into the SAME slot — asset-backed
    //    geometry from the AssetManager-resident ModelAsset (M1, cache-first: no disk
    //    load), built-in primitives regenerated from their deterministic GUIDs,
    //    procedural-with-picking from the retained CPU mirror. Sourceless procedural
    //    entries (EZTree/HLOD/morph runtime meshes) are tombstoned here and
    //    re-registered by their owners' generation-polled recovery once Healthy.
    //    Runs BEFORE GPUScene so the F9 row fixups land in the CPU mirror that the
    //    next step re-flushes wholesale, and the registry pools are LIVE before any
    //    consumer can RegisterSubmesh/upload after resume.
    {
        // Owns the regenerated built-ins for the duration of the pass: it hands the
        // registry pointers into its own storage and the registry reads them
        // synchronously.
        MeshReprovisionSource sourceLookup;

        const Rendering::MeshGPUReprovisionReport meshReport =
            m_MeshGPURegistry.ReprovisionAfterDeviceRebuild(std::ref(sourceLookup));
        Logger::Log::Warning(
            "RenderServices::OnDeviceRebuilt: MeshGPURegistry re-provisioned (Q6 slice 4) — entries={}, "
            "restored(asset={}, generated={}, cpuMesh={}), tombstoned={}, GPUScene rows preserved={}",
            meshReport.EntriesTotal, meshReport.RestoredFromAsset, meshReport.RestoredFromGenerated,
            meshReport.RestoredFromCpuMesh, meshReport.Tombstoned, meshReport.GpuRowsPreserved);
    }

    // 5. GPUScene: recreate GPU buffers, mark the retained CPU mirror all-dirty,
    //    re-flush. The mesh rows step 4 UpdateMesh'd into the mirror ride along in
    //    the wholesale re-flush; instance meshIndex/materialIndex stay valid because
    //    step 4 preserved every mesh-row index in place (F9).
    if (m_GpuScene)
        m_GpuScene->ReprovisionAfterDeviceRebuild();

    // 6. PerFrameWritePool rings: teardown + re-init (refilled from ECS extraction
    //    every frame; SkinPaletteAtlas rides on it and needs no separate step).
    InitializePerFrameWritePool();

    // 7. GPU culling / HZB: reset the one-shot pipeline guards, drop the dead
    //    GPU-only buffers, and clear the per-view occlusion history so it
    //    re-imports all-visible (design §8 HZB row + §4 guard sweep).
    if (m_GpuCullingPipeline)
        m_GpuCullingPipeline->ReprovisionAfterDeviceRebuild();

    // 8. GPUDrawStreamBuilder: the draw scatter's arena/ring/sentinel buffers and
    //    scatter pipeline are dead but their handles still read IsValid(), so the
    //    lazy EnsureSharedBuffers would map/dispatch dead buffers on the FIRST
    //    resumed frame and crash. Drop the handles (no DestroyBuffer) so they
    //    rebuild lazily. This is the system slice 3a deferred to "restored when the
    //    chain resumes" — slice 4 is that resume, so it must land here.
    if (m_DrawStreamBuilder)
        m_DrawStreamBuilder->ReprovisionAfterDeviceRebuild();

    // 8b. Sorted transparent drain state: the CPU-drain ring buffers are dead but
    //     their handles still read IsValid(), so the capacity gate in
    //     AddSortedTransparentDrainForView would skip recreation and MapBuffer a
    //     dead handle — the CPU-path view's transparents would vanish silently
    //     and stickily (S2 review R1-F1). Zero handles + capacities (no
    //     DestroyBuffer — the rebuild teardown already freed the VkBuffers) so
    //     the next past-capacity drain recreates lazily. The drain compute
    //     pipeline's concrete VkPipeline was dropped with the cache in step 1;
    //     its interned desc survives, and the pool-imported args/counts/
    //     indirection buffers are pool-owned (reprovisioned with the pool).
    for (auto& [sortedViewId, sortedView] : m_SortedTransparentByView)
    {
        (void)sortedViewId;
        for (auto& buf : sortedView.CpuDrainBuffers)
            buf = {};
        for (auto& cap : sortedView.CpuDrainCapacityBytes)
            cap = 0;
    }

    // 9. GPU animation data store: persistent skeleton/clip SSBOs + the per-frame
    //    instance ring are dead. Zero the handles (avoiding CreateOrGrowBuffer's
    //    grow-path double-free) and re-flush from the surviving CPU mirrors. Only
    //    non-trivial on skinned scenes, but the reset must run so the first skinned
    //    Allocate after resume writes into a live ring, not a dangling mapped pointer.
    m_GPUAnimDataStore.ReprovisionAfterDeviceRebuild();

    // 9b. The scene acceleration-structure pool and the ray-traced shadow mask:
    //     the device kept its backend but destroyed every BLAS and TLAS with the
    //     old device, so both forget their handles and rebuild on the next claim.
    //     Before the features, whose TLAS channels the pool shares.
    if (m_SceneAS)
        m_SceneAS->OnDeviceRebuilt();
    if (m_RTShadowMask)
        m_RTShadowMask->OnDeviceRebuilt();
    // 9c. Passes RenderServices owns that cache a texture or sampler across frames.
    if (m_ScreenSpaceShadows)
        m_ScreenSpaceShadows->OnDeviceRebuilt();
    if (m_TransmissionSceneGrab)
        m_TransmissionSceneGrab->OnDeviceRebuilt();

    // 10. Render features (§8-completion): each feature that caches persistent GPU
    //     resources outside the per-frame render graph overrides OnDeviceRebuilt to
    //     drop its dead handles here so the first resumed frame binds live resources.
    //     Covered engine features: ShadowMapRenderFeature (samplers + per-view maps),
    //     ImageBasedLightingFeature (env cubes + BRDF LUT, re-armed for re-bake),
    //     SkyRenderFeature (atmosphere LUTs), RetargetRenderFeature (retarget SSBOs),
    //     LensFlareRenderFeature (mapped instance rings + atlas sampler),
    //     ExposureReadbackFeature (mapped readback rings). Features holding only
    //     RG-transient resources inherit the default no-op.
    //
    //     Module-owned features: CBTTerrain's CBTRenderFeature now overrides this — it
    //     forgets its dead kernel pipelines + instance buffers and re-arms its lazy
    //     bring-up. That was the one with a demonstrated crash: a stale CBT
    //     PipelineHandle made SetPipeline a silent no-op and the following Dispatch
    //     aborted on "no compute pipeline bound".
    //
    //     KNOWN DEVICE-RECOVERY GAP (follow-up, still NOT covered): the four remaining
    //     MODULE-owned render features — Ocean, Terrain, TerrainGrass, VolumetricFog —
    //     do not recover their rendering resources. They carry persistent GPU resources
    //     (heightfield/wave buffers, fog froxel volumes + samplers, grass blade/instance
    //     buffers) that a rebuild leaves dead; scenes using them will fault on the first
    //     resumed frame until each adds an override. Unlike CBT, none of the four is a
    //     pure handle-forget: each holds at least one resource with NO lazy re-create
    //     path, so each needs its own Initialize-re-run design (Ocean's ~14 sub-objects
    //     each latch their own m_Ready; Terrain's heightmap sampler + 3 one-shot bake
    //     pipelines; TerrainGrass's blade VB/IB + atlas-params UBOs; fog's 3 samplers).
    //     Tracked in the §8 inventory (q6 design doc).
    //     TerrainGrass DOES override this, but only to drop its mapped placement-stats
    //     readback ring; its rendering resources are still part of the gap above.
    ForEachFeature([&](IRenderFeature& feature) { feature.OnDeviceRebuilt(m_Device); });

    // 10b. Point-shadow atlas planners (RenderServices-owned, not feature-owned, so
    //      omitted from the feature sweep above): the persistent
    //      PointShadowAtlas.View{N} textures are dropped by the pool on rebuild
    //      (RGResourcePool::DropAllAfterDeviceRebuild), leaving every planner's
    //      per-slot render cache stale-against-a-fresh-physical. Reset the planners
    //      and clear the per-view assignment / liveness / log state so the first
    //      resumed frame re-renders every slot. (The resume full lane also bumps the
    //      caster epoch, which would force a re-render too — this makes the atlas
    //      recreation explicit rather than relying on that incidental coupling.)
    for (auto& [viewId, planner] : m_PointShadowPlanners)
    {
        (void)viewId;
        planner.Reset();
    }
    m_PointShadowAssignments.clear();
    m_PointShadowAtlasLiveness.clear();
    m_PointShadowCacheLogState.clear();

    // 11. Render-pipeline node instances (Q6 slice 4, §8 class 7): each live
    //     stream's RenderPipelineInstance owns node objects that lazily cache
    //     device handles outside the per-frame render graph (AONode's GTAO
    //     sampler, FullscreenShader/ComputeShader nodes' samplers). Those handles
    //     are dead on the rebuilt device; force a full node re-instantiation on
    //     the next Declare so the first resumed frame binds live resources.
    m_FrameOrchestrator.OnDeviceRebuilt();

    const double reprovMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tReprovStart).count();

    // Full re-provision chain complete (design §7 step 9): resume rendering. The
    // device transitions AwaitingReprovision -> Healthy; RenderingLoop stops
    // suppressing, and the first Healthy extraction takes the FULL lane (slice 3b
    // force-dirtied every render column + set the escalation flag) to rebuild all
    // records — the picture returns. NoteReprovisioned is CAS-guarded on
    // AwaitingReprovision: if a SECOND device loss was observed mid-re-provision it
    // already flipped health to Lost, so this is a no-op and the state machine
    // drives the bounded rebuild retry (M3). We never resume on a partial recovery.
    //
    // ECS-component-resident handles (EZTree/HLOD/morph runtime meshes) and the
    // change-gated extraction that draws them are restored by slice 3b's world-thread
    // recovery pass + the owners' generation-polled re-registration, which run on the
    // first Healthy tick this resume unblocks — not synchronously here.
    m_Device->NotifyReprovisionComplete();
    if (m_Device->GetDeviceHealth() == Rendering::DeviceHealth::Healthy)
        Logger::Log::Warning(
            "RenderServices::OnDeviceRebuilt: full re-provision complete in {:.1f}ms — rendering RESUMED "
            "(health=Healthy); first extraction rebuilds all records via slice-3b force-dirty.",
            reprovMs);
    else
        Logger::Log::Error(
            "RenderServices::OnDeviceRebuilt: re-provision ran {:.1f}ms but did NOT resume — a second loss "
            "intervened; staying suppressed, the device state machine drives the bounded retry (M3).",
            reprovMs);
}

void RenderServices::SetDefaultMSAASampleCount(uint32_t samples)
{
    // Only 1/2/4/8 name a real sample count; 1 disables MSAA. Anything else is
    // a caller bug rather than a request to resolve, so it says so and lands on
    // no multisampling instead of silently picking a count nobody asked for.
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8)
    {
        Logger::Log::Warning(
            "RenderServices::SetDefaultMSAASampleCount: {} is not a sample count (expected 1, 2, "
            "4 or 8) — disabling MSAA",
            samples);
        samples = 1u;
    }
    // Clamp to what the device can actually create render targets with
    // (Apple GPUs cap at 4; an 8x texture there fails creation outright).
    if (m_Device != nullptr)
    {
        const uint32_t deviceMax = std::max(1u, m_Device->GetCapabilities().maxMSAASamples);
        while (samples > deviceMax)
            samples /= 2;
    }
    if (samples == m_AA.MsaaSamples)
        return;

    // Just record the new count. The re-spec is implicit: the view controllers
    // re-declare their persistent color/depth every frame reading this value, and
    // the render-graph resource pool reallocs a target when its desc's sampleCount
    // changes, deferring the old physical until in-flight frames retire — so there
    // is nothing to drain or destroy here. Shadow textures are single-sample and
    // never re-spec on this change.
    m_AA.MsaaSamples = samples;
}

void RenderServices::SetDefaultRenderScale(float scale)
{
    if (!(scale > 0.0f)) // NaN / zero guard
        scale = 1.0f;
    scale = std::clamp(scale, kMinRenderScale, kMaxRenderScale);
    if (scale == m_DefaultRenderScale)
        return;
    m_DefaultRenderScale = scale;
    // Like the MSAA setter, the re-spec is implicit: the pipeline pre-pass
    // derives the internal extent from this value each frame and the pool
    // reallocs on desc change. The material texture mip bias is equally live —
    // ViewParamsUploadNode derives it per view from the actual extent ratio.
}

float RenderServices::ResolveViewRenderScale(Rendering::ViewId viewId) const
{
    if (const auto viewScale = m_ViewRegistry.GetViewRenderScale(viewId))
        return *viewScale;
    return m_DefaultRenderScale;
}

void RenderServices::SetDynamicResolutionMode(DynamicResolutionMode mode)
{
    if (mode == m_DrsMode)
    {
        // A same-mode call still enforces the Off contract (Native == scale
        // pinned to 1.0). At startup the persisted slider value is applied
        // BEFORE the mode resolves, and the default mode is already Off, so
        // this path is the only chance to pin — without it a project that
        // used Fixed once and then chose Native keeps silently upscaling at
        // the leftover persisted scale after a restart.
        if (mode == DynamicResolutionMode::Off)
            SetDefaultRenderScale(1.0f);
        return;
    }

    if (m_DrsMode == DynamicResolutionMode::Dynamic)
    {
        // Leaving Dynamic: hand the scale back to the user's static value
        // rather than stranding it wherever the controller last settled.
        SetDefaultRenderScale(m_DrsFixedScale);
    }
    else
    {
        m_DrsFixedScale = m_DefaultRenderScale;
    }

    m_DrsMode = mode;
    m_WarnedDrsUnreachable = false;
    m_WarnedDrsIneffective = false;

    if (mode == DynamicResolutionMode::Off)
        SetDefaultRenderScale(1.0f);
    else if (mode == DynamicResolutionMode::Dynamic)
        m_DrsController.Reset(m_DefaultRenderScale);

    m_DrsLastTickEpoch = 0xFFFFFFFFFFFFFFFFull;
}

const DynamicResolutionConfig& RenderServices::GetDynamicResolutionConfig() const
{
    return m_DrsController.Config();
}

void RenderServices::SetDynamicResolutionConfig(const DynamicResolutionConfig& config)
{
    m_DrsController.Configure(config);
    m_WarnedDrsUnreachable = false;
    m_WarnedDrsIneffective = false;
}

const DynamicResolutionController::Stats& RenderServices::GetDynamicResolutionStats() const
{
    return m_DrsController.GetStats();
}

void RenderServices::UpdateDynamicResolution(const DynamicResolutionSample& sample,
                                             float deltaSeconds, uint64_t appFrameEpoch)
{
    if (m_DrsMode != DynamicResolutionMode::Dynamic)
        return;
    // One tick per APP frame regardless of how many windows declare. The key
    // is the spine's world-frame epoch: per-window RGFrame counters advance
    // independently (windows open at different times, skip frames), so keying
    // on them let a second window tick the controller again the same frame —
    // dwell/slew/EMA ran at N× real time with N windows.
    if (appFrameEpoch == m_DrsLastTickEpoch)
        return;
    m_DrsLastTickEpoch = appFrameEpoch;

    const float scale = m_DrsController.Update(sample, deltaSeconds);
    SetDefaultRenderScale(scale);

    // Both notices latch for the episode and are re-armed only by a mode or
    // config change (the setters clear them). They must NOT re-arm on the
    // condition going false: these flags flicker frame to frame while the loop
    // is still moving, and re-arming on each dip turned "warn once" into a
    // warning every few frames -- observed as 7 identical lines in 2 seconds
    // during the 2026-07-24 gate-scene run.
    const auto& stats = m_DrsController.GetStats();
    if (stats.ScalingIneffective && !m_WarnedDrsIneffective)
    {
        m_WarnedDrsIneffective = true;
        Logger::Log::Info(
            "RenderServices: dynamic resolution measured the render-scale lever as ineffective for "
            "this content — driving {:.2f}..{:.2f} would save only {:.2f} ms against a {:.2f} ms "
            "target, so it is returning to full resolution. This frame is bound by "
            "resolution-independent work (geometry, shadows, UI), not fill.",
            m_DrsController.Config().MinScale, m_DrsController.Config().MaxScale,
            stats.EstimatedFullRangeSavingMs, m_DrsController.Config().TargetGpuMs);
    }


    if (stats.SaturatedLow && !m_WarnedDrsUnreachable)
    {
        m_WarnedDrsUnreachable = true;
        Logger::Log::Warning(
            "RenderServices: dynamic resolution is at its minimum scale ({:.2f}) and still over "
            "the {:.2f} ms target (measured {:.2f} ms) — the cost that does not scale with "
            "resolution already exceeds the target; raise the target or reduce fixed-resolution "
            "work",
            m_DrsController.Config().MinScale, m_DrsController.Config().TargetGpuMs,
            stats.SmoothedGpuMs);
    }
}

AntiAliasingDeviceCaps RenderServices::GetAntiAliasingDeviceCaps() const
{
    if (m_Device == nullptr)
        return {};
    const auto& caps = m_Device->GetCapabilities();
    return {caps.maxMSAASamples, caps.prefersNoDefaultMSAA};
}

ResolvedAntiAliasing
RenderServices::ResolveAntiAliasing(uint32_t cameraAAMode, uint32_t cameraMsaaSamples) const
{
    // Camera override encoding: 0 = inherit, otherwise AntiAliasingMode + 1.
    AntiAliasingMode mode = m_AA.Mode;
    if (cameraAAMode >= 1u && cameraAAMode <= 6u)
        mode = static_cast<AntiAliasingMode>(cameraAAMode - 1u);
    else if (cameraMsaaSamples == 2u || cameraMsaaSamples == 4u || cameraMsaaSamples == 8u)
        mode = AntiAliasingMode::MSAA; // legacy per-camera contract: explicit samples force MSAA

    ResolvedAntiAliasing resolved{};
    resolved.Mode = mode;
    if (mode == AntiAliasingMode::MSAA)
    {
        // Legacy per-camera sample semantics: 0 = engine default, 1 = off,
        // 2/4/8 = explicit; anything else falls back to single-sample.
        uint32_t samples = cameraMsaaSamples == 0u ? m_AA.MsaaSamples : cameraMsaaSamples;
        if (samples != 2u && samples != 4u && samples != 8u)
            samples = 1u;
        resolved.SampleCount = samples;
    }
    return resolved;
}

RetargetRenderFeature& RenderServices::GetRetargetRenderFeature()
{
    auto& feat = EnsureFeature<RetargetRenderFeature>();
    if (!feat.IsInitialized() && m_Device)
        feat.Initialize(m_Device);
    return feat;
}

void RenderServices::InstallAssetReloadInvalidators()
{
    auto& engine = GameEngine::EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;

    auto& dispatcher = engine.GetAssetManager().GetEventDispatcher();

    // The Texture/Model/CubeLut handlers only ENQUEUE. They can fire on the
    // file-watcher thread (AssetUnloaded/AssetDestroyed dispatch synchronously
    // on the raising thread), and the real work destroys GPU resources and
    // mutates live material bindings — render-thread territory.
    // DrainPendingAssetInvalidations runs the work at BeginWorldDrawFrame.
    const auto enqueue = [this](AssetType type, const GUID& guid)
    {
        std::lock_guard lock(m_PendingAssetInvalidationsMutex);
        m_PendingAssetInvalidations.push_back({type, guid});
    };

    m_TextureReloadInvalidator = AssetReloadInvalidator(
        dispatcher, AssetType::Texture,
        [enqueue](const GUID& guid) { enqueue(AssetType::Texture, guid); });

    m_ModelReloadInvalidator = AssetReloadInvalidator(
        dispatcher, AssetType::Model,
        [enqueue](const GUID& guid) { enqueue(AssetType::Model, guid); });

    m_CubeLutReloadInvalidator = AssetReloadInvalidator(
        dispatcher, AssetType::CubeLut,
        [enqueue](const GUID& guid) { enqueue(AssetType::CubeLut, guid); });

    // The .material reload subscriber (and its re-register lambda) lives on the
    // material facade now (A1.3). Installed here, alongside the other three, so
    // the dispatcher wiring stays in one place.
    m_MaterialSystem.InstallReloadInvalidator(dispatcher);

    // The active-.rendergraph reload subscriber (A1.4 S3): a redundant fast path
    // beside the spine's cached-asset source-hash poll. Same dispatcher; the
    // handler flips the orchestrator's refresh flag on the main thread.
    m_FrameOrchestrator.InstallReloadInvalidator(dispatcher);
}

void RenderServices::DrainPendingAssetInvalidations()
{
    std::vector<PendingAssetInvalidation> pending;
    {
        std::lock_guard lock(m_PendingAssetInvalidationsMutex);
        pending.swap(m_PendingAssetInvalidations);
    }
    if (pending.empty())
        return;

    // Dedupe: an editor save can fire modify+reload bursts for one asset;
    // evicting once per frame is enough.
    std::sort(pending.begin(), pending.end(),
              [](const PendingAssetInvalidation& a, const PendingAssetInvalidation& b)
              { return a.Type != b.Type ? a.Type < b.Type : a.Guid < b.Guid; });
    pending.erase(std::unique(pending.begin(), pending.end(),
                              [](const PendingAssetInvalidation& a, const PendingAssetInvalidation& b)
                              { return a.Type == b.Type && a.Guid == b.Guid; }),
                  pending.end());

    for (const auto& inv : pending)
    {
        switch (inv.Type)
        {
        case AssetType::Texture:
            m_Textures->Evict(inv.Guid);
            break;
        case AssetType::Model:
            // Mesh GPU buffers are stale. Which way we heal them is decided by
            // the model's RESIDENCY in the AssetManager, not by the event kind:
            //
            //  - still resident (the hot-reload case): refresh in place. The
            //    handles live MeshRenderer components hold are generational, so
            //    unregistering here would strand every entity on a dead slot
            //    with no producer to re-register it — RegisterModelMeshes
            //    short-circuits on an already-known GUID and the only callers
            //    that re-resolve entities run at scene load. Reloading in place
            //    keeps the handles and GPUScene rows and swaps the geometry
            //    underneath them, so the new mesh is simply visible next frame.
            //
            //  - gone from memory: release the GPU mirror. A deleted FILE alone
            //    does not evict the ModelAsset, so a save that lands as
            //    delete+create still takes the in-place path above.
            {
                // Keep the strong ref alive for the whole refresh — `reloaded`
                // borrows from it.
                SharedPtr<Asset> resident;
                if (GameEngine::AssetManager* assetManager =
                        GameEngine::EngineCore::GetInstance().TryGetAssetManager())
                {
                    // Cache-only: this is a resident-RAM to VRAM refresh on the
                    // render thread, never a disk load.
                    resident = assetManager->GetAsset(inv.Guid);
                }
                const auto* reloaded = dynamic_cast<const ModelAsset*>(resident.get());

                if (reloaded)
                {
                    const MeshGPUReloadReport report =
                        m_MeshGPURegistry.ReloadModelMeshes(inv.Guid, *reloaded);
                    m_GPUAnimDataStore.InvalidateSkeleton(reloaded->GetSkeletonId());
                    m_GPUAnimDataStore.InvalidateAllClips();
                    if (report.ChangedAnything())
                    {
                        // The handles survived, so nothing re-resolves the
                        // entities pointing at them: publish the GUID so the
                        // world-owning caller can re-derive the LocalBounds
                        // those entities still carry from the old geometry.
                        m_ModelsReloadedInPlace.push_back(inv.Guid);
                        Logger::Log::Info(
                            "[AssetReload] model {} GPU meshes refreshed in place: "
                            "{} re-uploaded, {} unchanged, {} removed (of {} submeshes)",
                            inv.Guid.ToString(), report.SubmeshesReuploaded,
                            report.SubmeshesUnchanged, report.SubmeshesRemoved,
                            report.SubmeshesTotal);
                    }
                }
                else
                {
                    m_MeshGPURegistry.UnregisterModel(inv.Guid);
                }
                // Live materials take the reloaded images before the old
                // textures go (no images when the model left memory).
                m_Textures->ReloadEmbeddedForModel(
                    inv.Guid, reloaded ? std::span<const EmbeddedImage>(reloaded->GetEmbeddedImages())
                                       : std::span<const EmbeddedImage>{});
            }
            m_ModelsInvalidated.push_back(inv.Guid);
            // Bake reload keeps skeleton/clip ids stable while rest/IBM/keys
            // change; InvalidateSkeleton/InvalidateAllClips above drop the
            // GPU mirrors so Ensure* re-packs from the adopted CPU payload.
            break;
        case AssetType::CubeLut:
            m_Textures->EvictCubeLut(inv.Guid);
            break;
        default:
            break;
        }
    }
}

std::vector<GUID> RenderServices::TakeModelsReloadedInPlace()
{
    return std::exchange(m_ModelsReloadedInPlace, {});
}

std::vector<GUID> RenderServices::TakeModelsInvalidated()
{
    return std::exchange(m_ModelsInvalidated, {});
}

void RenderServices::Shutdown()
{
    // Detach asset-reload subscribers before tearing down anything they touch
    // (texture cache, mesh / model registries). Without this a late
    // AssetReloaded fired during shutdown could call into freed members. The
    // material subscriber is detached inside ShutdownPhaseA below.
    m_TextureReloadInvalidator.Reset();
    m_ModelReloadInvalidator.Reset();
    m_CubeLutReloadInvalidator.Reset();
    // Detach the pipeline reload subscriber here too — BEFORE m_FrameOrchestrator
    // .Shutdown() tears down its members — so a late AssetReloaded can't flip a
    // flag on a half-destroyed orchestrator (§0a-A7 Part 2).
    m_FrameOrchestrator.ResetReloadInvalidator();

    // Material teardown phase A (§0a-A2): material invalidator reset, prewarm job
    // + service drains, binder reset. Runs BEFORE feature teardown — a feature
    // destructor can unregister a material, and the pre-unregister callback +
    // variant cache must still be alive/armed for that (phase B disarms them).
    m_MaterialSystem.ShutdownPhaseA();

    // Cancel any in-flight AssetManager texture loads and drop the queues
    // before subsystems they reference (device, material registry) shut down.
    if (m_Textures)
        m_Textures->CancelPendingLoads();

    // Retire recorded callbacks and this frame's GPU work while the features,
    // material/mesh stores and device they can reference are still alive.
    // Phase A above has already joined material/PSO prewarm workers.
    m_FrameOrchestrator.Shutdown();
    m_FrameRG = {};

    // The only shrink of the registry. Feature destructors run under the
    // exclusive lock: by shutdown the wave-parallel producers are stopped, so
    // nothing can reach EnsureFeature and re-insert behind the clear.
    {
        std::unique_lock lock(m_FeaturesMutex);
        m_FeatureSnapshot.reset();
        m_Features.clear();
    }

    // Destroy GPU animation data store before device shutdown.
    m_GPUAnimDataStore.Shutdown();

    // Destroy per-frame write pools before device shutdown.
    m_PerFrameWritePool.Shutdown();

    // Material teardown phase B (§0a-A2): shader cache clear, pre-unregister
    // callback disarm, variant-cache reset, registry shutdown, prewarm service +
    // compiler destroy, SSBO index + fallback-handle state reset. Runs AFTER
    // feature teardown. The physical MaterialParams fallback buffer is destroyed
    // below via m_FallbackBuffers (§0a-A5).
    m_MaterialSystem.ShutdownPhaseB();

    // Destroy GPU mesh registry before device shutdown.
    m_MeshGPURegistry.Shutdown();

    // Destroy the bucketer driver before device shutdown so its VkBuffers
    // are released through the still-live device.
    if (m_DrawStreamBuilder)
    {
        m_DrawStreamBuilder->Shutdown();
        m_DrawStreamBuilder.reset();
    }

    // Destroy fallback buffers (best-effort). This owns the physical
    // MaterialParams fallback the facade seeded; the facade already cleared its
    // handle copy in ShutdownPhaseB.
    if (m_Device)
    {
        for (auto& [id, buf] : m_FallbackBuffers)
        {
            if (buf.IsValid())
                m_Device->DestroyBuffer(buf);
        }
        // Sorted transparent CPU-drain ring buffers (host-visible, lazily
        // created only for views that exceeded the GPU sort capacity).
        for (auto& [viewId, view] : m_SortedTransparentByView)
        {
            for (auto& buf : view.CpuDrainBuffers)
            {
                if (buf.IsValid())
                    m_Device->DestroyBuffer(buf);
                buf = {};
            }
        }
    }
    m_FallbackBuffers.clear();
    m_SortedTransparentByView.clear();
    if (m_GpuScene)
        m_GpuScene->Shutdown();

    // Destroy per-view GPU resources; CPU-side per-view state (letterbox,
    // pixel-perfect, keywords) survives device teardown. The registry's device
    // pointer stays live until the m_Device null below (A2).
    m_ViewRegistry.DestroyAllPerViewGpuResources();

    if (m_Device)
    {
        if (m_AreaShadowFallbackTexture.IsValid())
            m_Device->DestroyTexture(m_AreaShadowFallbackTexture);
        if (m_TransmittanceShadowFallback.IsValid())
            m_Device->DestroyTexture(m_TransmittanceShadowFallback);
        if (m_PointShadowFallbackTexture.IsValid())
            m_Device->DestroyTexture(m_PointShadowFallbackTexture);
        if (m_CascadeShadowFallbackTexture.IsValid())
            m_Device->DestroyTexture(m_CascadeShadowFallbackTexture);
        if (m_AreaShadowSampler.IsValid())
            m_Device->DestroySampler(m_AreaShadowSampler);
        if (m_AreaShadowRawSampler.IsValid())
            m_Device->DestroySampler(m_AreaShadowRawSampler);
        if (m_SpotShadowSampler.IsValid())
            m_Device->DestroySampler(m_SpotShadowSampler);
        if (m_PointShadowSampler.IsValid())
            m_Device->DestroySampler(m_PointShadowSampler);
        if (m_CascadeShadowSampler.IsValid())
            m_Device->DestroySampler(m_CascadeShadowSampler);
    }
    m_AreaShadowFallbackTexture = {};
    m_TransmittanceShadowFallback = {};
    m_AreaShadowSampler = {};
    m_AreaShadowRawSampler = {};
    m_SpotShadowSampler = {};
    m_PointShadowSampler = {};
    m_CascadeShadowSampler = {};
    m_PointShadowFallbackTexture = {};
    m_CascadeShadowFallbackTexture = {};

    // Destroy the unwired-binding placeholder buffer
    if (m_Device && m_DefaultPlaceholderBuffer.IsValid())
    {
        m_Device->DestroyBuffer(m_DefaultPlaceholderBuffer);
        m_DefaultPlaceholderBuffer = {};
    }



    if (m_GpuCullingPipeline)
    {
        m_GpuCullingPipeline.reset();
    }



    m_GpuScene.reset();
    // Texture service teardown: cached GPU textures, defaults, LUTs, samplers,
    // the bindless set/views/manager. After GPUScene so any bindless-registered
    // scene resources are gone first.
    if (m_Textures)
        m_Textures->Shutdown();
    m_Textures.reset();
    // (The prewarm service is drained + destroyed in MaterialSystem::ShutdownPhaseB,
    // while the device is still live.)
    // Null the registry's + material facade's device at the SAME point as RS's
    // own (A2/A1.2 discipline): any ReleaseView between the per-view GPU destroy
    // above and here still had a live device to free through; from here on none
    // is needed.
    m_ViewRegistry.SetDevice(nullptr);
    m_MaterialSystem.SetDevice(nullptr);
    m_ScreenSpaceShadows.reset();
    m_Device = nullptr;
}

} // namespace Engine::Renderer
} // namespace GameEngine
