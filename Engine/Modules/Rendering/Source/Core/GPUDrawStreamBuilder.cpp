#include "Rendering/Core/GPUDrawStreamBuilder.h"

#include "Logger/Logger.h"

#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace GameEngine
{
namespace Rendering
{

GPUDrawStreamBuilder::ShaderLoaderFunc GPUDrawStreamBuilder::s_ShaderLoader = nullptr;

void GPUDrawStreamBuilder::SetShaderLoader(ShaderLoaderFunc loader)
{
    s_ShaderLoader = loader;
}

ResourceBarrier GPUDrawStreamBuilder::CreateStreamReadyBarrier()
{
    return ResourceBarrier::CreateMemoryBarrier(
        static_cast<uint64_t>(PipelineStageMask::Transfer)
            | static_cast<uint64_t>(PipelineStageMask::ComputeShader),
        static_cast<uint64_t>(PipelineStageMask::DrawIndirect)
            | static_cast<uint64_t>(PipelineStageMask::GraphicsVertex),
        static_cast<uint64_t>(ResourceAccessMask::TransferWrite)
            | static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
        static_cast<uint64_t>(ResourceAccessMask::IndirectCommandRead)
            | static_cast<uint64_t>(ResourceAccessMask::ShaderRead));
}

namespace
{

// Mirrors VkDrawIndexedIndirectCommand. 5 uint32s = 20 bytes per record.
constexpr uint32_t kDrawCommandStride = 5 * sizeof(uint32_t);

// Floor for the per-view per-instance history buffers (the dwell band's, and
// the rendered level and phase pair), and the size of the never-read stand-in
// bound where a slice carries neither. Keeps tiny scenes off a zero-sized
// allocation.
constexpr uint32_t kPerInstanceHistoryMinBytes = 4096;

// GE_LOD_HYSTERESIS: "0" forces every slice's dwell band to 0, restoring
// stateless per-frame LOD selection. Default on, meaning "honour whatever band
// the caller passed" — which is 0 unless something sets it, so this knob only
// ever takes the feature AWAY. It exists so an A/B measurement (and the
// captures whose reproducibility depends on stateless selection) can pin the
// old behaviour without rebuilding.
bool LodHysteresisEnabledFromEnv()
{
    const char* env = std::getenv("GE_LOD_HYSTERESIS");
    return env == nullptr || env[0] != '0';
}

// GE_SCATTER_COMPACT: coalesced scatter-hot fetch (lever 1). Default on; "0"
// selects the full-fat package for A/B benching. Read once — GPUScene reads the
// same var to decide whether to maintain/upload the mirror, so the two agree.
bool ScatterCompactRequestedFromEnv()
{
    const char* env = std::getenv("GE_SCATTER_COMPACT");
    return env == nullptr || env[0] != '0';
}

// GE_SCATTER_STATS: per-slice drawnTriangles subgroup reduction + atomic (the
// P0 shadow-arc instrumentation). Default OFF — the single-address atomic was
// ~68% of Scatter.World post-#447 (scatter-world-decomp2-2026-07), and it feeds
// only the bench/editor shadow-debug readout, never render correctness. Opt in
// with "1" when triangle counts are needed. Orthogonal to GE_SCATTER_COMPACT;
// the tripwire atomics (tableMisses/overflows) stay live in both variants.
bool ScatterStatsRequestedFromEnv()
{
    const char* env = std::getenv("GE_SCATTER_STATS");
    return env != nullptr && env[0] == '1';
}

// Shaderpkg path for the (fetch, instrumentation) variant. The four packages
// are {full-fat,compact} × {stats-off,on}; the base names keep the compact
// axis's meaning and a "_stats" suffix is the orthogonal instrumentation axis.
const char* ScatterPkgPath(bool compact, bool stats)
{
    if (compact)
        return stats ? "Shaders/draw_command_scatter_compact_stats.shaderpkg"
                     : "Shaders/draw_command_scatter_compact.shaderpkg";
    return stats ? "Shaders/draw_command_scatter_stats.shaderpkg"
                 : "Shaders/draw_command_scatter.shaderpkg";
}

// std430 stride of the coalesced scatter-hot mirror (GPUInstanceScatterHot).
// The scatter binds binding 1 at this stride in the compact variant.
constexpr size_t kScatterHotStride = 32;

// Winding parity rides bit 24 of the BatchTableEntry meshIndex FIELD (real
// mesh indices are 24-bit — BatchRegistry::MakeKey masks them). A parity-1
// (mirrored) sibling row sorts above all parity-0 rows of the same material
// (bit 24 is high); the scatter searches (searchMat, realMesh | this bit).
// MUST match draw_command_scatter.comp's kMeshParityBit.
constexpr uint32_t kMeshParityBit = 1u << 24;

// The table builders merge per-(classKey, meshIndex-with-parity). MakeKey masks
// the mesh field to 24 bits and so cannot carry the parity bit, so the merge
// map uses this wider pack: classKey (24 bits) in the high half, the full
// meshIndex-with-parity (25 bits used) in the low half. Emission unpacks it
// back into BatchTableEntry{materialIndex=classKey, meshIndex=meshWithParity}.
constexpr uint64_t MakeParityMergeKey(uint32_t classKey, uint32_t meshWithParity)
{
    return (static_cast<uint64_t>(classKey) << 32) | static_cast<uint64_t>(meshWithParity);
}
constexpr uint32_t ParityMergeClass(uint64_t key) { return static_cast<uint32_t>(key >> 32); }
constexpr uint32_t ParityMergeMesh(uint64_t key) { return static_cast<uint32_t>(key & 0xFFFFFFFFu); }

// Pseudo group for mesh rows absent from the pool-group map (draw
// consolidation) — see the header constant's contract. Absent rows can
// survive the scatter's mesh-row checks (a live mesh row with no plan entry)
// and are scattered into the merged pseudo row; it is never drawn because
// consumers filter the sentinel before their range lookup.
constexpr uint32_t kAbsentPoolGroup = GPUDrawStreamBuilder::kAbsentPoolGroup;

// Draw-consolidation mesh axis: the pool group when the map is active, the
// meshIndex itself otherwise. The value lands in the table's 24-bit mesh field
// (parity bit 24 rides on top), so real group ids must stay below the absent
// sentinel — MeshPoolGroupPlan asserts that on assignment.
inline uint32_t TableMeshAxis(std::span<const uint32_t> meshPoolGroup, uint32_t meshIndex)
{
    if (meshPoolGroup.empty())
        return meshIndex;
    const uint32_t group =
        meshIndex < meshPoolGroup.size() ? meshPoolGroup[meshIndex] : kAbsentPoolGroup;
    assert(group <= kAbsentPoolGroup && "pool group id exceeds the 24-bit mesh field");
    return group;
}

} // namespace

GPUDrawStreamBuilder::GPUDrawStreamBuilder(IDevice* device) : m_Device(device) {}

GPUDrawStreamBuilder::~GPUDrawStreamBuilder()
{
    Shutdown();
}

bool GPUDrawStreamBuilder::Initialize()
{
    if (!m_Device)
        return false;

    // Color-class map upload cadence. Default: re-upload per schedule call so a
    // later call's binding-8 map matches its freshly-rebuilt table (the map is
    // NOT frame-invariant — see m_ClassMapRingOffset). "0" restores the buggy
    // once-per-frame upload to reproduce the scatter tableMiss tripwire.
    if (const char* e = std::getenv("GE_SCATTER_COLORMAP_PERCALL"))
        m_ColorMapPerCall = e[0] != '0';

    m_LodHysteresisEnabled = LodHysteresisEnabledFromEnv();

    // Allocate the shared ordering sentinel up front so consumers (e.g. the
    // world color pass setup) can import it on the first BuildFrameGraph call
    // -- before any scatter pass exists. See header comment on
    // GetSentinelBuffer for the cross-pass-sync rationale. The scatter
    // pipeline and arena buffers are created lazily on first schedule.
    BufferDesc sentinelDesc{};
    sentinelDesc.size = 16; // smallest practical Storage buffer
    sentinelDesc.usage =
        static_cast<uint32_t>(BufferUsage::Storage)
        | static_cast<uint32_t>(BufferUsage::TransferDst);
    sentinelDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    sentinelDesc.debugName   = "GPUDrawStream.Sentinel";
    try
    {
        m_SentinelBuffer = m_Device->CreateBuffer(sentinelDesc);
    }
    catch (const std::exception& e)
    {
        if (!m_SentinelFailureLogged)
        {
            Logger::Log::Error("[GPUDrawStream] failed to create sentinel buffer: {}", e.what());
            m_SentinelFailureLogged = true;
        }
        return false;
    }
    catch (...)
    {
        if (!m_SentinelFailureLogged)
        {
            Logger::Log::Error("[GPUDrawStream] failed to create sentinel buffer (unknown exception)");
            m_SentinelFailureLogged = true;
        }
        return false;
    }

    return m_SentinelBuffer.IsValid();
}

void GPUDrawStreamBuilder::Shutdown()
{
    if (m_Device)
    {
        if (m_SentinelBuffer.IsValid())
            m_Device->DestroyBuffer(m_SentinelBuffer);
        for (BufferHandle h : {m_SharedRecords, m_SharedCursors, m_SharedIndirection,
                               m_SharedStats, m_TableRing, m_SharedCursorReadback,
                               m_SharedStatsReadback})
            if (h.IsValid())
                m_Device->DestroyBuffer(h);
        if (m_HistoryStandIn.IsValid())
            m_Device->DestroyBuffer(m_HistoryStandIn);
        for (const auto& entry : m_PrevLod)
            if (entry.second.buffer.IsValid())
                m_Device->DestroyBuffer(entry.second.buffer);
        for (const RetiredBuffer& r : m_RetiredBuffers)
            if (r.handle.IsValid())
                m_Device->DestroyBuffer(r.handle);
        for (const auto& [viewId, fade] : m_LodFade)
            if (fade.buffer.IsValid())
                m_Device->DestroyBuffer(fade.buffer);
        for (const auto& [viewId, history] : m_RenderedHistory)
            for (BufferHandle h : history.buffers)
                if (h.IsValid())
                    m_Device->DestroyBuffer(h);
    }
    ResetTrackingState();
}

void GPUDrawStreamBuilder::ResetTrackingState()
{
    m_SharedRecords            = BufferHandle{};
    m_SharedCursors            = BufferHandle{};
    m_SharedIndirection        = BufferHandle{};
    m_SharedStats              = BufferHandle{};
    m_TableRing                = BufferHandle{};
    m_SharedCursorReadback     = BufferHandle{};
    m_SharedStatsReadback      = BufferHandle{};
    // Dropping the history is always safe: the dwell band converges from an
    // empty buffer in one frame, so a device rebuild costs one stateless frame.
    m_PrevLod.clear();
    m_PrevLodResetTracker = PrevVisibleResetTracker{};
    m_PrevLodResetScratch.clear();
    m_PrevLodResetRuns.clear();
    m_HistoryStandIn          = BufferHandle{};
    m_SharedIndirectionAddress = 0;
    m_RecordCapacity = m_CursorCapacity = m_TableRingBytes = 0;
    m_ArenaRecordCursor = m_ArenaCursorCursor = m_ArenaStatsCursor = m_TableRingCursor = 0;
    m_SliceStatsBindings.clear();
    m_UnobservableTailSlices = 0;
    m_ShadowArcStats.clear();
    // The census described GPU regions that no longer exist, and the gate stats
    // its stamp indexes are about to be dropped too — invalidate rather than
    // leave a stamp that a rebuilt count could eventually match by coincidence.
    m_LiveTail = LiveTailObservation{};
    m_LastTripwire = ScatterStats{};
    m_LastTripwireSlices.clear();
    m_TripwireLatched = false;
    m_StatsSliceOverflowLogged = false;
    m_PendingRecordCapacity = m_PendingCursorCapacity = m_PendingTableRingBytes = 0;
    m_ClassMapRingOffset = m_ClassMapBytes = 0;
    m_ClassMapUploaded = false;
    m_RetiredBuffers.clear();
    // Per-view crossfade history: the handles are dead (destroyed above, or
    // freed wholesale by a device rebuild), so drop them and let the next
    // enabled frame re-create + refill with kLodNoHistory. Losing the history
    // is benign: a no-history slot adopts its next pick as its settled state,
    // so an instance that was mid-dissolve pops to its target level.
    m_LodFade.clear();
    m_LodFadeResetTracker = PrevVisibleResetTracker{};
    m_LodFadeResetScratch.clear();
    m_LodFadeResetRuns.clear();
    // Per-view rendered level and phase pairs: the handles are dead, so drop
    // them. The next requesting frame re-creates the pair and refills both
    // halves with kLodNoHistory, which costs that view one frame with no
    // previous pair — the same price a device rebuild pays everywhere else
    // here, and the state a consumer must already handle on its first frame.
    m_RenderedHistory.clear();
    m_RenderedHistoryViewCount.store(0u, std::memory_order_relaxed);
    m_RenderedHistoryResetTracker = PrevVisibleResetTracker{};
    m_RenderedHistoryResetScratch.clear();
    m_RenderedHistoryResetRuns.clear();
    m_FrameSlices.clear();
    m_RangeMap.clear();
    m_SentinelBuffer               = BufferHandle{};
    m_ScatterPipeline              = PipelineHandle{};
    m_GroupCompactPipeline         = PipelineHandle{};
    m_GroupCompactFailureLogged    = false;
    m_ScatterPipelineFailureLogged = false;
    m_ScatterHotMissingLogged      = false;
    m_SentinelFailureLogged        = false;
    // Every retained GPU region the elision gates' records describe is gone
    // (shutdown or device rebuild) — drop the gates so each suffix restarts
    // at FirstEvaluate.
    m_ElisionGates.clear();
}

RecomputeElisionGate::Stats GPUDrawStreamBuilder::GetElisionStats() const
{
    RecomputeElisionGate::Stats sum{};
    for (const auto& [suffix, gate] : m_ElisionGates)
    {
        const RecomputeElisionGate::Stats& s = gate.Gate.GetStats();
        sum.Evaluated += s.Evaluated;
        sum.Skipped += s.Skipped;
        for (size_t i = 0; i < sum.CauseCounts.size(); ++i)
            sum.CauseCounts[i] += s.CauseCounts[i];
    }
    return sum;
}

uint64_t GPUDrawStreamBuilder::GetScatterRecomputeCount() const
{
    // The signal is the gates' OWN recompute verdicts rather than a predicate
    // assembled one layer up: the epochs the engine pushes are a strict subset of
    // what the elision blob compares (it also carries the camera, the LOD knobs
    // and the tables), so any hand-rolled mirror silently misses the changes that
    // start most transitions.
    const RecomputeElisionGate::Stats stats = GetElisionStats();
    const auto count = [&stats](ElisionCause c)
    { return stats.CauseCounts[static_cast<size_t>(c)]; };
    return count(ElisionCause::FirstEvaluate) + count(ElisionCause::Forced)
           + count(ElisionCause::EvaluationGap) + count(ElisionCause::InputsChanged);
}

void GPUDrawStreamBuilder::ReprovisionAfterDeviceRebuild()
{
    // The device-rebuild teardown already freed every VkBuffer/VkPipeline; drop the
    // dead handles WITHOUT DestroyBuffer (that would double-free a recycled slot),
    // then recreate the up-front sentinel. The arenas, table ring, and scatter
    // pipeline rebuild lazily on the next schedule via EnsureSharedBuffers.
    ResetTrackingState();
    Initialize();
}

// ---- R1.3/R1.4 scatter path ------------------------------------------------

std::vector<GPUDrawStreamBuilder::BatchTableEntry>
GPUDrawStreamBuilder::BuildBatchTable(const BatchRegistry& registry,
                                      std::span<const uint32_t> meshPoolGroup,
                                      uint32_t* outTotalRecords)
{
    std::vector<BatchTableEntry> table;
    table.reserve(registry.BatchCount());
    if (meshPoolGroup.empty())
    {
        for (const auto& [key, count] : registry.Counts())
        {
            const uint32_t materialIndex = BatchRegistry::MaterialIndexFromKey(key);
            const uint32_t meshIndex     = BatchRegistry::MeshIndexFromKey(key);
            assert((meshIndex & kMeshParityBit) == 0u
                   && "real meshIndex must be < 2^24 — bit 24 is the winding parity flag");
            // Color/depth-prepass split every parity (the gl_FrontFacing shading
            // nuance applies to double-sided too). Emit only the non-empty lanes so
            // a mirror-free scene produces exactly today's one-row-per-batch table.
            if (count.Even > 0u)
                table.push_back({materialIndex, meshIndex, 0u, count.Even});
            if (count.Odd > 0u)
                table.push_back({materialIndex, meshIndex | kMeshParityBit, 0u, count.Odd});
        }
    }
    else
    {
        // Draw consolidation: remap the mesh axis to pool groups and MERGE the
        // rows that collapse — one region per (materialIndex, group, parity).
        // Registry keys are unique per (mat, mesh), so merging only happens
        // across meshes of one group; Σ capacities is invariant.
        std::unordered_map<uint64_t, uint32_t> merged;
        merged.reserve(registry.BatchCount());
        for (const auto& [key, count] : registry.Counts())
        {
            const uint32_t materialIndex = BatchRegistry::MaterialIndexFromKey(key);
            const uint32_t meshIndex     = BatchRegistry::MeshIndexFromKey(key);
            assert((meshIndex & kMeshParityBit) == 0u
                   && "real meshIndex must be < 2^24 — bit 24 is the winding parity flag");
            const uint32_t meshAxis = TableMeshAxis(meshPoolGroup, meshIndex);
            if (count.Even > 0u)
                merged[MakeParityMergeKey(materialIndex, meshAxis)] += count.Even;
            if (count.Odd > 0u)
                merged[MakeParityMergeKey(materialIndex, meshAxis | kMeshParityBit)] += count.Odd;
        }
        for (const auto& [mergedKey, count] : merged)
        {
            BatchTableEntry e{};
            e.materialIndex = ParityMergeClass(mergedKey);
            e.meshIndex     = ParityMergeMesh(mergedKey); // group | parity bit 24
            e.capacity      = count;
            table.push_back(e);
        }
    }
    std::sort(table.begin(), table.end(),
              [](const BatchTableEntry& a, const BatchTableEntry& b)
              {
                  if (a.materialIndex != b.materialIndex)
                      return a.materialIndex < b.materialIndex;
                  return a.meshIndex < b.meshIndex;
              });

    uint32_t running = 0u;
    for (BatchTableEntry& e : table)
    {
        e.recordOffset = running;
        running += e.capacity;
    }
    if (outTotalRecords)
        *outTotalRecords = running;
    return table;
}

std::vector<GPUDrawStreamBuilder::BatchTableEntry>
GPUDrawStreamBuilder::BuildShadowBatchTable(const BatchRegistry& registry,
                                            std::span<const uint8_t> materialDepthClass,
                                            std::span<const uint32_t> meshPoolGroup,
                                            uint32_t* outTotalRecords)
{
    // Map each (materialIndex, meshIndex) batch to a class key: eligible
    // materials collapse into their mesh's single/double-sided sentinel;
    // material-dependent ones keep their real materialIndex. Counts merge per
    // (classKey, meshIndex-with-parity) so a mesh's opaque casters draw once per
    // side. Parity split rule (must match draw_command_scatter.comp's shadow
    // gate): single-sided-eligible and material-dependent casters split into
    // parity-0/parity-1 sibling rows; the double-sided sentinel does NOT split
    // (its casters are cull-None — winding is irrelevant depth-only — and the
    // scatter forces their parity to 0, so a parity-1 row would be dead and a
    // mirrored DS caster searching it would have its shadow silently dropped).
    std::unordered_map<uint64_t, uint32_t> merged;
    merged.reserve(registry.BatchCount());
    for (const auto& [key, count] : registry.Counts())
    {
        const uint32_t materialIndex = BatchRegistry::MaterialIndexFromKey(key);
        const uint32_t meshIndex     = BatchRegistry::MeshIndexFromKey(key);
        assert((meshIndex & kMeshParityBit) == 0u
               && "real meshIndex must be < 2^24 — bit 24 is the winding parity flag");
        // Draw consolidation: the mesh axis is the pool group when the map is
        // active — the parity + class rules below are axis-agnostic.
        const uint32_t meshAxis = TableMeshAxis(meshPoolGroup, meshIndex);
        const MaterialDepthClass cls =
            (materialIndex < materialDepthClass.size())
                ? static_cast<MaterialDepthClass>(materialDepthClass[materialIndex])
                : MaterialDepthClass::MaterialDependent; // unknown -> never merge
        uint32_t classKey;
        bool splitParity;
        switch (cls)
        {
        case MaterialDepthClass::EligibleSingleSided:
            classKey = kSharedDepthSingleSidedSentinel; splitParity = true; break;
        case MaterialDepthClass::EligibleDoubleSided:
            classKey = kSharedDepthDoubleSidedSentinel; splitParity = false; break;
        case MaterialDepthClass::MaterialDependent:
        default:
            classKey = materialIndex; splitParity = true; break;
        }
        if (splitParity)
        {
            if (count.Even > 0u)
                merged[MakeParityMergeKey(classKey, meshAxis)] += count.Even;
            if (count.Odd > 0u)
                merged[MakeParityMergeKey(classKey, meshAxis | kMeshParityBit)] += count.Odd;
        }
        else
        {
            // Double-sided sentinel: both parities collapse into the parity-0 row.
            merged[MakeParityMergeKey(classKey, meshAxis)] += count.Total();
        }
    }

    std::vector<BatchTableEntry> table;
    table.reserve(merged.size());
    for (const auto& [mergedKey, count] : merged)
    {
        BatchTableEntry e{};
        e.materialIndex = ParityMergeClass(mergedKey); // classKey lives in the material field
        e.meshIndex     = ParityMergeMesh(mergedKey);  // real mesh | parity bit 24
        e.capacity      = count;
        table.push_back(e);
    }
    std::sort(table.begin(), table.end(),
              [](const BatchTableEntry& a, const BatchTableEntry& b)
              {
                  if (a.materialIndex != b.materialIndex)
                      return a.materialIndex < b.materialIndex;
                  return a.meshIndex < b.meshIndex;
              });

    uint32_t running = 0u;
    for (BatchTableEntry& e : table)
    {
        e.recordOffset = running;
        running += e.capacity;
    }
    if (outTotalRecords)
        *outTotalRecords = running;
    return table;
}

std::vector<GPUDrawStreamBuilder::BatchTableEntry>
GPUDrawStreamBuilder::BuildColorBatchTable(const BatchRegistry& registry,
                                           std::span<const uint32_t> materialColorClass,
                                           std::span<const uint32_t> meshPoolGroup,
                                           uint32_t* outTotalRecords)
{
    // Remap each (materialIndex, meshIndex) batch to a color-class key through
    // the caller's domain map: opaque casters that share a PSO collapse into
    // one downward-allocated colorClassId (< kColorClassBase); blend /
    // transmissive / material-dependent rows map to themselves (identity) and
    // keep their (materialIndex, mesh) region. Counts merge per (classKey,
    // meshIndex) so a mesh's shared-PSO opaque draws issue once.
    std::unordered_map<uint64_t, uint32_t> merged;
    merged.reserve(registry.BatchCount());
    // Track the two domains for the disjointness invariant (P2-a): a class id
    // that aliases a real materialIndex present in the same table would fold
    // two distinct domains into one row.
    uint32_t maxIdentityKey = 0u;
    bool     anyIdentity    = false;
    uint32_t minClassKey    = 0xFFFFFFFFu;
    bool     anyClass       = false;
    for (const auto& [key, count] : registry.Counts())
    {
        const uint32_t materialIndex = BatchRegistry::MaterialIndexFromKey(key);
        const uint32_t meshIndex     = BatchRegistry::MeshIndexFromKey(key);
        assert((meshIndex & kMeshParityBit) == 0u
               && "real meshIndex must be < 2^24 — bit 24 is the winding parity flag");
        const uint32_t classKey = (materialIndex < materialColorClass.size())
                                      ? materialColorClass[materialIndex]
                                      : materialIndex; // unknown -> identity (never merge)
        if (classKey == materialIndex)
        {
            anyIdentity    = true;
            maxIdentityKey = std::max(maxIdentityKey, classKey);
        }
        else
        {
            anyClass    = true;
            minClassKey = std::min(minClassKey, classKey);
        }
        // Draw consolidation: the mesh axis is the pool group when the map is
        // active (identity rows group across meshes too — the consumer binds
        // per material, and members of a group share every geometry bind).
        const uint32_t meshAxis = TableMeshAxis(meshPoolGroup, meshIndex);
        // Color pass splits every parity (shading flip applies double-sided too).
        if (count.Even > 0u)
            merged[MakeParityMergeKey(classKey, meshAxis)] += count.Even;
        if (count.Odd > 0u)
            merged[MakeParityMergeKey(classKey, meshAxis | kMeshParityBit)] += count.Odd;
    }
    assert((!anyIdentity || !anyClass || maxIdentityKey < minClassKey)
           && "color class-id domain aliases a real materialIndex in the same table "
              "(raise kColorClassBase headroom / see P2-a)");

    std::vector<BatchTableEntry> table;
    table.reserve(merged.size());
    for (const auto& [mergedKey, count] : merged)
    {
        BatchTableEntry e{};
        e.materialIndex = ParityMergeClass(mergedKey); // classKey in the material field
        e.meshIndex     = ParityMergeMesh(mergedKey);  // real mesh | parity bit 24
        e.capacity      = count;
        table.push_back(e);
    }
    std::sort(table.begin(), table.end(),
              [](const BatchTableEntry& a, const BatchTableEntry& b)
              {
                  if (a.materialIndex != b.materialIndex)
                      return a.materialIndex < b.materialIndex;
                  return a.meshIndex < b.meshIndex;
              });

    uint32_t running = 0u;
    for (BatchTableEntry& e : table)
    {
        e.recordOffset = running;
        running += e.capacity;
    }
    if (outTotalRecords)
        *outTotalRecords = running;
    return table;
}

void GPUDrawStreamBuilder::OnInstanceSlotsRecycled(std::vector<uint32_t> recycledSlots)
{
    if (recycledSlots.empty())
        return;
    m_LodFadeResetTracker.OnSlotsRecycled(recycledSlots);
    m_PrevLodResetTracker.OnSlotsRecycled(recycledSlots);
}

GPUDrawStreamBuilder::LodFadeBinding
GPUDrawStreamBuilder::EnsureLodFadeBuffer(uint32_t viewId, uint32_t instanceCount)
{
    LodFadeBinding out{};
    if (!m_Device)
        return out;

    LodFadeView& view = m_LodFade[viewId];
    // Sized for the global instance index the shader addresses (lodFade[i]);
    // grown like GPUCulling's per-view occlusion history, and floored so a
    // near-empty scene still binds a legal range.
    constexpr size_t kMinBytes = 4096;
    const size_t needBytes =
        std::max(kMinBytes, static_cast<size_t>(instanceCount) * kLodFadeStateBytes);
    if (!view.buffer.IsValid() || view.bytes < needBytes)
    {
        if (view.buffer.IsValid())
            RetireBuffer(view.buffer); // may be in flight; freed after kRetireFrameCount
        BufferDesc d{};
        d.size        = needBytes;
        d.usage       = static_cast<uint32_t>(BufferUsage::Storage)
                      | static_cast<uint32_t>(BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::DeviceLocal;
        d.debugName   = "GPUDrawStream.LodFade";
        view.buffer   = m_Device->CreateBuffer(d);
        view.bytes    = view.buffer.IsValid() ? needBytes : 0;
        if (!view.buffer.IsValid())
            return out;
        // The whole-buffer refill below supersedes any pending per-slot resets,
        // so register the view and drop them (this also starts tracking a
        // brand-new view, so later frames' recycled slots accumulate for it).
        out.clearAll = true;
        m_LodFadeResetTracker.OnBufferInitialized(viewId);
    }
    else
    {
        // A recycled slot must be cleared before this view reads it again, or the
        // new tenant inherits the previous tenant's transition. Taken per view so
        // a view idle for a frame still applies the resets it missed.
        m_LodFadeResetTracker.TakeViewPending(viewId, m_LodFadeResetScratch);
        if (!m_LodFadeResetScratch.empty())
        {
            PrevVisibleResetTracker::CoalesceResetRuns(m_LodFadeResetScratch, m_LodFadeResetRuns);
            out.resetRuns = m_LodFadeResetRuns;
        }
    }
    out.handle = view.buffer;
    out.bytes  = view.bytes;
    return out;
}

DescriptorSetLayoutDesc GPUDrawStreamBuilder::MakeScatterDescriptorSetLayout()
{
    // 14 SSBO bindings -- match draw_command_scatter.comp set 0:
    //   0: VisibilityBuffer   (read)        7: ScatterStatsBuffer   (read-write)
    //   1: InstanceBuffer     (read)        8: MaterialColorClass   (read, P2)
    //   2: MeshBuffer         (read)        9: MeshPoolGroup        (read, consolidation)
    //   3: BatchTableBuffer   (read)       10: PrevLodBuffer        (read-write, dwell band)
    //   4: DrawCommandsBuffer (write)      11: LodFadeBuffer        (read-write, crossfade)
    //   5: BatchCursorsBuffer (read-write) 12: PrevRenderedHistory  (read, rendered level/phase)
    //   6: IndirectionBuffer  (write)      13: CurrentRenderedHistory (write, same)
    DescriptorSetLayoutDesc layout{};
    layout.debugName = "GPUDrawScatter_SetLayout";
    for (uint32_t i = 0; i < 14; ++i)
    {
        DescriptorBinding b{};
        b.binding      = i;
        b.type         = DescriptorType::StorageBuffer;
        b.count        = 1;
        b.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(b);
    }
    return layout;
}

GPUDrawStreamBuilder::PrevLodBinding
GPUDrawStreamBuilder::EnsurePrevLodBuffer(uint32_t viewId, uint32_t instanceCount)
{
    PrevLodBinding out{};
    if (!m_Device)
        return out;

    PrevLodView& view = m_PrevLod[viewId];
    const size_t needBytes = std::max(static_cast<size_t>(kPerInstanceHistoryMinBytes),
                                      static_cast<size_t>(instanceCount) * sizeof(uint32_t));
    if (!view.buffer.IsValid() || view.bytes < needBytes)
    {
        // Growing discards the old history rather than copying it: the sentinel
        // refill reads as "no history", so this view pays one frame of the
        // stateless pick and then tracks normally.
        if (view.buffer.IsValid())
            RetireBuffer(view.buffer);

        BufferDesc d{};
        d.size        = needBytes;
        d.usage       = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::DeviceLocal;
        view.debugName = "GPUDrawScatter.PrevLod.V" + std::to_string(viewId);
        d.debugName   = view.debugName.c_str();
        view.buffer   = m_Device->CreateBuffer(d);
        view.bytes    = view.buffer.IsValid() ? needBytes : 0;
        if (!view.buffer.IsValid())
            return out;
        // The whole-buffer sentinel fill supersedes any pending per-slot
        // resets, so register the view and drop them (this also starts
        // tracking a brand-new view, so later frames' recycled slots
        // accumulate for it).
        out.clearAll = true;
        m_PrevLodResetTracker.OnBufferInitialized(viewId);
    }
    else
    {
        // A recycled slot must be refilled with the no-history sentinel before
        // this view reads it again: the dwell rule is idempotent, so a level
        // inherited from the prior tenant inside the band is a PERMANENT fixed
        // point at a still camera, not a one-frame transient. Taken per view
        // so a view idle for a frame still applies the resets it missed.
        m_PrevLodResetTracker.TakeViewPending(viewId, m_PrevLodResetScratch);
        if (!m_PrevLodResetScratch.empty())
        {
            PrevVisibleResetTracker::CoalesceResetRuns(m_PrevLodResetScratch, m_PrevLodResetRuns);
            out.resetRuns = m_PrevLodResetRuns;
        }
    }
    out.handle = view.buffer;
    out.bytes  = view.bytes;
    return out;
}

void GPUDrawStreamBuilder::OnInstanceContinuityBroken(std::vector<uint32_t> changedSlots)
{
    if (changedSlots.empty())
        return;
    m_RenderedHistoryResetTracker.OnSlotsRecycled(changedSlots);
}

GPUDrawStreamBuilder::RenderedHistoryBinding
GPUDrawStreamBuilder::EnsureRenderedHistoryBuffers(uint32_t viewId, uint32_t instanceCount)
{
    RenderedHistoryBinding out{};
    if (!m_Device)
        return out;

    RenderedHistoryView& view = m_RenderedHistory[viewId];
    const size_t needBytes = std::max(static_cast<size_t>(kPerInstanceHistoryMinBytes),
                                      static_cast<size_t>(instanceCount) * sizeof(uint32_t));
    const bool fresh = !view.buffers[0].IsValid() || !view.buffers[1].IsValid()
                       || view.bytes < needBytes;
    if (fresh)
    {
        // Growing discards both halves rather than copying them: the no-history
        // refill costs this view one frame with no previous pair, which is the
        // same price every other invalidation here pays.
        for (uint32_t i = 0; i < 2u; ++i)
        {
            if (view.buffers[i].IsValid())
                RetireBuffer(view.buffers[i]);
            BufferDesc d{};
            d.size        = needBytes;
            d.usage       = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
            d.memoryUsage = BufferMemoryUsage::DeviceLocal;
            view.debugNames[i] =
                "GPUDrawScatter.RenderedHistory.V" + std::to_string(viewId) + "." + std::to_string(i);
            d.debugName    = view.debugNames[i].c_str();
            view.buffers[i] = m_Device->CreateBuffer(d);
        }
        if (!view.buffers[0].IsValid() || !view.buffers[1].IsValid())
        {
            ReleaseRenderedHistory(viewId);
            return out;
        }
        view.bytes        = needBytes;
        view.currentIndex = 0u;
        m_RenderedHistoryViewCount.store(static_cast<uint32_t>(m_RenderedHistory.size()),
                                         std::memory_order_relaxed);
        view.recorded     = std::make_shared<std::atomic<bool>>(false);
        // The whole-buffer refills supersede any pending per-slot resets, so
        // register the view and drop them (this also starts tracking a brand-new
        // view, so later frames' broken slots accumulate for it).
        m_RenderedHistoryResetTracker.OnBufferInitialized(viewId);
        out.clearPreviousAll = true;
        out.clearCurrent     = true;
        view.assignedStamp   = m_ArenaFrameStamp;
    }
    else if (view.assignedStamp != m_ArenaFrameStamp)
    {
        // First call of a new frame for this view. The outgoing current buffer
        // becomes the previous one ONLY if the frame that wrote it reached the
        // scatter pass's exec closure — a frame that was declared and then
        // abandoned recorded nothing, so rotating on it would hand a later
        // frame a "previous" that never reached a pixel. Not rotating leaves
        // the last recorded frame in place as previous and re-clears the
        // current buffer below, which discards the abandoned frame's writes.
        if (view.recorded->load(std::memory_order_acquire))
        {
            view.currentIndex = 1u - view.currentIndex;
            m_RenderedHistoryRotations.fetch_add(1u, std::memory_order_relaxed);
        }
        view.recorded->store(false, std::memory_order_release);
        view.assignedStamp = m_ArenaFrameStamp;
        out.clearCurrent   = true;
        // A slot whose tenant or payload changed must read as no-history before
        // this frame's dispatches compare against it. Taken per view so a view
        // idle for a frame still applies the resets it missed.
        m_RenderedHistoryResetTracker.TakeViewPending(viewId, m_RenderedHistoryResetScratch);
        if (!m_RenderedHistoryResetScratch.empty())
        {
            PrevVisibleResetTracker::CoalesceResetRuns(m_RenderedHistoryResetScratch,
                                                       m_RenderedHistoryResetRuns);
            out.previousResetRuns = m_RenderedHistoryResetRuns;
        }
    }
    // Later calls of the same frame keep the assignment and fill nothing: every
    // dispatch of one frame reads the same previous buffer and writes the same
    // current one.
    out.previous = view.buffers[1u - view.currentIndex];
    out.current  = view.buffers[view.currentIndex];
    out.bytes    = view.bytes;
    out.recorded = view.recorded;
    return out;
}

void GPUDrawStreamBuilder::ReleaseRenderedHistory(uint32_t viewId)
{
    auto it = m_RenderedHistory.find(viewId);
    if (it == m_RenderedHistory.end())
        return;
    for (BufferHandle& handle : it->second.buffers)
        if (handle.IsValid())
            RetireBuffer(handle);
    m_RenderedHistory.erase(it);
    m_RenderedHistoryViewCount.store(static_cast<uint32_t>(m_RenderedHistory.size()),
                                     std::memory_order_relaxed);
}

GPUDrawStreamBuilder::ScatterIntrospection GPUDrawStreamBuilder::IntrospectScatter() const
{
    // Same lock GetOrCreateScatterPipeline writes these members under, so a
    // reader never sees a torn variant latch. Nothing here creates.
    std::lock_guard<std::mutex> lock(m_ScatterInitMutex);
    return ScatterIntrospection{m_ScatterPipeline.IsValid(), m_ScatterCompactActive,
                                m_ScatterStatsActive,
                                m_RenderedHistoryViewCount.load(std::memory_order_relaxed),
                                m_RenderedHistoryRotations.load(std::memory_order_relaxed)};
}

PipelineHandle GPUDrawStreamBuilder::GetOrCreateScatterPipeline()
{
    if (!m_Device)
        return {};

    // A2.4-P0-R: serialize the one-time lazy init. On guard-refused frames the
    // serial spine (ScheduleUnifiedScatter) never pre-warms this, so record
    // workers (diagnostics args on skip paths) can otherwise concurrently run the
    // SPIR-V load + pipeline create and tear the m_Scatter* member writes. Taking
    // the lock unconditionally keeps the valid-check race-free (m_ScatterPipeline
    // is written under this lock); the call is per-pass, not per-draw.
    std::lock_guard<std::mutex> lock(m_ScatterInitMutex);
    if (m_ScatterPipeline.IsValid())
        return m_ScatterPipeline;

    if (!m_ScatterVariantInit)
    {
        m_ScatterCompactRequested = ScatterCompactRequestedFromEnv();
        m_ScatterStatsRequested   = ScatterStatsRequestedFromEnv();
        m_ScatterVariantInit      = true;
    }

    // Prefer the coalesced-fetch package when requested; fall back to the
    // full-fat package (same stats setting) if the compact one is unavailable
    // so the scatter path stays alive (a build/staging gap, not a normal path).
    // The fallback crosses the FETCH axis only — the stats axis is a semantic
    // choice, not a perf degradation, so a missing stats package fails loud
    // rather than silently dropping the requested instrumentation. Missing
    // bytes are NOT latched below — a loader that becomes ready later recovers.
    std::string loadErr;
    std::vector<uint8_t> shaderBytes;
    bool compactLoaded = false;
    if (m_ScatterCompactRequested)
    {
        shaderBytes = LoadComputeStageBytes(ScatterPkgPath(true, m_ScatterStatsRequested),
                                            m_Device->PreferredShaderSource(), s_ShaderLoader, &loadErr);
        compactLoaded = !shaderBytes.empty();
        if (!compactLoaded && !m_ScatterCompactFallbackLogged)
        {
            Logger::Log::Warning("[GPUDrawStream] {} unavailable ({}); using full-fat scatter fetch",
                                 ScatterPkgPath(true, m_ScatterStatsRequested), loadErr);
            m_ScatterCompactFallbackLogged = true;
        }
    }
    if (shaderBytes.empty())
        shaderBytes = LoadComputeStageBytes(ScatterPkgPath(false, m_ScatterStatsRequested),
                                            m_Device->PreferredShaderSource(), s_ShaderLoader, &loadErr);
    if (shaderBytes.empty())
    {
        if (!m_ScatterPipelineFailureLogged)
        {
            Logger::Log::Error("[GPUDrawStream] {} unavailable ({}); scatter path unavailable "
                               "until retry", ScatterPkgPath(false, m_ScatterStatsRequested),
                               loadErr);
            m_ScatterPipelineFailureLogged = true;
        }
        return {};
    }
    m_ScatterCompactActive = compactLoaded;
    m_ScatterStatsActive   = m_ScatterStatsRequested;

    try
    {
        m_ScatterPipeline = BuildScatterPipeline(std::move(shaderBytes));
    }
    catch (const std::exception& e)
    {
        if (!m_ScatterPipelineFailureLogged)
        {
            Logger::Log::Error("[GPUDrawStream] failed to create scatter pipeline: {}", e.what());
            m_ScatterPipelineFailureLogged = true;
        }
        m_ScatterPipeline = PipelineHandle{};
        return {};
    }
    if (m_ScatterPipeline.IsValid())
    {
        m_ScatterPipelineFailureLogged = false;
        Logger::Log::Info("[GPUDrawStream] scatter pipeline ready: {} fetch, stats {}",
                          m_ScatterCompactActive ? "coalesced 32B (compact)" : "full-fat 240B",
                          m_ScatterStatsActive ? "ON" : "off");
    }
    return m_ScatterPipeline;
}

PipelineHandle GPUDrawStreamBuilder::BuildScatterPipeline(std::vector<uint8_t> shaderBytes)
{
    ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(shaderBytes));
    cd.DescriptorSetLayouts.push_back(
        m_Device->InternDescriptorSetLayout(MakeScatterDescriptorSetLayout()));
    static_assert(sizeof(ScatterPushConstants) == 100,
                  "ScatterPushConstants must match draw_command_scatter.comp's block");
    cd.PushConstants.Size      = sizeof(ScatterPushConstants);
    cd.PushConstants.StageMask = kShaderStageCompute;
    cd.DebugName               = "GPUDrawStreamScatter";
    const auto id = m_Device->InternComputePipeline(cd);
    return m_Device->GetOrCreateComputePipeline(id);
}

DescriptorSetLayoutDesc GPUDrawStreamBuilder::MakeGroupCompactDescriptorSetLayout()
{
    // 5 SSBO bindings -- match draw_stream_group_compact.comp set 0:
    //   0: BatchTableBuffer (read)   3: DrawCommandsBuffer (read-write)
    //   1: RowRunBuffer     (read)   4: IndirectionBuffer  (read-write)
    //   2: BatchCursorsBuffer (read-write)
    DescriptorSetLayoutDesc layout{};
    layout.debugName = "GPUDrawGroupCompact_SetLayout";
    for (uint32_t i = 0; i < 5; ++i)
    {
        DescriptorBinding b{};
        b.binding      = i;
        b.type         = DescriptorType::StorageBuffer;
        b.count        = 1;
        b.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(b);
    }
    return layout;
}

PipelineHandle GPUDrawStreamBuilder::GetOrCreateGroupCompactPipeline()
{
    if (!m_Device)
        return {};
    std::lock_guard<std::mutex> lock(m_ScatterInitMutex);
    if (m_GroupCompactPipeline.IsValid())
        return m_GroupCompactPipeline;

    std::string loadErr;
    std::vector<uint8_t> shaderBytes =
        LoadComputeStageBytes("Shaders/draw_stream_group_compact.shaderpkg",
                              m_Device->PreferredShaderSource(), s_ShaderLoader, &loadErr);
    if (shaderBytes.empty())
    {
        if (!m_GroupCompactFailureLogged)
        {
            Logger::Log::Error("[GPUDrawStream] draw_stream_group_compact.shaderpkg unavailable "
                               "({}); consolidated scatter dropped until retry", loadErr);
            m_GroupCompactFailureLogged = true;
        }
        return {};
    }
    try
    {
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(shaderBytes));
        cd.DescriptorSetLayouts.push_back(
            m_Device->InternDescriptorSetLayout(MakeGroupCompactDescriptorSetLayout()));
        static_assert(sizeof(GroupCompactPushConstants) == 28,
                      "GroupCompactPushConstants must match draw_stream_group_compact.comp");
        cd.PushConstants.Size      = sizeof(GroupCompactPushConstants);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName               = "GPUDrawStreamGroupCompact";
        m_GroupCompactPipeline =
            m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(cd));
    }
    catch (const std::exception& e)
    {
        if (!m_GroupCompactFailureLogged)
        {
            Logger::Log::Error("[GPUDrawStream] failed to create group-compact pipeline: {}",
                               e.what());
            m_GroupCompactFailureLogged = true;
        }
        m_GroupCompactPipeline = PipelineHandle{};
        return {};
    }
    if (m_GroupCompactPipeline.IsValid())
        m_GroupCompactFailureLogged = false;
    return m_GroupCompactPipeline;
}

PipelineHandle GPUDrawStreamBuilder::CreateScatterPipelineForVariant(bool compact, bool stats)
{
    if (!m_Device)
        return {};
    std::string loadErr;
    std::vector<uint8_t> shaderBytes = LoadComputeStageBytes(
        ScatterPkgPath(compact, stats), m_Device->PreferredShaderSource(), s_ShaderLoader, &loadErr);
    if (shaderBytes.empty())
        return {};
    try
    {
        return BuildScatterPipeline(std::move(shaderBytes));
    }
    catch (const std::exception&)
    {
        return {};
    }
}

namespace
{
// Initial arena sizing, and only that — the arena is demand-sized at runtime.
// A call claims (slices + tail-capable slices) × blocksPerSlice × live
// instances records, so the multiplier over "slices × instances" is 2 under
// draw consolidation and up to 4 with a live crossfade; the 110k-instance ×
// 5-slice bench therefore asks for ~1.3M, not 550k. 1M records = 20 MB records
// + 4 MB indirection, which is a starting point rather than a budget: a
// schedule call that outgrows it while the frame has claimed nothing resizes in
// place and proceeds, and only a mid-frame trip defers to the next frame. Do
// not treat this constant as a scene-size limit.
constexpr uint32_t kInitialRecordCapacity = 1u << 20;
constexpr uint32_t kInitialCursorCapacity = 1u << 15;
constexpr uint32_t kTableRingBytes        = 1u << 20;
constexpr uint32_t kRetireFrameCount      = IDevice::kMaxSupportedFramesInFlight + 1u;
} // namespace

std::vector<uint32_t> GPUDrawStreamBuilder::BuildSliceCursorOffsets(
    std::span<const uint32_t> tailIndex, uint32_t halfStride, uint32_t* totalOut)
{
    std::vector<uint32_t> offsets(tailIndex.size(), 0u);
    uint32_t end = 0u;
    for (size_t s = 0; s < tailIndex.size(); ++s)
    {
        offsets[s] = end;
        end += (tailIndex[s] != kNoTailBlock) ? halfStride * 2u : halfStride;
    }
    if (totalOut)
        *totalOut = end;
    return offsets;
}

void GPUDrawStreamBuilder::RetireBuffer(BufferHandle handle)
{
    if (handle.IsValid())
        m_RetiredBuffers.push_back({handle, kRetireFrameCount});
}

void GPUDrawStreamBuilder::RetireOutgrownArenaBuffers()
{
    // Drop every buffer a pending capacity has outgrown; the next
    // EnsureArenaBuffers recreates it at the requested size. Retirement (not
    // destruction) is what makes this safe while last frame's consumers are
    // still in flight. Records and indirection always grow as a pair (one uint
    // per record).
    if (m_PendingRecordCapacity > m_RecordCapacity && m_SharedRecords.IsValid())
    {
        Logger::Log::Info("[GPUDrawStream] growing scatter arena: {} -> {} records",
                          m_RecordCapacity, m_PendingRecordCapacity);
        RetireBuffer(m_SharedRecords);
        RetireBuffer(m_SharedIndirection);
        m_SharedRecords            = BufferHandle{};
        m_SharedIndirection        = BufferHandle{};
        m_SharedIndirectionAddress = 0;
        m_RecordCapacity           = 0;
    }
    if (m_PendingCursorCapacity > m_CursorCapacity && m_SharedCursors.IsValid())
    {
        Logger::Log::Info("[GPUDrawStream] growing scatter cursor arena: {} -> {} cursors",
                          m_CursorCapacity, m_PendingCursorCapacity);
        RetireBuffer(m_SharedCursors);
        // The host-visible mirror is sized off the cursor capacity, so it grows
        // with cursors (retire the outgrown copy — an in-flight copy may target it).
        RetireBuffer(m_SharedCursorReadback);
        m_SharedCursors        = BufferHandle{};
        m_SharedCursorReadback = BufferHandle{};
        m_CursorCapacity       = 0;
    }
    // Table ring grows the same way: parity ~2x's a mirror-heavy scene's table
    // bytes. Retire the outgrown ring (an in-flight dispatch may still read it).
    // The monotonic cursor resets in EnsureArenaBuffers (fresh ring).
    if (m_PendingTableRingBytes > m_TableRingBytes && m_TableRing.IsValid())
    {
        Logger::Log::Info("[GPUDrawStream] growing scatter table ring: {} -> {} B",
                          m_TableRingBytes, m_PendingTableRingBytes);
        RetireBuffer(m_TableRing);
        m_TableRing      = BufferHandle{};
        m_TableRingBytes = 0;
    }
}

bool GPUDrawStreamBuilder::EnsureArenaBuffers()
{
    // Each buffer is created only when its handle is invalid, so a growth
    // recreate of records/indirection (or cursors) never clobbers — or leaks —
    // the other still-live handles.
    if (m_SharedRecords.IsValid() && m_SharedCursors.IsValid() && m_SharedIndirection.IsValid()
        && m_SharedStats.IsValid() && m_TableRing.IsValid() && m_SharedCursorReadback.IsValid()
        && m_SharedStatsReadback.IsValid() && m_HistoryStandIn.IsValid())
        return true;
    if (!m_Device)
        return false;

    const uint32_t recordCapacity = std::max(m_PendingRecordCapacity, kInitialRecordCapacity);
    const uint32_t cursorCapacity = std::max(m_PendingCursorCapacity, kInitialCursorCapacity);

    try
    {
        if (!m_SharedRecords.IsValid())
        {
            BufferDesc records{};
            records.size = static_cast<size_t>(recordCapacity) * kDrawCommandStride;
            records.usage = static_cast<uint32_t>(BufferUsage::Storage)
                          | static_cast<uint32_t>(BufferUsage::Indirect)
                          | static_cast<uint32_t>(BufferUsage::TransferDst);
            records.memoryUsage = BufferMemoryUsage::DeviceLocal;
            records.debugName   = "GPUDrawStream.SharedRecords";
            m_SharedRecords     = m_Device->CreateBuffer(records);
            m_RecordCapacity    = recordCapacity;
        }

        // Indirection is sized in lockstep with records (one uint per record);
        // the grow path always retires the pair together.
        if (!m_SharedIndirection.IsValid())
        {
            BufferDesc indirection{};
            indirection.size = static_cast<size_t>(m_RecordCapacity) * sizeof(uint32_t);
            indirection.usage = static_cast<uint32_t>(BufferUsage::Storage);
            // BDA is requested only when the device has it; consumers of the
            // cached address already bail on 0 and take the SSBO path.
            if (m_Device->GetCapabilities().supportsBufferDeviceAddress)
                indirection.usage |= static_cast<uint32_t>(BufferUsage::ShaderDeviceAddress);
            indirection.memoryUsage = BufferMemoryUsage::DeviceLocal;
            indirection.debugName   = "GPUDrawStream.SharedIndirection";
            m_SharedIndirection     = m_Device->CreateBuffer(indirection);
            m_SharedIndirectionAddress = m_Device->GetCapabilities().supportsBufferDeviceAddress
                ? m_Device->GetBufferDeviceAddress(m_SharedIndirection)
                : 0;
        }

        if (!m_SharedCursors.IsValid())
        {
            BufferDesc cursors{};
            cursors.size = static_cast<size_t>(cursorCapacity) * sizeof(uint32_t);
            // TransferSrc lets the end-of-scatter copy mirror the written cursor
            // range into m_SharedCursorReadback for CPU passed-caster reduction.
            cursors.usage = static_cast<uint32_t>(BufferUsage::Storage)
                          | static_cast<uint32_t>(BufferUsage::Indirect)
                          | static_cast<uint32_t>(BufferUsage::TransferSrc)
                          | static_cast<uint32_t>(BufferUsage::TransferDst);
            cursors.memoryUsage = BufferMemoryUsage::DeviceLocal;
            cursors.debugName   = "GPUDrawStream.SharedCursors";
            m_SharedCursors     = m_Device->CreateBuffer(cursors);
            m_CursorCapacity    = cursorCapacity;
        }

        // Host-visible mirror of the cursor arena, sized in lockstep. Recreated
        // by the cursor-growth path (BeginArenaFrame) whenever cursors grow.
        if (!m_SharedCursorReadback.IsValid())
        {
            m_SharedCursorReadback = m_Device->CreateReadbackBuffer(
                static_cast<size_t>(m_CursorCapacity) * sizeof(uint32_t),
                "GPUDrawStream.CursorReadback");
        }

        if (!m_SharedStats.IsValid())
        {
            BufferDesc stats{};
            stats.size  = static_cast<size_t>(kStatsSliceCapacity) * sizeof(ScatterSliceStats);
            // TransferSrc: the per-slice rows are copied into the host-cached
            // m_SharedStatsReadback at the end of the pass (a direct CPU read of
            // this Upload buffer does not reflect the shader's atomic writes).
            stats.usage = static_cast<uint32_t>(BufferUsage::Storage)
                        | static_cast<uint32_t>(BufferUsage::TransferSrc)
                        | static_cast<uint32_t>(BufferUsage::TransferDst);
            stats.memoryUsage = BufferMemoryUsage::Upload; // device atomic target
            stats.debugName   = "GPUDrawStream.ScatterStats";
            m_SharedStats     = m_Device->CreateBuffer(stats);
        }

        // Host-cached readback mirror of the per-slice stats (GPU-write → CPU-read).
        if (!m_SharedStatsReadback.IsValid())
        {
            m_SharedStatsReadback = m_Device->CreateReadbackBuffer(
                static_cast<size_t>(kStatsSliceCapacity) * sizeof(ScatterSliceStats),
                "GPUDrawStream.ScatterStatsReadback");
        }

        if (!m_TableRing.IsValid())
        {
            const uint32_t tableRingBytes = std::max(m_PendingTableRingBytes, kTableRingBytes);
            BufferDesc table{};
            table.size        = tableRingBytes;
            table.usage       = static_cast<uint32_t>(BufferUsage::Storage);
            table.memoryUsage = BufferMemoryUsage::Upload; // CPU-written ring, GPU-read
            table.debugName   = "GPUDrawStream.BatchTableRing";
            m_TableRing       = m_Device->CreateBuffer(table);
            m_TableRingBytes  = tableRingBytes;
            m_TableRingCursor = 0u; // fresh ring — restart the monotonic cursor
        }

        // Never-read stand-in for bindings 10, 12 and 13, so a slice with no
        // dwell band and no rendered history still presents valid descriptors.
        // Created here rather than lazily so it joins the validity gate below —
        // a slice must never bind an invalid handle just because an allocation
        // failed.
        if (!m_HistoryStandIn.IsValid())
        {
            BufferDesc standIn{};
            standIn.size        = kPerInstanceHistoryMinBytes;
            standIn.usage       = static_cast<uint32_t>(BufferUsage::Storage)
                                | static_cast<uint32_t>(BufferUsage::TransferDst);
            standIn.memoryUsage = BufferMemoryUsage::DeviceLocal;
            standIn.debugName   = "GPUDrawStream.HistoryStandIn";
            m_HistoryStandIn    = m_Device->CreateBuffer(standIn);
        }
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("[GPUDrawStream] arena buffer creation failed: {}", e.what());
        return false;
    }

    if (!m_SharedRecords.IsValid() || !m_SharedCursors.IsValid()
        || !m_SharedIndirection.IsValid() || !m_SharedStats.IsValid() || !m_TableRing.IsValid()
        || !m_SharedCursorReadback.IsValid() || !m_SharedStatsReadback.IsValid()
        || !m_HistoryStandIn.IsValid())
        return false;

    m_PendingRecordCapacity = 0;
    m_PendingCursorCapacity = 0;
    m_PendingTableRingBytes = 0;
    return true;
}

void GPUDrawStreamBuilder::BeginArenaFrame()
{
    ++m_ArenaFrameStamp;

    for (auto it = m_RetiredBuffers.begin(); it != m_RetiredBuffers.end();)
    {
        if (--it->framesLeft == 0u)
        {
            if (m_Device && it->handle.IsValid())
                m_Device->DestroyBuffer(it->handle);
            it = m_RetiredBuffers.erase(it);
        }
        else
        {
            ++it;
        }
    }

    RetireOutgrownArenaBuffers();

    // Reduce the just-completed frame's per-slice scatter counters into
    // m_ShadowArcStats (per-cascade passed-casters + drawn-triangles) and return
    // the summed tripwire counters. Reads the host-visible stats array + cursor
    // mirror one-frame-stale; correct only at steady state (the bench warmup
    // protocol guarantees it — see the P0.3 harness). Must run before the
    // per-frame reset below clears m_SliceStatsBindings.
    //
    // Tripwire check. This counts instances the GPU could not route into the
    // captured snapshot (tableMisses) or that overran a batch's snapshot
    // capacity (overflows). Both are SELF-HEALING by construction: a mid-frame
    // second FlushGPUBuffers — HZB phase-B occlusion recovery
    // (ScheduleWorldOcclusionRecoverScatterForView) or the thumbnail seam
    // (ScheduleBucketerDispatchesForView) — makes a later flush's bytes visible
    // to the spine's ALREADY-RECORDED dispatch at GPU-execute time, so a row
    // mutated after this frame's snapshot is read against a table that predates
    // it and one draw is dropped for a single frame (design §3.2 — the
    // worldKey filter covers cross-world mutations; a same-world post-snapshot
    // mutation or a 16-bit worldKey hash collision degrades to this one-frame
    // drop). It fires transiently while GPUScene is streaming (scene load) and
    // clears the moment mutation stops — verified: steady-state counters are 0.
    // So it is a WARNING, rate-limited to the rising edge, NOT an error. A
    // PERSISTENT fire at steady state (no scene load in flight) would instead
    // mean a real registry/table divergence — that is the case worth hunting,
    // with GE_STRICT_SCATTER_TRIPWIRE for a hard stop (DebugFast compiles
    // asserts in, so the assert stays opt-in — it must not crash the
    // daily-driver config on benign load-time content).
    const ScatterStats stats = ReduceShadowArcStats();
    if (stats.tableMisses != 0u || stats.overflows != 0u)
    {
        if (!m_TripwireLatched)
        {
            std::string slices;
            for (const TripwireSliceHit& h : m_LastTripwireSlices)
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s{view=%u cascade=%u phase=%u miss=%u ovf=%u}",
                              slices.empty() ? "" : " ", h.viewId,
                              static_cast<uint32_t>(h.cascadeIndex),
                              static_cast<uint32_t>(h.phase), h.tableMisses, h.overflows);
                slices += buf;
            }
            Logger::Log::Warning(
                "[GPUDrawStream] scatter snapshot seam: tableMisses={} overflows={} — a draw was "
                "dropped for one frame (a mid-frame flush mutated a row after this frame's "
                "snapshot; self-healing, design §3.2). Expected while a scene streams in; "
                "investigate only if it persists at steady state. Offending slices: {}",
                stats.tableMisses, stats.overflows,
                slices.empty() ? "(no per-slice attribution)" : slices.c_str());
            m_TripwireLatched = true;
        }
        static const bool kStrictTripwire =
            std::getenv("GE_STRICT_SCATTER_TRIPWIRE") != nullptr;
        if (kStrictTripwire)
            assert(false && "GPUDrawStream scatter tripwire fired");
    }
    else
    {
        m_TripwireLatched = false; // re-arm once the counters clear
    }

    m_ArenaRecordCursor    = 0;
    m_ArenaCursorCursor    = 0;
    m_ArenaStatsCursor     = 0;
    m_ArenaSkipLogged      = false;
    m_TableRingFrameBytes  = 0;
    m_ClassMapUploaded     = false;
    m_ClassMapRingOffset   = 0;
    m_ClassMapBytes        = 0;
    m_FrameSlices.clear();
    m_RangeMap.clear();
    m_SliceStatsBindings.clear();
    m_UnobservableTailSlices = 0;
}

void GPUDrawStreamBuilder::RegisterSlice(const SliceRegistration& slice)
{
    m_FrameSlices.push_back(slice);
}

GPUDrawStreamBuilder::BatchDrawRange
GPUDrawStreamBuilder::FindBatchDrawRange(uint32_t viewId, uint8_t cascadeIndex,
                                         uint32_t materialIndex, uint32_t meshIndex,
                                         SlicePhase phase) const
{
    const uint64_t key = MakeStreamKey(viewId, cascadeIndex, materialIndex, meshIndex, phase);
    const auto it = m_RangeMap.find(key);
    return (it == m_RangeMap.end()) ? BatchDrawRange{} : it->second;
}

GPUDrawStreamBuilder::RenderedHistoryRead
GPUDrawStreamBuilder::GetRenderedHistoryForRead(uint32_t viewId) const
{
    const auto it = m_RenderedHistory.find(viewId);
    if (it == m_RenderedHistory.end())
        return {};
    const RenderedHistoryView& view = it->second;
    return {view.buffers[view.currentIndex], view.bytes};
}

bool GPUDrawStreamBuilder::HasPublishedRangesForPhase(uint32_t viewId, uint8_t cascadeIndex,
                                                      SlicePhase phase) const
{
    // The key packs the three axes above the material and mesh fields, so one
    // masked comparison answers for every batch of the slice.
    constexpr uint64_t kBatchAxesMask = (1ull << 48) - 1ull;
    const uint64_t wanted = MakeStreamKey(viewId, cascadeIndex, 0u, 0u, phase);
    for (const auto& [key, range] : m_RangeMap)
    {
        if ((key & ~kBatchAxesMask) == wanted && range.IsValid())
            return true;
    }
    return false;
}

GPUDrawStreamBuilder::ScatterStats GPUDrawStreamBuilder::ReadScatterStats() const
{
    // Cached tripwire from the last ReduceShadowArcStats (summed over the real
    // slices). No per-call remap; get_render_stats and the BeginArenaFrame
    // tripwire log share this one value.
    return m_LastTripwire;
}

GPUDrawStreamBuilder::ScatterStats GPUDrawStreamBuilder::ReduceShadowArcStats()
{
    // Per-slice reduction of a completed frame's scatter counters:
    //   drawnTriangles = sliceStats[statsIndex].drawnTriangles (GPU-accumulated)
    //   passedCasters  = Σ cursor mirror entries over the slice's batch range
    //                    (the DrawIndexedIndirectCount counts — no extra atomic)
    // Both read from the host-CACHED readback mirrors the scatter pass copied into
    // (a direct read of the write-combined Upload buffers does not reflect device
    // writes). Tripwire = sum over the real slices only, cached for ReadScatterStats.
    m_ShadowArcStats.clear();
    m_LastTripwire = ScatterStats{};
    m_LastTripwireSlices.clear();
    // m_LiveTail SURVIVES a reduce that produces no rows. What makes the crossfade
    // observation trustworthy is its recomputeCount STAMP, not its age: while the
    // stamp still equals the live count, every dispatch since it was taken had
    // byte-identical inputs and therefore started no transition, so the reading
    // still describes them. Clearing it here instead would let a transient CPU/GPU
    // timing miss — the fence poll below finding the previous frame's graphics work
    // merely not yet signalled — read as "a fade may be live" and hold idle elision
    // off for that frame. The returns below are split accordingly: a TIMING miss
    // retains, a STRUCTURAL one (no readback resources, no rows, unobservable
    // tails) invalidates because the retained reading no longer describes the
    // slices being drawn.
    if (!m_Device || !m_SharedStatsReadback.IsValid() || !m_SharedCursorReadback.IsValid())
    {
        m_LiveTail = LiveTailObservation{};
        return m_LastTripwire;
    }

    // FENCE GATE. The readback mirrors are a SINGLE unfenced buffer overwritten by
    // every frame's scatter copy, but m_SliceStatsBindings describes THIS reduction
    // frame's arena layout. Under frames-in-flight lag the mirror may still hold an
    // OLDER frame's copy under a DIFFERENT layout (a batch add/remove or a
    // face-mask flip shifts every slice's cursorBase), so summing at these offsets
    // would read other slices' entries — a populated slice could read 0 and be
    // wrongly skipped (a missing-shadow flicker the row-absence sentinel can't
    // catch). Only reduce once the previous frame's graphics work (which wrote the
    // mirror) has completed: then the mirror is that frame's copy and matches the
    // bindings by construction. Otherwise leave m_ShadowArcStats empty — every
    // survivor lookup returns unknown, so the early-out records rather than risk a
    // wrong skip. Count the reject for the reject-rate instrumentation the M2a
    // early-out's viability depends on (a ~100% reject ratio == a no-op).
    ++m_ShadowArcReduceAttempts;
    if (!m_Device->IsPreviousFrameGraphicsComplete())
    {
        ++m_ShadowArcStaleRejects;
        return m_LastTripwire; // timing only — m_LiveTail retained
    }

    void* statsMapped = m_Device->MapBuffer(m_SharedStatsReadback);
    if (!statsMapped)
        return m_LastTripwire; // transient map failure — m_LiveTail retained
    const auto* rows = static_cast<const ScatterSliceStats*>(statsMapped);

    if (void* cursorMapped = m_Device->MapBuffer(m_SharedCursorReadback))
    {
        const auto* cursorVals = static_cast<const uint32_t*>(cursorMapped);
        m_ShadowArcStats.reserve(m_SliceStatsBindings.size());
        uint32_t frameTailRecords = 0;
        for (const SliceStatsBinding& b : m_SliceStatsBindings)
        {
            uint32_t passedCasters = 0;
            if (static_cast<size_t>(b.cursorBase) + b.batchCount <= m_CursorCapacity)
                for (uint32_t k = 0; k < b.batchCount; ++k)
                    passedCasters += cursorVals[b.cursorBase + k];
            // Crossfade tail rows, when this slice has them: a fading instance's
            // pair lives there instead of the head, so the head sum alone drops
            // mid-transition instead of rising. Counted on its own as well —
            // whether ANY tail row is populated is the live-fade signal the
            // elision suppression needs, and it is invisible once folded in.
            uint32_t tailRecords = 0;
            if (b.tailCursorBase != 0u
                && static_cast<size_t>(b.tailCursorBase) + b.batchCount <= m_CursorCapacity)
                for (uint32_t k = 0; k < b.batchCount; ++k)
                    tailRecords += cursorVals[b.tailCursorBase + k];
            passedCasters += tailRecords;
            frameTailRecords += tailRecords;

            ShadowArcSliceStat s{};
            s.viewId        = b.viewId;
            s.cascadeIndex  = b.cascadeIndex;
            s.phase         = b.phase;
            s.passedCasters = passedCasters;
            s.tailRecords   = tailRecords;
            if (b.statsIndex < kStatsSliceCapacity)
            {
                const ScatterSliceStats& row = rows[b.statsIndex];
                s.drawnTriangles = row.drawnTriangles;
                s.lodChanges     = row.lodChanges;
                m_LastTripwire.tableMisses += row.tableMisses;
                m_LastTripwire.overflows   += row.overflows;
                if (row.tableMisses != 0u || row.overflows != 0u)
                    m_LastTripwireSlices.push_back(
                        {b.viewId, b.cascadeIndex, b.phase, row.tableMisses, row.overflows});
            }
            m_ShadowArcStats.push_back(s);
        }
        // Stamp the census with the recompute count it covers. The mirror was
        // written by the newest EXECUTED dispatch, whose gate evaluation is
        // already folded into this count; a later dispatch that recomputes will
        // push the count past the stamp, which is how the consumer learns the
        // reading no longer describes the newest dispatch.
        if (!m_ShadowArcStats.empty() && m_UnobservableTailSlices == 0u)
        {
            m_LiveTail.valid          = true;
            m_LiveTail.tailRecords    = frameTailRecords;
            m_LiveTail.recomputeCount = GetScatterRecomputeCount();
        }
        else
        {
            // Structural: no slice reduced, or a tail-capable slice fell past the
            // stats array. Any retained reading describes a slice set that is no
            // longer the one being drawn, so drop it rather than carry it forward.
            m_LiveTail = LiveTailObservation{};
        }
        m_Device->UnmapBuffer(m_SharedCursorReadback);
    }

    m_Device->UnmapBuffer(m_SharedStatsReadback);
    return m_LastTripwire;
}

uint32_t GPUDrawStreamBuilder::LookupShadowSurvivors(const std::vector<ShadowArcSliceStat>& stats,
                                                     uint32_t viewId, uint8_t cascadeIndex,
                                                     SlicePhase phase)
{
    // Linear scan of the reduced per-slice stats (a handful of slices per view —
    // main + up to 4 cascades + area/spot + point faces). A slice absent here
    // (not scheduled last frame, or a stats-row overflow) yields the unknown
    // sentinel so the caller records rather than risk a wrong skip.
    const uint8_t phaseByte = (phase == SlicePhase::B) ? 1u : 0u;
    for (const ShadowArcSliceStat& s : stats)
    {
        if (s.viewId == viewId && s.cascadeIndex == cascadeIndex && s.phase == phaseByte)
            return s.passedCasters;
    }
    return kShadowSurvivorsUnknown;
}

uint32_t GPUDrawStreamBuilder::PreviousFrameShadowSurvivors(uint32_t viewId, uint8_t cascadeIndex,
                                                            SlicePhase phase) const
{
    // The stats reflect the previous app frame's scatter (ReduceShadowArcStats in
    // BeginArenaFrame) — one-frame-stale, correct at steady state.
    return LookupShadowSurvivors(m_ShadowArcStats, viewId, cascadeIndex, phase);
}

RenderGraph::RGBuffer GPUDrawStreamBuilder::ScheduleUnifiedScatter(
    RenderGraph::RGFrame& frame, const char* passNameSuffix, int32_t rgPassPhase,
    RenderGraph::RGBuffer visibility, RenderGraph::RGBuffer instances,
    RenderGraph::RGBuffer scatterHot, RenderGraph::RGBuffer meshes, const BatchRegistry& registry,
    std::span<const uint8_t> materialDepthClass, std::span<const uint32_t> materialColorClass,
    std::span<const uint32_t> meshPoolOrdered, std::span<const uint32_t> orderedToGroup,
    uint32_t instanceCount, uint32_t meshTableCount)
{
    std::vector<SliceRegistration> slices = std::move(m_FrameSlices);
    m_FrameSlices.clear();

    if (slices.empty() || instanceCount == 0u)
        return {};
    if (!GetOrCreateScatterPipeline().IsValid() || !EnsureArenaBuffers())
        return {};
    // Draw consolidation is a paired signal: the per-mesh ordered axis and its
    // rank->group inverse come from ONE MeshPoolGroupPlan refresh.
    assert(meshPoolOrdered.empty() == orderedToGroup.empty()
           && "pass both consolidation spans or neither");
    const bool groupActive = !meshPoolOrdered.empty() && !orderedToGroup.empty();
    // The mesh-major compaction pass is load-bearing under consolidation (the
    // published draw block is ITS output); a missing package drops the call
    // loudly rather than publishing ranges over never-written records.
    if (groupActive && !GetOrCreateGroupCompactPipeline().IsValid())
        return {};
    if (!m_SentinelBuffer.IsValid())
    {
        Logger::Log::Error("[GPUDrawStream] ScheduleUnifiedScatter: ordering sentinel invalid — "
                           "scatter dropped for this frame");
        return {};
    }
    if (!visibility.IsValid() || !instances.IsValid() || !meshes.IsValid())
        return {};

    // Lever 1: bind GPUScene's 32 B coalesced mirror at binding 1 when the
    // compact pipeline variant is active. If it is active but the caller passed
    // no mirror, the env-flag/wiring disagreed — binding the 240 B instance
    // buffer to the compact-struct shader would read garbage, so drop the
    // scatter instead of corrupting every draw (cannot happen with correct
    // wiring: GPUScene reads the same GE_SCATTER_COMPACT and creates the buffer).
    const bool useCompact = m_ScatterCompactActive;
    if (useCompact && !scatterHot.IsValid())
    {
        if (!m_ScatterHotMissingLogged)
        {
            Logger::Log::Error("[GPUDrawStream] compact scatter active but scatter-hot buffer "
                               "invalid — scatter dropped this frame (GE_SCATTER_COMPACT wiring)");
            m_ScatterHotMissingLogged = true;
        }
        return {};
    }
    const RenderGraph::RGBuffer instanceReadRG = useCompact ? scatterHot : instances;

    const bool anyShadowSlice =
        std::any_of(slices.begin(), slices.end(),
                    [](const SliceRegistration& s)
                    { return s.table == SliceTable::Shadow; });

    // Captured snapshot: the caller flushed GPUScene immediately before this
    // call, so the tables match the buffer content this call's dispatches read
    // (design §3.2). Later calls re-snapshot after THEIR flush.
    //
    // Two groupings of ONE registry snapshot: the color table (main-view +
    // depth prepass, keyed (material, mesh)) and — only when a shadow slice is
    // present — the shadow table (keyed (depth-class, mesh)). Each slice binds
    // whichever its table field names. Both total the same record count
    // (Σ capacities == live instances), so a slice consumes snapshotRecords
    // records regardless of which table it uses.
    // Color-class merge (P2) is active when the caller hands a non-empty map;
    // the cascade=None color/depth-prepass table then groups same-PSO opaque
    // casters into their colorClassId. Empty span = merge OFF = today's
    // (material, mesh) table, bit-for-bit.
    // Draw consolidation: a non-empty ORDERED map remaps BOTH tables' mesh
    // axis to per-mesh ranks sorted group-major — rows stay per-mesh (the
    // ranks are unique per live mesh, so the builders' merge maps never
    // collapse them) but a pool group's member rows become CONTIGUOUS, which
    // is what lets the compact pass pack each group region mesh-major and the
    // publisher span member rows with one range. The map itself is uploaded
    // per call below (binding 9) so the GPU search and this table build read
    // one snapshot.
    const bool colorMergeActive = !materialColorClass.empty();
    // The crossfade TAIL is a CALL-level property: it adds a whole record block
    // per slice, so the arena claim and the per-slice block strides must agree
    // across the call. A slice that does not crossfade leaves its tail block
    // unused; the shader's per-slice crossfadeTailActive is what decides whether
    // an instance may claim a tail pair. The tables themselves are UNCHANGED —
    // the tail reuses their per-row offsets against a different block base, so
    // no table byte moves when the feature engages.
    // Only a camera slice (Color table at cascade None) crossfades; a color
    // fan-out slice (probe face) registers with crossfadeDuration 0 anyway.
    const auto sliceCrossfades = [](const SliceRegistration& sl)
    {
        return sl.table == SliceTable::Color && sl.cascadeIndex == kCascadeIndexNone
               && sl.lod.crossfadeDuration > 0.0f && sl.lod.projScaleY > 0.0f
               && sl.lod.forceLod == 0xFFFFFFFFu;
    };
    const bool callCrossfadeActive =
        std::any_of(slices.begin(), slices.end(), sliceCrossfades);

    uint32_t colorRecords = 0u;
    std::vector<BatchTableEntry> colorTable =
        colorMergeActive
            ? BuildColorBatchTable(registry, materialColorClass, meshPoolOrdered, &colorRecords)
            : BuildBatchTable(registry, meshPoolOrdered, &colorRecords);
    if (colorTable.empty() || colorRecords == 0u)
        return {};

    uint32_t shadowRecords = 0u;
    std::vector<BatchTableEntry> shadowTable;
    if (anyShadowSlice)
    {
        shadowTable = BuildShadowBatchTable(registry, materialDepthClass, meshPoolOrdered,
                                            &shadowRecords);
        assert(shadowRecords == colorRecords
               && "shadow table Σ capacities must equal color table's (merging only reduces rows)");
    }

    // Consolidation runs: consecutive table rows sharing (classKey, pool
    // group, parity) — contiguous by the ordered-axis construction. Each run
    // is one published consumer range: its records compact densely from the
    // run's first row's capacity prefix (CPU-known), its live total lands in
    // one per-run count slot, and the per-row {runStart, runIndex} map drives
    // the GPU compact pass (uploaded to the ring beside the table — one
    // snapshot). Empty when consolidation is off.
    struct TableRun
    {
        uint32_t startRow;
        uint32_t capacity; // Σ member row capacities = the range's maxDrawCount
    };
    struct TableRunLayout
    {
        std::vector<TableRun>              runs;
        std::vector<std::array<uint32_t, 2>> rowRun; // per row: {runStart, runIndex}
    };
    auto computeRuns = [&](const std::vector<BatchTableEntry>& table) -> TableRunLayout
    {
        TableRunLayout out;
        if (!groupActive || table.empty())
            return out;
        auto groupOfRow = [&](const BatchTableEntry& e) -> uint32_t
        {
            const uint32_t ordered = e.meshIndex & ~kMeshParityBit;
            return ordered < orderedToGroup.size() ? orderedToGroup[ordered] : kAbsentPoolGroup;
        };
        out.rowRun.resize(table.size());
        uint32_t runStart = 0u;
        for (uint32_t r = 0; r < static_cast<uint32_t>(table.size()); ++r)
        {
            const bool newRun =
                r == 0u
                || table[r].materialIndex != table[runStart].materialIndex
                || ((table[r].meshIndex ^ table[runStart].meshIndex) & kMeshParityBit) != 0u
                || groupOfRow(table[r]) != groupOfRow(table[runStart]);
            if (newRun)
            {
                runStart = r;
                out.runs.push_back({r, 0u});
            }
            out.runs.back().capacity += table[r].capacity;
            out.rowRun[r] = {runStart, static_cast<uint32_t>(out.runs.size() - 1u)};
        }
        return out;
    };
    const TableRunLayout colorRuns  = computeRuns(colorTable);
    const TableRunLayout shadowRuns = computeRuns(shadowTable);

    const uint32_t snapshotRecords  = colorRecords;
    const uint32_t colorBatchCount  = static_cast<uint32_t>(colorTable.size());
    const uint32_t shadowBatchCount = static_cast<uint32_t>(shadowTable.size());
    // One slice's cursor half-block. Shadow B <= color B by construction
    // (merging only reduces rows), so this is colorBatchCount; std::max keeps it
    // robust regardless. Under consolidation each half appends its table's
    // per-RUN count slots after the row cursors — the DrawIndexedIndirectCount
    // counts the compact pass writes (rows keep their scatter cursors for
    // capacity bounds, passed-caster stats, and the compact pass's live sums).
    const uint32_t cursorHalfStride = std::max(
        colorBatchCount + static_cast<uint32_t>(colorRuns.runs.size()),
        shadowBatchCount + static_cast<uint32_t>(shadowRuns.runs.size()));

    const uint32_t sliceCount    = static_cast<uint32_t>(slices.size());
    // Tail blocks belong to the TAIL-CAPABLE slices, not to every slice. Only a
    // camera slice can write a tail record (sliceCrossfades), so a crossfading
    // call's shadow buckets would otherwise each reserve a whole dead tail block
    // — on a 4-cascade scene that is the difference between doubling the claim
    // and raising it by a seventh. The dense index is what the tail record base
    // expressions address, so reservation and addressing cannot drift.
    std::vector<uint32_t> sliceTailIndex(sliceCount, kNoTailBlock);
    uint32_t tailSliceCount = 0u;
    for (uint32_t s = 0; s < sliceCount; ++s)
        if (sliceCrossfades(slices[s]))
            sliceTailIndex[s] = tailSliceCount++;
    assert((tailSliceCount > 0u) == callCrossfadeActive
           && "tail block count and the call-level crossfade flag must agree");

    // Per-slice cursor blocks (see BuildSliceCursorOffsets): every cursor base in
    // this call comes from here, and `s * stride` is no longer an address.
    uint32_t cursorOffsetEnd = 0u;
    const std::vector<uint32_t> sliceCursorOffset =
        BuildSliceCursorOffsets(sliceTailIndex, cursorHalfStride, &cursorOffsetEnd);

    // Record blocks, in arena order, in BLOCK units of snapshotRecords:
    //   [s]                                    head staging (the scatter's output)
    //   [sliceCount + s]                       head draw    (consolidation only)
    //   [blocksPerSlice*sliceCount + t]        tail staging (crossfade only)
    //   [blocksPerSlice*sliceCount + tailSliceCount + t]
    //                                          tail draw    (crossfade AND consolidation)
    // with t = sliceTailIndex[s]. Head blocks come first and keep today's base
    // expressions, so the OFF path claims the same arena bytes at the same
    // offsets as before the tail existed.
    const uint32_t blocksPerSlice = groupActive ? 2u : 1u;
    const uint32_t neededRecords =
        (sliceCount + tailSliceCount) * blocksPerSlice * snapshotRecords;
    const uint32_t neededCursors = cursorOffsetEnd;

    size_t align = m_Device->GetCapabilities().minStorageBufferOffsetAlignment;
    if (align < sizeof(BatchTableEntry))
        align = sizeof(BatchTableEntry);
    const size_t colorTableBytes  = static_cast<size_t>(colorBatchCount) * sizeof(BatchTableEntry);
    const size_t shadowTableBytes = static_cast<size_t>(shadowBatchCount) * sizeof(BatchTableEntry);
    // Everything this call writes into the ring needs its own aligned slot:
    // both tables, plus (under consolidation) the ordered mesh-axis map and
    // the per-row run maps the compact pass reads. Sizing them here keeps the
    // grow-or-skip guard ahead of the ring-wrap headroom fallback.
    const size_t groupRingBytes =
        !groupActive ? 0u
                     : meshPoolOrdered.size() * sizeof(uint32_t)
                           + (colorRuns.rowRun.size() + shadowRuns.rowRun.size()) * 2u
                                 * sizeof(uint32_t)
                           + 3u * align;
    const size_t combinedTableBytes =
        colorTableBytes + shadowTableBytes + groupRingBytes + 2u * align;

    // Schedule-time arena guard (design §8-A2/M2): resize BEFORE any recording —
    // the GPU tripwire cannot prevent an out-of-bounds write. Each exhausted
    // dimension requests its own growth.
    auto arenaExhausted = [&]
    {
        return m_ArenaRecordCursor + neededRecords > m_RecordCapacity
               || m_ArenaCursorCursor + neededCursors > m_CursorCapacity
               || combinedTableBytes > m_TableRingBytes;
    };
    if (arenaExhausted())
    {
        if (m_ArenaRecordCursor + neededRecords > m_RecordCapacity)
        {
            const uint32_t need = m_ArenaRecordCursor + neededRecords;
            m_PendingRecordCapacity = std::max(m_PendingRecordCapacity, need + need / 2u);
        }
        if (m_ArenaCursorCursor + neededCursors > m_CursorCapacity)
        {
            const uint32_t need = m_ArenaCursorCursor + neededCursors;
            m_PendingCursorCapacity = std::max(m_PendingCursorCapacity, need + need / 2u);
        }
        if (combinedTableBytes > m_TableRingBytes)
        {
            // Size for kRetireFrameCount frames of this call's tables so an
            // in-flight dispatch is never lapped, +50% slack. Upload memory, cheap.
            const uint64_t need = static_cast<uint64_t>(combinedTableBytes) * kRetireFrameCount;
            m_PendingTableRingBytes = static_cast<uint32_t>(
                std::min<uint64_t>(std::max<uint64_t>(m_PendingTableRingBytes, need + need / 2u),
                                   0xFFFFFFFFull));
        }
        // Grow NOW when this is the frame's first claim. At a zero arena cursor
        // no call this frame has claimed a range, so no pass of this frame has
        // been recorded against these buffers and no consumer range references
        // them — retiring and recreating here is the same operation
        // BeginArenaFrame performs, one call later. Deferring instead costs the
        // frame a whole pass: an undersized initial arena would present a frame
        // with no world geometry, which no growth policy makes acceptable.
        const bool arenaUntouchedThisFrame =
            m_ArenaRecordCursor == 0u && m_ArenaCursorCursor == 0u && m_TableRingFrameBytes == 0u;
        if (arenaUntouchedThisFrame)
        {
            RetireOutgrownArenaBuffers();
            EnsureArenaBuffers();
        }
        // A mid-frame trip cannot resize (earlier calls this frame hold the
        // current buffers in already-recorded passes), and an allocation failure
        // cannot either: both still skip, loudly, and self-heal next frame.
        if (arenaExhausted())
        {
            if (!m_ArenaSkipLogged)
            {
                Logger::Log::Error("[GPUDrawStream] scatter arena exhausted ({}+{} records of {}, "
                                   "{}+{} cursors of {}, tables {} of {} B); "
                                   "skipping '{}' this frame, growing next frame",
                                   m_ArenaRecordCursor, neededRecords, m_RecordCapacity,
                                   m_ArenaCursorCursor, neededCursors, m_CursorCapacity,
                                   combinedTableBytes, m_TableRingBytes,
                                   passNameSuffix ? passNameSuffix : "");
                m_ArenaSkipLogged = true;
            }
            return {};
        }
    }

    // Claim this call's arena ranges.
    const uint32_t callRecordBase = m_ArenaRecordCursor;
    const uint32_t callCursorBase = m_ArenaCursorCursor;
    m_ArenaRecordCursor += neededRecords;
    m_ArenaCursorCursor += neededCursors;

    // Block-index offsets for the four record blocks (see the layout above).
    // Head blocks index by slice, tail blocks by sliceTailIndex[s]. Without
    // consolidation the compact pass never runs, so staging IS the draw block
    // and the two offsets coincide.
    const uint32_t headDrawBlockOffset    = groupActive ? sliceCount : 0u;
    const uint32_t tailStagingBlockOffset = blocksPerSlice * sliceCount;
    const uint32_t tailDrawBlockOffset =
        groupActive ? tailStagingBlockOffset + tailSliceCount : tailStagingBlockOffset;


    // ── Idle recompute elision (lever #2): byte-record this call's complete
    // dispatch input set; identical for kElisionSettleFrames consecutive
    // frames ⇒ the shared records/cursors already hold exactly what this
    // call's dispatches would write at these same arena offsets — skip the
    // ring uploads and the RG pass, keep the claims + range publication + the
    // stats bindings (the readback mirror still holds the last executed
    // frame's copy under this identical layout, so the reduce stays valid).
    // Deliberate exclusions from the record: per-slot GPUScene buffer handles
    // (they cycle every frame; content identity = ctx.GpuSceneEpoch) and the
    // upload-ring table OFFSETS (inputs to skipped uploads only).
    SuffixElisionGate& elisionGate =
        m_ElisionGates[std::string(passNameSuffix ? passNameSuffix : "")];
    if (m_PendingRecordCapacity > m_RecordCapacity || m_PendingCursorCapacity > m_CursorCapacity ||
        m_PendingTableRingBytes > m_TableRingBytes)
        elisionGate.Gate.ForceRecompute(); // growth applies next BeginArenaFrame — never skip into it
    ElisionInputBlob elisionInputs;
    {
        ElisionInputBlob& b = elisionInputs;
        b.Append(m_ElisionCtx.ContentEpoch);
        b.Append(m_ElisionCtx.GpuSceneEpoch);
        b.Append(m_ElisionCtx.VisibilityWriteEpoch);
        b.Append(rgPassPhase);
        b.Append(instanceCount);
        b.Append(meshTableCount);
        b.Append(colorMergeActive);
        b.Append(groupActive);
        // Sizes the arena claim, the cursor stride and the published tail
        // segments. Derivable from the per-slice durations below, but recorded
        // in its own right so the layout decision can never drift from the gate.
        b.Append(callCrossfadeActive);
        // How many slices own a tail block, which is what places every tail
        // base — a slice set that changes only in WHICH slices crossfade moves
        // the tail blocks without changing any other recorded input.
        b.Append(tailSliceCount);
        b.Append(useCompact);
        b.Append(m_ScatterStatsActive);
        // Arena identity + this call's claimed bases: identical bases are what
        // make the retained record/cursor regions line up with the published
        // ranges below.
        b.Append(m_SharedRecords.id);
        b.Append(m_SharedCursors.id);
        b.Append(m_SharedIndirection.id);
        b.Append(m_SharedStats.id);
        b.Append(m_TableRing.id);
        b.Append(m_SentinelBuffer.id);
        b.Append(m_RecordCapacity);
        b.Append(m_CursorCapacity);
        b.Append(callRecordBase);
        b.Append(callCursorBase);
        b.Append(m_ArenaStatsCursor); // == callStatsBase (claimed just below)
        // Cursor layout: the half-block size plus every slice's own base. The
        // bases are no longer derivable from a single stride, and they are what
        // the retained cursor regions line up against.
        b.Append(cursorHalfStride);
        b.AppendBytes(sliceCursorOffset.data(), sliceCursorOffset.size() * sizeof(uint32_t));
        b.Append(snapshotRecords);
        // Full slice set, field by field (padding never enters the blob).
        b.Append(static_cast<uint32_t>(slices.size()));
        for (const SliceRegistration& s : slices)
        {
            b.Append(s.viewId);
            b.Append(s.cascadeIndex);
            b.Append(static_cast<uint8_t>(s.phase));
            b.Append(static_cast<uint8_t>(s.table));
            b.Append(s.visibilityOffsetBytes);
            b.Append(s.disableVisibilityCheck);
            b.Append(s.viewWorldKey);
            b.Append(s.lod.cameraPos[0]);
            b.Append(s.lod.cameraPos[1]);
            b.Append(s.lod.cameraPos[2]);
            b.Append(s.lod.projScaleY);
            b.Append(s.lod.lodBiasGlobal);
            b.Append(s.lod.forceLod);
            b.Append(s.lod.smallCullCoverage);
            b.Append(s.lod.sseThresholdToCoverage);
            b.Append(s.lod.sseThresholdToCoverageTight);
            // The crossfade DURATION is a dispatch input (it sizes the tables and
            // gates the shader); the frame CLOCK deliberately is not — see
            // SetFrameTimeSeconds.
            b.Append(s.lod.crossfadeDuration);
            b.Append(s.lod.lodHysteresisBand);
        }
        // Both batch tables + the run maps + the routing maps — the exact
        // bytes the dispatches would consume (BatchTableEntry is 4×uint32,
        // padding-free by construction).
        b.Append(static_cast<uint32_t>(colorTable.size()));
        b.AppendBytes(colorTable.data(), colorTable.size() * sizeof(BatchTableEntry));
        b.Append(static_cast<uint32_t>(shadowTable.size()));
        b.AppendBytes(shadowTable.data(), shadowTable.size() * sizeof(BatchTableEntry));
        b.Append(static_cast<uint32_t>(colorRuns.rowRun.size()));
        b.AppendBytes(colorRuns.rowRun.data(),
                      colorRuns.rowRun.size() * sizeof(uint32_t) * 2u);
        b.Append(static_cast<uint32_t>(shadowRuns.rowRun.size()));
        b.AppendBytes(shadowRuns.rowRun.data(),
                      shadowRuns.rowRun.size() * sizeof(uint32_t) * 2u);
        b.Append(static_cast<uint32_t>(materialColorClass.size()));
        b.AppendBytes(materialColorClass.data(), materialColorClass.size() * sizeof(uint32_t));
        b.Append(static_cast<uint32_t>(meshPoolOrdered.size()));
        b.AppendBytes(meshPoolOrdered.data(), meshPoolOrdered.size() * sizeof(uint32_t));
        b.Append(static_cast<uint32_t>(orderedToGroup.size()));
        b.AppendBytes(orderedToGroup.data(), orderedToGroup.size() * sizeof(uint32_t));
    }
    const RecomputeElisionGate::Decision elide =
        elisionGate.Gate.Evaluate(m_ArenaFrameStamp, std::move(elisionInputs),
                                  m_ElisionCtx.AllowElision, kElisionSettleFrames);
    // Settle/unsettle edges are the same opt-in instrument as the window below.
    if (IdleElisionLoggingEnabled() && elide.Skip != elisionGate.LogState)
    {
        if (elide.Skip)
            Logger::Log::Info("[IdleElision] Scatter '{}' engaged: {} slice dispatch(es) elided",
                              passNameSuffix ? passNameSuffix : "", slices.size());
        else
            Logger::Log::Info("[IdleElision] Scatter '{}' disengaged: {}",
                              passNameSuffix ? passNameSuffix : "",
                              ElisionDisengageReason(elide.Cause, elide.Skip));
        elisionGate.LogState = elide.Skip;
    }
    // Periodic cause window per suffix, for windows that lost elision — a run
    // that never engages names its blocker; a settled one stays quiet.
    if (elisionGate.Gate.ShouldReportWindow(600) && IdleElisionLoggingEnabled())
    {
        const RecomputeElisionGate::Stats& st = elisionGate.Gate.GetStats();
        Logger::Log::Info("[IdleElision] Scatter '{}' window: eval {} skip {} | first {} forced {} "
                          "gap {} changed {} unsettled {} | epochs: content {} scene {} viswrite {}",
                          passNameSuffix ? passNameSuffix : "", st.Evaluated, st.Skipped,
                          st.CauseCounts[static_cast<size_t>(ElisionCause::FirstEvaluate)],
                          st.CauseCounts[static_cast<size_t>(ElisionCause::Forced)],
                          st.CauseCounts[static_cast<size_t>(ElisionCause::EvaluationGap)],
                          st.CauseCounts[static_cast<size_t>(ElisionCause::InputsChanged)],
                          st.CauseCounts[static_cast<size_t>(ElisionCause::NotSettled)],
                          m_ElisionCtx.ContentEpoch, m_ElisionCtx.GpuSceneEpoch,
                          m_ElisionCtx.VisibilityWriteEpoch);
    }

    // Per-slice stats rows (P0 shadow-arc instrumentation). Slices past the
    // fixed array capacity share the last scratch row and lose attribution. The
    // draw path is unaffected, but a TAIL-CAPABLE slice losing attribution also
    // loses its crossfade tail from the census, which IS draw-affecting — the
    // range-publish loop below counts those into m_UnobservableTailSlices and the
    // reduce then reports the whole reading unknown rather than a silent zero.
    const uint32_t callStatsBase = m_ArenaStatsCursor;
    m_ArenaStatsCursor += sliceCount;
    if (m_ArenaStatsCursor > kStatsSliceCapacity && !m_StatsSliceOverflowLogged)
    {
        Logger::Log::Warning("[GPUDrawStream] scatter slice stats capacity ({}) exceeded ({} slices); "
                             "per-slice shadow-arc attribution clamped for the overflow",
                             kStatsSliceCapacity, m_ArenaStatsCursor);
        m_StatsSliceOverflowLogged = true;
    }

    // Write both tables into the host-visible ring at aligned offsets. The ring
    // is sized for (frames-in-flight + 1) frames of tables, so the monotonic
    // cursor never laps an in-flight region. On an elided call the uploads are
    // skipped wholesale — the tables are inputs to the skipped dispatches only
    // (range publication below reads the CPU-side vectors, not the ring).
    void* mapped = elide.Skip ? nullptr : m_Device->MapBuffer(m_TableRing);
    if (!mapped && !elide.Skip)
    {
        Logger::Log::Error("[GPUDrawStream] scatter table ring map failed; pass dropped");
        return {};
    }
    auto uploadBytes = [&](const void* src, size_t bytes) -> uint32_t
    {
        if (!mapped)
            return 0u; // elided call: the ring content is an input to skipped dispatches only
        uint32_t offset =
            static_cast<uint32_t>((m_TableRingCursor + align - 1u) / align * align);
        if (offset + bytes > m_TableRingBytes)
            offset = 0u; // wrap
        m_TableRingCursor = offset + static_cast<uint32_t>(bytes);
        if (bytes > 0u)
            std::memcpy(static_cast<uint8_t*>(mapped) + offset, src, bytes);
        // Wrap-headroom guard (review n6): the "monotonic cursor never laps
        // an in-flight region" claim holds only while one frame's total ring
        // bytes × (frames-in-flight + 1) fits the ring. Enforce it instead of
        // assuming it — a lap would mean an in-flight dispatch reading a
        // region we just overwrote.
        m_TableRingFrameBytes += static_cast<uint32_t>(bytes) + static_cast<uint32_t>(align);
        if (m_TableRingFrameBytes * kRetireFrameCount > m_TableRingBytes)
        {
            // Request a larger ring for next frame (self-heals in one frame, like
            // the cursor grow-or-skip). This frame's write already landed, so at
            // worst an in-flight table is stale for one frame — the same
            // one-frame flash the cursor path tolerates, never a silent
            // permanent overwrite.
            const uint64_t need =
                static_cast<uint64_t>(m_TableRingFrameBytes) * kRetireFrameCount;
            m_PendingTableRingBytes = static_cast<uint32_t>(
                std::min<uint64_t>(std::max<uint64_t>(m_PendingTableRingBytes, need + need / 2u),
                                   0xFFFFFFFFull));
            if (!m_ArenaSkipLogged)
            {
                Logger::Log::Error("[GPUDrawStream] table ring headroom exceeded ({} B/frame x {} "
                                   "frames > {} B ring) — growing to {} B next frame",
                                   m_TableRingFrameBytes, kRetireFrameCount, m_TableRingBytes,
                                   m_PendingTableRingBytes);
                m_ArenaSkipLogged = true;
            }
        }
        return offset;
    };
    auto uploadTable = [&](const std::vector<BatchTableEntry>& t) -> uint32_t
    { return uploadBytes(t.data(), t.size() * sizeof(BatchTableEntry)); };
    // Upload THIS call's materialColorClass map into the ring, just ahead of
    // this call's tables (so no within-frame wrap clobbers it). BuildColorBatchTable
    // grouped the table through the SAME span, so binding-8 and the table a slice
    // binds come from one snapshot. Re-uploading per call is what keeps them
    // consistent: the map is NOT frame-invariant — mid-frame material
    // registration bumps MaterialSSBOGeneration, so a later call's rebuilt table
    // no longer matches a map uploaded on the first call (design §3.2, color
    // axis; the stale map made searchMat = staleMap[matIdx] miss the fresh
    // table's classKey row → tableMiss). The map is a few KB, so the extra
    // per-call upload is noise. GE_SCATTER_COLORMAP_PERCALL=0 falls back to the
    // old once-per-frame upload to reproduce the tripwire.
    if (!elide.Skip && colorMergeActive && (m_ColorMapPerCall || !m_ClassMapUploaded))
    {
        m_ClassMapBytes      = static_cast<uint32_t>(materialColorClass.size() * sizeof(uint32_t));
        m_ClassMapRingOffset = uploadBytes(materialColorClass.data(), m_ClassMapBytes);
        m_ClassMapUploaded   = true;
    }
    // Ordered mesh-axis map (binding 9): uploaded per call for the same reason
    // as the color-class map — the tables above were built through THIS span,
    // so the map a dispatch binds must be the same snapshot (a mid-frame mesh
    // registration leaves new meshes absent; a stale map would route them to
    // the absent pseudo row → tableMiss instead of a wrong region). The
    // ordered ranks are additionally per-Refresh data with NO cross-frame
    // stability — per-call upload is what makes that safe.
    uint32_t groupMapRingOffset = 0u;
    uint32_t groupMapBytes      = 0u;
    if (groupActive)
    {
        groupMapBytes      = static_cast<uint32_t>(meshPoolOrdered.size() * sizeof(uint32_t));
        groupMapRingOffset = uploadBytes(meshPoolOrdered.data(), groupMapBytes);
    }
    // Per-row {runStart, runIndex} maps for the compact pass — one snapshot
    // with the tables they describe.
    uint32_t colorRowRunOffset  = 0u;
    uint32_t shadowRowRunOffset = 0u;
    if (groupActive)
    {
        colorRowRunOffset = uploadBytes(colorRuns.rowRun.data(),
                                        colorRuns.rowRun.size() * sizeof(uint32_t) * 2u);
        if (anyShadowSlice && !shadowRuns.rowRun.empty())
            shadowRowRunOffset = uploadBytes(shadowRuns.rowRun.data(),
                                             shadowRuns.rowRun.size() * sizeof(uint32_t) * 2u);
    }
    const uint32_t colorTableOffset  = uploadTable(colorTable);
    const uint32_t shadowTableOffset = anyShadowSlice ? uploadTable(shadowTable) : 0u;
    if (mapped)
        m_Device->UnmapBuffer(m_TableRing);

    // Binding-8 (materialColorClass) source for every dispatch this call. When
    // merging is active every slice binds the uploaded map (color slices read
    // it; shadow/off slices leave it unread but the binding must be valid).
    // Otherwise bind the color table region — a valid, never-read stand-in that
    // avoids any extra upload on the OFF path.
    const uint32_t classBindOffset = colorMergeActive ? m_ClassMapRingOffset : colorTableOffset;
    const uint32_t classBindBytes  =
        colorMergeActive ? m_ClassMapBytes : static_cast<uint32_t>(colorTableBytes);
    // Binding-9 (meshPoolGroup) source: the per-call map when consolidation is
    // active, else the color table region as a valid never-read stand-in (the
    // same pattern binding 8 uses on the merge-OFF path).
    const uint32_t groupBindOffset = groupActive ? groupMapRingOffset : colorTableOffset;
    const uint32_t groupBindBytes =
        groupActive ? groupMapBytes : static_cast<uint32_t>(colorTableBytes);

    // Publish consumer ranges for every slice × batch. A shadow slice
    // (SliceTable::Shadow) publishes from the shadow table — class-0 groups
    // land under MakeStreamKey(view, cascade, sentinel, mesh), material-
    // dependent groups under the real (material, mesh) — everything else uses
    // the color table's (material, mesh) keys. Under consolidation ranges are
    // published per RUN (class × pool group × parity) over the slice's DRAW
    // block: the compact pass packs each run densely from its first row's
    // capacity prefix and writes its live total into the run's count slot.
    for (uint32_t s = 0; s < sliceCount; ++s)
    {
        const SliceRegistration& slice = slices[s];
        const bool sliceIsShadow = slice.table == SliceTable::Shadow;
        const std::vector<BatchTableEntry>& sliceTable = sliceIsShadow ? shadowTable : colorTable;
        const TableRunLayout& sliceRuns = sliceIsShadow ? shadowRuns : colorRuns;
        const uint32_t sliceRecordBase =
            callRecordBase + (headDrawBlockOffset + s) * snapshotRecords;
        const uint32_t sliceCursorBase = callCursorBase + sliceCursorOffset[s];
        // Tail publication runs only where the slice actually has a tail: a
        // shadow bucket in a crossfading call reserves no tail block and never
        // writes a tail record, so publishing one would issue an indirect draw
        // against a permanently zero count slot.
        const bool sliceHasTail = sliceTailIndex[s] != kNoTailBlock;
        const uint32_t tailRecordBase =
            sliceHasTail
                ? callRecordBase + (tailDrawBlockOffset + sliceTailIndex[s]) * snapshotRecords
                : 0u;
        // Only a tail-capable slice reserved the second half; for any other slice
        // base + half is the NEXT slice's block, so it must never be formed.
        const uint32_t tailCursorBase = sliceHasTail ? sliceCursorBase + cursorHalfStride : 0u;

        // Record the per-slice binding so the next BeginArenaFrame can sum this
        // slice's passed-casters (its cursor entries) and read its triangle row.
        // Overflow slices (past the stats array) get no binding — no attribution.
        if (const uint32_t sliceStatsIndex = callStatsBase + s;
            sliceStatsIndex < kStatsSliceCapacity)
        {
            SliceStatsBinding sb{};
            sb.viewId       = slice.viewId;
            sb.cascadeIndex = slice.cascadeIndex;
            sb.phase        = static_cast<uint8_t>(slice.phase);
            sb.statsIndex   = sliceStatsIndex;
            sb.cursorBase   = sliceCursorBase;
            sb.batchCount   = static_cast<uint32_t>(sliceTable.size());
            // Fading instances write their pair to the tail, so the head cursors
            // alone would under-report this slice's records mid-transition. Sum
            // both halves: passed-casters stays "records this slice emitted",
            // which is the quantity the measurement harnesses read.
            sb.tailCursorBase = tailCursorBase;
            m_SliceStatsBindings.push_back(sb);
        }
        else if (sliceHasTail)
        {
            // A tail-capable slice with no binding is a hole in the crossfade
            // census, not just missing attribution: its tail cursors are never
            // summed, so a live fade in this slice would read as a settled zero
            // and elision would freeze its record pair. Make the whole reading
            // honest-unknown instead (see m_UnobservableTailSlices).
            ++m_UnobservableTailSlices;
        }

        if (groupActive)
        {
            // One range per run, keyed by POOL GROUP (the consumers' axis).
            // MakeStreamKey masks the mesh field to 24 bits, so a group's
            // parity-0 run and its parity-1 sibling run collapse to the SAME
            // key — fill the matching segment (runs arrive in table sort
            // order: parity-0 runs of a class precede its parity-1 runs).
            const uint32_t rowCount = static_cast<uint32_t>(sliceTable.size());
            for (uint32_t g = 0; g < static_cast<uint32_t>(sliceRuns.runs.size()); ++g)
            {
                const TableRun&        run   = sliceRuns.runs[g];
                const BatchTableEntry& first = sliceTable[run.startRow];
                const bool     mirroredRun = (first.meshIndex & kMeshParityBit) != 0u;
                const uint32_t ordered     = first.meshIndex & ~kMeshParityBit;
                const uint32_t groupKey    = ordered < orderedToGroup.size()
                                                 ? orderedToGroup[ordered]
                                                 : kAbsentPoolGroup;
                BatchDrawRange& range = m_RangeMap[MakeStreamKey(
                    slice.viewId, slice.cascadeIndex, first.materialIndex, groupKey,
                    slice.phase)];
                range.recordBuffer = m_SharedRecords;
                range.countBuffer  = m_SharedCursors;
                BatchDrawSegment head{};
                head.cmdByteOffset = static_cast<size_t>(sliceRecordBase + first.recordOffset)
                                     * kDrawCommandStride;
                head.countByteOffset =
                    static_cast<size_t>(sliceCursorBase + rowCount + g) * sizeof(uint32_t);
                head.maxDrawCount = run.capacity;
                // The tail block is compacted by a second dispatch of the same
                // pass, so its run layout — bases, run count slots, dense
                // packing — mirrors the head's exactly. run.capacity is a safe
                // bound here (not the paired bound the unconsolidated path
                // needs): the compact pass writes the count slot itself and
                // rounds each row's live tail down to an even count.
                BatchDrawSegment tail{};
                if (sliceHasTail)
                {
                    tail.cmdByteOffset = static_cast<size_t>(tailRecordBase + first.recordOffset)
                                         * kDrawCommandStride;
                    tail.countByteOffset =
                        static_cast<size_t>(tailCursorBase + rowCount + g) * sizeof(uint32_t);
                    tail.maxDrawCount = run.capacity;
                }
                if (mirroredRun)
                {
                    range.mirrored     = head;
                    range.mirroredTail = tail;
                }
                else
                {
                    range.even     = head;
                    range.evenTail = tail;
                }
            }
            continue;
        }

        for (uint32_t b = 0; b < static_cast<uint32_t>(sliceTable.size()); ++b)
        {
            const BatchTableEntry& e = sliceTable[b];
            // MakeStreamKey masks the mesh field to 24 bits, so a parity-0 row
            // and its parity-1 sibling collapse to the SAME range-map key — they
            // MUST merge into one BatchDrawRange (two segments), not overwrite
            // each other. Detect parity from bit 24 and fill the matching
            // segment, preserving the other (rows arrive in sort order, so the
            // parity-0 block precedes the parity-1 block for a material).
            const bool     mirroredRow = (e.meshIndex & kMeshParityBit) != 0u;
            const uint32_t realMesh    = e.meshIndex & ~kMeshParityBit;
            BatchDrawRange& range = m_RangeMap[MakeStreamKey(
                slice.viewId, slice.cascadeIndex, e.materialIndex, realMesh, slice.phase)];
            range.recordBuffer = m_SharedRecords;
            range.countBuffer  = m_SharedCursors;
            BatchDrawSegment head{};
            head.cmdByteOffset = static_cast<size_t>(sliceRecordBase + e.recordOffset)
                                 * kDrawCommandStride;
            head.countByteOffset = static_cast<size_t>(sliceCursorBase + b) * sizeof(uint32_t);
            head.maxDrawCount    = e.capacity;
            // The tail reuses the row's own recordOffset against the tail block
            // base. There is no compact pass here, so the count slot IS the raw
            // cursor and maxDrawCount is the only clamp — it must be the PAIRED
            // bound, or an odd-capacity row whose tail filled would draw one
            // slot the scatter refused to write.
            BatchDrawSegment tail{};
            if (sliceHasTail)
            {
                tail.cmdByteOffset = static_cast<size_t>(tailRecordBase + e.recordOffset)
                                     * kDrawCommandStride;
                tail.countByteOffset =
                    static_cast<size_t>(tailCursorBase + b) * sizeof(uint32_t);
                tail.maxDrawCount = TailDrawBound(e.capacity);
            }
            if (mirroredRow)
            {
                range.mirrored     = head;
                range.mirroredTail = tail;
            }
            else
            {
                range.even     = head;
                range.evenTail = tail;
            }
        }
    }

    const RenderGraph::RGBuffer ordering =
        frame.ImportExternalBuffer("DrawStream.Ordering", m_SentinelBuffer);

    // Elided call: the published ranges above point into the retained
    // record/cursor regions the last executed identical call wrote (same
    // bases by construction — the bases are part of the gate's record).
    // Consumers' Read on the ordering sentinel is a plain first-touch import
    // this frame (no writer): cross-frame memory visibility rides the last
    // executed scatter's global stream-ready barrier + the frame fence.
    if (elide.Skip)
        return ordering;

    // Per-camera-view crossfade state, resolved AFTER the elision early-out: an
    // elided call runs no dispatch, so taking a view's pending resets here would
    // drop them. One entry per slice, index-aligned with `slices`; a slice that
    // does not crossfade leaves it empty and binds the stand-in below.
    std::vector<LodFadeBinding> sliceFade(slices.size());
    if (callCrossfadeActive)
    {
        for (size_t i = 0; i < slices.size(); ++i)
            if (sliceCrossfades(slices[i]))
                sliceFade[i] = EnsureLodFadeBuffer(slices[i].viewId, instanceCount);
    }
    const float frameTimeSeconds = m_FrameTimeSeconds;

    char passName[96];
    std::snprintf(passName, sizeof(passName), "GPUDrawStream.Scatter.%s",
                  passNameSuffix ? passNameSuffix : "");

    // Resolve each slice's dwell-band history buffer BEFORE the pass closure, so
    // the lazy create/grow and the pending-reset take stay on the CPU timeline
    // (the exec closure only fills and binds; an elided call never takes, so
    // its views' pending resets survive to the next dispatching call). A slice
    // gets real history only when it asked for a band, the env kill-switch is
    // off, and it is a CAMERA slice that actually auto-selects; everything else
    // binds the never-read stand-in so binding 10 is always valid. Bands are
    // zeroed in lockstep with the binding choice, so the shader can never read
    // the stand-in as if it were history.
    std::vector<PrevLodBinding> slicePrevLod(slices.size());
    std::vector<float>          sliceBand(slices.size(), 0.0f);
    for (size_t s = 0; s < slices.size(); ++s)
    {
        const SliceRegistration& slice = slices[s];
        const bool wantsBand = m_LodHysteresisEnabled && slice.lod.lodHysteresisBand > 0.0f
                              && slice.table == SliceTable::Color
                              && slice.cascadeIndex == kCascadeIndexNone
                              && slice.lod.projScaleY > 0.0f
                              && slice.lod.forceLod == 0xFFFFFFFFu;
        if (wantsBand)
        {
            PrevLodBinding binding = EnsurePrevLodBuffer(slice.viewId, instanceCount);
            if (binding.handle.IsValid())
            {
                slicePrevLod[s] = std::move(binding);
                sliceBand[s]    = slice.lod.lodHysteresisBand;
            }
        }
    }

    // Resolve each slice's rendered level and phase pair, on the same CPU
    // timeline and under the same camera-slice test as the dwell band — a
    // shadow bucket has no motion consumer and a slice that forces a level
    // selects nothing to record. The pair is INDEPENDENT of the band: it is
    // requested per view by a consumer that needs the previous rendered frame,
    // whether or not the project enabled hysteresis, and it leaves binding 10
    // untouched so a band-carrying view behaves exactly as it does today.
    // Views that stop asking release their pair here rather than holding it for
    // the process lifetime. Slices that get no pair bind the stand-in at both
    // 12 and 13 with the push-constant gate at 0, so the shader reads neither.
    const auto isCameraSlice = [](const SliceRegistration& slice)
    {
        return slice.table == SliceTable::Color && slice.cascadeIndex == kCascadeIndexNone
               && slice.lod.projScaleY > 0.0f && slice.lod.forceLod == 0xFFFFFFFFu;
    };
    std::vector<RenderedHistoryBinding> sliceRenderedHistory(slices.size());
    for (size_t s = 0; s < slices.size(); ++s)
    {
        const SliceRegistration& slice = slices[s];
        if (!isCameraSlice(slice) || !slice.lod.renderedLevelHistoryRequested)
            continue;
        RenderedHistoryBinding binding =
            EnsureRenderedHistoryBuffers(slice.viewId, instanceCount);
        if (binding.previous.IsValid() && binding.current.IsValid())
            sliceRenderedHistory[s] = std::move(binding);
    }
    // The request travels per slice and a view's culling phases register
    // several, so a view keeps its pair while any camera slice of this frame
    // asks for it: a pair assigned this frame — by this call or an earlier one —
    // is in use, and only a pair no slice of the frame has claimed is retired.
    // Releasing on the first non-asking slice would retire a pair a sibling
    // slice had just bound, or recreate it every frame.
    for (const SliceRegistration& slice : slices)
    {
        if (!isCameraSlice(slice) || slice.lod.renderedLevelHistoryRequested)
            continue;
        const auto it = m_RenderedHistory.find(slice.viewId);
        if (it != m_RenderedHistory.end() && it->second.assignedStamp != m_ArenaFrameStamp)
            ReleaseRenderedHistory(slice.viewId);
    }

    frame.AddPass(
        passName, rgPassPhase,
        [&](RenderGraph::RGPassBuilder& p)
        {
            // Same two-mechanism contract as the per-bucket pass: RAW edges
            // via visibility + the ordering sentinel; memory visibility for
            // the graph-undeclared shared buffers via the end-of-exec
            // stream-ready barrier. The CopyDst write covers the fills.
            p.Read(visibility);
            p.Read(instanceReadRG);
            p.Read(meshes);
            p.Write(ordering);
            p.Write(ordering, RenderGraph::RGBufferWrite::CopyDst);
        },
        [slices = std::move(slices), pipeline = m_ScatterPipeline, records = m_SharedRecords,
         cursors = m_SharedCursors, indirection = m_SharedIndirection, statsBuf = m_SharedStats,
         cursorReadback = m_SharedCursorReadback, statsReadback = m_SharedStatsReadback,
         tableRing = m_TableRing, colorTableOffset,
         colorTableBytes, colorBatchCount,
         shadowTableOffset, shadowTableBytes, shadowBatchCount, callRecordBase, callCursorBase,
         callStatsBase, snapshotRecords, cursorHalfStride, headDrawBlockOffset,
         tailStagingBlockOffset, tailDrawBlockOffset, sliceTailIndex = std::move(sliceTailIndex),
         sliceCursorOffset,
         instanceCount, meshTableCount, neededRecords,
         neededCursors, classBindOffset, classBindBytes, colorMergeActive,
         groupBindOffset, groupBindBytes, groupActive,
         compactPipeline = m_GroupCompactPipeline, colorRowRunOffset, shadowRowRunOffset,
         sliceFade = std::move(sliceFade), frameTimeSeconds,
         visibility, instanceReadRG, instanceStride = (useCompact ? kScatterHotStride : sizeof(GPUInstance)),
         slicePrevLod = std::move(slicePrevLod), sliceBand = std::move(sliceBand),
         sliceRenderedHistory = std::move(sliceRenderedHistory),
         historyStandIn = m_HistoryStandIn,
         meshes](RenderGraph::RGContext& ctx)
        {
            IDevice* device = ctx.GetDevice();
            BufferHandle visBuf  = ctx.GetBuffer(visibility);
            BufferHandle instBuf = ctx.GetBuffer(instanceReadRG);
            BufferHandle meshBuf = ctx.GetBuffer(meshes);

            // Zero this call's cursor range; on count-emulating backends also
            // zero this call's record range (stale records replay as ghost
            // draws there — design §3.4). Stats rows are zeroed PER CALL over
            // this call's own row range (not once per app frame by the first
            // call): under idle elision the frame's first call may skip its
            // pass entirely, and a later dispatching call must not accumulate
            // onto rows a zero-fill never touched. Rows outside any call's
            // range go unread (the reduce walks this frame's bindings only).
            ctx.Cmd->FillBuffer(cursors,
                                static_cast<size_t>(callCursorBase) * sizeof(uint32_t),
                                static_cast<size_t>(neededCursors) * sizeof(uint32_t), 0u);
            if (callStatsBase < kStatsSliceCapacity)
            {
                const uint32_t statsRows = std::min(static_cast<uint32_t>(slices.size()),
                                                    kStatsSliceCapacity - callStatsBase);
                ctx.Cmd->FillBuffer(statsBuf,
                                    static_cast<size_t>(callStatsBase) * sizeof(ScatterSliceStats),
                                    static_cast<size_t>(statsRows) * sizeof(ScatterSliceStats), 0u);
            }
            if (!device->GetCapabilities().supportsDrawIndirectCountNative)
            {
                ctx.Cmd->FillBuffer(records,
                                    static_cast<size_t>(callRecordBase) * kDrawCommandStride,
                                    static_cast<size_t>(neededRecords) * kDrawCommandStride, 0u);
            }
            // Crossfade state invalidation, inside the same transfer block the
            // barrier below covers: a fresh or grown per-view buffer fills
            // whole, an established one fills only the slots GPUScene recycled
            // since this view last ran. Both are empty in the steady state.
            //
            // Filled with the no-history sentinel, not zero. Zero is a REAL
            // state (settled, displaying LOD0), so a slot zeroed here whose
            // first pick was any other level read as a transition out of LOD0
            // and dissolved in from geometry it had never displayed — on its
            // first dispatch, as a tail pair with no head record. The sentinel
            // makes the first pick the settled state instead.
            for (const LodFadeBinding& fade : sliceFade)
            {
                if (!fade.handle.IsValid())
                    continue;
                if (fade.clearAll)
                {
                    ctx.Cmd->FillBuffer(fade.handle, 0, fade.bytes, kLodNoHistory);
                    continue;
                }
                for (const auto& [startElement, countElements] : fade.resetRuns)
                {
                    const size_t offset =
                        static_cast<size_t>(startElement) * kLodFadeStateBytes;
                    if (offset >= fade.bytes)
                        continue; // trimmed past the buffer; the next grow re-zeroes it
                    const size_t bytes =
                        std::min(static_cast<size_t>(countElements) * kLodFadeStateBytes,
                                 fade.bytes - offset);
                    ctx.Cmd->FillBuffer(fade.handle, offset, bytes, kLodNoHistory);
                }
            }
            // Dwell-band history invalidation, same contract as the crossfade
            // loop above but refilled with the no-history sentinel: a fresh or
            // grown per-view buffer fills whole (every slot takes the stateless
            // pick for one frame), an established one refills only the slots
            // GPUScene recycled since this view last ran. The dwell rule is
            // idempotent, so without the refill a recycled slot's new tenant
            // holds the prior tenant's level for as long as the camera is
            // still. Declared here so the Transfer->Compute barrier below
            // covers it.
            for (const PrevLodBinding& prev : slicePrevLod)
            {
                if (!prev.handle.IsValid())
                    continue;
                if (prev.clearAll)
                {
                    ctx.Cmd->FillBuffer(prev.handle, 0, prev.bytes, kLodNoHistory);
                    continue;
                }
                for (const auto& [startElement, countElements] : prev.resetRuns)
                {
                    const size_t offset = static_cast<size_t>(startElement) * sizeof(uint32_t);
                    if (offset >= prev.bytes)
                        continue; // trimmed past the buffer; the next grow refills it
                    const size_t bytes =
                        std::min(static_cast<size_t>(countElements) * sizeof(uint32_t),
                                 prev.bytes - offset);
                    ctx.Cmd->FillBuffer(prev.handle, offset, bytes, kLodNoHistory);
                }
            }
            // Rendered level and phase history. The CURRENT buffer is cleared
            // whole once per frame, so an instance that claims no record this
            // frame (culled, table-missed, or in a view that did not dispatch)
            // leaves the no-history value and reads as discontinuous when it
            // reappears — frame eligibility, decided by absence rather than by
            // a second signal. The PREVIOUS buffer is refilled where a slot's
            // tenant or payload changed since this view last ran, which is how
            // the processor-side continuity stamp reaches the device. Both are
            // declared here so the Transfer->Compute barrier below covers them,
            // and the flags are false on every call after the frame's first.
            for (const RenderedHistoryBinding& history : sliceRenderedHistory)
            {
                if (!history.current.IsValid())
                    continue;
                if (history.clearCurrent)
                    ctx.Cmd->FillBuffer(history.current, 0, history.bytes, kLodNoHistory);
                if (history.clearPreviousAll)
                {
                    ctx.Cmd->FillBuffer(history.previous, 0, history.bytes, kLodNoHistory);
                    continue;
                }
                for (const auto& [startElement, countElements] : history.previousResetRuns)
                {
                    const size_t offset = static_cast<size_t>(startElement) * sizeof(uint32_t);
                    if (offset >= history.bytes)
                        continue; // trimmed past the buffer; the next grow refills it
                    const size_t bytes =
                        std::min(static_cast<size_t>(countElements) * sizeof(uint32_t),
                                 history.bytes - offset);
                    ctx.Cmd->FillBuffer(history.previous, offset, bytes, kLodNoHistory);
                }
            }
            ctx.Cmd->Barrier(ResourceBarrier::CreateMemoryBarrier(
                static_cast<uint64_t>(PipelineStageMask::Transfer),
                static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                static_cast<uint64_t>(ResourceAccessMask::TransferWrite),
                static_cast<uint64_t>(ResourceAccessMask::ShaderRead)
                    | static_cast<uint64_t>(ResourceAccessMask::ShaderWrite)));

            ctx.Cmd->SetPipeline(pipeline);

            const size_t visibilityReadBytes =
                static_cast<size_t>(instanceCount) * sizeof(uint32_t);
            // 240 B full-fat GPUInstance, or the 32 B coalesced mirror (lever 1).
            const size_t instanceReadBytes =
                static_cast<size_t>(instanceCount) * instanceStride;
            const size_t meshReadBytes =
                static_cast<size_t>(std::max(meshTableCount, 1u)) * sizeof(GPUMesh);

            const uint32_t groupCount = (instanceCount + 63u) / 64u;
            for (uint32_t s = 0; s < static_cast<uint32_t>(slices.size()); ++s)
            {
                const SliceRegistration& slice = slices[s];
                const bool sliceIsShadow = slice.table == SliceTable::Shadow;
                // Shadow slices bind the shadow table + route on the depth
                // class (classMode == Shadow); color/main-view + depth prepass
                // bind the color table + route by materialIndex/colorClassId.
                const uint32_t sliceTableOffset = sliceIsShadow ? shadowTableOffset : colorTableOffset;
                const size_t   sliceTableBytes  = sliceIsShadow ? shadowTableBytes : colorTableBytes;
                const uint32_t sliceBatchCount  = sliceIsShadow ? shadowBatchCount : colorBatchCount;
                if (sliceBatchCount == 0u || sliceTableBytes == 0u)
                    continue; // shadow table absent (no eligible/dependent rows) — nothing to route

                DescriptorSetDesc setDesc{};
                setDesc.layout    = MakeScatterDescriptorSetLayout();
                setDesc.transient = true;
                setDesc.debugName = "GPUDrawScatter_DS0";
                DescriptorSetHandle setHandle = device->CreateDescriptorSet(setDesc);
                if (!setHandle.IsValid())
                    continue;

                device->UpdateStorageBufferBinding(setHandle, 0, visBuf,
                                                   slice.visibilityOffsetBytes,
                                                   visibilityReadBytes);
                device->UpdateStorageBufferBinding(setHandle, 1, instBuf, 0, instanceReadBytes);
                device->UpdateStorageBufferBinding(setHandle, 2, meshBuf, 0, meshReadBytes);
                device->UpdateStorageBufferBinding(setHandle, 3, tableRing, sliceTableOffset,
                                                   sliceTableBytes);
                device->UpdateStorageBufferBinding(setHandle, 4, records, 0,
                                                   static_cast<size_t>(callRecordBase + neededRecords)
                                                       * kDrawCommandStride);
                device->UpdateStorageBufferBinding(setHandle, 5, cursors, 0,
                                                   static_cast<size_t>(callCursorBase + neededCursors)
                                                       * sizeof(uint32_t));
                device->UpdateStorageBufferBinding(setHandle, 6, indirection, 0,
                                                   static_cast<size_t>(callRecordBase + neededRecords)
                                                       * sizeof(uint32_t));
                device->UpdateStorageBufferBinding(
                    setHandle, 7, statsBuf, 0,
                    static_cast<size_t>(kStatsSliceCapacity) * sizeof(ScatterSliceStats));
                // materialColorClass (P2). Bound on every dispatch so the 9th
                // layout binding is always valid; only color slices with merge
                // active actually read it (classMode == Color).
                device->UpdateStorageBufferBinding(setHandle, 8, tableRing, classBindOffset,
                                                   classBindBytes);
                // meshPoolGroup (draw consolidation). Bound on every dispatch so
                // the 10th layout binding is always valid; read only when
                // meshGroupMode != 0.
                device->UpdateStorageBufferBinding(setHandle, 9, tableRing, groupBindOffset,
                                                   groupBindBytes);
                // prevLod (dwell band). Either this view's persistent history or
                // the never-read stand-in; sliceBand[s] is 0 in the latter case,
                // which is what stops the shader touching it.
                const PrevLodBinding& prevBind = slicePrevLod[s];
                if (prevBind.handle.IsValid())
                    device->UpdateStorageBufferBinding(
                        setHandle, 10, prevBind.handle, 0,
                        static_cast<size_t>(instanceCount) * sizeof(uint32_t));
                else
                    device->UpdateStorageBufferBinding(setHandle, 10, historyStandIn, 0,
                                                       static_cast<size_t>(kPerInstanceHistoryMinBytes));
                // lodFade (crossfade state). A crossfading slice binds its view's
                // persistent buffer; every other slice binds the stand-in, which
                // is legal precisely because ge_UpdateLodFade returns before its
                // first read when crossfadeTailActive is 0.
                const LodFadeBinding& fade = sliceFade[s];
                if (fade.handle.IsValid())
                    device->UpdateStorageBufferBinding(setHandle, 11, fade.handle, 0, fade.bytes);
                else
                    device->UpdateStorageBufferBinding(setHandle, 11, tableRing, colorTableOffset,
                                                       colorTableBytes);
                // prevRendered / currentRendered. Either this view's rotated
                // pair or the never-read stand-in at both; renderedHistoryEnabled
                // below moves in lockstep with the choice, so the shader can
                // never read or write the stand-in.
                const RenderedHistoryBinding& history = sliceRenderedHistory[s];
                const bool historyBound =
                    history.previous.IsValid() && history.current.IsValid();
                if (historyBound)
                {
                    device->UpdateStorageBufferBinding(setHandle, 12, history.previous, 0,
                                                       history.bytes);
                    device->UpdateStorageBufferBinding(setHandle, 13, history.current, 0,
                                                       history.bytes);
                }
                else
                {
                    device->UpdateStorageBufferBinding(
                        setHandle, 12, historyStandIn, 0,
                        static_cast<size_t>(kPerInstanceHistoryMinBytes));
                    device->UpdateStorageBufferBinding(
                        setHandle, 13, historyStandIn, 0,
                        static_cast<size_t>(kPerInstanceHistoryMinBytes));
                }

                ScatterPushConstants pc{};
                pc.instanceCount   = instanceCount; // call-captured
                pc.batchCount      = sliceBatchCount;                 // this slice's table row count
                pc.cursorBase      = callCursorBase + sliceCursorOffset[s];
                pc.recordBase      = callRecordBase + s * snapshotRecords;
                pc.disableVisCheck = slice.disableVisibilityCheck ? 1u : 0u;
                pc.viewWorldKey    = slice.viewWorldKey;
                pc.cameraPosX      = slice.lod.cameraPos[0];
                pc.cameraPosY      = slice.lod.cameraPos[1];
                pc.cameraPosZ      = slice.lod.cameraPos[2];
                pc.projScaleY      = slice.lod.projScaleY;
                pc.lodBiasGlobal   = slice.lod.lodBiasGlobal;
                pc.forceLod        = slice.lod.forceLod;
                // Shadow slices always route on the R1.5 depth class; color /
                // depth-prepass slices route on the color class only when the
                // merge is active (otherwise raw materialIndex, today's path).
                pc.classMode = sliceIsShadow ? kClassModeShadow
                                             : (colorMergeActive ? kClassModeColor : kClassModeOff);
                // Per-slice stats row (clamped to the scratch row on overflow —
                // matches the no-binding path in the range-publish loop above).
                pc.statsBase = std::min(callStatsBase + s, kStatsSliceCapacity - 1u);
                // Draw consolidation: every slice of a call routes its mesh axis
                // the same way its tables were built — grouped or per-mesh.
                pc.meshGroupMode = groupActive ? 1u : 0u;
                pc.smallCullCoverage = slice.lod.smallCullCoverage;
                pc.sseThresholdToCoverage      = slice.lod.sseThresholdToCoverage;
                pc.sseThresholdToCoverageTight = slice.lod.sseThresholdToCoverageTight;
                // Crossfade: the tail is armed ONLY where a real per-view state
                // buffer is bound, so the shader can never read the stand-in at
                // binding 11 nor claim a tail slot the arena did not reserve.
                pc.nowSeconds = frameTimeSeconds;
                if (fade.handle.IsValid() && sliceTailIndex[s] != kNoTailBlock)
                {
                    pc.invFadeDuration      = 1.0f / slice.lod.crossfadeDuration;
                    pc.crossfadeTailActive  = 1u;
                    pc.tailCursorBase       = pc.cursorBase + cursorHalfStride;
                    pc.tailRecordBase =
                        callRecordBase
                        + (tailStagingBlockOffset + sliceTailIndex[s]) * snapshotRecords;
                }
                pc.lodHysteresisBand = sliceBand[s];
                pc.renderedHistoryEnabled = historyBound ? 1u : 0u;
                ctx.Cmd->SetPushConstants(pc);

                ctx.Cmd->BindDescriptorSet(0, setHandle, pipeline);
                ctx.Cmd->Dispatch(groupCount, 1, 1);
            }

            // This frame's current rendered-history buffer is now a faithful
            // record of what this view rendered: the fill above cleared it and
            // the dispatches above wrote the instances that claimed a record —
            // none, if this call routed no slice, which is itself the truth that
            // nothing rendered. It may therefore become the next frame's
            // previous. The flag is set HERE and nowhere else because this
            // closure runs only from RGFrame::Execute: a frame that was declared
            // and then abandoned, or whose pass the graph culled, never reaches
            // it, and the next frame re-clears the same buffer and keeps the
            // last recorded frame as previous.
            for (const RenderedHistoryBinding& history : sliceRenderedHistory)
                if (history.recorded)
                    history.recorded->store(true, std::memory_order_release);

            // Mesh-coherent group compaction (draw consolidation): pack each
            // slice's live records from its staging block into its draw block,
            // mesh-major per (class, group, parity) run, and write per-run
            // counts. One barrier makes every slice's scatter output (records,
            // cursors) visible; the stream-ready barrier at the end covers the
            // compact writes for the indirect/vertex consumers.
            if (groupActive && compactPipeline.IsValid())
            {
                ctx.Cmd->Barrier(ResourceBarrier::CreateMemoryBarrier(
                    static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                    static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                    static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
                    static_cast<uint64_t>(ResourceAccessMask::ShaderRead)
                        | static_cast<uint64_t>(ResourceAccessMask::ShaderWrite)));

                ctx.Cmd->SetPipeline(compactPipeline);
                const uint32_t compactGroups = (snapshotRecords + 63u) / 64u;
                const uint32_t sliceCount    = static_cast<uint32_t>(slices.size());
                for (uint32_t s = 0; s < sliceCount; ++s)
                {
                    const bool sliceIsShadow =
                        slices[s].table == SliceTable::Shadow;
                    const uint32_t sliceTableOffset =
                        sliceIsShadow ? shadowTableOffset : colorTableOffset;
                    const size_t   sliceTableBytes =
                        sliceIsShadow ? shadowTableBytes : colorTableBytes;
                    const uint32_t sliceRowCount =
                        sliceIsShadow ? shadowBatchCount : colorBatchCount;
                    const uint32_t sliceRowRunOffset =
                        sliceIsShadow ? shadowRowRunOffset : colorRowRunOffset;
                    if (sliceRowCount == 0u || sliceTableBytes == 0u)
                        continue; // matches the skipped scatter dispatch above

                    // ONE descriptor set per slice: every binding below is
                    // region-invariant (head and tail differ only in push
                    // constants), and a set-creation failure must skip the
                    // WHOLE slice — compacting the tail without the head would
                    // draw only the fading pairs of an otherwise-empty slice.
                    DescriptorSetDesc setDesc{};
                    setDesc.layout    = MakeGroupCompactDescriptorSetLayout();
                    setDesc.transient = true;
                    setDesc.debugName = "GPUDrawGroupCompact_DS0";
                    DescriptorSetHandle setHandle = device->CreateDescriptorSet(setDesc);
                    if (!setHandle.IsValid())
                        continue;

                    device->UpdateStorageBufferBinding(setHandle, 0, tableRing,
                                                       sliceTableOffset, sliceTableBytes);
                    device->UpdateStorageBufferBinding(
                        setHandle, 1, tableRing, sliceRowRunOffset,
                        static_cast<size_t>(sliceRowCount) * 2u * sizeof(uint32_t));
                    device->UpdateStorageBufferBinding(
                        setHandle, 2, cursors, 0,
                        static_cast<size_t>(callCursorBase + neededCursors) * sizeof(uint32_t));
                    device->UpdateStorageBufferBinding(
                        setHandle, 3, records, 0,
                        static_cast<size_t>(callRecordBase + neededRecords) * kDrawCommandStride);
                    device->UpdateStorageBufferBinding(
                        setHandle, 4, indirection, 0,
                        static_cast<size_t>(callRecordBase + neededRecords) * sizeof(uint32_t));

                    // The crossfade tail block has the same row layout as the
                    // head (same table, same recordOffsets, same runs), so it
                    // compacts with the SAME shader — one more dispatch aimed at
                    // the tail cursors, tail staging and tail draw block. Its
                    // records are claimed in pairs, which is the one thing the
                    // shader has to be told (pairedRecords).
                    const bool sliceHasTail = sliceTailIndex[s] != kNoTailBlock;
                    const uint32_t regionCount = sliceHasTail ? 2u : 1u;
                    for (uint32_t region = 0; region < regionCount; ++region)
                    {
                        const bool isTail = region != 0u;
                        // Head blocks index by slice, tail blocks by the dense
                        // tail index — the same addressing the claim reserved.
                        const uint32_t blockIndex = isTail ? sliceTailIndex[s] : s;

                        const uint32_t sliceCursorBase = callCursorBase + sliceCursorOffset[s];
                        GroupCompactPushConstants cpc{};
                        cpc.recordCount = snapshotRecords;
                        cpc.rowCount    = sliceRowCount;
                        cpc.cursorBase  = isTail ? sliceCursorBase + cursorHalfStride
                                                 : sliceCursorBase;
                        cpc.countBase   = cpc.cursorBase + sliceRowCount;
                        cpc.srcRecordBase =
                            callRecordBase
                            + ((isTail ? tailStagingBlockOffset : 0u) + blockIndex)
                                  * snapshotRecords;
                        cpc.dstRecordBase =
                            callRecordBase
                            + ((isTail ? tailDrawBlockOffset : headDrawBlockOffset) + blockIndex)
                                  * snapshotRecords;
                        cpc.pairedRecords = isTail ? 1u : 0u;
                        ctx.Cmd->SetPushConstants(cpc);

                        ctx.Cmd->BindDescriptorSet(0, setHandle, compactPipeline);
                        ctx.Cmd->Dispatch(compactGroups, 1, 1);
                    }
                }
            }

            // Mirror this call's written cursor range into the host-visible
            // readback so the next BeginArenaFrame sums per-slice passed-casters
            // WITHOUT a GPU atomic (the cursors ARE the DrawIndexedIndirectCount
            // counts — methodology F1). The cursor writes are compute; make them
            // visible to the transfer copy. Scatter-pass timing is excluded from
            // every arc gate, so this extra barrier + copy never perturbs a gated
            // number; the copy only reads cursors, so consumers' indirect reads
            // are unaffected.
            ctx.Cmd->Barrier(ResourceBarrier::CreateMemoryBarrier(
                static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                static_cast<uint64_t>(PipelineStageMask::Transfer),
                static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
                static_cast<uint64_t>(ResourceAccessMask::TransferRead)));
            ctx.Cmd->CopyBuffer(cursors, cursorReadback,
                                static_cast<size_t>(neededCursors) * sizeof(uint32_t),
                                static_cast<size_t>(callCursorBase) * sizeof(uint32_t),
                                static_cast<size_t>(callCursorBase) * sizeof(uint32_t));
            // Mirror this call's per-slice stats rows (same barrier covers it).
            if (callStatsBase < kStatsSliceCapacity)
            {
                const uint32_t statsRows =
                    std::min(static_cast<uint32_t>(slices.size()), kStatsSliceCapacity - callStatsBase);
                ctx.Cmd->CopyBuffer(statsBuf, statsReadback,
                                    static_cast<size_t>(statsRows) * sizeof(ScatterSliceStats),
                                    static_cast<size_t>(callStatsBase) * sizeof(ScatterSliceStats),
                                    static_cast<size_t>(callStatsBase) * sizeof(ScatterSliceStats));
            }

            ctx.Cmd->Barrier(CreateStreamReadyBarrier());
        });

    return ordering;
}

} // namespace Rendering
} // namespace GameEngine
