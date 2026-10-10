#pragma once

// MeshGPURegistry: owns GPU-resident mesh resources (VB/IB/optional streams),
// keyed by (asset GUID, submesh index) for deduplication and sharing.
//
// Responsibilities:
//   - Upload vertex/index data from ModelAsset meshes to GPU buffers.
//   - Return stable MeshGPUHandle values that can be stored in ECS components
//     and used as geometry draw keys for instanced bucketing.
//   - Guarantee that the same (GUID, submeshIndex) always returns the same handle
//     and underlying GPU buffers (no duplication).
//   - Manage GPU buffer lifetime (ref-counted or registry-owned).
//
// Ownership:
//   - MeshGPURegistry is owned by RenderServices.
//   - GPU buffers are created via IDevice and destroyed when the registry shuts down
//     or when an entry is explicitly unregistered.
//   - Callers hold lightweight MeshGPUHandle values; they do NOT own GPU resources.
//
// Thread safety:
//   - Every mutation (register / unregister / reload / re-provision) and every
//     table walk is serialized by an internal recursive mutex. WHICH thread
//     calls them varies between calls -- an ECS wave dispatches its systems onto
//     job workers and more than one of those systems registers meshes -- so what
//     must hold is mutual exclusion, not affinity to one thread.
//   - Find() and the drawability chokepoints take no lock. They sit in the
//     per-record draw loops, where acquiring even an uncontended shared lock
//     costs an order of magnitude more than the lookup it protects. They are
//     safe only while no mutation is in flight, which the frame shape hands the
//     per-frame draw consumers for free: the ECS wave that registers is joined
//     before graph build reads. A caller that touches the registry from INSIDE a
//     wave holds a TableScope across its whole interaction instead -- that
//     excludes every mutator, its own included. Developer builds assert when a
//     lock-free lookup runs against a scope another thread holds.

#include "Types/ScopedSubscription.h"
#include "AssetCore/GUID.h"
#include "Engine/Rendering/BucketPoolAllocator.h"
#include "Engine/Rendering/MeshGPUResidency.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Mathematics/Geometry.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
class ModelAsset;
struct Mesh;

namespace Rendering
{
class IDevice;
class GPUScene;

// A bucket groups meshes that share the same vertex-attribute layout.
// `BucketKey` is just the VertexAttributeFlags set the mesh owns -- meshes
// with the same key share VBs and can be drawn in one
// vkCmdDrawIndexedIndirectCount call (once Phase 4 lands). The key is
// stable across re-imports of the same mesh provided the source attribute
// set is unchanged.
using MeshGPUBucketKey = VertexAttributeFlags;

inline constexpr uint32_t kInvalidMeshGPUPoolIndex = UINT32_MAX;

// One GPU buffer + suballocator for a bucket stream. A stream can own
// multiple sibling pools so large meshes and later overflow do not invalidate
// existing entries.
struct MeshGPUStreamPool
{
    BufferHandle buffer{};
    BucketPoolAllocator allocator;
    uint64_t capacityBytes = 0;
    uint32_t strideBytes = 1;
};

// Per-bucket GPU storage. One or more VkBuffers per stream the bucket owns +
// lazy uint16 / uint32 index pools. Allocations are suballocated via
// BucketPoolAllocator (byte-offsets), and the result is recorded on each
// MeshGPUEntry as (pool index, vertexOffset/firstIndex) in *vertices* /
// *indices* -- the conversion happens at upload time using per-stream strides.
struct MeshGPUBucket
{
    MeshGPUBucketKey key = VertexAttributeFlags::None;

    // Stream VBs. corePools is always non-empty for a successfully uploaded
    // mesh; optional stream pools are created on first use.
    std::vector<MeshGPUStreamPool> corePools;
    std::vector<MeshGPUStreamPool> tangentPools;
    std::vector<MeshGPUStreamPool> colorPools;
    std::vector<MeshGPUStreamPool> uv1Pools;
    std::vector<MeshGPUStreamPool> jointsPools;
    std::vector<MeshGPUStreamPool> weightsPools;
    std::vector<MeshGPUStreamPool> joints1Pools;
    std::vector<MeshGPUStreamPool> weights1Pools;
    std::array<std::vector<MeshGPUStreamPool>, 6> extraUvPools;

    // Index buffer pools, lazily created. A bucket can have both 16- and
    // 32-bit indices, with sibling pools per index type.
    std::vector<MeshGPUStreamPool> ib16Pools;
    std::vector<MeshGPUStreamPool> ib32Pools;

    // Vertex stride (bytes) of the core interleaved binding for this
    // bucket's key. Cached so byte->vertex conversion is one mul.
    uint32_t coreStrideBytes = 0;
};

// GPU-resident data for a single submesh.
//
// Buffers are bucket-owned; consumers fetch the actual BufferHandles via
// MeshGPURegistry::TryGetDrawableBindings(entry, out). The entry itself carries
// draw-time metadata (offsets in vertex / index units, the bucket key,
// the vertex-flag set the source mesh provided) plus an optional CPU
// mesh mirror for generated geometry that needs editor picking.
struct MeshGPUEntry
{
    // Draw parameters (LOD0 — kept for non-LOD consumers).
    uint32_t indexCount   = 0;
    uint32_t firstIndex   = 0;   // in indices, not bytes
    uint32_t vertexOffset = 0;   // in vertices, not bytes
    uint32_t indexType    = 0;   // Rendering::IndexType cast to uint32_t
    Rendering::PrimitiveTopology topology = Rendering::PrimitiveTopology::TriangleList;

    // LOD index ranges. All LODs share the entry's vertex buffer and live in a
    // single contiguous index allocation. lodCount>=1; lodFirstIndex[0] /
    // lodIndexCount[0] mirror firstIndex / indexCount (LOD0).
    static constexpr uint32_t kMaxEntryLODs = 4;
    uint32_t lodCount = 1;
    uint32_t lodFirstIndex[kMaxEntryLODs] = {0, 0, 0, 0};
    uint32_t lodIndexCount[kMaxEntryLODs] = {0, 0, 0, 0};

    // Per-LOD vertex offset RELATIVE to vertexOffset (in vertices). 0 for LOD0
    // and for any generated (index-only) level that shares LOD0's vertex block;
    // >0 only for an authored level whose own vertices were concatenated after
    // LOD0's in the core VB (Phase C1). The draw resolves absolute vertex as
    // vertexOffset + lodVertexOffset[lod]. Note the space difference from
    // lodFirstIndex, which is ABSOLUTE within the index allocation.
    uint32_t lodVertexOffset[kMaxEntryLODs] = {0, 0, 0, 0};

    // Achieved meshopt relative error per LOD, absolute-LOD indexed (lodError[0]
    // is LOD0 = 0). lodSloppy[k]!=0 marks a sloppy-simplified level whose error
    // is on a different scale. BuildGpuMeshRow derives per-mesh coverage
    // thresholds from these (see MeshLODThresholds.h). Zeroed when LODs carry no
    // error data (legacy / procedural meshes), which falls back to defaults.
    float   lodError[kMaxEntryLODs]  = {0, 0, 0, 0};
    uint8_t lodSloppy[kMaxEntryLODs] = {0, 0, 0, 0};

    // Artist-authored switch coverages (Phase C2 direct-coverage path), same
    // index space as row.lodThreshold: lodCoverage[k] governs leaving LOD k.
    // lodCoverageCount>0 makes BuildGpuMeshRow seed thresholds directly from
    // these (ApplyAuthoredLODCoverage) instead of the error-derived mapping.
    // Copied from Mesh::ExtraLODCoverage only for an honored authored chain (not
    // for a chain dropped to LOD0-only over optional streams), so it never seeds
    // thresholds for LODs the GPU did not upload.
    float    lodCoverage[kMaxEntryLODs] = {0, 0, 0, 0};
    uint32_t lodCoverageCount           = 0;

    // True when the uploaded chain is artist-authored (Phase C1, honored — a
    // chain dropped to LOD0-only over optional streams stays false). Authored
    // levels carry no meshopt metric, so BuildGpuMeshRow routes them to the
    // default threshold table (or authored coverages) instead of the
    // error-derived mapping. Provenance is explicit — never inferred from
    // lodError values, which a generated sloppy level can legitimately zero.
    bool lodAuthored = false;

    // Vertex layout descriptor for this submesh.
    VertexAttributeFlags vertexFlags = VertexAttributeFlags::None;

    // Bucket this entry's geometry lives in. Equal to vertexFlags for
    // the standard case; kept as a distinct field so a future bucket-
    // merging policy could collapse buckets without touching the source
    // `vertexFlags`. Set to None when allocation failed (mesh fails to
    // draw -- bucket pool exhaustion is the only path to this state).
    MeshGPUBucketKey bucketKey = VertexAttributeFlags::None;

    // Conservative local culling envelope, including admitted own-vertex LODs.
    Mathematics::BoundingBox bounds;
    // LOD0 metric used for authored coverage and meshopt error thresholds.
    float lodReferenceRadius = 0.0f;
    float lodReferenceMaxExtent = 0.0f;

    // Optional CPU copy used by editor picking for generated/runtime meshes
    // whose source geometry is not backed by a ModelAsset.
    std::shared_ptr<const Mesh> cpuMesh;

    // Per-stream suballocations into the owning bucket's pools (units =
    // bytes). Returned to the pools' deferred-free queue on
    // UnregisterModel so frames-in-flight that were still reading the
    // range can finish before it is reused.
    BucketPoolAllocator::Allocation coreSubAlloc{};
    BucketPoolAllocator::Allocation tangentSubAlloc{};
    BucketPoolAllocator::Allocation colorSubAlloc{};
    BucketPoolAllocator::Allocation uv1SubAlloc{};
    BucketPoolAllocator::Allocation jointsSubAlloc{};
    BucketPoolAllocator::Allocation weightsSubAlloc{};
    BucketPoolAllocator::Allocation joints1SubAlloc{};
    BucketPoolAllocator::Allocation weights1SubAlloc{};
    std::array<BucketPoolAllocator::Allocation, 6> extraUvSubAlloc{};
    BucketPoolAllocator::Allocation indexSubAlloc{};

    // Per-stream pool selectors into the owning bucket. Optional streams keep
    // kInvalidMeshGPUPoolIndex when absent.
    uint32_t corePoolIndex    = kInvalidMeshGPUPoolIndex;
    uint32_t tangentPoolIndex = kInvalidMeshGPUPoolIndex;
    uint32_t colorPoolIndex   = kInvalidMeshGPUPoolIndex;
    uint32_t uv1PoolIndex     = kInvalidMeshGPUPoolIndex;
    uint32_t jointsPoolIndex  = kInvalidMeshGPUPoolIndex;
    uint32_t weightsPoolIndex = kInvalidMeshGPUPoolIndex;
    uint32_t joints1PoolIndex  = kInvalidMeshGPUPoolIndex;
    uint32_t weights1PoolIndex = kInvalidMeshGPUPoolIndex;
    std::array<uint32_t, 6> extraUvPoolIndex{
        kInvalidMeshGPUPoolIndex,
        kInvalidMeshGPUPoolIndex,
        kInvalidMeshGPUPoolIndex,
        kInvalidMeshGPUPoolIndex,
        kInvalidMeshGPUPoolIndex,
        kInvalidMeshGPUPoolIndex};
    uint32_t indexPoolIndex = kInvalidMeshGPUPoolIndex;

    // Slot in GPUScene's mesh SSBO (~0u when no GPUScene is wired). The
    // future bucketer + draw_command_scatter.comp index this table via
    // GPUInstance.meshIndex.
    uint32_t gpuMeshIndex = ~0u;

    // FNV-1a hash of the source mesh's geometry (vertices, indices, LODs,
    // optional streams). RegisterSubmesh compares this on a dedup hit to detect
    // in-place content changes (e.g. LOD generation) and re-upload, since the
    // dedup key {assetGuid, submeshIndex} alone cannot see content edits.
    uint64_t contentHash = 0;

    // Identity of the upload that filled this entry's suballocations, minted by
    // MeshGPUResidency. THE residency key: 0 means never uploaded, released or
    // tombstoned, and reads NOT resident. Every chokepoint answers "may this
    // entry be drawn?" from this field, so it must be cleared on any path that
    // invalidates the bytes it stands for.
    uint64_t uploadSeq = 0;
};

// Stream BufferHandles for an entry, resolved against its bucket. Only ever
// produced by a TryGetDrawableBindings call that returned true, so coreVB and
// indexBuffer are always valid here; the optional-stream handles are invalid
// iff the mesh's vertexFlags does not own that stream.
struct MeshGPUEntryBindings
{
    BufferHandle coreVB{};
    BufferHandle tangentVB{};
    BufferHandle colorVB{};
    BufferHandle uv1VB{};
    BufferHandle jointsVB{};
    BufferHandle weightsVB{};
    BufferHandle joints1VB{};
    BufferHandle weights1VB{};
    std::array<BufferHandle, 6> extraUvVB{};
    BufferHandle indexBuffer{};
    uint32_t     indexType = 0;

    // Pool-identity compare: two entries with equal bindings draw from the
    // same physical buffers, so their records may share one indirect draw
    // (the sorted transparent run key groups on this).
    bool operator==(const MeshGPUEntryBindings&) const = default;
};

// The vertex layout a draw that binds every valid stream of `bindings` at its fixed slot satisfies:
// the entry's vertexFlags without the streams that are not bound, so a pipeline keyed on the result
// declares no binding the draw leaves empty (VUID-vkCmdDrawIndexed-04007). Core (position, normal,
// UV0) has no flag and is always bound. `ignoreVertexColor` (MaterialDocument::ignoreVertexColor)
// also drops HasColor: the variant compiles without HAS_COLOR and the bound colour stream goes
// unread at its slot, which shifts nothing since every stream's slot is fixed.
VertexAttributeFlags BoundStreamVertexFlags(const MeshGPUEntry& entry, const MeshGPUEntryBindings& bindings,
                                            bool ignoreVertexColor);

// Lightweight handle to a registered GPU mesh entry.
// This is the canonical geometry draw key used for instanced bucketing.
struct MeshGPUHandleTag {};
using MeshGPUHandle = StrongHandle<MeshGPUHandleTag>;

// Composite key for deduplication: (asset GUID, submesh index within that asset).
struct MeshGPUKey
{
    GUID assetGuid;
    uint32_t submeshIndex = 0;

    bool operator==(const MeshGPUKey& other) const
    {
        return assetGuid == other.assetGuid && submeshIndex == other.submeshIndex;
    }
};

} // namespace Rendering
} // namespace GameEngine

// Hash for MeshGPUKey
namespace std
{
template <>
struct hash<GameEngine::Rendering::MeshGPUKey>
{
    size_t operator()(const GameEngine::Rendering::MeshGPUKey& k) const noexcept
    {
        size_t h = std::hash<GameEngine::GUID>{}(k.assetGuid);
        h ^= std::hash<uint32_t>{}(k.submeshIndex) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }
};
} // namespace std

namespace GameEngine
{
namespace Rendering
{

// Where a re-provisioned entry's CPU geometry came from. The registry does not
// resolve sources itself — the caller's lookup does — so this is how a restore
// reports which class it belonged to.
enum class MeshGPUSourceOrigin : uint8_t
{
    None = 0,  // no reachable source; the entry tombstones
    Asset,     // an AssetManager-resident ModelAsset (M1, cache-first: no disk load)
    Generated, // regenerated from a deterministic identity (engine built-in primitives)
};

// A CPU source for one re-upload. `Geometry` must stay alive for the duration of
// the ReprovisionAfterDeviceRebuild call: the registry reads it synchronously and
// does not take ownership.
struct MeshGPUCpuSource
{
    const Mesh*         Geometry = nullptr;
    MeshGPUSourceOrigin Origin   = MeshGPUSourceOrigin::None;
};

// Outcome of ReprovisionAfterDeviceRebuild — a single observable line proving the
// pass ran and how each entry class was handled (Q6 slice 4).
struct MeshGPUReprovisionReport
{
    uint32_t EntriesTotal         = 0; // live entries walked
    uint32_t RestoredFromAsset    = 0; // re-uploaded from an AssetManager-resident ModelAsset (M1)
    uint32_t RestoredFromCpuMesh  = 0; // re-uploaded from a retained CPU picking mirror
    uint32_t RestoredFromGenerated = 0; // regenerated from identity (built-in primitives)
    uint32_t Tombstoned           = 0; // no reachable CPU source (asset unloaded / sourceless procedural)
    uint32_t GpuRowsPreserved     = 0; // entries whose GPUScene mesh-row index survived in place (F9)
};

// Outcome of ReloadModelMeshes — one observable line per hot-reloaded model.
struct MeshGPUReloadReport
{
    uint32_t SubmeshesTotal      = 0; // submeshes in the reloaded asset
    uint32_t SubmeshesReuploaded = 0; // geometry differed: fresh GPU allocation, same handle
    uint32_t SubmeshesUnchanged  = 0; // byte-identical geometry: upload skipped
    uint32_t SubmeshesRemoved    = 0; // trailing submeshes the re-import dropped

    bool ChangedAnything() const { return SubmeshesReuploaded != 0 || SubmeshesRemoved != 0; }
};

// What the Route B chokepoint refused, so a gate that silently never fires is
// distinguishable from no gate at all. Route A reports its own skips, from
// MeshPoolGroupPlan, because that is where they happen.
struct MeshGPUResidencyStats
{
    // Per-frame, reset by BeginFrame. Unit is CALLS refused: an entry refused
    // by two consumers in one frame counts twice. Only the non-resident count
    // is a defect signal — a missing bucket is a designed state (a tombstoned
    // entry, a failed allocation, a tree part with no built mesh), so gating on
    // the total would fire on correct behaviour. These answer "what is being
    // refused right now"; a poll that reads them zero cannot tell "never
    // refused" from "refused in a frame nobody sampled".
    uint64_t RouteBRefusedNonResident   = 0;
    uint64_t RouteBRefusedBucketMissing = 0;

    // The same two events, monotonic for the process and never cleared. A
    // steady-state criterion is checked against these: one poll at any time
    // answers whether the gate has ever refused in this session.
    uint64_t RouteBRefusedNonResidentTotal   = 0;
    uint64_t RouteBRefusedBucketMissingTotal = 0;

    // Cumulative for the process: a wrong window constant is a sizing defect,
    // and clearing the evidence each frame would hide it. Must read 0 — but a
    // zero is NOT evidence while the upload bracket is the only obligation a
    // stamp can carry: occupancy is then at most one slot, so exhaustion would
    // need a window's worth of simultaneously-open uploads and cannot happen.
    // The arm is covered by the unit tests that run a 4-slot window, and the
    // counter starts reporting once staged upload records arm several
    // obligations per stamp.
    uint64_t WindowExhausted = 0;
};

// One pool buffer's suballocator occupancy, tagged with enough identity to
// locate it: which bucket owns it, which vertex/index stream it backs, and
// which sibling pool within that stream it is.
//
// Fragmentation in these pools has a cost the allocator's own counters are the
// only witness to: a failed best-fit appends a sibling pool at double the
// capacity, so fragmentation converts into GPU memory growth and into extra
// pool groups (and therefore extra indirect draws). Reporting Stats::fragRatio
// and Stats::freeRangeCount is what lets that be measured rather than asserted.
struct MeshGPUPoolStats
{
    MeshGPUBucketKey BucketKey = VertexAttributeFlags::None;
    // Stream identity, matching the pool debug-name suffix ("Core", "Tangent",
    // "IB16", ...). A string literal with static lifetime.
    const char* Stream = "";
    // Index of this pool within its stream's sibling list.
    uint32_t PoolIndex = 0;
    BucketPoolAllocator::Stats Allocator{};
};

class MeshGPURegistry
{
  public:
    // Mutual exclusion over the registry tables, held across a whole
    // read-then-use sequence rather than around each call.
    //
    // This is what a caller that touches the registry from inside an ECS wave
    // holds: the wave runs its systems on job workers, several at once, so one
    // system's Find() can otherwise observe another's registration mid-resize.
    // The individual calls cannot close that on their own -- Find() hands back a
    // pointer INTO storage a concurrent registration reallocates, so the lookup
    // and the dereference must sit inside the same scope.
    //
    // Recursive: the mutators compose (RegisterModelMeshes -> RegisterSubmesh,
    // ReloadModelMeshes -> Register/UnregisterSubmesh) and a scope holder goes
    // on to call them.
    class TableScope
    {
      public:
        explicit TableScope(const MeshGPURegistry& registry);
        ~TableScope();

        TableScope(const TableScope&)            = delete;
        TableScope& operator=(const TableScope&) = delete;

      private:
        const MeshGPURegistry& m_Registry;
    };

    // THE LOD switch points (descending screen coverage: fraction of half
    // viewport height the bounding sphere must project to; entries beyond a
    // mesh's lodCount stay 0 so the coarsest level is the floor). Consumed by
    // ge_SelectLOD in draw_command_scatter.comp via the GPUMesh row. This is the
    // single source of truth — per-entity influence is LODGroup.Bias only.
    static constexpr float kDefaultLODThresholds[4] = {0.5f, 0.2f, 0.08f, 0.0f};

    // `pendingUploadWindow` sizes the residency ring: how many uploads can be
    // in flight before BeginUpload fails closed. Parameterised rather than
    // fixed so the window-advance and exhaustion behaviour can be driven in a
    // test at a size a test can reach — an arm nothing can drive is an arm
    // nothing has checked.
    explicit MeshGPURegistry(uint32_t pendingUploadWindow = MeshGPUResidency::kDefaultPendingWindow)
        : m_Residency(pendingUploadWindow)
    {
    }
    // Subscribers that outlive this object need no notification of its death:
    // their ScopedSubscription keeps a weak_ptr to the subscriber BLOCK, so a
    // late unsubscribe either finds the block (registry gone, block kept
    // alive by the subscription for the call) or no-ops. No lifetime flag,
    // no manual destructor bookkeeping on either side.
    ~MeshGPURegistry() = default;

    // Non-copyable, non-movable (owns GPU resources via device).
    MeshGPURegistry(const MeshGPURegistry&) = delete;
    MeshGPURegistry& operator=(const MeshGPURegistry&) = delete;

    // Initialize with a device for buffer creation.
    void Initialize(IDevice* device);

    // Shutdown: destroy all GPU buffers.
    void Shutdown();

    // Optional GPUScene link. When set, RegisterSubmesh writes a GPUMesh
    // row into the SSBO and UnregisterModel clears it. Used by the future
    // bucketer + draw_command_scatter.comp consumers. Safe to leave unset --
    // unit tests and bringup paths run fine without a GPUScene.
    void SetGPUScene(GPUScene* gpuScene)
    {
        TableScope scope(*this);
        m_GPUScene = gpuScene;
    }
    GPUScene* GetGPUScene() const { return m_GPUScene; }

    // Register all submeshes of a ModelAsset. Returns handles for each submesh.
    // If the asset (by GUID) is already registered, returns existing handles
    // without re-uploading. This is the primary entry point.
    std::vector<MeshGPUHandle> RegisterModelMeshes(const GUID& assetGuid,
                                                    const ModelAsset& asset);

    // Register a single submesh by key. Used internally and for manual registration.
    // `mesh` is the CPU-side mesh data to upload. Set retainCpuMesh for
    // generated/runtime meshes that have no asset-backed CPU source, so
    // consumers that need to read the geometry back on the CPU can still find
    // it: editor ray picking (MeshPickingService) and the DDGI software trace
    // lane (DDGISceneService, which builds its BVH from CPU triangles and
    // silently omits any mesh it cannot resolve). Asset-backed meshes need it
    // only if the asset may be evicted, since those consumers fall back to
    // ModelAsset. Returns existing handle if already registered.
    MeshGPUHandle RegisterSubmesh(const MeshGPUKey& key,
                                  const Mesh& mesh,
                                  bool retainCpuMesh = false);

    // Unregister one submesh key and release its bucket allocations.
    // Intended for runtime-generated meshes (morph targets, procedural edits)
    // that need to be replaced without unregistering the source model.
    bool UnregisterSubmesh(const MeshGPUKey& key);

    // Look up a previously registered entry by handle.
    const MeshGPUEntry* Find(MeshGPUHandle handle) const;

    // Iterate every live entry; used by MeshPoolGroupPlan to derive the
    // per-frame geometry-bind group map. Walks under the table mutex, so it is
    // safe beside a registration on another thread -- but `func` must not
    // register or unregister: that mutates the storage being walked, and the
    // mutex being recursive would let it through.
    template <typename Func>
    void ForEachEntry(Func&& func) const
    {
        TableScope scope(*this);
        m_Entries.ForEach([&](auto /*handle*/, const MeshGPUEntry& entry) { func(entry); });
    }

    // Iterate every live entry WITH its composite key (asset GUID + submesh).
    // Same walk contract as ForEachEntry. Diagnostics/introspection: the key is
    // what maps a GPUScene mesh row back to its source asset.
    template <typename Func>
    void ForEachKeyedEntry(Func&& func) const
    {
        TableScope scope(*this);
        for (const auto& [key, handle] : m_KeyToHandle)
        {
            if (const MeshGPUEntry* entry = Find(handle))
                func(key, *entry);
        }
    }

    // Look up by composite key. The scope this takes internally covers the
    // lookup, NOT the caller's use of the result: like Find, the returned
    // pointer aims into storage that a registration reallocates, so it is valid
    // only while the caller holds a TableScope of its own -- otherwise only
    // until the next mutation, which the developer-build detector cannot see
    // because the read already returned.
    const MeshGPUEntry* FindByKey(const MeshGPUKey& key) const;

    // CPU geometry/identity epoch for derived-data caches. Pool residency and
    // frame progress do not change it; registration, removal and rebuild do.
    uint64_t GetContentRevision() const
    {
        TableScope scope(*this);
        return m_ContentRevision;
    }

    // Look up handle by composite key. Returns invalid handle if not found.
    MeshGPUHandle FindHandle(const MeshGPUKey& key) const;

    // Get all submesh handles for a previously registered model asset.
    // Returns empty vector if the asset has not been registered.
    std::vector<MeshGPUHandle> GetModelHandles(const GUID& assetGuid) const;

    // Unregister all submeshes for an asset GUID and destroy their GPU buffers.
    // Destructive: the generational MeshGPUHandles that live MeshRenderer
    // components hold are invalidated, and nothing re-registers them. Use it
    // when the model is genuinely leaving memory, NOT for a hot-reload —
    // ReloadModelMeshes is the in-place path that keeps those handles alive.
    void UnregisterModel(const GUID& assetGuid);

    // Refresh a registered model's GPU geometry from a reloaded ModelAsset,
    // preserving identity: each submesh re-uploads into the SAME handle slot and
    // the SAME GPUScene mesh row, so the MeshGPUHandles cached on live
    // MeshRenderer components and the GPUInstance.meshIndex values referencing
    // them keep resolving. This is what a .glb hot-reload must call —
    // RegisterModelMeshes short-circuits on an already-registered GUID and so
    // cannot see new geometry, and UnregisterModel destroys the handles the
    // scene still points at.
    //
    // Per-submesh content hashing means an unchanged submesh costs no upload, so
    // a re-import that touched one part of a model re-uploads only that part.
    // Submeshes the re-import dropped are unregistered; submeshes it added are
    // registered (no entity references them until the scene is reloaded).
    //
    // No-op for a GUID that was never registered. Serialized against every other
    // registry mutation, so the calling thread may vary between calls.
    MeshGPUReloadReport ReloadModelMeshes(const GUID& assetGuid, const ModelAsset& asset);

    // Re-provision every live entry's GPU storage after an in-place device rebuild
    // (Q6 slice 4, design §8). The rebuild teardown freed every bucket-pool
    // VkBuffer, but the registry TABLE survives — so the generational MeshGPUHandles
    // cached on ECS components and their GPUScene mesh-row indices must stay valid.
    // Recreates the bucket pools and re-uploads each entry's geometry into the SAME
    // handle and SAME GPUScene row (F9 index preservation). `sourceLookup` resolves
    // an entry's key to its CPU geometry — the registry does not know how sources are
    // found, only that it needs one per key. It falls back to a retained CPU picking
    // mirror when the lookup yields nothing. An entry with no reachable source at all
    // is tombstoned: its bindings go invalid and its GPU row empties, so extraction
    // and the draw scatter skip it gracefully rather than read a stale range.
    //
    // `sourceLookup` MUST NOT trigger a disk load — re-provision is a synchronous
    // RAM->VRAM re-upload, not streaming — and any geometry it returns must outlive
    // the call.
    //
    // A tombstone is permanent until something re-registers the key, so every class
    // of entry needs a source here or an owner that re-registers it. Built-in
    // primitives have no asset and no picking mirror; they are recovered because the
    // lookup regenerates them from their deterministic GUIDs.
    MeshGPUReprovisionReport ReprovisionAfterDeviceRebuild(
        const std::function<MeshGPUCpuSource(const MeshGPUKey&)>& sourceLookup);

    // Query: how many distinct model assets are registered?
    size_t GetRegisteredModelCount() const
    {
        TableScope scope(*this);
        return m_ModelHandles.size();
    }

    // Query: how many total GPU mesh entries exist?
    size_t GetEntryCount() const
    {
        TableScope scope(*this);
        return m_Entries.Size();
    }

    // Which mapping seeds LOD switch points (MeshLODThresholds.h), including
    // Off. Setting it re-derives and re-uploads the lodThreshold/lodFlags
    // fields of every registered GPUMesh row; nothing else in the row changes,
    // so no buffer is reallocated and the draw stream keeps its structure —
    // the same instances, batches and record slots. What DOES re-run is
    // selection: the row rewrite dirties GPUScene, which advances its content
    // epoch, and that epoch is an input to the scatter's idle-elision blob, so
    // the next frame recomputes the draw records instead of reusing them.
    // Retained static shadow cascades re-render because the mode is part of
    // their cache key (CascadeRenderInputs::SelectionMode).
    //
    // Serialized against registration, and only outside the render phase: the
    // write lands in GPUScene's CPU mirror, and the device write is the ordinary
    // per-slot flush during graph build.
    //
    // Returns the number of rows rewritten (0 when the mode is unchanged).
    size_t SetLodSelectionMode(LodSelectionMode mode);
    LodSelectionMode GetLodSelectionMode() const { return m_LodSelectionMode; }

    // THE read-half chokepoint: the only way to get a pool BufferHandle out of
    // this registry. Returns false -- and leaves `out` value-initialised, with
    // no pool handle written to it -- when the entry must not be drawn, which
    // is either of:
    //   - its bytes are not readable by the GPU yet (residency), or
    //   - it has no bucket (allocation failed, or it was tombstoned).
    //
    // It refuses; it never lies. Handing back all-invalid bindings instead
    // would make two different non-drawable meshes compare EQUAL as a pool
    // identity and collapse into one sorted-transparent run.
    //
    // Every refusal is COUNTED (MeshGPUResidencyStats). A caller that wants the
    // answer rather than the bytes must ask IsEntryDrawable instead.
    [[nodiscard]] bool TryGetDrawableBindings(const MeshGPUEntry& entry,
                                              MeshGPUEntryBindings& out) const;

    // The same tests as TryGetDrawableBindings, with no bindings handed back
    // and no refusal counted. This is how a diagnostic asks: the refusal totals
    // never reset, and the mesh-pool gate's acceptance criterion is that they
    // read zero, so an observer routed through the counting chokepoint writes
    // the number it is sampling — one render-stats poll previews several batch
    // keys and can bank several refusals per view.
    [[nodiscard]] bool IsEntryDrawable(const MeshGPUEntry& entry) const;

    // Whether this entry's uploaded bytes are readable by the GPU. Fails
    // closed: an entry whose state cannot be established is not resident.
    // Route A (the pool-group plan) asks this directly because it publishes an
    // absence rather than a binding.
    [[nodiscard]] bool IsFlushResident(const MeshGPUEntry& entry) const;

    // Core interleaved-VB stride for the entry's bucket, in bytes. 0 when the
    // entry has no bucket. Narrow read for consumers that need the stride
    // without any buffer handle (BLAS geometry description).
    uint32_t GetEntryCoreStrideBytes(const MeshGPUEntry& entry) const;

    // Sibling index-pool census for a bucket: how many pools of the given
    // index width it owns. Grows when a mesh does not fit the existing pools.
    // `indexType` uses MeshGPUEntry::indexType's encoding (Rendering::IndexType
    // cast to uint32_t).
    size_t GetBucketIndexPoolCount(MeshGPUBucketKey key, uint32_t indexType) const;

    // Test / debug accessor for the live bucket map.
    size_t GetBucketCount() const
    {
        TableScope scope(*this);
        return m_Buckets.size();
    }

    // What the two chokepoints refused. See MeshGPUResidencyStats for the unit
    // of each field.
    MeshGPUResidencyStats GetResidencyStats() const;

    // Suballocator occupancy for every live pool, one entry per pool buffer.
    // Walks the bucket table, so call it from diagnostics rather than from a
    // per-frame path.
    std::vector<MeshGPUPoolStats> GetPoolStats() const;

    // Advance the registry's frame counter and reclaim any deferred-free
    // suballocations whose retire-after frame has passed. Also resets the
    // per-frame residency refusal counters. Call once per logical render frame
    // from the same thread that owns rendering (so there is a single writer to
    // the frame counter).
    //
    // `frameIndex` must be monotonically non-decreasing across calls.
    void BeginFrame(uint64_t frameIndex);

    // Current frame index as last observed by BeginFrame. Used by the
    // bucket-aware UnregisterModel path to stamp deferred frees.
    uint64_t GetCurrentFrameIndex() const { return m_CurrentFrameIndex; }

    // Mesh-change notification.
    //
    // Subscribers receive an event whenever a registered asset's GPU
    // geometry stops being the geometry they last saw: the asset is
    // unregistered/destroyed, or its .glb is hot-reloaded in place. In the
    // unregister case the affected handles become invalid; in the reload
    // case they stay valid but every buffer range, GPU row and bound behind
    // them is new. Either way subscribers must drop cached state keyed by
    // (assetGuid, ...) — a BLAS, the Scene TLAS's per-leaf bounds, a cached
    // draw submission — and re-derive it from the registry.
    //
    // Thread affinity: callbacks fire synchronously on whichever thread
    // triggered the unregister -- an ECS wave worker among them -- and they run
    // WITH the registry's table mutex held. A subscriber must therefore be
    // wait-free: never call back into MeshGPURegistry, never block on another
    // thread, and never take a lock another thread can hold while waiting on
    // this registry. The engine's subscribers publish a relaxed atomic flag and
    // return, which is the shape to copy.
    using ReloadCallback = std::function<void(const GUID& assetGuid)>;

    // The returned subscription unsubscribes on destruction, and does so
    // safely even if this registry died first — see ScopedSubscription.
    // Subscribing after some assets have already registered does not
    // retroactively notify; subscribers query existing state separately if
    // they need a starting snapshot.
    // A callback must not throw: it observes a removal the registry has already
    // committed, so there is nothing for it to refuse. One that throws anyway is
    // logged and isolated rather than allowed to strand the remaining listeners.
    //
    // Delivery snapshots registrations without allocating. New registrations
    // start with the next event; explicit removal before a listener's turn
    // suppresses that call. Reset does not wait for an executing call;
    // unloading callback code still requires the existing quiescent boundary.
    [[nodiscard]] ScopedSubscription SubscribeReload(ReloadCallback callback);

  private:
    // Look up the bucket that owns the given vertex-attribute set. Returns
    // nullptr if no bucket has been created for this key (i.e. no mesh
    // with these flags has been registered yet). The returned pointer is
    // stable for the bucket's lifetime; buckets are not destroyed until
    // Shutdown.
    //
    // Private: a bucket carries pool BufferHandles, so exposing it would be a
    // second route to an entry's bytes that bypasses TryGetDrawableBindings.
    const MeshGPUBucket* GetBucket(MeshGPUBucketKey key) const;

    // How the drawability tests came out, carrying the refusal reason the two
    // counters split on.
    enum class Drawability
    {
        Drawable,
        BucketMissing,
        NonResident
    };

    // Sole implementation of the drawability tests: it resolves `out` on
    // success and value-initialises it on every refusal, and it counts nothing.
    // TryGetDrawableBindings and IsEntryDrawable differ ONLY in what they do
    // with the verdict, so the counted and uncounted reads cannot drift into
    // disagreeing about what is drawable.
    Drawability ClassifyDrawability(const MeshGPUEntry& entry, MeshGPUEntryBindings& out) const;

    // Fire the unregister event to all subscribers. Every call site holds the
    // table mutex, so subscribers run under it -- see SubscribeReload for the
    // wait-free obligation that puts on them.
    void NotifyUnregister(const GUID& assetGuid);

    // Upload a CPU-side Mesh to GPU buffers, creating a MeshGPUEntry.
    MeshGPUEntry UploadMesh(const Mesh& mesh);

    // Create an empty sibling pool buffer of the given byte size and append it
    // to a bucket stream. Returns false on failure.
    bool CreateBucketStreamPool(std::vector<MeshGPUStreamPool>& pools,
                                uint64_t capacityBytes,
                                uint64_t alignmentBytes,
                                const char* debugName) const;

    // Log which memory a freshly created pool buffer actually landed in.
    //
    // Where mesh geometry lives cannot be inferred from device capabilities: a
    // large resizable-BAR window says only that a mapped write CAN reach VRAM,
    // while the residency policy the buffer was created under decides whether
    // that memory was a candidate at all. Any later claim about mesh-pool
    // residency has to rest on this observation instead.
    //
    // Logs the first pool, then only pools whose resolved memory differs from
    // it — so a pool that spills to a different heap once the BAR window fills
    // is reported, and a scene that creates dozens of identical pools stays
    // quiet. Never per frame: pool creation is rare.
    //
    // "First" is per device, not per process: ReprovisionAfterDeviceRebuild
    // clears the latch, because every pool that set it was freed with the old
    // device and the rebuilt one re-decides residency from scratch.
    void LogPoolMemoryResidency(BufferHandle buffer, const char* debugName) const;

    // Find or create the bucket for the given vertex-attribute key.
    // Bucket VB pools are created lazily on first use per stream (see
    // UploadMesh).
    MeshGPUBucket& GetOrCreateBucket(MeshGPUBucketKey key);

    // Return all of an entry's bucket suballocations to the pools'
    // deferred-free queue, stamped to retire kMaxFramesInFlight after
    // the current frame, and retire the upload stamp those suballocations
    // belonged to.
    void ReleaseEntryToBucket(MeshGPUEntry& entry);

    // THE mutual-exclusion point for m_Entries, m_KeyToHandle, m_ModelHandles,
    // m_Buckets, the GPUScene mesh rows and m_Residency's writer half. Taken
    // through TableScope and never directly, so scope entry and exit are the
    // one place the developer-build counter below is maintained.
    mutable std::recursive_mutex m_TableMutex;

#if GE_DEBUG_INSTRUMENTATION
    // Best-effort tripwire for the half this design leaves lock-free, not a
    // proof of absence: it samples at the instant of one lookup, so it fires
    // only when a lock-free read and a foreign scope actually interleave, and a
    // racing read that lands either side of the scope goes unseen. It also does
    // not distinguish a mutating scope from a read-only one -- a report means
    // "read while another thread held the tables", which is a contract
    // violation either way, not necessarily "read mid-resize".
    mutable std::atomic<uint32_t> m_ScopesHeld{0};
    static thread_local uint32_t  s_LocalScopeDepth;

    void AssertNotRacingMutation() const;
#endif

    IDevice*  m_Device   = nullptr;
    GPUScene* m_GPUScene = nullptr;

    // Upload stamps and their outstanding obligations. Sole owner: every
    // uploadSeq in this registry's entries is minted here.
    MeshGPUResidency m_Residency;

    // Refusal counters, per-frame beside process-total. Written from const
    // chokepoints and read from any thread, so atomic; see
    // MeshGPUResidencyStats for their units. Count through
    // CountRefused{NonResident,BucketMissing} so the two halves of a pair can
    // never drift apart.
    mutable std::atomic<uint64_t> m_RouteBRefusedNonResident{0};
    mutable std::atomic<uint64_t> m_RouteBRefusedBucketMissing{0};
    mutable std::atomic<uint64_t> m_RouteBRefusedNonResidentTotal{0};
    mutable std::atomic<uint64_t> m_RouteBRefusedBucketMissingTotal{0};

    void CountRefusedNonResident() const
    {
        m_RouteBRefusedNonResident.fetch_add(1u, std::memory_order_relaxed);
        m_RouteBRefusedNonResidentTotal.fetch_add(1u, std::memory_order_relaxed);
    }
    void CountRefusedBucketMissing() const
    {
        m_RouteBRefusedBucketMissing.fetch_add(1u, std::memory_order_relaxed);
        m_RouteBRefusedBucketMissingTotal.fetch_add(1u, std::memory_order_relaxed);
    }

    LodSelectionMode m_LodSelectionMode = LodSelectionMode::Sse;

    // Generational storage for entries (handle -> entry).
    GenerationalVector<MeshGPUEntry> m_Entries;
    uint64_t m_ContentRevision = 1;

    // Dedup map: composite key -> handle.
    std::unordered_map<MeshGPUKey, MeshGPUHandle> m_KeyToHandle;

    // Per-asset index: GUID -> list of submesh handles (in submesh order).
    std::unordered_map<GUID, std::vector<MeshGPUHandle>> m_ModelHandles;

    // Bucket storage. Keyed by the vertex-attribute flag set; one bucket
    // per layout. Buckets are created lazily by UploadMesh and live until
    // Shutdown. Suballocations come and go as meshes register / unregister.
    // The map provides reference stability for live buckets (unordered_map
    // does not move existing nodes on insertion).
    std::unordered_map<uint32_t, MeshGPUBucket> m_Buckets;

    // What LogPoolMemoryResidency saw for the first pool of the session; later
    // pools are only logged when they differ from it. Mutable because pool
    // creation runs from a const helper, and written under the same table lock
    // that serialises every other registry mutation.
    mutable std::optional<IDevice::BufferMemoryResidency> m_FirstPoolMemoryResidency;

    // Subscription changes allocate snapshots; notification and Reset do not.
    // A removed record can remain in an in-flight snapshot, but its detachable
    // callable is released immediately unless that call is currently executing.
    struct ReloadSubscriber
    {
        std::shared_ptr<ReloadCallback> Callback;
    };
    // shared_ptr-owned so a ScopedSubscription can outlive the registry: the
    // subscription's keepalive is a weak_ptr to THIS block, and its
    // unsubscribe closure touches only the block, never the registry.
    struct ReloadSubscriberBlock
    {
        std::mutex                    Mutex;
        using Snapshot = std::vector<std::shared_ptr<ReloadSubscriber>>;
        std::shared_ptr<const Snapshot> Subscribers;
    };
    std::shared_ptr<ReloadSubscriberBlock> m_ReloadSubscribers =
        std::make_shared<ReloadSubscriberBlock>();

    // Frame counter for deferred suballocation reclamation. Starts at 0;
    // each BeginFrame call increments and reclaims pool ranges retired
    // before this frame. If BeginFrame is never wired in by a host
    // (e.g. in unit tests), the counter stays at 0 and deferred ranges
    // are never reclaimed -- safe for short-running tests, would leak
    // pool capacity in a long-running editor session, which is why
    // RenderServices is expected to wire BeginFrame in.
    uint64_t m_CurrentFrameIndex = 0;
};

} // namespace Rendering
} // namespace GameEngine
