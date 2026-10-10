#pragma once

#include "Engine/Rendering/IEnvironmentSource.h"
#include "Rendering/Core/PipelineIdentifiers.h" // GraphicsPipelineId, ComputePipelineId
#include "Rendering/Core/RenderGraph/RGFrame.h" // RGContext
#include "Rendering/Sky/SkySettings.h"

#include <cstdint>
#include <optional>

namespace GameEngine
{
namespace Rendering
{
class IDevice;
} // namespace Rendering

namespace Engine::Renderer
{
class RenderServices;
class SkyRenderFeature;
class ImageBasedLightingFeature;

struct SkyEnvironmentProbeOverrides
{
    float Intensity = 1.0f;
    float ExposureEV = 0.0f;
    float RotationRadians = 0.0f;
    float LowerHemisphereDarkness = 1.0f;
};

// Captures the atmospheric dynamic sky into the IBL feature's environment cube,
// then convolves it into the diffuse irradiance cube + the GGX-prefiltered
// specular cube so metals and dielectrics reflect the SAME sky the user sees.
// The capture samples the renderer's atmospheric sky-view LUT (scattering +
// multiscatter) and composites the shared ground/night blend, minus the bright
// discs (sun/moon/stars) so the prefilter never fireflies. The first concrete
// IEnvironmentSource; future HDRI / probe / baked sources fill the same cubes
// through this same contract, after which the source-agnostic convolve passes can
// be hoisted out of here.
//
// InputDigest hashes exactly the sun + atmosphere inputs the sky-view LUT + the
// ground/night composite consume, so the bake only re-runs when the sky actually
// changes (a static sky in an idle editor bakes once).
class SkyEnvironmentSource final : public IEnvironmentSource
{
  public:
    explicit SkyEnvironmentSource(RenderServices& services) : m_Services(&services) {}

    const char* Name() const override { return "SkyEnvironmentSource"; }
    uint64_t InputDigest() const override;
    bool HasActiveContent() const override { return HasActiveSky(); }
    float IblIntensity() const override;
    float LowerHemisphereDarkness() const override;
    void AmbientGradientTint(float outSky[3], float outEquator[3], float outGround[3]) const override;
    void ScheduleBake(const EnvironmentBakeContext& ctx) override;

    // Digest of the sky state the renderer's sky-view LUT currently CONTAINS
    // (0 until first computed / no active sky). InputDigest() hashes the LIVE
    // settings, which a capture declared on the change frame can race — the
    // LUT recompute may land a frame later (or in another window's stream),
    // so the capture samples the previous sky. Digest-driven captures fold
    // this in so the LUT catching up re-triggers them.
    uint64_t MaterializedLutDigest() const;

    void SetProbeOverrides(const SkyEnvironmentProbeOverrides& overrides);
    void ClearProbeOverrides();
    bool HasProbeOverrides() const { return m_ProbeOverrides.has_value(); }

    // faceFilter: UINT32_MAX declares all six faces; a face index declares only
    // that face (probe time-slicing bakes one face per frame).
    bool ScheduleCaptureFacesOnly(const EnvironmentBakeContext& ctx, const char* passPrefix,
                                  int32_t phase,
                                  uint32_t faceFilter = UINT32_MAX);

  private:
    SkyRenderFeature* GetSky() const;
    bool HasActiveSky() const;
    GameEngine::Rendering::SkySettings GetEffectiveSettings() const;
    // Intern the capture graphics pipeline + the two convolve compute pipelines
    // and load their SPIR-V (once). Returns false until all are available.
    bool EnsurePipelines(GameEngine::Rendering::IDevice& device,
                         const ImageBasedLightingFeature& feature);

    // Per-pass execute bodies (record into the pass's command list).
    // Per-face capture UBO allocs, written into the frame upload ring at
    // declaration (SkyUBO block + AtmosphereUBO block). The ring guarantees
    // no aliasing across in-flight frames — replaces the old per-(face,
    // frame-slot) persistent buffer ring on ImageBasedLightingFeature.
    struct CaptureFaceUploads
    {
        GameEngine::Rendering::BufferHandle SkyBuffer{};
        uint64_t SkyOffset = 0;
        GameEngine::Rendering::BufferHandle AtmoBuffer{};
        uint64_t AtmoOffset = 0;
        bool Valid() const { return SkyBuffer.IsValid() && AtmoBuffer.IsValid(); }
    };
    // Fill both capture UBO blocks for `face` into the frame upload ring.
    // Called at declaration; the returned allocs ride the exec closure.
    CaptureFaceUploads AllocCaptureFaceUploads(
        GameEngine::Rendering::RenderGraph::RGFrame& frame, uint32_t face) const;
    void RecordCaptureFace(GameEngine::Rendering::RenderGraph::RGContext& ctx,
                           ImageBasedLightingFeature& feature,
                           const CaptureFaceUploads& uploads) const;
    // Box-downsample the env cube's mip `dstMip` from `dstMip - 1`. Run for each
    // mip 1..N-1 after the capture renders mip 0, so the convolve/prefilter read a
    // pre-blurred chain (kills the residual horizon-glow banding).
    void RecordMipDownsample(GameEngine::Rendering::RenderGraph::RGContext& ctx,
                             ImageBasedLightingFeature& feature, uint32_t dstMip) const;
    void RecordDiffuseConvolve(GameEngine::Rendering::RenderGraph::RGContext& ctx,
                               ImageBasedLightingFeature& feature) const;
    void RecordSpecularPrefilter(GameEngine::Rendering::RenderGraph::RGContext& ctx,
                                 ImageBasedLightingFeature& feature, uint64_t bakedDigest) const;

    RenderServices* m_Services = nullptr;
    std::optional<SkyEnvironmentProbeOverrides> m_ProbeOverrides{};

    GameEngine::Rendering::GraphicsPipelineId m_CapturePipelineId{};
    GameEngine::Rendering::ComputePipelineId m_MipDownsamplePipelineId{};
    GameEngine::Rendering::ComputePipelineId m_DiffusePipelineId{};
    GameEngine::Rendering::ComputePipelineId m_SpecularPipelineId{};
    uint32_t m_PipelineCaptureResolution = 0;
    uint32_t m_PipelinePrefilterMipCount = 0;
    bool m_PipelinesReady = false;
};

} // namespace Engine::Renderer
} // namespace GameEngine
