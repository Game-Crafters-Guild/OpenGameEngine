#pragma once

#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/SceneProbeRecaptureSchedule.h"
#include "Engine/Rendering/SkyEnvironmentSource.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/CameraTypes.h"

#include <cstdint>

namespace GameEngine
{
namespace Rendering::RenderGraph
{
class RGContext;
}

namespace Engine::Renderer
{

class ImageBasedLightingFeature;
class RenderServices;
struct FeatureCullingContext;

enum class SceneReflectionProbeUpdateMode : uint32_t
{
    Once = 0,
    Realtime = 1,
};

struct SceneReflectionProbeEnvironmentDesc
{
    float Position[3] = {0.0f, 0.0f, 0.0f};
    float InfluenceRadius = 12.0f;
    bool BoxProjection = true;
    float BoxCenter[3] = {0.0f, 0.0f, 0.0f};
    float BoxHalfExtents[3] = {0.0f, 0.0f, 0.0f};
    float BoxAxisX[3] = {1.0f, 0.0f, 0.0f};
    float BoxAxisY[3] = {0.0f, 1.0f, 0.0f};
    float BoxAxisZ[3] = {0.0f, 0.0f, 1.0f};
    float MaxDistance = 0.0f;
    uint32_t CullMask = 0xFFFFFFFFu;
    uint64_t WorldId = 0;
    uint64_t SourceKey = 0;
    float Intensity = 1.0f;
    float ExposureEV = 0.0f;
    float RotationRadians = 0.0f;
    float LowerHemisphereDarkness = 1.0f;
    uint32_t CaptureResolution = 256;
    // See Components::ReflectionProbe::CaptureEnvironment. False excludes the
    // environment cube from the capture's shading (recursion guard).
    bool CaptureEnvironment = false;
    SceneReflectionProbeUpdateMode UpdateMode = SceneReflectionProbeUpdateMode::Realtime;
    float RealtimeUpdateInterval = 0.5f;
};

// Captures the current scene from the probe position into the shared IBL
// capture cube, using the active sky/HDRI as the background and then rendering
// world geometry over the six cubemap faces.
class SceneReflectionProbeEnvironmentSource final : public IEnvironmentSource
{
  public:
    explicit SceneReflectionProbeEnvironmentSource(RenderServices& services);
    ~SceneReflectionProbeEnvironmentSource() override;

    const char* Name() const override { return "SceneReflectionProbeEnvironmentSource"; }
    uint64_t InputDigest() const override;
    bool HasActiveContent() const override { return m_Desc.SourceKey != 0; }
    float IblIntensity() const override { return m_Desc.Intensity; }
    float LowerHemisphereDarkness() const override { return m_Desc.LowerHemisphereDarkness; }
    EnvironmentLocalReflectionData LocalReflectionData() const override;
    void ScheduleBake(const EnvironmentBakeContext& ctx) override;
    // One fused CascadeCullingGroup per run of armed faces (faces 0-3 and 4-5
    // for a full bake, one face while slicing), each face a color slice at
    // ProbeFaceCullingIndex(face) of the single capture view.
    void ScheduleCulling(const FeatureCullingContext& ctx) override;

    void SetProbe(const SceneReflectionProbeEnvironmentDesc& desc);
    const SceneReflectionProbeEnvironmentDesc& GetProbe() const { return m_Desc; }


  private:
    // Capture-view lifecycle, advanced once per frame by ScheduleBake. ONE
    // OnDemand capture view (ViewParticipation) serves all six faces: it
    // participates in extraction, batch-key build, and the GPU culling loop
    // only for the one frame ArmFaces requested (RequestViewFrame), and the
    // request auto-expires — a probe between bakes costs nothing per frame
    // and there is no disarm step. Each armed face is one color cull slice of
    // that view (ScheduleCulling fans the armed faces into fused groups), one
    // Color-table scatter slice (a single bucketer call registers them all),
    // and one world pass drawing only its own slice; extraction, the light
    // buffer and the batch keys are shared.
    //   Idle --BakeDue--> Arming (request the view for all six faces; NEXT
    //   frame's extraction/culling feed it and the full bake runs that same
    //   frame) --> Idle (the request lapses).
    // Realtime probes rebake at most once per interval, and only while the
    // world changes (SceneProbeRecaptureSchedule) plus two convergence bakes after it
    // stops. Realtime REBAKES (a valid bake exists and UpdateMode == Realtime)
    // time-slice instead: Idle --BakeDue--> Slicing, which arms ONE face per
    // frame and declares only that face's capture, then the mips + convolves
    // after face 5 — so an animated sun costs one world pass per frame
    // instead of six, and the VISIBLE result (irradiance + prefilter) still
    // updates atomically at cycle end. The digest is consumed once at face 0
    // and held for the cycle; a sun still moving simply starts the next
    // cycle. First bakes and Once probes keep the single-frame path (no
    // pop-in of a half-baked cube).
    // A realtime probe with interval 0 captures EVERY frame: it skips the
    // rebake throttle, takes the single-frame path, and re-arms all six faces
    // as soon as a bake is declared, so Arming never falls back to Idle.
    enum class CaptureState : uint8_t
    {
        Idle,
        Arming,
        Slicing,
    };

    bool EnsureViews();
    // Request the view frame + world/mask stamp, and record which faces the
    // next frame's ScheduleCulling submits. No state change.
    void ArmFaces(uint32_t faceMask);
    void ArmViews(); // ArmFaces(all six); -> Arming
    // Per-frame shared prelude for the armed faces: camera/mask/world stamp,
    // light buffer, batch keys, and the ONE bucketer call registering a
    // Color-table slice per face. Must precede every DeclareFaceCapture.
    bool DeclareCaptureFrame(const EnvironmentBakeContext& ctx, uint32_t faceMask);
    // One face's world capture: sky background face, face camera + clear,
    // this face's forward emit, and the world pass drawing the face's slice
    // into envCube layer `face`.
    bool DeclareFaceCapture(const EnvironmentBakeContext& ctx,
                            GameEngine::Rendering::RenderGraph::RGTexture envCube,
                            uint32_t face);
    // Mip downsample chain + diffuse/specular convolves + digest stamp.
    void DeclareFinalizePasses(const EnvironmentBakeContext& ctx,
                               GameEngine::Rendering::RenderGraph::RGTexture envCube,
                               GameEngine::Rendering::RenderGraph::RGTexture irrCube,
                               GameEngine::Rendering::RenderGraph::RGTexture prefCube,
                               uint64_t bakedDigest);
    bool BakeDue() const;
    bool CapturesEveryFrame() const;
    void TickBakeClock(float deltaTimeSeconds);
    bool EnsurePipelines(GameEngine::Rendering::IDevice& device,
                         const ImageBasedLightingFeature& feature);
    GameEngine::Rendering::CameraData MakeFaceCamera(uint32_t face) const;
    uint64_t BaseDigest() const;
    SceneProbeWorldEpochs WorldEpochs() const;
    // The schedule's interval argument: < 0 for a Once probe.
    float RealtimeInterval() const;
    uint64_t NextBakeDigest();
    void RecordMipDownsample(GameEngine::Rendering::RenderGraph::RGContext& ctx,
                             ImageBasedLightingFeature& feature, uint32_t dstMip) const;
    void RecordDiffuseConvolve(GameEngine::Rendering::RenderGraph::RGContext& ctx,
                               ImageBasedLightingFeature& feature) const;
    void RecordSpecularPrefilter(GameEngine::Rendering::RenderGraph::RGContext& ctx,
                                 ImageBasedLightingFeature& feature, uint64_t bakedDigest) const;

    RenderServices* m_Services = nullptr;
    SkyEnvironmentSource m_SkyBackground;
    SceneReflectionProbeEnvironmentDesc m_Desc{};
    GameEngine::Rendering::CameraId m_CameraId = 0;
    GameEngine::Rendering::ViewId m_ViewId = 0;
    bool m_ViewsAllocated = false;
    CaptureState m_CaptureState = CaptureState::Idle;
    uint32_t m_ArmedFaceMask = 0; // faces the current arm covers (bit = face)
    uint32_t m_SliceFace = 0;     // next face to capture while Slicing
    uint64_t m_SliceDigest = 0;   // digest the current slice cycle bakes FOR
    uint32_t m_CaptureGeneration = 0;
    SceneProbeRecaptureSchedule m_Schedule;

    GameEngine::Rendering::ComputePipelineId m_MipDownsamplePipelineId{};
    GameEngine::Rendering::ComputePipelineId m_DiffusePipelineId{};
    GameEngine::Rendering::ComputePipelineId m_SpecularPipelineId{};
    uint32_t m_PipelineCaptureResolution = 0;
    uint32_t m_PipelinePrefilterMipCount = 0;
    bool m_PipelinesReady = false;
};

} // namespace Engine::Renderer
} // namespace GameEngine
