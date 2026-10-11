#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "ECS/Components.h"
#include "ECS/Entity.h"
#include "ECS/OrderedHandleSet.h"

#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/PostProcessEffectRegistry.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Core/Engine.h"
#include "Rendering/Common/MatrixUtils.h"

#include "AssetCore/AssetRegistry.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/GIEmitter.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/Particles.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/Ocean.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "GPUFogParticles/GPUFogParticlesMaterial.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Ocean/OceanRenderFeature.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/GPUInstanceWorldKey.h"
#include "Rendering/Core/HashUtils.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

inline bool IsFrameTraceEnabled()
{
    static bool s_Enabled = []()
    {
        const char* env = std::getenv("GE_FRAME_TRACE");
        if (!env || !*env)
        {
            return false;
        }

        // Treat "0" as explicit off; anything else enables tracing.
        return !(env[0] == '0' && env[1] == '\0');
    }();
    return s_Enabled;
}

inline bool IsLightDiagEnabled()
{
    static bool s_Enabled = []()
    {
        const char* env = std::getenv("GE_RENDER_DIAG_LIGHTS");
        if (!env || !*env)
        {
            return false;
        }
        return !(env[0] == '0' && env[1] == '\0');
    }();
    return s_Enabled;
}

inline bool IsExtractionDiagEnabled()
{
    static bool s_Enabled = []()
    {
        const char* env = std::getenv("GE_RENDER_DIAG_WORLD_DRAW_VALUES");
        if (!env || !*env)
            return false;
        return !(env[0] == '0' && env[1] == '\0');
    }();
    return s_Enabled;
}

// Fusion S3 lane selector: the dirty-feed fast path is the default;
// GE_EXTRACTION_FEED=0 pins the full lane (ON-unless-"0", the standard
// flag idiom — unset/empty/non-"0" all mean ON). Process-static latch —
// unit tests select the lane through the constructor bool seam instead of
// per-fixture env pins (the flip arc's G1 lesson).
inline bool IsExtractionFeedEnabled()
{
    static bool s_Enabled = []()
    {
        const char* env = std::getenv("GE_EXTRACTION_FEED");
        if (!env || !*env)
            return true;
        return !(env[0] == '0' && env[1] == '\0');
    }();
    return s_Enabled;
}

// Ceiling on the always-refresh subset, in unique carrier entities: its
// re-prepare loop is serial and re-resolves every input of a carrier, so past
// this many carriers the parallel full lane is cheaper. 4096 is the value the
// subset shipped against; a skinned-N sweep is the datum that could move it.
// Bulk motion has no ceiling: the patch lane runs in parallel at any count.
constexpr uint32_t kMaxAlwaysRefreshDefault = 4096u;

inline void Normalize3(float& x, float& y, float& z)
{
    const float len2 = x * x + y * y + z * z;
    if (len2 <= 1e-12f)
    {
        x = 0.0f;
        y = 1.0f;
        z = 0.0f;
        return;
    }
    const float invLen = 1.0f / std::sqrt(len2);
    x *= invLen;
    y *= invLen;
    z *= invLen;
}

struct WorldTimeOfDayState
{
    float hours = 12.0f;
    float dayKeyTimesHours[4] = {0.0f, 6.0f, 12.0f, 18.0f};
};

WorldTimeOfDayState FindWorldTimeOfDayState(GameEngine::ECS::World& world)
{
    WorldTimeOfDayState state{};
    bool found = false;
    world.Query<
             GameEngine::ECS::Read<GameEngine::Components::SkyEnvironment>>()
        .Each([&](GameEngine::ECS::EntityHandle,
                  const GameEngine::Components::SkyEnvironment& sky)
              {
                  if (found)
                      return;
                  state.hours = sky.TimeOfDayHours;
                  state.dayKeyTimesHours[0] = sky.DayKeyTimesHours.Midnight;
                  state.dayKeyTimesHours[1] = sky.DayKeyTimesHours.Dawn;
                  state.dayKeyTimesHours[2] = sky.DayKeyTimesHours.Midday;
                  state.dayKeyTimesHours[3] = sky.DayKeyTimesHours.Sunset;
                  found = true; });
    return state;
}

inline void ReleaseMeshGpuInstance(GameEngine::Rendering::GPUScene& scene,
                                   GameEngine::Components::MeshGPUData& meshGpu)
{
    if (meshGpu.instanceIndex != 0xFFFFFFFFu)
    {
        scene.RemoveInstance(meshGpu.instanceIndex);
        meshGpu.instanceIndex = 0xFFFFFFFFu;
    }
    meshGpu.meshIndex = 0xFFFFFFFFu;
    meshGpu.materialIndex = 0xFFFFFFFFu;
}

inline void ClearMeshGpuInstance(GameEngine::Rendering::GPUScene& scene,
                                 GameEngine::Components::MeshGPUData& meshGpu)
{
    if (meshGpu.instanceIndex == 0xFFFFFFFFu)
        return;
    const auto& instances = scene.GetInstances();
    if (meshGpu.instanceIndex >= instances.size())
    {
        meshGpu.instanceIndex = 0xFFFFFFFFu;
        meshGpu.meshIndex = 0xFFFFFFFFu;
        meshGpu.materialIndex = 0xFFFFFFFFu;
        return;
    }
    if (instances[meshGpu.instanceIndex].meshIndex == 0xFFFFFFFFu &&
        instances[meshGpu.instanceIndex].materialIndex == 0xFFFFFFFFu)
    {
        meshGpu.meshIndex = 0xFFFFFFFFu;
        meshGpu.materialIndex = 0xFFFFFFFFu;
        return;
    }

    GameEngine::Rendering::GPUInstance cleared{};
    cleared.meshIndex = 0xFFFFFFFFu;
    cleared.materialIndex = 0xFFFFFFFFu;
    scene.UpdateInstance(meshGpu.instanceIndex, cleared);
    meshGpu.meshIndex = 0xFFFFFFFFu;
    meshGpu.materialIndex = 0xFFFFFFFFu;
}




} // namespace

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

// A2.1 parallel-extraction scratch state (this defines the type the header
// forward-declares). Kept in the .cpp so GPUScene/GPUInstance stay out of the
// public header. All members retain capacity across frames.
struct RenderExtractionParallelState
{
    enum class Op : uint8_t { Drop, Skip, Update, Add, NeedsComponent };

    // One per record, index-aligned with m_Records: chunk c owns the slots in
    // its range exclusively, so prepare never shares a write target.
    struct Prepared
    {
        Op op = Op::Drop;
        Components::MeshGPUData* meshGpu = nullptr; // null only for NeedsComponent
        ECS::EntityHandle entity{};
        GPUInstance payload{};                       // valid for Update/Add/NeedsComponent
        MeshGPUHandle meshHandle{};
        const Material* material = nullptr;
        uint32 flags = 0u;
        uint32 renderLayerMask = 0u;
        uint32 stableInstanceIndex = 0xFFFFFFFFu;    // Update/Skip emission index
        // Mover qualification needs both palette endpoints, and Skip carries no
        // payload to read the current one from.
        uint32 skinPaletteOffset = 0u;
        uint32 prevSkinPaletteOffset = RenderServices::kNoPreviousSkinPalette;
        // Update only: a payload input other than the transform changed, so the
        // apply moves the slot's continuity stamp along with the row.
        bool continuityBreak = false;
        // MeshGPUEntry::uploadSeq the payload was built against; the apply
        // writes it into the Last* caches for NeedsComponent (no MeshGPUData
        // existed in prepare).
        uint64 meshUploadSequence = 0u;
    };

    // Per-chunk accumulators merged in the serial apply (conc-F1): a racing ++
    // on any of these would corrupt the P0 kill-criterion cross-check itself.
    struct DiagCounters
    {
        uint32 skipNoMeshHandle = 0u;
        uint32 skipNoMeshEntry = 0u;
        uint32 skipNoMatGuid = 0u;
        uint32 skipNoMatFound = 0u;
        uint32 skipNoPipeline = 0u;
        uint32 rebuildCount = 0u;
        uint32 skippedCount = 0u;
        // Fusion E8 pending-transient shapes counted UNCONDITIONALLY (the
        // diag-gated skipNoMatFound above stays diagnostics-only). Together
        // with skipNoMeshEntry/skipNoPipeline these are the records the fast
        // path must keep retrying via the full lane until they render —
        // load-bearing for the sentinel->valid mesh re-register shape
        // (correctness F2), not just streaming latency.
        uint32 pendingMaterialLoad = 0u;
        uint32 pendingSentinelIndex = 0u;
    };

    struct ChunkResult
    {
        uint64 digestPartial = 0u;               // Σ recDigest over this chunk (wrapping)
        DiagCounters diag{};
        std::vector<WorldSubmissionRecord> subs; // Update/Skip emissions (stable index)
    };

    std::vector<Prepared> prepared;   // index-aligned with m_Records
    std::vector<ChunkResult> chunks;  // one per chunk

    // Fast-frame reads are resolved under one world lock. No structural writes
    // occur on this lane, so these pointers remain valid through serial apply.
    using DirtyRead = std::tuple<const Components::WorldTransform*,
                                 const Components::MeshGPUData*,
                                 const Components::MeshRenderer*>;
    std::vector<DirtyRead> dirtyReads;
    // Patch-lane scratch, index-aligned with dirtyReads after compaction: each
    // write's GPUScene row, whether it is a frame mover (its prevTransform
    // differs) and its mover record, built where the row is.
    std::vector<GPUScene::InstanceWrite> patchWrites;
    std::vector<uint8_t> patchIsMover;
    std::vector<RenderServices::FrameMoverRecord> patchMoverRecords;
    std::vector<RenderServices::FrameMoverRecord> patchMovers;
    // Patch-lane ordering of the feed snapshot: unique, by entity index. The
    // snapshot spans two windows, so a steady mover appears twice.
    ECS::OrderedHandleSet feedOrder;
    using RefreshRead = std::tuple<const Components::WorldTransform*,
                                   const Components::MeshGPUData*,
                                   const Components::MeshRenderer*,
                                   const Components::GIEmitter*,
                                   const Components::LocalBounds*,
                                   const Components::SkinnedMeshRenderer*,
                                   const Components::SkeletonRef*,
                                   const Components::AnimatorRef*,
                                   const Components::LODGroup*,
                                   const ECS::ComponentDisabled<Components::MeshRenderer>*,
                                   const ECS::ComponentDisabled<Components::SkinnedMeshRenderer>*>;
    std::vector<RefreshRead> refreshReads;
};

namespace
{
using WorldRenderableRecord = RenderExtractionSystem::WorldRenderableRecord;

// L1b: cap on per-frame changed-caster attribution spheres. Past this many
// changed casters the frame is effectively a bulk change — attribution would
// cost more than it saves — so it degrades to an unattributed (every-light)
// bump, exactly the pre-L1b behavior.
constexpr std::size_t kMaxChangedCasterSpheres = 64u;

// GE_SHADOW_PROXIMITY=0 forces every caster bump unattributed — the pre-L1b
// world-scoped invalidation (A/B lane and kill switch, standard GE_ flag
// idiom: ON unless explicitly "0"). Read once; stable for the process.
bool IsShadowProximityKeyingEnabled()
{
    static const bool s_Enabled = []()
    {
        const char* env = std::getenv("GE_SHADOW_PROXIMITY");
        return !env || std::strcmp(env, "0") != 0;
    }();
    return s_Enabled;
}

// The Last* cache write, isolated so prepare (Update/Add, meshGpu already
// present), apply (NeedsComponent, component added there) and the fast
// path's always-refresh subset stay identical.
// Field-based (not record-based) because the fast path has no record — it
// writes the same caches from fresh component reads.
void WriteLastCaches(Components::MeshGPUData& meshGpu, uint32 mshIdx, uint32 matIdx,
                     uint64 meshUploadSequence, uint32 transformVersion, uint32 flags,
                     uint32 skinPaletteOffset, float lodBias, uint32 runtimeId,
                     const float boundsCenter[3], float boundsRadius, int32 sectorX,
                     int32 sectorY, int32 sectorZ, bool prevDiffers)
{
    meshGpu.meshIndex = mshIdx;
    meshGpu.materialIndex = matIdx;
    meshGpu.LastMeshUploadSequence = meshUploadSequence;
    meshGpu.LastTransformVersion = transformVersion;
    meshGpu.LastFlags = flags;
    meshGpu.LastSkinPaletteOffset = skinPaletteOffset;
    meshGpu.LastLodBias = lodBias;
    meshGpu.LastRuntimeId = runtimeId;
    meshGpu.LastBoundsCenter[0] = boundsCenter[0];
    meshGpu.LastBoundsCenter[1] = boundsCenter[1];
    meshGpu.LastBoundsCenter[2] = boundsCenter[2];
    meshGpu.LastBoundsRadius = boundsRadius;
    meshGpu.LastSectorX = sectorX;
    meshGpu.LastSectorY = sectorY;
    meshGpu.LastSectorZ = sectorZ;
    meshGpu.LastPrevDiffers = prevDiffers;
}

// This frame's and last frame's atlas offsets for an entity's skin palette.
// Current == 0 means "not skinned this frame" (atlas slot 0 is the identity
// block, so a zero offset renders bind pose). Previous ==
// kNoPreviousSkinPalette means there is no last-frame pose to pair with.
//
// That needs its own value rather than Previous := Current: the atlas
// bump-allocates in a stable order, so a settled character keeps the SAME
// offset frame to frame and Previous == Current is the STEADY-STATE case. A
// consumer could not tell the two apart, and would happily index last frame's
// atlas at a range that belonged to whichever runtime occupied it then.
struct SkinPaletteOffsets
{
    uint32 Current = 0u;
    uint32 Previous = RenderServices::kNoPreviousSkinPalette;

    bool Active() const { return Current != 0u; }
};

SkinPaletteOffsets ResolveSkinPaletteOffsets(uint32 skeletonId, bool hasActiveAnimation,
                                             uint32 runtimeId)
{
    SkinPaletteOffsets offsets{};
    if (skeletonId == 0 || !hasActiveAnimation || runtimeId == 0)
        return offsets;
    const auto* runtime = SkeletonStore::Instance().GetRuntime(runtimeId);
    if (runtime == nullptr || runtime->CompactSkinMatrices.empty())
        return offsets;
    offsets.Current = runtime->AtlasPaletteOffsetBones;
    offsets.Previous = runtime->PrevAtlasPaletteValid
                           ? runtime->PrevAtlasPaletteOffsetBones
                           : RenderServices::kNoPreviousSkinPalette;
    return offsets;
}

// True when a built payload carries a nonzero object motion vector — the
// value WriteLastCaches latches (see MeshGPUData::LastPrevDiffers).
bool PayloadPrevDiffers(const GPUInstance& payload)
{
    return std::memcmp(payload.transform.Data(), payload.prevTransform.Data(),
                       sizeof(float) * 16) != 0;
}

// GPUInstance flag bits from their extraction inputs. Shared by the parallel
// prepare and the fast path's subset refresh so the two can't disagree on
// bit layout (bit 0 castShadows, bit 1 receiveShadows, bits 2/3 depth class,
// bit 4 mirrored, bit 5 sorted transparent, bit 6 DDGI GI emitter,
// upper 16 = worldKey).
uint32 ComputeInstanceFlags(bool castShadows, bool receiveShadows,
                            Rendering::MaterialDepthClass depthClass, bool sortedTransparent,
                            bool giEmitter, uint64 worldId)
{
    uint32 flags = 0u;
    if (castShadows)
        flags |= 1u;
    if (receiveShadows)
        flags |= 2u;
    flags |= Rendering::DepthClassInstanceFlagBits(depthClass);
    if (sortedTransparent)
        flags |= Rendering::kInstanceFlagSortedTransparent;
    if (giEmitter)
        flags |= Rendering::kInstanceFlagGIEmitter;
    const uint32 worldKey16 = Rendering::PackWorldKey16(worldId);
    flags |= (worldKey16 << 16);
    return flags;
}

// The GPUInstance payload build, shared by the parallel-lane rebuild and the
// fast path's cache patch. One body on purpose: the fast path's byte-parity
// with the full lane (lock T4, D9 shadow validator) requires the exact same
// float ops in the exact same order. boundsCenter/localBoundsRadius are in
// LOCAL space (the Last* cache representation; radius < 0 = no bounds).
// prevMatrix = the transform this instance rendered with LAST frame (the
// GPUScene CPU mirror's transform, read before this frame's UpdateInstance
// overwrites it) — the motion-vector source. nullptr = no history (first
// frame, recycled slot, or a render-origin sector change whose prev lives in
// a different sector frame): prev := current, zero object MV.
void BuildGpuInstancePayload(GPUInstance& instance, const float* matrix,
                             const float* prevMatrix, bool hasBounds,
                             const float boundsCenter[3], float localBoundsRadius,
                             uint32 mshIdx, uint32 matIdx, uint32 flags, float lodBias,
                             uint32 skinPaletteOffset, uint32 runtimeId,
                             int32 sectorX, int32 sectorY, int32 sectorZ, uint32 renderLayerMask)
{
    instance = GPUInstance{};
    std::memcpy(instance.transform.Data(), matrix, sizeof(float) * 16);
    std::memcpy(instance.prevTransform.Data(), prevMatrix != nullptr ? prevMatrix : matrix,
                sizeof(float) * 16);
    bool mirrored = false;
    Vector4 normalColumn0;
    MatrixUtils::ComputeNormalMatrixColumns(matrix,
                                            normalColumn0,
                                            instance.normalMatrixCol1,
                                            instance.normalMatrixCol2,
                                            &mirrored);
    instance.normalMatrixCol0 = Vector3(normalColumn0.x, normalColumn0.y, normalColumn0.z);
    instance.renderLayerMask = renderLayerMask;
    // Camera-relative rendering: the transform column stays SECTOR-LOCAL (small);
    // the integer sector is packed into the reserved words and the vertex stage
    // reconstructs the camera-relative position. boundingCenter, by contrast, is
    // kept FULL-WORLD (sector origin added back) because GPU culling / LOD run in
    // world space this slice. For an untagged entity the sector is (0,0,0), so
    // the packed words are zero and boundingCenter is unchanged — byte-identical.
    GameEngine::Engine::Renderer::PackSector(sectorX, sectorY, sectorZ,
                                             instance.sectorPacked[0], instance.sectorPacked[1]);
    const float sectorOx = static_cast<float>(sectorX) * GameEngine::Engine::Renderer::kSectorSize;
    const float sectorOy = static_cast<float>(sectorY) * GameEngine::Engine::Renderer::kSectorSize;
    const float sectorOz = static_cast<float>(sectorZ) * GameEngine::Engine::Renderer::kSectorSize;
    if (hasBounds)
    {
        const float* m = matrix;
        const float* c = boundsCenter;
        instance.boundingCenter = Vector3(
            m[0] * c[0] + m[4] * c[1] + m[8] * c[2] + m[12] + sectorOx,
            m[1] * c[0] + m[5] * c[1] + m[9] * c[2] + m[13] + sectorOy,
            m[2] * c[0] + m[6] * c[1] + m[10] * c[2] + m[14] + sectorOz);
        // The sphere has to contain the mesh under the WHOLE transform, which
        // is that transform's largest singular value — the same number as its
        // largest COLUMN only while the basis is perpendicular, and short of it
        // on a sheared one (MatrixUtils::LargestAxisScale).
        instance.boundingRadius = localBoundsRadius * MatrixUtils::LargestAxisScale(m);
    }
    else
    {
        instance.boundingCenter =
            Vector3(matrix[12] + sectorOx, matrix[13] + sectorOy, matrix[14] + sectorOz);
        instance.boundingRadius = 1.0f;
    }
    instance.meshIndex = mshIdx;
    instance.materialIndex = matIdx;
    instance.flags = flags;
    // Winding parity is a pure function of the world matrix (re-derived on every
    // build from the det sign above), so it is NOT stored in LastFlags: the
    // transform-version gate already forces a rebuild on any matrix change,
    // including a mirror toggle, and the fast path rebuilds from wt->matrix.
    if (mirrored && Rendering::MirroredWindingEnabled())
        instance.flags |= Rendering::kInstanceFlagMirrored;
    instance.lodBias = lodBias;
    instance.skinPaletteOffset = skinPaletteOffset;
    instance.runtimeId = runtimeId;
}

// The per-view submission emission, shared so every call site emits
// identically.
void EmitSubmissions(std::vector<WorldSubmissionRecord>& subs,
                     const std::vector<const ViewDesc*>& matchingViews,
                     MeshGPUHandle meshHandle, const Material* material,
                     uint32 instanceIndex, uint32 flags, uint32 renderLayerMask)
{
    for (const ViewDesc* view : matchingViews)
    {
        if ((renderLayerMask & view->ActiveRenderLayerMask()) == 0u)
            continue;
        WorldSubmissionRecord sub{};
        sub.viewId = view->id;
        sub.meshHandle = meshHandle;
        sub.material = material;
        sub.instanceIndex = instanceIndex;
        sub.renderLayerMask = renderLayerMask;
        sub.flags = flags;
        subs.push_back(sub);
    }
}

// Pure per-record prepare (parallel-safe). Reads rec + const registries; writes
// ONLY this record's Prepared slot, its chunk's ChunkResult, and — for entities
// that already own MeshGPUData — that entity's disjoint Last* caches. No
// GPUScene mutation: every write is deferred to the serial apply (§3).
// allowPrevSettle: true only on the first extraction run of a feed swap
// window. The LastPrevDiffers settle (prev := current for a stopped mover)
// must not fire on a same-window re-run (editor passive refresh steps the
// same frame twice) — that would zero a fresh mover's motion vector.
void PrepareRecord(const WorldRenderableRecord& rec,
                   RenderExtractionParallelState::Prepared& out,
                   RenderExtractionParallelState::ChunkResult& cr,
                   MeshGPURegistry& meshReg, MaterialRegistry& matReg,
                   const std::vector<const ViewDesc*>& matchingViews,
                   RenderServices& rs, uint64 worldId, bool extractDiag,
                   const GPUScene& scene, bool allowPrevSettle)
{
    using Op = RenderExtractionParallelState::Op;
    out.op = Op::Drop;

    // Membership digest term — accumulated for EVERY record BEFORE the readiness
    // gates below (so streaming/recompiling assets don't thrash probe rebakes).
    // Order-independent wrapping sum.
    {
        using ::GameEngine::Rendering::HashUtils::HashValue;
        constexpr uint64 kFnvOffsetBasis = 14695981039346656037ull;
        uint64 recDigest = HashValue(kFnvOffsetBasis, rec.meshRenderer.meshGpuHandleId);
        recDigest = HashValue(recDigest, rec.meshRenderer.materialAssetGuid);
        recDigest = HashValue(recDigest, rec.meshRenderer.castShadows);
        recDigest = HashValue(recDigest, rec.meshRenderer.renderLayerMask);
        // In the digest or a live toggle does nothing until an unrelated field
        // happens to re-arm extraction.
        recDigest = HashValue(recDigest, rec.giEmitter);
        cr.digestPartial += recDigest;
    }

    MeshGPUHandle meshHandle(rec.meshRenderer.meshGpuHandleId);
    if (!meshHandle.IsValid())
    {
        ++cr.diag.skipNoMeshHandle;
        return;
    }
    const auto* meshEntry = meshReg.Find(meshHandle);
    if (!meshEntry)
    {
        ++cr.diag.skipNoMeshEntry;
        return;
    }

    const Material* material = nullptr;
    {
        const GameEngine::GUID matGuid = rec.meshRenderer.materialAssetGuid.ToGuid();
        if (!matGuid.IsNull())
        {
            material = matReg.Find(matGuid);
            if (!material)
            {
                // Non-null GUID with no registered material: may still stream
                // in — E8-pending. A GUID whose asset was deleted from disk
                // never resolves and pins the full lane, silently but
                // attributably (the per-shape stats row, correctness F6).
                ++cr.diag.pendingMaterialLoad;
                if (extractDiag)
                    ++cr.diag.skipNoMatFound;
            }
        }
        else if (extractDiag)
        {
            // Null GUID is permanently unrenderable, NOT pending: a later
            // assignment is a MeshRenderer value write -> E5.
            ++cr.diag.skipNoMatGuid;
        }
    }
    if (!material)
        return;
    if (!material->GetGraphicsPipelineId().IsValid())
    {
        ++cr.diag.skipNoPipeline;
        return;
    }

    Components::MeshGPUData* meshGpu = rec.meshGpu; // null => first-frame (NeedsComponent, D2)
    // A reset may leave a derived component without a backing row. Re-enter
    // the existing Add path before reading either its layer or transform.
    if (meshGpu && meshGpu->instanceIndex != 0xFFFFFFFFu &&
        meshGpu->instanceIndex >= scene.GetInstances().size())
        meshGpu->instanceIndex = 0xFFFFFFFFu;

    const uint32 mshIdx = meshEntry->gpuMeshIndex;
    const uint32 matIdx = material->GetGpuSceneMaterialIndex();
    if (mshIdx == 0xFFFFFFFFu || matIdx == 0xFFFFFFFFu)
    {
        // GPU index still the sentinel: asset not GPU-resident yet (E8; this
        // shape covers sentinel->valid mesh re-registers, correctness F2).
        ++cr.diag.pendingSentinelIndex;
        return;
    }

    const bool castShadows = rec.meshRenderer.castShadows &&
                             (!rec.hasBounds || rec.bounds.CastShadows);
    const uint32 flags = ComputeInstanceFlags(castShadows, rec.meshRenderer.receiveShadows,
                                              rs.Materials().GetMaterialDepthClass(matIdx),
                                              IsOrderDependentBlendMaterial(*material),
                                              rec.giEmitter, worldId);

    // Bone-palette atlas offset (SkinDiag logging is intentionally omitted here —
    // it is env-gated diagnostics that would log unordered from workers).
    const SkinPaletteOffsets paletteOffsets =
        ResolveSkinPaletteOffsets(rec.skeletonId, rec.hasActiveAnimation, rec.runtimeId);
    const uint32 skinPaletteOffset = paletteOffsets.Current;

    const float boundsRadius = rec.hasBounds ? rec.bounds.Box.Radius() : -1.0f;
    // Render-origin sector (camera-relative rendering). Adding/editing a
    // WorldSectorCoord changes the packed sector AND the full-world boundingCenter
    // without touching the transform version, so it MUST participate in the
    // no-op check — otherwise a component-ADD escalates the full lane but this
    // guard skips the rebuild and sectorPacked stays 0 (instance renders where it
    // does not cull → invisible).
    const int32 secX = rec.hasSectorCoord ? rec.sectorCoord.x : 0;
    const int32 secY = rec.hasSectorCoord ? rec.sectorCoord.y : 0;
    const int32 secZ = rec.hasSectorCoord ? rec.sectorCoord.z : 0;
    // Every payload input except the transform. Split out from the no-op
    // check because it also decides continuity: a rebuild caused by any of
    // these breaks the instance's frame-to-frame continuity (the payload the
    // previous frame rendered with is not the payload this one renders with),
    // whereas a transform-only rebuild does not — prevTransform carries that
    // history exactly. The upload sequence is what sees an in-place mesh
    // reload: the handle and the mesh row survive it, the geometry does not.
    const bool nonTransformInputsUnchanged =
        meshGpu != nullptr &&
        meshGpu->instanceIndex != 0xFFFFFFFFu &&
        meshGpu->meshIndex == mshIdx &&
        meshGpu->materialIndex == matIdx &&
        meshGpu->LastMeshUploadSequence == meshEntry->uploadSeq &&
        meshGpu->LastFlags == flags &&
        scene.GetInstances()[meshGpu->instanceIndex].renderLayerMask == rec.meshRenderer.renderLayerMask &&
        meshGpu->LastSkinPaletteOffset == skinPaletteOffset &&
        meshGpu->LastLodBias == rec.lodBias &&
        meshGpu->LastRuntimeId == rec.runtimeId &&
        meshGpu->LastBoundsRadius == boundsRadius &&
        meshGpu->LastSectorX == secX && meshGpu->LastSectorY == secY &&
        meshGpu->LastSectorZ == secZ &&
        (!rec.hasBounds ||
         (meshGpu->LastBoundsCenter[0] == rec.bounds.Box.center.x &&
          meshGpu->LastBoundsCenter[1] == rec.bounds.Box.center.y &&
          meshGpu->LastBoundsCenter[2] == rec.bounds.Box.center.z));
    const bool instanceUnchanged =
        nonTransformInputsUnchanged &&
        // Stopped mover: force ONE settle rebuild (prev := current), but only
        // on the first run of a new feed window (see allowPrevSettle).
        !(allowPrevSettle && meshGpu->LastPrevDiffers) &&
        meshGpu->LastTransformVersion == rec.worldTransform.Version;

#ifdef _DEBUG
    // Version-contract tripwire. The read of GPUScene here happens during the
    // parallel phase; it is safe because no thread WRITES GPUScene in prepare
    // (§2.3). The warn-once guard is atomic so concurrent chunks can't tear it.
    if (instanceUnchanged && meshGpu &&
        std::memcmp(scene.GetInstances()[meshGpu->instanceIndex].transform.Data(),
                    rec.worldTransform.matrix, sizeof(rec.worldTransform.matrix)) != 0)
    {
        static std::atomic<bool> s_VersionContractWarned{false};
        if (!s_VersionContractWarned.exchange(true, std::memory_order_relaxed))
        {
            Logger::Log::Error(
                "RenderExtraction: WorldTransform.matrix changed without a Version bump "
                "(entity={}) — it will render at a stale transform. See the direct-writer "
                "contract in Transform.h.",
                rec.entity.id);
        }
    }
#endif

    out.meshGpu = meshGpu;
    out.entity = rec.entity;
    out.meshHandle = meshHandle;
    out.material = material;
    out.flags = flags;
    out.renderLayerMask = rec.meshRenderer.renderLayerMask;
    out.skinPaletteOffset = paletteOffsets.Current;
    out.prevSkinPaletteOffset = paletteOffsets.Previous;
    out.meshUploadSequence = meshEntry->uploadSeq;

    if (instanceUnchanged)
    {
        ++cr.diag.skippedCount;
        out.op = Op::Skip;
        out.stableInstanceIndex = meshGpu->instanceIndex;
        EmitSubmissions(cr.subs, matchingViews, meshHandle, material,
                        meshGpu->instanceIndex, flags, rec.meshRenderer.renderLayerMask);
        return;
    }

    // Rebuild: compute the GPUInstance payload (pure f(rec) — legal in prepare
    // even for a null meshGpu, CONC-F2). Motion-vector history: last frame's
    // transform still sits in the GPUScene CPU mirror (prepare only READS the
    // scene, §2.3 — the same guarantee the tripwire above relies on). A sector
    // change means prev lives in a different sector frame: drop history.
    ++cr.diag.rebuildCount;
    const float boundsCenter[3] = {rec.bounds.Box.center.x, rec.bounds.Box.center.y,
                                   rec.bounds.Box.center.z};
    const float* prevMatrix = nullptr;
    if (meshGpu != nullptr && meshGpu->instanceIndex != 0xFFFFFFFFu &&
        meshGpu->LastSectorX == secX && meshGpu->LastSectorY == secY &&
        meshGpu->LastSectorZ == secZ)
        prevMatrix = scene.GetInstances()[meshGpu->instanceIndex].transform.Data();
    // secX/secY/secZ computed above (part of the no-op check).
    BuildGpuInstancePayload(out.payload, rec.worldTransform.matrix, prevMatrix, rec.hasBounds,
                            boundsCenter, boundsRadius, mshIdx, matIdx, flags,
                            rec.lodBias, skinPaletteOffset, rec.runtimeId, secX, secY, secZ,
                            rec.meshRenderer.renderLayerMask);
    const bool prevDiffers = PayloadPrevDiffers(out.payload);

    if (meshGpu == nullptr)
    {
        // First-frame: prepare must NOT AddComponent (structural change forbidden
        // mid-iteration). Defer AddComponent + AddInstance + emission to apply.
        out.op = Op::NeedsComponent;
        return;
    }
    if (meshGpu->instanceIndex == 0xFFFFFFFFu)
    {
        // Component present but no GPUScene slot yet: defer AddInstance + emission
        // (index unknown until apply). Last* caches are per-entity disjoint, so
        // writing them here is safe.
        out.op = Op::Add;
        WriteLastCaches(*meshGpu, mshIdx, matIdx, meshEntry->uploadSeq,
                        rec.worldTransform.Version, flags, skinPaletteOffset, rec.lodBias,
                        rec.runtimeId, boundsCenter, boundsRadius, secX, secY, secZ,
                        prevDiffers);
        return;
    }
    out.op = Op::Update;
    out.stableInstanceIndex = meshGpu->instanceIndex;
    // A slot that keeps its tenant but changes what it renders: the serial
    // apply moves the continuity stamp (GPUScene is read-only in prepare).
    out.continuityBreak = !nonTransformInputsUnchanged;
    WriteLastCaches(*meshGpu, mshIdx, matIdx, meshEntry->uploadSeq, rec.worldTransform.Version,
                    flags, skinPaletteOffset, rec.lodBias, rec.runtimeId, boundsCenter,
                    boundsRadius, secX, secY, secZ, prevDiffers);
    EmitSubmissions(cr.subs, matchingViews, meshHandle, material,
                    meshGpu->instanceIndex, flags, rec.meshRenderer.renderLayerMask);
}

// A2.1 parallel PROCESS: fork the pure per-record prepare over m_Records index
// ranges (one ParallelFor unit per chunk, legal on a worker), then a serial
// apply that performs every GPUScene mutation and merges the per-chunk
// accumulators in record order (D6).
// The fork-join chunk plan both extraction lanes use: about kChunksPerWorker
// chunks per worker, so idle workers steal when per-item cost is uneven, and
// never under kMinChunkItems, so small inputs do not over-split (the same
// order of magnitude as Query::Parallel's MinBatchSizeDefault). A null pool
// plans for one worker.
struct ChunkPlan
{
    std::size_t Workers = 1;
    std::size_t ChunkSize = 0;
    std::size_t ChunkCount = 0;
};

ChunkPlan PlanChunks(JobSystem::WorkStealingThreadPool* js, std::size_t count)
{
    constexpr std::size_t kChunksPerWorker = 4;
    constexpr std::size_t kMinChunkItems = 1000;
    ChunkPlan plan;
    plan.Workers = js ? std::max<std::size_t>(1, js->GetWorkerCount()) : 1;
    const std::size_t targetChunks = plan.Workers * kChunksPerWorker;
    plan.ChunkSize = std::max(kMinChunkItems, (count + targetChunks - 1) / targetChunks);
    plan.ChunkCount = (count + plan.ChunkSize - 1) / plan.ChunkSize;
    return plan;
}

// Runs body(begin, end) over [0, count) as fork-join chunks on `js` when the
// plan has more than one chunk, inline otherwise; legal from a pool worker.
void ForkJoinRanges(JobSystem::WorkStealingThreadPool* js, std::size_t count,
                    const std::function<void(std::size_t begin, std::size_t end)>& body)
{
    if (count == 0)
        return;
    const ChunkPlan plan = PlanChunks(js, count);
    if (!js || plan.ChunkCount < 2)
    {
        body(0, count);
        return;
    }
    const auto runChunk = [&body, &plan, count](std::size_t c)
    {
        const std::size_t begin = c * plan.ChunkSize;
        body(begin, std::min(begin + plan.ChunkSize, count));
    };
    JobSystem::ParallelForOptions options;
    options.Helpers = plan.Workers;
    JobSystem::ParallelFor(js, plan.ChunkCount, runChunk, options);
}

// Resolves handles [begin, end) into their reads. GetComponentsBatch takes
// the world's shared lock and writes only its own output range, so disjoint
// ranges run concurrently.
void ResolveDirtyReads(const ECS::World& world, std::span<const ECS::EntityHandle> handles,
                       std::span<RenderExtractionParallelState::DirtyRead> reads, std::size_t begin,
                       std::size_t end)
{
    world.GetComponentsBatch<Components::WorldTransform, Components::MeshGPUData,
                             Components::MeshRenderer>(handles.subspan(begin, end - begin),
                                                       reads.subspan(begin, end - begin));
}

// The patch lane's per-write build for writes [begin, end): the GPUScene row
// from the fresh WorldTransform and the entity's Last* caches, and, when its
// prevTransform differs, its frame-mover record. Skinned entities never reach
// this lane (every one is an always-refresh member), so no mover carries a pose
// delta or a previous palette. Reads GPUScene rows (the previous transform) and
// writes only the writes' own slots and their entities' MeshGPUData, so
// disjoint ranges run concurrently; GPUScene is not written here.
void BuildPatchedInstances(std::span<const RenderExtractionParallelState::DirtyRead> reads,
                           const GPUScene& scene, std::span<GPUScene::InstanceWrite> writes,
                           std::span<uint8_t> isMover,
                           std::span<RenderServices::FrameMoverRecord> moverRecords,
                           std::size_t begin, std::size_t end)
{
    const std::vector<GPUInstance>& rows = scene.GetInstances();
    for (std::size_t i = begin; i < end; ++i)
    {
        const auto& [wt, mgRead, mr] = reads[i];
        // Granted before the build (StampComponentWriteBatch); see PatchDirtyInstances.
        auto* meshGpu = const_cast<Components::MeshGPUData*>(mgRead);
        // Every non-transform GPUInstance input lives in the Last* caches
        // (D2); their inputs either escalate (E5/E6/E7) or live in the
        // always-refresh subset, so on a fast frame they are current.
        GPUScene::InstanceWrite& write = writes[i];
        write.InstanceIndex = meshGpu->instanceIndex;
        const bool hasBounds = meshGpu->LastBoundsRadius >= 0.0f; // -1 = extracted without bounds
        const float* prevMatrix = rows[meshGpu->instanceIndex].transform.Data();
        BuildGpuInstancePayload(write.Instance, wt->matrix, prevMatrix, hasBounds,
                                meshGpu->LastBoundsCenter, meshGpu->LastBoundsRadius,
                                meshGpu->meshIndex, meshGpu->materialIndex, meshGpu->LastFlags,
                                meshGpu->LastLodBias, meshGpu->LastSkinPaletteOffset,
                                meshGpu->LastRuntimeId, meshGpu->LastSectorX,
                                meshGpu->LastSectorY, meshGpu->LastSectorZ, mr->renderLayerMask);
        const bool differs = PayloadPrevDiffers(write.Instance);
        isMover[i] = (differs && mr) ? 1u : 0u;
        if (isMover[i] != 0u)
            moverRecords[i] = {write.InstanceIndex, MeshGPUHandle(mr->meshGpuHandleId),
                               write.Instance.skinPaletteOffset,
                               RenderServices::kNoPreviousSkinPalette};
        meshGpu->LastTransformVersion = wt->Version;
        meshGpu->LastPrevDiffers = differs;
    }
}

void ProcessRecordsParallel(
    ECS::World& world, GPUScene& scene, RenderServices& rs, uint64 worldId,
    const std::vector<const ViewDesc*>& matchingViews,
    MeshGPURegistry& meshReg, MaterialRegistry& matReg, bool extractDiag,
    const std::vector<WorldRenderableRecord>& records,
    std::vector<WorldSubmissionRecord>& submissions,
    RenderExtractionParallelState& state,
    uint64& contentDigest, uint32_t& rebuildCount, uint32_t& skippedCount,
    uint32_t& skipNoMeshHandle, uint32_t& skipNoMeshEntry, uint32_t& skipNoMatGuid,
    uint32_t& skipNoMatFound, uint32_t& skipNoPipeline,
    uint32_t& pendingMaterialLoad, uint32_t& pendingSentinelIndex,
    double& prepareMs, double& applyMs, bool allowPrevSettle)
{
    using Op = RenderExtractionParallelState::Op;
    using PrepClock = std::chrono::steady_clock;
    const auto elapsed = [](PrepClock::time_point a, PrepClock::time_point b)
    { return std::chrono::duration<double, std::milli>(b - a).count(); };

    const std::size_t recCount = records.size();

    // Chunking: PlanChunks, shared with the patch lane (rebuild cost is
    // unevenly distributed across records, which is what the per-worker
    // chunk count is for).
    // World::GetJobSystem() is authoritative: non-null = fork on that pool,
    // null = deliberately serial (thumbnail/preview and bare test worlds run
    // the inline chunk below) — EngineCore is never consulted. The primary
    // world is wired at creation (EnsurePrimaryWorld) to the same engine pool
    // the SystemManager dispatched THIS extraction on (RenderingLoop::Initialize
    // → EngineCore::GetJobSystem); the fork claims chunks on that pool from
    // whichever thread runs this extraction, a worker included.
    auto* js = world.GetJobSystem();
    const ChunkPlan plan = PlanChunks(js, recCount);
    const std::size_t workers = plan.Workers;
    const std::size_t chunkSize = plan.ChunkSize;
    const std::size_t numChunks = plan.ChunkCount;

    // §7 Q2 diagnostic (logged once per process): does extraction fork from a
    // worker (one worker fewer to help, contending with sibling wave systems) or
    // the main thread (every worker free to help)? This is the single fact that
    // attributes a failed prepare speedup, so it is worth one line at process
    // start.
    {
        static std::atomic<bool> s_LoggedWorkerContext{false};
        // Latch on the first world that actually forks (numChunks > 1), so
        // previews/thumbnails — tiny worlds that run a single inline chunk and
        // may lack a job system — don't consume the once-per-process log.
        if (numChunks > 1 && !s_LoggedWorkerContext.exchange(true, std::memory_order_relaxed))
        {
            const std::size_t wid = js ? js->GetCurrentWorkerId()
                                       : std::numeric_limits<std::size_t>::max();
            const bool onWorker = js && wid != std::numeric_limits<std::size_t>::max();
            Logger::Log::Warning(
                "[A2.1] parallel prepare: jsNull={} onWorker={} workerId={} workers={} records={} chunks={} chunkSize={} forked={}",
                js ? 0 : 1, onWorker ? 1 : 0,
                onWorker ? static_cast<long long>(wid) : -1,
                workers, recCount, numChunks, chunkSize, (js && numChunks > 1) ? 1 : 0);
        }
    }

    // ---- PREPARE (parallel) ----
    const auto prepareStart = PrepClock::now();
    state.prepared.resize(recCount);
    state.chunks.resize(numChunks);
    for (std::size_t c = 0; c < numChunks; ++c)
    {
        auto& cr = state.chunks[c];
        cr.digestPartial = 0u;
        cr.diag = {};
        cr.subs.clear(); // retains capacity across frames
        const std::size_t begin = c * chunkSize;
        const std::size_t end = std::min(begin + chunkSize, recCount);
        cr.subs.reserve((end - begin) * std::max<std::size_t>(1, matchingViews.size()));
    }

    auto prepareChunk = [&](std::size_t c)
    {
        const std::size_t begin = c * chunkSize;
        const std::size_t end = std::min(begin + chunkSize, recCount);
        auto& cr = state.chunks[c];
        for (std::size_t i = begin; i < end; ++i)
            PrepareRecord(records[i], state.prepared[i], cr, meshReg, matReg,
                          matchingViews, rs, worldId, extractDiag, scene, allowPrevSettle);
    };

#ifdef _DEBUG
    const std::size_t capBefore = scene.GetInstances().capacity();
#endif

    JobSystem::ParallelFor(js, numChunks, prepareChunk);

#ifdef _DEBUG
    // R3: prepare defers every AddInstance to apply, so the instance buffer must
    // not have reallocated under the concurrent readers.
    assert(scene.GetInstances().capacity() == capBefore &&
           "GPUScene instance buffer reallocated during parallel prepare");
#endif
    prepareMs = elapsed(prepareStart, PrepClock::now());

    // ---- APPLY (serial, record order) ----
    const auto applyStart = PrepClock::now();
    for (const auto& cr : state.chunks)
    {
        contentDigest += cr.digestPartial;
        rebuildCount += cr.diag.rebuildCount;
        skippedCount += cr.diag.skippedCount;
        skipNoMeshHandle += cr.diag.skipNoMeshHandle;
        skipNoMeshEntry += cr.diag.skipNoMeshEntry;
        skipNoMatGuid += cr.diag.skipNoMatGuid;
        skipNoMatFound += cr.diag.skipNoMatFound;
        skipNoPipeline += cr.diag.skipNoPipeline;
        pendingMaterialLoad += cr.diag.pendingMaterialLoad;
        pendingSentinelIndex += cr.diag.pendingSentinelIndex;
    }

    for (std::size_t i = 0; i < recCount; ++i)
    {
        auto& prep = state.prepared[i];
        switch (prep.op)
        {
        case Op::Drop:
            break;
        case Op::Skip:
            // Skip already emitted its submissions (stable index) in prepare.
            // A deforming character still qualifies here: its root transform
            // and every cached field are unchanged, and its palette offset is
            // stable frame to frame (the atlas bump-allocates in a stable
            // order), so nothing about the payload moves even though the pose
            // does. Without this the in-place-animating case — the common one
            // — would never reach the motion-vector pass.
            if (prep.skinPaletteOffset != 0u)
                rs.AddFrameMover(prep.stableInstanceIndex, prep.meshHandle,
                                 prep.skinPaletteOffset, prep.prevSkinPaletteOffset);
            break;
        case Op::Update:
            scene.UpdateInstance(prep.stableInstanceIndex, prep.payload);
            if (prep.continuityBreak)
                scene.BumpInstanceContinuityStamp(prep.stableInstanceIndex);
            // Object motion, pose deformation, or both -> the TAA movers pass
            // draws it this frame.
            if (prep.skinPaletteOffset != 0u || PayloadPrevDiffers(prep.payload))
                rs.AddFrameMover(prep.stableInstanceIndex, prep.meshHandle,
                                 prep.skinPaletteOffset, prep.prevSkinPaletteOffset);
            break;
        case Op::Add:
            prep.meshGpu->instanceIndex = scene.AddInstance(prep.payload);
            EmitSubmissions(submissions, matchingViews, prep.meshHandle, prep.material,
                            prep.meshGpu->instanceIndex, prep.flags, prep.renderLayerMask);
            break;
        case Op::NeedsComponent:
        {
            const auto& rec = records[i];
            Components::MeshGPUData init{};
            world.AddComponentImmediate<Components::MeshGPUData>(rec.entity, init);
            auto* mg = world.GetComponentForWrite<Components::MeshGPUData>(rec.entity);
            if (!mg)
                break;
            mg->instanceIndex = scene.AddInstance(prep.payload);
            const float boundsCenter[3] = {rec.bounds.Box.center.x, rec.bounds.Box.center.y,
                                           rec.bounds.Box.center.z};
            WriteLastCaches(*mg, prep.payload.meshIndex, prep.payload.materialIndex,
                            prep.meshUploadSequence, rec.worldTransform.Version, prep.flags,
                            prep.payload.skinPaletteOffset, rec.lodBias, rec.runtimeId,
                            boundsCenter, rec.hasBounds ? rec.bounds.Box.Radius() : -1.0f,
                            rec.hasSectorCoord ? rec.sectorCoord.x : 0,
                            rec.hasSectorCoord ? rec.sectorCoord.y : 0,
                            rec.hasSectorCoord ? rec.sectorCoord.z : 0,
                            /*prevDiffers*/ false); // first frame: prev := current
            EmitSubmissions(submissions, matchingViews, prep.meshHandle, prep.material,
                            mg->instanceIndex, prep.flags, prep.renderLayerMask);
            break;
        }
        }
    }

    // Append the chunk-local Update/Skip emissions in chunk order (== record
    // order). Submission order is immaterial to draw order (re-established by
    // WorldDrawBuilder's total-order sort, CONC-F16) — chosen only for
    // determinism.
    for (const auto& cr : state.chunks)
        submissions.insert(submissions.end(), cr.subs.begin(), cr.subs.end());

    applyMs = elapsed(applyStart, PrepClock::now());
}

// Fusion E5 probe: did any chunk holding C take a write grant since the
// gate? Visiting form on purpose — Query::Count() ignores the Changed<>
// filter (correctness A5b) — and const-bound so the probe itself never
// stamps. Morph carriers are probed like every other archetype: their
// MeshRenderer/LocalBounds edits are submission-visible (material, mesh
// handle, layer mask, shadow flags feed the cached submissions the fast
// path replays verbatim) and MUST escalate. That costs nothing at soak:
// idle morph frames take no grants (the MorphTargetSystem writes are
// value-gated), and playing morph-channel animation stamps only on frames
// its runtime-mesh re-register already escalates via E6. The visit covers
// every gated chunk (only changed chunks are visited and no early-exit
// iteration form exists — the cost is escalation-frame-only).
template <typename C>
bool ProbeColumnChanged(ECS::World& world, ECS::ChangeGate& gate, uint64 entrySampledVersion)
{
    bool fired = false;
    auto q = world.Query<ECS::Read<C>>();
    q.template Changed<C>(gate);
    q.BatchEach([&](const C*, std::size_t) { fired = true; });
    // M14: the entry-sampled version becomes the next gate — never an
    // end-of-run resample (a concurrent same-wave writer's stamp could exceed
    // it and be skipped permanently).
    gate.LastRunVersion = entrySampledVersion;
    return fired;
}

} // namespace

RenderExtractionSystem::RenderExtractionSystem(RenderServices* renderServices)
    : RenderExtractionSystem(renderServices, IsExtractionFeedEnabled())
{
}

RenderExtractionSystem::RenderExtractionSystem(RenderServices* renderServices, std::shared_ptr<Particles::ParticleWorldState> particles)
    : RenderExtractionSystem(renderServices)
{
    m_Particles.SetSimulationState(std::move(particles));
}

RenderExtractionSystem::RenderExtractionSystem(RenderServices* renderServices,
                                               bool feedFastPathEnabled)
    : m_RenderServices(renderServices)
{
    m_ParallelState = std::make_unique<RenderExtractionParallelState>();

    m_FeedFastPath = feedFastPathEnabled;
    if (m_FeedFastPath)
    {
        // The subset is re-prepared every fast frame (heavier per entity than
        // the patch), so a heavy-animation scene stays on the parallel full
        // lane past this ceiling.
        m_MaxAlwaysRefresh = kMaxAlwaysRefreshDefault;
        if (m_RenderServices)
        {
            // E6: mesh registry reload/unregister events. Covers model
            // reloads, the in-place valid->sentinel re-register, and submesh
            // unregisters — a freed GPU row referenced by cached submissions
            // is the one enumerated-trigger hole that fails non-conservatively
            // (wrong-mesh render, correctness F2).
            m_MeshReloadSubscription = m_RenderServices->GetMeshGPURegistry().SubscribeReload(
                [this](const GameEngine::GUID& /*assetGuid*/)
                {
                    // Fires on whatever thread drives the reload; consumed on
                    // the extraction thread in Update (TLAS precedent).
                    m_MeshReloadPending.store(true, std::memory_order_relaxed);
                });
        }
    }
}

void RenderExtractionSystem::RequestFullExtractionNextFrame()
{
    if (m_RenderServices)
        m_RenderServices->RequestHlodResidencyExtraction();
}

// Out of line to anchor the vtable; ParticleExtraction's destructor releases
// the particle upload buffers.
RenderExtractionSystem::~RenderExtractionSystem() = default;

namespace
{
constexpr float kDominantWeightEpsilon = 1e-4f;

bool ComputeSpatialWeight(const PostProcessExtractedVolume& vol, const float cameraPos[3],
                          bool hasCameraPos, float& outSpatialWeight)
{
    outSpatialWeight = 1.0f;
    if (vol.IsGlobal)
        return true;
    if (!hasCameraPos || !vol.ValidSpatial)
        return false;

    const float vx = cameraPos[0] - vol.Center[0];
    const float vy = cameraPos[1] - vol.Center[1];
    const float vz = cameraPos[2] - vol.Center[2];

    const float lx = vx * vol.AxisX[0] + vy * vol.AxisX[1] + vz * vol.AxisX[2];
    const float ly = vx * vol.AxisY[0] + vy * vol.AxisY[1] + vz * vol.AxisY[2];
    const float lz = vx * vol.AxisZ[0] + vy * vol.AxisZ[1] + vz * vol.AxisZ[2];

    const float hx = vol.HalfExtents[0];
    const float hy = vol.HalfExtents[1];
    const float hz = vol.HalfExtents[2];

    float dist = 0.0f;
    const auto shape = static_cast<GameEngine::Components::PostProcessVolumeShape>(vol.Shape);
    if (shape == GameEngine::Components::PostProcessVolumeShape::Sphere)
    {
        const float k1x = lx / hx, k1y = ly / hy, k1z = lz / hz;
        const float k1 = std::sqrt(k1x * k1x + k1y * k1y + k1z * k1z);
        const float k2x = lx / (hx * hx);
        const float k2y = ly / (hy * hy);
        const float k2z = lz / (hz * hz);
        const float k2 = std::sqrt(k2x * k2x + k2y * k2y + k2z * k2z);
        dist = std::max(0.0f, k1 > 0.0f && k2 > 0.0f ? k1 * (k1 - 1.0f) / k2 : 0.0f);
    }
    else if (shape == GameEngine::Components::PostProcessVolumeShape::Capsule ||
             shape == GameEngine::Components::PostProcessVolumeShape::Cylinder)
    {
        // Volume local Y is capsule/cylinder axis.
        const float radial = std::max(hx, hz);
        const float halfHeight = hy;
        const float axisCore = std::max(0.0f, halfHeight - radial);
        const float qx = lx;
        const float qy = std::fabs(ly) - axisCore;
        const float qz = lz;
        const float qLen = std::sqrt(qx * qx + qz * qz);
        if (shape == GameEngine::Components::PostProcessVolumeShape::Capsule)
        {
            const float dx = qLen;
            const float dy = std::max(qy, 0.0f);
            dist = std::max(0.0f, std::sqrt(dx * dx + dy * dy) - radial);
        }
        else
        {
            const float dx = qLen - radial;
            const float dy = std::max(qy, 0.0f);
            dist = std::max(dx, 0.0f);
            if (dy > 0.0f)
                dist = std::sqrt(std::max(0.0f, dx) * std::max(0.0f, dx) + dy * dy);
        }
    }
    else
    {
        const float dx = std::max(0.0f, std::fabs(lx) - hx);
        const float dy = std::max(0.0f, std::fabs(ly) - hy);
        const float dz = std::max(0.0f, std::fabs(lz) - hz);
        dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    const float blend = std::max(0.0f, vol.BlendDistance);
    if (dist <= 0.0f)
    {
        outSpatialWeight = 1.0f;
        return true;
    }
    if (blend <= 0.0f || dist >= blend)
        return false;

    const float t = 1.0f - (dist / blend);
    outSpatialWeight = t * t * (3.0f - 2.0f * t);
    return true;
}

void CopyFogVolumeBounds(PostProcessSettings& dst, const PostProcessExtractedVolume& vol)
{
    dst.VolumetricFogIsGlobal = vol.IsGlobal ? 1 : 0;
    dst.VolumetricFogVolumeShape = vol.Shape;
    dst.VolumetricFogVolumeValid = (!vol.IsGlobal && vol.ValidSpatial) ? 1 : 0;
    dst.VolumetricFogVolumeBlendDistance = std::max(vol.BlendDistance, 0.0f);
    dst.VolumetricFogVolumeCenterX = vol.Center[0];
    dst.VolumetricFogVolumeCenterY = vol.Center[1];
    dst.VolumetricFogVolumeCenterZ = vol.Center[2];
    dst.VolumetricFogVolumeAxisXX = vol.AxisX[0];
    dst.VolumetricFogVolumeAxisXY = vol.AxisX[1];
    dst.VolumetricFogVolumeAxisXZ = vol.AxisX[2];
    dst.VolumetricFogVolumeAxisYX = vol.AxisY[0];
    dst.VolumetricFogVolumeAxisYY = vol.AxisY[1];
    dst.VolumetricFogVolumeAxisYZ = vol.AxisY[2];
    dst.VolumetricFogVolumeAxisZX = vol.AxisZ[0];
    dst.VolumetricFogVolumeAxisZY = vol.AxisZ[1];
    dst.VolumetricFogVolumeAxisZZ = vol.AxisZ[2];
    dst.VolumetricFogVolumeHalfExtentX = vol.HalfExtents[0];
    dst.VolumetricFogVolumeHalfExtentY = vol.HalfExtents[1];
    dst.VolumetricFogVolumeHalfExtentZ = vol.HalfExtents[2];
}

// Blends only the volumetric-fog block (plus the shared glow block the volume
// authored) of fogSettings over base.
PostProcessSettings BlendOnlyVolumetricFog(const PostProcessSettings& base,
                                           const PostProcessSettings& fogSettings, float weight)
{
    using GameEngine::Rendering::SettingsBlendGroup;
    PostProcessSettings fogOnly = base;
    CopySettingsGroup(fogOnly, fogSettings, SettingsBlendGroup::VolumetricFog);
    CopySettingsGroup(fogOnly, fogSettings, SettingsBlendGroup::FogGlow);
    SettingsBlendGroupMask groups = SettingsBlendGroupMask::None();
    groups.Set(SettingsBlendGroup::VolumetricFog, true);
    return BlendPostProcessSettings(base, fogOnly, weight, groups);
}

PostProcessSettings BlendOnlyHeightFog(const PostProcessSettings& base,
                                       const PostProcessSettings& fogSettings, float weight,
                                       bool includeSharedGlow)
{
    using GameEngine::Rendering::SettingsBlendGroup;
    PostProcessSettings heightFogOnly = base;
    CopySettingsGroup(heightFogOnly, fogSettings, SettingsBlendGroup::HeightFog);
    if (includeSharedGlow)
        CopySettingsGroup(heightFogOnly, fogSettings, SettingsBlendGroup::FogGlow);
    SettingsBlendGroupMask groups = SettingsBlendGroupMask::None();
    groups.Set(SettingsBlendGroup::HeightFog, true);
    return BlendPostProcessSettings(base, heightFogOnly, weight, groups);
}

// The groups a volume contributes to the spatial blend. Height fog is
// screen-space and blends separately at volume weight (BlendOnlyHeightFog).
SettingsBlendGroupMask SpatialBlendGroups(const PostProcessExtractedVolume& volume)
{
    using GameEngine::Rendering::SettingsBlendGroup;
    SettingsBlendGroupMask groups = SettingsBlendGroupMask::None();
    groups.Set(SettingsBlendGroup::ColorGrade, volume.HasColorGrade);
    groups.Set(SettingsBlendGroup::VolumetricFog, volume.HasVolumetricFog);
    groups.Set(SettingsBlendGroup::AtmosphericCloud, volume.HasAtmosphericCloud);
    groups.Set(SettingsBlendGroup::VolumetricClouds, volume.HasVolumetricClouds);
    return groups;
}

// Highest priority wins; equal priority goes to the heavier (or later, on a
// tie) volume. Discrete state follows the dominant volume so crossing a blend
// boundary never pops between two volumes' values.
struct DominantVolume
{
    const PostProcessExtractedVolume* Volume = nullptr;
    float Weight = 0.0f;
    int32 Priority = std::numeric_limits<int32>::min();

    void Consider(const PostProcessExtractedVolume& volume, float weight)
    {
        if (weight <= kDominantWeightEpsilon)
            return;
        if (Volume && (volume.Priority < Priority || (volume.Priority == Priority && weight < Weight)))
            return;
        Volume = &volume;
        Priority = volume.Priority;
        Weight = weight;
    }
};

struct ResolvedVolumeStack
{
    PostProcessSettings Settings{};
    std::vector<VolumetricFogLocalVolume> FogVolumes;
    // True when at least one volume blended into Settings.
    bool Any = false;
    // Dominant spatially contributing volume carrying a ShadowSettingsEffect.
    const PostProcessExtractedVolume* ShadowVolume = nullptr;
};

// Folds the priority-sorted volumes, as seen from cameraPos, into one settings
// block. With a layerMask, volumes whose PostProcessMask misses it are skipped.
ResolvedVolumeStack ResolveVolumeStack(std::span<const PostProcessExtractedVolume> volumes,
                                       const float cameraPos[3], bool hasCameraPos,
                                       std::optional<uint32> layerMask)
{
    ResolvedVolumeStack stack;
    stack.FogVolumes.reserve(std::min<size_t>(volumes.size(), kMaxVolumetricFogLocalVolumes));
    DominantVolume dominant;
    DominantVolume dominantCubeLut;
    DominantVolume dominantBloomLensDirt;
    DominantVolume dominantFog;
    DominantVolume dominantShadow;
    for (const PostProcessExtractedVolume& v : volumes)
    {
        if (layerMask && (v.LayerMask & *layerMask) == 0u)
            continue;
        float spatialWeight = 1.0f;
        const bool spatialContributes = ComputeSpatialWeight(v, cameraPos, hasCameraPos, spatialWeight);
        const float weight = v.BaseWeight * spatialWeight;
        if (spatialContributes)
        {
            stack.Settings = BlendPostProcessSettings(stack.Settings, v.Settings, weight, SpatialBlendGroups(v));
            stack.Any = true;
            dominant.Consider(v, weight);
            if (v.HasCubeLutAsset && v.Settings.LutIntensity > kDominantWeightEpsilon)
                dominantCubeLut.Consider(v, weight);
            if (v.HasBloomLensDirtAsset && v.Settings.BloomLensDirtEnabled != 0 &&
                v.Settings.BloomLensDirtIntensity > kDominantWeightEpsilon)
                dominantBloomLensDirt.Consider(v, weight);
            if (v.HasShadowSettings)
                dominantShadow.Consider(v, weight);
        }

        float fogWeight = spatialContributes ? weight : 0.0f;
        if (v.HasVolumetricFog && !spatialContributes && !v.IsGlobal && v.ValidSpatial)
        {
            fogWeight = v.BaseWeight;
            stack.Settings = BlendOnlyVolumetricFog(stack.Settings, v.Settings, fogWeight);
            stack.Any = true;
        }
        if (v.HasHeightFog && spatialContributes)
        {
            stack.Settings = BlendOnlyHeightFog(stack.Settings, v.Settings, weight, !v.HasVolumetricFog);
            stack.Any = true;
        }
        if (v.HasVolumetricFog && !v.IsGlobal && v.ValidSpatial && v.LocalFogVolume.enabled &&
            stack.FogVolumes.size() < kMaxVolumetricFogLocalVolumes)
        {
            stack.FogVolumes.push_back(v.LocalFogVolume);
        }
        if (v.HasVolumetricFog)
            dominantFog.Consider(v, fogWeight);
    }

    PostProcessSettings& resolved = stack.Settings;
    if (stack.Any && dominant.Volume)
    {
        // Stack order is discrete; use the dominant active volume to avoid
        // threshold-driven order popping while crossing blend boundaries.
        resolved.ColorFilterStackOrder = dominant.Volume->Settings.ColorFilterStackOrder;
        resolved.CasStackOrder = dominant.Volume->Settings.CasStackOrder;
        if (dominantCubeLut.Volume)
        {
            resolved.LutStackOrder = dominantCubeLut.Volume->Settings.LutStackOrder;
            resolved.LutInputEncoding = dominantCubeLut.Volume->Settings.LutInputEncoding;
            resolved.LutTextureFormat = dominantCubeLut.Volume->Settings.LutTextureFormat;
            const auto& gd = dominantCubeLut.Volume->CubeLutAssetGuid.GetData();
            std::memcpy(resolved.LutAssetGuidWords, gd.data(), sizeof(resolved.LutAssetGuidWords));
        }
        else
        {
            resolved.LutStackOrder = dominant.Volume->Settings.LutStackOrder;
            resolved.LutInputEncoding = dominant.Volume->Settings.LutInputEncoding;
            resolved.LutTextureFormat = dominant.Volume->Settings.LutTextureFormat;
            resolved.LutAssetGuidWords[0] = 0;
            resolved.LutAssetGuidWords[1] = 0;
        }
    }
    if (dominantBloomLensDirt.Volume)
    {
        const auto& gd = dominantBloomLensDirt.Volume->BloomLensDirtAssetGuid.GetData();
        std::memcpy(resolved.BloomLensDirtAssetGuidWords, gd.data(),
                    sizeof(resolved.BloomLensDirtAssetGuidWords));
    }
    else
    {
        resolved.BloomLensDirtAssetGuidWords[0] = 0;
        resolved.BloomLensDirtAssetGuidWords[1] = 0;
    }
    if (dominantFog.Volume)
        CopyFogVolumeBounds(resolved, *dominantFog.Volume);
    stack.ShadowVolume = dominantShadow.Volume;
    return stack;
}
} // namespace

void RenderExtractionSystem::ExtractPostProcessVolumes(ECS::World& world, uint64 worldId, RenderServices* rs)
{
    m_Volumes.clear();
    const WorldTimeOfDayState worldTimeOfDay = FindWorldTimeOfDayState(world);
    // Overwrite a view's exposure with its camera's (the camera owns the sensor; volumes contribute
    // only grading). Mirrors the volume exposure resolve but sourced from the registered camera.
    auto applyCameraExposure = [](PostProcessSettings& s, const ViewRegistry::CameraExposure& ce)
    {
        const auto mode = static_cast<Components::ExposureMode>(ce.Mode);
        const float ev100 = Rendering::ExposureEv100ForMode(mode, ce.ManualExposureEV,
                                                            ce.Aperture, ce.ShutterTime, ce.Iso);
        s.Exposure = Rendering::ResolveExposureScale(mode, ce.Exposure, ev100, ce.ExposureCompensation);
        if (mode == Components::ExposureMode::Auto)
        {
            s.AutoExposureActive    = true;
            s.AutoExposureMinEv     = ce.AutoExposureMinEv;
            s.AutoExposureMaxEv     = ce.AutoExposureMaxEv;
            s.AutoExposureSpeedUp   = ce.AutoExposureSpeedUp;
            s.AutoExposureSpeedDown = ce.AutoExposureSpeedDown;
            // All EV trims ride the bias, applied post-clamp (TryWriteField), so compensation
            // stays effective even when adaptation is pinned at the MinEv/MaxEv envelope edge.
            // The default bias is the engine's display-brightness anchor for Auto metering
            // (see kDefaultAutoExposureBiasEv) — Manual/Physical/Fixed take no default trim.
            s.AutoExposureBiasEv    = kDefaultAutoExposureBiasEv + ce.ExposureCompensation;
        }
        else
        {
            s.AutoExposureActive = false;
        }
        // The lens: physical DoF reads the camera's focus plane, derived focal
        // length, and the same f-number that drives physical exposure.
        s.DofFocusDistance = std::max(ce.FocusDistance, Components::Camera::kFocusDistanceMin);
        s.DofFocalLengthMm = std::max(ce.FocalLengthMm, 1.0f);
        s.DofAperture = std::max(ce.Aperture, Components::Camera::kApertureMin);
        s.DofSensorHeightMm = std::max(ce.SensorHeightMm, 1.0f);
        s.DofApertureBladeCount = std::clamp(
            ce.ApertureBladeCount,
            static_cast<int32>(Components::Camera::kApertureBladeCountMin),
            static_cast<int32>(Components::Camera::kApertureBladeCountMax));
        s.DofApertureRoundness = std::clamp(ce.ApertureRoundness, 0.0f, 1.0f);
        s.DofApertureRotation = std::clamp(ce.ApertureRotation, 0.0f, 360.0f);
        s.DofAnamorphicSqueeze = std::clamp(ce.AnamorphicSqueeze, 1.0f, 4.0f);
        s.DofDebugMode = ce.FocusDebugMode != 0 ? 1 : 0;
        s.DofDebugAlpha = std::clamp(ce.FocusDebugAlpha, 0.0f, 1.0f);
        // Volume exposure modifiers (ExposureAdjustmentEffect) ride the blended settings and
        // stack on the sensor resolve above: compensation in log space (added to the post-clamp
        // metering bias in Auto, like the camera's own compensation); the clamps only narrow
        // the adaptation envelope — a volume can tighten the camera's range, never widen it.
        if (s.AutoExposureActive)
        {
            Rendering::NarrowAutoExposureEnvelope(s.AutoExposureMinEv, s.AutoExposureMaxEv,
                                                  s.ExposureClampMinEv, s.ExposureClampMaxEv);
            s.AutoExposureBiasEv += s.ExposureCompensationEv;
        }
        else
        {
            s.Exposure *= std::exp2(s.ExposureCompensationEv);
        }
    };

    // Snapshot the effect registrations once per extraction (descriptor storage
    // is process-lifetime stable; ForEach allocates, the volume loop must not).
    std::vector<const GameEngine::Rendering::PostProcessEffectDescriptor*> effectHooks;
    GameEngine::Rendering::PostProcessEffectRegistry::ForEach(
        [&effectHooks](const GameEngine::Rendering::PostProcessEffectDescriptor& d)
        {
            if (d.Extract || d.NeutralizeSettings)
                effectHooks.push_back(&d);
        });
    PostProcessExtractContext extractCtx{};
    extractCtx.Services = rs;
    extractCtx.TimeOfDayHours = worldTimeOfDay.hours;
    std::memcpy(extractCtx.DayKeyTimesHours, worldTimeOfDay.dayKeyTimesHours,
                sizeof(extractCtx.DayKeyTimesHours));

    world.Query<
             GameEngine::ECS::Read<GameEngine::Components::PostProcessVolume>,
             GameEngine::ECS::Read<GameEngine::Components::WorldTransform>>()
        .Each([&](GameEngine::ECS::EntityHandle eHandle,
                  const GameEngine::Components::PostProcessVolume& vol,
                  const GameEngine::Components::WorldTransform& xf)
              {
                  PostProcessExtractedVolume ev{};
                  ev.Priority = vol.Priority;
                  ev.BaseWeight = vol.Weight;
                  ev.LayerMask = vol.PostProcessMask;
                  ev.IsGlobal = vol.IsGlobal;
                  ev.Shape = static_cast<int32>(vol.Shape);
                  ev.BlendDistance = vol.BlendDistance;
                  ev.Center[0] = xf.matrix[12];
                  ev.Center[1] = xf.matrix[13];
                  ev.Center[2] = xf.matrix[14];
                  // Exposure is not a volume property — it belongs to the camera (the sensor), applied
                  // per-view below, or to the world default for camera-less views. Volumes contribute
                  // only grading / tonemap / bloom / fog.
                  ev.Settings.TonemapMode    = static_cast<int32>(vol.Tonemap);
                  ev.Settings.DitherMode     = vol.DitherMode;
                  ev.Settings.IctcpChromaCompression =
                      std::clamp(vol.IctcpChromaCompression, 0.0f, 1.0f);
                  ev.ValidSpatial = true;

                  const float* mtx = xf.matrix;
                  const float ax[3] = {mtx[0], mtx[1], mtx[2]};
                  const float ay[3] = {mtx[4], mtx[5], mtx[6]};
                  const float az[3] = {mtx[8], mtx[9], mtx[10]};
                  const float sx = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                  const float sy = std::sqrt(ay[0] * ay[0] + ay[1] * ay[1] + ay[2] * ay[2]);
                  const float sz = std::sqrt(az[0] * az[0] + az[1] * az[1] + az[2] * az[2]);
                  if (sx <= 0.0f || sy <= 0.0f || sz <= 0.0f)
                  {
                      ev.ValidSpatial = false;
                  }
                  else
                  {
                      const float invSx = 1.0f / sx;
                      const float invSy = 1.0f / sy;
                      const float invSz = 1.0f / sz;

                      ev.AxisX[0] = ax[0] * invSx; ev.AxisX[1] = ax[1] * invSx; ev.AxisX[2] = ax[2] * invSx;
                      ev.AxisY[0] = ay[0] * invSy; ev.AxisY[1] = ay[1] * invSy; ev.AxisY[2] = ay[2] * invSy;
                      ev.AxisZ[0] = az[0] * invSz; ev.AxisZ[1] = az[1] * invSz; ev.AxisZ[2] = az[2] * invSz;
                      ev.HalfExtents[0] = sx * 0.5f;
                      ev.HalfExtents[1] = sy * 0.5f;
                      ev.HalfExtents[2] = sz * 0.5f;
                  }

                  // Per-effect folds live on the effect registrations
                  // (PostProcessEffectDescriptors.cpp). NeutralizeSettings runs
                  // for every volume (baselines effects whose struct defaults
                  // are not authored-off); Extract folds the component when
                  // present + enabled. Hook order = registration order — the
                  // shared fog-glow merge depends on it.
                  for (const auto* effectDesc : effectHooks)
                  {
                      if (effectDesc->NeutralizeSettings)
                          effectDesc->NeutralizeSettings(ev.Settings);
                      if (effectDesc->Extract)
                          effectDesc->Extract(world, eHandle, extractCtx, ev);
                  }

                  m_Volumes.push_back(ev); });

    if (m_Volumes.empty())
    {
        // No active volumes: ensure defaults are applied (clears any stale settings from prior frames).
        // Still stamp the default Auto sensor so camera-less views (the editor Scene View) auto-meter
        // even with zero volumes, matching the resolved-volumes path below.
        PostProcessSettings worldSettings{};
        applyCameraExposure(worldSettings, ViewRegistry::CameraExposure{});
        rs->SetWorldPostProcessSettings(worldId, worldSettings);
        rs->SetWorldVolumetricFogVolumes(worldId, {});
    }
    else
    {
        std::stable_sort(m_Volumes.begin(), m_Volumes.end(),
                         [](const PostProcessExtractedVolume& a, const PostProcessExtractedVolume& b)
                         { return a.Priority < b.Priority; });

        bool hasWorldCameraPos = false;
        float worldCameraPos[3] = {0.0f, 0.0f, 0.0f};
        const Rendering::CameraData* worldCamData = nullptr;
        const auto& views = rs->Views().GetViews();
        for (const auto& view : views)
        {
            if (view.worldId != 0 && view.worldId != worldId)
                continue;
            const auto* cam = rs->Views().FindCameraData(view.cameraId);
            if (!cam)
                continue;
            worldCameraPos[0] = cam->cameraPos[0];
            worldCameraPos[1] = cam->cameraPos[1];
            worldCameraPos[2] = cam->cameraPos[2];
            worldCamData = cam;
            hasWorldCameraPos = true;
            break;
        }

        ResolvedVolumeStack worldStack =
            ResolveVolumeStack(m_Volumes, worldCameraPos, hasWorldCameraPos, std::nullopt);
        // Directional shadows are world-global, so the dominant spatially
        // contributing volume carrying a ShadowSettingsEffect wins.
        if (worldStack.ShadowVolume)
            rs->SetWorldShadowSettings(worldId, worldStack.ShadowVolume->ShadowSettings);
        // Exposure is not a volume property; camera-less views (the editor Scene View, which re-derives
        // from the world settings) expose from the world default. Stamp the default Auto sensor onto
        // the blended world settings so a scene with post-processing auto-meters even though no volume
        // carries exposure. Camera-driven views override this per-view from their own sensor below.
        PostProcessSettings worldSettings = worldStack.Any ? worldStack.Settings : PostProcessSettings{};
        applyCameraExposure(worldSettings, ViewRegistry::CameraExposure{});
        rs->SetWorldPostProcessSettings(worldId, worldSettings);
        rs->SetWorldVolumetricFogVolumes(worldId, std::move(worldStack.FogVolumes));
    }

    // Per-view filtering: views whose camera registered a PostProcess mask via
    // CameraSystem get a filtered blend that only includes volumes whose
    // PostProcessMask intersects the camera's mask. Views without a registered
    // mask (e.g. the editor Scene View, which manages its own override) are
    // left untouched and fall back to the world-level settings.
    {
        const auto& views = rs->Views().GetViews();
        for (const auto& view : views)
        {
            if (view.worldId != 0 && view.worldId != worldId)
                continue;
            if (view.cameraId == 0)
                continue;
            if (!rs->Views().HasCameraPostProcessMask(view.cameraId))
                continue;

            const uint32 cameraMask = rs->Views().GetCameraPostProcessMask(view.cameraId);
            const auto* cam = rs->Views().FindCameraData(view.cameraId);
            const bool hasViewCameraPos = (cam != nullptr);
            float viewCameraPos[3] = {0.0f, 0.0f, 0.0f};
            if (cam)
            {
                viewCameraPos[0] = cam->cameraPos[0];
                viewCameraPos[1] = cam->cameraPos[1];
                viewCameraPos[2] = cam->cameraPos[2];
            }

            ResolvedVolumeStack viewStack =
                ResolveVolumeStack(m_Volumes, viewCameraPos, hasViewCameraPos, cameraMask);
            PostProcessSettings perView = viewStack.Settings;
            const bool any = viewStack.Any;

            // The camera owns its exposure: if this view's camera registered one, it sets the view's
            // exposure (volumes no longer carry exposure). With no contributing volume we still publish
            // an override so the camera exposure applies, starting from the world grade so global
            // post-fx is kept.
            const ViewRegistry::CameraExposure* camExp = rs->Views().FindCameraExposure(view.cameraId);
            if (camExp)
            {
                if (!any)
                {
                    perView = rs->GetWorldPostProcessSettings(worldId);
                    // The world grade stays as the baseline, but exposure modifiers are
                    // per-volume contributions: a camera whose mask matched no volume must
                    // not inherit them from the mask-ignoring world blend.
                    perView.ExposureCompensationEv = 0.0f;
                    perView.ExposureClampMinEv = kExposureClampNoOpMinEv;
                    perView.ExposureClampMaxEv = kExposureClampNoOpMaxEv;
                }
                applyCameraExposure(perView, *camExp);
            }
            else if (any)
            {
                // Masked but camera-less view (e.g. the bookmark preview): the contributing volume
                // carries only grading, so stamp the world-default Auto sensor instead of leaving the
                // static Exposure=1.0 default, which would blow the preview out under physical lighting.
                applyCameraExposure(perView, ViewRegistry::CameraExposure{});
            }

            if (any)
            {
                rs->Views().SetViewPostProcessOverride(view.id, perView);
                rs->Views().SetViewVolumetricFogVolumesOverride(view.id, std::move(viewStack.FogVolumes));
            }
            else if (camExp)
            {
                rs->Views().SetViewPostProcessOverride(view.id, perView);
                rs->Views().ClearViewVolumetricFogVolumesOverride(view.id);
            }
            else
            {
                rs->Views().ClearViewPostProcessOverride(view.id);
                rs->Views().ClearViewVolumetricFogVolumesOverride(view.id);
            }
        }
    }
}


bool RenderExtractionSystem::EvaluateFastPathPreconditions(ECS::World& world, uint64 worldId,
                                                           uint32& reasons)
{
    // E3 — consumed first, once per run, on every path (compare-then-assign:
    // a skipped consume accrues a false gap that buys a spurious full frame).
    if (m_FeedGuard.ConsumeAndCheckMissed(worldId,
                                          world.GetComponentDirtyFeed().SwapGeneration()))
        reasons |= kEscalationMissedWindow;

    // E1 — per-world subscription (thumbnail/preview worlds are deliberately
    // unsubscribed at bootstrap; their extraction instances stay full-lane).
    if (!world.IsComponentDirtyFeedEnabledFor(
            ECS::GetComponentTypeId<Components::WorldTransform>()))
        reasons |= kEscalationFeedDisabled;

    // E2 — either readable buffer dropped appends: Snapshot() is incomplete.
    if (world.GetComponentDirtyFeed().Overflowed())
        reasons |= kEscalationFeedOverflow;

    // E4 — structural version vs the sample taken at the END of the last
    // full pass (see the prime block in Update for the TLAS asymmetry note).
    if (world.GetStructuralChangeVersion() != m_FullPassStructuralVersion)
        reasons |= kEscalationStructuralChange;

    // E5 — Changed<> probes over the four record columns extraction reads
    // and never writes, morph carriers included (their MeshRenderer/
    // LocalBounds edits change submission fields the replay depends on).
    // Evaluated every frame on both lanes so the gates advance and edits are
    // consumed, not latched stale (D3).
    const uint64 probeVersion = world.GetGlobalSystemVersion();
    if (ProbeColumnChanged<Components::MeshRenderer>(world, m_ProbeGateMeshRenderer, probeVersion))
        reasons |= kEscalationProbeMeshRenderer;
    if (ProbeColumnChanged<Components::LocalBounds>(world, m_ProbeGateLocalBounds, probeVersion))
        reasons |= kEscalationProbeLocalBounds;
    if (ProbeColumnChanged<Components::WorldSectorCoord>(world, m_ProbeGateSectorCoord, probeVersion))
        reasons |= kEscalationProbeSectorCoord;
    if (ProbeColumnChanged<Components::LODGroup>(world, m_ProbeGateLodGroup, probeVersion))
        reasons |= kEscalationProbeLodGroup;

    // E6 — mesh registry reload/unregister event since the last consume.
    if (m_MeshReloadPending.exchange(false, std::memory_order_relaxed))
        reasons |= kEscalationMeshReload;

    // HLOD cluster flip: a member/proxy residency change was requested. Eviction
    // frees slots directly, but re-adding the winning set is a full-lane Op::Add,
    // so this frame must run the full lane. The request is routed through
    // RenderServices so HLODSelectSystem and this system stay decoupled.
    if (m_RenderServices && m_RenderServices->ConsumeHlodResidencyExtractionPending())
        reasons |= kEscalationHlodResidency;

    // E7 — material dirtiness digest (D-MAT). Entry-computed; the value is
    // stored as the new compare base only at full-lane end, so a mid-frame
    // registration escalates the NEXT frame instead of being swallowed.
    m_FrameMaterialDigest = ComputeMaterialDigest();
    if (m_FrameMaterialDigest != m_LastMaterialDigest)
        reasons |= kEscalationMaterialDigest;

    // E8 — records the last full pass skipped as transiently unrenderable
    // must be retried until they render. Load-bearing for correctness, not
    // just streaming latency: the sentinel->valid mesh re-register shape is
    // covered ONLY by this counter (correctness F2).
    if (m_LastPendingTransient != 0)
        reasons |= kEscalationPendingTransient;

    // E9 — ordered view-set fingerprint.
    m_FrameViewFingerprints = ComputeViewFingerprints(worldId);
    if (m_FrameViewFingerprints.All != m_LastViewFingerprint)
        reasons |= kEscalationViewSet;

    // E10 — the always-refresh subset's ceiling. Motion alone never escalates:
    // the patch lane is O(dirty) and parallel at any mover count; a feed that
    // overflowed its cap escalates through E2 above.
    if (m_AlwaysRefresh.size() > m_MaxAlwaysRefresh)
        reasons |= kEscalationSubsetOverBudget;

    // E11 — defensive: never primed, or last primed against a different
    // world (instance reuse across worlds).
    if (!m_CachesPrimed || m_CachesWorldId != worldId)
        reasons |= kEscalationCachesUnprimed;

#if GE_DEBUG_INSTRUMENTATION
    if (m_ValidatorForceFull)
    {
        m_ValidatorForceFull = false;
        reasons |= kEscalationValidator;
    }
#endif

    return reasons == 0;
}

uint64 RenderExtractionSystem::ComputeMaterialDigest() const
{
    // E7 primary mechanism (D-MAT): ordered hash of the depth-class authority
    // array + the SSBO index-mapping generation. Robust to future writers by
    // construction — every register/re-register refreshes the array, every
    // index assignment/free bumps the generation. ~10 KB/frame at the
    // material cap, microsecond-class.
    //
    // Cached-submission Material* lifetime rides on this digest (D4 rider):
    // Unregister always fires the PreUnregister callback -> MaterialSystem
    // frees the index and bumps the generation -> this digest moves ->
    // escalation precedes any replay dereference. The chain relies on the
    // registry's documented single-threaded contract (MaterialRegistry.h);
    // in-tree, Unregister has no production callers (tests only — verified at
    // S2b). Accepted divergence: RecompileMaterialPipeline mutates pipeline
    // validity outside all bump sites — benign for draws (WorldDrawBuilder
    // drops invalid-pipeline submissions, so replay converges to the same
    // draw set), but a full-vs-fast submission-membership divergence during
    // failed-recompile windows; bump-on-recompile is the cheap upgrade if
    // soak ever shows it mattering.
    using GameEngine::Rendering::HashUtils::Fnv1a64;
    using GameEngine::Rendering::HashUtils::HashValue;
    constexpr uint64 kFnvOffsetBasis = 14695981039346656037ull;
    auto& materials = m_RenderServices->Materials();
    uint64 digest = HashValue(kFnvOffsetBasis, materials.MaterialSSBOGeneration());
    const auto depthClasses = materials.MaterialDepthClassSpan();
    digest = HashValue(digest, static_cast<uint64>(depthClasses.size()));
    if (!depthClasses.empty())
        digest = Fnv1a64(depthClasses.data(), depthClasses.size(), digest);
    return digest;
}

RenderExtractionSystem::ViewFingerprints
RenderExtractionSystem::ComputeViewFingerprints(uint64 worldId) const
{
    // E9: ORDERED hash (sequence-sensitive, api-perf F11) — a pure view
    // reorder must escalate, because the replayed submission vector would
    // diverge byte-wise from a fresh walk even though draws are unaffected
    // (WorldDrawBuilder re-sorts). Camera movement does not invalidate —
    // submissions carry no matrices. OnDemand views (probe rebakes, ocean
    // mirror) flip ActiveRenderLayerMask() with participationFrames, so
    // activation AND expiry each cost one full-lane frame; both are
    // semantically required (submissions must appear/disappear for that
    // view), and E9's dedicated telemetry bit keeps soak analysis from
    // misreading the churn as an escalation storm. The persistent hash leaves
    // the OnDemand views out, so that churn is distinguishable from a change
    // to what the Always views draw (IsOnDemandViewChurnOnly).
    using GameEngine::Rendering::HashUtils::HashValue;
    constexpr uint64 kFnvOffsetBasis = 14695981039346656037ull;
    ViewFingerprints fingerprints{kFnvOffsetBasis, kFnvOffsetBasis};
    uint32 matchingCount = 0;
    uint32 persistentCount = 0;
    for (const auto& view : m_RenderServices->Views().GetViews())
    {
        if (view.worldId != 0 && view.worldId != worldId)
            continue;
        fingerprints.All = HashValue(fingerprints.All, view.id);
        fingerprints.All = HashValue(fingerprints.All, view.worldId);
        fingerprints.All = HashValue(fingerprints.All, view.ActiveRenderLayerMask());
        ++matchingCount;
        if (view.participation == Rendering::ViewParticipation::OnDemand)
            continue;
        fingerprints.Persistent = HashValue(fingerprints.Persistent, view.id);
        fingerprints.Persistent = HashValue(fingerprints.Persistent, view.worldId);
        fingerprints.Persistent = HashValue(fingerprints.Persistent, view.ActiveRenderLayerMask());
        ++persistentCount;
    }
    fingerprints.All = HashValue(fingerprints.All, matchingCount);
    fingerprints.Persistent = HashValue(fingerprints.Persistent, persistentCount);
    return fingerprints;
}

bool RenderExtractionSystem::IsOnDemandViewChurnOnly(const ECS::World& world, uint32 reasons) const
{
    // Every other trigger can change an instance record or a persistent
    // view's submissions. With E9 alone, the Always views unchanged, nothing in
    // the dirty feed and no always-refresh carrier, the full lane rebuilds the
    // same records the fast path would have replayed (lane parity) and only
    // the OnDemand view's own submissions appear or disappear. Bumping the
    // content versions there would revoke the TAA stationary certification
    // and the shadow caches on every probe capture of a static scene.
    return reasons == kEscalationViewSet &&
           m_FrameViewFingerprints.Persistent == m_LastPersistentViewFingerprint &&
           world.GetComponentDirtyFeed().SnapshotSize() == 0 && m_AlwaysRefresh.empty();
}

void RenderExtractionSystem::TrackChangedCasterSphere(const Rendering::GPUInstance& previous,
                                                      const Rendering::GPUInstance& current)
{
    // bit 0 == castsShadows (ComputeInstanceFlags). Shadow-flag edits escalate to
    // the full lane (E5), so previous/current agree on fast frames — a non-caster
    // change cannot alter any light's rendered shadow depth and is not tracked
    // (its bump attribution is simply absent, which reads as "no light affected").
    if ((current.flags & 1u) == 0u)
        return;
    if (m_ChangedCasterOverflow)
        return;
    if (m_ChangedCasterSpheres.size() >= kMaxChangedCasterSpheres)
    {
        // Once per overflow episode: without this line, the perf cliff at the
        // N+1th mover (every point light re-rendering every frame) is silent
        // and indistinguishable from a genuine bulk change.
        if (!m_OverflowLogLatched)
        {
            m_OverflowLogLatched = true;
            Logger::Log::Info("[PointShadow] caster attribution overflow (>{} changed casters this "
                              "frame) — shadow invalidation globalized",
                              kMaxChangedCasterSpheres);
        }
        m_ChangedCasterSpheres.clear();
        m_ChangedCasterOverflow = true;
        return;
    }
    // Sphere enclosing old ∪ new world bounds: midpoint + half-distance + the
    // larger radius. Slightly conservative vs the exact enclosing sphere — over-
    // inclusion costs at most an extra re-render, never leaves a stale shadow.
    // Enclosing BOTH poses is what makes range-boundary crossings sound in both
    // directions (a caster leaving a light's range still triggers the one final
    // re-render that removes its shadow).
    const float dx = current.boundingCenter.x - previous.boundingCenter.x;
    const float dy = current.boundingCenter.y - previous.boundingCenter.y;
    const float dz = current.boundingCenter.z - previous.boundingCenter.z;
    const float halfDist = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    ShadowCasterChangeSphere s{};
    s.X = 0.5f * (previous.boundingCenter.x + current.boundingCenter.x);
    s.Y = 0.5f * (previous.boundingCenter.y + current.boundingCenter.y);
    s.Z = 0.5f * (previous.boundingCenter.z + current.boundingCenter.z);
    s.Radius = halfDist + std::max(previous.boundingRadius, current.boundingRadius);
    m_ChangedCasterSpheres.push_back(s);
}

uint32 RenderExtractionSystem::PatchDirtyInstances(ECS::World& world, GPUScene& scene)
{
    // Dedupe complete handles before reading components: both feed windows may
    // contain the same mover, and applying a duplicate would settle its fresh
    // motion vector within the same frame. The ordering pass also gives the
    // lane its entity order (row writes, caster attribution and movers are
    // reproducible whatever order the producers appended in), in O(n) however
    // scattered the producers' order is.
    world.GetComponentDirtyFeed().Snapshot(m_FeedScratch);
    m_ParallelState->feedOrder.OrderUnique(m_FeedScratch, world.GetEntityIndexBound());
    if (!m_AlwaysRefreshSet.empty())
        std::erase_if(m_FeedScratch, [&](ECS::EntityHandle e)
                      { return m_AlwaysRefreshSet.contains(e.id); });
    if (m_FeedScratch.empty())
        return 0;

    auto& reads = m_ParallelState->dirtyReads;
    reads.resize(m_FeedScratch.size());
    JobSystem::WorkStealingThreadPool* js = world.GetJobSystem();
    ForkJoinRanges(js, m_FeedScratch.size(), [&](std::size_t begin, std::size_t end)
                   { ResolveDirtyReads(world, m_FeedScratch, reads, begin, end); });

    // Compact the actual writes before acquiring a grant. Stale handles, feed
    // re-deliveries and static neighbors never stamp MeshGPUData. Complete
    // handles (including generation) were resolved by World, so a recycled
    // entity index cannot substitute a different entity's cached GPU slot.
    std::size_t writeCount = 0;
    for (std::size_t i = 0; i < reads.size(); ++i)
    {
        const auto* wt = std::get<0>(reads[i]);
        if (!wt)
            continue; // died mid-window
        // Const read first: re-delivered duplicates and same-version entries
        // must stay grant-free (reads never stamp).
        const auto* mgRead = std::get<1>(reads[i]);
        if (!mgRead || mgRead->instanceIndex == 0xFFFFFFFFu ||
            mgRead->meshIndex == 0xFFFFFFFFu)
            continue; // no slot yet / tombstoned / released: creation,
                      // readiness and disable transitions are escalation
                      // triggers (E4/E5/E8), never patched here
        if (mgRead->LastTransformVersion == wt->Version &&
            !(m_AllowPrevSettle && mgRead->LastPrevDiffers))
            continue; // duplicate / re-delivery / multi-step window: no-op.
                      // == on purpose, not <: an undo byte-restore brings an
                      // OLDER Version back and must repaint (T9). A set
                      // LastPrevDiffers latch overrides the no-op on the first
                      // run of a NEW window: the feed re-delivers a write for
                      // one extra window, and that re-delivery is the settle
                      // rebuild that advances a stopped mover's prevTransform
                      // to equal its transform.
        m_FeedScratch[writeCount] = m_FeedScratch[i];
        reads[writeCount] = reads[i];
        ++writeCount;
    }
    m_FeedScratch.resize(writeCount);
    reads.resize(writeCount);
    // This is the same write-grant contract as GetComponentForWrite, batched
    // once for the selected entities. The owning World is mutable and the fast
    // lane performs no structural changes; only these granted pointers are cast.
    world.StampComponentWriteBatch(m_FeedScratch.data(), writeCount,
                                   ECS::GetComponentTypeId<Components::MeshGPUData>());

    // Build every write's row on the world's pool (inline when it has none),
    // then attribute changed casters against the rows still holding the
    // previous pose, then write the rows, then publish the movers. Each step
    // keeps the entity-index order, so the rows, the caster spheres and the
    // mover list match a per-entity loop's exactly, and the dirty words match
    // it as a set (the flush sorts their list).
    auto& writes = m_ParallelState->patchWrites;
    auto& isMover = m_ParallelState->patchIsMover;
    auto& moverRecords = m_ParallelState->patchMoverRecords;
    writes.resize(writeCount);
    isMover.resize(writeCount);
    moverRecords.resize(writeCount);
    ForkJoinRanges(js, writeCount, [&](std::size_t begin, std::size_t end)
                   { BuildPatchedInstances(reads, scene, writes, isMover, moverRecords, begin, end); });
    for (std::size_t i = 0; i < writeCount && !m_ChangedCasterOverflow; ++i)
        TrackChangedCasterSphere(scene.GetInstances()[writes[i].InstanceIndex], writes[i].Instance);
    scene.UpdateInstances(writes, [js](std::size_t count,
                                       const std::function<void(std::size_t, std::size_t)>& body)
                          { ForkJoinRanges(js, count, body); });

    auto& movers = m_ParallelState->patchMovers;
    movers.clear();
    movers.reserve(writeCount);
    for (std::size_t i = 0; i < writeCount; ++i)
        if (isMover[i] != 0u)
            movers.push_back(moverRecords[i]);
    m_RenderServices->AddFrameMovers(movers);
    return static_cast<uint32>(writeCount);
}

void RenderExtractionSystem::RebuildAlwaysRefreshSubset(ECS::World& world)
{
    // D5: entities with record inputs that are legitimately per-frame
    // volatile through channels no probe covers (SkinningUpload's palette
    // offsets, AnimatorRef playback state, SkeletonStore runtime state) get
    // a full per-entity re-prepare every fast frame instead. Rebuilt on
    // every full pass; membership changes are component add/removes and
    // Disabled-tag flips — all structural (E4) — so the set cannot go stale
    // between full passes. Require an extraction bridge: animation controllers
    // and skeleton-only entities cannot update GPU instances and must not spend
    // the subset budget or pin an otherwise static scene onto the full lane.
    // Adding their rendering components escalates via E4 and rebuilds this set.
    // Disabled entities are excluded from re-prepare by the query engine.
    m_AlwaysRefresh.clear();
    m_AlwaysRefreshSet.clear();
    const auto collect = [&](ECS::EntityHandle e)
    {
        if (m_AlwaysRefreshSet.insert(e.id).second)
            m_AlwaysRefresh.push_back(e);
    };
    world.Query<ECS::Read<Components::SkinnedMeshRenderer>>()
        .With<Components::WorldTransform, Components::MeshRenderer, Components::MeshGPUData>()
        .Each([&](ECS::EntityHandle e, const Components::SkinnedMeshRenderer&) { collect(e); });
    world.Query<ECS::Read<Components::SkeletonRef>>()
        .With<Components::WorldTransform, Components::MeshRenderer, Components::MeshGPUData>()
        .Each([&](ECS::EntityHandle e, const Components::SkeletonRef&) { collect(e); });
    world.Query<ECS::Read<Components::AnimatorRef>>()
        .With<Components::WorldTransform, Components::MeshRenderer, Components::MeshGPUData>()
        .Each([&](ECS::EntityHandle e, const Components::AnimatorRef&) { collect(e); });
    world.Query<ECS::Read<Components::MorphTargetWeights>>()
        .With<Components::WorldTransform, Components::MeshRenderer, Components::MeshGPUData>()
        .Each([&](ECS::EntityHandle e, const Components::MorphTargetWeights&) { collect(e); });
}

uint32 RenderExtractionSystem::RefreshAlwaysRefreshSubset(ECS::World& world, GPUScene& scene,
                                                          MeshGPURegistry& meshReg,
                                                          MaterialRegistry& matReg,
                                                          uint64 worldId)
{
    uint32 refreshed = 0;
    if (m_AlwaysRefresh.empty())
        return refreshed;
    auto& reads = m_ParallelState->refreshReads;
    reads.resize(m_AlwaysRefresh.size());
    world.GetComponentsBatch<Components::WorldTransform, Components::MeshGPUData,
                             Components::MeshRenderer, Components::GIEmitter,
                             Components::LocalBounds, Components::SkinnedMeshRenderer,
                             Components::SkeletonRef, Components::AnimatorRef,
                             Components::LODGroup, ECS::ComponentDisabled<Components::MeshRenderer>,
                             ECS::ComponentDisabled<Components::SkinnedMeshRenderer>>(
        std::span{m_AlwaysRefresh}, std::span{reads});
    for (std::size_t i = 0; i < m_AlwaysRefresh.size(); ++i)
    {
        // Full per-entity re-prepare from FRESH reads of every record input —
        // the per-frame-volatile inputs no probe covers (a palette offset
        // reassigned by SkinningUpload, AnimatorRef playback state) are
        // picked up here, same frame, same as today's gather latency.
        const ECS::EntityHandle e = m_AlwaysRefresh[i];
        const auto& [wt, mgRead, mr, giEmitterTag, lb, smr, skelRef, anim, lodGroup, rendererOff, skinnedOff] =
            reads[i];
        if (!wt || !mgRead || mgRead->instanceIndex == 0xFFFFFFFFu)
            continue; // never extracted / released: creation is the full lane's job (E4)
        // Tag component: presence is the signal (Components::GIEmitter).
        const bool giEmitter = giEmitterTag != nullptr;
        if (!mr || rendererOff)
        {
            // Disable transitions escalate (tag adds and component removals
            // are structural, E4), so CLEANUP tombstones on the escalated
            // frame and this branch's steady state is the already-tombstoned
            // skip below. The
            // tombstone mirror is kept as the CLEANUP-equivalent guard: it
            // is what stops the re-prepare from repainting a slot whose
            // entity is not renderable if a probe regression ever lets a
            // disable through (the T15 tripwire's failure mode).
            if (mgRead->meshIndex != 0xFFFFFFFFu)
            {
                if (auto* meshGpu = world.GetComponentForWrite<Components::MeshGPUData>(e))
                    ClearMeshGpuInstance(scene, *meshGpu);
            }
            continue;
        }

        MeshGPUHandle meshHandle(mr->meshGpuHandleId);
        if (!meshHandle.IsValid())
            continue; // unrenderable; a later assignment is an E5/E6-covered transition
        const auto* meshEntry = meshReg.Find(meshHandle);
        if (!meshEntry)
            continue; // registry transitions fire the E6 reload/unregister event
        const Material* material = nullptr;
        {
            const GameEngine::GUID matGuid = mr->materialAssetGuid.ToGuid();
            if (!matGuid.IsNull())
                material = matReg.Find(matGuid);
        }
        if (!material || !material->GetGraphicsPipelineId().IsValid())
            continue; // register/recompile transitions move the E7 digest
        const uint32 mshIdx = meshEntry->gpuMeshIndex;
        const uint32 matIdx = material->GetGpuSceneMaterialIndex();
        if (mshIdx == 0xFFFFFFFFu || matIdx == 0xFFFFFFFFu)
            continue; // sentinel: the E6 event / E7 digest cover the transition

        const bool hasBounds = lb != nullptr;
        const uint32 skeletonId =
            (smr && !skinnedOff && smr->skeletonId != 0) ? smr->skeletonId : 0u;
        const uint32 runtimeId = (skelRef && skelRef->runtimeId != 0) ? skelRef->runtimeId : 0u;
        const bool hasActiveAnimation = anim && anim->ClipIndex != 0;
        const float lodBias = lodGroup ? lodGroup->Bias : 0.0f;

        const bool castShadows = mr->castShadows && (!hasBounds || lb->CastShadows);
        const uint32 flags =
            ComputeInstanceFlags(castShadows, mr->receiveShadows,
                                 m_RenderServices->Materials().GetMaterialDepthClass(matIdx),
                                 IsOrderDependentBlendMaterial(*material), giEmitter, worldId);

        const SkinPaletteOffsets paletteOffsets =
            ResolveSkinPaletteOffsets(skeletonId, hasActiveAnimation, runtimeId);
        const uint32 skinPaletteOffset = paletteOffsets.Current;

        const float boundsRadius = hasBounds ? lb->Box.Radius() : -1.0f;
        // Same split as PrepareRecord: the non-transform inputs decide
        // continuity, the transform version only decides the rebuild.
        const bool nonTransformInputsUnchanged =
            mgRead->meshIndex == mshIdx &&
            mgRead->materialIndex == matIdx &&
            mgRead->LastMeshUploadSequence == meshEntry->uploadSeq &&
            mgRead->LastFlags == flags &&
            mgRead->LastSkinPaletteOffset == skinPaletteOffset &&
            mgRead->LastLodBias == lodBias &&
            mgRead->LastRuntimeId == runtimeId &&
            mgRead->LastBoundsRadius == boundsRadius &&
            (!hasBounds ||
             (mgRead->LastBoundsCenter[0] == lb->Box.center.x &&
              mgRead->LastBoundsCenter[1] == lb->Box.center.y &&
              mgRead->LastBoundsCenter[2] == lb->Box.center.z));
        const bool instanceUnchanged =
            nonTransformInputsUnchanged &&
            // Stopped mover: one settle rebuild (prev := current), first run
            // of a new feed window only (see m_AllowPrevSettle).
            !(m_AllowPrevSettle && mgRead->LastPrevDiffers) &&
            mgRead->LastTransformVersion == wt->Version;
        if (instanceUnchanged)
        {
            // Deforming in place: the payload is byte-identical (stable palette
            // offset, unchanged transform version) yet the pose moved. Qualify
            // it as a mover WITHOUT taking a write grant — a GetComponentForWrite
            // here would stamp the chunk and defeat the change-gate this loop
            // exists to satisfy.
            if (paletteOffsets.Active())
                m_RenderServices->AddFrameMover(mgRead->instanceIndex, meshHandle,
                                                paletteOffsets.Current, paletteOffsets.Previous);
            continue;
        }

        auto* meshGpu = world.GetComponentForWrite<Components::MeshGPUData>(e);
        if (!meshGpu)
            continue;
        const float boundsCenter[3] = {hasBounds ? lb->Box.center.x : 0.0f,
                                       hasBounds ? lb->Box.center.y : 0.0f,
                                       hasBounds ? lb->Box.center.z : 0.0f};
        GPUInstance payload;
        const GPUInstance& previousInstance = scene.GetInstances()[meshGpu->instanceIndex];
        const float* prevMatrix = previousInstance.transform.Data();
        BuildGpuInstancePayload(payload, wt->matrix, prevMatrix, hasBounds, boundsCenter,
                                boundsRadius, mshIdx, matIdx, flags, lodBias,
                                skinPaletteOffset, runtimeId,
                                meshGpu->LastSectorX, meshGpu->LastSectorY, meshGpu->LastSectorZ, mr->renderLayerMask);
        const bool prevDiffers = PayloadPrevDiffers(payload);
        // Before UpdateInstance overwrites the mirror `previousInstance` aliases.
        // A refreshed caster counts as changed whatever field moved — a palette
        // re-offset implies a new skinned pose, i.e. a new shadow silhouette.
        TrackChangedCasterSphere(previousInstance, payload);
        // Patch only — the fast path never allocates or frees GPUScene slots
        // (D4): tombstoned slots keep their instanceIndex, so a re-enable
        // repaints in place; brand-new entities arrive via the full lane.
        // GPUScene-only is sufficient here because the inputs that reach a
        // fast frame are the non-submission fields (skinPaletteOffset /
        // lodBias / runtimeId); submission-visible edits — material, mesh
        // handle, layer mask, shadow flags — stamp MeshRenderer/LocalBounds
        // and escalate via the E5 probes before any stale replay (D5).
        scene.UpdateInstance(meshGpu->instanceIndex, payload);
        if (!nonTransformInputsUnchanged)
            scene.BumpInstanceContinuityStamp(meshGpu->instanceIndex);
        if (prevDiffers || paletteOffsets.Active())
            m_RenderServices->AddFrameMover(meshGpu->instanceIndex, meshHandle,
                                            paletteOffsets.Current, paletteOffsets.Previous);
        WriteLastCaches(*meshGpu, mshIdx, matIdx, meshEntry->uploadSeq, wt->Version, flags,
                        skinPaletteOffset, lodBias, runtimeId, boundsCenter, boundsRadius,
                        meshGpu->LastSectorX, meshGpu->LastSectorY, meshGpu->LastSectorZ,
                        prevDiffers);
        ++refreshed;
    }
    return refreshed;
}

void RenderExtractionSystem::Update(ECS::World& world, float32 deltaTime)
{
    RenderServices* rs = m_RenderServices;
    if (!rs)
        return;

    auto* scene = rs->GetGPUScene();
    if (!scene)
        return;

    const uint64 worldId = world.GetWorldId();

    // prevTransform settle discipline: the LastPrevDiffers settle rebuild may
    // fire only on the FIRST extraction run of a feed swap window — a
    // same-window re-run (editor passive refresh) steps the same frame again
    // and must not zero a fresh mover's motion vector.
    {
        const uint64 swapGen = world.GetComponentDirtyFeed().SwapGeneration();
        m_AllowPrevSettle = (m_SettleWorldId != worldId) || (m_SettleSwapGen != swapGen);
        m_SettleWorldId = worldId;
        m_SettleSwapGen = swapGen;
    }

    // A2.1 sub-phase timing. Wall-clock at phase boundaries only — no
    // per-instance timers. Accumulators stay local; a snapshot is published to
    // RenderServices at the end for get_render_stats.
    using ExtractionClock = std::chrono::steady_clock;
    const auto extractStart = ExtractionClock::now();
    double lightsMs = 0.0;
    double gatherMs = 0.0;
    double processMs = 0.0;
    double prepareMs = 0.0; // full-lane parallel prepare (0 on fast frames)
    double applyMs = 0.0;   // full-lane serial apply (0 on fast frames)
    double submitMs = 0.0;
    uint32_t rebuildCount = 0;
    uint32_t skippedCount = 0;
    uint32_t recordCountOut = 0;
    uint32_t submissionCountOut = 0;
    uint32_t oceanCountOut = 0;
    const auto elapsedMs = [](ExtractionClock::time_point a, ExtractionClock::time_point b)
    { return std::chrono::duration<double, std::milli>(b - a).count(); };

    const bool trace = IsFrameTraceEnabled();
    const uint32_t sceneFrameIndex = scene->GetFrameIndex();
    if (trace)
    {
        Logger::Log::Debug(
            "[FrameTrace] frame={} phase=RenderExtraction.Begin world={} worldId={}",
            sceneFrameIndex,
            static_cast<const void*>(&world),
            worldId);
    }

    // ---- GE_EXTRACTION_FEED lane decision (fusion S2b) ----
    // All-or-nothing per frame: every precondition E1-E11 is evaluated every
    // frame regardless of lane (triggers consumed, never latched stale — the
    // TLAS m_NeedsReconcile precedent); any failure runs the shipped full
    // lane below unchanged, which self-repairs everything and re-primes
    // every fast-path cache at its end.
    uint32_t escalationReasons = 0;
    bool fastFrame = false;
    uint32_t feedPatchedCount = 0;
    uint32_t subsetRefreshedCount = 0;
    // L1b: fresh caster-change attribution each frame (the published set explains
    // exactly this frame's caster content-version bump, if one happens). A frame
    // that ends without overflow closes the log episode, so a persistent crowd
    // logs once, not per frame.
    if (!m_ChangedCasterOverflow)
        m_OverflowLogLatched = false;
    m_ChangedCasterSpheres.clear();
    m_ChangedCasterOverflow = false;
    if (m_FeedFastPath)
        fastFrame = EvaluateFastPathPreconditions(world, worldId, escalationReasons);
    else
        escalationReasons = kEscalationFeedDisabled;
    // Sampled before the full lane re-primes the caches it compares against.
    const bool onDemandViewChurnOnly =
        !fastFrame && IsOnDemandViewChurnOnly(world, escalationReasons);

    // Note: BeginWorldDrawFrame() is now called by RenderingLoop::Update()
    // before any system runs, so all extraction systems (terrain, render,
    // particles) start from a clean slate.

    // Clear cached GPUScene instances for disabled renderers in place. Removing
    // rows from the free-list leaves holes below GetInstanceCount(), so the
    // slot-scanning culling / draw-command passes must see an explicit
    // tombstone. If the MeshRenderer component itself is gone, release the slot.
    // Fast frames skip CLEANUP entirely: disable/removal transitions are
    // E4/E5 triggers, so tombstoning happens on the escalated frame (Q3) —
    // and the O(N) Write<MeshGPUData> grant stops stamping idle chunks.
    if (!fastFrame)
    {
        // The one extraction query that must still see what the gather no
        // longer does: a row the gather skips keeps its GPU slot, and this pass
        // is what tombstones it.
        world.Query<
                 GameEngine::ECS::Write<GameEngine::Components::MeshGPUData>,
                 GameEngine::ECS::Optional<GameEngine::Components::MeshRenderer>,
                 GameEngine::ECS::Optional<GameEngine::ECS::Disabled>,
                 GameEngine::ECS::Optional<GameEngine::ECS::DisabledInHierarchy>,
                 GameEngine::ECS::Optional<
                     GameEngine::ECS::ComponentDisabled<GameEngine::Components::MeshRenderer>>>()
            .IncludeDisabled()
            .Each([&](GameEngine::Components::MeshGPUData& meshGpu,
                      const GameEngine::Components::MeshRenderer* mr,
                      const GameEngine::ECS::Disabled* disabled,
                      const GameEngine::ECS::DisabledInHierarchy* inactive,
                      const GameEngine::ECS::ComponentDisabled<
                          GameEngine::Components::MeshRenderer>* rendererOff)
                  {
                      if (!mr)
                          ReleaseMeshGpuInstance(*scene, meshGpu);
                      else if (disabled || inactive || rendererOff)
                          ClearMeshGpuInstance(*scene, meshGpu); });
    }

    // Extract lights (CPU-side) for future Forward+/Deferred lighting passes.
    // Today this is used for diagnostics and to establish a stable data path.
    const auto lightsStart = ExtractionClock::now();
    uint32_t lightCount = 0;
    {
        world.Query<
                 GameEngine::ECS::Read<GameEngine::Components::WorldTransform>,
                 GameEngine::ECS::Read<GameEngine::Components::Light>>()
            .Each([&](GameEngine::ECS::EntityHandle e,
                      const GameEngine::Components::WorldTransform& xf,
                      const GameEngine::Components::Light& light)
                  {
                      GameEngine::Engine::Renderer::ExtractedLight out{};
                      out.SortId = e.id; // stable tie-break for the contribution sort
                      out.type = light.Type;
                      // Resolve photometric controls (color temperature + physical-intensity unit)
                      // into the plain GPU color/intensity — the same resolve ocean/terrain use, so
                      // every light consumer agrees. The GPU path is unchanged.
                      GameEngine::Components::ResolveLightColorIntensity(light, out.color, out.intensity);
                      out.range = light.Range;
                      out.innerAngle = light.InnerAngle;
                      out.outerAngle = light.OuterAngle;
                      out.areaShape = light.AreaShape;
                      out.areaWidth = light.AreaWidth;
                      out.areaHeight = light.AreaHeight;
                      out.areaRadius = light.AreaRadius;
                      out.falloff = light.Falloff;
                      out.decay = light.Decay;
                      out.fogContribution = std::max(light.FogContribution, 0.0f);
                      out.fogDensityBoost = std::max(light.FogDensityBoost, 0.0f);
                      out.fogAnisotropy = std::clamp(light.FogAnisotropy, -0.95f, 0.95f);
                      out.fogOriginFade = std::clamp(light.FogOriginFade, 0.0f, 1.0f);
                      out.castsLight = light.CastsLight ? 1u : 0u;
                      out.castsShadows = light.CastsShadows ? 1u : 0u;
                      out.cascadeCount = light.CascadeCount;
                      static_assert(static_cast<uint32_t>(Components::LightShadowTier::Inherit) == 0u,
                                    "kPunctualShadowTierInherit must match LightShadowTier::Inherit");
                      out.shadowResolutionTier =
                          static_cast<uint32_t>(light.ShadowResolutionTier);
                      out.shadowAngularDiameter = std::max(light.ShadowAngularDiameter, 0.0f);

                      // WorldTransform matrix is stored column-major; translation is the last column.
                      out.positionWS[0] = xf.matrix[12];
                      out.positionWS[1] = xf.matrix[13];
                      out.positionWS[2] = xf.matrix[14];

                      // Shine along entity forward (+Z column). Matches cameras and Unity.
                      out.directionWS[0] = xf.matrix[8];
                      out.directionWS[1] = xf.matrix[9];
                      out.directionWS[2] = xf.matrix[10];
                      Normalize3(out.directionWS[0], out.directionWS[1], out.directionWS[2]);
                      out.rightWS[0] = xf.matrix[0];
                      out.rightWS[1] = xf.matrix[1];
                      out.rightWS[2] = xf.matrix[2];
                      Normalize3(out.rightWS[0], out.rightWS[1], out.rightWS[2]);
                      out.upWS[0] = xf.matrix[4];
                      out.upWS[1] = xf.matrix[5];
                      out.upWS[2] = xf.matrix[6];
                      Normalize3(out.upWS[0], out.upWS[1], out.upWS[2]);

                      rs->SubmitLight(worldId, out);
                      ++lightCount; });
    }
    lightsMs = elapsedMs(lightsStart, ExtractionClock::now());

    ExtractPostProcessVolumes(world, worldId, rs);
    m_Particles.Extract(world, worldId, rs, deltaTime);
    // Single source of truth: order the world's lights strongest-first after
    // every SubmitLight, the particle lights included, and before any consumer
    // (LightUploadNode packing and the shadow-index builders both walk this
    // list in the same order).
    if (rs) rs->FinalizeWorldLights(worldId);
    if (rs && IsLightDiagEnabled())
        Logger::Log::Info("RenderExtraction: extracted {} lights, {} of them from particles (worldId={})",
                          rs->GetWorldLights(worldId).size(), rs->GetWorldLights(worldId).size() - lightCount, worldId);

    // World mesh extraction: MeshRenderer entities resolved via MeshGPURegistry +
    // MaterialRegistry and backed by GPUScene instances via MeshGPUData.
    {
        const auto& views = rs->Views().GetViews();
        if (!views.empty())
        {
            auto& meshReg = rs->GetMeshGPURegistry();
            auto& matReg = rs->Materials().Registry();

            std::vector<const GameEngine::Rendering::ViewDesc*> matchingViews;
            matchingViews.reserve(views.size());
            for (const auto& view : views)
            {
                if (view.worldId != 0 && view.worldId != worldId)
                    continue;
                matchingViews.push_back(&view);
                if (view.ActiveRenderLayerMask() != 0u)
                    rs->Views().MarkViewExtracted(view.id, sceneFrameIndex);
            }
            rs->Views().MarkWorldExtracted(worldId, sceneFrameIndex);

            if (IsExtractionDiagEnabled())
            {
                // Delta-only: only log when values change.
                static thread_local uint32_t s_PrevTotal = 0, s_PrevMatching = 0;
                const uint32_t curTotal = static_cast<uint32_t>(views.size());
                const uint32_t curMatching = static_cast<uint32_t>(matchingViews.size());
                if (s_PrevTotal != curTotal || s_PrevMatching != curMatching)
                {
                    s_PrevTotal = curTotal;
                    s_PrevMatching = curMatching;
                    Logger::Log::Info(
                        "RenderExtraction: worldId={} totalViews={} matchingViews={} (changed)",
                        worldId, curTotal, curMatching);
                }
            }

            static bool sWorldDiagLogged = false;

            // Renderable-membership digest for this world: what content is
            // rendered, not where it is. Terms hash STABLE identities (registry
            // handle, material asset ref) so recycled GPUScene slot indices
            // can't alias distinct content, and are combined with a wrapping
            // SUM so archetype iteration order can't move the digest. Published
            // every frame — including an empty one, so deleting the last
            // renderable still registers as a change. Visibility toggles count
            // as membership by design (a hidden object should leave probe
            // captures); transforms and transient particles are excluded.
            constexpr uint64 kFnvOffsetBasis = 14695981039346656037ull;
            uint64 contentDigest = kFnvOffsetBasis;

            if (fastFrame)
            {
                // FAST PATH (fusion S2b): no CLEANUP, no gather, no record
                // store — an O(dirty) patch from the Last* caches + fresh
                // WorldTransform reads, an O(subset) re-prepare for the
                // always-refresh archetypes, and a verbatim replay of the
                // last full pass's submissions and mesh digest. Every input
                // that could invalidate the replay escalated at Update entry.
                const auto processStart = ExtractionClock::now();
                feedPatchedCount = PatchDirtyInstances(world, *scene);
                subsetRefreshedCount =
                    RefreshAlwaysRefreshSubset(world, *scene, meshReg, matReg, worldId);
                processMs = elapsedMs(processStart, ExtractionClock::now());

                // Counters describe the last full pass on fast frames (D8).
                recordCountOut = static_cast<uint32_t>(m_Records.size());
                submissionCountOut = static_cast<uint32_t>(m_Submissions.size());

                // Replay: cached submissions are valid because none of their
                // inputs can have changed on a fast frame (E4/E5/E7/E9 guard
                // them; instance allocation/release happens only on full-lane
                // frames). Order is the full lane's deterministic order. The
                // zero-matching-views transition escalates via E9 and the
                // full lane rebuilds the cache (empty), so the replay can't
                // run outside its validity domain (correctness A9); the
                // views-empty state never reaches here (outer gate mirrors
                // the full lane's).
                if (!m_Submissions.empty())
                {
                    const auto submitStart = ExtractionClock::now();
                    std::span<const WorldSubmissionRecord> batch(m_Submissions.data(),
                                                                 m_Submissions.size());
                    rs->SubmitWorldSubmissions(batch);
                    submitMs = elapsedMs(submitStart, ExtractionClock::now());
                }

                // Mesh-membership digest replays from the cache; the ocean
                // term is genuinely per-frame and re-added below on both
                // lanes.
                contentDigest = m_CachedMeshDigest;

#if GE_DEBUG_INSTRUMENTATION
                // D9 shadow validator (GE_DEBUG_INSTRUMENTATION — active in
                // Debug and DebugFast, deliberately NOT the adjacent
                // #ifdef _DEBUG tripwire idiom, which DebugFast never
                // compiles): every 256th fast frame,
                // every entity in the patched population must satisfy
                // LastTransformVersion == live Version. A stale entity means
                // a producer bumped Version without a feed emission (managed
                // chunk-span writers are the known A4 residual) or a
                // fast-path bug. No silent reconcile in release — loud, and
                // force the full lane next frame.
                if ((++m_ValidateCounter & 255u) == 0u)
                {
                    uint32_t staleCount = 0;
                    world.Query<GameEngine::ECS::Read<GameEngine::Components::WorldTransform>,
                                GameEngine::ECS::Read<GameEngine::Components::MeshGPUData>>()
                        .Each([&](GameEngine::ECS::EntityHandle e,
                                  const GameEngine::Components::WorldTransform& wt,
                                  const GameEngine::Components::MeshGPUData& mg)
                              {
                                  if (mg.instanceIndex == 0xFFFFFFFFu ||
                                      mg.meshIndex == 0xFFFFFFFFu)
                                      return; // pending / tombstoned: not patched
                                  if (m_AlwaysRefreshSet.contains(e.id))
                                      return; // refreshed through the subset lane
                                  if (mg.LastTransformVersion != wt.Version)
                                      ++staleCount;
                              });
                    if (staleCount != 0)
                    {
                        Logger::Log::Error(
                            "RenderExtraction: feed shadow validator found {} stale "
                            "instance(s) after a fast frame — a WorldTransform writer "
                            "bumped Version without emitting to the dirty feed (managed "
                            "chunk-span writers are the known residual) or the fast path "
                            "failed to patch. Forcing the full lane.",
                            staleCount);
                        m_ValidatorForceFull = true;
                    }
                }
#endif
            }
            else
            {
            m_Records.clear();
            m_Submissions.clear();
            auto& records = m_Records;
            auto& submissions = m_Submissions;

            const auto gatherStart = ExtractionClock::now();
            world.Query<
                     GameEngine::ECS::Read<GameEngine::Components::WorldTransform>,
                     GameEngine::ECS::Read<GameEngine::Components::MeshRenderer>,
                     GameEngine::ECS::Optional<GameEngine::Components::LocalBounds>,
                     GameEngine::ECS::Optional<GameEngine::Components::WorldSectorCoord>,
                     GameEngine::ECS::Optional<GameEngine::Components::SkinnedMeshRenderer>,
                     GameEngine::ECS::Optional<GameEngine::Components::SkeletonRef>,
                     GameEngine::ECS::Optional<GameEngine::Components::AnimatorRef>,
                     GameEngine::ECS::Optional<GameEngine::Components::LODGroup>,
                     GameEngine::ECS::Optional<GameEngine::Components::MeshGPUData>,
                     GameEngine::ECS::Optional<GameEngine::Components::GIEmitter>>()
                .Each([&](GameEngine::ECS::EntityHandle e,
                          const GameEngine::Components::WorldTransform& xf,
                          const GameEngine::Components::MeshRenderer& mr,
                          const GameEngine::Components::LocalBounds* wb,
                          const GameEngine::Components::WorldSectorCoord* sc,
                          const GameEngine::Components::SkinnedMeshRenderer* smr,
                          const GameEngine::Components::SkeletonRef* skelRef,
                          const GameEngine::Components::AnimatorRef* anim,
                          const GameEngine::Components::LODGroup* lodGroup,
                          GameEngine::Components::MeshGPUData* meshGpu,
                          const GameEngine::Components::GIEmitter* giEmitter)
                      {
                          // HLOD residency suppression: a member whose cluster is
                          // proxy-active (or an inactive proxy entity) has had its
                          // GPUScene slot freed by the HLODSelectSystem. Skip it so
                          // it is not re-added to any view (color or shadow) while
                          // suppressed. Clearing hlodEvicted (proxy-exit) lets the
                          // next full lane re-add it via Op::Add. C6: eviction never
                          // touches mr.enabled / ECS::Disabled, so picking + outline
                          // + undo keep working off the still-live entity.
                          if (meshGpu && meshGpu->hlodEvicted)
                              return;
                          WorldRenderableRecord rec{};
                          rec.entity = e;
                          rec.worldTransform = xf;
                          rec.meshRenderer = mr;
                          rec.giEmitter = giEmitter != nullptr;
                          rec.meshGpu = meshGpu;
                          if (wb)
                          {
                              rec.hasBounds = true;
                              rec.bounds = *wb;
                          }
                          if (sc)
                          {
                              rec.hasSectorCoord = true;
                              rec.sectorCoord    = *sc;
                          }
                          if (smr && smr->skeletonId != 0)
                              rec.skeletonId = smr->skeletonId;
                          if (skelRef && skelRef->runtimeId != 0)
                              rec.runtimeId = skelRef->runtimeId;
                          if (anim && anim->ClipIndex != 0)
                              rec.hasActiveAnimation = true;
                          if (lodGroup)
                              rec.lodBias = lodGroup->Bias;
                          records.push_back(rec); });
            gatherMs = elapsedMs(gatherStart, ExtractionClock::now());
            recordCountOut = static_cast<uint32_t>(records.size());

            const bool extractDiag = IsExtractionDiagEnabled();
            if (extractDiag)
            {
                // Delta-only: only log when entity count changes.
                static thread_local uint32_t s_PrevRecords = 0xFFFFFFFFu;
                const uint32_t curRecords = static_cast<uint32_t>(records.size());
                if (s_PrevRecords != curRecords)
                {
                    s_PrevRecords = curRecords;
                    Logger::Log::Info("RenderExtraction: entityRecords={} (changed)", curRecords);
                }
            }

            uint32_t skipNoMeshHandle = 0, skipNoMeshEntry = 0;
            // skipNoMatType retained as 0 in the diagnostic struct after the
            // material-asset-type field was dropped from MeshRenderer; the
            // material-presence gate now reads the GUID directly.
            uint32_t skipNoMatType = 0, skipNoMatGuid = 0, skipNoMatFound = 0;
            uint32_t skipNoPipeline = 0;
            // Fusion E8 pending-transient shapes (unconditional).
            uint32_t pendingMaterialLoad = 0, pendingSentinelIndex = 0;

            const auto processStart = ExtractionClock::now();
            if (!records.empty())
            {
                // PROCESS (A2.1): parallel prepare + serial apply. Fills
                // submissions, contentDigest, rebuild/skipped + skipNo*
                // counters, plus the prepare/apply sub-splits.
                ProcessRecordsParallel(
                    world, *scene, *rs, worldId, matchingViews, meshReg, matReg,
                    extractDiag, records, submissions, *m_ParallelState,
                    contentDigest, rebuildCount, skippedCount,
                    skipNoMeshHandle, skipNoMeshEntry, skipNoMatGuid,
                    skipNoMatFound, skipNoPipeline,
                    pendingMaterialLoad, pendingSentinelIndex, prepareMs, applyMs,
                    m_AllowPrevSettle);
                processMs = elapsedMs(processStart, ExtractionClock::now());
                submissionCountOut = static_cast<uint32_t>(submissions.size());
                if (!submissions.empty())
                {
                    const auto submitStart = ExtractionClock::now();
                    std::span<const WorldSubmissionRecord> batch(submissions.data(), submissions.size());
                    rs->SubmitWorldSubmissions(batch);
                    submitMs = elapsedMs(submitStart, ExtractionClock::now());
                }
                if (extractDiag)
                {
                    // Delta-only: only log when submission counts change.
                    struct ExtrPrev
                    {
                        uint32_t subs;
                        uint32_t sMH;
                        uint32_t sME;
                        uint32_t sMT;
                        uint32_t sMG;
                        uint32_t sMF;
                        uint32_t sP;
                    };
                    static thread_local ExtrPrev s_Prev{};
                    ExtrPrev cur{static_cast<uint32_t>(submissions.size()), skipNoMeshHandle, skipNoMeshEntry, skipNoMatType, skipNoMatGuid, skipNoMatFound, skipNoPipeline};
                    if (std::memcmp(&s_Prev, &cur, sizeof(cur)) != 0)
                    {
                        s_Prev = cur;
                        Logger::Log::Info(
                            "RenderExtraction: submissions={} skipMeshHandle={} skipMeshEntry={} "
                            "skipMatType={} skipMatGuid={} skipMatFound={} skipPipeline={} (changed)",
                            cur.subs, cur.sMH, cur.sME, cur.sMT, cur.sMG, cur.sMF, cur.sP);
                    }
                }
                if (trace && !sWorldDiagLogged)
                {
                    sWorldDiagLogged = true;
                    Logger::Log::Info(
                        "RenderExtraction: views={}, records={}, submissions={}",
                        static_cast<uint32_t>(views.size()),
                        static_cast<uint32_t>(records.size()),
                        static_cast<uint32_t>(submissions.size()));
                }
            }

            // Fast-path cache re-prime. The full lane is the sole repair
            // path: everything the fast frames patch against or replay is
            // rebuilt here, unconditionally, whatever the escalation reason.
            if (m_FeedFastPath)
            {
                // Mesh-only digest (the per-frame ocean term is re-added on
                // both lanes below, before the publish).
                m_CachedMeshDigest = contentDigest;
                RebuildAlwaysRefreshSubset(world);
                m_LastPendingMeshEntry = skipNoMeshEntry;
                m_LastPendingMaterialLoad = pendingMaterialLoad;
                m_LastPendingPipeline = skipNoPipeline;
                m_LastPendingSentinelIndex = pendingSentinelIndex;
                m_LastPendingTransient = skipNoMeshEntry + pendingMaterialLoad +
                                         skipNoPipeline + pendingSentinelIndex;
                m_LastMaterialDigest = m_FrameMaterialDigest;
                m_LastViewFingerprint = m_FrameViewFingerprints.All;
                m_LastPersistentViewFingerprint = m_FrameViewFingerprints.Persistent;
                // E4 compare value sampled at full-lane END, post-apply — NOT
                // at entry: this lane's apply creates MeshGPUData
                // components and would trip its own gate, buying a wasted
                // second full frame after every creation burst (api-perf F7).
                m_FullPassStructuralVersion = world.GetStructuralChangeVersion();
                m_CachesPrimed = true;
                m_CachesWorldId = worldId;
            }
            } // full lane

            // Ocean surfaces are feature-rendered (no MeshRenderer record) but
            // still change what a probe capture photographs — fold their
            // presence in so creating or removing an ocean recaptures a "Once"
            // probe like any other geometry change. Terrain and particle
            // emitters are NOT covered yet: content rendered by those features
            // does not reach this digest.
            {
                // Arbitrary stable tag for "an ocean renderer".
                constexpr uint64 kOceanContentTag = 0x0CEA0CEAull;
                const uint64 oceanTerm =
                    GameEngine::Rendering::HashUtils::HashValue(kFnvOffsetBasis, kOceanContentTag);
                world.Query<GameEngine::ECS::Read<GameEngine::Components::OceanRenderer>>()
                    .Each([&](const GameEngine::Components::OceanRenderer&)
                          {
                              contentDigest += oceanTerm;
                              ++oceanCountOut;
                          });
            }

            rs->SetWorldRenderContentDigest(worldId, contentDigest);
        }
        else if (m_FeedFastPath)
        {
            // Neither lane ran, but the entry evaluation already consumed
            // the window-delivered triggers (E3 guard window, E5 gates, E6
            // reload flag) with no lane to act on them. Drop the prime so
            // the next gated frame takes the full lane locally, instead of
            // relying on the E9 fingerprint's storage point and view-id
            // monotonicity to force it from a distance.
            m_CachesPrimed = false;
        }
    }

    // L1a point-shadow caching: bump the world's shadow-caster content version
    // whenever this frame was NOT a pure idle fast frame, except a full lane run
    // only for OnDemand view churn. This reuses the lane decision as the
    // authoritative "did the renderable scene change?" verdict:
    //   - !fastFrame        : the full lane ran (structural / Changed<MeshRenderer,
    //                         LocalBounds, LODGroup> / mesh-reload / HLOD residency /
    //                         material digest / feed disabled-or-overflowed). Any of
    //                         these can alter a caster's rendered depth. An
    //                         OnDemand view's activation or expiry alone changes no
    //                         record a persistent view draws (IsOnDemandViewChurnOnly).
    //   - feedPatchedCount  : >=1 moved renderable was patched on the fast path.
    //   - subsetRefreshed   : an always-refresh carrier (skinned / animated) was
    //                         re-prepared — its shadow silhouette changes each frame.
    // Vertex-modifier content is NOT a term here: it deforms from the shared
    // animation clock with no instance record changing, and it reaches the
    // frame through producers this lane never sees, so RenderServices derives
    // it from every view's batch keys instead
    // (RaiseAnimatedVertexModifierContentSignals).
    // A pure idle fast frame (fastFrame, 0 patched, 0 subset) leaves the version
    // fixed, so EnsurePointShadowAssignment keeps every point light cached.
    // Camera motion is an idle fast frame here (E9 view fingerprint carries no
    // matrices), which is why a static-scene flythrough retains shadows.
    const bool contentChanged = (!fastFrame && !onDemandViewChurnOnly) ||
                                feedPatchedCount > 0 || subsetRefreshedCount > 0;
    if (contentChanged)
    {
        // L1b attribution: on a pure fast frame the ONLY instance-record changes
        // are the feed patches + subset refreshes above, whose caster spheres were
        // tracked — so the bump can name exactly which casters moved and far point
        // lights keep their cached maps. A full-lane frame (anything structural)
        // or sphere-list overflow cannot attribute and affects every light. The
        // bump TIMING is identical in both cases (unchanged from L1a).
        const bool unattributed = !fastFrame || m_ChangedCasterOverflow ||
                                  !IsShadowProximityKeyingEnabled();
        rs->NotifyShadowCasterContentChanged(
            worldId,
            unattributed ? std::span<const ShadowCasterChangeSphere>{}
                         : std::span<const ShadowCasterChangeSphere>(m_ChangedCasterSpheres),
            unattributed);
    }
    // Idle-recompute-elision content version: the same lane verdict. The
    // animated-vertex term that also belongs here is raised by the batch-key
    // derivation, which sees every producer's records rather than this lane's.
    if (contentChanged)
        rs->NotifyRenderContentChanged(worldId);

    // Publish A2.1 sub-phase timings. Ring the last-frame totals so a single
    // poll sees a stable windowed mean/max under the bench's low FPS.
    {
        const double totalMs = elapsedMs(extractStart, ExtractionClock::now());
        m_StatWindow[m_StatWindowHead] =
            ExtractionStatSample{totalMs, gatherMs, processMs, prepareMs, applyMs, fastFrame};
        m_StatWindowHead = (m_StatWindowHead + 1) % kExtractionStatsWindow;
        if (m_StatWindowCount < kExtractionStatsWindow)
            ++m_StatWindowCount;

        double totalSum = 0.0, gatherSum = 0.0, processSum = 0.0, totalMax = 0.0;
        double prepareSum = 0.0, applySum = 0.0;
        uint32_t fastFramesInWindow = 0;
        for (std::size_t i = 0; i < m_StatWindowCount; ++i)
        {
            const auto& s = m_StatWindow[i];
            totalSum += s.totalMs;
            gatherSum += s.gatherMs;
            processSum += s.processMs;
            prepareSum += s.prepareMs;
            applySum += s.applyMs;
            totalMax = std::max(totalMax, s.totalMs);
            if (s.fastFrame)
                ++fastFramesInWindow;
        }
        const double invCount = m_StatWindowCount ? 1.0 / static_cast<double>(m_StatWindowCount) : 0.0;

        RenderServices::RenderExtractionStats stats{};
        stats.TotalMs = totalMs;
        stats.LightsMs = lightsMs;
        stats.GatherMs = gatherMs;
        stats.ProcessMs = processMs;
        stats.PrepareMs = prepareMs;
        stats.ApplyMs = applyMs;
        stats.SubmitMs = submitMs;
        stats.TotalMsMean = totalSum * invCount;
        stats.GatherMsMean = gatherSum * invCount;
        stats.ProcessMsMean = processSum * invCount;
        stats.PrepareMsMean = prepareSum * invCount;
        stats.ApplyMsMean = applySum * invCount;
        stats.TotalMsMax = totalMax;
        stats.RecordCount = recordCountOut;
        stats.SubmissionCount = submissionCountOut;
        stats.RebuildCount = rebuildCount;
        stats.SkippedCount = skippedCount;
        stats.FastFrame = fastFrame ? 1u : 0u;
        stats.FastFramesInWindow = fastFramesInWindow;
        stats.WindowTicks = static_cast<uint32_t>(m_StatWindowCount);
        stats.EscalationReasonBits = escalationReasons;
        stats.FeedPatchedCount = feedPatchedCount;
        stats.SubsetRefreshedCount = subsetRefreshedCount;
        stats.PendingMeshEntry = m_LastPendingMeshEntry;
        stats.PendingMaterialLoad = m_LastPendingMaterialLoad;
        stats.PendingPipeline = m_LastPendingPipeline;
        stats.PendingSentinelIndex = m_LastPendingSentinelIndex;
        const Particles::ParticleExtractionStats particles = m_Particles.Stats();
        stats.ParticleEmitterCount = particles.EmitterCount;
        stats.LiveParticles = particles.LiveParticles;
        stats.ParticleSimulationMs = particles.SimulationMs;
        stats.VolumeCount = static_cast<uint32_t>(m_Volumes.size());
        stats.OceanCount = oceanCountOut;
        rs->SetRenderExtractionStats(stats);
    }

    if (trace)
    {
        Logger::Log::Debug(
            "[FrameTrace] frame={} phase=RenderExtraction.End world={} worldId={}",
            sceneFrameIndex,
            static_cast<const void*>(&world),
            worldId);
    }
}

} // namespace Engine::Renderer
} // namespace GameEngine
