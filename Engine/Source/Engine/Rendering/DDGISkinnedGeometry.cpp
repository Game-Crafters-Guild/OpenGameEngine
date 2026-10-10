#include "Engine/Rendering/DDGISkinnedGeometry.h"

#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/SceneAccelerationStructureService.h"
#include "Logger/Logger.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <cmath>
#include <unordered_set>

namespace GameEngine::Engine::Renderer
{
namespace
{
namespace RG = Rendering::RenderGraph;

// Mirrors ddgi_skin_positions.comp's DDGISkinParams (std140, two uvec4s).
struct SkinParamsUBO
{
    uint32_t VertexCount = 0;
    uint32_t CoreStrideFloats = 0;
    uint32_t PaletteOffset = 0;
    uint32_t HasSecondInfluence = 0;
    uint32_t Uv0OffsetFloats = 0xFFFFFFFFu;
    uint32_t VertexOffset = 0;
    uint32_t NormalOffsetFloats = 0xFFFFFFFFu;
    uint32_t Pad0 = 0;
};

// [pos.xyz | normal.xyz | uv.xy] — ddgi_skin_positions.comp's output record,
// byte-identical to SceneBvh::ThreadedBvh::VertexData so the software lane
// refits BVH ranges by verbatim copy. One posed buffer serves the BLAS build,
// the hardware hit-shade geometry row (UV0 at byte 24) and the software
// lane's posed vertex records.
constexpr uint32_t kSkinnedVertexStrideBytes = 32;
constexpr uint32_t kSkinnedUv0OffsetBytes = 24;

constexpr uint32_t kMaxSkinnedEntries = 1024;     // fail-visible ceiling, far above real content
constexpr uint32_t kMaxSkinnedVertices = 1u << 20;
constexpr uint64_t kRetireAfterTicks = 600;       // ~10 s of absence at 60 Hz
constexpr uint64_t kBufferRetireMargin = Rendering::IDevice::kMaxSupportedFramesInFlight + 1;
constexpr uint32_t kSkinGroupSize = 64;           // ddgi_skin_positions.comp local_size_x

// Core interleaved vertex: position (vec3), then normal (vec3) when present,
// then UV0 — the same layout law SceneAS's CoreVertexUV0Offset encodes.
uint32_t CoreVertexUv0OffsetFloats(Rendering::VertexAttributeFlags flags)
{
    using Rendering::VertexAttributeFlags;
    if ((flags & VertexAttributeFlags::HasUV0) == VertexAttributeFlags::None)
        return 0xFFFFFFFFu;
    uint32_t offsetFloats = 3u;
    if ((flags & VertexAttributeFlags::HasNormal) != VertexAttributeFlags::None)
        offsetFloats += 3u;
    return offsetFloats;
}

uint32_t CoreVertexNormalOffsetFloats(Rendering::VertexAttributeFlags flags)
{
    using Rendering::VertexAttributeFlags;
    if ((flags & VertexAttributeFlags::HasNormal) == VertexAttributeFlags::None)
        return 0xFFFFFFFFu;
    return 3u;  // right after position
}

uint64_t EntryKey(uint32_t runtimeId, uint32_t meshIndex)
{
    return (static_cast<uint64_t>(runtimeId) << 32) | meshIndex;
}

}  // namespace

DDGISkinnedGeometry::DDGISkinnedGeometry(Rendering::IDevice* device,
                                         Rendering::MeshGPURegistry* meshRegistry,
                                         SceneAccelerationStructureService* sceneAS)
    : m_Device(device), m_MeshRegistry(meshRegistry), m_SceneAS(sceneAS)
{
}

DDGISkinnedGeometry::~DDGISkinnedGeometry()
{
    Rendering::IAccelerationStructureBackend* backend = m_SceneAS ? m_SceneAS->GetBackend() : nullptr;
    for (auto& [key, entry] : m_Entries)
    {
        if (backend && entry.Blas.IsValid())
            backend->DestroyBlas(entry.Blas);
        if (m_Device && entry.SkinnedPositions.IsValid())
            m_Device->DestroyBuffer(entry.SkinnedPositions);
    }
    for (const RetiredBuffer& r : m_RetiredBuffers)
        if (m_Device && r.Buffer.IsValid())
            m_Device->DestroyBuffer(r.Buffer);
}

void DDGISkinnedGeometry::AbandonDeviceObjects(Rendering::IDevice* device)
{
    m_Device = device;
    m_Entries.clear();
    m_RetiredBuffers.clear();
    m_SkinPipeline = {};
    m_SkinSet0Layout = {};
    m_SkinMeta.reset();
    m_SkinLoadAttempted = false;
}

void DDGISkinnedGeometry::RetireAll()
{
    for (auto& [key, entry] : m_Entries)
        RetireEntry(entry);
    m_Entries.clear();
}

bool DDGISkinnedGeometry::LoadKernelIfNeeded()
{
    if (m_SkinLoadAttempted)
        return m_SkinPipeline.IsValid();
    m_SkinLoadAttempted = true;

    Rendering::ShaderPackage pkg{};
    std::string loadErr;
    if (!Rendering::LoadShaderPkg("Shaders/ddgi_skin_positions.shaderpkg",
                                 m_Device->PreferredShaderSource(), pkg, &loadErr))
    {
        Logger::Log::Warning("DDGISkinnedGeometry: failed to load skin kernel: {}", loadErr);
        return false;
    }
    auto itCs = pkg.stageBytes.find("cs");
    if (itCs == pkg.stageBytes.end() || itCs->second.empty())
    {
        Logger::Log::Warning("DDGISkinnedGeometry: skin kernel missing cs stage");
        return false;
    }

    m_SkinMeta = std::make_unique<Rendering::ShaderMeta>(std::move(pkg.meta));

    Rendering::ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
    cd.DebugName = "DDGI.SkinPositions";

    m_SkinSet0Layout = Rendering::DescriptorSetLayoutDesc{};
    auto patchLayout = [&](uint32_t setIndex, Rendering::DescriptorSetLayoutDesc& dsl)
    {
        if (setIndex == 0)
            m_SkinSet0Layout = dsl;
    };
    std::string err;
    Rendering::MaterialHelper::ApplyShaderMetaToComputeDesc(
        *m_Device, *m_SkinMeta, cd, Rendering::MaterialBuilder::MergeMode::Auto, {true, 128},
        patchLayout, &err);

    m_SkinPipeline = m_Device->InternComputePipeline(std::move(cd));
    return m_SkinPipeline.IsValid();
}

void DDGISkinnedGeometry::RetireEntry(Entry& entry)
{
    if (Rendering::IAccelerationStructureBackend* backend =
            m_SceneAS ? m_SceneAS->GetBackend() : nullptr;
        backend && entry.Blas.IsValid())
        backend->DestroyBlas(entry.Blas);  // backend defers past frames in flight
    if (entry.SkinnedPositions.IsValid())
        m_RetiredBuffers.push_back(RetiredBuffer{entry.SkinnedPositions, m_TickClock});
    entry.Blas = {};
    entry.SkinnedPositions = {};
}

bool DDGISkinnedGeometry::TryGetPosedBuffer(uint32_t runtimeId, uint32_t meshIndex,
                                            uint32_t expectedVertexCount, PosedBuffer& out) const
{
    const auto it = m_Entries.find(EntryKey(runtimeId, meshIndex));
    if (it == m_Entries.end() || !it->second.SkinnedPositions.IsValid() ||
        it->second.VertexCount != expectedVertexCount || !it->second.PosedRG.IsValid())
        return false;
    out.Buffer = it->second.SkinnedPositions;
    out.RG = it->second.PosedRG;
    out.Bytes = static_cast<uint64_t>(it->second.VertexCount) * kSkinnedVertexStrideBytes;
    return true;
}

DDGISkinnedGeometry::TickResult DDGISkinnedGeometry::Tick(
    RG::RGFrame& frame, const std::vector<Rendering::GPUInstance>& instances,
    Rendering::BufferHandle paletteBuffer, uint64_t paletteBytes, bool hardwareLane)
{
    TickResult result;
    ++m_TickClock;

    // RG resource ids are per-frame: last frame's imported posed handles must
    // die before ANY exit from this function, or an early-out tick (palette
    // not yet valid, kernel missing) leaves stale ids that TryGetPosedBuffer
    // would serve into this frame's pass declarations.
    for (auto& [key, entry] : m_Entries)
        entry.PosedRG = {};

    // Drain retired buffers past the in-flight margin.
    std::erase_if(m_RetiredBuffers,
                  [&](const RetiredBuffer& r)
                  {
                      if (m_TickClock - r.TickStamp <= kBufferRetireMargin)
                          return false;
                      m_Device->DestroyBuffer(r.Buffer);
                      return true;
                  });

    Rendering::IAccelerationStructureBackend* backend =
        m_SceneAS ? m_SceneAS->GetBackend() : nullptr;
    if ((hardwareLane && !backend) || !paletteBuffer.IsValid() || paletteBytes == 0)
        return result;
    if (!LoadKernelIfNeeded())
        return result;

    // Latch last tick's exec confirmations: a confirmed build makes the
    // entry's BLAS content trustworthy for TLAS inclusion.
    for (auto& [key, entry] : m_Entries)
    {
        if (entry.PendingConfirm && entry.PendingConfirm->load(std::memory_order_acquire))
        {
            entry.Ready = true;
            entry.PendingConfirm.reset();
        }
    }

    // Skinned mesh info, resolved fresh each tick (streams can repool on
    // reload; the registry sweep is a flat walk and skinned entries are few).
    std::unordered_map<uint32_t, SkinnedMeshInfo> meshInfo;
    m_MeshRegistry->ForEachEntry(
        [&](const Rendering::MeshGPUEntry& entry)
        {
            using Rendering::VertexAttributeFlags;
            if ((entry.vertexFlags & VertexAttributeFlags::HasJoints) == VertexAttributeFlags::None ||
                (entry.vertexFlags & VertexAttributeFlags::HasWeights) == VertexAttributeFlags::None)
                return;
            if (entry.indexCount < 3u || entry.topology != Rendering::PrimitiveTopology::TriangleList)
                return;
            const uint32_t coreStride = m_MeshRegistry->GetEntryCoreStrideBytes(entry);
            if (coreStride < 12u || (coreStride % 4u) != 0u)
                return;
            Rendering::MeshGPUEntryBindings bindings{};
            if (!m_MeshRegistry->TryGetDrawableBindings(entry, bindings))
                return;
            if (!bindings.jointsVB.IsValid() || !bindings.weightsVB.IsValid())
                return;
            SkinnedMeshInfo info;
            info.CoreVB = bindings.coreVB;
            info.JointsVB = bindings.jointsVB;
            info.WeightsVB = bindings.weightsVB;
            info.Joints1VB = bindings.joints1VB;
            info.Weights1VB = bindings.weights1VB;
            info.IndexBuffer = bindings.indexBuffer;
            info.CoreStrideBytes = coreStride;
            info.VertexOffset = entry.vertexOffset;
            info.VertexCount = static_cast<uint32_t>(entry.coreSubAlloc.size / coreStride);
            info.FirstIndex = entry.firstIndex;
            info.IndexCount = entry.indexCount;
            info.IndexType = entry.indexType;
            info.Uv0OffsetFloats = CoreVertexUv0OffsetFloats(entry.vertexFlags);
            info.NormalOffsetFloats = CoreVertexNormalOffsetFloats(entry.vertexFlags);
            info.HasSecondInfluence =
                (entry.vertexFlags & VertexAttributeFlags::HasJoints1) != VertexAttributeFlags::None &&
                (entry.vertexFlags & VertexAttributeFlags::HasWeights1) != VertexAttributeFlags::None &&
                bindings.joints1VB.IsValid() && bindings.weights1VB.IsValid();
            if (info.VertexCount == 0 || info.VertexCount > kMaxSkinnedVertices)
                return;
            meshInfo.emplace(entry.gpuMeshIndex, info);
        });

    // Sweep this tick's instances; every skinned instance re-skins and
    // rebuilds each tick (same-address in-place rebuild — topology is fixed,
    // so the created storage always suffices).
    struct TouchedEntry
    {
        Entry* E = nullptr;
        const SkinnedMeshInfo* Info = nullptr;
        uint32_t PaletteOffset = 0;
        RG::RGBuffer OutRG{};
    };
    std::vector<TouchedEntry> touched;

    // Eligibility is the MESH carrying joints, not the palette being live: an
    // instance drawn at bind pose (skinPaletteOffset 0 — no active animation)
    // still renders, so it must still occlude and bounce. Palette offset 0 is
    // the atlas's identity block, so the skin dispatch produces exactly the
    // bind pose; such instances share one entry per mesh (runtimeId 0 key),
    // deduped below, since their pose is identical by construction.
    std::unordered_set<uint64_t> touchedKeys;
    for (uint32_t i = 0; i < instances.size(); ++i)
    {
        const Rendering::GPUInstance& inst = instances[i];
        if ((inst.sectorPacked[0] | inst.sectorPacked[1]) != 0u)
            continue;
        auto itInfo = meshInfo.find(inst.meshIndex);
        if (itInfo == meshInfo.end())
            continue;
        const SkinnedMeshInfo& info = itInfo->second;

        const float* m = inst.transform.Data();
        bool sane = std::isfinite(inst.boundingRadius);
        for (int f = 0; f < 16 && sane; ++f)
            sane = std::isfinite(m[f]);
        if (!sane)
            continue;

        const uint64_t key =
            EntryKey(inst.skinPaletteOffset != 0u ? inst.runtimeId : 0u, inst.meshIndex);
        const bool firstTouch = touchedKeys.insert(key).second;
        auto itEntry = m_Entries.find(key);
        if (itEntry == m_Entries.end())
        {
            if (m_Entries.size() >= kMaxSkinnedEntries)
            {
                if (!m_WarnedOverCap)
                {
                    m_WarnedOverCap = true;
                    Logger::Log::Warning(
                        "DDGISkinnedGeometry: over {} skinned instances — further ones are "
                        "absent from GI until entries retire (first occurrence, then silent)",
                        kMaxSkinnedEntries);
                }
                continue;
            }
            Entry fresh;
            fresh.VertexCount = info.VertexCount;
            fresh.HasUv0 = info.Uv0OffsetFloats != 0xFFFFFFFFu;

            Rendering::BufferDesc bd{};
            bd.size = static_cast<uint64_t>(info.VertexCount) * kSkinnedVertexStrideBytes;
            bd.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage |
                                             Rendering::BufferUsage::AccelerationStructureBuildInput |
                                             Rendering::BufferUsage::ShaderDeviceAddress);
            bd.debugName = "DDGI.SkinnedPositions";
            fresh.SkinnedPositions = m_Device->CreateBuffer(bd);
            if (!fresh.SkinnedPositions.IsValid())
                continue;
            fresh.SkinnedPositionsAddress = m_Device->GetBufferDeviceAddress(fresh.SkinnedPositions);

            fresh.Geometry.VertexAddress = fresh.SkinnedPositionsAddress;
            fresh.Geometry.VertexStrideBytes = kSkinnedVertexStrideBytes;
            fresh.Geometry.FirstVertex = 0;  // dense output; index values stay mesh-local
            fresh.Geometry.MaxVertex = info.VertexCount - 1u;
            fresh.Geometry.IndexAddress = m_Device->GetBufferDeviceAddress(info.IndexBuffer);
            fresh.Geometry.FirstIndex = info.FirstIndex;
            fresh.Geometry.IndexCount = info.IndexCount;
            fresh.Geometry.IndexKind = static_cast<Rendering::IndexType>(info.IndexType);
            if (fresh.Geometry.VertexAddress == 0 || fresh.Geometry.IndexAddress == 0)
            {
                m_RetiredBuffers.push_back(RetiredBuffer{fresh.SkinnedPositions, m_TickClock});
                continue;
            }
            if (hardwareLane)
            {
                fresh.Blas = backend->CreateBlas(fresh.Geometry);
                if (!fresh.Blas.IsValid())
                {
                    m_RetiredBuffers.push_back(RetiredBuffer{fresh.SkinnedPositions, m_TickClock});
                    continue;
                }
                fresh.BlasAddress = backend->GetBlasDeviceAddress(fresh.Blas);
            }
            itEntry = m_Entries.emplace(key, std::move(fresh)).first;
        }
        Entry& entry = itEntry->second;
        entry.LastSeenTick = m_TickClock;
        if (entry.VertexCount != info.VertexCount)
        {
            // Repooled/reimported mesh under the same key: the created BLAS
            // storage no longer matches. Retire now; next tick recreates.
            RetireEntry(entry);
            m_Entries.erase(itEntry);
            continue;
        }

        if (hardwareLane && entry.Ready)
            result.TlasRows.push_back(SkinnedTlasRow{i, entry.BlasAddress});
        if (!firstTouch)
            continue;  // shared bind-pose entry: one skin + one build serves every instance
        if (hardwareLane)
        {
            if (!backend->ReserveBlasScratch(entry.Blas))
                continue;
            result.Builds.push_back(PendingSkinnedBuild{entry.Blas, entry.Geometry, {}});
        }
        touched.push_back(TouchedEntry{&entry, &info, inst.skinPaletteOffset, {}});
    }

    // Retire entries that vanished (despawn, mesh reload, LOD-out).
    for (auto it = m_Entries.begin(); it != m_Entries.end();)
    {
        Entry& entry = it->second;
        if ((m_TickClock - entry.LastSeenTick) > kRetireAfterTicks)
        {
            RetireEntry(entry);
            it = m_Entries.erase(it);
        }
        else
            ++it;
    }

    // Per-instance geometry-row override for the trace kernel: every TLAS row
    // points at its entry's POSED buffer, so a hit's face normal and UVs are
    // the posed surface's (bind-pose attribute rows in the shared table stay
    // as the fallback for anything without an override).
    if (!result.TlasRows.empty())
    {
        const uint64_t mapBytes = static_cast<uint64_t>(instances.size()) * sizeof(uint32_t);
        auto mapAlloc = frame.AllocUpload(mapBytes, 4);
        auto rowsAlloc =
            frame.AllocUpload(result.TlasRows.size() * sizeof(SkinnedGeomRowGPU), 16);
        if (mapAlloc.Valid() && rowsAlloc.Valid())
        {
            auto* map = static_cast<uint32_t*>(mapAlloc.Ptr);
            std::fill(map, map + instances.size(), 0xFFFFFFFFu);
            auto* rows = static_cast<SkinnedGeomRowGPU*>(rowsAlloc.Ptr);
            for (size_t r = 0; r < result.TlasRows.size(); ++r)
            {
                const SkinnedTlasRow& tlasRow = result.TlasRows[r];
                const Rendering::GPUInstance& rowInst = instances[tlasRow.InstanceIndex];
                const uint64_t key = EntryKey(
                    rowInst.skinPaletteOffset != 0u ? rowInst.runtimeId : 0u, rowInst.meshIndex);
                const Entry& entry = m_Entries.at(key);
                SkinnedGeomRowGPU row;
                row.VertexAddress = entry.SkinnedPositionsAddress;
                row.IndexAddress = entry.Geometry.IndexAddress;
                row.VertexStrideBytes = entry.Geometry.VertexStrideBytes;
                row.FirstVertex = 0;
                row.FirstIndex = entry.Geometry.FirstIndex;
                row.IndexKind = static_cast<uint32_t>(entry.Geometry.IndexKind);
                row.UV0OffsetBytes = entry.HasUv0 ? kSkinnedUv0OffsetBytes : 0xFFFFFFFFu;
                rows[r] = row;
                map[tlasRow.InstanceIndex] = static_cast<uint32_t>(r);
            }
            result.RowMapBuffer = mapAlloc.Buffer;
            result.RowMapOffset = mapAlloc.Offset;
            result.RowMapBytes = mapBytes;
            result.GeomRowsBuffer = rowsAlloc.Buffer;
            result.GeomRowsOffset = rowsAlloc.Offset;
            result.GeomRowsBytes = result.TlasRows.size() * sizeof(SkinnedGeomRowGPU);
        }
        else
        {
            // No upload space: drop the rows rather than trace posed BLASes
            // with unmatched attribute rows pointing at bind-pose positions.
            result.TlasRows.clear();
        }
    }

    if (touched.empty())
        return result;

    if (hardwareLane)
    {
        result.ConfirmToken = std::make_shared<std::atomic<bool>>(false);
        for (TouchedEntry& t : touched)
            t.E->PendingConfirm = result.ConfirmToken;
    }

    // Declare the skin pass. Each touched output buffer is imported so the
    // caller's AS pass can declare Reads against the same handles and the
    // graph orders skin-writes before BLAS builds.
    struct SkinDispatch
    {
        RG::RGFrame::TypedUpload<SkinParamsUBO> Ub;
        Rendering::BufferHandle CoreVB, JointsVB, WeightsVB, Joints1VB, Weights1VB, Out, Palette;
        uint64_t CoreBytes = 0, JointsBytes = 0, WeightsBytes = 0, Joints1Bytes = 0,
                 Weights1Bytes = 0, OutBytes = 0, PaletteBytes = 0;
        uint32_t Groups = 0;
    };
    auto dispatches = std::make_shared<std::vector<SkinDispatch>>();
    dispatches->reserve(touched.size());

    std::vector<RG::RGBuffer> outHandles;
    outHandles.reserve(touched.size());
    for (TouchedEntry& t : touched)
    {
        const SkinnedMeshInfo& info = *t.Info;
        SkinDispatch d;
        d.Ub = frame.AllocUpload<SkinParamsUBO>();
        SkinParamsUBO ub;
        ub.VertexCount = info.VertexCount;
        ub.CoreStrideFloats = info.CoreStrideBytes / 4u;
        ub.PaletteOffset = t.PaletteOffset;
        ub.HasSecondInfluence = info.HasSecondInfluence ? 1u : 0u;
        ub.Uv0OffsetFloats = info.Uv0OffsetFloats;
        ub.VertexOffset = info.VertexOffset;
        ub.NormalOffsetFloats = info.NormalOffsetFloats;
        *d.Ub.Ptr = ub;

        // The kernel indexes streams from vertex 0 of the POOL, so sizes span
        // through this mesh's range (offset + count). Absent second-influence
        // streams alias the primary ones; the flag keeps them unread.
        const uint64_t vertEnd = static_cast<uint64_t>(info.VertexOffset) + info.VertexCount;
        d.CoreVB = info.CoreVB;
        d.CoreBytes = vertEnd * info.CoreStrideBytes;
        d.JointsVB = info.JointsVB;
        d.JointsBytes = vertEnd * 8u;
        d.WeightsVB = info.WeightsVB;
        d.WeightsBytes = vertEnd * 16u;
        d.Joints1VB = info.HasSecondInfluence ? info.Joints1VB : info.JointsVB;
        d.Joints1Bytes = info.HasSecondInfluence ? vertEnd * 8u : d.JointsBytes;
        d.Weights1VB = info.HasSecondInfluence ? info.Weights1VB : info.WeightsVB;
        d.Weights1Bytes = info.HasSecondInfluence ? vertEnd * 16u : d.WeightsBytes;
        d.Out = t.E->SkinnedPositions;
        d.OutBytes = static_cast<uint64_t>(t.E->VertexCount) * kSkinnedVertexStrideBytes;
        d.Palette = paletteBuffer;
        d.PaletteBytes = paletteBytes;
        d.Groups = (info.VertexCount + kSkinGroupSize - 1u) / kSkinGroupSize;
        dispatches->push_back(d);

        t.OutRG = frame.ImportExternalBuffer("DDGI.SkinnedPositions", t.E->SkinnedPositions,
                                             d.OutBytes);
        t.E->PosedRG = t.OutRG;
        outHandles.push_back(t.OutRG);
    }
    // touched and Builds run in lockstep on the hardware lane (every touched
    // entry pushed a build); the software lane has no builds to patch.
    for (size_t b = 0; b < result.Builds.size(); ++b)
        result.Builds[b].SkinnedRG = outHandles[b];

    const Rendering::ComputePipelineId skinPipe = m_SkinPipeline;
    const Rendering::DescriptorSetLayoutDesc skinLayout = m_SkinSet0Layout;
    const Rendering::ShaderMeta* skinMeta = m_SkinMeta.get();
    frame.AddComputePass(
        "DDGI.SkinPositions", Rendering::PassPhase::kEarlySetup,
        [outHandles](RG::RGPassBuilder& p)
        {
            p.PreventCulling();
            for (RG::RGBuffer b : outHandles)
                p.Write(b, RG::RGBufferWrite::Storage);
        },
        [dispatches, skinPipe, skinLayout, skinMeta](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !skinMeta)
                return;
            Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(skinPipe);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            for (const SkinDispatch& d : *dispatches)
            {
                Rendering::DescriptorSetDesc dsDesc{};
                dsDesc.layout = skinLayout;
                dsDesc.transient = true;
                dsDesc.debugName = "DDGI.SkinPositions.Set0";
                auto ds = dev->CreateDescriptorSet(dsDesc);
                Rendering::NamedDescriptorWriter wd(dev, ds, *skinMeta, 0);
                wd.AddUniformBuffer("SkinParams", d.Ub.Buffer, d.Ub.Offset, sizeof(SkinParamsUBO));
                wd.AddStorageBuffer("CoreVertices", d.CoreVB, 0, d.CoreBytes);
                wd.AddStorageBuffer("JointsStream", d.JointsVB, 0, d.JointsBytes);
                wd.AddStorageBuffer("WeightsStream", d.WeightsVB, 0, d.WeightsBytes);
                wd.AddStorageBuffer("Joints1Stream", d.Joints1VB, 0, d.Joints1Bytes);
                wd.AddStorageBuffer("Weights1Stream", d.Weights1VB, 0, d.Weights1Bytes);
                wd.AddStorageBuffer("BonePaletteAtlasSSBO", d.Palette, 0, d.PaletteBytes);
                wd.AddStorageBuffer("SkinnedPositions", d.Out, 0, d.OutBytes);
                wd.Flush();
                cl->BindDescriptorSet(0, ds, pipe);
                cl->Dispatch(d.Groups, 1, 1);
            }
        });

    return result;
}

}  // namespace GameEngine::Engine::Renderer
