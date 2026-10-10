#include "TerrainECS/TerrainUploadNode.h"

#include "TerrainECS/TerrainHeightPageFeature.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::TerrainECS
{

bool TerrainUploadNode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);
    return true;
}

void TerrainUploadNode::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d)
{
    namespace RenderGraph = ::GameEngine::Rendering::RenderGraph;
    auto& rs = d.Services;
    auto* device = rs.GetDevice();
    if (!device)
        return;

    auto& feature = rs.EnsureFeature<TerrainRenderFeature>();
    if (!feature.IsInitialized() && !feature.Initialize(device))
        return;

    // Retires deferred texture/buffer destroys (ReleaseTerrainResources) once per frame.
    // `rs` frees each retired terrain texture's bindless slot before its image is destroyed.
    feature.TryCleanupStaleViews(device->GetFrameIndex(), rs);

    // The claim keys on the render graph's MONOTONIC per-frame index (shared across
    // every window's graph in one engine frame — see CBTRenderFeature::ClaimUpdate),
    // NOT the device's cyclic frame slot: the cyclic slot wraps, so a stamp from a
    // wrapped-around frame with no upload could false-match and mis-fire the cross-
    // graph tripwire; and in multi-window all windows share this index, so the first
    // graph claims and the rest are refused (single flush, no double copy).
    const uint64 rgFrame = d.Frame.FrameIndex();

    // The height pages published since the last frame: their staged slot uploads ride this pass,
    // and the page tables latched with them become readable once the pass is declared
    // (TerrainHeightPageFeature::CommitLatch).
    auto& pages = rs.EnsureFeature<TerrainHeightPageFeature>();
    pages.Latch(*device, rgFrame);

    // Heightmap/splatmap/normalmap upload: ONCE per frame across views (feature-owned
    // frame stamp), on the GRAPHICS queue. The textures are bindless (never
    // RenderGraph-imported), so this pass shares no declared access with the CBT update, the
    // surface or grass, and phase alone does NOT order it ahead of them: the scheduler ranks
    // whole connected components, and an access-less pass is a component of one that loses to
    // any consumer component holding an earlier kEarlySetup pass. The ordering guarantee is the
    // explicit edge each consumer adds from this pass to its own, which is why the pass is
    // published on the feature below. PreventCulling keeps the edge-less CPU-staged copy alive,
    // and FlushPendingUploads carries its own pre/post-copy barriers.
    // Runs when there are pending texture uploads, a pending GPU height bake (slice-1c
    // eval-skip produces bakes with no CPU upload) or staged height page uploads. One flush per
    // rgFrame across views.
    if ((feature.HasPendingUploads() || feature.HasPendingBakes() || pages.HasStagedUploads()) &&
        feature.TryClaimHeightmapUploadFrameRG(rgFrame, &d.Frame))
    {
        const RenderGraph::RGPass uploadPass = d.Frame.AddPass(
            "TerrainHeightmapUpload", Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p) { p.PreventCulling(); },
            [featurePtr = &feature, pagesPtr = &pages](RenderGraph::RGContext& ctx)
            {
                if (!ctx.Cmd)
                    return;
                // Upload (clear + normal/splat slot copies) first, then the GPU height bake
                // imageStore into the same atlas — recorded sequentially in one command list so
                // the clear/upload precede the bake, and the bake's post-barrier makes the height
                // visible to the CBT update sampling binding 18.
                featurePtr->FlushPendingUploads(ctx.Cmd);
                featurePtr->FlushPendingBakes(ctx.Cmd);
                pagesPtr->FlushUploads(*ctx.Cmd);
            });

        // Consumers pull this and order themselves after it. This node never learns who they
        // are — the dependency belongs to whoever samples the textures.
        feature.RecordHeightmapUploadPassRG(rgFrame, &d.Frame, uploadPass.Id);
        pages.CommitLatch();
    }
}

} // namespace GameEngine::TerrainECS
