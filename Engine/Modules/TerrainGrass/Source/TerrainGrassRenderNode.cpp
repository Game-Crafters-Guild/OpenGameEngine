#include "TerrainGrass/TerrainGrassRenderNode.h"

#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainGrass/TerrainGrassPlacementStats.h"
#include "TerrainGrass/TerrainGrassRenderFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Logger/Logger.h"

#include <cstdlib>
#include <cstring>
#include <memory>

namespace GameEngine::TerrainGrass
{

bool TerrainGrassRenderNode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);
    return true;
}

namespace
{

// Every path out of DeclareForView that does NOT take a readback slot has placed
// no grass this frame, and the placed count is latched from the newest resolved
// slot — so without this the instrument keeps serving the last healthy number on
// exactly the frames where grass is broken (no device, no terrain feature, the
// grass feature failed to initialise, placement buffers failed to allocate). Armed
// at the top, disarmed once a slot is actually taken.
//
// Only reports if the feature already exists: if it never did, "no grass feature"
// is the honest answer and the payload reports that separately.
class IdleOnEarlyReturn
{
  public:
    IdleOnEarlyReturn(Engine::Renderer::RenderServices& rs, Rendering::ViewId viewId)
        : m_Services(rs), m_ViewId(viewId)
    {
    }

    ~IdleOnEarlyReturn()
    {
        if (!m_Armed)
            return;
        if (auto* feature = m_Services.GetFeature<TerrainGrassRenderFeature>())
            feature->PlacementStats().RecordIdle(m_ViewId);
    }

    void Disarm() { m_Armed = false; }

    IdleOnEarlyReturn(const IdleOnEarlyReturn&) = delete;
    IdleOnEarlyReturn& operator=(const IdleOnEarlyReturn&) = delete;

  private:
    Engine::Renderer::RenderServices& m_Services;
    Rendering::ViewId m_ViewId;
    bool m_Armed = true;
};

} // namespace

void TerrainGrassRenderNode::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d)
{
    namespace RenderGraph = ::GameEngine::Rendering::RenderGraph;
    auto& rs = d.Services;
    const auto viewId = d.View.id;
    IdleOnEarlyReturn idleGuard(rs, viewId);

    auto* device = rs.GetDevice();
    if (!device)
        return;

    auto* terrainFeature = rs.GetFeature<TerrainECS::TerrainRenderFeature>();
    if (!terrainFeature || !terrainFeature->IsInitialized())
        return;

    const uint32 frameIndex = device->GetFrameIndex();

    // Grass is renderer-agnostic: it reads the terrain params from the SSBO the
    // extraction system fills every frame (UploadTerrainParamsArray), keyed by the
    // renderer-blind LastTerrainParamsSlot — NOT the CDLOD render node's captured
    // grid. Both the CDLOD and CBT renderers share the same heightmap + params, so
    // grass placement is identical A/B, and grass keeps working after the CDLOD
    // render core is deleted. Inactive => declare nothing, ensure nothing (grassless
    // scenes would otherwise dispatch ~8M candidates + 100-200MB/view).
    const uint32 paramsSlot = terrainFeature->GetLastTerrainParamsSlot();
    const uint32 terrainParamsCount = terrainFeature->GetTerrainParamsCount(paramsSlot);
    const uint32 activeGrassCount = terrainFeature->GetTerrainGrassActiveCount(paramsSlot);
    // GE_TERRAIN_GRASS_DEBUG: this gate decides whether grass declares ANYTHING for the view. If it
    // returns here nothing downstream runs (no forward contributor, no placement pass) -> total silence
    // on both the emit + dispatch traces. The counts are renderer-blind (the params SSBO the
    // extraction fills); log the slot + counts the node reads vs what the extraction reports.
    static const bool kDbg = std::getenv("GE_TERRAIN_GRASS_DEBUG") != nullptr;
    {
        static uint32 s_LastActive = 0xFFFFFFFFu, s_LastParams = 0xFFFFFFFFu;
        if (kDbg && (activeGrassCount != s_LastActive || terrainParamsCount != s_LastParams))
        {
            s_LastActive = activeGrassCount;
            s_LastParams = terrainParamsCount;
            Logger::Log::Info("TerrainGrass.Node view={} slot={} params={} active={} -> {}",
                              static_cast<uint32>(viewId), paramsSlot, terrainParamsCount,
                              activeGrassCount,
                              (terrainParamsCount == 0 || activeGrassCount == 0) ? "STOP (declares nothing)"
                                                                                : "declare");
        }
    }
    if (terrainParamsCount == 0 || activeGrassCount == 0)
        return; // idleGuard reports the measured zero

    auto& grass = rs.EnsureFeature<TerrainGrassRenderFeature>();
    if (!grass.IsInitialized() && !grass.Initialize(device))
        return;

    // The forward contributor emits in step-0 BuildWorldBatchKeys (uniform with
    // terrain; covers thumbnail BuildWorldBatchKeysForView).
    grass.EnsureForwardContributor(rs, *terrainFeature);

    // Ensure the placement buffers exist and the view's plan is resolved NOW so the pass can
    // import them and the forward draw can size the instance binding from the plan's capacity; the
    // exec re-ensures idempotently. Decline cleanly if allocation failed.
    if (!grass.EnsurePlacementBuffersForViewRG(rs, *terrainFeature, viewId, frameIndex,
                                               activeGrassCount))
        return;
    const auto instancesBuf = grass.GetInstanceBuffer(viewId, frameIndex);
    const auto argsBuf = grass.GetIndirectArgsBuffer(viewId, frameIndex);
    const auto countBuf = grass.GetIndirectCountBuffer(viewId, frameIndex);
    if (!instancesBuf.IsValid() || !argsBuf.IsValid() || !countBuf.IsValid())
        return;

    // Placement runs on the GRAPHICS queue this slice: the heightmap dependency
    // it samples is bindless/undeclared, so async compute is a post-parity
    // redesign. Imports auto-external (survive cull, no PreventCulling); the exec
    // clears only the counts the compute accumulates, seeds the CPU-owned draw
    // fields once per buffer slot, and carries its own internal barriers (the
    // MoltenVK indirect-count contract).
    const std::string instancesName = "TerrainGrass.Instances.View" + std::to_string(static_cast<uint32>(viewId));
    const std::string argsName = "TerrainGrass.IndirectArgs.View" + std::to_string(static_cast<uint32>(viewId));
    const std::string countName = "TerrainGrass.IndirectCount.View" + std::to_string(static_cast<uint32>(viewId));
    const RenderGraph::RGBuffer instancesRG = d.Frame.ImportExternalBuffer(instancesName.c_str(), instancesBuf);
    const RenderGraph::RGBuffer argsRG = d.Frame.ImportExternalBuffer(argsName.c_str(), argsBuf);
    const RenderGraph::RGBuffer countRG = d.Frame.ImportExternalBuffer(countName.c_str(), countBuf);

    // Idle elision: when every input the placement kernels read is byte-identical to the last time
    // THIS (view, frame slot) was visited, the buffers imported above already hold exactly what a
    // re-run would write, so the pass is not declared at all. The imports, the world-read emit and the
    // readback below still happen — an elided frame's grass is drawn from retained buffers and the
    // instrument keeps reporting the numbers that describe them, rather than going dark.
    if (!grass.ShouldSkipPlacement(*terrainFeature, viewId, frameIndex))
    {
        const RenderGraph::RGPass placePass = d.Frame.AddPass(
            d.PassName("Place").c_str(), Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            {
                p.Write(instancesRG, RenderGraph::RGBufferWrite::Storage);
                p.Write(argsRG, RenderGraph::RGBufferWrite::Storage);
                p.Write(countRG, RenderGraph::RGBufferWrite::Storage);
            },
            [grassPtr = &grass, rsPtr = &rs, terrainFeature, viewId,
             frameIndex](RenderGraph::RGContext& ctx)
            {
                grassPtr->DispatchPlacementForView(ctx, *rsPtr, *terrainFeature, viewId, frameIndex);
            });

        // Placement samples the terrain heightmap/splat bindlessly, so the graph cannot see the
        // dependency on this frame's upload flush from the declared accesses. Order it explicitly:
        // without the edge the scheduler is free to emit this whole component — placement and the
        // draws that read its output — before the flush, and it does.
        const RenderGraph::RGPassId uploadPass =
            terrainFeature->GetHeightmapUploadPassRG(d.Frame.FrameIndex(), &d.Frame);
        if (uploadPass != RenderGraph::kInvalidId)
            d.Frame.AddOrderingEdge(RenderGraph::RGPass{uploadPass}, placePass);
    }

    // The world arm declares a Read(Indirect) on the args (decision 2b): the
    // placement orders before the indirect draw, and this is the only shape
    // that ever permits async compute. The vertex shader reads the blade
    // instances through the feature's descriptor set, so that read is declared
    // too: a graph barrier covers one resource, and the args read does not make
    // the compute writes to the instance buffer visible to vertex fetch.
    // The draws' prepass heads read the same two buffers, from the non-occluding prepass after DepthResolve
    // (or from the camera prepass when the pipeline has none). This node is declared ahead of both: it is
    // registered as feeding the prepass, and the pipeline compiler declares it first.
    const Engine::Renderer::ForwardBufferReaders readers = grass.DrawsPrepassHeads(viewId)
                                                               ? Engine::Renderer::ForwardBufferReaders::WorldPassAndPrepass
                                                               : Engine::Renderer::ForwardBufferReaders::WorldPass;
    rs.EmitForwardSampledBufferRead(d.Frame, viewId, argsRG, RenderGraph::RGBufferRead::Indirect, readers);
    rs.EmitForwardSampledBufferRead(d.Frame, viewId, instancesRG, RenderGraph::RGBufferRead::Storage, readers);

    // Placed-blade instrumentation: copy the placement compute's counters — the per-LOD
    // InstanceCounts the draws consume, plus the candidates/cells/refusals — into this view's
    // readback ring slot. Draws and counters share one buffer, so the copy is one contiguous range
    // and a resolved count can never describe a different dispatch than the draw beside it. The
    // dispatch shape is stamped into the slot's payload for the same reason. Declared only on the
    // path that already dispatched, so a grassless scene pays nothing.
    {
        const GrassPlacementPlan& plan = grass.GetPlan(viewId, frameIndex);
        TerrainGrassPlacementStats::Dispatch dispatched{};
        dispatched.Capacity = plan.Capacity;
        dispatched.CellCount = plan.CellCount;
        dispatched.PlannedCandidates = plan.PlannedCandidates;
        dispatched.TerrainParamsCount = terrainParamsCount;
        dispatched.ActiveGrassTerrains = activeGrassCount;
        dispatched.NearDensity = plan.Params.NearDensity;
        dispatched.Range = plan.Params.FarRadius;

        if (const Rendering::BufferHandle statsSlot = grass.PlacementStats().AcquireSlotRG(
                device, d.Frame, viewId, dispatched);
            statsSlot.IsValid())
        {
            // A slot is taken: this view IS placing grass, so the early-return
            // guard must not report it idle when the function returns normally.
            idleGuard.Disarm();

            d.Frame.AddPass(
                d.PassName("PlacedCountReadback").c_str(),
                static_cast<int32>(Rendering::PassPhase::kFinalize),
                [&](RenderGraph::RGPassBuilder& p)
                {
                    p.Read(argsRG, RenderGraph::RGBufferRead::CopySrc);
                    // CPU-consumed output — the ring slot is not a graph resource.
                    p.PreventCulling();
                },
                [argsRG, statsSlot](RenderGraph::RGContext& ctx)
                {
                    const Rendering::BufferHandle src = ctx.GetBuffer(argsRG);
                    if (!src.IsValid() || !ctx.Cmd)
                        return;
                    ctx.Cmd->Barrier(Rendering::ResourceBarrier::CreateMemoryBarrier(
                        static_cast<uint64>(Rendering::PipelineStageMask::Transfer)
                            | static_cast<uint64>(Rendering::PipelineStageMask::ComputeShader),
                        static_cast<uint64>(Rendering::PipelineStageMask::Transfer),
                        static_cast<uint64>(Rendering::ResourceAccessMask::TransferWrite)
                            | static_cast<uint64>(Rendering::ResourceAccessMask::ShaderWrite),
                        static_cast<uint64>(Rendering::ResourceAccessMask::TransferRead)));
                    ctx.Cmd->CopyBuffer(
                        src, statsSlot, TerrainGrassPlacementStats::kSlotBytes,
                        TerrainGrassPlacementStats::kIndirectArgsCopyOffset, 0);
                });
        }
    }

    // GE_TERRAIN_GRASS_DEBUG: the placed count comes from the SAME instrument the
    // payload reports — polling the stats ring here, not a second readback of the
    // same word at the same offset. Polling from the declare keeps the latch fresh
    // every frame instead of only when someone calls get_terrain_debug, and it is
    // the same main thread the debug-server handler polls on.
    if (kDbg)
    {
        static uint32 s_LastPlaced = 0xFFFFFFFFu;
        grass.PlacementStats().Poll(device);
        uint32 placed = 0xFFFFFFFFu;
        for (const auto& row : grass.PlacementStats().LatchedPlacements())
        {
            if (row.View == viewId && row.HasSample)
            {
                placed = row.Placed.PlacedInstances;
                break;
            }
        }
        if (placed != 0xFFFFFFFFu && placed != s_LastPlaced)
        {
            s_LastPlaced = placed;
            for (const auto& row : grass.PlacementStats().LatchedPlacements())
            {
                if (row.View != viewId)
                    continue;
                const auto& placement = row.Placed;
                Logger::Log::Info(
                    "TerrainGrass.PlacedCount view={} instances={} accepted={} cellsVisible={} "
                    "candidates={} fittedRangeScale={}",
                    static_cast<uint32>(viewId), placed, placement.AcceptedBlades,
                    placement.CellsVisible, placement.CandidatesConsidered,
                    placement.FittedRangeScale);
                break;
            }
        }

        // Instance root positions (WorldX, WorldZ, RootY of the first 3 blades) — proves whether the
        // baked roots sit on the terrain surface. GrassBladeInstance is 48 bytes; the first 3 fields
        // are the world XZ + RootY.
        //
        // The placement compute never rewrites the instance buffer when it places 0 blades, so this readback
        // would otherwise echo the last non-zero frame's roots as if they were live. Gate on the
        // resolved placed count: only a readback whose placed count is known > 0 may print roots;
        // placed == 0 prints its own line (on state change), and an unresolved count prints nothing.
        static std::shared_ptr<Rendering::RGBufferReadbackTicket> s_RootTicket;
        static float s_LastRootY = -1e9f;
        static bool s_RootsIdle = false;
        if (s_RootTicket)
        {
            Rendering::BufferReadbackResult res{};
            if (s_RootTicket->TryGet(res) && res.bytes.size() >= 48u * 3u)
            {
                if (s_LastPlaced == 0)
                {
                    if (!s_RootsIdle)
                    {
                        s_RootsIdle = true;
                        s_LastRootY = -1e9f; // re-arm so live roots reprint when placement resumes
                        Logger::Log::Info(
                            "TerrainGrass.Roots view={} placed=0 (instance buffer not rewritten; "
                            "roots readback idle)",
                            static_cast<uint32>(viewId));
                    }
                }
                else if (s_LastPlaced != 0xFFFFFFFFu)
                {
                    s_RootsIdle = false;
                    const float* f = reinterpret_cast<const float*>(res.bytes.data());
                    if (f[2] != s_LastRootY)
                    {
                        s_LastRootY = f[2];
                        Logger::Log::Info(
                            "TerrainGrass.Roots view={} i0=({},{},{}) i1=({},{},{}) i2=({},{},{})",
                            static_cast<uint32>(viewId), f[0], f[1], f[2], f[12], f[13], f[14],
                            f[24], f[25], f[26]);
                    }
                }
                s_RootTicket.reset();
            }
        }
        if (!s_RootTicket)
            s_RootTicket = Rendering::RequestBufferReadbackRG(device, d.Frame, instancesRG, 0u,
                                                             48u * 3u, "TerrainGrass.Roots");
    }
}

} // namespace GameEngine::TerrainGrass
