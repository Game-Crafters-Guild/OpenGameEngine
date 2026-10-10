// Engine-level RenderServices API and related types.
//
// This header is the canonical home for RenderServices, which owns
// RenderGraph, GPUScene, bindless manager, camera/view registries,
// and world/skinned GPU registries. It is consumed by ECSModules,
// the Editor, tests, and examples.

#pragma once

#include "AssetCore/Asset.h"          // SharedPtr<Asset> in PendingGpuUpload
#include "AssetCore/AssetRegistry.h" // AssetReference
#include "Types/ScopedSubscription.h"
#include "AssetCore/AssetReloadInvalidator.h"
#include "Assets/AssetManager.h"     // AssetLoadHandle in m_TextureLoadHandles
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Components/Rendering/Light.h"
#include "Engine/Rendering/DrawCommandProducer.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/SceneColorGrab.h"
#include "Rendering/CameraTypes.h"
#include "Engine/Rendering/AntiAliasing.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "Engine/Rendering/DeformationMotionParams.h"
#include "Engine/Rendering/DynamicResolutionController.h"
#include "Engine/Rendering/LodCrossfadeLiveness.h" // LodCrossfadeLivenessState member
#include "Engine/Rendering/LodProjectSettings.h"   // kDefaultCrossfadeDuration
#include "Engine/Rendering/ViewRegistry.h"
#include "Engine/Rendering/ViewTemporalHistory.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RendererProfile.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/BindlessResourceManager.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/ResolvedShadowSettings.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/TextureService.h"
#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Engine/Rendering/CpuDrawStreamBuilder.h"
#include "Engine/Rendering/WorldDrawBuilder.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Engine/Rendering/MeshPoolGroupPlan.h"
#include "Engine/Rendering/PassBindingContext.h"
#include "Engine/Rendering/DepthPassTypes.h"
#include "Engine/Rendering/FeatureDeclareContext.h"
#include "Engine/Rendering/PointShadowAtlasPlanner.h"
#include "Engine/Rendering/VolumetricFogTypes.h"
#include "Engine/Rendering/AnimationComputePass.h"
#include "Engine/Rendering/EmbeddedImageDecoder.h"
#include "Engine/Rendering/GPUAnimationDataStore.h"
#include "Engine/Rendering/PerFrameWritePool.h"
#include "Engine/Rendering/ShaderCompilationCache.h"
#include "Engine/Rendering/SortedTransparentRuns.h"
#include "Engine/Rendering/Pipeline/PipelineFrameResources.h"
#include "Engine/Rendering/SkinPaletteAtlas.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "JobSystem/TaskHandle.h"
#include "Types/Types.h"
#include <filesystem>

#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUInstanceDepthClass.h" // MaterialDepthClass (GetMaterialDepthClass)

namespace JobSystem { class WorkStealingThreadPool; }

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::GPUDrawStreamBuilder;
namespace RenderGraph = ::GameEngine::Rendering::RenderGraph;
using ::GameEngine::Rendering::BindlessResourceManager;
using ::GameEngine::Rendering::BindlessTextureDesc;
using ::GameEngine::Rendering::BindlessTextureHandle;
using ::GameEngine::Rendering::CameraData;
using ::GameEngine::Rendering::CameraId;
using ::GameEngine::Rendering::CameraInfo;
namespace CullModeFlagBits = ::GameEngine::Rendering::CullModeFlagBits;
using ::GameEngine::Rendering::CullModeFlags;
using ::GameEngine::Rendering::DescriptorSetHandle;
using ::GameEngine::Rendering::DescriptorSetLayoutId;
using ::GameEngine::Rendering::FrontFace;
using ::GameEngine::Rendering::GPUCullingPipeline;
using ::GameEngine::Rendering::GPUScene;
using ::GameEngine::Rendering::ICullingStrategy;
using ::GameEngine::Rendering::MeshGPURegistry;
using ::GameEngine::Rendering::PipelineHandle;
using ::GameEngine::Rendering::PrimitiveTopology;
using ::GameEngine::Rendering::SamplerHandle;
using ::GameEngine::Rendering::SamplerPreset;
using ::GameEngine::Rendering::ShaderPackage;
using ::GameEngine::Rendering::TextureAspect;
using ::GameEngine::Rendering::TextureFormat;
using ::GameEngine::Rendering::TextureViewHandle;
using ::GameEngine::Rendering::ViewDesc;
using ::GameEngine::Rendering::ViewTextureHandle;
using ::GameEngine::Rendering::kInvalidBindlessTexture;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{
namespace Mathematics { struct Vector4; }
namespace UI { class UITextureSpace; }
namespace Rendering
{
class GPUScene;
struct GPUInstance;
class BindlessResourceManager;
class IDevice;
class GPUCullingPipeline;
class GPUDrawStreamBuilder;
class ICullingStrategy;
class NamedPushConstantWriter;
struct ShaderPackage;
} // namespace Rendering
class ModelAsset;
struct EmbeddedImage;
namespace Hlod { class HlodRuntime; }

namespace Engine::Renderer
{
class IRenderFeature;
class ShadowMapRenderFeature;
class RetargetRenderFeature;
class MaterialCompiler;
class SceneAccelerationStructureService;
class RTShadowMaskService;
class ScreenSpaceShadowPasses;
struct DepthDrawServices;
class DepthUnderDrawTracker;
namespace Pipeline
{
class IRenderPipelineNode;
class RenderPipelineInstance;
class RenderPipelineCompiler;
class RenderPipelineNodeRegistry;
class IRenderPipelineNode;
struct RenderPipelineBlueprint;
} // namespace Pipeline

// MaterialAlphaMode is defined in WorldDrawTypes.h (included above).

// Well-known default world mesh/material IDs used by the Editor bootstrap
// world and simple test scenes. ID 0 is treated as "unassigned"; these
// defaults provide a minimal path to get something on screen without
// requiring a full asset/material pipeline.

// WorldSubmissionRecord is defined in WorldDrawTypes.h.
// DrawCommand (the unified per-draw record) is defined in DrawCommand.h.

// Extracted CPU-side light record (uploaded later by lighting passes).
// This is the first step towards Forward+/Deferred lighting extraction from ECS.
struct ExtractedLight
{
    GameEngine::Components::LightType type{GameEngine::Components::LightType::Directional};
    float positionWS[3]{0.0f, 0.0f, 0.0f};
    float range{10.0f};
    float directionWS[3]{0.0f, -1.0f, 0.0f}; // shine (+Z / col2); surface->light is -dir
    float rightWS[3]{1.0f, 0.0f, 0.0f};
    float upWS[3]{0.0f, 1.0f, 0.0f};
    float innerAngle{0.5f};                 // radians (spot)
    float outerAngle{0.8f};                 // radians (spot)
    GameEngine::Components::AreaLightShape areaShape{GameEngine::Components::AreaLightShape::Rectangle};
    float areaWidth{1.0f};
    float areaHeight{1.0f};
    float areaRadius{0.5f};
    GameEngine::Components::LightFalloff falloff{GameEngine::Components::LightFalloff::PhysicalInverseSquare};
    float decay{2.0f};
    float fogContribution{1.0f};
    float fogDensityBoost{0.0f};
    float fogAnisotropy{0.25f};
    float fogOriginFade{0.2f};
    float intensity{1.0f};
    float color[3]{1.0f, 1.0f, 1.0f};
    uint32 castsLight{1};
    uint32 castsShadows{0};
    uint32 cascadeCount{4};
    // Opt-in punctual shadow resolution tier (Components::LightShadowTier as
    // uint); 0 = inherit the pipeline node's punctual resolution.
    uint32 shadowResolutionTier{0};
    // Angular diameter in DEGREES (Components::Light::ShadowAngularDiameter).
    // Resolved to tan(halfAngle) at cascade build — see
    // CascadeFrameData::ShadowTanHalfAngle.
    float shadowAngularDiameter{0.53f};
    // Stable per-light id (ECS entity id). Deterministic tie-break for the
    // contribution sort so equal-contribution lights keep a frame-stable order
    // independent of ECS iteration order.
    uint32 SortId{0};
};

// The engine shades up to kMaxDirectionalLights directionals per world, but
// exactly ONE of them — the PRIMARY — drives cascaded shadows, fog sun
// tracking, and every other non-BRDF directional consumer. Every such consumer
// must agree on WHICH one, so the selection is centralized here: the first
// directional in the world-light list, which FinalizeWorldLights orders
// strongest-first. The selected light's OWN castsShadows flag decides whether
// cascades exist — a different directional's flags can never ride along.
// Returns nullptr when the world has no directional light. (Three consumers
// pick their own directional from the ECS components instead, because they all
// run before this finalized list exists in the frame: the sky — its explicit
// SunLight link when one is set, else the scene's brightest emitting
// directional; the ocean — first in ECS order; and a lens flare — a directional
// on its own entity or its parent. Unifying them is tracked.)
const ExtractedLight* SelectPrimaryDirectional(std::span<const ExtractedLight> sortedLights);

// Luminance of the light a light delivers, colour times intensity (unitless, 203-nit scale). Zero
// for a light that is on but black: a sky-driven sun at night with the moon hidden carries its
// darkness in its colour, not its intensity, so no consumer may gate on the intensity alone.
float DeliveredLuminance(const ExtractedLight& light);

// The primary directional when it lights anything: SelectPrimaryDirectional, or null when there is
// none, it does not cast light, or it delivers nothing. The fog sun (height fog, clouds, volumetric
// fog) comes from here and falls back to the sky when this is null.
const ExtractedLight* SelectLitPrimaryDirectional(std::span<const ExtractedLight> sortedLights);

// Directional lighting capacity: the primary (shadowed) directional plus up to
// kMaxSecondaryDirectionals unshadowed secondaries evaluated additively with
// the same BRDF. Secondaries deliberately carry NO shadow term (v1 semantic:
// cascades exist for the primary only; a secondary never darkens anything).
inline constexpr uint32 kMaxDirectionalLights = 4;
inline constexpr uint32 kMaxSecondaryDirectionals = kMaxDirectionalLights - 1;

// GPU layout matching the composed adapters' LightUBO (set 0, binding 6 — see
// adapter_forward.glsl / adapter_vertex.glsl). std140: every field is a vec4
// or mat4, so the C++ mirror needs no hand padding; the static_asserts below
// lock the byte layout the shaders index. Single definition site — the
// per-view writer (WriteViewLightBuffer) and the zero fallback buffer both
// size/offset from THIS struct.
struct alignas(16) ForwardLightUBO
{
    float uLightVP[16];        // identity unless a legacy single-shadow path writes it
    float uLightDirWorld[4];   // xyz=light->surface direction, w=intensity (primary)
    float uLightColorWorld[4]; // xyz=color, w=castsShadows (1.0 if true) (primary)
    float uAmbient[4];         // xyz=color, w=intensity
    // x=shader time s, y=scroll time s — the process-wide globals a fragment
    // stage reads. z=the deformation time, rebased against the origin every
    // view shares (ViewDeformationClock), w=its scroll lane: the pair a vertex
    // modifier's InstanceData endpoint is stamped from, which must be
    // differenceable between two endpoints and so cannot be absolute uptime.
    float uTimeParams[4];
    float uSecondaryCount[4];  // x=valid secondary directional count (as float)
    float uSecondaryDirs[kMaxSecondaryDirectionals][4];   // xyz=light->surface direction
    float uSecondaryColors[kMaxSecondaryDirectionals][4]; // rgb=color*intensity (premultiplied)
};
static_assert(sizeof(ForwardLightUBO) == 240, "LightUBO layout drifted from the GLSL block");
static_assert(offsetof(ForwardLightUBO, uLightDirWorld) == 64);
static_assert(offsetof(ForwardLightUBO, uLightColorWorld) == 80);
static_assert(offsetof(ForwardLightUBO, uAmbient) == 96);
static_assert(offsetof(ForwardLightUBO, uTimeParams) == 112);
static_assert(offsetof(ForwardLightUBO, uSecondaryCount) == 128);
static_assert(offsetof(ForwardLightUBO, uSecondaryDirs) == 144);
static_assert(offsetof(ForwardLightUBO, uSecondaryColors) == 192);

// Fills the directional lanes of the composed LightUBO from a finalized
// (strongest-first) world light list: primary = SelectPrimaryDirectional
// (unchanged semantics — its castsShadows flag rides in uLightColorWorld.w),
// then the next kMaxSecondaryDirectionals emitting directionals become
// unshadowed secondaries with premultiplied color*intensity. Non-emitting
// (castsLight == 0) directionals and ones that deliver nothing (zero
// intensity or a black colour) never pack as secondaries.
// Leaves every other lane of the UBO untouched.
void PackForwardLightDirectionals(std::span<const ExtractedLight> sortedLights, ForwardLightUBO& ubo);

// GPU layout matching clustered_lighting.glsl AreaShadowData.
struct alignas(16) AreaShadowDataGPU
{
    // Light-local transform for Godot-style paraboloid area shadow sampling.
    float areaShadowVP[16]{};
    // x=enabled, y=cluster-light index, z=depthBias, w=normalBias
    float areaShadowParams[4]{};
    // x=shadow map resolution, y=shadow range, z=light size, w=unused
    float areaShadowParams2[4]{};
};
static_assert(sizeof(AreaShadowDataGPU) == 96, "AreaShadowDataGPU must be 96 bytes");

// GPU layout matching clustered_lighting.glsl SpotShadowData.
struct alignas(16) SpotShadowDataGPU
{
    float spotShadowVP[16]{};
    // x=enabled, y=cluster-light index, z=depthBias, w=normalBias
    float spotShadowParams[4]{};
    // x=shadow map resolution, y=near plane, z=far plane, w=unused
    float spotShadowParams2[4]{};
};
static_assert(sizeof(SpotShadowDataGPU) == 96, "SpotShadowDataGPU must be 96 bytes");

// One element of the M1 std430 point-shadow slot SSBO array (binding 25), mirroring
// clustered_lighting.glsl PointShadowSlot. std430 array stride == sizeof == 416
// (already 16-aligned: six mat4 columns are each 16-aligned, two vec4 tails). Hand-
// verify against the GLSL if any field moves (the AtmosphereUBO std140 lesson).
struct alignas(16) PointShadowSlotGPU
{
    float pointShadowVP[6][16]{};
    // x=enabled, y=baseLayer (slot*6), z=depthBias, w=normalBias
    float pointShadowParams[4]{};
    // x=tileResolution, y=near plane, z=far plane, w=tileScale (tileRes/atlasRes)
    float pointShadowParams2[4]{};
};
static_assert(sizeof(PointShadowSlotGPU) == 416, "PointShadowSlotGPU must be 416 bytes");

class RenderServices;
class MaterialBinder;

// Central owner of rendering subsystems used by ECS-facing systems and Editor
class RenderServices
{
    // The material facade reads the rasterizer state
    // (m_CullMode/m_FrontFace) synchronously at PSO build — the single narrow
    // reach-back edge documented in the A1.3 design (§0a-A3).
    friend class MaterialSystem;
    // The frame orchestrator (A1.4) reaches the RS-resident frame-local state
    // that STAYS on RenderServices (m_FrameRG, m_ViewFrameRG,
    // m_CullingScheduledThisFrame, the feature map, the per-world build
    // counters) at its ~7 spine sites — glue by nature, documented (§0a-A1).
    friend class FrameOrchestrator;

  public:
    RenderServices();
    ~RenderServices();

    bool Initialize(Rendering::IDevice* device);
    void Shutdown();

    // Weak lifetime token, same contract as MeshGPURegistry::LifetimeToken.
    // ECS systems hold a RenderServices* and are destroyed with the world —
    // AFTER RenderDeviceContext tears this object down — so a destructor that
    // calls back in (device access at teardown) must check this first.
    std::weak_ptr<std::atomic<bool>> LifetimeToken() const { return m_Alive; }

    Rendering::GPUScene* GetGPUScene() const
    {
        return m_GpuScene.get();
    }
    Rendering::IDevice* GetDevice() const
    {
        return m_Device;
    }

    // Resolved feature policy for this device, derived once in Initialize.
    // Render features gate on this rather than re-querying raw capabilities,
    // so the policy stays loggable and testable in one place.
    const Rendering::RendererProfile& GetProfile() const
    {
        return m_Profile;
    }

    // Package-backed render features use the mounted asset source as their
    // runtime availability contract. Disabled packages are absent from the
    // mount table after project reopen, so their passes must stitch through
    // even when their serialized post-process controls remain enabled.
    bool IsPackageAvailable(std::string_view packageName) const;

    // Host/test seam for environments whose package state does not come from
    // EngineCore's AssetManager. An empty callback restores mount-backed
    // discovery.
    void SetPackageAvailabilityQuery(std::function<bool(std::string_view)> query)
    {
        m_PackageAvailabilityQuery = std::move(query);
    }

    // Texture residency + binding service: the GPU texture cache and async
    // upload pump, the bindless array, samplers, default textures, .cube LUTs,
    // and material texture binding. Valid between Initialize() and Shutdown().
    TextureService& Textures() { return *m_Textures; }
    const TextureService& Textures() const { return *m_Textures; }

    // Per-view pass keywords (ForwardPlus, Shadows, Instanced, ...) registered
    // by AddWorldPassForView. Forward contributors read these to drive
    // MaterialBinder::BeginPass with the same keyword set the world pass uses.
    std::optional<Rendering::MaterialKeyword> GetWorldPassKeywords(Rendering::ViewId viewId) const;

    // GPU culling pipeline (frustum/HZB). Owned by RenderServices and created
    // during Initialize(); ScheduleViewCullingDispatches drives it per frame
    // via ICullingStrategy per view + IRenderFeature::OnScheduleCulling for
    // fan-outs (e.g. CSM cascades).
    Rendering::GPUCullingPipeline* GetGPUCullingPipeline() const
    {
        return m_GpuCullingPipeline.get();
    }

    // GPU mesh registry: GUID-keyed, deduplicated owner of GPU-resident mesh
    // buffers (VB/IB/optional streams). Multiple entities referencing the same
    // ModelAsset share the same GPU resources via this registry.
    Rendering::MeshGPURegistry& GetMeshGPURegistry() { return m_MeshGPURegistry; }
    const Rendering::MeshGPURegistry& GetMeshGPURegistry() const { return m_MeshGPURegistry; }

    // Route A's half of the residency gate's observability: what the pool-group
    // plan left absent because the uploaded bytes were not readable by the GPU
    // yet, with the Refresh count that scopes it. Route B's counters live on
    // the registry above. Exposed as numbers rather than the plan itself — the
    // plan's spans are per-frame scatter inputs with a single owner.
    Rendering::MeshPoolGroupResidencyStats GetMeshPoolGroupResidencyStats() const
    {
        return m_MeshPoolGroups.ResidencyStats();
    }

    // HLOD runtime state: the reconciled cluster table + spawned proxies for the
    // currently bound world. Owned here so both the scene-load reconcile seam and
    // the per-frame HLODSelectSystem reach one instance. Always non-null between
    // Initialize() and Shutdown().
    Hlod::HlodRuntime& GetHlodRuntime() { return *m_HlodRuntime; }
    const Hlod::HlodRuntime& GetHlodRuntime() const { return *m_HlodRuntime; }

    // HLOD residency-flip escalation (design v0.2 §5.1). A cluster flip evicts the
    // losing set directly, but re-adding the winning set (proxy on enter, members
    // on exit) is a full-lane Op::Add. HLODSelectSystem requests the full lane
    // here on any flip; RenderExtractionSystem consumes the request when it
    // evaluates fast-path preconditions. Routed through RenderServices so the two
    // systems stay decoupled (neither holds a pointer to the other).
    void RequestHlodResidencyExtraction() { m_HlodResidencyPending.store(true, std::memory_order_relaxed); }
    bool ConsumeHlodResidencyExtractionPending() { return m_HlodResidencyPending.exchange(false, std::memory_order_relaxed); }

    // L1a point-shadow whole-light on-dirty caching signal. A per-world monotonic
    // version of the renderable scene's shadow-caster content. RenderExtractionSystem
    // bumps the extracted world's version whenever it did NOT take a pure idle
    // fast-path frame — i.e. the full lane ran (structural / material / LOD / HLOD /
    // mesh-reload / feed change) OR the fast path patched >=1 moved instance. A
    // full lane run only because an OnDemand view (a probe capture, a mirror)
    // started or stopped does not bump it: no persistent view's records changed.
    // EnsurePointShadowAssignment folds the version into each slot's cached render
    // key, so any caster change re-renders that world's shadowed point lights.
    // Camera motion never bumps it (ComputeViewFingerprints carries no matrices), so a
    // flythrough over a static scene keeps every light cached. Keyed by world so a
    // preview/thumbnail world (which always runs the full lane) cannot invalidate the
    // main world's cache.
    //
    // L1b caster-proximity keying: alongside the bump, the caller supplies the
    // world-space spheres (old ∪ new bounds) of the specific casters that changed —
    // when it can attribute them (fast-path moved-instance patches, always-refresh
    // subset re-prepares). EnsurePointShadowAssignment forwards the set to the
    // per-view atlas planner, which dirties only the point lights whose influence
    // sphere a changed caster intersects. `unattributed` = true (full-lane frame,
    // animated-vertex caster content, bone-palette pose change, sphere-list overflow)
    // affects every light —
    // exactly the pre-L1b world-scoped behavior. The bump TIMING is unchanged
    // either way. Directional cascades also use the bounds to retain unaffected
    // layers; the RT mask continues to consume the scalar version.
    void NotifyShadowCasterContentChanged(
        uint64 worldId, std::span<const ShadowCasterChangeSphere> changedCasterSpheres,
        bool unattributed);
    uint64_t ShadowCasterContentVersion(uint64 worldId) const
    {
        const auto it = m_ShadowCasterChanges.find(worldId);
        return it != m_ShadowCasterChanges.end() ? it->second.Version : 0u;
    }

    ShadowCasterChangeSet GetShadowCasterChanges(uint64 worldId) const
    {
        const auto it = m_ShadowCasterChanges.find(worldId);
        if (it == m_ShadowCasterChanges.end())
            return {};
        return {it->second.Version, it->second.Spheres, it->second.Unattributed};
    }

    // Idle-recompute-elision content signal (lever #2): the ANY-renderable
    // analogue of the shadow-caster version above. The extraction lane bumps
    // it on the same conditions; the batch-key derivation bumps it additionally
    // whenever ANY submitted renderable (caster or not) uses a material that
    // moves its own vertices — its rasterized depth changes every frame with the
    // shared animation clock, which the depth-derived elision families (HZB/P2
    // occlusion, DepthMinMax, ClusteredLightCull, SDSM DepthReduce) must observe
    // even though no instance data moved. Camera motion never bumps it (per-view
    // camera bytes are a separate key term at each elision gate).
    void NotifyRenderContentChanged(uint64 worldId) { ++m_RenderContentVersions[worldId]; }
    uint64_t RenderContentVersion(uint64 worldId) const
    {
        const auto it = m_RenderContentVersions.find(worldId);
        return it != m_RenderContentVersions.end() ? it->second : 0u;
    }
    // Idle-recompute-elision skeletal-animation signal: called by
    // AnimationSystem (exact-input palette set delta) and
    // HumanoidRetargetSystem (conservative character presence) on frames
    // whose bone-palette CONTENT can differ from last frame's. Palettes live
    // in a per-frame ring the extraction lane never inspects — a playing
    // in-place animation changes rasterized depth without bumping
    // ContentEpoch — so the spine folds this flag into DepthDynamicEpoch,
    // waking the depth-derived families (cull recompute then chains into the
    // scatter gates via VisibilityWriteEpoch). Consumed and cleared once per
    // app frame by UpdateIdleElisionFrameState; animation systems run earlier
    // in the same frame (Animation/Skinning phases precede the culling
    // schedule), so the signal is same-frame.
    //
    // A re-posed palette also changes what the caster rasterizes into every
    // shadow map, so the same signal bumps the world's shadow-caster content
    // version. Without it the shadow caches (CascadeShadowCache and the point/
    // spot PointShadowAtlasPlanner, both keyed on that version) see every input
    // byte-identical for a character animating in place and retain the layer
    // rendered at an older pose — the mesh animates while its shadow holds
    // still. Unattributed, matching the vertex-mod caster precedent in
    // RenderExtractionSystem: the extraction lane carries no per-caster signal
    // for pose-only change, so proximity keying cannot name which casters moved.
    void NotifySkinPaletteContentChanged(uint64 worldId)
    {
        m_SkinPaletteContentChanged = true;
        NotifyShadowCasterContentChanged(
            worldId, std::span<const ShadowCasterChangeSphere>{}, /*unattributed=*/true);
    }
    // Per-world light-list version: FinalizeWorldLights bumps it whenever the
    // finalized (sorted) list's bytes differ from the previous frame's. Exact
    // memcmp — the light signal the cluster/culling elision gates key on.
    uint64_t WorldLightListVersion(uint64 worldId) const
    {
        const auto it = m_WorldLightVersions.find(worldId);
        return it != m_WorldLightVersions.end() ? it->second : 0u;
    }
    // Per-frame reset of one world's submitted lights — the unit a frame
    // boundary applies to every world at once (BeginWorldFrame's bulk clear).
    // Exposed so tests can emulate frame boundaries around SubmitLight/
    // FinalizeWorldLights without the frame spine.
    void ResetWorldLights(uint64 worldId)
    {
        const auto it = m_WorldLights.find(worldId);
        if (it != m_WorldLights.end())
            it->second.clear();
    }

    // Per-frame idle-recompute-elision state, refreshed once per app frame by
    // the spine (UpdateIdleElisionFrameState) and consumed by the pipeline
    // nodes' per-view gates (ClusteredLightCull/DepthMinMax via
    // ComputeShaderNode idleElide, SDSM DepthReduce in ShadowMapNode) and by
    // the module-side gate contexts. Allow* fold the master kill-switch
    // (GE_IDLE_ELISION, default on) with the per-family switches
    // (GE_IDLE_ELISION_{CULL,SCATTER,CLUSTER,SDSM}).
    struct IdleElisionFrameState
    {
        bool AllowCull = false;
        bool AllowScatter = false;
        bool AllowCluster = false;
        bool AllowSdsm = false;
        uint64_t ContentEpoch = 0;      // Σ RenderContentVersion + ShadowCasterContentVersion
        uint64_t LightEpoch = 0;        // Σ per-world light-list versions
        uint64_t DepthDynamicEpoch = 0; // advances every frame while contributor draw
                                        // producers (terrain/ocean/grass) are registered
    };
    const IdleElisionFrameState& GetIdleElisionFrameState() const { return m_IdleElision; }

    // Phase 4 indirect-draw bucketer driver. Owns per-(view, mesh, material)
    // stream slots; ScheduleWorldBucketerDispatches enumerates batches and
    // queues compute passes via RG. World color pass execute consumes the
    // resulting drawCmd/count buffers via vkCmdDrawIndexedIndirectCount.
    Rendering::GPUDrawStreamBuilder* GetDrawStreamBuilder() { return m_DrawStreamBuilder.get(); }

    // Global runtime LOD selection controls for the GPU-driven draw path. LOD
    // index ranges are baked per-mesh (MeshLODGenerator); these tune selection.
    // bias is a log2 offset (positive keeps more detail); forceLevel ==
    // 0xFFFFFFFF auto-selects by screen coverage, otherwise pins that level.
    void SetLODGlobalBias(float biasLog2) { m_LODGlobalBias = biasLog2; }
    float GetLODGlobalBias() const { return m_LODGlobalBias; }
    // Extra log2 bias applied only to shadow buckets (cascades/spot/point/area):
    // negative = shadow maps switch to coarser LODs sooner than the view.
    // Default 0 keeps shadow and view silhouettes on the same level for the
    // camera-following buckets (cascade/spot/area); point faces select their
    // LOD from the light, so their level tracks light distance, not the view.
    void SetShadowLODBias(float biasLog2) { m_ShadowLODBias = biasLog2; }
    float GetShadowLODBias() const { return m_ShadowLODBias; }
    void SetLODForceLevel(uint32_t level) { m_LODForceLevel = level; }
    uint32_t GetLODForceLevel() const { return m_LODForceLevel; }
    // SSE-budget LOD selection (MeshLODThresholds.h): the projected-error
    // pixel budget generated chains may spend before switching to the next
    // level, and the multiplier that tightens it for skinned/character
    // chains. Persisted as project settings rendering.lodErrorBudgetPx /
    // rendering.lodSkinnedBudgetScale, alongside rendering.lodMode which picks
    // the mapping itself; Rendering::LodProjectSettings owns those keys and
    // applies them, in the editor and in a built game. <= 0 budget disables SSE
    // coarsening (keep-detail fail-safe in the scatter) — that is a keep-detail
    // fail-safe for SSE-flagged slots only, NOT "LOD off": turning selection
    // off is rendering.lodMode = "off", which zeroes every row's switch points.
    void SetLODErrorBudgetPx(float budgetPx) { m_LODErrorBudgetPx = budgetPx; }
    float GetLODErrorBudgetPx() const { return m_LODErrorBudgetPx; }
    void SetLODSkinnedBudgetScale(float scale) { m_LODSkinnedBudgetScale = scale; }
    float GetLODSkinnedBudgetScale() const { return m_LODSkinnedBudgetScale; }
    // Per-view-class override of the budget above, keyed by the view's
    // Rendering::ViewPurpose — the engine's existing view classification, which
    // behaviour must key off rather than debug names (CameraTypes.h). Layered:
    // a class whose override is disabled spends the global budget unchanged, so
    // all-disabled is byte-identical to no overrides at all.
    //
    // Applied in ComputeViewSseScales, which is the ONLY correct insertion
    // point: ShadowMapRenderFeature folds that function's result into the
    // cascade static-cache key, so an override change invalidates retained
    // cascades. Scaling the budget any later (e.g. when building ViewLODParams)
    // would bypass the key and strand retained cascades on stale LOD picks.
    //
    // Persisted per project as rendering.lodGameViewBudget* /
    // rendering.lodSceneViewBudget* (Editor::LodProjectSettings).
    void SetLODViewBudgetOverride(Rendering::ViewPurpose purpose,
                                  const Rendering::LodViewBudgetOverride& classOverride);
    const Rendering::LodViewBudgetOverride& GetLODViewBudgetOverride(
        Rendering::ViewPurpose purpose) const;
    // Per-view SSE coverage scales for the scatter (default + tight class),
    // from the view's last published render-target height and the budget
    // knobs above. Public because ShadowMapRenderFeature folds them into the
    // cascade static-cache key (a budget/viewport change must invalidate
    // retained cascades, or their frozen LOD picks would diverge from a
    // fresh render's).
    struct ViewSseScales
    {
        float Default = 0.0f;
        float Tight = 0.0f;
    };
    ViewSseScales ComputeViewSseScales(Rendering::ViewId viewId) const;
    // Small-object cull threshold (PROTOTYPE, default 0 = off): camera-slice
    // instances whose projected bounding-sphere coverage falls below this are
    // dropped by the scatter like a visibility cull. Never applied to shadow
    // buckets or ortho/thumb slices. Evaluation knob only until the pop-in
    // cost is signed off (dithered fade is the productization path).
    void SetSmallObjectCullCoverage(float coverage) { m_SmallObjectCullCoverage = coverage; }
    float GetSmallObjectCullCoverage() const { return m_SmallObjectCullCoverage; }
    // Dithered LOD crossfade duration in seconds; 0 = off. Ships ON, at
    // Rendering::LodProjectSettings::kDefaultCrossfadeDuration.
    // Non-zero makes a camera-slice level change dissolve over this many seconds
    // instead of popping: the scatter emits both levels with complementary
    // screen-space dither phases, and the forward adapter compiles in the
    // discard under MaterialKeyword::LodCrossfade. Never applied to shadow
    // buckets — dithering a caster punches holes in its shadow map, which PCF
    // averages into a washed-out shadow. Zero is byte-identical to a build
    // without the feature: no headroom, no fade buffer, no dither in any SPIR-V.
    //
    // Depth-correct with or without a depth prepass, by drawing the pair in BOTH
    // passes rather than by relaxing the depth state of either. The prepass and
    // the colour pass share one cascade=None record set; a fading instance
    // writes its record PAIR into the row's TAIL region and no head record, and
    // both passes issue that tail through the LodCrossfade variant
    // (BatchDrawRange::Segments). The one shared GE_LodCrossfadeKeep runs in
    // both fragment stages on the same record code at the same gl_FragCoord, so
    // the prepass writes depth for exactly the fragments the colour pass keeps —
    // each level on its own half of the dither, neither writing where the other
    // does, no mutual z-kill and no holes. The depth attachment therefore stays
    // read-only under a prepass whether or not a fade is live, exactly as it is
    // with the feature off. That matters well beyond the picture: roughly twenty
    // nodes read View.DepthResolved (GTAO and the contact shadows its occluders' copy,
    // Names::View::OccluderDepthResolved) — the SDSM depth reduce, the clustered
    // light-cull depth bounds, the HZB, SSR, TAA, DoF, lens-flare occlusion, the
    // transparent depth test, height fog and the depth-keyed post chain — and a
    // silhouette missing from it reads to every one of them as empty sky.
    //
    // Cost, and where it lands: a fading tail forfeits early-Z and pays a
    // fragment stage in the DEPTH pass too (an opaque tail's depth fragment runs
    // the dither alone — no material fetch, no surface eval), and it shades
    // twice in colour. All of it is scoped to fading segments; settled segments
    // keep the fragment-shader-free depth fast path. Residual: a frame whose
    // LodCrossfade DEPTH variant is still compiling skips that tail in depth
    // (self-heals on publish), and for those frames the instance is missing from
    // prepass depth — see BatchDrawRange::Segments for the two cold-variant
    // paths.
    // Clamped at the setter: the shader divides by the duration, and the
    // comparison also maps NaN to "off" rather than into 1/NaN.
    void SetLODCrossfadeDuration(float seconds)
    {
        m_LODCrossfadeDuration = seconds > 0.0f ? seconds : 0.0f;
    }
    float GetLODCrossfadeDuration() const { return m_LODCrossfadeDuration; }
    // LOD dwell band (default 0 = off, stateless per-frame selection) as a
    // FRACTION of the switch coverage: gaining detail requires the coverage to
    // clear a level's threshold by (1+band), while holding or losing it requires
    // only the threshold. Stops a camera hovering at a threshold from
    // alternating between two levels every frame; a single switch is unaffected.
    // Camera slices only — shadow buckets keep the stateless pick.
    //
    // Turning this on makes LOD selection depend on the previous frame, so a
    // capture is no longer reproducible from pose alone. Set 0 (or
    // GE_LOD_HYSTERESIS=0) for A/B measurement.
    //
    // Clamped at the setter to the settings domain [0, 1]
    // (LodProjectSettings::kMin/kMaxHysteresisBand): the settings loader clamps
    // on read, but the runtime path (set_lod IPC) reaches here directly. The
    // comparison direction also maps NaN to "off", like the crossfade setter.
    void SetLODHysteresisBand(float band)
    {
        m_LODHysteresisBand = band > 0.0f ? (band < 1.0f ? band : 1.0f) : 0.0f;
    }
    float GetLODHysteresisBand() const { return m_LODHysteresisBand; }
    // Resolves the effective force level for a view: the per-view override from
    // the registry if set, otherwise the global m_LODForceLevel. Public because
    // the cascade static-shadow cache keys on the value the bucketer consumes.
    uint32_t ResolveViewForceLOD(Rendering::ViewId viewId) const;
    // The LOD selection knobs this view's point-shadow face slices rasterize
    // under. THE single source for both consumers: the atlas planner keys these
    // values and the bucketer builds the face slices' ViewLODParams from them
    // (MakePointFaceLodParams). Deriving them independently at the two sites is
    // how a key silently stops matching what it keys.
    PointShadowLodKey ResolvePointShadowLodKey(Rendering::ViewId viewId) const;
    // Declares the per-view bucketer into the window's
    // frame mid-frame, AFTER BuildFrameGraph(RGFrame&). Threads the ordering
    // proxy into FrameRG() so a following AddWorldPassForView derives the
    // bucketer→draw RAW edge. REQUIRES the spine to have stamped this frame
    // incarnation (FrameRG().For.IsFor(...)): its Visibility value is the
    // dispatch's binding, and the SkinPaletteAtlas producers are only ordered
    // on spine frames. Returns false when buckets exist but could not be
    // scheduled — the caller must defer the render and retry (finalizing it
    // would bake a blank slot); returns true when scheduled OR when the view
    // has no batch keys (a clear-only render is legitimate).
    bool ScheduleBucketerDispatchesForView(Rendering::RenderGraph::RGFrame& frame,
                                           Rendering::ViewId viewId);
    // Same declaration, but one Color-table slice per cascade index instead of
    // the single cascade-None slice: a view fanned out into several cull slices
    // (reflection probe faces at ProbeFaceCullingIndex(face)) scatters all of
    // them in ONE call, and each later AddWorldPassForView selects its slice
    // through `sliceCascadeIndex`. Every slice binds that index's published
    // visibility range (disableVisibilityCheck when none was scheduled).
    bool ScheduleBucketerDispatchesForViewSlices(Rendering::RenderGraph::RGFrame& frame,
                                                 Rendering::ViewId viewId,
                                                 std::span<const uint8_t> cascadeIndices);

    // ── RenderGraph arms.
    // Frame-local RGBuffer VALUES thread through m_FrameRG — THE replacement
    // for the GPUCullingDone tag + sentinel name-dedup: downstream passes
    // declare Reads on these exact values, forming real RAW edges.
    // Fixed declaration order per frame: skinning → culling → bucketer →
    // (pipeline/world passes, slices 3-4) → union → aggregate. The bucketer
    // MUST precede every pass that Reads DrawStreamOrdering: recording order
    // gives the edge its direction, and a Read recorded before the Write
    // derives WAR — the draws would consume frame N−1's slot buffers.
    // PER VIEW, the producer arms (prepass, shadow cascades, area shadow)
    // MUST precede the world pass for the same reason — the world's reads
    // of the shadow array / area map / loaded depth chain from THIS frame's
    // writes only when those writes were recorded first (ViewFrameRG carries
    // a loud tripwire). ──
    // GpuDrivenFrameRG is defined in FrameOrchestrator.h (A1.4): the spine's
    // per-stream slots hold one each, and m_FrameRG (which STAYS here, A1) is
    // the declare-scope cursor the stage-G schedulers and pass arms write.
    const GpuDrivenFrameRG& FrameRG() const { return m_FrameRG; }
    void ScheduleGpuSkinningAndRetarget(Rendering::RenderGraph::RGFrame& frame);
    void ScheduleViewCullingDispatches(Rendering::RenderGraph::RGFrame& frame, float deltaTime);
    void ScheduleWorldBucketerDispatches(Rendering::RenderGraph::RGFrame& frame);
    // RT shadow-mask lane (DirectionalShadowMode::RayTraced, default
    // Cascades): builds BLAS/TLAS for the ray-query hard-shadow mask. No-op
    // unless some world's resolved shadow settings ask for RayTraced AND the
    // device reports supportsRayQuery (warned once otherwise).
    void ScheduleRTShadowMask(Rendering::RenderGraph::RGFrame& frame);

    // Lazily constructs (or returns the already-constructed) shared scene
    // acceleration-structure service — the BLAS pool + multi-slot TLAS
    // backend every ray-query consumer shares (RTShadowMaskService,
    // DDGIProbeFeature). Construction is gated ONLY on supportsRayQuery, not
    // on any particular consumer's activation, so DDGI can use it on a
    // ray-query-capable device even when no world has ever requested
    // RayTraced directional shadows. Returns nullptr when the device does
    // not support ray query (never warns here — ScheduleRTShadowMask already
    // owns the user-facing "RayTraced requested but unsupported" warning;
    // DDGINode's own no-ray-query fallback, M7, will own its own).
    SceneAccelerationStructureService* EnsureSceneAccelerationStructureService();
    // Phase-B "recover" scatter (design §5-A4/§1 SCATTER B): the second
    // ScheduleUnifiedScatter of the app frame for the main view. Registers a
    // phase-B slice bound to the view's reserved phase-B visibility slice
    // (the ViewVisibilityRange with slicePhase==1 that EndFrameRG published)
    // and appends a scatter into the shared arena at a LATE pass phase so it
    // is never pulled ahead of the phase-A raster its HZB input derives from.
    // Mirrors ScheduleBucketerDispatchesForView's per-view registration.
    // Returns false when there is no phase-B reservation for the view (the
    // view opted out of HZB) or the frame state is not ready.
    bool ScheduleWorldOcclusionRecoverScatterForView(Rendering::RenderGraph::RGFrame& frame,
                                                      Rendering::ViewId viewId);

    // ── Slice-3 RenderGraph pass arms. ViewDesc.targets carries OLD-graph logical
    // ids; these arms take frame-local RGTexture VALUES instead — effective-
    // target resolution (pipeline depth/resolve redirects) moves to the
    // caller (the slice-4 pipeline nodes know their own overrides). Clear
    // config, letterbox and camera still come from ViewDesc/registries.
    // Declare the prepass BEFORE the world pass; declare the bucketer before
    // both (see GpuDrivenFrameRG order contract above). ──
    struct WorldPassTargetsRG
    {
        Rendering::RenderGraph::RGTexture Color{};   // primary (MSAA or 1-sample)
        Rendering::RenderGraph::RGTexture Depth{};   // EFFECTIVE depth (caller applied any override)
        Rendering::RenderGraph::RGTexture Resolve{}; // effective resolve; invalid = none
        Rendering::RenderGraph::RGRange ColorRange = Rendering::RenderGraph::RGRange::All();
        Rendering::RenderGraph::RGRange DepthRange = Rendering::RenderGraph::RGRange::All();
        // 1-sample R32F bound as ge_sceneDepth; invalid = falls back to Depth.
        Rendering::RenderGraph::RGTexture DepthResolved{};
        // 1-sample R32F the contact shadows march (Names::View::OccluderDepthResolved: no non-occluding
        // heads); invalid = they march DepthResolved.
        Rendering::RenderGraph::RGTexture OccluderDepthResolved{};
        // Screen-space GTAO (rgba16f: rgb world bent normal, a visibility) bound as
        // ge_gtao; invalid = falls back to the (0,0,0,1) no-occlusion default.
        Rendering::RenderGraph::RGTexture GTAO{};
        // Scene-colour grab (SceneColorGrab::GetGrabTextureRG) bound as ge_sceneColor
        // on SceneColorGrab-keyword passes; the pass declares the Read(Sampled) that
        // forms the copy->draw RAW edge. Invalid = env-cube refraction fallback.
        Rendering::RenderGraph::RGTexture SceneGrab{};
        // DDGI scaled glossy-reflection resolve outputs
        // (DDGIProbeFeature::DeclareGlossyResolveForView), bound as
        // ge_ddgiResolveRough/ge_ddgiResolveGlossy on DDGI-keyword passes.
        // Invalid = black fallback; the shader then only samples them when
        // the C0 volume UBO's uParams2.w flag is set.
        Rendering::RenderGraph::RGTexture DDGIResolveRough{};
        Rendering::RenderGraph::RGTexture DDGIResolveGlossy{};
        Rendering::RenderGraph::RGTexture DDGIResolveIrradiance{};
        // Feature-owned MRT slices (the SSR G-buffer today). The world pass
        // attaches them verbatim and carries no knowledge of what a slice
        // stores — formats, clears and locations belong to the contributing
        // provider (ReflectionsProvider). Fixed-capacity on purpose: this
        // struct is rebuilt per view per frame, so no heap. Location 0 is the
        // world color; providers use 1+.
        struct ExtraColorAttachment
        {
            uint32_t Location = 0;
            Rendering::RenderGraph::RGTexture Texture{};
            // Single-sample resolve target for a multisampled Texture; the
            // pass attaches the pair (AttachColorResolve) when valid.
            Rendering::RenderGraph::RGTexture Resolve{};
            Rendering::RenderGraph::RGAttachmentOps Ops{};
        };
        static constexpr uint32_t kMaxExtraColorAttachments = 3;
        std::array<ExtraColorAttachment, kMaxExtraColorAttachments> ExtraColor{};
        uint32_t ExtraColorCount = 0;
        void AddExtraColor(uint32_t location, Rendering::RenderGraph::RGTexture texture,
                           const Rendering::RenderGraph::RGAttachmentOps& ops,
                           Rendering::RenderGraph::RGTexture resolve = {})
        {
            assert(ExtraColorCount < kMaxExtraColorAttachments &&
                   "world pass extra color attachments exceeded");
            ExtraColor[ExtraColorCount++] = {location, texture, resolve, ops};
        }
        // True when Resolve is a pipeline redirect (ForwardPlus "SceneColor"):
        // with MSAA off the pass renders STRAIGHT into Resolve so the post-FX
        // chain never samples an uninitialized texture.
        bool ResolveFromPipeline = false;
    };
    struct WorldPassRG
    {
        Rendering::RenderGraph::RGPass Pass{};
        // What the pass ACTUALLY wrote (resolve target when a resolve or the
        // MSAA-off collapse fired, else Color) — the thumbnail/readback
        // contract that replaces GetEffectiveWorldColorResolveRef.
        Rendering::RenderGraph::RGTexture EffectiveColor{};
    };
    // Which subset of the view's entity batches a world pass draws. The scene world pass
    // peels OpaqueOnly (transmissive glass is drawn later, after the scene-colour grab);
    // the dedicated transmissive pass draws TransmissiveOnly. All = no filter (thumbnails /
    // single-material previews render glass inline via the env-cube fallback).
    enum class WorldPassDrawScope
    {
        All,
        OpaqueOnly,
        TransmissiveOnly
    };

    // `sliceCascadeIndex` selects which of the view's Color-table scatter
    // slices this pass draws: kCascadeIndexNone is the view's own camera slice
    // (every ordinary view); a fan-out consumer (reflection probe face) passes
    // the index it registered through ScheduleBucketerDispatchesForViewSlices.
    // The class axis stays the color class either way — the depth-class remap
    // only applies to Shadow-table lookups.
    WorldPassRG AddWorldPassForView(Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
                                    const WorldPassTargetsRG& targets,
                                    Rendering::MaterialKeyword passKeywords =
                                        Rendering::MaterialKeyword::None,
                                    WorldPassDrawScope scope = WorldPassDrawScope::All,
                                    uint8_t sliceCascadeIndex =
                                        Rendering::GPUDrawStreamBuilder::kCascadeIndexNone);
    // Phase-B "recover" world colour pass (two-phase HZB, design §5-A4/§1): the
    // colour twin of AddWorldDepthRecoverPassForView. Purely additive geometry
    // into the SAME targets the phase-A world pass used — LOADS colour+depth (no
    // clears), consumes SlicePhase::B scatter ranges, and does NOT set
    // WorldDeclared or publish "View.EffectiveColor" (the phase-A pass owns the
    // blackboard + post-chain). Declared by ScatterBNode AFTER the phase-A raster
    // and the phase-B depth recover; no-op when the view drew nothing.
    WorldPassRG AddWorldColorRecoverPassForView(Rendering::RenderGraph::RGFrame& frame,
                                                Rendering::ViewId viewId,
                                                const WorldPassTargetsRG& targets,
                                                Rendering::MaterialKeyword passKeywords,
                                                WorldPassDrawScope scope);
    // A compute-written buffer a forward command reads (indirect args, index or
    // vertex-stage SSBO). Declaring it on the forward pass is what orders the draw
    // after its producer — the sampledTextures span covers only textures, so
    // GPU-driven producers (CBT: indirect-draw + index + vertex buffers) pass their
    // outputs here to form the compute->graphics barrier.
    struct ForwardBufferReadRG
    {
        Rendering::RenderGraph::RGBuffer Buffer{};
        Rendering::RenderGraph::RGBufferRead Access = Rendering::RenderGraph::RGBufferRead::Storage;
    };
    // `depthAccess` binds targets.Depth: ReadWrite for contributors that own depth
    // (the ocean surface), ReadOnly for those that only test against it. ReadOnly is
    // a pass-level guarantee, not a hint — where the device exposes
    // vkCmdSetDepthWriteEnable the recorder issues it as VK_FALSE for the whole
    // pass so no contributor material's depthWriteEnable can write (without the
    // extension the guarantee rests on the pipelines' own static state), and the
    // graph records a depth READ rather than a write (no WAW edge, no store).
    Rendering::RenderGraph::RGPass AddForwardCommandPassForView(
        Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
        const WorldPassTargetsRG& targets,
        Rendering::MaterialKeyword passKeywords,
        std::span<const DrawCommand> commands,
        std::span<const Rendering::RenderGraph::RGTexture> sampledTextures = {},
        const char* passName = "ForwardCommands",
        std::span<const ForwardBufferReadRG> sampledBuffers = {},
        Rendering::RenderGraph::RGDepthAccess depthAccess =
            Rendering::RenderGraph::RGDepthAccess::ReadWrite);

    // True when any of the view's entity batches uses a Transmission-keyword material.
    // Drives whether the scene-colour grab + the dedicated transmissive pass declare.
    bool HasTransmissionInView(Rendering::ViewId viewId);

    // True when the view has transmissive geometry that also CASTS shadows — the
    // only geometry the glass-tint shadow cascade can ever draw. Distinct from
    // HasTransmissionInView on purpose: visible glass needs the colour pass, but
    // only shadow-casting glass can tint a cascade, and a non-casting instance is
    // dropped by the shadow cull long before the tint pass's indirect draw.
    bool HasTransmissiveCasterInView(Rendering::ViewId viewId) const;

    // --- Sorted transparent path (T2/S2) ---
    //
    // Order-dependent Blend materials are peeled out of the GPU-driven opaque
    // batch path and drawn back-to-front as per-(surface×blend) RUNS — one
    // DrawIndexedIndirectCount per run over a GPU-sorted indirect stream, the
    // GE_INSTANCED vertex path fetching per-record data (materialIndex,
    // sectors, custom0, flags) through the standard indirection. There is no
    // draw ceiling and no whole-view fallback: views whose visible set exceeds
    // the GPU sort capacity (kSortedTransparentSortCapacity) take a CPU-sorted
    // twin of the same indirect stream. See SortedTransparentRuns.h for the
    // run/ordering contract.

    // Build (once per view per frame) the sorted transparent record set: reads
    // the view's Blend submissions, frustum-culls against their GPUScene
    // bounding spheres (camera-relative, matching the GPU cull), and groups the
    // survivors into runs. Empty-set early-out costs nothing. Called by the
    // world node before its opaque pass so the peel sees the active flag.
    void BuildSortedTransparentForView(Rendering::ViewId viewId);
    // True when this frame's sorted transparent pass owns the view's Blend draws,
    // so the opaque batch path must peel them. Read by recordEntityBatch.
    bool IsSortedTransparentActive(Rendering::ViewId viewId) const;
    // Declare this frame's drain for the view: the fused GPU
    // gather+key+sort+scatter dispatch (or its CPU-sorted twin) plus one
    // indirect draw per run into the same HDR colour + depth the world pass
    // wrote. Called by the SortedTransparent pipeline node (after HZBBuild).
    // No-op when the view's set is inactive this frame.
    void AddSortedTransparentDrainForView(Rendering::RenderGraph::RGFrame& frame,
                                          Rendering::ViewId viewId,
                                          const WorldPassTargetsRG& targets,
                                          Rendering::MaterialKeyword passKeywords);
    // Drain composition of the view's LAST build (tests / diagnostics).
    struct SortedTransparentStats
    {
        uint32_t Collected = 0; // order-dependent Blend candidates found
        uint32_t Culled = 0;    // frustum-culled candidates
        uint32_t Records = 0;   // visible records staged for the drain
        uint32_t Runs = 0;      // per-(surface×blend) runs
        bool CpuSorted = false; // past-capacity CPU order path engaged

        bool operator==(const SortedTransparentStats&) const = default;
    };

    // The post-opaque scene-colour grab the transmissive pass refracts. Lazily created; the
    // transmissive node calls DeclareForView on it (it has the ViewDeclare) before declaring
    // its pass, and BuildPassResourcesRG binds its texture as ge_sceneColor.
    SceneColorGrab& GetOrCreateTransmissionSceneGrab();
    Rendering::RenderGraph::RGPass AddWorldDepthPrepassForView(Rendering::RenderGraph::RGFrame& frame,
                                                       Rendering::ViewId viewId,
                                                       Rendering::RenderGraph::RGTexture depth,
                                                       float clearDepthValue);
    // Phase-B "recover" prepass (design §5-A4): the two-phase HZB depth twin of
    // AddWorldDepthPrepassForView. Loads the phase-A depth instead of clearing
    // (GreaterOrEqual is the pipeline default, so newly-revealed fragments
    // depth-test against the existing surface) and consumes phase-B scatter
    // ranges. Declared AFTER the phase-A world raster — its HZB input derives
    // from that depth — so the world-already-declared tripwire that guards the
    // phase-A prepass does NOT apply here.
    Rendering::RenderGraph::RGPass AddWorldDepthRecoverPassForView(
        Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
        Rendering::RenderGraph::RGTexture depth);
    // The non-occluding prepass: the ForwardDrawDepth::PrepassNonOccluding heads (grass) drawn into the
    // view depth AFTER DepthResolve took the occluders' copy (Names::View::OccluderDepthResolved), so GTAO
    // and the contact shadows do not see them while the colour pass still depth-tests against them, and
    // DepthResolve's second copy (View.DepthResolved) has them for every other reader. Loads the camera
    // prepass's depth; draws no entity batch. Declared by DepthResolveNode right after its first copy, and
    // before the world pass (the same tripwire as the camera prepass). Declares nothing when the view
    // has no such heads this frame; a view that never declares it draws them in the camera prepass.
    Rendering::RenderGraph::RGPass AddWorldNonOccludingDepthPrepassForView(
        Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
        Rendering::RenderGraph::RGTexture depth);
    // The deforming-motion producer's separate-pass arm: the deforming subset
    // of the view's batch keys re-rasterized into the shared motion target
    // under READ-ONLY camera depth, so a closer surface simply wins the test
    // and the deformer's fragment never reaches the attachment. Declared by
    // the target's owner (ViewMotionVectors) so the clear and the mover writes
    // stay one ownership decision, and declared once per culling generation —
    // a deformer recovered in phase B is not left on the sentinel.
    // `clearTarget` is true for the owner of the clear and false for every
    // later write into the same target.
    Rendering::RenderGraph::RGPass AddDeformationMotionPassForView(
        Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
        Rendering::RenderGraph::RGTexture motionTarget,
        Rendering::RenderGraph::RGTexture depth,
        const DeformationMotionEndpoint& endpoint, bool clearTarget,
        Rendering::GPUDrawStreamBuilder::SlicePhase phase);
    // All five shadow-map pass DECLARATIONS (directional cascade + glass-tint,
    // and the area/spot/point punctual families) moved to
    // ShadowMapRenderFeature (design A1.1 S1 + S2): the feature that owns the
    // shadow textures declares the passes that write them. See
    // ShadowMapRenderFeature::Declare (and the per-family DeclareCascadePass /
    // DeclareTransmittancePass / Declare{Area,Spot,Point}Pass{,es}), driven by
    // ShadowMapNode via IRenderFeature::Declare.

    // Resolve the depth-draw executor's dependency surface into a DepthDrawServices
    // bundle for the free RecordDepthOnlyPass (DepthDrawRecorder) — THE shared
    // depth/shadow draw recorder both the world depth prepass and every shadow
    // family invoke from their pass exec lambda. Built fresh at EXEC time (inside
    // the lambda, which captures `this`/`rsPtr` by value — never the declaration
    // ctx) and consumed synchronously; never stored across frames. Public, no
    // passkey: every field is already individually reachable through a public
    // accessor, so the bundle is a convenience aggregate, not a privilege grant.
    // m_CullMode rides in the bundle so no GetCullMode() getter is added.
    DepthDrawServices MakeDepthDrawServices();

    // Exec→declare under-draw feedback for the cascade static-shadow cache
    // (see DepthUnderDrawTracker in DepthDrawRecorder.h). Created eagerly in
    // the constructor — reached from parallel-record exec workers via
    // MakeDepthDrawServices, so lazy creation would race; the tracker's own
    // mutex covers concurrent Marks within one exec.
    DepthUnderDrawTracker& DepthUnderDraw();

    // Build the per-(feature, view) declaration snapshot for
    // IRenderFeature::Declare. THE single place RenderServices spine state is
    // read for a feature: the frame-validated m_FrameRG values (IsFor guard
    // against `frame` for multi-window safety), the pending-work diagnostics,
    // the WorldDeclared tripwire, and the activation predicates. `passName`
    // forwards the node's ViewDeclare::PassName; pass an empty function in
    // granular tests that supply a literal pass name.
    FeatureDeclareContext MakeFeatureDeclareContext(
        Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
        uint32_t cascadeCount, uint32_t punctualResolution,
        const float* directionalLightDirWS,
        std::function<std::string(const char*)> passName);

    // ── Passkey-gated shadow-declaration plumbing (design A1.1 amend. 3) ──
    // Reachable only by a holder of a ShadowDeclareSeam — which only a
    // FeatureDeclareContext carries, and only MakeFeatureDeclareContext mints.
    // A feature declaring a shadow pass calls these via the RenderServices& it
    // receives in Declare; nothing else can, and there is no friend on this class.

    // Build the keyword-less, target-less resolved binding table a depth/shadow
    // pass needs (the narrow subset of the private BuildPassResourcesRG).
    ResolvedPassResources BuildShadowPassResources(ShadowDeclareSeam,
                                                   Rendering::RenderGraph::RGFrame& frame,
                                                   Rendering::ViewId viewId,
                                                   Rendering::BufferHandle camBuf,
                                                   uint64_t camOffset);
    // Single import point for the POOL-owned shadow depth array
    // ("ShadowMapArray.View<N>"): idempotent per (view, frame) via ViewFrameRG,
    // adopts the pooled physical into ShadowMapRenderFeature. PRODUCER arms
    // only. Invalid when the feature is absent or uninitialized.
    Rendering::RenderGraph::RGTexture ImportShadowMapArrayRG(
        Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId, ShadowDeclareSeam);
    // Pool-owned per-cascade glass-tint colour array ("GlassShadowTint.View<N>"),
    // parallel to the depth array; created only when the view has transmissive
    // casters. PRODUCER arms only.
    Rendering::RenderGraph::RGTexture ImportTransmittanceShadowArrayRG(
        Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId, ShadowDeclareSeam);

    // Publish a punctual (area/spot/point) family's per-view map + data upload
    // into ViewFrameRG so the world binding table's BuildPassResourcesRG reads
    // them (bound by name "AreaShadowData" / "SpotShadowData" / "PointShadowData",
    // and the map sampled via Get{Area,Spot,Point}ShadowSampler). WRITE-ONLY: the
    // punctual DeclarePass owns creation (the ViewFrameRG entry is frame-local and
    // the old dedup branch is always-taken within one declaration), so the feature
    // never reads these back. PRODUCER arms only.
    void PublishAreaShadow(ShadowDeclareSeam, Rendering::RenderGraph::RGFrame& frame,
                           Rendering::ViewId viewId, Rendering::RenderGraph::RGTexture map,
                           Rendering::RenderGraph::RGFrame::TypedUpload<AreaShadowDataGPU> data);
    void PublishSpotShadow(ShadowDeclareSeam, Rendering::RenderGraph::RGFrame& frame,
                           Rendering::ViewId viewId, Rendering::RenderGraph::RGTexture map,
                           Rendering::RenderGraph::RGFrame::TypedUpload<SpotShadowDataGPU> data);
    // M1: the point-shadow map is the shared 2D-array atlas and the data is a
    // std430 SSBO ARRAY of PointShadowSlotGPU (variable length), so this takes the
    // slot buffer as handle+offset+bytes rather than a single TypedUpload.
    void PublishPointShadow(ShadowDeclareSeam, Rendering::RenderGraph::RGFrame& frame,
                            Rendering::ViewId viewId, Rendering::RenderGraph::RGTexture map,
                            Rendering::BufferHandle slotBuffer, uint64_t slotBufferOffset,
                            uint64_t slotBufferBytes);
    // Publish the PCSS min/max pyramid (ShadowMinMaxPyramid) so the world arms
    // declare the sampled read on it. The shader reaches the pyramid through a
    // BINDLESS index, which forms no producer->consumer edge at all: without the
    // declared read the reduction's storage writes and the fragment's fetches
    // have no memory dependency between them. PRODUCER arms only.
    void PublishPcssPyramid(ShadowDeclareSeam, Rendering::RenderGraph::RGFrame& frame,
                            Rendering::ViewId viewId, Rendering::RenderGraph::RGTexture pyramid);

    // Max simultaneously-shadowed point lights per view (design §5 product call;
    // default kDefaultPointShadowBudget). Set by ShadowMapNode from its config.
    void SetPointShadowBudget(uint32_t budget);
    uint32_t PointShadowBudget() const { return m_PointShadowBudget; }

    // Per-light atlas slot for this frame's view, indexed by packed clusterable
    // index (== the receiver shader's light index) — the LightUploadNode writes it
    // into each GPULightPacked so the receiver can index ge_pointShadowSlots.
    // Entries are -1 for unshadowed lights. Triggers the (idempotent) per-frame
    // assignment if no other consumer computed it yet this view/frame.
    std::span<const int32_t> PointShadowSlotOfCluster(Rendering::ViewId viewId, uint64 worldId,
                                                      const Rendering::CameraData& camData);

    // The material stack (registry, compiler, shader cache, variant cache,
    // prewarm, build context, MaterialParams SSBO packing, classification maps,
    // hot-reload invalidator, binder). Owned by value; valid between
    // Initialize() and Shutdown(). See MaterialSystem.h.
    MaterialSystem& Materials() { return m_MaterialSystem; }
    const MaterialSystem& Materials() const { return m_MaterialSystem; }

    // Skin palette atlas: shared SSBO for all bone palettes (instanced skinned draws).
    SkinPaletteAtlas& GetSkinPaletteAtlas() { return m_SkinPaletteAtlas; }
    const SkinPaletteAtlas& GetSkinPaletteAtlas() const { return m_SkinPaletteAtlas; }

    // GPU animation data store for compute skinning (Tier 2).
    GPUAnimationDataStore& GetGPUAnimationDataStore() { return m_GPUAnimDataStore; }
    const GPUAnimationDataStore& GetGPUAnimationDataStore() const { return m_GPUAnimDataStore; }
    bool IsComputeSkinningReady() const { return m_AnimComputePass != nullptr; }

    // Per-frame write pool: double-buffered allocators for CPU-written SSBOs
    // (material params, bone palettes). Used by the world draw builder and
    // extraction systems to write transient per-frame data without
    // per-mesh/per-draw buffer allocations.
    PerFrameWritePool& GetPerFrameWritePool() { return m_PerFrameWritePool; }
    const PerFrameWritePool& GetPerFrameWritePool() const { return m_PerFrameWritePool; }

    // Camera + view registries live in ViewRegistry (A1.2). RenderServices owns
    // it by value; reach it via Views().
    ViewRegistry& Views() { return m_ViewRegistry; }
    const ViewRegistry& Views() const { return m_ViewRegistry; }

    // Per-view temporal history (unjittered camera and deformation clock per
    // rendered frame) for temporal effects (SSSR reprojection, motion vectors).
    // Lives beside the registry, not in it: it exists for every AA mode and is
    // consumed only by the pipeline nodes that reproject.
    ViewTemporalHistory& TemporalHistory() { return m_ViewTemporalHistory; }

    // The frame spine (A1.4): per-window RenderGraph stream slots, app-frame
    // epochs, pipeline-asset lifecycle, and BuildFrameGraph itself. Owned by
    // value; valid between Initialize() and Shutdown(). See FrameOrchestrator.h.
    FrameOrchestrator& Spine() { return m_FrameOrchestrator; }
    const FrameOrchestrator& Spine() const { return m_FrameOrchestrator; }

    // The pipeline-asset surface (SetActiveRenderPipelinePath/Blueprint,
    // GetPipelineNodeRegistry, RegisterPipelineNodeType, shader-package preload)
    // and the BuildFrameGraph spine moved to FrameOrchestrator (A1.4); reach them
    // via Spine().
    //
    // FrameGraphBuildParamsRG keeps its RenderServices::FrameGraphBuildParamsRG
    // spelling at the frame drivers / tests through this alias (§0a-A3); the
    // definition lives on FrameOrchestrator with the spine.
    using FrameGraphBuildParamsRG = FrameOrchestrator::FrameGraphBuildParamsRG;

    // The view's FinalColor for the CURRENTLY DECLARED frame — the F4
    // consumer contract. Valid only after BuildFrameGraph(frame, ...) this
    // epoch (frame-identity-guarded). Pool-backed (never a transient) and
    // marked external at ShaderReadOnly by the spine, so the physical is
    // sampleable next frame too. Editor UI slots and the Player blit
    // re-point HERE per frame; under FinalCopy elision the physical varies
    // with PP toggles, so consumers must not cache it across frames.
    struct PipelineOutputRG
    {
        Rendering::RenderGraph::RGTexture Out{};
        Rendering::TextureHandle Physical{};
        bool IsValid() const { return Out.IsValid() && Physical.IsValid(); }
    };
    PipelineOutputRG GetPipelineOutputRG(Rendering::RenderGraph::RGFrame& frame,
                                         Rendering::ViewId viewId) const;
    // Space of the pixels the pipeline wrote into FinalColor this frame — the
    // producer's stamp (#767): the per-view Tonemap emits display-referred
    // linear under SDR output and paper-white-relative linear under HDR
    // output, and the mode is frame-stable, so this states what the pipeline
    // actually wrote for the frame being declared. Consumers carry the value
    // with the output they sampled (or store it with a frozen copy) instead
    // of deriving a space from the display at bind time. UI::UITextureSpace
    // stays forward-declared here — modules below the UI layer include this
    // header; callers of this accessor include UI/UITextureSpace.h.
    ::GameEngine::UI::UITextureSpace GetPipelineOutputSpaceRG() const;

    // Consume-only view of the cascade arm's pooled shadow array for THIS
    // frame: the SAME resource id the producer imported/attached, or invalid
    // when no cascade producer declared into `frame` for this view. NEVER
    // creates the array — ImportShadowMapArrayRG is producer-arms-only.
    // Consumers: fog's lighting froxels, terrain's debug pass, MSM.
    Rendering::RenderGraph::RGTexture GetShadowMapArrayRG(Rendering::RenderGraph::RGFrame& frame,
                                                  Rendering::ViewId viewId) const;

    // Read-only inputs for this frame's spot/point shadows.
    // Maps retain the producer's graph identity for resource barriers;
    // upload ranges retain the exact point-slot count for shader bounds checks.
    struct LocalShadowInputsRG
    {
        Rendering::RenderGraph::RGTexture SpotMap{}, PointMap{};
        Rendering::BufferHandle SpotData{}, PointData{};
        uint64_t SpotOffset = 0, PointOffset = 0, PointBytes = 0;
    };
    LocalShadowInputsRG GetLocalShadowInputsRG(Rendering::RenderGraph::RGFrame& frame,
                                               Rendering::ViewId viewId) const;

    // The declare sequence of the spine currently being declared — nodes pass
    // it to the feature-side Acquire APIs that queue CPU-readback pendings.
    uint64_t RGDeclareSeq() const { return m_FrameOrchestrator.RGDeclareSeq(); }

    // Small zero-filled buffer bound as a placeholder for any reflected set0
    // storage/uniform-buffer binding a render-graph node fails to wire. Keeps
    // the descriptor valid (no unbound-descriptor VUID on devices without
    // nullDescriptor) and reads predictable zeros. Sized for small UBO/SSBO
    // ranges only; large/unbounded SSBOs are always explicitly provided.
    Rendering::BufferHandle GetDefaultPlaceholderBuffer() const { return m_DefaultPlaceholderBuffer; }

    Rendering::TextureFormat GetDepthFormat() const { return m_DepthFormat; }

    // MSAA policy (engine-wide default for views that allocate MSAA targets).
    // The project's rendering.msaa drives it through
    // AntiAliasingProjectSettings; per-camera Camera::MSAASamples overrides it
    // per view.
    uint32_t GetDefaultMSAASampleCount() const { return m_AA.MsaaSamples; }
    // This device's inputs to ResolveDefaultAntiAliasing (AntiAliasing.h);
    // all-defaults, which that ladder reads as "no device answered", when there
    // is no device. MSAA stays opt-in regardless: the sample count only applies
    // where the AA mode resolves to MSAA (ResolveAntiAliasing).
    AntiAliasingDeviceCaps GetAntiAliasingDeviceCaps() const;
    // Sets the engine-wide default MSAA sample count (1 disables MSAA); also the
    // fallback for cameras whose Camera::MSAASamples is 0 (Default). The change
    // re-specs targets IMPLICITLY on the next frame: the view controllers re-declare
    // their persistent color/depth every frame reading this value, and the
    // render-graph resource pool reallocs a target whenever its desc's sampleCount
    // differs, deferring the old physical until in-flight frames retire — so the
    // setter itself neither drains nor destroys anything, and no in-flight race is
    // possible. World-pass pipeline variants are keyed to the bound attachment's
    // sample count, so the matching PSO is fetched on the re-specced frame.
    // Shadow-map textures are unaffected: they are depth-only and always
    // single-sample. Out-of-line to keep the clamp helpers in the .cpp.
    void SetDefaultMSAASampleCount(uint32_t samples);

    // Anti-aliasing mode (engine-wide default; per-camera Camera::AntiAliasing
    // overrides it). MSAA and TAA are mutually exclusive by construction: the
    // MSAA sample count above is honored only when the resolved mode is MSAA
    // (ResolveAntiAliasing), and jitter + history resolve run only under TAA.
    // Takes effect like the MSAA setter: view controllers re-resolve every
    // frame, so the change re-specs targets implicitly on the next frame.
    AntiAliasingMode GetDefaultAntiAliasingMode() const { return m_AA.Mode; }
    void SetDefaultAntiAliasingMode(AntiAliasingMode mode) { m_AA.Mode = mode; }
    // FXAA spatial-pass quality (AntiAliasing.h). Picks the pass-1 pipeline
    // only — no target re-spec, takes effect next frame.
    FxaaQuality GetFxaaQuality() const { return m_AA.Fxaa; }
    void SetFxaaQuality(FxaaQuality quality) { m_AA.Fxaa = quality; }
    // TAA jitter cycle length (8 default, 16 selectable via GE_TAA_SAMPLES).
    uint32_t GetTaaSequenceLength() const { return m_AA.TaaSequenceLength; }

    // Engine-default internal render scale: the pipeline renders the world half
    // at scale x display extent and crosses back to the display extent once,
    // before the post chain's output-basis half. Under TAA that crossing is the
    // temporal resolve (TAAU); otherwise it is the RenderScaleUpscale node.
    // Views with a ViewRegistry override ignore this value — resolve with
    // ResolveViewRenderScale, never read the default directly. 1.0 = exactly
    // the unsplit path (no scaled targets, every crossing elided) — that
    // identity is the byte-neutrality gate. Clamped to
    // [kMinRenderScale, kMaxRenderScale]; above 1.0 the split supersamples
    // (SSAA) and the crossing downsamples.
    // Takes effect next frame like the AA mode, including the material texture
    // mip bias (a per-view ViewParams uniform derived from the actual extent
    // ratio by ViewParamsUploadNode — live, never baked into samplers).
    float GetDefaultRenderScale() const { return m_DefaultRenderScale; }
    void SetDefaultRenderScale(float scale);
    // The scale the pipeline actually applies to `viewId`: its ViewRegistry
    // override when set, else the engine default.
    float ResolveViewRenderScale(Rendering::ViewId viewId) const;

    // Which producer arm writes the deforming surfaces into the shared motion
    // target this run. Resolved at initialization from the three switches; the
    // setter is what resolution calls, and what lets a test fixture render one
    // arm without mutating the process environment. None means the deformer
    // lane records nothing at all, which is the default and the state in which
    // this frame is the frame it was before the producer existed.
    DeformationMotionArm GetDeformationMotionArm() const { return m_DeformationMotionArm; }
    void SetDeformationMotionArm(DeformationMotionArm arm) { m_DeformationMotionArm = arm; }

    // Dynamic resolution scaling. Off pins the scale to 1.0 (Native) — even
    // when the mode is already Off, so a startup-applied persisted scale
    // cannot survive into Native. Fixed leaves the scale exactly where the
    // setter above put it; Dynamic hands it to DynamicResolutionController,
    // which steers it to hold a GPU-time target. Default Off — DRS is opt-in.
    DynamicResolutionMode GetDynamicResolutionMode() const { return m_DrsMode; }
    void SetDynamicResolutionMode(DynamicResolutionMode mode);
    const DynamicResolutionConfig& GetDynamicResolutionConfig() const;
    void SetDynamicResolutionConfig(const DynamicResolutionConfig& config);
    const DynamicResolutionController::Stats& GetDynamicResolutionStats() const;

    // Feed one frame of GPU cost to the controller and apply its verdict.
    // No-op outside Dynamic mode. `appFrameEpoch` makes the tick idempotent
    // per APP frame: every window's spine declares on the main RenderServices
    // with its own per-window RGFrame counter, so the key must be the spine's
    // world-frame epoch (FrameOrchestrator::WorldFrameEpoch) — a per-window
    // counter lets N windows tick dwell/slew/EMA N times a frame.
    void UpdateDynamicResolution(const DynamicResolutionSample& sample, float deltaSeconds,
                                 uint64_t appFrameEpoch);

    // Blend the engine default with a camera's override fields
    // (Camera::AntiAliasing, Camera::MSAASamples — pass 0/0 for views without
    // a Camera component, e.g. the editor scene view). Returns the effective
    // mode and the sample count the view's color target should use.
    ResolvedAntiAliasing ResolveAntiAliasing(uint32_t cameraAAMode,
                                             uint32_t cameraMsaaSamples) const;

    // World draw builder API
    //
    // These helpers are thin wrappers around an internal WorldDrawBuilder
    // instance. Systems submit WorldSubmissionRecord batches, and the
    // builder derives per-view unique batch keys that the bucketer
    // scheduler and world / depth-pass execute lambdas both consume to
    // issue one indirect draw per (Material, mesh) batch.
    WorldDrawBuilder& GetWorldDrawBuilder();

    void BeginWorldDrawFrame();

    // Model GUIDs whose GPU geometry the last invalidation drain refreshed IN
    // PLACE — the reload path that keeps live MeshGPUHandles valid and so
    // leaves every LocalBounds derived from those meshes describing the old
    // geometry. Empty on a frame with no such reload.
    //
    // Draining accessor: the caller takes the list and clears it, so the ECS
    // repair runs exactly once per reload. The drain that fills it runs inside
    // BeginWorldDrawFrame, so the world-owning caller reads this immediately
    // after that call and ahead of the ECS schedule — then no wave can observe
    // a refreshed GPUMesh row against a stale LocalBounds. Same thread affinity
    // as the drain; carries no synchronization of its own.
    std::vector<GUID> TakeModelsReloadedInPlace();

    // Every model GUID the last invalidation drain handled, whichever way it
    // healed the GPU mirror (refreshed in place or released): state derived
    // from the model's asset, such as the resolve service's kept registrations,
    // is stale. Draining accessor with the same timing and thread affinity as
    // TakeModelsReloadedInPlace.
    std::vector<GUID> TakeModelsInvalidated();

    // Instances that moved THIS frame in a way the motion-vector pass must
    // rasterize: a nonzero object motion vector (prevTransform != transform),
    // an active skin palette (the pose deforms even when the root is still),
    // or both. Extraction appends during its serial apply/patch phases
    // (mutex-guarded for multi-world safety); the TAA movers motion-vector
    // pass drains the list at declare time. Cleared in BeginWorldDrawFrame.
    //
    // Palette offsets are captured here rather than re-read at declare time
    // because the producing sites already hold the SkeletonStore runtime, and
    // the previous offset is only meaningful paired with the frame that
    // published it. SkinPaletteOffset == 0 means "not skinned this frame"
    // (atlas slot 0 is the identity block) and selects the rigid PSO.
    //
    // PrevSkinPaletteOffset == kNoPreviousSkinPalette means this instance had
    // no pose last frame (spawn, animation start, no previous atlas ring). It
    // needs its own value because steady-state offsets are frame-STABLE, so
    // Previous == Current is the normal case and cannot also carry "no
    // history" — a consumer that conflated the two would sample last frame's
    // atlas at an offset another runtime owned that frame.
    static constexpr uint32_t kNoPreviousSkinPalette = ~0u;

    struct FrameMoverRecord
    {
        uint32_t InstanceIndex;
        Rendering::MeshGPUHandle MeshHandle;
        uint32_t SkinPaletteOffset;
        uint32_t PrevSkinPaletteOffset;
    };
    void AddFrameMover(uint32_t instanceIndex, Rendering::MeshGPUHandle meshHandle,
                       uint32_t skinPaletteOffset, uint32_t prevSkinPaletteOffset);
    // Appends a bulk-motion batch under one lock, in span order: the same list
    // AddFrameMover per record would build.
    void AddFrameMovers(std::span<const FrameMoverRecord> movers);
    // Declare-time only (after extraction completed for the frame).
    std::span<const FrameMoverRecord> GetFrameMovers() const { return m_FrameMovers; }
    // Content that changes a view's pixels this frame without moving an
    // instance or advancing any content epoch (moving particles). Extraction
    // reports it per view; TAA then does not certify that view stationary.
    // Cleared by BeginWorldDrawFrame like the movers list.
    void NotifyUnversionedMotion(Rendering::ViewId viewId);
    // Declare-time only. True for a view reported this frame or the previous
    // one: the frame after the last report still revokes, so the stale copy of
    // the last moving content is clipped instead of fading out of history.
    bool HasUnversionedMotion(Rendering::ViewId viewId) const;

    // Animated-material clocks, both derived per call from the process-global
    // Time::GetCumulativeSeconds() rather than stored — so they survive this
    // RenderServices being destroyed and recreated (device-lost recovery, window
    // rebuild) without snapping animation phase back to zero. The unbounded value
    // (uTimeParams.x) drives phase-continuous effects read from a fragment stage;
    // the scroll value (uTimeParams.y) wraps every kScrollAnimationPeriodSeconds so
    // tile-periodic UV panners (water flow/falls) keep fp32 sub-frame precision.
    // A vertex modifier reads neither: its endpoint carries the rebased
    // deformation lanes (uTimeParams.zw, ViewTemporalHistory).
    float GetShaderAnimationTimeSeconds() const;
    float GetScrollAnimationTimeSeconds() const;
    void BuildWorldBatchKeys();
    void EmitProducerForwardCommandsForView(
        Rendering::ViewId viewId,
        ForwardEmitPurpose purpose = ForwardEmitPurpose::World);
    // While true, the next world pass(es) declared bind a zero-intensity
    // EnvData so the forward IBL contributes no environment radiance. The
    // reflection-probe capture sets this around its own face passes (with
    // CaptureEnvironment off) so it never samples the cube it is baking. Set
    // synchronously around AddWorldPassForView and reset immediately after;
    // BuildPassResourcesRG reads it at declare time.
    void SetWorldPassExcludeEnvironment(bool exclude) { m_WorldPassExcludeEnvironment = exclude; }
    // Re-runs every registered depth-emit callback for the view's light-space
    // passes (cascades, area, spot, point), after clearing those streams.
    // ShadowMapNode calls it once the frame's cascades are cached. The camera
    // prepass stream is left alone: its heads come from the forward producers.
    void EmitProducerDepthCommandsForView(Rendering::ViewId viewId);
    // Build batch keys for a single view only (no global material/pipeline switching).
    void BuildWorldBatchKeysForView(Rendering::ViewId viewId);
    void SubmitWorldSubmissions(std::span<const WorldSubmissionRecord> records);

    // Per-sub-phase wall-clock timing published once per frame by
    // RenderExtractionSystem::Update. Diagnostic only (benign racy read from
    // the debug server); drives the A2.1 measure-gated parallelization
    // decision via get_render_stats. Last-frame values plus a short windowed
    // mean/max so a single poll is stable under the bench's low FPS.
    struct RenderExtractionStats
    {
        // Wall-clock milliseconds for the last extraction tick.
        double TotalMs = 0.0;
        double LightsMs = 0.0;  // light-extraction query
        double GatherMs = 0.0;  // world-mesh renderable records-gather query
        double ProcessMs = 0.0; // per-record rebuild + GPUScene update + submission build
        // A2.1: PROCESS split into a parallel prepare (pure per-record
        // compute over m_Records chunks) and a serial apply (GPUScene mutation
        // + emission merge). ProcessMs == the sum. Both read 0 on fast frames.
        double PrepareMs = 0.0;
        double ApplyMs = 0.0;
        double SubmitMs = 0.0;  // SubmitWorldSubmissions batch

        // Windowed aggregates over the last kRenderExtractionStatsWindow ticks.
        double TotalMsMean = 0.0;
        double GatherMsMean = 0.0;
        double ProcessMsMean = 0.0;
        double PrepareMsMean = 0.0; // A2.1 parallel prepare (0 on fast frames)
        double ApplyMsMean = 0.0;   // A2.1 serial apply (0 on fast frames)
        double TotalMsMax = 0.0;

        // Last-frame counters (context for the timing splits).
        // On GE_EXTRACTION_FEED fast frames RecordCount/RebuildCount/
        // SkippedCount describe the LAST FULL PASS, not a per-frame walk —
        // read FastFrame/FeedPatchedCount for what the tick actually did.
        uint32_t RecordCount = 0;     // renderable records gathered
        uint32_t SubmissionCount = 0; // per-view submissions produced
        uint32_t RebuildCount = 0;    // records that MISSED the dirty-skip (rebuilt + uploaded)
        uint32_t SkippedCount = 0;    // records that HIT the dirty-skip

        // GE_EXTRACTION_FEED lane telemetry (fusion S2b, design D7). Reason
        // bit values live in RenderExtractionSystem::EscalationReason; E9
        // (view-set) is its own bit so OnDemand view churn (probe rebakes,
        // mirrors) is attributable without misreading it as an escalation
        // storm.
        uint32_t FastFrame = 0;            // 1 = this tick took the dirty-feed fast path
        uint32_t FastFramesInWindow = 0;   // fast ticks within the stats window
        uint32_t WindowTicks = 0;          // ticks currently in the stats window
        uint32_t EscalationReasonBits = 0; // why the full lane ran (0 on fast ticks)
        uint32_t FeedPatchedCount = 0;     // dirty entities patched this fast tick
        uint32_t SubsetRefreshedCount = 0; // always-refresh entities re-prepared this fast tick
        // Pending-transient breakdown at the last full pass (E8). Per-shape so
        // a permanently pinned full lane is attributable in one poll — a
        // never-resolving material GUID shows up as PendingMaterialLoad.
        uint32_t PendingMeshEntry = 0;     // mesh registry entry absent (streaming)
        uint32_t PendingMaterialLoad = 0;  // material GUID set but not registered yet
        uint32_t PendingPipeline = 0;      // material pipeline still compiling
        uint32_t PendingSentinelIndex = 0; // mesh/material GPU index still the ~0u sentinel
        // Scene-composition counts (the S0 residual-attribution assertion:
        // "residual ~= CLEANUP" holds only for particle/volume/ocean-free
        // compositions — bench post-processing asserts these are zero before
        // attributing the untimed residual to CLEANUP).
        uint32_t ParticleEmitterCount = 0;
        // Particles alive after the frame's simulation tick, and that tick's
        // wall time in milliseconds.
        uint32_t LiveParticles = 0;
        float ParticleSimulationMs = 0.0f;
        uint32_t VolumeCount = 0;
        uint32_t OceanCount = 0;
    };

    void SetRenderExtractionStats(const RenderExtractionStats& stats) { m_RenderExtractionStats = stats; }
    const RenderExtractionStats& GetRenderExtractionStats() const { return m_RenderExtractionStats; }

    // A2 STEP-0: render-thread CPU-timeline brackets owned by RenderServices —
    // the per-view batch-key/sort work (BuildWorldBatchKeys → DeriveBatchKeysForView)
    // and the bucketer-dispatch scheduling (ScheduleWorldBucketerDispatches). The
    // RenderGraph phases (compile/schedule/barrier/submit) live in RGFrame::FrameStats;
    // get_render_stats stitches both plus RenderExtractionStats into one blob.
    // Same last-frame + short windowed-mean shape as RenderExtractionStats so a
    // single poll is stable under the bench's low FPS. Written by BuildWorldBatchKeys
    // (this) and the FrameOrchestrator spine (friend); read by the debug server.
    struct RenderTimelineStats
    {
        // Last app frame.
        double SortMs = 0.0;          // batch-key derivation + std::sort across views
        uint32_t SortViewCount = 0;   // views (re)derived this frame
        double BucketerScheduleMs = 0.0; // ScheduleWorldBucketerDispatches

        // Windowed means (FrameOrchestrator maintains the ring once per app frame).
        double SortMsMean = 0.0;
        double BucketerScheduleMsMean = 0.0;
    };
    const RenderTimelineStats& GetRenderTimelineStats() const { return m_RenderTimelineStats; }

    // Per-view ECS-derived batch keys (produced by WorldDrawBuilder). The
    // bucketer dispatch scheduler and the world / depth-pass execute
    // lambdas iterate this set to issue one indirect draw per batch.
    std::span<const WorldDrawBuilder::BatchKey> GetEntityBatchKeys(Rendering::ViewId viewId) const;

    // gpu mesh index -> pool group, the mesh axis those batch keys' draw
    // ranges were published under. Record-time read for the draw consumers (no
    // refresh); empty when draw consolidation is off, in which case the mesh
    // axis stays the meshIndex. Feed it to ResolveDrawStreamLookupKey rather
    // than indexing it directly.
    std::span<const uint32_t> MeshPoolGroupSpanForDraws() const;

    struct MeshDrawDiagnostic
    {
        std::string pass;
        std::string reason;
        uint32_t viewId = 0;
        uint32_t materialIndex = 0;
        uint32_t meshIndex = 0;
        uint32_t maxDrawCount = 0;
        size_t entityKeyCount = 0;
        size_t commandCount = 0;
        size_t slotCount = 0;
    };

    std::vector<MeshDrawDiagnostic> GetRecentMeshDrawDiagnostics() const;

    // Submit a light for a specific world. Accumulated per-frame; cleared at the
    // start of each frame by BeginWorldDrawFrame(). Called by RenderExtractionSystem
    // for ECS worlds and directly by isolated renderers (e.g. thumbnails).
    void SubmitLight(uint64 worldId, const ExtractedLight& light);

    // Sort a world's submitted lights strongest-first (view-independent
    // contribution), deterministic tie-break by SortId. Single source of truth:
    // both the LightBuffer upload packing and the shadow-index builders read the
    // sorted list, so the 1024-light global cap and the per-cluster cap drop the
    // least significant lights the same way every frame (no dense-scene flicker).
    // Call once after all SubmitLight calls for the world, before any consumer.
    void FinalizeWorldLights(uint64 worldId);

    // Returns all lights submitted this frame for a given world. Empty span if none.
    std::span<const ExtractedLight> GetWorldLights(uint64 worldId) const;

    // Submit resolved post-process settings for a world (called by extraction system).
    void SetWorldPostProcessSettings(uint64 worldId, const PostProcessSettings& settings);
    // Returns the resolved post-process settings for a world, or defaults if none submitted.
    const PostProcessSettings& GetWorldPostProcessSettings(uint64 worldId) const;

    // Submit resolved directional-shadow overrides for a world (called by the
    // extraction system from the dominant PostProcessVolume's ShadowSettingsEffect).
    void SetWorldShadowSettings(uint64 worldId, const ResolvedShadowSettings& settings);
    // Returns the resolved shadow overrides for a world, or a HasOverride==false
    // default when no volume overrides shadows there (ShadowMapNode then keeps its
    // blueprint values).
    const ResolvedShadowSettings& GetWorldShadowSettings(uint64 worldId) const;

    // Digest of a world's renderable MEMBERSHIP (mesh, material, flags per
    // extracted instance — not transforms), published by RenderExtractionSystem
    // each frame. Reflection probes fold it into their bake digest so a "Once"
    // probe recaptures when scene content appears, disappears, or changes
    // appearance, not merely when the sky or the probe itself changes.
    void SetWorldRenderContentDigest(uint64 worldId, uint64 digest);
    // Returns the world's last published content digest (0 = never published).
    uint64 GetWorldRenderContentDigest(uint64 worldId) const;

    // Blends the registry override (if any) with the world fallback — stays on
    // RenderServices because both halves are reachable only here.
    const PostProcessSettings& GetEffectivePostProcessSettings(Rendering::ViewId viewId, uint64 worldId) const;

    void SetWorldVolumetricFogVolumes(uint64 worldId, std::vector<VolumetricFogLocalVolume> volumes);
    std::span<const VolumetricFogLocalVolume> GetWorldVolumetricFogVolumes(uint64 worldId) const;
    std::span<const VolumetricFogLocalVolume> GetEffectiveVolumetricFogVolumes(Rendering::ViewId viewId, uint64 worldId) const;

    // Write a per-view light buffer from the view's world light list, allocating
    // the buffer on first call. The buffer is bound as LightUBO for that view's
    // draw pass instead of the global fallback, so non-Forward+ views (e.g.
    // thumbnails) get proper per-view directional lighting.
    // Call this after all SubmitLight calls for the view's world, before AddWorldPassForView.
    //
    // The ONLY writer of that buffer, and it must stay that way: the buffer is
    // one slot of a per-frame ring indexed by IDevice::GetFrameIndex(), so a
    // host write is safe only inside an acquired frame, where BeginFrame has
    // waited the fence proving the frame that last used this slot is done. A
    // view that renders calls this on every frame it renders, from its declare
    // path; a second writer outside that window races a frame in flight.
    void WriteViewLightBuffer(Rendering::ViewId viewId);

    Rendering::SamplerHandle GetAreaShadowSampler();
    Rendering::SamplerHandle GetAreaShadowRawSampler();
    Rendering::SamplerHandle GetSpotShadowSampler();
    Rendering::SamplerHandle GetPointShadowSampler();
    // Comparison sampler for ge_shadowMapArray. ShadowMapRenderFeature owns an
    // identical one, but the Shadows keyword is blueprint-driven and does not
    // require that feature to exist, so the world binding table needs a source
    // that is always available — this one.
    Rendering::SamplerHandle GetCascadeShadowSampler();

    // 1x1 kMaxShadowCascades-layer D32 array, every texel at reverse-Z far, for
    // binding ge_shadowMapArray on frames with no live cascade array. Every
    // consumer of that GLSL name needs it: the world binding table and the
    // volumetric-fog lighting dispatch, which builds its own descriptor set.
    Rendering::TextureHandle GetCascadeShadowFallbackTexture() const
    {
        return m_CascadeShadowFallbackTexture;
    }

    // 1x1 D32 at reverse-Z far, bound for ge_areaShadowMap, ge_areaShadowMapRaw
    // and ge_spotShadowMap when a view has no area/spot caster — and as the
    // ge_sceneDepth fallback on every pass that resolves no view depth.
    Rendering::TextureHandle GetAreaShadowFallbackTexture() const
    {
        return m_AreaShadowFallbackTexture;
    }

    // 1x1 six-layer D32 array, every texel at reverse-Z far, for binding
    // ge_pointShadowMap when a view published no point-shadow atlas.
    Rendering::TextureHandle GetPointShadowFallbackTexture() const
    {
        return m_PointShadowFallbackTexture;
    }

    // Register a material and dispatch its variant prewarm in one call. This is
    // the one material entry point that stays on RenderServices rather than
    // rs.Materials(): it resolves the world-pass keyword set from the active
    // blueprint + per-view state (ResolveWorldPassKeywordsForPrewarm — both RS
    // concerns) and passes the result into the facade's prewarm, so the material
    // facade never reaches back into blueprint/view state. Plain register,
    // recompile, base-shader prewarm, and variant prewarm all live directly on
    // rs.Materials(). additionalKeywords are injected before pipeline compilation
    // (e.g. ForwardPlus | Shadows for forward contributors in the Forward+ pass).
    // Returns the registered Material*, or nullptr on failure.
    Material* RegisterAndPrewarmMaterial(const GUID& guid,
                                         const MaterialDocument& doc,
                                         Rendering::MaterialKeyword additionalKeywords =
                                             Rendering::MaterialKeyword::None);

    // Typed render feature registry. Features are created on demand and owned
    // by RenderServices until Shutdown(). GPU resource lifetime is guaranteed
    // because m_Features is cleared before device teardown.
    //
    // Callable from any thread. ECS systems run wave-parallel on JobSystem
    // workers (SystemManager dispatches every system in a wave concurrently),
    // and same-wave systems reach this registry — so lookup and insert are
    // guarded by m_FeaturesMutex. The map is read-mostly-then-frozen: a given
    // type inserts exactly once, on the frame it is first touched, and every
    // later call is a pure lookup. Hence a shared_mutex, so the concurrent
    // lookups that dominate do not serialize against each other.
    //
    // Feature objects themselves are NOT synchronized. Concurrent callers of
    // EnsureFeature<T> for the SAME T must still coordinate their own writes
    // to the returned feature; the guarantee here is only that the registry
    // hands every caller the one canonical instance without corrupting itself.
    template<typename T, typename... Args>
    T& EnsureFeature(Args&&... args)
    {
        static_assert(std::is_base_of_v<IRenderFeature, T>);
        const auto key = std::type_index(typeid(T));
        {
            std::shared_lock lock(m_FeaturesMutex);
            auto it = m_Features.find(key);
            if (it != m_Features.end())
                return *static_cast<T*>(it->second.get());
        }
        // Construct with no lock held: T's constructor is caller code and may
        // reach back into RenderServices (EnsureFeature included), which would
        // self-deadlock on the non-recursive mutex. The cost of building it
        // outside is that two threads missing on the same T simultaneously
        // both construct — resolved by the re-check below, which publishes the
        // first arrival and destroys the loser. That can only happen on the
        // one frame T is first touched, and feature constructors are member
        // init only (GPU resources come later, via Initialize(device)).
        auto feat = std::make_unique<T>(std::forward<Args>(args)...);
        std::unique_lock lock(m_FeaturesMutex);
        // try_emplace leaves feat untouched when the key is already present, so
        // the loser's instance dies with this scope and never reaches a caller.
        auto [it, inserted] = m_Features.try_emplace(key, std::move(feat));
        if (inserted)
            PublishFeatureSnapshot(it->second.get());
        return *static_cast<T*>(it->second.get());
    }

    template<typename T>
    T* GetFeature() const
    {
        static_assert(std::is_base_of_v<IRenderFeature, T>);
        std::shared_lock lock(m_FeaturesMutex);
        auto it = m_Features.find(std::type_index(typeid(T)));
        return (it != m_Features.end()) ? static_cast<T*>(it->second.get()) : nullptr;
    }

    // Phase 14 — humanoid retarget GPU pipeline. Returns the live feature
    // (constructed on first call). The feature owns the per-character
    // RetargetGPUDataStore + the 11 compute passes. HumanoidRetargetSystem
    // uses this to ReserveCharacter/SubmitCharacter every frame; the
    // BuildFrameGraph path schedules its passes when r.RetargetGPU=1.
    RetargetRenderFeature& GetRetargetRenderFeature();

    // ---- Draw command producer registry ----
    // Systems that contribute draws to the world forward pass or to the
    // shared depth passes (prepass + shadow cascades + area shadows) register a callback
    // here. RenderServices invokes every registered callback once per
    // (active view) — and, for depth, once per light-space DepthPassType —
    // during the per-frame emit step that runs at the end of
    // BuildWorldBatchKeys; ShadowMapNode re-runs the depth callbacks once it
    // has cached the frame's cascades.
    //
    // Callbacks push DrawCommand records via EmitForwardCommand /
    // EmitDepthCommand. The world pass and depth pass execute lambdas walk
    // those per-view streams and record draws via MaterialBinder. The camera
    // prepass's stream is not a registered producer's: a forward producer
    // emits each draw's prepass head with the draw (EmitForwardCommand).
    //
    // The returned subscription unregisters on destruction; drop it before
    // any state the callback closure captures is destroyed. Safe even if this
    // RenderServices died first (see ScopedSubscription).
    using ForwardEmitFn = std::function<void(ForwardEmitContext&)>;
    using DepthEmitFn = std::function<void(DepthEmitContext&, DepthPassType)>;

    // `writesDepth` is the producer's depth contract for idle recompute
    // elision: true (the conservative default) declares its draws can change
    // the rasterized DEPTH of a view (opaque/alpha-tested content — terrain,
    // grass, water surfaces), which pins every depth-derived elision family
    // to recompute each frame while the producer is registered. Blend-only
    // producers that never write depth (smoke/spray billboards) pass false —
    // their color animation is invisible to depth-derived passes.
    [[nodiscard]] ScopedSubscription RegisterForwardEmit(ForwardEmitFn fn, bool writesDepth = true);
    [[nodiscard]] ScopedSubscription RegisterDepthEmit(DepthEmitFn fn);

    // Forward DrawCommand stream API. Producers (contributors today; ECS
    // extraction in a follow-up) push records via EmitForwardCommand; the
    // world pass execute lambda walks GetForwardCommands for the active
    // view and records each draw through MaterialBinder. The stream is
    // cleared every frame in BeginWorldDrawFrame, after the prior frame's
    // pass has executed.
    //
    // `depth` states where the draw's depth comes from. A producer that draws
    // the draw's depth into the camera prepass passes ForwardDrawDepth::Prepass
    // with that draw's depth-only head, which goes into the view's prepass
    // stream with it; ForwardDrawDepth::PrepassNonOccluding passes the head for
    // the non-occluding prepass after DepthResolve (terrain grass). Every other
    // draw passes no head. One ColourPass draw keeps the view's world depth
    // writable for the frame.
    void EmitForwardCommand(::GameEngine::Rendering::ViewId viewId, const DrawCommand& cmd,
                            ForwardDrawDepth depth, const DrawCommand* prepassHead = nullptr);
    void EmitLateForwardCommand(::GameEngine::Rendering::ViewId viewId, const DrawCommand& cmd);
    // Declared-read side channel for EmitForwardCommand: a producer whose
    // emitted draw samples a render-graph-written texture descriptor-direct
    // registers the frame's import here, and the view's world pass declares it
    // (SampledVertex — emitted draws can displace vertices) so the graph
    // derives the layout transition + producer edge the bind alone cannot.
    // Same lifetime as the command stream: cleared per frame and per emit.
    // frame is the frame the texture was imported into; only that frame's
    // world pass declares the read.
    void EmitForwardSampledRead(::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                                ::GameEngine::Rendering::ViewId viewId,
                                ::GameEngine::Rendering::RenderGraph::RGTexture texture);
    // Buffer analog of EmitForwardSampledRead for GPU-driven nodes whose world
    // draw reads a compute pass's outputs (indirect args + index/vertex SSBOs,
    // e.g. CBT terrain, terrain grass). The access scope is per-buffer (Indirect
    // vs Storage) so the world pass declares the correct barrier — a plain
    // Storage read would not make the arg buffer visible to the indirect fetch
    // stage. frame is the frame the buffer was imported into; only that frame's
    // passes declare the read. `readers` names the passes that declare it: the
    // world pass, and also the camera prepass and the non-occluding prepass when
    // the draws reading the buffer carry prepass heads (ForwardDrawDepth::Prepass
    // or PrepassNonOccluding). The prepasses declare their reads when they are
    // declared, so a node emitting them must be declared ahead of DepthPrepass
    // (the pipeline compiler declares every node registered as feeding the
    // prepass there).
    void EmitForwardSampledBufferRead(::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                                      ::GameEngine::Rendering::ViewId viewId,
                                      ::GameEngine::Rendering::RenderGraph::RGBuffer buffer,
                                      ::GameEngine::Rendering::RenderGraph::RGBufferRead access,
                                      ForwardBufferReaders readers);
    std::span<const DrawCommand> GetForwardCommands(::GameEngine::Rendering::ViewId viewId) const;
    bool HasForwardCommands(::GameEngine::Rendering::ViewId viewId) const;
    std::span<const DrawCommand> GetLateForwardCommands(
        ::GameEngine::Rendering::ViewId viewId) const;
    bool HasLateForwardCommands(::GameEngine::Rendering::ViewId viewId) const;
    void ClearForwardCommands();

    // Depth DrawCommand stream API. Mirrors the forward stream but keys a
    // separate vector per (view, pass type) so prepass and shadow passes
    // can carry distinct DrawCommands. Cleared every frame in
    // BeginWorldDrawFrame; the prepass stream also with the view's forward
    // stream (EmitProducerForwardCommandsForView), whose heads it holds
    // (EmitForwardCommand).
    void EmitDepthCommand(::GameEngine::Rendering::ViewId viewId,
                          DepthPassType passType,
                          const DrawCommand& cmd);
    std::span<const DrawCommand> GetDepthCommands(::GameEngine::Rendering::ViewId viewId,
                                                  DepthPassType passType) const;
    bool HasDepthCommands(::GameEngine::Rendering::ViewId viewId,
                          DepthPassType passType) const;
    bool HasShadowCasters(::GameEngine::Rendering::ViewId viewId) const;
    bool ViewNeedsShadowCascadePasses(::GameEngine::Rendering::ViewId viewId) const;
    void ClearDepthCommands();

  private:
    // Fan a per-frame hook out to every live feature. Copies the snapshot
    // handle under a shared lock and releases it before invoking fn, so a hook
    // that reaches back into EnsureFeature cannot deadlock against the
    // non-recursive m_FeaturesMutex. The snapshot loses nothing: feature
    // pointers are stable for the registry's whole life (the map only ever
    // grows until Shutdown clears it), and a feature created concurrently with
    // a fan-out has no state the hook could act on yet.
    template<typename F>
    void ForEachFeature(F&& fn) const
    {
        std::shared_ptr<const std::vector<IRenderFeature*>> snapshot;
        {
            std::shared_lock lock(m_FeaturesMutex);
            snapshot = m_FeatureSnapshot;
        }
        if (!snapshot)
            return;
        for (IRenderFeature* feature : *snapshot)
            fn(*feature);
    }

    // Replaces m_FeatureSnapshot with a copy that also holds feature. Caller
    // holds m_FeaturesMutex exclusively. Runs once per feature type, so the
    // per-frame fan-out never allocates.
    void PublishFeatureSnapshot(IRenderFeature* feature)
    {
        auto next = m_FeatureSnapshot
                        ? std::make_shared<std::vector<IRenderFeature*>>(*m_FeatureSnapshot)
                        : std::make_shared<std::vector<IRenderFeature*>>();
        next->push_back(feature);
        m_FeatureSnapshot = std::move(next);
    }

    std::function<bool(std::string_view)> m_PackageAvailabilityQuery;
    // Shared body for the phase-A clear prepass and the phase-B recover pass
    // (design §5-A4). Phase A attaches the depth with a clear; phase B loads it
    // and skips the world-already-declared tripwire (it declares after the
    // phase-A world by construction). Consumers select their scatter generation
    // via DepthOnlyPassParamsRG::Phase at the FindBatchDrawRange lookup.
    Rendering::RenderGraph::RGPass AddWorldDepthPrepassImpl(
        Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
        Rendering::RenderGraph::RGTexture depth, float clearDepthValue,
        Rendering::GPUDrawStreamBuilder::SlicePhase phase, PrepassHeads heads);
    // Shared body for AddWorldPassForView (phase A) and
    // AddWorldColorRecoverPassForView (phase B). Phase B loads colour+depth,
    // skips WorldDeclared, and looks up SlicePhase::B draw ranges.
    WorldPassRG AddWorldPassImpl(Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
                                 const WorldPassTargetsRG& targets,
                                 Rendering::MaterialKeyword passKeywords, WorldPassDrawScope scope,
                                 Rendering::GPUDrawStreamBuilder::SlicePhase phase,
                                 uint8_t sliceCascadeIndex);

    // Declaration-time resolved-resource table for the RenderGraph arms (the old
    // ResolvePassResources resolves retained-graph handles at exec). Every
    // texture entry must be pool-persistent or external — physicals exist at
    // declaration (pool imports realize eagerly); a TRANSIENT can never enter
    // this table (no physical until Execute). `targets` is the world pass's
    // (ge_sceneDepth source); depth passes pass nullptr. Cam binds the
    // upload-ring alloc at {camBuf, camOffset, sizeof(CameraData)}.
    ResolvedPassResources BuildPassResourcesRG(Rendering::RenderGraph::RGFrame& frame,
                                               Rendering::ViewId viewId,
                                               Rendering::MaterialKeyword passKeywords,
                                               const WorldPassTargetsRG* targets,
                                               Rendering::BufferHandle camBuf,
                                               uint64_t camOffset);

    // Invoke every registered forward-emit callback for every active view.
    // Runs once per frame at the end of BuildWorldBatchKeys — after
    // producer-side GPU uploads but before the world pass's activation
    // predicate evaluates the per-view command stream.
    void EmitProducerForwardCommands();

    // Invoke every registered depth-emit callback for every active view,
    // once per light-space DepthPassType. Runs once per frame alongside
    // EmitProducerForwardCommands. The depth pass execute lambdas then walk
    // GetDepthCommands(viewId, passType) and record draws.
    void EmitProducerDepthCommands();

    // Hot-reload subscribers wired up during Initialize(). Each fires on the
    // AssetManager's drop events for its asset type — Reloaded, Unloaded and
    // Destroyed — and only enqueues; DrainPendingAssetInvalidations heals the
    // GUID-keyed caches on the render thread, by eviction or by an in-place
    // refresh where live handles point into the cache.
    void InstallAssetReloadInvalidators();

    // Device-scoped GPU resource creation, shared by Initialize() and
    // OnDeviceRebuilt(). Each recreates over dead handle members after a rebuild
    // (the teardown already freed the old VkObjects — no Destroy here).
    void CreateFallbackBuffers();
    void CreateDeviceDefaultBuffers();
    void CreateShadowFallbackResources();
    void InitializePerFrameWritePool();

    // Q6 device-lost re-provision (design §8). Registered as an IDevice
    // device-rebuilt callback in Initialize(); fires on the render thread inside
    // RebuildDevice while the device is AwaitingReprovision. Re-provisions the
    // RenderServices-owned systems (pipeline concrete cache, default/fallback
    // buffers + textures, TextureService/bindless, GPUScene, per-frame pools,
    // GPU culling/HZB) and cache-invalidates the stale GPU handles. It does NOT
    // call NotifyReprovisionComplete — meshes (slice 4) and ECS component handles
    // (slice 3b) are still dead, so the device stays AwaitingReprovision until the
    // full recovery chain resumes.
    void OnDeviceRebuilt();

    Rendering::IDevice* m_Device{nullptr};
    Rendering::RendererProfile m_Profile{};

    // GPU mesh registry (GUID-keyed, dedup'd GPU buffer owner).
    //
    // Declared ahead of every member that subscribes to it, so reverse-order
    // member destruction tears those subscribers down FIRST and their
    // destructors' UnsubscribeReload calls still find a live
    // m_ReloadSubscribersMutex. Declaring it later makes ~SceneAccelerationStructureService
    // lock a destroyed mutex, which throws std::system_error out of a
    // destructor and aborts the process at shutdown.
    std::shared_ptr<std::atomic<bool>> m_Alive{std::make_shared<std::atomic<bool>>(true)};
    Rendering::MeshGPURegistry m_MeshGPURegistry;

    std::unique_ptr<TextureService> m_Textures;
    std::unique_ptr<Rendering::GPUScene> m_GpuScene;
    std::unique_ptr<Rendering::GPUCullingPipeline> m_GpuCullingPipeline;
    // Bucketer host driver -- owns per-(stream-key) indirect cmd / count /
    // indirection buffers for the Phase 4 indirect-draw path.
    std::unique_ptr<Rendering::GPUDrawStreamBuilder> m_DrawStreamBuilder;

    // Shared BLAS pool + multi-slot TLAS backend for every ray-query
    // consumer (RT shadow mask, DDGI). Lazily created by the first consumer
    // on a ray-query device; outlives any single consumer's enable/disable
    // cycle. FrameOrchestrator ticks it once per app frame. See
    // SceneAccelerationStructureService.h.
    std::unique_ptr<SceneAccelerationStructureService> m_SceneAS;

    // RT shadow-mask service (DirectionalShadowMode::RayTraced). Lazily
    // created by ScheduleRTShadowMask on first enable when the device supports
    // ray query; null while every world has only ever used Cascades. When the
    // mode switches back to Cascades, TickInactive releases its claim on the
    // shared pool; the service object itself (shader meta, interned pipeline,
    // empty maps) survives so a later enable only rebuilds its TLAS.
    std::unique_ptr<RTShadowMaskService> m_RTShadowMask;
    std::unique_ptr<ScreenSpaceShadowPasses> m_ScreenSpaceShadows;

    // Exec→declare under-draw feedback sink (see DepthUnderDraw()).
    std::unique_ptr<DepthUnderDrawTracker> m_DepthUnderDraw;

    // Runtime LOD selection knobs (see SetLODGlobalBias / SetLODForceLevel).
    float    m_LODGlobalBias = 0.0f;
    float    m_ShadowLODBias = 0.0f;
    uint32_t m_LODForceLevel = 0xFFFFFFFFu; // 0xFFFFFFFF = auto-select
    float    m_SmallObjectCullCoverage = 0.0f; // prototype small-object cull; 0 = off
    // Dithered LOD crossfade; 0 = off. Initialized from the settings default so
    // a fresh renderer and an absent project key cannot disagree.
    float    m_LODCrossfadeDuration = Rendering::LodProjectSettings::kDefaultCrossfadeDuration;
    // Replays one crossfade-liveness verdict across a frame's several scatter
    // calls; see ResolveCrossfadeLiveness for why the verdict is an observed tail
    // census rather than a clock, and why a frozen pair is the failure it guards.
    LodCrossfadeLivenessState m_CrossfadeLiveness{};
    // SSE-budget LOD selection knobs (see SetLODErrorBudgetPx).
    float    m_LODErrorBudgetPx = Rendering::kDefaultLodErrorBudgetPx;
    float    m_LODSkinnedBudgetScale = Rendering::kDefaultLodSkinnedBudgetScale;
    // Indexed by static_cast<size_t>(Rendering::ViewPurpose); default-constructed
    // entries are disabled, so a fresh RenderServices spends the global budget in
    // every view (see SetLODViewBudgetOverride).
    std::array<Rendering::LodViewBudgetOverride, Rendering::kViewPurposeCount>
             m_LODViewBudgetOverrides{};
    float    m_LODHysteresisBand       = 0.0f; // LOD dwell band fraction; 0 = off

    // Draw consolidation: geometry-bind group ids over the registry's pools.
    // Refreshed at the scatter schedule sites (declaration time, render
    // thread); read by the draw consumers at pass record — same single-writer
    // frame discipline as the scatter range map. Ids are append-only, so
    // record-time lookups never shift against an earlier call's tables.
    Rendering::MeshPoolGroupPlan m_MeshPoolGroups;
    // The paired spans ScheduleUnifiedScatter consumes: the per-mesh ORDERED
    // axis (group members contiguous — what the mesh-major compaction keys
    // on) and its rank->group inverse. Both empty when the kill-switch
    // (GE_DRAW_CONSOLIDATION=0) selects the per-bucket path.
    struct ScatterGroupMaps
    {
        std::span<const uint32_t> MeshOrdered;
        std::span<const uint32_t> OrderedToGroup;
    };
    // Refresh the plan against the current registry/mesh table and return the
    // spans ScheduleUnifiedScatter consumes.
    ScatterGroupMaps MeshPoolGroupMapsForScatter();

    // HLOD runtime cluster table + proxy lifecycle for the bound world (created in
    // Initialize). The residency-flip request routed from HLODSelectSystem to
    // RenderExtractionSystem lives here so neither system references the other.
    std::unique_ptr<Hlod::HlodRuntime> m_HlodRuntime;
    std::atomic<bool> m_HlodResidencyPending{false};

    // The material stack (A1.3). Owns the registry, compiler, shader-compilation
    // cache, pipeline-variant cache, prewarm service + drain, build context, the
    // MaterialParams SSBO packing state, the depth/color classification maps, the
    // material hot-reload invalidator, and the MaterialBinder. Reached via
    // Materials(); the frame-spine call sites (BeginFrame, FinalizeFrameBuffers,
    // the two-phase Shutdown) stay RenderServices-side.
    MaterialSystem m_MaterialSystem;

    // Skin palette atlas (per-frame shared SSBO for instanced skinned draws).
    SkinPaletteAtlas m_SkinPaletteAtlas;

    // GPU animation data store for compute skinning (Tier 2).
    GPUAnimationDataStore m_GPUAnimDataStore;
    std::unique_ptr<AnimationComputePass> m_AnimComputePass;
    // The skinning shader was resolved and is absent — stop probing the
    // filesystem for it every frame (retry-storm; on web a main-thread OPFS
    // hazard).
    bool m_AnimShaderResolvedMissing = false;

    // Per-frame double-buffered SSBO write pool.
    PerFrameWritePool m_PerFrameWritePool;

    // Camera + view registries and persistent per-view state (A1.2). Owned by
    // value so lifetime is identical to the former inline members and the
    // world-pass execute lambdas can hold spans into its storage across
    // declare→execute. Reached by RS internals via m_ViewRegistry directly and
    // by external callers through Views().
    ViewRegistry m_ViewRegistry;
    ViewTemporalHistory m_ViewTemporalHistory;

    // Zero-filled placeholder for unwired set0 buffer descriptors (see
    // GetDefaultPlaceholderBuffer). Created during Initialize.
    Rendering::BufferHandle m_DefaultPlaceholderBuffer{};

    // Reload subscribers — RAII handles that auto-unsubscribe on Shutdown().
    // Reset before m_TextureGPUCache / registries are torn down so callbacks
    // never fire into freed members. Texture-type events evict single entries;
    // Model-type events drop GPU mesh buffers, runtime models, and any
    // embedded-image textures derived from the parent model GUID.
    // Texture/Model/CubeLut invalidations arrive on ANY thread: only
    // AssetReloaded dispatches from the main thread (AssetManager::Update ->
    // CheckForReloads); Unloaded/Destroyed fire synchronously on the raising
    // thread — file deletes/renames/branch switches come in on the
    // file-watcher thread. The handlers destroy GPU resources and walk live
    // material bindings, so they enqueue here and BeginWorldDrawFrame drains
    // on the render thread (same deferral as RequestTextureReupload and the
    // variant-cache evictions).
    struct PendingAssetInvalidation
    {
        AssetType Type;
        GUID Guid;
    };
    std::mutex m_PendingAssetInvalidationsMutex;
    std::vector<PendingAssetInvalidation> m_PendingAssetInvalidations;
    void DrainPendingAssetInvalidations();

    // Filled by the drain above for each model it refreshed in place, taken by
    // TakeModelsReloadedInPlace. Needs no mutex: unlike the pending list, which
    // producers append to from the file-watcher thread, this is written and read
    // on the render thread only.
    std::vector<GUID> m_ModelsReloadedInPlace;
    // Filled by the same drain for every model it handled; taken by
    // TakeModelsInvalidated. Render thread only, as above.
    std::vector<GUID> m_ModelsInvalidated;

    AssetReloadInvalidator m_TextureReloadInvalidator;
    AssetReloadInvalidator m_ModelReloadInvalidator;
    AssetReloadInvalidator m_CubeLutReloadInvalidator;

    // Latest RenderExtractionSystem sub-phase timings (see the public accessor).
    RenderExtractionStats m_RenderExtractionStats{};

    // A2 STEP-0 render-thread CPU-timeline brackets (see the public accessor).
    // Written by BuildWorldBatchKeys and the FrameOrchestrator spine (friend).
    RenderTimelineStats m_RenderTimelineStats{};

    // SkeletonStore::GetRuntimeCreateEpoch() as of the last skinning-gate
    // arm. A mismatch means a runtime spawned this frame — the gate is
    // forced off for the frame (see ScheduleGpuSkinningAndRetarget).
    // Initialized to a sentinel so the first frame always counts as a spawn.
    uint64_t m_LastRuntimeCreateEpoch = ~0ull;

    // Rasterizer state the material facade reads synchronously at PSO build via
    // the friend edge (§0a-A3). The front face follows the device API at init.
    Rendering::CullModeFlags m_CullMode = Rendering::CullModeFlagBits::Back;
    Rendering::FrontFace m_FrontFace = Rendering::FrontFace::CounterClockwise;
    bool m_WorldPassExcludeEnvironment = false;
    Rendering::TextureFormat m_DepthFormat = Rendering::TextureFormat::D32_FLOAT;
    // Engine-wide AA defaults (mode, MSAA count, FXAA quality, TAA cycle).
    // The MSAA count starts on ResolveDefaultAntiAliasing's no-device rung (1
    // sample = off) and Initialize replaces it with the rung this device can
    // run. See AntiAliasingSettings for each field's override sources.
    AntiAliasingSettings m_AA{AntiAliasingMode::Off, ResolveDefaultAntiAliasing({}).SampleCount,
                              FxaaQuality::Quality, 8u};
    // Engine-default internal render scale for views without a per-view
    // override; 1.0 = native (GE_TAA_RENDER_SCALE, the project's
    // rendering.taaRenderScale, and dynamic resolution drive it below 1.0).
    float m_DefaultRenderScale = 1.0f;
    // Off until a switch or a test fixture selects an arm.
    DeformationMotionArm m_DeformationMotionArm = DeformationMotionArm::None;

    // Dynamic resolution. The controller is inert unless m_DrsMode is Dynamic;
    // m_DrsFixedScale remembers the user's static slider value so leaving
    // Dynamic mode restores it instead of stranding whatever scale the
    // controller happened to settle on.
    DynamicResolutionMode m_DrsMode = DynamicResolutionMode::Off;
    DynamicResolutionController m_DrsController;
    float m_DrsFixedScale = 1.0f;
    // Last spine world-frame epoch the controller ticked on (dedup key for
    // multi-window declares); the sentinel means "never ticked".
    uint64_t m_DrsLastTickEpoch = 0xFFFFFFFFFFFFFFFFull;
    bool m_WarnedDrsUnreachable = false;
    bool m_WarnedDrsIneffective = false;

    // Per-world extracted light lists (cleared at start of each frame).
    std::unordered_map<uint64, std::vector<ExtractedLight>> m_WorldLights;
    // Worlds that already warned about carrying more directional lights than
    // kMaxDirectionalLights (the overflow lights contribute nothing; see
    // PackForwardLightDirectionals). Membership clears when the world drops
    // back within the cap, so the warning re-fires once per episode instead of
    // spamming per frame.
    std::unordered_set<uint64> m_MultiDirectionalWarnedWorlds;
    // Per-world renderable-membership digests (cleared at start of each frame;
    // republished by each world's extraction pass before probes read them).
    std::unordered_map<uint64, uint64> m_WorldRenderContentDigests;
    // Per-world resolved post-process settings (cleared at start of each frame).
    std::unordered_map<uint64, PostProcessSettings> m_WorldPostProcess;
    std::unordered_map<uint64, std::vector<VolumetricFogLocalVolume>> m_WorldVolumetricFogVolumes;
    static const PostProcessSettings kDefaultPostProcess;
    // Per-world resolved directional-shadow overrides (cleared at start of each frame).
    std::unordered_map<uint64, ResolvedShadowSettings> m_WorldShadowSettings;
    static const ResolvedShadowSettings kDefaultShadowSettings;

    // This frame's mover instances (see FrameMoverRecord above). The mutex
    // guards multi-world extraction appends; reads happen at declare time
    // after extraction completed.
    std::vector<FrameMoverRecord> m_FrameMovers;
    // Views that reported unversioned motion this frame and the previous one;
    // guarded by the same mutex as the movers list.
    std::vector<Rendering::ViewId> m_UnversionedMotionViews;
    std::vector<Rendering::ViewId> m_PreviousUnversionedMotionViews;
    std::mutex m_FrameMoversMutex;

    // Per-view culling guard. Set by ScheduleViewCullingDispatches on first
    // call each frame; cleared by BeginWorldDrawFrame. Multiple BuildFrameGraph
    // calls per frame (SceneView + GameView view controllers share an RG)
    // would otherwise wipe GPUCullingPipeline's pending-views via BeginFrame
    // on the second invocation. Same idempotence pattern used by
    // m_GPUAnimDataStore.IsDispatchScheduledThisFrame().
    bool m_CullingScheduledThisFrame = false;
    // RenderGraph frame-local values (reset in BeginWorldDrawFrame; see GpuDrivenFrameRG).
    GpuDrivenFrameRG m_FrameRG;
    // Per-view RenderGraph frame-locals — the frame half of PerViewResources
    // (the persistent half above). Reset wherever m_FrameRG resets: values are
    // RGFrame-local ids and die with their frame, so they are never destroyed,
    // only identity-checked — which is why they don't live in PerViewResources.
    struct ViewFrameRG
    {
        // Identity guard (shared RGFrameStamp): the ids below are frame-local —
        // an entry written by a different frame stream, or a re-begun
        // incarnation of this one, is stale and must be treated as empty. The
        // pointer alone is never enough (RGFrames are re-begun across frames).
        Rendering::RenderGraph::RGFrameStamp For;
        Rendering::RenderGraph::RGTexture ShadowMapArray{}; // pool import, ONCE per (view, frame)
        Rendering::RenderGraph::RGTexture TransmittanceShadowArray{}; // pool import: per-cascade glass tint (translucent shadows), parallel to ShadowMapArray; only when the view has transmissive casters
        Rendering::RenderGraph::RGTexture MsmMoments{};     // external import (feature-owned physical)
        // PCSS min/max pyramid over the cascade depth (ShadowMinMaxPyramid):
        // pool import, declared only on effective-PCSS frames. The world arms
        // read it Sampled — the fragment reaches it bindlessly, so the declared
        // read is the ONLY thing ordering the reduction before the draw.
        Rendering::RenderGraph::RGTexture ShadowPcssPyramid{};
        Rendering::RenderGraph::RGTexture AreaShadowMap{};  // pool import
        Rendering::RenderGraph::RGTexture SpotShadowMap{};  // pool import
        Rendering::RenderGraph::RGTexture PointShadowMap{}; // pool import, budget*6 atlas layers (M1)
        // IBL bake outputs (engine-shared, feature-owned physicals). Imported here
        // ONCE per (view, frame); dedup-by-handle lands these on the SAME ids the
        // IBLGen bake's writes produced, forming the bake->world StorageWrite->Sampled
        // edge (and ordering the bake before this view's world pass).
        Rendering::RenderGraph::RGTexture IblIrradiance{};
        Rendering::RenderGraph::RGTexture IblPrefilter{};
        Rendering::RenderGraph::RGTexture IblBrdfLut{};
        // RT shadow-mask (DirectionalShadowMode::RayTraced): the per-view ray-query mask
        // declared by the phase-A world arm, bound as ge_rtShadowMask on
        // RTShadowMask-keyword passes; phase B (recover) reuses it.
        Rendering::RenderGraph::RGTexture RTShadowMask{};
        Rendering::RenderGraph::RGTexture ScreenSpaceShadowMask{};
        // The SAME upload alloc the world pass's resolved table reads —
        // written at the area arm's declaration, bound by name "AreaShadowData".
        Rendering::RenderGraph::RGFrame::TypedUpload<AreaShadowDataGPU> AreaShadowData{};
        // The motion variant's per-view block, written at the producer arm's
        // declaration and bound by the block's INSTANCE name "MotionParams" —
        // reflection names a block by its instance, as it does for Cam and
        // Light. Absent on a view whose producer did not run this frame; the
        // zero fallback then binds, so the descriptor is never unwritten and
        // never another view's matrix.
        Rendering::RenderGraph::RGFrame::TypedUpload<DeformationMotionParamsGPU>
            DeformationMotion{};
        Rendering::RenderGraph::RGFrame::TypedUpload<SpotShadowDataGPU> SpotShadowData{};
        // A variable-length buffer range bound by name over handle+offset+bytes
        // (not a single TypedUpload).
        struct UploadedBuffer
        {
            Rendering::BufferHandle Buffer{};
            uint64_t Offset = 0;
            uint64_t Bytes = 0;
            bool Valid() const { return Buffer.IsValid() && Bytes > 0; }
        };
        // M1: the point-shadow SSBO is a variable-length slot array, bound by name
        // "PointShadowData".
        UploadedBuffer PointShadowData{};
        // Compatibility profile: this view's instance index list
        // (CpuDrawStreamBuilder::GetIndexList), uploaded at the first pass
        // declaration that binds it and bound by name "CompatInstanceList" in
        // every pass of the view. Invalid when the view draws nothing.
        UploadedBuffer CompatInstanceList{};
        // The depth texture the prepass arm attached this frame. The world
        // arm suppresses its depth clear / binds read-only ONLY when this
        // matches ITS depth target — identity, not a bool: a prepass run on
        // an override depth (DepthPrepassNode depthRef) leaves the world's
        // own depth unwritten, and suppressing the clear there would render
        // against stale depth.
        Rendering::RenderGraph::RGTexture DepthPrepassDepth{};
        // Misorder tripwire: producer arms (prepass/cascade/area) declared
        // AFTER this view's world pass would derive WAR edges — the world
        // would sample frame N−1 content, silently. The arms error loudly.
        bool WorldDeclared = false;
        // Set by ImportShadowMapArrayRG at the one point where the cascade arm
        // has committed to importing. It is the producer's own record of that
        // decision, so consumers never re-derive it: a re-derived predicate has
        // to mirror every gate upstream of the import (the node alone has six)
        // and reports a false alarm on each one it misses. With the flag, a
        // missing ShadowMapArray is a producer bug if and only if this is true.
        bool CascadeArrayOwed = false;
    };
    std::unordered_map<Rendering::ViewId, ViewFrameRG> m_ViewFrameRG;
    // Compatibility profile: the view's CPU-built instance index list, copied
    // into this frame's upload ring once per (frame, view) and cached in
    // ViewFrameRG. The recorders read batch offsets into it at record time, so
    // the view's list must not be rebuilt after its first pass declares.
    ViewFrameRG::UploadedBuffer UploadCompatInstanceList(Rendering::RenderGraph::RGFrame& frame,
                                                         Rendering::ViewId viewId);
    // Sole mutation entry point: resets a stale (foreign-frame) entry before
    // handing it out. Readers that only probe use FindViewFrameRGFor.
    ViewFrameRG& ViewFrameRGFor(Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId);
    // Non-inserting read probe: same (frame, FrameIndex) validity guard as
    // ViewFrameRGFor, but returns null on an absent/stale entry instead of
    // minting a fresh one — the shape the read-only GetShadowMapArrayRG /
    // world-pass probes collapse onto.
    const ViewFrameRG* FindViewFrameRGFor(Rendering::RenderGraph::RGFrame& frame,
                                          Rendering::ViewId viewId) const;
    // ImportShadowMapArrayRG / ImportTransmittanceShadowArrayRG are declared in
    // the public passkey-gated block above (they adopt the pooled physical into
    // ShadowMapRenderFeature and write ViewFrameRG; the feature calls them
    // through the ShadowDeclareSeam). The world arm consumes the cached
    // ViewFrameRG value instead of importing, so a producerless frame never
    // creates the ~Resolution²×cascades pool array.
    // Shared slice-registration body (CPU staging): guards + one
    // RegisterSlice per (view, cascade) pair. Returns the instance count
    // (0 = nothing). The caller appends the ScheduleUnifiedScatter tail.
    uint32_t ScheduleWorldBucketerCommon();
    // Per-view world tag the bucketer/scatter filter GPUInstance.flags[16:32]
    // against — folds the 64-bit worldId to 16 bits the same way
    // RenderExtractionSystem packs it. 0 = untagged (match any) or unknown view.
    // Single source for the bucketer, shadow-bucketer, and occlusion-recover
    // scatter registrations.
    uint32_t WorldKeyForView(Rendering::ViewId viewId) const;
    // Lazy-init of the GPU skinning pass (shared by both arms).
    void EnsureAnimComputePass();

    // Monotonically increments once per actual cull-frame inside
    // ScheduleViewCullingDispatches. Hands a stable frameIndex to every
    // submitted ViewCullingInput so the pipeline's per-frame caches stay
    // coherent across views.
    uint32_t m_CullingFrameIndex = 0;

    // World draw builder (engine-level helper). Initially wired up
    // in a simple, single-threaded implementation; later iterations can use
    // JobSystem for parallel submission and list construction.
    WorldDrawBuilder m_WorldDrawBuilder;
    // Compatibility profile only: the CPU twin of the GPU scatter's per-slice
    // instance stream. Rebuilt from the merged submissions right after the
    // batch keys, and empty on the full profile (nothing calls Build there).
    CpuDrawStreamBuilder m_CpuDrawStream;
    // Per-frame bookkeeping so we can safely decide when it is valid to (re)build
    // world draw lists. This prevents Editor-side pipeline scheduling from
    // accidentally building empty lists before extraction runs.
    uint64_t m_WorldBeginFrameIndex = 0xFFFFFFFFFFFFFFFFull;
    uint64_t m_WorldDrawListsBuiltFrameIndex = 0xFFFFFFFFFFFFFFFFull;
    std::atomic<uint32_t> m_WorldSubmissionCountThisFrame{0};

    // Color/world pass keyword set to prewarm for, derived from the active
    // blueprint's WorldRender pass (the same source the draw path keys off via
    // PerViewResources::WorldPassKeywords). Lets prewarm match the loaded pipeline (IBL in
    // the editor, non-IBL in WASDDemo) instead of guessing.
    Rendering::MaterialKeyword ResolveWorldPassKeywordsForPrewarm() const;

    Rendering::SamplerHandle m_AreaShadowSampler;
    Rendering::SamplerHandle m_AreaShadowRawSampler;
    Rendering::TextureHandle m_AreaShadowFallbackTexture;
    // 1x1 white 2D-array bound to ge_transmittanceShadowArray when a view has no
    // glass casters (so the receiver's tint sample reads white = no attenuation).
    Rendering::TextureHandle m_TransmittanceShadowFallback;
    Rendering::SamplerHandle m_SpotShadowSampler;
    Rendering::SamplerHandle m_PointShadowSampler;
    Rendering::SamplerHandle m_CascadeShadowSampler;
    Rendering::TextureHandle m_PointShadowFallbackTexture;
    // 1x1 kMaxShadowCascades-layer D32 array bound to ge_shadowMapArray when a
    // view has no live cascade array this frame. Cleared to 0.0 — see
    // CreateShadowFallbackResources for why that value means "fully lit".
    Rendering::TextureHandle m_CascadeShadowFallbackTexture;

    // ── M1 point-shadow atlas planning ──
    // The budgeted set + per-cluster slot map for one (view, device frame),
    // computed once by EnsurePointShadowAssignment and shared by declaration, GPU
    // cull scheduling, batch registration, and light upload so all four agree.
    struct PointShadowAssignment
    {
        uint64_t Frame = 0;
        // World whose caster epoch this view's planner continuity belongs to.
        // Epochs are per-world; a view presenting a different world must Reset()
        // its planner instead of comparing epochs across worlds.
        uint64_t WorldId = 0;
        bool Computed = false;
        std::vector<PointShadowFrameInfo> Slots;   // dense; each carries shadowSlot
        std::vector<int32_t> SlotOfCluster;        // clusterableIndex -> slot (-1 = unshadowed)
    };
    // Compute-or-return the assignment for (viewId, current device frame). Advances
    // the view's planner hysteresis EXACTLY once per frame (guarded on the device
    // frame index), so repeated calls within a frame are free and consistent.
    const PointShadowAssignment& EnsurePointShadowAssignment(Rendering::ViewId viewId,
                                                             uint64 worldId,
                                                             const Rendering::CameraData& camData);

    std::unordered_map<Rendering::ViewId, PointShadowAtlasPlanner> m_PointShadowPlanners;
    std::unordered_map<Rendering::ViewId, PointShadowAssignment> m_PointShadowAssignments;
    uint32_t m_PointShadowBudget = kDefaultPointShadowBudget;
    // L1a/L1b per-world caster-content change sets (see
    // NotifyShadowCasterContentChanged): the monotonic version plus the changed
    // casters that explain its LATEST advance. Written during the extraction and
    // animation waves, then in the terminal RenderGraphBuild wave by
    // BuildWorldBatchKeys; ECS waves are joined in sequence, so a later wave's
    // write never overlaps an earlier one's and no atomic is needed. Read by
    // EnsurePointShadowAssignment later the same frame.
    struct ShadowCasterChanges
    {
        uint64_t Version = 0;
        bool Unattributed = true;
        std::vector<ShadowCasterChangeSphere> Spheres;
    };
    std::unordered_map<uint64, ShadowCasterChanges> m_ShadowCasterChanges;

    // Worlds whose submissions this frame include a material that moves its own
    // vertices, and whether any of those submissions casts shadows. Rebuilt
    // every frame from the per-view flags the batch-key derivation produces —
    // every draw producer's records meet there, so it covers the ECS extraction
    // lane and the package producers (EZTree) that build their GPUScene
    // instances themselves and never appear in an extraction submission list.
    struct AnimatedVertexModifierWorld
    {
        uint64 WorldId = 0;
        bool CastsShadows = false;
    };
    std::vector<AnimatedVertexModifierWorld> m_AnimatedVertexModifierWorlds;
    // Raise the per-world content signals for one view's derived flags. Called
    // for every view once its batch keys are derived; the latch above keeps it
    // to one advance per world per frame however many views share that world.
    void RaiseAnimatedVertexModifierContentSignals(Rendering::ViewId viewId);

    // ── Idle recompute elision (lever #2) engine-side state ──
    // Per-world ANY-renderable content versions (NotifyRenderContentChanged);
    // same single-writer/main-thread discipline as the shadow versions above.
    std::unordered_map<uint64, uint64_t> m_RenderContentVersions;
    // Per-world finalized light-list snapshots + versions: FinalizeWorldLights
    // memcmp's the sorted list against the snapshot and bumps the version on
    // any byte difference — the exact-equality light signal the cluster and
    // culling elision gates key on.
    std::unordered_map<uint64, std::vector<ExtractedLight>> m_WorldLightSnapshots;
    std::unordered_map<uint64, uint64_t> m_WorldLightVersions;
    IdleElisionFrameState m_IdleElision;
    uint64_t m_IdleElisionDepthDynamicEpoch = 0;
    // Set by NotifySkinPaletteContentChanged; consumed + cleared once per app
    // frame by UpdateIdleElisionFrameState (which folds it into
    // m_IdleElisionDepthDynamicEpoch).
    bool m_SkinPaletteContentChanged = false;
    // Refresh m_IdleElision once per app frame (owner spine, before the
    // culling schedule) and push the module-side gate contexts.
    void UpdateIdleElisionFrameState();
    // Re-inject the scatter gate context immediately before EACH
    // ScheduleUnifiedScatter call site so the visibility-write epoch it keys
    // on is current (phase-B P2 dispatches advance it mid-pipeline).
    void RefreshScatterElisionContext();

    // L1a atlas physical-lifetime guard. The persistent PointShadowAtlas.View{N} is
    // pool-evicted after ~300 frames of not being imported (the assignment went
    // empty — no visible shadowed point light) and is reallocated fresh on a budget
    // change (arrayLayers = budget*6). Either recreates the texture as
    // fresh/Undefined while the planner's per-slot render cache still matches, so a
    // "cached" slot would sample garbage. EnsurePointShadowAssignment tracks, per
    // view, the monotonic GPUScene frame the atlas was last active (imported) and
    // the budget then; a gap or a budget change invalidates that view's render
    // cache BEFORE the planner decides (glitch-free — a reactive post-import signal
    // would land a frame after cull scheduling already skipped the cached slots).
    struct PointShadowAtlasLiveness
    {
        bool EverActive = false;
        uint64_t LastActiveFrame = 0; // monotonic GPUScene frame of the last non-empty assignment
        uint32_t LastBudget = 0;
    };
    std::unordered_map<Rendering::ViewId, PointShadowAtlasLiveness> m_PointShadowAtlasLiveness;
    // Per-view last-logged cache-stats signature (log-on-change), a member rather
    // than a function-local static so instances don't share log state.
    std::unordered_map<Rendering::ViewId, uint64_t> m_PointShadowCacheLogState;

    // Fallback buffers for Forward+ bindings when the active pipeline does not
    // provide them (or culls the producing passes). Keyed by StringId of the
    // binding name so the draw loop can look them up without hardcoded names.
    std::unordered_map<StringId, Rendering::BufferHandle> m_FallbackBuffers;

    // The frame spine (A1.4): per-window RenderGraph stream slots, app-frame
    // epochs + declare sequence, the pipeline-asset lifecycle (path/blueprint/
    // compiler/node-registry + EnsureActiveRenderPipelineBlueprint), the
    // background shader-package cache, and BuildFrameGraph itself. Owned by
    // value; reached via Spine(). The frame-local declare-scope state it
    // consumes (m_FrameRG, m_ViewFrameRG, m_CullingScheduledThisFrame) STAYS
    // here and is reached through the friend grant. See FrameOrchestrator.h.
    FrameOrchestrator m_FrameOrchestrator;

    std::unordered_map<std::type_index, std::unique_ptr<IRenderFeature>> m_Features;
    // Insertion-ordered, immutable view of m_Features for ForEachFeature.
    // Swapped (never mutated) under m_FeaturesMutex; readers share the handle.
    std::shared_ptr<const std::vector<IRenderFeature*>> m_FeatureSnapshot;

    // Guards m_Features' structure (see EnsureFeature). Mutable so the
    // const GetFeature<T>() lookup can take a shared lock.
    mutable std::shared_mutex m_FeaturesMutex;

    // The post-opaque scene-colour grab the dedicated transmissive pass refracts (Phase 1-B
    // S4). Feature-owned device texture, lazily created on the first transmissive view; bound
    // as ge_sceneColor (Transmission/SceneColorGrab-keyword passes only).
    std::unique_ptr<SceneColorGrab> m_TransmissionSceneGrab;

    // Sorted transparent (T2/S2) per-view staging, rebuilt once per view per
    // frame by BuildSortedTransparentForView (records + run layout) and
    // consumed by AddSortedTransparentDrainForView (uploads, drain dispatch,
    // per-run indirect draws). PcBytes is the stable 16-byte GE_INSTANCED
    // push-constant backing every run command's Bindings.PushConstants span
    // aliases through render-graph execution. Frame stamps the last build so a
    // stale Active never leaks across frames or to views the world node did
    // not declare.
    struct SortedTransparentView
    {
        std::vector<SortedTransparentRecord> Records;
        // One entry per run GROUP in discovery order; the partition maps
        // groups to draw slots. Representative binds the run's PSO +
        // descriptor state; Mesh resolves the shared bucket pools every
        // member of the run suballocates from.
        struct Run
        {
            const Material* Representative = nullptr;
            Rendering::MeshGPUHandle Mesh{};
        };
        std::vector<Run> Runs;
        SortedTransparentRunPartition Partition;
        std::vector<DrawCommand> Commands;
        std::vector<std::byte> PcBytes;
        // Camera snapshot from the build (the drain's GPU key must use the
        // same values the CPU run metric + overflow order used).
        float CameraPos[3]{};
        float CameraForward[3]{};
        bool Active = false;
        uint64_t Frame = ~0ull;
        // One warning per drain-bail EPISODE, not per frame: a persistently
        // failing drain declare (device-rebuild window, pool exhaustion) must
        // not spam the log. Set by the drain's bailToBatched, cleared by the
        // next successful drain declare.
        bool WarnedDrainBail = false;
        SortedTransparentStats Stats{};
        // Log-on-transition memory for the path's Debug lines: the lines fire
        // only when the path engages or its composition changes (records /
        // runs / sort path), never per steady-state frame. LastLoggedStats is
        // the composition the build line last printed; StagedLastBuild marks
        // whether the PREVIOUS build staged records, so a disengage ->
        // re-engage edge re-fires the lines even when the composition matches;
        // LoggedBuildTransition couples the drain line to a build line that
        // fired this frame. Any future diagnostic on this path must keep the
        // convention: gate on a transition, steady state stays silent.
        SortedTransparentStats LastLoggedStats{};
        bool StagedLastBuild = false;
        bool LoggedBuildTransition = false;
        struct DrainLogSnapshot
        {
            uint32_t Runs = 0;
            uint32_t Records = 0;
            bool GpuSort = false;
        };
        DrainLogSnapshot LastLoggedDrain{};
        // CPU-sorted drain buffers (host-visible args+counts+indirection),
        // one per frame-in-flight slot, created lazily only when the view's
        // visible set exceeds kSortedTransparentSortCapacity and grown
        // geometrically. A ring exactly as deep as the device paces, so what makes
        // rewrites and growth-destroys safe is that the only writer runs behind
        // BeginFrame's fence wait (FrameBufferAllocator::BeginFrame states the rule);
        // the buffers carry BufferCreateFlags::FrameSlotted so a write from any
        // earlier phase is diagnosed.
        Rendering::BufferHandle CpuDrainBuffers[Rendering::IDevice::kMaxSupportedFramesInFlight]{};
        uint64_t CpuDrainCapacityBytes[Rendering::IDevice::kMaxSupportedFramesInFlight]{};
    };
    std::unordered_map<Rendering::ViewId, SortedTransparentView> m_SortedTransparentByView;

    // Sorted transparent drain compute (sorted_transparent_drain.comp), interned
    // lazily on the first drain declare. The set layout is the six-binding
    // records/instances/args/counts/indirection/runCounts block.
    Rendering::ComputePipelineId m_SortedTransparentDrainPipeline{};
    Rendering::DescriptorSetLayoutDesc m_SortedTransparentDrainLayout{};
    // Latched only on DEFINITIVE failure (SPIR-V rejected at intern).
    bool m_SortedTransparentDrainAttempted = false;
    // Transient package-load failures (hot-reload mid-write) retry on a frame
    // backoff instead of sticking the drain on the CPU sorter all session.
    static constexpr uint64_t kSortedTransparentDrainRetryInterval = 512;
    uint64_t m_SortedTransparentDrainRetryFrame = 0;
    // Lazily create + intern the drain pipeline; false when the shader package
    // is unavailable (the drain then uses the CPU-sorted twin at any count).
    bool EnsureSortedTransparentDrainPipeline();

    // Registered draw producer callbacks. Small vectors of (handle, fn) —
    // registration is rare (feature setup) and iteration runs once per view
    // per frame. shared_ptr-owned so a ScopedSubscription's late unregister
    // operates on the block alone, never on a possibly-dead RenderServices;
    // main-thread only, like registration always was.
    struct ForwardProducerEntry
    {
        uint64_t Handle = 0;
        ForwardEmitFn Fn;
        bool WritesDepth = true; // depth contract (see RegisterForwardEmit)
    };
    struct DepthProducerEntry
    {
        uint64_t Handle = 0;
        DepthEmitFn Fn;
    };
    struct DrawProducerBlock
    {
        std::vector<ForwardProducerEntry> Forward;
        std::vector<DepthProducerEntry> Depth;
        uint64_t NextHandle = 1;
    };
    std::shared_ptr<DrawProducerBlock> m_DrawProducers = std::make_shared<DrawProducerBlock>();
};

} // namespace Engine::Renderer
} // namespace GameEngine
