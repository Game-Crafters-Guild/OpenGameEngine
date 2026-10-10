#pragma once

#include "Engine/Rendering/CascadeMotionScheduler.h"
#include "Engine/Rendering/CascadeShadowCache.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/ShadowMinMaxPyramid.h"
#include "Engine/Rendering/ShadowReceiverMeasurement.h"
#include "Engine/Rendering/TerrainShadowMap.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/PerViewReadbackRings.h"
#include "Rendering/Core/RenderGraph/RGFrame.h" // RGFrame, RGFrameStamp

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
namespace RenderGraph = ::GameEngine::Rendering::RenderGraph;
using ::GameEngine::Rendering::BufferHandle;
using ::GameEngine::Rendering::CameraData;
using ::GameEngine::Rendering::IDevice;
using ::GameEngine::Rendering::SamplerHandle;
using ::GameEngine::Rendering::TextureHandle;
using ::GameEngine::Rendering::TextureViewHandle;
using ::GameEngine::Rendering::ViewId;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{
namespace Engine::Renderer
{
class RenderServices;
struct ExtractedLight;
struct FeatureDeclareContext;

static constexpr uint32_t kMaxShadowCascades = 4;
static_assert(kMaxShadowCascades == CascadeShadowCache::kMaxCascadesPerView,
              "CascadeShadowCache slot layout assumes the cascade ceiling");

// How the cascade projection trades stability against resolution. Mirrors the
// same choice Unity exposes as Shadow Projection, and for the same reason: the
// two goals are in direct conflict and only the project can say which it wants.
enum class ShadowProjection : int
{
    // Splits are fixed (MaxShadowDistance + SplitLambda only) and the ortho
    // extent is the slice's bounding-sphere radius. Both are then invariant to
    // camera motion and orientation, so worldPerTexel never changes and the
    // shadow texel lattice stays welded to the world — no crawling edges.
    Stable = 0,
    // Splits follow the SDSM depth bounds and the extent is a tight light-space
    // AABB. Sharper — the cascade only covers what is actually visible — but both
    // inputs move as the camera moves, so worldPerTexel steps and every texel
    // re-quantizes at once. That is the classic shadow shimmer.
    Close = 1,
    // Texel size is AUTHORED and the coverage is derived from it, inverting the
    // relationship the other two share. halfExtent = resolution * texelSize / 2,
    // and the splits fall out of where each fixed box stops covering the frustum
    // slice; MaxShadowDistance becomes a readout and a hard clamp.
    //
    // This supersedes the Stable-vs-Close trade rather than picking a side.
    // Stable buys a texel size that cannot move by bounding a ROTATING slice
    // with a rotation-invariant sphere, paying ~19% coarser texels than the mean
    // AABB. An authored extent cannot breathe, so nothing needs to be made
    // invariant and there is no premium to pay.
    //
    // It is also what welds the lattice. The centre snap is
    // round(centre / texelWorldSize) * texelWorldSize; while texelWorldSize is
    // DERIVED, the quantum moves with the fit and the "grid" being snapped to is
    // not a fixed grid. Author it and the quantum is constant, so the snap lands
    // on a genuine world lattice and the cascade window slides across it in
    // integer texel steps.
    WorldTexel = 2,
};

struct CascadedShadowConfig
{
    uint32_t NumCascades = 4;
    // Defaults to Close, which is what shipped before the mode existed.
    //
    // Stable is not free and the price is content-dependent: it fits the slice's
    // bounding SPHERE, and for a long thin slice -- a camera near the ground
    // looking along it -- that sphere is far larger than the slice's light-space
    // AABB. Measured on ShadowStress at a ground-level pose, cascade 0 went
    // halfExtent 8 -> 20, i.e. 2.5x coarser texels, which turns smooth shadow
    // edges into visible texel stair-steps. That is a worse artifact than the
    // crawl it removes, so Stable is opt-in until a project has tightened its
    // splits enough to absorb it (SplitLambda ~0.92 more than repays it on the
    // same content: halfExtent 6.5, finer than Close's 8).
    ShadowProjection Projection = ShadowProjection::Close;
    uint32_t Resolution = 2048;
    // MSM4 moments-array resolution per cascade. 1024 is the default —
    // it's the quality sweet spot: high enough that the moment field
    // captures sharp depth discontinuities cleanly, low enough that the
    // texture stays at 32 MiB per view. 512 trades visible aliasing on
    // contact shadows for 4x less memory, and 2048 yields fine penumbra
    // detail at 4x the cost (recommend only for closeup scenes).
    // Allowed: 512, 1024, 2048. Set via .rendergraph (key
    // "momentsResolution"), inspector dropdown, or programmatically.
    uint32_t MomentsResolution = 1024;
    // Superseded by Cascade0TexelSize/CascadeTexelRatio under
    // ShadowProjection::WorldTexel, where the splits come from coverage and this
    // has nothing to do. Retained (and still honoured by Stable/Close) until the
    // settings-unification pass removes it from the volume and scene schema.
    float SplitLambda = 0.5f;
    float MaxShadowDistance = 100.0f;
    // WorldTexel authoring surface. texel_i = Cascade0TexelSize * ratio^i, and
    // halfExtent_i = Resolution * texel_i / 2. Two numbers rather than four so a
    // bad ladder cannot leave gaps between cascades.
    //
    // An explicit texel size is the DEFAULT and the verified path: distance then
    // changes only coverage and sharpness never moves, which already removes the
    // SplitLambda/MaxShadowDistance coupling.
    //
    // 0 selects AUTO — solve the ladder to span MaxShadowDistance, so shortening
    // the range sharpens shadows automatically. AUTO is NOT verified: at runtime
    // it resolves against a distance of ~2.3 m rather than the configured 200 m,
    // producing a ladder ~80x too fine, and the cause is not yet found. The pure
    // solver is unit-tested and correct for the inputs it is given, and the
    // explicit path is measured correct end to end, so the fault is in what
    // reaches the solve. Do not default to AUTO until that is understood.
    float Cascade0TexelSize = 0.02f;
    float CascadeTexelRatio = 3.0f;
    // Always overwritten by ShadowMapNode (blueprint or volume override) before
    // the cascade fit; kept equal to the node default for auditability.
    float DepthBias = 0.0001f;
    float NormalBias = 0.02f;
};

// Angular diameter (degrees, full disc) -> tan of the half angle: the
// dimensionless slope the penumbra relation multiplies a world-space depth
// delta by. A directional light is at infinity, so its penumbra is
// depthDelta * tan(halfAngle) with no divide by blocker distance — which is
// what makes an angle, not a length, the physically meaningful control.
// Clamped at zero: a negative authored diameter is meaningless and must not
// become a positive tangent.
inline float ResolveShadowTanHalfAngle(float angularDiameterDegrees)
{
    constexpr float kDegToRad = 3.14159265358979f / 180.0f;
    const float halfRad = std::max(angularDiameterDegrees, 0.0f) * 0.5f * kDegToRad;
    return std::tan(halfRad);
}

// Per-frame computed cascade data ready for GPU upload.
struct CascadeFrameData
{
    // The cascade fit with the render origin added back to its eye — world
    // space, for the world-space caster culling in OnScheduleCulling only (cull
    // planes, scene-bounds AABB, caster footprint tighten). Its translation
    // column inherits ULP(|eye|): 0.5 m at Earth radius, which the cull widens
    // its footprint by, and far coarser than a shadow texel, so nothing that
    // samples the shadow map may use it.
    Mathematics::Matrix4x4 LightVP[kMaxShadowCascades];
    // Camera-relative light VP per cascade (Earth-scale precision) — the fit as
    // actually built, consuming render-origin-relative positions exactly as
    // ViewRegistry rebases the camera into CameraData::viewProjRel. The depth
    // pass projects reconstructed relative caster positions through this, and the
    // receiver (ge_shadowVP) samples with the fragment's relative position — so a
    // caster and its receiver agree to fp32-of-small-magnitude instead of the
    // big·big cancellation that speckles self-shadows at planetary distance, and
    // the clip-space texel snap can resolve the grid at any camera magnitude.
    // When the render origin is inactive (RenderOriginSector all zero) this is a
    // byte-for-byte copy of LightVP (the dark-ship gate).
    Mathematics::Matrix4x4 LightVPRel[kMaxShadowCascades];
    float SplitDistances[kMaxShadowCascades]{};           // view-space split planes
    // PCSS metrics per cascade: orthographic half-extent (world units) and
    // depth-range span (world units). Used to convert blocker depth delta
    // into a physical penumbra size in texels.
    float OrthoHalfExtent[kMaxShadowCascades]{};
    float DepthSpan[kMaxShadowCascades]{};
    // tan(half angular diameter) of the primary directional light —
    // dimensionless. Penumbra width is depthDeltaWorld * this, and the blocker
    // search radius is this * the searched depth range. Uniform across cascades
    // by construction: a light's angular size does not depend on which cascade a
    // fragment lands in. A LIGHT property, so — like NumCascades — it is applied
    // at the ShadowMapNode call site rather than inside ComputeCascades, which
    // owns the fit and only fills feature-config fields.
    float ShadowTanHalfAngle = 0.0f;
    uint32_t NumCascades = 0;
    float DepthBias = 0.001f;
    float NormalBias = 0.02f;
    float MaxShadowDistance = 100.0f;
    // Render origin sector the LightVPRel matrices (and the depth-pass shadow
    // camera) were rebased against — the camera's sector for this frame. Zero =>
    // origin inactive => LightVPRel == LightVP and the whole path is byte-identical
    // to the pre-feature build. Mirrors CameraData::renderOriginSector.
    int32 RenderOriginSector[3]{};

    // ── Per-cascade cull-content inputs (fit-freeze lane) ──
    // Everything the cascade CULL consumes besides the fit, snapshotted per
    // cascade. With the fit freeze ON these hold the FROZEN snapshot captured
    // when the cascade last refit — the caster footprint, the GPU LOD selection
    // camera, and the static-cache content key all read them, so a cascade's
    // rendered content is a pure function of fields that only step when the
    // cascade actually refits. With the freeze OFF they carry this frame's
    // live values.
    Mathematics::Matrix4x4 CullCameraViewProj[kMaxShadowCascades];
    // What the cascade's casters are culled to: the light-NDC rectangle
    // {xMin, yMin, xMax, yMax}, in this cascade's own fit, of the receivers the
    // fit was built to hold (the measured receivers, or the frustum slice)
    // widened by a shadow lookup's reach (CascadeFitBounds::CasterBox). Under
    // an orthographic light a caster outside it shadows nothing the cascade
    // serves. Applies when CasterFootprintApplies
    // (TightenCascadeSidePlanes); a caster cull for something other than mesh
    // instances (a terrain caster grid) reads the same rectangle.
    float CasterFootprint[kMaxShadowCascades][4]{};
    bool CasterFootprintApplies[kMaxShadowCascades]{};
    // GPU LOD selection inputs for this cascade's bucketer slice (xyz + the
    // 2D-ortho flag in w), plus |proj[1][1]| — MakeViewLODParams' inputs.
    float LodCameraPos[kMaxShadowCascades][4]{};
    float LodProjScaleY[kMaxShadowCascades]{};
};

// Shadow debug modes matching ge_shadowDebug.x in shadow_sampling.glsl.
enum class ShadowDebugMode : int
{
    Off = 0,           // Normal shadows
    CascadeColors = 1, // Tint fragments by cascade index
    ShadowFactor = 2,  // Grayscale shadow factor (ignore lighting)
    // PCSS min/max-pyramid branch classification: green = early-out fully
    // lit, red = early-out fully shadowed, blue = full penumbra path. Flat,
    // unmodulated colors (not scaled by the shadow factor) so a fully-shadowed
    // early-out — whose factor is exactly 0.0 — stays visibly red instead of
    // going black and hiding in the rest of the shadowed scene.
    PcssBranch = 3,
    BaseShadowFactor = 4, // Cascade/RT visibility before contact shadows
    ContactShadowFactor = 5, // Screen-space visibility alone
    ContactOnly = 6, // Lit materials with only screen-space shadows
    BaseOnly = 7, // Lit materials with only cascade/RT shadows
    Count = 8,
};

// Shadow filter quality. Numerical values match the integer dispatched on
// in shadow_sampling.glsl::GE_SampleCascade via ge_shadowDebug.w. PCSS and
// MSM4 silently fall back to PCF when their prerequisites (bindless / a
// ready moments texture) aren't met. Default is PCSS to match the engine's
// effective default before the explicit-enum migration.
enum class ShadowFilterQuality : int
{
    Grid5x5 = 0,
    Grid3x3 = 1,
    PoissonPCF = 2,
    PCSS = 3,
    MSM4 = 4,
    // Dilated PCF (Treyarch, Cold War). Contact hardening from ONE tap set: the
    // occluder count and the occluder-distance sum come from the same gathered
    // depths, so there is no blocker-search pass and no dependent texture read —
    // which is why it fit a 60 Hz gen8 budget where PCSS did not. Softer
    // guarantee than PCSS in exchange: it approximates contact hardening rather
    // than deriving a physical penumbra, so it sits ALONGSIDE PCSS as a
    // cost/quality point, not above it.
    DPCF = 5,
};

// What the per-pixel Vogel rotation is keyed on. Numeric values are part of the
// GPU contract: the shader reads this as a float from ge_shadowFilterParams.x
// and compares against 0.5, so they may not be reordered.
//
// The two trade against each other and neither wins outright:
//   Screen      keys on gl_FragCoord, so the noise field is nailed to the
//               SCREEN and geometry slides under it as the camera moves — the
//               swim. Its spatial frequency is high, which is the property
//               shadow space gives up.
//   ShadowSpace keys on the continuous shadow coordinate, welding the field to
//               the shadow lattice (and through the centre texel snap, to the
//               world). Under magnification adjacent screen pixels map to
//               nearby shadow coords and decorrelate LESS, so the noise drops
//               in frequency — blotchy rather than grainy.
//
// Which reads better is range-dependent and a look judgement, so there is
// deliberately no "correct" default. Screen is the default only because it is
// what shipped.
enum class ShadowDitherBasis : int
{
    Screen = 0,
    ShadowSpace = 1,
};

// MSM4 separable Gaussian blur quality. Selects between a perf-optimised
// linear-sampled 5-tap kernel and a more conservative discrete 9-tap
// kernel. Both target the same sigma=2 Gaussian; the 5-tap version uses
// bilinear sampling between texel pairs to combine adjacent taps with
// no perceptible quality loss for typical penumbra widths. Default is
// Linear5Tap because it cuts blur fragment work nearly in half with no
// visible difference; switch to Discrete9Tap if you need to A/B compare
// or eliminate the (very rare) bilinear-precision artefacts at sharp
// depth discontinuities.
enum class MsmBlurMode : int
{
    Linear5Tap = 0,
    Discrete9Tap = 1,
};

static constexpr float kPcssMaxPenumbraWorld = 2.0f;
// The PCSS kernel cap a Max Penumbra of 0 (auto) resolves to, in world units.
// Mirror of kPcssMaxPenumbraWorldAuto in shadow_sampling.glsl.
static constexpr float kPcssMaxPenumbraWorldAuto = 0.5f;

// GPU layout matching shadow_sampling.glsl ShadowData UBO.
struct alignas(16) ShadowDataGPU
{
    float shadowVP[kMaxShadowCascades][16]; // mat4 per cascade (column-major)
    float shadowSplits[4];                  // vec4: view-space split distances
    float shadowParams[4];                  // vec4: x=bias, y=normalBias, z=numCascades, w=maxDistance
    float shadowDebug[4];                   // vec4: x=debugMode (0=off, 1=cascade colors, 2=shadow factor)
    // vec4: x=softness multiplier (Poisson PCF fallback only — PCSS penumbra is
    //         driven by the light's angular size alone),
    //       y=PCSS / Poisson disk tap count (float-packed; cast to int and
    //         clamped to [8, 64] in the shader),
    //       z=maxPenumbraWorld (world-space upper clamp; converted per-cascade in shader),
    //       w=receiverPlaneBias (0/1 toggle, float-packed for UBO alignment).
    float shadowPcss[4];
    // Per-cascade PCSS metrics (precomputed on CPU):
    //   x = worldPerTexel, y = depthSpan, z = bindless index (PCSS raw depth),
    //   w = tan(half angular diameter) of the primary directional light —
    //       dimensionless, and identical on every cascade (a light's angular
    //       size does not depend on which cascade a fragment lands in).
    float shadowPcssCascades[kMaxShadowCascades][4];
    // Per-cascade PCSS min/max pyramid (ShadowMinMaxPyramid):
    //   x = bindless index of this cascade's pyramid view; NEGATIVE means the
    //       view has no pyramid this frame and the PCSS filter runs
    //       unaccelerated,
    //   y = mip levels in the pyramid,
    //   z = base downshift — level 0 covers a (1 << z) square of shadow texels,
    //   w = reserved (0).
    float shadowPcssPyramid[kMaxShadowCascades][4];
    // vec4: x = ShadowDitherBasis as a float (0 = screen, 1 = shadow space),
    //       y = authored distance fade fraction, zw reserved (0).
    // A named frame-global slot rather than a borrowed per-cascade word: the
    // basis does not vary by cascade, and a per-cascade home for it would read
    // as a bug to anyone who found it later.
    float shadowFilterParams[4];
    // The terrain's sun-space clearance map (TerrainShadowMap, terrain_shadow.glsl), zero when
    // none is published. Positions relative to the view's render origin, heights relative to the
    // terrain's base:
    //   terrainShadowGrid0: x, y = grid centre X, Z; z, w = terrain corner X, Z
    //   terrainShadowGrid1: x, y = terrain extent X, Z; z, w = sun horizontal direction X, Z
    //   terrainShadowGrid2: x = tan(elevation); y = texel (m); z, w = u of sample 0, v of line 0
    //   terrainShadowGrid3: x = samples per line; y = lines; z = the terrain's base height;
    //                       w = metres per normalized height
    //   terrainShadowSource: x = map bindless index; y = height texture bindless index;
    //                        z = 1 when present; w = map side (texels)
    float terrainShadowGrid0[4];
    float terrainShadowGrid1[4];
    float terrainShadowGrid2[4];
    float terrainShadowGrid3[4];
    uint32_t terrainShadowSource[4];
};
static_assert(sizeof(ShadowDataGPU) == 544, "ShadowDataGPU must be 544 bytes");

class ShadowMapRenderFeature : public IRenderFeature
{
  public:
    // Latest SDSM depth bounds (view-space distances).
    struct SDSMBounds
    {
        float nearDepth = 0.0f;
        float farDepth = 0.0f;
        bool valid = false;
    };

    ShadowMapRenderFeature();
    ~ShadowMapRenderFeature() override;

    bool Initialize(Rendering::IDevice* device, const CascadedShadowConfig& config);
    bool IsInitialized() const { return m_Initialized; }

    const CascadedShadowConfig& GetConfig() const { return m_Config; }

    // Changes the fixed directional cascade-map dimensions. RenderGraph pool
    // imports observe the new descriptor on the next frame and safely replace
    // the old physical after in-flight work retires.
    void SetResolution(uint32_t resolution);

    // Editor project override for the directional cascade-map dimensions.
    // Kept on the shadow feature (the texture owner), not on RenderServices.
    // It may be set before Initialize(); initialization then combines it with
    // the remaining pipeline-authored cascade config.
    void SetProjectResolutionOverride(uint32_t resolution);
    std::optional<uint32_t> GetProjectResolutionOverride() const
    {
        return m_ProjectResolutionOverride;
    }

    // Push per-frame directional shadow overrides (from a volume ShadowSettingsEffect)
    // into the cascade config. Only the four per-frame tunables are writable here;
    // NumCascades / Resolution / MomentsResolution stay fixed because they size the
    // pooled GPU shadow textures. ShadowMapNode calls this each frame before
    // ComputeCascades — with the resolved volume values when a ShadowSettingsEffect
    // is present, otherwise with the node's blueprint defaults — so toggling or
    // removing the volume reverts to the pipeline-authored look with no residue.
    void ApplyRuntimeShadowSettings(float maxShadowDistance, float splitLambda,
                                    float depthBias, float normalBias)
    {
        m_Config.MaxShadowDistance = maxShadowDistance;
        m_Config.SplitLambda = splitLambda;
        m_Config.DepthBias = depthBias;
        m_Config.NormalBias = normalBias;
    }

    // Scene world-bounds expressed in the RENDER-ORIGIN-RELATIVE frame the
    // cascade fit works in. Null = no usable bounds, and the fit then runs
    // exactly as it did before the scene clamp existed.
    struct SceneBoundsRel
    {
        Mathematics::Vector3 Min;
        Mathematics::Vector3 Max;
    };

    // Compute cascade splits and light VP matrices for the current frame.
    // `lightDir` is the world-space direction the light points (normalized).
    // `viewId` keys per-view rotation-stability state (the half-extent
    // hysteresis); pass 0 for one-off / non-persistent calls.
    // `sceneBounds` clamps each cascade's fit to the geometry — see
    // ComputeClampedLightSpaceBounds. Null leaves every fit byte-identical.
    // `receivers` is the view's latest SDSM measurement. Under the Close
    // projection cascade 0's slice starts at the nearest surface it measured
    // (Cascade0SliceStart) and its box holds the measured air in front of it
    // (HoldAirInFrontOfCascade0), and each other cascade's box narrows to the
    // surfaces and the air that can sample it (NarrowFitToReceivers). Null or
    // invalid fits the frustum slices from the camera near plane.
    CascadeFrameData ComputeCascades(const Rendering::CameraData& camera,
                                     float nearPlane, float farPlane,
                                     const Mathematics::Vector3& lightDir,
                                     const SDSMBounds* sdsmBounds = nullptr,
                                     Rendering::ViewId viewId = 0,
                                     const SceneBoundsRel* sceneBounds = nullptr,
                                     const ShadowReceiverMeasurement* receivers = nullptr);

    // View depth cascade 0's slice starts at, before its overlap: the nearest
    // receiver `receivers` measured, minus the motion of the `current` camera
    // since (ShadowReceiverMotionBound), within [nearPlane, split0]. At a
    // pitched RTS camera nothing is visible for the first tens of metres, and a
    // slice that started at the camera near plane spent cascade 0's texels on
    // them. The camera near plane when there is no usable measurement
    // (UsableReceivers).
    float Cascade0SliceStart(const ShadowReceiverMeasurement* receivers, float nearPlane,
                             float split0,
                             const ShadowReceiverMeasurement::Context& current) const;

    // Rotation-only light view basis (light at the origin, looking down the unit
    // `lightDirection`) that every cascade fit, and the SDSM receiver reduce,
    // express light space in.
    static Mathematics::Matrix4x4 CascadeLightRotation(const Mathematics::Vector3& lightDirection);

    // View depth (world units) before cascade `cascadeIndex`'s far split over
    // which a receiver also samples the next cascade and fades into it, as
    // GE_CascadeBlendBand (shadow_cascade_blend.glsl) selects it: a fixed
    // fraction of the cascade's depth range, cascade 0's measured from the
    // camera. 0 for the last cascade, which blends into nothing.
    static float CascadeBlendBand(const CascadeFrameData& frame, uint32_t cascadeIndex);

    // Populate the complete GPU-side ShadowData UBO for a view. Writes matrices,
    // splits, per-cascade PCSS metrics (worldPerTexel, depthSpan), and all
    // runtime scalar fields (debug mode, PCF quality, PCSS params). Promotes
    // quality to 3 (PCSS) when PCSS is enabled and bindless is available.
    // Single entry point — future shadow consumers can't forget a phase.
    // `rgFrame` is the declaring render-graph frame — needed because the PCSS
    // promotion registers bindless views of the adopted cascade physical, which
    // is only servable for the frame it was adopted for.
    void BuildShadowDataGPU(const CascadeFrameData& cascadeFrame,
                            Rendering::ViewId viewId, RenderServices& rs,
                            RenderGraph::RGFrame& rgFrame,
                            uint32_t frameIndex, ShadowDataGPU& out);

    // Content-fit snapshot of one cascade layer: the world-space fit of the
    // depth content ACTUALLY retained in that layer plus the PCSS metrics
    // matching it. Public only because SelectCascadeSamplingFit consumes it;
    // the per-view snapshot maps stay private (see m_CascadeContentFit for
    // the invariant and lifecycle).
    struct CascadeContentFit
    {
        Mathematics::Matrix4x4 LightVP; // world-space fit of the retained layer
        // The render-origin-relative fit that actually rasterized the retained
        // layer, plus the origin sector it is relative to. Receivers must
        // transform by THIS matrix (re-anchored across any sector step via the
        // exact integer-sector delta) — re-deriving it from the world-space
        // LightVP would re-inherit ULP(|origin|) storage rounding at planetary
        // magnitude (#660 shadow follow-up). Depth-family sampling contract
        // only; the tint twin fills just LightVP for the lockstep gate.
        Mathematics::Matrix4x4 LightVPRel;
        int32 Sector[3]{};
        float OrthoHalfExtent = 0.0f;
        float DepthSpan = 0.0f;
        // Frame the content was rendered — or last proven byte-identical to
        // the current fit (a Cached skip refreshes the stamp without touching
        // the fit fields; they are equal by the cache's byte-equality). The
        // scheduler's staleness input = declaring frame − this stamp: ground
        // truth for "how many frames of drift would receivers sample",
        // immune to view-hide / cascade-count-regrow tracking lapses.
        uint64_t FrameStamp = 0;
        bool Valid = false;
    };

    // Per-cascade sampling-fit selection of BuildShadowDataGPU, extracted pure
    // (static, no device) and exposed for unit tests — BuildShadowDataGPU
    // itself needs an initialized RenderServices for its bindless/quality
    // phases. A valid `fit` wins: its origin-relative LightVPRel uploads
    // bit-for-bit when its Sector matches the frame's (the retained layer's
    // rasterizing matrix — receivers key into its exact texels), re-anchored
    // across a sector step via the exact integer-sector delta
    // (RebaseTranslationColumnBySectorDelta); its PCSS metrics ride along
    // (outWorldPerTexel = 2*halfExtent/resolution, outDepthSpan). A null or
    // !Valid fit falls back to the current frame's LightVPRel + metrics
    // (first frames, post-invalidate, standalone calls — those slots render
    // this frame anyway). When the snapshot equals the current frame's fit
    // the selection reproduces frame.LightVPRel bit-for-bit — the cap-0
    // dark-ship guarantee.
    static void SelectCascadeSamplingFit(const CascadeContentFit* fit,
                                         const CascadeFrameData& frame, uint32_t cascadeIndex,
                                         float resolution, float outShadowVP[16],
                                         float& outWorldPerTexel, float& outDepthSpan);

    // The widest PCF kernel the PCSS filter can pick in `viewId`'s first
    // `cascadeCount` cascades, in shadow texels: the kernel cap (Max Penumbra,
    // or kPcssMaxPenumbraWorldAuto at 0) over the finest cascade's texel size,
    // never below one texel. Sized from the same sampling fit the shader's
    // per-cascade texel size comes from, so the min/max pyramid built for it
    // always covers the lit proof's query.
    float PcssWidestKernelTexels(Rendering::ViewId viewId, const CascadeFrameData& frame,
                                 uint32_t cascadeCount) const;

    // IRenderFeature declaration hook — declares this view's shadow producer
    // passes, driven by ShadowMapNode. When ctx.DirectionalLightDirWS is
    // present it emits the directional cascade depth passes followed by the
    // glass-tint cascade passes; then, regardless of the directional light, it
    // emits the punctual (area/spot/point) families for each valid ctx snapshot.
    // A null ctx.DirectionalLightDirWS declares the punctual families only,
    // matching the node's no-light branch. Reaches the shared depth executor +
    // shadow-array imports through `rs`; the pass exec lambdas capture a
    // RenderServices* by value, never the ctx. (A1.1 S1 + S2.)
    void Declare(Rendering::RenderGraph::RGFrame& frame, RenderServices& rs,
                 const FeatureDeclareContext& ctx) override;

    // Per-family declaration entry points (also driven directly by the granular
    // RenderServicesRGPassTests, which keep their per-family call shapes).
    // DeclareCascadePass writes one layer of the pooled shadow depth array;
    // DeclareTransmittancePass draws glass into the tint array, depth-testing
    // read-only against the cascade layer it follows. Both no-op internally
    // unless the view needs cascades (and, for the tint, has transmissive casters).
    Rendering::RenderGraph::RGPass DeclareCascadePass(Rendering::RenderGraph::RGFrame& frame,
                                                      RenderServices& rs,
                                                      const FeatureDeclareContext& ctx,
                                                      uint32_t cascadeIndex, const char* passName);
    Rendering::RenderGraph::RGPass DeclareTransmittancePass(Rendering::RenderGraph::RGFrame& frame,
                                                            RenderServices& rs,
                                                            const FeatureDeclareContext& ctx,
                                                            uint32_t cascadeIndex,
                                                            const float lightDirWS[3],
                                                            const char* passName);

    // Punctual shadow families (A1.1 S2): each no-ops unless the matching
    // ctx.{Area,Spot,Point}Shadow snapshot is present. They import their own
    // per-view depth map, upload the *ShadowDataGPU the world binding table reads,
    // and publish both back into ViewFrameRG through rs (passkey-gated). Pass
    // names are byte-identical to the former RenderServices arms
    // ("AreaShadow[View#N]" / "SpotShadow[View#N]" / "PointShadow[View#N.FaceF]").
    Rendering::RenderGraph::RGPass DeclareAreaPass(Rendering::RenderGraph::RGFrame& frame,
                                                   RenderServices& rs,
                                                   const FeatureDeclareContext& ctx);
    Rendering::RenderGraph::RGPass DeclareSpotPass(Rendering::RenderGraph::RGFrame& frame,
                                                   RenderServices& rs,
                                                   const FeatureDeclareContext& ctx);
    void DeclarePointPasses(Rendering::RenderGraph::RGFrame& frame, RenderServices& rs,
                            const FeatureDeclareContext& ctx);

    // Per-view shadow map textures — each view gets its own array so render
    // graph dependencies remain independent (no cross-view overwrites).
    //
    // Frame-scoped by contract: the pool owns the physical and destroys it once
    // idle, and the only frames that re-adopt are the ones whose cascade arm
    // actually imported (all three import sites are gated on ViewNeedsCascades).
    // An entry is therefore trustworthy for exactly the frame it was adopted
    // for; a request from any other frame reports ABSENCE so callers bind their
    // typed fallback instead of a handle whose image the pool may have freed.
    Rendering::TextureHandle GetShadowMapTexture(Rendering::ViewId viewId,
                                                 const RenderGraph::RGFrame& frame) const;

    // RenderGraph arm: the shadow array is POOL-owned (RGResourcePool, imported per
    // frame by RenderServices::ImportShadowMapArrayRG); the feature only
    // *adopts* the pooled physical so its consumers (PCSS bindless views,
    // MSM moments sampling, the world pass binding table) keep working against it.
    // A physical change (pool realloc on config change) invalidates the
    // PCSS bindless registrations + cached frame data — but never destroys the
    // texture; the pool defer-destroys the old physical itself. RenderServices
    // is required because the old physical's bindless slots (and the per-layer
    // views written into them) are released through TextureService.
    //
    // The adopting frame is re-recorded even when the physical is unchanged:
    // the stamp is what keeps a steady-state view's handle servable, so
    // refreshing it is a correctness step, not an optimisation to skip.
    void AdoptPooledShadowMap(Rendering::ViewId viewId, Rendering::TextureHandle physical,
                              RenderServices& rs, RenderGraph::RGFrame& frame);

    // Update MomentsResolution at runtime. Validates to {512, 1024, 2048}
    // (other values snap to nearest). Returns true if the value changed —
    // caller should expect EnsureMsmMomentsForView to recreate the moments
    // texture on the next pipeline build, mirroring the depth-array's
    // resolution-change invalidation contract.
    bool SetMomentsResolution(uint32_t res)
    {
        if (res <= 768)       res = 512;
        else if (res <= 1536) res = 1024;
        else                  res = 2048;
        if (m_Config.MomentsResolution == res)
            return false;
        m_Config.MomentsResolution = res;
        return true;
    }
    uint32_t GetMomentsResolution() const { return m_Config.MomentsResolution; }

    // MSM4 moments-array (RGBA16_UNORM, NumCascades layers @ MomentsResolution).
    // Allocated lazily per-view, invalidated on NumCascades / MomentsResolution
    // change. Thumbnail / preview views should NOT call EnsureMsm — they fall
    // back to grid PCF to keep memory bounded; gating happens at the pipeline
    // node, not here.
    Rendering::TextureHandle GetMsmMomentsTexture(Rendering::ViewId viewId) const;
    Rendering::TextureHandle EnsureMsmMomentsForView(Rendering::ViewId viewId,
                                                     const RenderServices& rs,
                                                     bool* outRecreated = nullptr);
    // 1x1 stub texture used as a fallback bind for set 0 binding 10 when
    // MSM4 isn't the active filter quality. Allocated lazily on first
    // access; ~128 bytes total, shared across views. The world-pass
    // descriptor write uses GetMsmMomentsTextureForBinding which prefers
    // the per-view live moments and falls back to this stub.
    Rendering::TextureHandle GetMsmMomentsStubTexture();
    Rendering::TextureHandle GetMsmMomentsTextureForBinding(Rendering::ViewId viewId);
    // Linear-clamp sampler used by the MSM read-side. Created lazily on first
    // EnsureMsmMomentsForView. Distinct from the depth-compare sampler used by
    // the PCF/PCSS path.
    Rendering::SamplerHandle GetMsmSampler() const { return m_MsmSampler; }
    Rendering::SamplerHandle GetShadowSampler() const { return m_ShadowSampler; }

    // MSM4 blur kernel quality (Linear5Tap default; see enum docs).
    MsmBlurMode GetMsmBlurMode() const { return m_MsmBlurMode; }
    void SetMsmBlurMode(MsmBlurMode mode) { m_MsmBlurMode = mode; }

    // The ShadowMap node's authored directional shadow settings, which a view
    // uses wherever no world shadow-settings volume overrides them.
    struct DirectionalShadowSettings
    {
        float MaxShadowDistance = 100.0f;
        float SplitLambda = 0.5f;
        float DepthBias = 0.0001f;
        float NormalBias = 0.02f;
        // The ShadowMap node's "fitShadowDistanceToScene": the range is fitted per
        // view (FitShadowDistanceToScene) to the bounds of every GPUScene mesh
        // instance, instead of taken from MaxShadowDistance. MaxShadowDistance
        // applies only until the view's first fit; after it, a frame with nothing
        // ahead of the camera keeps the previous fit. Wins over a world volume's
        // MaxShadowDistance; the volume's other settings still apply.
        bool FitDistanceToScene = false;
    };

    // The directional shadow range for a view whose scene ends `sceneReach`
    // world units ahead of the camera (view depth of the scene's far side), with
    // the world's distance-fade fraction `fadeFraction`. A fit puts the start of
    // the fade band kSceneFitHeadroom beyond the reach, so every surface in the
    // scene keeps the full shadow and the cascade splits partition only the
    // range the scene occupies. `previousDistance` (0 = none) is kept while the
    // reach stays inside its band: until the reach would enter the fade, or
    // until it shrinks so far that the band starts kSceneFitShrinkRatio times
    // beyond it, so a static scene and an orbit about it never refit. A reach at
    // or before `nearPlane` (nothing ahead of the camera) keeps
    // `previousDistance`. Returns 0 when there is neither a fit nor a previous one.
    static float FitShadowDistanceToScene(float previousDistance, float sceneReach,
                                          float nearPlane, float fadeFraction);
    static constexpr float kSceneFitHeadroom = 1.1f;
    static constexpr float kSceneFitShrinkRatio = 1.5f;

    // This frame's cascade fit for `viewId`, lit by `light` (its world's
    // shadow-casting primary directional) through `camera`. The fit is computed
    // where the casters are culled (OnScheduleCulling, before the pipeline
    // declares), so the cull and the cascades it feeds use one fit; the
    // ShadowMap node's call returns that fit unless an input changed between
    // the two (an authored setting, the camera), in which case it refits and
    // that frame's cull ran one fit behind. Resolves the newest SDSM readback,
    // applies the world's shadow settings over `authored`, caches the result
    // (GetCachedFrameData) and returns it.
    const CascadeFrameData& FitViewCascades(RenderServices& rs, Rendering::ViewId viewId,
                                            uint64_t worldId, const Rendering::CameraData& camera,
                                            const ExtractedLight& light,
                                            const DirectionalShadowSettings& authored);

    // True where SDSM runs: the device wants fits that follow the depth buffer
    // (Radeon and stable-shadow devices opt out: noisy readbacks can oscillate
    // the splits). Every backend can read the result back: where a storage
    // buffer cannot be mapped (WebGPU) the readback ring splits the slot and
    // copies it into a mappable one (RGReadbackRing::Init).
    static bool SupportsSdsm(const Rendering::IDevice* device);

    // Cache per-view frame data so cascade depth passes can read light VP matrices.
    void CacheFrameData(Rendering::ViewId viewId, const CascadeFrameData& data);
    const CascadeFrameData* GetCachedFrameData(Rendering::ViewId viewId) const;

    // Camera-motion round-robin cap (GE_SHADOW_MOTION_CAP, default 2; 0 = off):
    // at most this many motion-class cascade renders PER (VIEW, FRAME) — each
    // shadow-casting view plans independently, so a frame's total motion
    // renders is cap × active cascade views. The rest keep their retained
    // layer for up to MotionMaxAge consecutive frames
    // (GE_SHADOW_MOTION_MAX_AGE, default 2) and receivers sample it through
    // the content fit. Correctness-class causes always render immediately and
    // never count against the cap. Requires the static shadow cache lane
    // (GE_SHADOW_STATIC_CACHE != 0) — its records define the retained content.
    uint32_t GetMotionCap() const { return m_MotionCap; }
    void SetMotionCap(uint32_t cap) { m_MotionCap = cap; }
    uint32_t GetMotionMaxAge() const { return m_MotionMaxAge; }
    void SetMotionMaxAge(uint32_t age) { m_MotionMaxAge = std::max(1u, age); }

    // Cascade fit freeze (GE_SHADOW_FIT_FREEZE, default ON; =0 forces the
    // per-frame refit baseline — the A/B lane and kill switch). A cascade's
    // fit is computed with a guard band (the ortho box and depth range are
    // padded past the slice's needs) and then FROZEN: while the fresh slice
    // stays inside the frozen coverage, the P2 receiver footprint stays inside
    // its drift allowance, and the camera stays inside the LOD drift bound,
    // ComputeCascades re-emits the frozen fit byte-for-byte and the frozen
    // cull-content inputs ride in CascadeFrameData. Every cull/LOD/cache-key
    // consumer then sees inputs that only step on an actual refit — so the
    // static cascade cache turns camera-motion frames into Cached skips
    // instead of Motion-class re-renders (measured: the per-frame texel-snap
    // fit changes on 57-100% of motion frames across orbit/fly/look, so
    // without the freeze, motion frames can never hit the cache).
    bool IsFitFreezeEnabled() const { return m_FitFreezeEnabled; }
    void SetFitFreezeEnabled(bool enabled) { m_FitFreezeEnabled = enabled; }

    // Stability-vs-sharpness for the cascade projection; see ShadowProjection.
    // Takes effect on the next ComputeCascades — the fit freeze re-derives the
    // extent the same way, so a mode change simply refits.
    ShadowProjection GetShadowProjection() const { return m_Config.Projection; }
    void SetShadowProjection(ShadowProjection projection)
    {
        if (m_Config.Projection == projection)
            return;
        m_Config.Projection = projection;
        // The frozen records were fitted by the OUTGOING mode, and the reuse
        // test only asks whether the fresh slice still fits inside them — which
        // a too-LARGE stale box always satisfies. Without this the freeze
        // reuses the old mode's extents indefinitely and the switch appears to
        // half-apply: the cascades that happened to refit adopt the new mode,
        // the rest keep the old one.
        InvalidateCascadeFitState();
    }

    // Drop every cached fit decision so the next ComputeCascades re-derives from
    // scratch. Needed by any change that alters what a fit SHOULD be without
    // altering what the fresh slice requires — the freeze's reuse test only asks
    // whether the slice still fits inside the frozen box, which a stale
    // oversize box always passes.
    void InvalidateCascadeFitState()
    {
        m_FrozenFits.clear();
        m_PrevSnappedHalfExtent.clear();
    }

    // WorldTexel authoring surface. Takes effect on the next ComputeCascades:
    // both the derived splits and the fixed extents read straight from these, so
    // a change simply re-solves rather than needing a resize.
    float GetCascade0TexelSize() const { return m_Config.Cascade0TexelSize; }
    float GetCascadeTexelRatio() const { return m_Config.CascadeTexelRatio; }
    void SetWorldTexelLadder(float cascade0TexelSize, float ratio)
    {
        const float texel = std::max(cascade0TexelSize, 1e-4f);
        const float clampedRatio = std::max(ratio, 1.0f);
        if (m_Config.Cascade0TexelSize == texel && m_Config.CascadeTexelRatio == clampedRatio)
            return;
        m_Config.Cascade0TexelSize = texel;
        m_Config.CascadeTexelRatio = clampedRatio;
        // Same reason as SetShadowProjection: a frozen box fitted to the previous
        // ladder still contains the fresh slice, so nothing would force a refit.
        InvalidateCascadeFitState();
    }

    // IRenderFeature hook — fans each shadow-casting view's main culling
    // submission out into one ViewCullingInput per cascade. Reads the
    // PREVIOUS frame's cached LightVP per view (cascade compute happens
    // after culling in the same frame); the one-frame lag is acceptable
    // because cascade frustums change slowly. Skips a cascade entirely
    // when its world-space AABB doesn't intersect the GPUScene's live
    // instance AABB — saves a per-cascade culling dispatch on small
    // scenes (editor thumbnails / interior asset previews where cascade
    // 3 is mostly empty).
    void OnScheduleCulling(const FeatureCullingContext& ctx) override;

    // Helpers exposed for unit tests of the cascade-skip optimization.
    // Compute a cascade's world-space AABB from its light-space VP by
    // back-projecting the 8 NDC corners. Conservative (axis-aligned bound
    // of an arbitrarily-oriented box) but cheap and stable.
    static void ComputeCascadeWorldAABB(const Mathematics::Matrix4x4& lightVP,
                                        Mathematics::Vector3& outMin,
                                        Mathematics::Vector3& outMax);
    // Standard slab-test AABB intersection.
    static bool AABBsIntersect(const Mathematics::Vector3& aMin,
                                const Mathematics::Vector3& aMax,
                                const Mathematics::Vector3& bMin,
                                const Mathematics::Vector3& bMax);

    // Cascade slice and caster-set helpers, exposed for unit tests of the fit
    // and plane math. Pure functions — no device or per-view state.
    //
    // Cascade c's 8 corners of the 20%-overlap-extended camera frustum slice —
    // the same slice ComputeCascades fits, in whatever frame `cameraViewProj`
    // is expressed in. The fit works from the origin-relative camera VP,
    // because world-magnitude corners carry ~ULP(|eye|) — metres, i.e. tens of
    // shadow texels. `sliceNearDist` / `sliceFarDist` are the view-space
    // distances bounding the slice before its overlap.
    static void ComputeExtendedSliceCornersWS(const Mathematics::Matrix4x4& cameraViewProj,
                                              float nearPlane, float farPlane,
                                              float sliceNearDist, float sliceFarDist,
                                              Mathematics::Vector3 outCorners[8]);

    // The caster-cull plane set for one directional cascade: the cascade's six
    // frustum planes with the NEAR plane retired, so it can never reject.
    //
    // Under a directional light the caster of a receiver at P lies exactly on the
    // ray P - t*L (t >= 0), sharing P's light-space XY and differing only in
    // light-space Z. Every occluder in a receiver's column is therefore a
    // legitimate caster at any distance toward the light, and a near plane — a
    // bound on Z alone — can only ever be wrong about it. The fit's back-pull
    // (minZSnapped - halfExtent * kShadowFrustumExtension) is not a caster
    // guarantee: measured in light-space Z, it reaches backExtension*sin(elev) of
    // world height while a caster H above its receiver needs H/sin(elev), so the
    // reach collapses as the sun drops (2.6 m at a 5 deg sun — see
    // CascadeCasterInclusion). The near plane's real job is bounding the depth
    // encode, which it still does.
    //
    // Culling is only half of it: the cascade depth pass must also set
    // DepthClampEnable, or the admitted casters are clipped at raster instead.
    // Far (planes[5]) is kept — a caster behind the receiver cannot shadow it.
    // Side-plane tightening to the caster footprint is applied by the caller afterwards.
    static void BuildCascadeCasterCullPlanes(const Mathematics::Matrix4x4& lightVP,
                                             Mathematics::Vector4 outPlanes[6]);

    // Tighten a cascade's four side planes (L/R/B/T = indices 0..3) in place to
    // its caster footprint. Under an orthographic light a caster casts its
    // shadow onto receivers at the SAME light-space XY for all depths, so a
    // caster whose light-space XY lies outside the receivers' footprint casts
    // onto nothing the cascade serves and is safe to cull. `footprintNdc` is
    // the light-NDC rectangle {xMin, yMin, xMax, yMax}
    // (CascadeFrameData::CasterFootprint), inflated by `slackWorld` (converted
    // to NDC via `orthoHalfExtent`) and intersected with the existing ortho
    // extent so the result is never looser than the original planes. planes[4]
    // (near) and planes[5] (far) are left untouched. No-op (planes unchanged)
    // on a degenerate footprint — never over-culls.
    static void TightenCascadeSidePlanes(const Mathematics::Matrix4x4& lightVP,
                                         const float footprintNdc[4], float orthoHalfExtent,
                                         float slackWorld, Mathematics::Vector4 planes[6]);

    // ── WorldTexel authoring (pure, static, unit-tested) ──

    // Authored world size of one shadow texel for cascade `i`:
    // Cascade0TexelSize * CascadeTexelRatio^i. Depends on nothing but the
    // config — not on resolution, camera, or splits. That independence IS the
    // feature.
    static float CascadeTexelSize(const CascadedShadowConfig& config, uint32_t cascadeIndex);

    // Same ladder, but from an explicit base rather than the config's — used
    // once Cascade0TexelSize has been resolved out of AUTO for this frame.
    static float CascadeTexelSizeFrom(float cascade0Texel, float ratio, uint32_t cascadeIndex);

    // Far distance the whole ladder reaches when built from `cascade0Texel`.
    // Monotonically increasing in it, which is what lets the AUTO solve bisect.
    static float CascadeChainReach(const CascadedShadowConfig& config, float cascade0Texel,
                                   float nearPlane, float tanHalfY, float aspect, float maxFar);

    // AUTO: the cascade-0 texel size whose ladder reaches exactly
    // `targetDistance`. This is what makes shadow distance a single knob —
    // the texel budget is re-spread over the requested range instead of the
    // author re-balancing SplitLambda against it.
    static float SolveCascade0TexelForDistance(const CascadedShadowConfig& config, float nearPlane,
                                               float targetDistance, float tanHalfY, float aspect);

    // halfExtent = Resolution * texelSize / 2, so 2*halfExtent/Resolution returns
    // the authored texel size exactly. Halving Resolution halves the REACH and
    // leaves sharpness untouched.
    static float CascadeHalfExtentForTexel(const CascadedShadowConfig& config,
                                           uint32_t cascadeIndex);

    // Bounding-sphere radius of the perspective frustum slice [nearDist, farDist].
    // Monotonically increasing in farDist, which is what makes the solver below a
    // valid bisection. `tanHalfY` = tan(fovY/2); `aspect` = width/height.
    static float SliceBoundingSphereRadius(float nearDist, float farDist, float tanHalfY,
                                           float aspect);

    // Largest farDist whose slice still fits inside a sphere of `radius` — i.e.
    // how far a cascade of that half-extent can reach. Bisected rather than
    // solved in closed form: the analytic inverse branches on whether the sphere
    // centre falls beyond the far plane, and getting that branch subtly wrong
    // fails silently as a slightly-wrong split rather than as an error.
    // Never returns more than `maxFar`, so MaxShadowDistance can only SHORTEN.
    static float SolveSliceFarForRadius(float nearDist, float radius, float tanHalfY, float aspect,
                                        float maxFar);

    // The cascade's ROTATION-INVARIANT extent: the bounding-sphere radius of the
    // (clamped) slice corners, plus that sphere's centre in light space.
    //
    // worldPerTexel is 2*halfExtent/resolution, so anything that moves the extent
    // moves the world point every shadow texel samples — every receiver
    // re-quantizes at once and the shadow edges crawl. That is the classic
    // shadow-map shimmer, and it is a projection defect: it happens with one hard
    // PCF tap and no dithering.
    //
    // A light-space AABB of the slice cannot provide it. The corner set is rigid,
    // but its axis-aligned bounds breathe as the camera turns — measured
    // 14.00..18.00 m over one pan, a 28.6% swing, which the power-of-two band
    // snap converts into occasional 12.5-20% jumps rather than removing. The
    // bounding sphere of that same rigid set is invariant by construction
    // (measured spread 0.00%), which is why it is the classic stable-CSM fit.
    //
    // The radius is 3D on purpose: only the 3D radius survives rotation. The
    // slice's projection onto the light's XY plane changes shape as the camera
    // turns, exactly as the AABB does. Bounding a 2D extent with a 3D radius is
    // what invariance costs — ~19% coarser texels than the mean AABB, ~9% against
    // the AABB's own worst case.
    //
    // ComputeCascadeLightVP and CanReuseFrozenCascadeFit MUST both derive the
    // extent from here; a divergence makes the frozen box look permanently
    // oversize and refits every frame.
    static void ComputeClampedLightSpaceSphere(const Mathematics::Matrix4x4& lightRot,
                                               const Mathematics::Vector3 cornersRel[8],
                                               const SceneBoundsRel* sceneBoundsRel,
                                               float& outRadius, float& outCenterXLS,
                                               float& outCenterYLS);

    // Accumulate a cascade slice's light-space AABB from its 8 frustum corners,
    // optionally clamping each corner into the scene's bounds FIRST. Still the
    // coverage/depth-window source; the ORTHO EXTENT now comes from the sphere
    // above.
    //
    // The clamp must happen in world space, not on the resulting light-space
    // AABB. The defect is that a low camera pitched down puts most of cascade
    // 0's slice BELOW the ground plane, and "below the ground" is a world-Y
    // fact. Projected into light space it is swamped: a 140 m ground spans
    // ~134 m along the light direction while the slice spans ~20 m, so the
    // slice sits wholly inside and a light-space intersection is a no-op.
    //
    // `sceneBoundsRel` is render-origin-RELATIVE, matching the frame the fit
    // works in. Null (or a degenerate box) skips the clamp entirely, leaving
    // this byte-identical to the plain corner accumulation it replaces.
    //
    // Callers must agree: ComputeCascadeLightVP and CanReuseFrozenCascadeFit
    // both derive these bounds, and the frozen record is fitted from CLAMPED
    // ones — clamp in one and not the other and the reuse test fails every
    // frame, refitting continuously.
    static void ComputeClampedLightSpaceBounds(const Mathematics::Matrix4x4& lightRot,
                                               const Mathematics::Vector3 cornersRel[8],
                                               const SceneBoundsRel* sceneBoundsRel,
                                               Mathematics::Vector3& outMinLS,
                                               Mathematics::Vector3& outMaxLS);

    // The terrain renderer's clearance map for `viewId` in the frame `frameStamp` names
    // (RGFrame::FrameIndex). It reaches that frame's ShadowData whichever node declares first:
    // BuildShadowDataGPU writes a map published before it, and a map published after the view's
    // ShadowData upload was attached is written into that upload here. Only that frame takes it, so
    // a view whose terrain stopped publishing loses the term the next frame rather than keep a stale
    // bindless index.
    void PublishTerrainShadowMap(ViewId viewId, uint64_t frameStamp, const TerrainShadowMap& map,
                                 RenderServices& rs);
    // The view's ShadowData upload for `frameStamp`, CPU-visible until the frame executes, so a
    // terrain map published later in the frame's declaration still reaches it. ShadowMapNode
    // attaches it when it allocates the upload, before BuildShadowDataGPU fills it.
    void AttachShadowDataUpload(ViewId viewId, uint64_t frameStamp, ShadowDataGPU* upload);
    // The map published for `viewId` in `frameStamp`, or null when none was.
    const TerrainShadowMap* FindTerrainShadowMap(ViewId viewId, uint64_t frameStamp) const;

    // Tracks whether the "no shadow-casting light" warning has been emitted,
    // so it only fires once until a shadow light reappears.
    bool HasWarnedNoLight() const { return m_WarnedNoLight; }
    void SetWarnedNoLight(bool warned) { m_WarnedNoLight = warned; }

    // Debug visualization mode (persists across frames until toggled off).
    ShadowDebugMode GetDebugMode() const { return m_DebugMode; }
    void SetDebugMode(ShadowDebugMode mode) { m_DebugMode = mode; }
    // The cascade, factor and mask views write display-range diagnostic values
    // rather than scene radiance, so exposure and tonemapping would misreport
    // them. The lit source-isolation views keep the authored look.
    bool ShowsDiagnosticValues() const
    {
        return m_DebugMode >= ShadowDebugMode::CascadeColors &&
               m_DebugMode <= ShadowDebugMode::ContactShadowFactor;
    }

    // Whether shadow map thumbnails should be displayed.
    bool GetShowThumbnails() const { return m_ShowThumbnails; }
    void SetShowThumbnails(bool show) { m_ShowThumbnails = show; }

    // Explicit base filter selected by debug/UI controls. Volume overrides
    // are resolved per view without changing this setting, so removing a
    // volume restores the latest base choice and cannot affect other worlds.
    ShadowFilterQuality GetFilterQuality() const { return m_FilterQuality; }
    void SetFilterQuality(ShadowFilterQuality q) { m_FilterQuality = q; }

    // The view's world-volume override, or the explicit base filter when
    // no override/view exists. Resource allocation uses this request before
    // applying device/readiness fallbacks to the shader's effective quality.
    ShadowFilterQuality ResolveRequestedFilterQuality(const RenderServices& rs,
                                                       Rendering::ViewId viewId) const;

    // The quality the SHADER will branch on, as opposed to the one the user
    // asked for: MSM4 without its moments texture demotes toward PCSS, and PCSS
    // without bindless — or on a device that prefers stable filtering — demotes
    // to PoissonPCF. Anything deciding whether to build PCSS-only resources must
    // ask this, not GetFilterQuality, or it pays for work the shader never uses.
    //
    // One demotion it cannot predict: BuildShadowDataGPU's per-view bindless
    // raw-depth registration can still fail and turn a PCSS verdict into Poisson
    // at that later point. Callers running before it get PCSS's PRECONDITIONS,
    // not its guaranteed outcome.
    ShadowFilterQuality ResolveEffectiveFilterQuality(RenderServices& rs,
                                                      Rendering::ViewId viewId) const;

    // Legacy boolean accessor kept as a compat shim for existing callers
    // (LightInspector pre-migration, DebugServer's pcss_set/get IPC). Maps
    // to/from FilterQuality::PCSS. New code should use GetFilterQuality.
    bool IsPcssEnabled() const { return m_FilterQuality == ShadowFilterQuality::PCSS; }
    void SetPcssEnabled(bool enabled)
    {
        if (enabled)
            m_FilterQuality = ShadowFilterQuality::PCSS;
        else if (m_FilterQuality == ShadowFilterQuality::PCSS)
            m_FilterQuality = ShadowFilterQuality::Grid5x5;
    }

    // Legacy int accessor; New code should use GetFilterQuality().
    int GetPcfQuality() const { return static_cast<int>(m_FilterQuality); }
    void SetPcfQuality(int q) { m_FilterQuality = static_cast<ShadowFilterQuality>(q); }

    // Poisson PCF softness: scales the filter radius (1.0 = default, higher = softer).
    float GetPoissonSoftness() const { return m_PoissonSoftness; }
    void SetPoissonSoftness(float s) { m_PoissonSoftness = std::max(0.0f, s); }

    // PCSS max penumbra radius in WORLD units. Upper clamp on the physical
    // penumbra width — keeps the same world-space penumbra size across
    // cascades, fixing the visible size jump at cascade splits. The shader
    // also enforces a hard kernel-quality ceiling so close cascades don't
    // under-sample the 16-tap Poisson disk at extreme radii.
    float GetPcssMaxPenumbra() const { return m_PcssMaxPenumbra; }
    void SetPcssMaxPenumbra(float w)
    {
        const float clamped = std::clamp(w, 0.0f, kPcssMaxPenumbraWorld);
        if (m_PcssMaxPenumbra == clamped)
            return;
        m_PcssMaxPenumbra = clamped;
    }

    // PCSS receiver-plane depth correction (default on). The shader captures
    // position derivatives before divergent shadow branches and projects them
    // into each cascade, keeping wide filters from self-shadowing on slopes.
    bool IsPcssReceiverPlaneBias() const { return m_PcssReceiverPlaneBias; }
    void SetPcssReceiverPlaneBias(bool enabled) { m_PcssReceiverPlaneBias = enabled; }

    // PCSS / Poisson PCF disk tap count. Higher = cleaner soft shadows at
    // the cost of more shadow-map fetches per fragment. Allowed values are
    // {8, 16, 32, 64}; the shader clamps to [8, 64] and the inspector UI
    // exposes a dropdown of these four. Default 16 matches the previous
    // hard-coded Poisson disk.
    // Dither basis; see ShadowDitherBasis. A look A/B rather than a quality
    // tier, so it is live-settable and has no "better" value.
    ShadowDitherBasis GetDitherBasis() const { return m_DitherBasis; }
    void SetDitherBasis(ShadowDitherBasis basis) { m_DitherBasis = basis; }

    uint32_t GetPcssTapCount() const { return m_PcssTapCount; }
    void SetPcssTapCount(uint32_t n)
    {
        // Snap to nearest allowed value so the shader's internal clamp and
        // the inspector dropdown agree on the legal set.
        if (n <= 12) n = 8;
        else if (n <= 24) n = 16;
        else if (n <= 48) n = 32;
        else              n = 64;
        m_PcssTapCount = n;
    }

    const SDSMBounds& GetSDSMBounds(Rendering::ViewId viewId) const;
    void UpdateSDSMBounds(Rendering::ViewId viewId, float newNear, float newFar);
    // Convert reverse-Z readback bounds, optionally suppressing raster jitter
    // with conservative, distance-relative rounding after linearization.
    static SDSMBounds ResolveSDSMBounds(float minNdc, float maxNdc, float nearPlane,
                                      float farPlane, bool orthographic, bool stabilize);

    // Latest decoded receiver measurement of a view (invalid until the first
    // readback lands), kept across frames that read back nothing new.
    const ShadowReceiverMeasurement& GetShadowReceivers(Rendering::ViewId viewId) const;
    void UpdateShadowReceivers(Rendering::ViewId viewId, const ShadowReceiverMeasurement& measured);

    // ── RenderGraph arm: token-gated SDSM readback on RGReadbackRing (the
    // #175 completion contract). AcquireSdsmSlotRG begins a per-view ring
    // write keyed by the declaring frame's identity, carrying the context the
    // reduce measures under; the generic IRenderFeature::OnFrameSubmittedRG
    // fan-out stamps this frame's pendings with the submission's GRAPHICS token
    // (another stream's pendings are left for their own submit; a re-begun
    // frame's dead declares are dropped; OnFrameStreamRetiredRG purges a dying
    // stream's unstamped pendings); TryResolveSdsmRG maps the NEWEST signaled
    // slot, with its context, and drops it plus everything older. The caller
    // decodes it (DecodeShadowReceiverResult). ──
    Rendering::BufferHandle AcquireSdsmSlotRG(const Rendering::RenderGraph::RGFrame& frame,
                                              Rendering::ViewId viewId,
                                              const ShadowReceiverMeasurement::Context& context);
    // Records the slot's resolve into the mappable ring buffer where the two
    // are separate allocations (WebGPU cannot map a storage buffer). A no-op
    // elsewhere, so the reduce pass calls it unconditionally after dispatch.
    void ResolveSdsmSlotRG(Rendering::ViewId viewId, Rendering::CommandList* cl,
                           Rendering::BufferHandle slot);
    void OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                            const Rendering::IDevice::GpuSyncToken& token) override;
    void OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame) override;
    void OnDeviceRebuilt(Rendering::IDevice* device) override;
    bool TryResolveSdsmRG(Rendering::ViewId viewId, ShadowReceiverReadback& out);

    // PCSS: register shadow map cascade layers as bindless textures for raw depth reads.
    // Routes through RenderServices for consistent caching and sampler pairing.
    // Returns an array of per-cascade bindless indices (zero entries = not registered).
    // Indices are NOT assumed to be contiguous — the bindless allocator may recycle
    // parked-free slots, so each cascade's index must be read independently.
    //
    // The per-layer VkImageViews these indices point at are owned by TextureService's
    // bindless cache, not by this feature: a slot and the view written into it are
    // created and released as one unit (InvalidateBindless), so the feature holds no
    // device objects and cannot strand a view on a texture it no longer references.
    struct PcssBindlessIndices
    {
        uint32_t Indices[kMaxShadowCascades]{};
    };
    const PcssBindlessIndices* EnsurePcssBindlessTextures(Rendering::ViewId viewId,
                                                          RenderServices& rs,
                                                          const RenderGraph::RGFrame& frame);
    const PcssBindlessIndices* GetPcssBindlessIndices(Rendering::ViewId viewId) const;

  private:
    Rendering::IDevice* m_Device = nullptr;
    CascadedShadowConfig m_Config;
    std::optional<uint32_t> m_ProjectResolutionOverride;
    // The adopted pooled physical plus the frame identity it was adopted FOR.
    // Validity is the (RGFrame*, FrameIndex) pair, never the pointer alone —
    // a stable per-window RGFrame is re-begun every app frame at the same
    // address, so an address match on its own would validate a dead handle.
    struct AdoptedShadowMap
    {
        Rendering::TextureHandle Physical;
        RenderGraph::RGFrameStamp AdoptedFor;
    };
    std::unordered_map<Rendering::ViewId, AdoptedShadowMap> m_ShadowMapByView;
    Rendering::SamplerHandle m_ShadowSampler;
    bool m_Initialized = false;
    bool m_WarnedNoLight = false;
    struct PublishedTerrainShadow
    {
        ViewId View{};
        // The map and the frame it was published for.
        bool HasMap = false;
        uint64_t MapStamp = 0;
        TerrainShadowMap Map{};
        // The view's ShadowData upload and the frame it belongs to.
        ShadowDataGPU* Upload = nullptr;
        uint64_t UploadStamp = 0;
    };
    // One entry per view; a handful at most, scanned linearly.
    std::vector<PublishedTerrainShadow> m_TerrainShadows;
    PublishedTerrainShadow& TerrainShadowEntry(ViewId viewId);
    // ShadowData's terrainShadow* words for `viewId`: the map published in `frameStamp`, or left
    // zero (no term) when none was.
    void WriteTerrainShadowMap(ViewId viewId, RenderServices& rs, uint64_t frameStamp,
                               ShadowDataGPU& out) const;
    // Point-shadow instrumentation (arc M1): per-view log-on-change of the atlas
    // assignment (admitted slot count + each slot's coverage tier / tile
    // resolution / surviving face count). Keyed by a hash of the slot set so a
    // stable assignment logs once and a tier flip under a dolly shows as a log
    // line — the hysteresis (§4.4) is verified by the ABSENCE of per-frame spam.
    std::unordered_map<Rendering::ViewId, uint64_t> m_PointShadowDeclareState;
    ShadowDebugMode m_DebugMode = ShadowDebugMode::Off;
    bool m_ShowThumbnails = false;
    // Default PCSS to match the engine's effective default before the
    // PCSS-checkbox-to-enum migration. (The old code's m_PcssEnabled=true
    // override silently promoted m_PcfQuality=0 to PCSS at runtime when
    // bindless was on; the dropdown just makes that explicit.) MSM4 stays
    // opt-in until projects validate it.
    ShadowFilterQuality m_FilterQuality = ShadowFilterQuality::PCSS;
    float m_PoissonSoftness = 1.5f; // Softness multiplier — scales Poisson PCF radius AND PCSS penumbra.
                                    // For a directional light this is tan(angular-extent/2):
                                    // sun ≈ 0.0044 (0.5° arc); 0.005 is the editor default.
                                    // Raise to ~0.05 for soft overcast / fluorescent looks.
    // 0 = auto: bound the kernel by what the tap budget can sample (the shader
    // derives it from tapCount and worldPerTexel). A non-zero value is an
    // artistic override in world units and takes the tighter of the two.
    // It defaulted to 0.04 m, which saturated for any light past ~2 degrees and
    // silently overrode the per-light angular diameter -- turning the physical
    // control into a no-op exactly where it starts to matter.
    // 0.2 m authored, not 0 (auto). Auto resolves to kPcssMaxPenumbraWorldAuto
    // (0.5 m), which is wider than most content wants — at 0.2 m a 2 degree sun
    // reaches the ceiling at a ~5.7 m caster-receiver gap, which is about where
    // a penumbra stops reading as contact and starts reading as ambient.
    float m_PcssMaxPenumbra = 0.2f;
    bool m_PcssReceiverPlaneBias = true;
    // 16 is the authored default: with the penumbra ceiling now a world constant
    // rather than a texel multiple, the kernel no longer widens with resolution,
    // so the extra taps 32 bought are spent on a disk that is not growing.
    // GE_PcssTapsForRadius still scales up from here when a cascade's kernel is
    // genuinely wide in texels. {8,16,32,64}; sent via ge_shadowPcss.y.
    uint32_t m_PcssTapCount = 16;
    // Screen is the shipped behaviour: an untouched project must render
    // byte-identically after this slice.
    ShadowDitherBasis m_DitherBasis = ShadowDitherBasis::Screen;

    // NOTE: the resolved cascade-0 texel size is deliberately NOT stored here.
    // Several views run the cascade fit per frame (scene view, thumbnails,
    // previews) with very different far planes, so a feature-level value is
    // simply whichever view ran last — which made AUTO resolve a preview's
    // 2 m range and hand it to the scene view. It is a local threaded through
    // the fit instead.
    MsmBlurMode m_MsmBlurMode = MsmBlurMode::Linear5Tap;

    // Min/max depth pyramid over the cascade depth, declared after the cascade
    // passes when PCSS is the active filter. One instance serves every view:
    // the device-level state it holds (pipeline, reflected layout, sampler) is
    // shared, and everything per-view — the pooled texture, the level count —
    // is keyed by ViewId inside it.
    ShadowMinMaxPyramid m_MinMaxPyramid;

    // Per-view cached frame data (written by upload pass, read by cascade depth passes).
    std::unordered_map<Rendering::ViewId, CascadeFrameData> m_CachedFrameData;

    // Static-scene cascade shadow caching (GE_SHADOW_STATIC_CACHE, default
    // ON; =0 forces the always-render baseline): per-(view, cascade/tint)
    // skip decisions over the last-rendered input record. Always evaluated
    // (the stats measure achievable hit rate
    // even when disabled); skips only when the flag is on. Invalidated on
    // pooled-physical adopt-change and device rebuild — content identity does
    // not track GPU lifetime.
    CascadeShadowCache m_CascadeShadowCache;

    // Count of cascade depth passes actually declared this Declare() call.
    // BuildShadowDataGPU uses it with content-fit validity: if this frame
    // declared none and no layer has retained content, publish numCascades=0
    // so sampling returns fully lit instead of sampling an unwritten reverse-Z
    // image (1.0 = fully shadowed).
    uint64_t m_CascadeDeclareFrameIndex = ~0ull;
    uint32_t m_DeclaredCascadePassesThisFrame = 0;

    // ── Camera-motion round-robin (GE_SHADOW_MOTION_CAP) ──
    // Content-fit invariant: receivers must transform by the fit of the depth
    // content ACTUALLY retained in each cascade layer, never by the current
    // frame's fit alone. On any frame where a cascade renders (or the static
    // cache skips it byte-identically) the two are equal; only a motion-
    // deferred cascade diverges, and then BuildShadowDataGPU uploads this
    // snapshot (world fit rebased against the CURRENT render origin) plus the
    // matching PCSS metrics. Updated on every declared depth cascade render;
    // invalidated with the cache (physical adopt-change / device rebuild).
    std::unordered_map<Rendering::ViewId, std::array<CascadeContentFit, kMaxShadowCascades>>
        m_CascadeContentFit;
    // Tint-family twin of the snapshot above (LightVP + FrameStamp + Valid
    // only; the PCSS metric fields stay zero — the tint is sampled through
    // the DEPTH content fit). Committed/refreshed exactly like the depth
    // family's: on every declared tint render and on tint Cached skips. The
    // plan phase requires byte-equal content-fit LightVPs across the pair
    // before a Motion pair may defer (ClassifyCascadePair) — a
    // transmission-visibility gap otherwise leaves an arbitrarily old tint
    // layer deferral-eligible on the depth family's fresh stamp.
    std::unordered_map<Rendering::ViewId, std::array<CascadeContentFit, kMaxShadowCascades>>
        m_TintContentFit;

    // Frame plan: built once per (view, frame) at the top of Declare's
    // directional branch, consumed by both cascade families so a deferred
    // depth layer and its glass-tint twin stay coherent under ONE sampling
    // fit. Stamped with the declaring frame index — a plan from another frame
    // (or a direct per-pass test call with no plan) never defers.
    struct MotionFramePlan
    {
        uint64_t FrameStamp = ~0ull;
        uint32_t DeferMask = 0;
    };
    std::unordered_map<Rendering::ViewId, MotionFramePlan> m_MotionPlanByView;
    CascadeMotionScheduler m_MotionScheduler;
    uint32_t m_MotionCap = 2;    // env-overridable in the constructor; 0 = off
    uint32_t m_MotionMaxAge = 2; // max consecutive deferrals per cascade

    // Classify every cascade pair's dirty cause (read-only peeks) and run the
    // scheduler; publishes this frame's MotionFramePlan for the view. Called
    // at the top of Declare's directional branch, BEFORE any cascade declares.
    void PlanCascadeMotionFrame(Rendering::RenderGraph::RGFrame& frame, RenderServices& rs,
                                const FeatureDeclareContext& ctx);
    bool IsCascadeDeferredThisFrame(Rendering::ViewId viewId, uint64_t frameStamp,
                                    uint32_t cascadeIndex) const;

    // Per-view declare counter setting the window cadence. Cadence only: the
    // window's snapshot and storm edge state live in the cache, with the
    // totals they derive from, so no clear here can desync them. A stale
    // counter across a device rebuild shifts the window phase and nothing
    // else.
    std::unordered_map<Rendering::ViewId, uint32_t> m_CascadeCacheLogCounter;

    // Periodic (~every 300 declares per view) static-cache window log; warns
    // (edge-triggered) when the cache is enabled but produced zero skips over
    // a full window whose misses are NOT plain camera motion — the cause
    // histogram names the churn source. Expected-motion windows (a sustained
    // orbit: ≥95% camera/fit/settle misses) stay Info. `casterContentVersion`
    // is the view's world shadow-caster content version as the cache keyed on
    // it this frame; both lines print it beside the caster miss count.
    void LogCascadeCacheStatsPeriodic(Rendering::ViewId viewId, bool enabled,
                                      uint64_t casterContentVersion);

    // Views whose m_ShadowMapByView entry is a POOL-owned physical adopted
    // from the RenderGraph arm (AdoptPooledShadowMap). The feature must never
    // DestroyTexture these — the RGResourcePool defer-destroys them.
    std::unordered_set<Rendering::ViewId> m_PoolOwnedShadowMapViews;

    // MSM4 moments per-view + parallel dims tracker for invalidation.
    // Resolution dim source is m_Config.MomentsResolution (NOT .Resolution).
    std::unordered_map<Rendering::ViewId, Rendering::TextureHandle> m_MsmMomentsByView;
    struct MomentsDims
    {
        uint32_t Resolution = 0;
        uint32_t NumCascades = 0;
    };
    std::unordered_map<Rendering::ViewId, MomentsDims> m_MsmMomentsDimsByView;
    Rendering::SamplerHandle m_MsmSampler;
    // Tiny 1x1 array used to keep set 0 binding 10 valid when MSM4 isn't
    // the active filter quality. See GetMsmMomentsStubTexture().
    Rendering::TextureHandle m_MsmMomentsStub;

    // Per-(view, cascade) snapped half-extent from the previous frame, used
    // to apply rotation-stability hysteresis: if the new raw half-extent is
    // close enough to the previous snapped value (within the deadband), reuse
    // the previous bucket. Prevents shadow shimmer when SDSM bounds or the
    // camera rotation cause the AABB extent to oscillate near a snap edge.
    std::unordered_map<Rendering::ViewId,
        std::array<float, kMaxShadowCascades>> m_PrevSnappedHalfExtent;

    std::unordered_map<Rendering::ViewId, SDSMBounds> m_SDSMBoundsByView;
    std::unordered_map<Rendering::ViewId, ShadowReceiverMeasurement> m_ShadowReceiversByView;

    // RenderGraph SDSM readback state (see the public RenderGraph-arm block).
    // Each slot's payload is the context its reduce measured under, so a result
    // that lands frames later still says which camera and light it belongs to.
    // The shared helper owns the per-view ring lifecycle (lazy init, fan-out,
    // teardown).
    Rendering::RenderGraph::PerViewReadbackRings<ShadowReceiverMeasurement::Context> m_SdsmRingRG;

    // PCSS: per-cascade bindless texture indices for raw depth reads.
    // Registered lazily on first use per view when the shadow map is created.
    // One-off mutex cost on first hit; cache hits thereafter.
    std::unordered_map<Rendering::ViewId, PcssBindlessIndices> m_PcssBindlessByView;

    // PCSS min/max pyramid: per-cascade bindless views of the pyramid the
    // early-out samples.
    //
    // Their lifetime tracks the pyramid's POOL ENTRY, not the publish decision.
    // The pool destroys a persistent texture that has gone
    // kPersistentMaxIdleFrames without an import and notifies nobody, so a slot
    // that outlives the frames the pyramid is declared for ends up naming a view
    // of a freed image. That is why BuildShadowDataGPU calls the ensure/release
    // below on every frame rather than inside its PCSS arm — the raw-depth
    // sibling above cannot key on a physical CHANGE for this, because the handle
    // does not change, it dies.
    struct PyramidBindlessIndices
    {
        // The physical the slots view. Both events that end a registration are
        // reads of this handle — the pool mints a new one on a resolution or
        // cascade-count change, and reports none at all once the pyramid stops
        // being declared — so it is stored rather than re-derived.
        Rendering::TextureHandle Physical;
        uint32_t Indices[kMaxShadowCascades]{};
    };
    // Registers one FULL-MIP-CHAIN view per layer of `pyramid`, or returns
    // nullptr when there is nothing to register. The mip chain is the whole
    // point: the early-out reads a coarse level via texelFetch(.., lod), and a
    // single-level view (what the raw-depth registration above creates) has no
    // such level.
    //
    // Takes the resolved frame state rather than the frame, so the caller's
    // published layer bound and the layers registered here are the same value,
    // not two lookups that have to agree.
    //
    // Also owns the release side, for both events that end a registration: the
    // pyramid moving (pool realloc on a resolution or cascade-count change, a
    // new handle) and the pyramid ceasing to exist (`pyramid` reports no
    // physical, because it stopped being declared). A device rebuild is handled
    // by OnDeviceRebuilt. Keying on the pyramid's OWN state rather than the
    // cascade array's is what makes the release exact — the two textures
    // reallocate on the same frames today, but nothing enforces that, and only
    // the pyramid's state reports the second event at all.
    const PyramidBindlessIndices* EnsurePyramidBindlessTextures(
        Rendering::ViewId viewId, RenderServices& rs,
        const ShadowMinMaxPyramid::FrameState& pyramid);
    std::unordered_map<Rendering::ViewId, PyramidBindlessIndices> m_PyramidBindlessByView;

  public:
    // Whether `viewId` currently holds registered pyramid slots. Test-only: the
    // release policy has no other externally visible effect, and asserting it
    // through re-registration would be asserting the descriptor allocator's
    // parking window instead of the policy.
    bool HasPyramidBindlessForTesting(Rendering::ViewId viewId) const
    {
        return m_PyramidBindlessByView.find(viewId) != m_PyramidBindlessByView.end();
    }

  private:

    // `tanHalfY` / `aspect` describe the camera and are only consulted under
    // ShadowProjection::WorldTexel, where the splits are DERIVED from each
    // cascade's authored coverage rather than authored via SplitLambda. Stable
    // and Close ignore them and stay byte-identical.
    // `worldTexelBase` is the cascade-0 texel size already resolved out of AUTO
    // by the caller, so the splits and the extents provably derive from ONE
    // value. Ignored unless the projection is WorldTexel.
    void ComputeSplits(float nearPlane, float farPlane, float tanHalfY, float aspect,
                       float worldTexelBase, float outSplits[kMaxShadowCascades],
                       const SDSMBounds* sdsmBounds = nullptr) const;

    // Result of cascade orthographic fitting. VP is the light view-projection,
    // OrthoHalfExtent is the world-space half-width of the cascade in X/Y
    // (the ortho is [-r, r] x [-r, r]), and DepthSpan is the world-space
    // depth range used to build the ortho projection. These metrics are
    // used by PCSS to convert a blocker depth delta into physical penumbra texels.
    struct CascadeFit
    {
        // The fit with the render origin added back to its eye — world space,
        // for world-space caster culling. Equals VPRel byte-for-byte when the
        // origin is inactive.
        Mathematics::Matrix4x4 VP;
        // The fit as built: render-origin-relative eye, so its clip translation
        // resolves the shadow texel grid at any camera magnitude. The depth pass
        // and receivers consume THIS one. See CascadeFrameData::LightVPRel.
        Mathematics::Matrix4x4 VPRel;
        float OrthoHalfExtent = 0.0f;
        float DepthSpan = 0.0f;
        // Light-space internals of the fit (light-rotation basis built from the
        // light dir, applied to RENDER-ORIGIN-RELATIVE positions), captured for
        // the fit-freeze coverage test: the snapped ortho center, and the
        // projected depth window [NearZLS, FarZLS] (NearZLS includes the caster
        // back-extension). Only comparable against fresh corners rebased against
        // the same origin — which CanReuseFrozenCascadeFit's sector compare
        // guarantees before it reads them.
        float CenterXLS = 0.0f;
        float CenterYLS = 0.0f;
        float NearZLS = 0.0f;
        float FarZLS = 0.0f;
    };

    // Everything a view's cascade fit is computed from, beyond the feature's
    // per-view state (SDSM results, hysteresis, frozen fits).
    struct ViewCascadeFitInputs
    {
        Rendering::CameraData Camera{};
        Mathematics::Vector3 LightDirection{};
        uint32_t LightCascadeCount = 0;
        float LightAngularDiameter = 0.0f;
        // The authored settings with the world's override applied.
        DirectionalShadowSettings Settings{};
        // The config fields the fit reads besides those settings.
        uint32_t NumCascades = 0;
        ShadowProjection Projection = ShadowProjection::Close;
        uint32_t Resolution = 0;
        float Cascade0TexelSize = 0.0f;
        float CascadeTexelRatio = 0.0f;
        float PcssMaxPenumbra = 0.0f;
        SceneBoundsRel SceneBounds{};
        bool HasSceneBounds = false;
        // View depth of the far side of the sphere around SceneBounds (world
        // units), read by the scene fit only; 0 without scene bounds.
        float SceneReach = 0.0f;
        float DistanceFadeFraction = 0.0f;
    };
    static bool SameFitInputs(const ViewCascadeFitInputs& a, const ViewCascadeFitInputs& b);

    // A view's latest fit and what it was computed from. `Authored` is the
    // ShadowMap node's last settings for the view, which the culling-time fit
    // reuses; `FittedForCull` marks a fit OnScheduleCulling computed this frame
    // that the node has not consumed yet.
    struct ViewCascadeFit
    {
        ViewCascadeFitInputs Inputs{};
        CascadeFrameData Frame{};
        DirectionalShadowSettings Authored{};
        bool HasAuthored = false;
        bool FittedForCull = false;
        // The scene fit's range (FitShadowDistanceToScene); 0 = none yet.
        float FittedDistance = 0.0f;
    };
    std::unordered_map<Rendering::ViewId, ViewCascadeFit> m_ViewFits;

    // The culling-time half of FitViewCascades: refits a view the ShadowMap node
    // has declared before. Null when its world has no shadow-casting primary
    // directional or its camera is not ready.
    const CascadeFrameData* FitViewCascadesForCulling(RenderServices& rs,
                                                      const Rendering::ViewDesc& view,
                                                      const Rendering::CameraData& camera);
    ViewCascadeFitInputs GatherFitInputs(RenderServices& rs, uint64_t worldId,
                                         const Rendering::CameraData& camera,
                                         const ExtractedLight& light,
                                         const DirectionalShadowSettings& authored) const;
    // Replaces `inputs`' MaxShadowDistance with the view's scene fit when the
    // node asks for one, carrying the fit's hysteresis in `record`.
    static void ApplySceneDistanceFit(ViewCascadeFit& record, ViewCascadeFitInputs& inputs);
    // Applies `inputs`' settings, resolves the newest SDSM readback, computes
    // and caches the fit into `record`.
    const CascadeFrameData& RefitView(RenderServices& rs, Rendering::ViewId viewId,
                                      const ViewCascadeFitInputs& inputs, ViewCascadeFit& record);
    // Adopts the newest signaled SDSM readback of a view, if any.
    void ResolveSdsmReadback(RenderServices& rs, Rendering::ViewId viewId);

    // ── Cascade fit freeze (GE_SHADOW_FIT_FREEZE) ──
    // One record per (view, cascade): the guard-banded fit plus the cull-
    // content inputs captured when it was built. Reuse requires, each frame:
    //   * identical normalized light dir / resolution / origin sector,
    //   * the fresh fit bounds (CascadeFitBounds: the slice's light-space AABB,
    //     narrowed to the measured receivers) inside the frozen ortho box and
    //     depth window (receiver coverage — exact, no drift allowed),
    //   * the fresh bounds' caster footprint (CascadeFitBounds::CasterBox)
    //     inside the frozen footprint
    //     plus kFreezeFootprintDriftFraction (the cull adds the same fraction
    //     to its slack, so drift eats budget that was genuinely reserved —
    //     casters for the drifted footprint are present in the retained layer),
    //   * camera travel under the LOD drift bound (GPU LOD selection reads the
    //     frozen camera; the bound caps silhouette-LOD staleness),
    //   * the frozen half-extent no more than one snap band above what a
    //     fresh refit would snap to (shrink refit — SDSM-contracted slices
    //     pass containment forever, so without this worldPerTexel stays
    //     pinned at capture size; the band + the refit hysteresis deadband
    //     keep slow contraction from oscillating).
    // Any failure refits with a fresh guard band and recaptures.
    struct FrozenCascadeFit
    {
        CascadeFit Fit;
        Mathematics::Vector3 LightDir;    // normalized, exact-compare
        Mathematics::Vector3 CameraPos;   // LOD drift reference
        Mathematics::Matrix4x4 CullCameraViewProj;
        float LodCameraPos[4]{};
        float LodProjScaleY = 0.0f;
        // View depth of the capture-time slice's far end (the LOD drift bound
        // scales with it).
        float SliceFarDist = 0.0f;
        // Light-NDC caster footprint {xMin, yMin, xMax, yMax} of the
        // capture-time bounds' CasterBox through Fit: the rectangle the cull
        // tightens the side planes to while this record is reused.
        float Footprint[4]{};
        int32 Sector[3]{};
        uint32_t Resolution = 0;
        bool Valid = false;
    };

    // Fit-freeze records (see FrozenCascadeFit). Cleared on device rebuild;
    // a pooled-physical adopt-change does NOT clear them — the frozen fit is
    // an input choice, not retained content, and staying frozen across the
    // re-render keeps the cache key stable.
    std::unordered_map<Rendering::ViewId,
        std::array<FrozenCascadeFit, kMaxShadowCascades>> m_FrozenFits;
    bool m_FitFreezeEnabled = true; // env-overridable in the constructor

    // What a cascade's box must hold, in the cascade light basis applied to
    // render-origin-relative positions: the light-space AABB of its frustum
    // slice clamped to the scene (narrowed to the measured receivers, the
    // visible surfaces and the air in front of them, when SDSM measured them),
    // and the slice's bounding sphere the Stable and WorldTexel
    // projections centre on. CasterBox is where the casters those receivers
    // can see lie laterally: the receivers, or the slice box when the cascade
    // keeps it, widened by a lookup's reach (ReceiverReachWorld) and not
    // clipped to the slice; its light-NDC rectangle is the caster footprint.
    struct CascadeFitBounds
    {
        Mathematics::AABB Box;
        Mathematics::AABB CasterBox;
        float SphereRadius = 0.0f;
        float SphereCenterXLS = 0.0f;
        float SphereCenterYLS = 0.0f;
    };
    static CascadeFitBounds SliceFitBounds(const Mathematics::Matrix4x4& lightRot,
                                           const Mathematics::Vector3 sliceCornersRel[8],
                                           const SceneBoundsRel* sceneBounds);

    // `receivers` when the fit can follow them: valid, under the Close
    // projection, measured through the `current` camera's window
    // (ShadowReceiverSameWindow). Null otherwise.
    const ShadowReceiverMeasurement* UsableReceivers(
        const ShadowReceiverMeasurement* receivers,
        const ShadowReceiverMeasurement::Context& current) const;

    // The transform from `measured`'s light space (its light direction and
    // render origin) into the current one (`lightRot`, `originSector`). False,
    // leaving `out` untouched, when the two are the same space.
    static bool MeasuredToCurrentLightSpace(const ShadowReceiverMeasurement& measured,
                                            const Mathematics::Matrix4x4& lightRot,
                                            const int32 originSector[3],
                                            Mathematics::Matrix4x4& out);

    // World distance a shadow lookup can land from its receiver beyond the
    // filter's texel reach: the authored normal offset, the penumbra ceiling
    // and kReceiverMotionAllowanceWorld.
    float ReceiverMarginWorld() const;
    // World distance a shadow lookup can land from its receiver in a cascade
    // fitted to `halfExtent`: ReceiverMarginWorld plus the filter's texel reach
    // in texels of that fit.
    float ReceiverReachWorld(float halfExtent) const;

    // Narrows `bounds.Box` to the measured receivers that can sample cascade
    // `cascadeIndex` of `frame` (its depth range plus the blend band before it),
    // the visible surfaces and the visible air in front of them,
    // widened by the `current` camera's motion since the measurement and by
    // ReceiverReachWorld, and sets `bounds.CasterBox` to those widened
    // receivers. False, leaving `bounds` unchanged, when no measured receiver
    // can reach the cascade or the measurement did not bin its depths.
    bool NarrowFitToReceivers(const ShadowReceiverMeasurement& measured,
                              const CascadeFrameData& frame, uint32_t cascadeIndex,
                              const ShadowReceiverMeasurement::Context& current,
                              const Mathematics::Matrix4x4* measuredToCurrent,
                              CascadeFitBounds& bounds) const;

    // Cascade 0's slice starts at the nearest measured surface
    // (Cascade0SliceStart), but lookups in the air between the eye and it
    // still select cascade 0: volumetric fog, transparent surfaces, and the
    // sun glare at the eye. Widens `bounds.Box` by the measured air in front
    // of view depth `sliceStart` (GatherShadowReceivers), within `fromNear`:
    // the box a slice from the camera near plane fits, plus the eye.
    static void HoldAirInFrontOfCascade0(const ShadowReceiverMeasurement& measured, float sliceStart,
                                         const ShadowReceiverMeasurement::Context& current,
                                         const Mathematics::Matrix4x4* measuredToCurrent,
                                         const Mathematics::AABB& fromNear, CascadeFitBounds& bounds);

    // Evaluate whether `frozen` still covers this frame's cascade. `fresh` are
    // the bounds ComputeCascades just built from the LIVE camera, splits and
    // receivers, render-origin-relative like the fit itself; static +
    // device-free so the fit-stability tests exercise it through
    // ComputeCascades trajectories.
    static bool CanReuseFrozenCascadeFit(const FrozenCascadeFit& frozen,
                                         const CascadeFitBounds& fresh,
                                         const Mathematics::Vector3& lightDir,
                                         const Mathematics::Vector3& cameraPos,
                                         uint32_t cascadeIndex, uint32_t resolution,
                                         const int32 originSector[3],
                                         bool casterReduction,
                                         ShadowProjection projection);

    // Compute a tight orthographic VP for a cascade from its fit bounds in the
    // RENDER-ORIGIN-RELATIVE frame. Fitting there is a precision requirement:
    // world-magnitude positions carry ~ULP(|eye|) (0.8 m at Earth radius = 20+
    // texels of cascade 0), which both mis-centres the box and sizes it against
    // the wrong geometry, until the true slice falls outside it.
    // `viewId` and `cascadeIdx` key the per-(view, cascade) hysteresis state
    // that stabilises the AABB ortho extents under camera rotation; pass 0 /
    // 0 if the caller doesn't need stability tracking. `originSector` is the
    // render origin the bounds are relative to: CascadeFit::VPRel carries the
    // local eye (the precise matrix — the depth pass and receivers consume it)
    // and CascadeFit::VP the same fit with the origin added back, for world-space
    // culling only. Sector 0 => the two are byte-identical (dark ship).
    // `guardBandFraction` inflates the raw half-extent and the light-space far
    // bound before snapping (the fit-freeze travel budget); 0 reproduces the
    // exact pre-freeze fit.
    CascadeFit ComputeCascadeLightVP(
        const CascadeFitBounds& bounds,
        const Mathematics::Vector3& lightDir,
        uint32_t resolution,
        Rendering::ViewId viewId,
        uint32_t cascadeIdx,
        const int32 originSector[3],
        // Cascade-0 texel size already resolved out of AUTO by the caller.
        // WorldTexel only; the other modes ignore it.
        float worldTexelBase,
        float guardBandFraction);
};

} // namespace Engine::Renderer
} // namespace GameEngine
