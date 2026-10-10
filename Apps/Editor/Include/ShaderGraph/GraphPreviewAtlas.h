#pragma once

// One render-graph pass that draws every material-graph node preview into a
// single persistent atlas texture: one unit sphere per cell, the node's preview
// material bound per draw.
//
// Replaces the per-slot thumbnail machinery for every preview the material graph
// draws — the node plates and the panel's own preview sphere, which is one more
// cell. The asset browser still uses ModelThumbnailHandler.
//
// One instance per material-graph panel. Every GPU and UI resource it owns is
// named with the instance's own id, so two open material graphs never write each
// other's atlas.

#include "AssetCore/GUID.h"
#include "ShaderGraph/GraphNodePreviewBinding.h"
#include "ShaderGraph/GraphPreviewAtlasLayout.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Handle.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
class UIManager;

namespace Rendering::RenderGraph
{
class RGFrame;
}

namespace Engine::Renderer
{
class RenderServices;
}

namespace Editor
{

class GraphPreviewAtlas
{
  public:
    /// Silhouette fraction of a cell: full-bleed for node plates, framed for the
    /// graph's own preview panel.
    static constexpr float kNodeCellFill = 0.88f;
    static constexpr float kMainPreviewFill = 0.70f;

    struct NodeRequest
    {
        std::string NodeId;
        GUID MaterialGuid{};
        /// Rotation about Y applied to this cell's sphere. Node cells leave it
        /// at zero; the graph's main preview spins and drags on it.
        float YawRadians = 0.f;
        /// Fraction of the cell the sphere spans. Node plates are full-bleed by
        /// design; the graph's preview panel is a framed picture and wants the
        /// margin the thumbnail slot used to give it.
        float FillFraction = kNodeCellFill;
    };

    void Initialize(Engine::Renderer::RenderServices* services);
    void Shutdown();

    /// The node previews the graph panel wants drawn, in graph order. Replaces
    /// the previous set; cells of nodes that disappeared are recycled. Returns
    /// true when the set actually moved, which is the caller's cue to rebind.
    bool SetRequests(uint64_t windowId, std::vector<NodeRequest> requests);

    /// Drops every request and unbinds the atlas from the UI. Called when the
    /// material graph closes or collapses its expanded nodes. Returns true when
    /// requests were actually released, which is the caller's cue to rebind.
    bool ClearRequests();

    /** A rebuild rebinds the whole canvas, which destroys the element under the
        pointer and kills an in-flight drag — so when a gesture owns the mouse
        the caller defers it here and the atlas replays it on release.
        Capped: a wedged gesture must never leave stale plates on screen
        forever, so past the cap the rebuild runs even at the cost of the drag.
        The host only reports whether a gesture is live; the atlas owns the
        policy, because the atlas is what rebuilds. */
    void DeferRebuild() { m_RebuildDeferred = true; }
    /** Consumes a pending deferral and returns true when the caller should
        rebuild now — either the gesture ended or the cap expired. Returns false
        (and keeps the deferral) while a gesture is still live. "Take" because
        it clears the flag: call it once per frame and act on the result. */
    bool TakeDeferredRebuild(bool gestureActive);

    /// Forces a redraw on the next frame (a parameter scrub the digest cannot
    /// see, an IBL toggle, a rebuild).
    void MarkDirty() { m_Dirty = true; }

    /// Spins one cell's sphere. Redraws only when the angle actually moves, so
    /// a still preview leaves the pass out of the frame.
    void SetRequestYaw(const std::string& nodeId, float yawRadians);

    bool TryGetBinding(const std::string& nodeId, GraphNodePreviewBinding& out) const;

    /// Declares the atlas pass into this window's frame when the atlas is dirty
    /// and registers the texture with the window's UIManager.
    void TickRG(uint64_t windowId, UIManager* ui, Rendering::RenderGraph::RGFrame& frame);

    /** Renders cells with each material's IBL keyword variant (environment
        cubes + BRDF LUT bound like the world pass) instead of the base
        single-light pipeline. Governs the atlas cells only; the graph's main
        preview sphere carries its own flag. */
    void SetIblEnabled(bool enabled);

  private:
    struct CellDraw
    {
        uint32_t Cell = 0;
        GUID MaterialGuid{};
        float YawRadians = 0.f;
        float FillFraction = kNodeCellFill;
    };

    // True when the material set, their parameters, or their pipelines moved
    // since the last successful render.
    bool ContentDigestMoved();
    void EnsureView();
    bool EnsureAtlasTexture(uint32_t width, uint32_t height);
    void SubmitPreviewLights();
    Rendering::CameraData BuildCameraData() const;
    /// IBL cells only draw when the feature exists AND has finished building its
    /// cubes; without that gate the keyword is recorded, every draw is skipped,
    /// and the undrawn-retry re-records the pass every frame forever.
    bool IblAvailable() const;

    Engine::Renderer::RenderServices* m_Services = nullptr;

    GraphPreviewAtlasLayout m_Layout;
    std::vector<NodeRequest> m_Requests;
    uint64_t m_WindowId = 0;

    // Per-instance names: the UI texture the thumbs bind and the render-graph
    // resource keys. ImportPersistentTexture is keyed by name, so sharing these
    // across panels would alias two atlases of different sizes onto one texture.
    std::string m_UiResourceName;
    std::string m_AtlasResourceKey;
    std::string m_MsaaResourceKey;
    std::string m_DepthResourceKey;
    std::string m_HdrResourceKey;
    std::string m_PassName;
    std::string m_TonemapPassName;
    // Light world for this atlas's view. Never an ECS world id, and never
    // shared: two panels submitting into one world would sum their key lights.
    uint64_t m_LightWorldId = 0;

    Rendering::TextureHandle m_AtlasTexture{};
    uint32_t m_AtlasWidth = 0;
    uint32_t m_AtlasHeight = 0;
    bool m_AtlasInitialized = false;
    bool m_UiBound = false;

    Rendering::CameraId m_CameraId = 0;
    Rendering::ViewId m_ViewId = 0;

    bool m_Dirty = true;

    bool m_IblEnabled = false;
    bool m_RebuildDeferred = false;
    int m_RebuildDeferredFrames = 0;
    static constexpr int kMaxDeferredRebuildFrames = 90;

    // Record-time skips (cold IBL variants) that must re-declare the pass;
    // written on the render thread, drained in TickRG.
    std::atomic<uint32_t> m_RecordSkippedDraws{0};
    // Cells whose material had no valid base pipeline last render: the pass
    // keeps re-declaring until every requested cell has been drawn once.
    uint32_t m_UndrawnCells = 0;
    uint64_t m_ContentDigest = 0;
};

} // namespace Editor
} // namespace GameEngine
