#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Rendering::RenderGraph
{
class RGFrame;
} // namespace Rendering::RenderGraph
namespace Rendering
{
class IDevice;
} // namespace Rendering

namespace Engine::Renderer
{
class ImageBasedLightingFeature;
struct FeatureCullingContext;

struct EnvironmentLocalReflectionData
{
    bool BoxProjection = false;
    float ProbePositionWS[3] = {0.0f, 0.0f, 0.0f};
    float BoxCenterWS[3] = {0.0f, 0.0f, 0.0f};
    float BoxHalfExtentsWS[3] = {0.0f, 0.0f, 0.0f};
    float BoxAxisXWS[3] = {1.0f, 0.0f, 0.0f};
    float BoxAxisYWS[3] = {0.0f, 1.0f, 0.0f};
    float BoxAxisZWS[3] = {0.0f, 0.0f, 1.0f};
};

// Context handed to an environment source when the IBL generation node asks it
// to (re)generate the environment for a frame. Carries this frame's RGFrame (the
// source declares its bake passes onto it immediately, digest-gated like the sky
// LUTs) and the feature that owns the capture cube / IBLSet the source writes into.
struct EnvironmentBakeContext
{
    GameEngine::Rendering::RenderGraph::RGFrame* Frame = nullptr;
    GameEngine::Rendering::IDevice* Device = nullptr;
    ImageBasedLightingFeature* Feature = nullptr;
    float DeltaTimeSeconds = 0.0f;
};

// Produces the environment that image-based lighting samples. The IBL feature
// owns one active source; the source fills the feature's capture cube — which
// the feature then convolves into an irradiance cube + a prefiltered specular
// cube — or, for a pre-convolved source, supplies those cubes directly.
//
// The split-sum consumer shader (Includes/ibl.glsl) and the IBLSet binding are
// source-agnostic: they never change as sources are added. Today's only
// implementation is the analytic sky (SkyEnvironmentSource). Future sources —
// an HDRI backdrop, a captured reflection probe, a baked KTX2 set — implement
// this same contract.
class IEnvironmentSource
{
  public:
    virtual ~IEnvironmentSource() = default;

    // Stable identifier for logging / debug overlays.
    virtual const char* Name() const = 0;

    // Digest of the inputs that determine this source's output. When it matches
    // the previous frame's value, the generation node skips the rebake. (Sky:
    // sun direction + atmosphere; HDRI: asset hash; baked: a constant.)
    virtual uint64_t InputDigest() const = 0;

    // True iff this source currently has content to light with. The cheap predicate for
    // "is the IBL live", so the generation node can zero the IBL intensity without hashing
    // InputDigest() every frame just to test its no-content sentinel (InputDigest() == 0).
    // Sky: an active sky; HDRI: a loaded asset.
    virtual bool HasActiveContent() const = 0;

    // Dynamic multiplier applied by ImageBasedLightingFeature's EnvData UBO. Source
    // implementations keep this out of InputDigest when changing intensity does not
    // alter the baked cubemaps.
    virtual float IblIntensity() const { return 1.0f; }

    // Runtime lower-hemisphere occlusion applied by ordinary IBL consumers.
    // Keeping this out of the baked cubemaps makes the control non-destructive:
    // specialized surfaces such as an unbounded ocean can opt out without
    // requiring a second environment capture.
    virtual float LowerHemisphereDarkness() const { return 0.0f; }

    // Three-color ambient gradient tint (scene-linear RGB) the IBL consumer multiplies
    // into the diffuse irradiance ONLY (specular stays physical), blended by world-normal.y
    // (up=sky, horizon=equator, down=ground). Applied at sampling time via the EnvData UBO,
    // so it is deliberately kept OUT of InputDigest (changing it never re-bakes). White =
    // identity: the default leaves every consumer byte-identical.
    virtual void AmbientGradientTint(float outSky[3], float outEquator[3], float outGround[3]) const
    {
        for (int i = 0; i < 3; ++i)
        {
            outSky[i] = 1.0f;
            outEquator[i] = 1.0f;
            outGround[i] = 1.0f;
        }
    }

    // Optional local-reflection projection data consumed by the IBL shader. The
    // default is a distant/global environment with no parallax correction.
    virtual EnvironmentLocalReflectionData LocalReflectionData() const { return {}; }

    // Schedule the GPU work that fills the feature's capture cube for this frame.
    // Invoked by the IBL generation node only when the digest changed or on first
    // use; the feature then convolves the capture cube into the IBLSet.
    virtual void ScheduleBake(const EnvironmentBakeContext& ctx) = 0;

    // GPU culling submissions for the source's own capture views, forwarded from
    // ImageBasedLightingFeature::OnScheduleCulling once per frame (before
    // GPUCullingPipeline::EndFrame). A source that captures the scene submits
    // its capture frusta here; an analytic sky has nothing to cull.
    virtual void ScheduleCulling(const FeatureCullingContext& /*ctx*/) {}
};

} // namespace Engine::Renderer
} // namespace GameEngine
