// RenderServicesDetail.h
// Shared file-scope helpers for the RenderServices*.cpp implementation family.
// Private to Engine/Source/Engine/Rendering — not part of the public API.
#pragma once

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialSsboLayout.h"
#include "Engine/Rendering/PointShadowFaceCull.h"
#include "Engine/Rendering/ShadowFrameInfo.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3.h"
#include "Rendering/CameraTypes.h"

#include <array>
#include <optional>
#include <span>
#include <type_traits>

namespace GameEngine
{
namespace Engine::Renderer
{

// 4x4 identity matrix for fallback transforms and camera UBOs.
static const Mathematics::Matrix4x4 kIdentity4x4 = Mathematics::Matrix4x4::Identity();

inline constexpr uint8_t kAreaShadowCullingIndex = 0xFEu;
inline constexpr uint8_t kSpotShadowCullingIndex = 0xFDu;
// M1: each atlas (slot, face) pair needs its OWN GPU-cull / M2a-survivor key —
// two lights' faces see different casters. The key is base + slot*6 + face (==
// base + PointShadowSlotFaceLayer). Base 0x10 keeps the whole range clear of the
// cascade indices (0..3), area (0xFE), spot (0xFD), and none (0xFF).
inline constexpr uint8_t kPointShadowCullingIndexBase = 0x10u;
static_assert(kPointShadowCullingIndexBase + kMaxPointShadowSlots * kPointShadowFaceCount <=
                  kSpotShadowCullingIndex,
              "point-shadow (slot,face) cull keys must not collide with spot/area/none");
// Reflection-probe cube faces: one color (non-shadow) cull slice per face,
// fanned out through CascadeCullingGroup::cascadeIndexBase. Base 0x80 keeps
// [base, base + 6) clear of the cascades (0..3), the point-shadow block, spot
// (0xFD), area (0xFE) and none (0xFF).
inline constexpr uint8_t kProbeFaceCullingIndexBase = 0x80u;
inline constexpr uint32_t kProbeFaceCount = 6u;
static_assert(kProbeFaceCullingIndexBase >=
                  kPointShadowCullingIndexBase + kMaxPointShadowSlots * kPointShadowFaceCount,
              "probe-face cull keys must not collide with the point-shadow block");
static_assert(kProbeFaceCullingIndexBase + kProbeFaceCount <= kSpotShadowCullingIndex,
              "probe-face cull keys must not collide with spot/area/none");
inline constexpr uint8_t ProbeFaceCullingIndex(uint32_t face)
{
    return static_cast<uint8_t>(kProbeFaceCullingIndexBase + face);
}

// MaterialBinder per-pass descriptor cache discriminator for world (color)
// passes. The camera UBO lives in a PER-PASS set keyed on (view, keywords,
// layout, passInstanceIndex), so two passes of ONE view that upload different
// cameras must differ here — a reflection probe captures its six cube faces as
// six passes of one view, each with its own face camera. The high half
// namespaces world passes away from DepthDrawRecorder's (DepthPassType << 16)
// encoding of the same field.
inline constexpr uint32_t kWorldPassInstanceTag =
    static_cast<uint32_t>(DepthPassType::Count) << 16;
inline constexpr uint32_t WorldPassInstanceIndex(
    uint8_t sliceCascadeIndex, Rendering::GPUDrawStreamBuilder::SlicePhase phase)
{
    return kWorldPassInstanceTag | (static_cast<uint32_t>(sliceCascadeIndex) << 1)
           | (phase == Rendering::GPUDrawStreamBuilder::SlicePhase::B ? 1u : 0u);
}
// Every probe face is its own key, and no world key can land in the depth
// encoding's space (its low half spans 9 bits, well inside one << 16 block).
static_assert(WorldPassInstanceIndex(ProbeFaceCullingIndex(0),
                                     Rendering::GPUDrawStreamBuilder::SlicePhase::A)
                  != WorldPassInstanceIndex(ProbeFaceCullingIndex(1),
                                            Rendering::GPUDrawStreamBuilder::SlicePhase::A),
              "two probe faces of one view must not share a per-pass descriptor cache key");
static_assert(WorldPassInstanceIndex(Rendering::GPUDrawStreamBuilder::kCascadeIndexNone,
                                     Rendering::GPUDrawStreamBuilder::SlicePhase::A)
                  != WorldPassInstanceIndex(Rendering::GPUDrawStreamBuilder::kCascadeIndexNone,
                                            Rendering::GPUDrawStreamBuilder::SlicePhase::B),
              "a view's phase-A and phase-B world passes must not share a key");
static_assert(WorldPassInstanceIndex(0xFFu, Rendering::GPUDrawStreamBuilder::SlicePhase::B)
                  < kWorldPassInstanceTag + (1u << 16),
              "world pass keys must stay inside their own DepthPassType block");
// kPointShadowFaceCount now lives in the public Engine/Rendering/ShadowFrameInfo.h
// (PointShadowFrameInfo needs it, and that type is carried on FeatureDeclareContext).
inline constexpr float kAreaShadowNearPlane = 0.0f;
inline constexpr float kSpotShadowNearPlane = 0.025f;
inline constexpr float kPointShadowNearPlane = 0.025f;
inline constexpr float kPi = 3.14159265358979323846f;

// slotFaceIndex == slot*6 + face (PointShadowSlotFaceLayer). One key per atlas
// (slot, face); consumed by the GPU cull scheduler, the batch registration, and
// the M2a survivor lookup (which passes DepthOnlyPassParamsRG::CascadeIndex, set
// to the same slot*6+face).
inline constexpr uint8_t PointShadowCullingIndex(uint32_t slotFaceIndex)
{
    return static_cast<uint8_t>(kPointShadowCullingIndexBase + slotFaceIndex);
}

// AreaShadowFrameInfo / SpotShadowFrameInfo / PointShadowFrameInfo are defined in
// the public Engine/Rendering/ShadowFrameInfo.h (included above) so
// FeatureDeclareContext can carry them as optional snapshots. The builders below
// stay implementation-private.

// Derive the shadow frame info for the first shadow-casting light of each type
// in the world light list (defined in RenderServicesDetail.cpp).
AreaShadowFrameInfo BuildAreaShadowFrameInfo(std::span<const ExtractedLight> lights);
SpotShadowFrameInfo BuildSpotShadowFrameInfo(std::span<const ExtractedLight> lights);

// Populate one point light's cube-face geometry (6 view/VP matrices, near/far,
// 90° proj) plus its S1 face-visibility mask and light-level reject. out.valid is
// set true iff the light is (cull-)visible and keeps >=1 face; `tier` and
// `packedLightIndex` are stamped in. Does NOT set shadowSlot / tileResolution
// (the atlas planner owns those). When `cull` is non-null the light is rejected
// if its range sphere is off-screen; null keeps every face. Called per candidate
// by RenderServices::EnsurePointShadowAssignment.
void PopulatePointShadowGeometry(PointShadowFrameInfo& out, const Mathematics::Vector3& positionWS,
                                 float range, uint32_t tier, uint32_t packedLightIndex,
                                 const PointShadowCameraCull* cull);

// Screen-coverage radius of a light's range sphere, in a resolution-independent
// NDC size scaled to a reference screen half-height — the atlas planner's tier +
// importance input. Camera inside the sphere => a very large coverage.
float PointShadowScreenCoverageRadiusPx(const ::GameEngine::Rendering::CameraData& camData,
                                        const Mathematics::Vector3& lightPositionWS, float range);

// Build the camera-side face-cull inputs for a view once, in full world space
// (ResolveCameraData's view/proj), so the declaration path and the GPU cull
// scheduler feed PopulatePointShadowGeometry identical data (design A10, no drift).
// Returns nullopt for a degenerate camera (thumbnail before its first update).
std::optional<PointShadowCameraCull> MakePointShadowCullInputs(
    const ::GameEngine::Rendering::CameraData& camData);

// Mesh-draw skip/issue diagnostics: append to the shared recent-diagnostics ring
// (surfaced by RenderServices::GetRecentMeshDrawDiagnostics and DebugMetrics).
// The residency gate's refusal reason is a named constant because the recorders
// that emit it and the reason->metric-code mapping live in different files: one
// definition is what keeps a rename from silently orphaning the mapping and
// coding every refusal as "unknown".
inline constexpr const char* kMeshNotDrawableReason = "mesh-not-drawable";
void LogMeshDrawSkipDiagnostic(const char* pass, const char* reason,
                               ::GameEngine::Rendering::ViewId viewId,
                               uint32_t materialIndex, uint32_t meshIndex,
                               bool drawStreamReady, bool drawSlotValid,
                               uint64_t indirectionBda, uint64_t instanceBda,
                               bool supportsBufferDeviceAddress, size_t slotCount);
void LogMeshPassDiagnostic(const char* pass, const char* reason,
                           ::GameEngine::Rendering::ViewId viewId,
                           size_t entityKeyCount, size_t commandCount, size_t slotCount);
void PushMeshDrawIssuedDiagnostic(const char* pass, ::GameEngine::Rendering::ViewId viewId,
                                  uint32_t materialIndex, uint32_t meshIndex,
                                  uint32_t maxDrawCount, size_t slotCount);

// GE_DRAW_ATTRIBUTION=1: per-pass draw-submission attribution (draws issued,
// non-redundant pipeline/descriptor binds) logged throttled from the world
// color pass and the depth recorder. The A/B evidence channel for the
// GE_DRAW_CONSOLIDATION kill-switch. Read once per process.
bool DrawAttributionEnabled();
// Throttled emit: one log per 127 calls of a single global tick shared by all
// call sites. The prime interval rotates the sample across every (pass, view,
// family) site — a round modulus would resample the same site forever when the
// per-frame site count divides it.
void LogDrawAttribution(const char* pass, ::GameEngine::Rendering::ViewId viewId,
                        size_t entityKeyCount, uint32_t drawsIssued,
                        uint32_t pipelineBinds, uint32_t descriptorBinds,
                        bool consolidationActive, uint32_t liveGroupCount);

// Bytes of the CompatInstanceList a pass table binds: the view's instance index
// list as uploaded at its first pass declaration, 0 for the fallback. The
// compat recorders read it before the table moves into the binder and assert it
// still matches the builder's live list, so a rebuild of the view between its
// first declaration and recording (offsets into a list the GPU never got) fails
// loudly instead of drawing the wrong instances.
uint64_t UploadedCompatInstanceListBytes(const ResolvedPassResources& resources);

// Map a material's authored filter mode to the engine sampler preset.
::GameEngine::Rendering::SamplerPreset SamplerPresetFromMaterialFilter(MaterialTextureFilter f);

// Push-constant payload for the GE_INSTANCED shader path. Carries buffer
// device addresses for the instance-indirection buffer + GPUInstances so the
// vertex shader can dereference them via GL_EXT_buffer_reference instead of
// binding them as SSBOs on set 0. Layout must match instance_io.glsl's
// InstancedPC block. See audit §7.1.7.
struct ComposedPCInstanced
{
    uint64_t indirectionAddr;   // BDA of GPUDrawStreamBuilder's shared indirection, or a sorted pass's own (offset included)
    uint64_t gpuInstancesAddr;  // BDA of GPUScene per-frame instance buffer (offset 0)
};
static_assert(std::is_trivially_copyable_v<ComposedPCInstanced>, "ComposedPCInstanced must be trivially copyable");
static_assert(sizeof(ComposedPCInstanced) == 16, "ComposedPCInstanced must be 16 bytes to match GLSL InstancedPC layout");

// Maximum number of textures in the global bindless descriptor array.
// Must match the descriptor set created in Initialize() and the pipeline
// layout patching in CompileMaterialPipeline().
// Initialized at runtime from device capabilities (RenderServices::Initialize
// writes it). Keep the fallback conservative so any pre-initialization layout
// creation cannot exceed Vulkan/MoltenVK's common update-after-bind sampler limit.
inline uint32_t kMaxBindlessTextures = 1;

// Canonical layout for set 1, the material texture set, for whichever indexing
// mode `device` selects: the bindless array sized from device caps (falling
// back to kMaxBindlessTextures pre-initialization), or the Classic profile's
// fixed 8-slot bind group. Pipelines patch their reflected set 1 with this —
// SPIR-V reflection collapses the unbounded bindless array to count=1, and on
// Classic a variant that samples only some slots would otherwise reflect a
// layout no cached bind group is compatible with. Defined in
// RenderServicesDetail.cpp.
::GameEngine::Rendering::DescriptorSetLayoutDesc CreateMaterialTextureSetLayout(
    ::GameEngine::Rendering::IDevice* device);

} // namespace Engine::Renderer
} // namespace GameEngine
