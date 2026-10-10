#pragma once

#include "Ocean/OceanShapeSampleInputs.h"
#include "Ocean/OceanTypes.h"
#include "Ocean/OceanSplineRaster.h"
#include "Ocean/OceanWaterMaterial.h"

#include "Rendering/Core/Device.h" // PipelineDesc, DescriptorSetLayoutDesc
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h" // GraphicsPipelineId

#include <array>
#include <cstdint>
#include <vector>

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
} // namespace GameEngine::Engine::Renderer::Pipeline

namespace GameEngine::Ocean
{

struct OceanCascadeLayoutGPU;
struct OceanUnderwaterSettings;

inline constexpr uint32_t kMaxOceanUnderwaterPortalVolumes = 8u;
inline constexpr uint32_t kMaxOceanUnderwaterPortalExclusions = 4u;
inline constexpr uint32_t kMaxOceanUnderwaterPortalOccluders = 8u;
inline constexpr uint32_t kMaxOceanUnderwaterPortalPolygons = 4u;
inline constexpr uint32_t kMaxOceanUnderwaterPortalPolygonPoints = 8u;

struct OceanUnderwaterPortalBox
{
    float CenterX = 0.0f, CenterY = 0.0f, CenterZ = 0.0f;
    float HalfX = 0.0f, HalfY = 0.0f, HalfZ = 0.0f;
};

struct OceanUnderwaterPortalPolygon
{
    float SurfaceY = 0.0f;
    float Depth = 0.0f;
    uint32_t PointCount = 0;
    float X[kMaxOceanUnderwaterPortalPolygonPoints] = {};
    float Z[kMaxOceanUnderwaterPortalPolygonPoints] = {};
};

struct OceanUnderwaterPortalData
{
    bool Enabled = false;
    std::vector<OceanWaterMaterialGPU> Materials;
    std::vector<OceanRibbonTriangleGPU> Ribbons;
    std::vector<OceanUnderwaterPortalBox> Volumes;
    std::vector<OceanUnderwaterPortalPolygon> Polygons;
    std::vector<OceanUnderwaterPortalBox> Exclusions;
    std::vector<OceanUnderwaterPortalBox> Occluders;
};

/// True when any portal volume of `data` (box, polygon prism or spline ribbon)
/// may be visible through the view-projection `viewProj` (16 floats, the
/// CameraData layout). A dry camera needs the underwater composite only then.
/// Ribbons are tested by the bounds of their tree's root record.
bool IsUnderwaterPortalInView(const OceanUnderwaterPortalData& data, const float* viewProj);

// Fullscreen underwater overlay (Phase 7). When the camera is submerged the
// render node CPU-gates this pass on; it tints the frame with a submerged
// colour, fades distant geometry into the deep water by per-channel depth fog
// (reusing the surface DepthFogDensity from below), and paints a bright meniscus
// band along the screen-space waterline.
//
// Mirrors OceanSceneGrab/VolumetricFog composite: a render-graph copy of the
// scene colour, then a fullscreen draw reading that copy + scene depth and
// writing back into SceneColor. The pipeline (a runtime-compiled vs+fs program,
// like the FFT compute) is feature-owned; the per-frame params live in the
// frame's render-graph upload ring. If SceneColor or the resolved depth can't
// be resolved (a project rendergraph stripped the post phase), the pass
// declines cleanly — no underwater overlay, no crash.
class OceanUnderwater
{
public:
    ~OceanUnderwater();

    // Schedules the underwater composite for this view at the post/composite
    // phase. `submergedDepth` is how far the camera sits below the displaced
    // surface (meters, >= 0). Returns true when the pass was scheduled.
    bool DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d, const OceanParamsGPU& params,
                        float submergedDepth,
                        const OceanShapeSampleInputs& shape,
                        ::GameEngine::Rendering::TextureHandle caustics,
                        ::GameEngine::Rendering::SamplerHandle causticsSampler,
                        const OceanUnderwaterSettings& settings,
                        const OceanUnderwaterPortalData* portalData = nullptr);

    bool IsReady() const { return m_PipelineId.IsValid(); }

private:
    // Lazily compile the fullscreen vs+fs program (runtime ShaderCompileService,
    // reading from the Ocean shader dir) + the linear-clamp sampler. Returns
    // false (declines) when the sources are missing or compilation fails.
    bool EnsurePipeline(::GameEngine::Rendering::IDevice& device);

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;

    // Overlay program (runtime-compiled vs+fs): reads the scene snapshot + depth,
    // writes the fogged/tinted/meniscus frame back into SceneColor.
    ::GameEngine::Rendering::PipelineDesc m_Pipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_PipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout{};

    // Shadowed overlay variant: same fullscreen pass, plus CSM visibility for
    // Wicked-style volumetric underwater shafts when the frame produced shadows.
    ::GameEngine::Rendering::PipelineDesc m_ShadowPipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_ShadowPipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_ShadowLayout{};

    // Scene-copy program (stock fullscreen copy.shaderpkg): snapshots SceneColor
    // so the overlay can read the scene while writing back into the same target.
    ::GameEngine::Rendering::PipelineDesc m_CopyPipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_CopyPipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_CopyLayout{};

    // MSAA-resolve program (sampler2DMS averaging) for the scene copy when SceneColor
    // is multisampled — so the underwater overlay works with MSAA on (a sampler2D copy
    // can't read MSAA). Used in place of the copy pipeline then.
    ::GameEngine::Rendering::PipelineDesc m_ResolvePipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_ResolvePipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_ResolveLayout{};

    bool m_PipelineLoadAttempted = false;
    bool m_WarnedLoadFailed = false;

    ::GameEngine::Rendering::SamplerHandle m_Sampler;
    ::GameEngine::Rendering::TextureHandle m_DummyArrayTexture;
    // 2D sibling of the array dummy: the caustics fallback (the fullscreen
    // helper binds per-input fallbacks, so it can't reuse the scene snapshot).
    ::GameEngine::Rendering::TextureHandle m_DummyTexture2D;
};

} // namespace GameEngine::Ocean
