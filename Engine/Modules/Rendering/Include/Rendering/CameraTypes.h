#pragma once

#include "Types/Types.h"

#include <memory>

namespace GameEngine
{
namespace Rendering
{

class ICullingStrategy;

using CameraId = uint32;

// Shared per-frame camera data used for rendering.
// Layout is column-major 4x4 matrices suitable for direct GPU upload,
// matching the existing Scene View camera uniform.
struct CameraData
{
    float view[16];     // View matrix (full world space)
    float proj[16];     // Projection matrix
    float viewProj[16]; // View-projection matrix (full world space)
    float cameraPos[4]; // World-space camera position (xyz), w=1 for editor 2D ortho views, 0 otherwise.
    // Camera-relative rendering (Earth-scale precision). Rebased view / view-proj
    // built from the camera's sector-local position (camera minus render origin);
    // the vertex stage projects render-origin-relative positions through these so
    // the clip position stays fp32-precise at planetary distance. When the origin
    // is inactive (renderOriginSector.xyz all zero) these equal view/viewProj and
    // the shader takes the full-world path (byte-identical to the pre-feature
    // layout). Filled by RenderOrigin.h::ComputeRebasedView at the
    // ViewRegistry::ResolveCameraData chokepoint; GLSL mirror camera_ubo_fields.glsl.
    float viewRel[16];
    float viewProjRel[16];
    int32 renderOriginSector[4]; // xyz = render origin sector; w = sector size (meters)
};
static_assert(sizeof(CameraData) == 352,
              "CameraData must be 352 bytes (5 mat4 + 1 vec4 + 1 ivec4) to match camera_ubo_fields.glsl");

// Optional aggregate that carries a camera identifier, its data,
// and an optional debug label. The debugName is non-owning and
// intended only for tooling / logging / UI, not core logic.
struct CameraInfo
{
    CameraId id;
    CameraData data;
    const char* debugName; // may be nullptr
};

// --- Views ---

// Small, opaque identifier for a logical view.
using ViewId = uint32;

// Minimal handle type for view render targets. This is intentionally
// just a 32-bit integer so it can be populated from different backends
// (e.g. RenderGraph logical texture handles) without coupling this
// header to a particular RG implementation.
using ViewTextureHandle = uint32;

struct ViewTargets
{
    // Main color target used for world/entity rendering for this view.
    // For MSAA, this is typically the multisampled color; resolve target
    // (if any) is expressed via `resolve` below.
    ViewTextureHandle color = 0; // 0 = unused

    // Depth-stencil attachment for this view.
    ViewTextureHandle depth = 0; // 0 = unused

    // Optional single-sample resolve target exposed to UI/post (e.g.
    // SceneView.Color when color is SceneView.ColorMSAA). 0 when not used.
    ViewTextureHandle resolve = 0; // 0 = unused

    // Sample count of the color target, tracked so a runtime MSAA change that
    // re-specs the target in place (same handle, new sample count) is still seen as
    // a target change — handle comparison alone misses it.
    uint32_t colorSampleCount = 0;

    // Clear configuration
    bool clearColor = false;
    bool clearDepth = false;
    bool clearStencil = false;
    float clearColorValue[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float clearDepthValue = 0.0f;  // reverse-Z: clear to far
    uint8 clearStencilValue = 0;
};

// What a view exists FOR. Behavioral decisions key off this, never off
// debugName substrings — name matching already rotted once (an ocean gate
// compared against a view name that no longer existed anywhere).
enum class ViewPurpose : uint8_t
{
    Game,          // gameplay view (Player, editor Game View)
    EditorScene,   // editor scene viewport
    EditorPreview, // thumbnails, bookmark previews, asset previews
    UtilityCapture // hidden capture views (probes, mirrors, depth captures)
};

// Number of ViewPurpose values, for tables indexed by purpose (the per-view-class
// LOD budget overrides). Keep in step when adding a purpose.
inline constexpr size_t kViewPurposeCount = 4;

// Whether extraction/culling feed the view every frame, or only for frames
// its owner explicitly requested (RenderServices::RequestViewFrame). OnDemand
// views auto-expire — no manual disarm, no hand-rolled mask flipping.
enum class ViewParticipation : uint8_t
{
    Always,
    OnDemand
};

// Lightweight description of a view: which camera to look through, which
// render targets to use, and an optional debug label. Higher-level pipeline
// selection (Forward+, Deferred, debug) will be layered on separately.
struct ViewDesc
{
    ViewId id;
    CameraId cameraId;
    ViewTargets targets;
    // Render layer mask used to decide which renderables should be submitted to
    // this view. Renderables with (renderLayerMask & view.renderLayerMask) == 0
    // are skipped during extraction/submission.
    //
    // Default is all bits set so existing behavior (render everything) is
    // preserved unless callers explicitly scope the view.
    uint32 renderLayerMask = 0xFFFFFFFFu;
    // When false, the active render pipeline skips this view. The view can
    // still render through explicit passes such as AddWorldPassForView.
    bool activeRenderPipeline = true;
    ViewPurpose purpose = ViewPurpose::Game;
    // OnDemand views participate in extraction, batch-key build, and the GPU
    // culling loop only while participationFrames > 0 (RequestViewFrame sets
    // it; BeginWorldDrawFrame decrements it once per frame). Every per-view
    // consumer gates through ActiveRenderLayerMask(), so an expired view
    // costs nothing — the mechanism the probe capture and ocean mirror
    // previously hand-rolled by flipping renderLayerMask.
    ViewParticipation participation = ViewParticipation::Always;
    uint32 participationFrames = 0;

    uint32 ActiveRenderLayerMask() const
    {
        return (participation == ViewParticipation::OnDemand && participationFrames == 0)
                   ? 0u
                   : renderLayerMask;
    }
    // Optional association of this view to a specific ECS world.
    // When non-zero, render extraction should only submit entities from the
    // matching world id. A value of 0 means "accept submissions from any world"
    // (back-compat default).
    uint64 worldId = 0;
    const char* debugName; // may be nullptr

    // Per-view culling strategy. Null = use the engine's default
    // FrustumCullingStrategy at schedule time. Editor thumbnails / asset
    // previews opt out via NoneCullingStrategy; future strategies (HZB,
    // spatial-partition, shared-from-other-view) plug in by assigning a
    // different ICullingStrategy implementation. shared_ptr because the
    // owner (typically the caller of AllocateView) may want the same
    // strategy instance to back multiple views.
    std::shared_ptr<ICullingStrategy> cullingStrategy;
};

// Optional clear configuration used by SetViewTargets variants. Canonical home
// is here (Rendering::ViewClearConfig).
struct ViewClearConfig
{
    bool clearColor;
    float clearColorValue[4];
    bool clearDepth;
    float clearDepthValue;
    bool clearStencil;
    uint8_t clearStencilValue;

    constexpr ViewClearConfig(
        bool clearColorIn = false,
        float clearR = 0.0f,
        float clearG = 0.0f,
        float clearB = 0.0f,
        float clearA = 1.0f,
        bool clearDepthIn = false,
        float clearDepthValueIn = 0.0f,
        bool clearStencilIn = false,
        uint8_t clearStencilValueIn = 0)
        : clearColor(clearColorIn),
          clearColorValue{clearR, clearG, clearB, clearA},
          clearDepth(clearDepthIn),
          clearDepthValue(clearDepthValueIn),
          clearStencil(clearStencilIn),
          clearStencilValue(clearStencilValueIn)
    {
    }
};

} // namespace Rendering
} // namespace GameEngine
