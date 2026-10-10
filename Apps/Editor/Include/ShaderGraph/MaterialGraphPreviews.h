#pragma once

#include "Editor/RenderGraphTickRegistry.h"
#include "Graph/GraphModel.h"
#include "AssetCore/GUID.h"
#include "ShaderGraph/GraphNodePreviewBinding.h"
#include "ShaderGraph/GraphPreviewAtlas.h"
#include "ShaderGraph/MaterialGraphNodePreviewMaterials.h"

namespace GameEngine {

class GraphCanvas;
class UIManager;
struct EditorContext;

namespace Editor {

/**
 * Owns everything the material graph needs to draw a preview on each node: the
 * GPU atlas, its render-graph tick registration, and the per-node preview
 * materials the atlas renders.
 *
 * MaterialGraphController holds one of these. The panel never reaches past
 * the controller into an atlas or a preview material.
 */
class MaterialGraphPreviews
{
  public:
    ~MaterialGraphPreviews() { Shutdown(); }

    /** Stands the atlas up against the editor's render-graph tick. Safe to call
        when already initialized. */
    void Initialize(const EditorContext* ctx, bool iblEnabled);

    /** Idempotent by contract: the atlas's own release callback runs it during
        renderer teardown, and the owner's destructor runs it again once
        RenderServices is already gone, by which point no handle may survive. */
    void Shutdown();

    bool IsInitialized() const { return m_Initialized; }

    /** Brings the node preview materials and the atlas in line with `model`.
        Cheap to call on every compile: an edit that cannot change a compiled
        surface costs one digest, and a collapsed view costs nothing at all.

        Collapsing releases the atlas cells but keeps the materials, so
        re-expanding is a request sync and a redraw rather than a recompile of
        the whole set. */
    void Sync(const EditorContext* ctx, const Graph::Model& model, bool expandedView,
              GraphCanvas* canvas, bool gestureActive);

    /** The graph's own preview sphere draws from the same atlas pass as the
        node plates: one more cell, one more draw, no thumbnail slot. Pass a
        null GUID to release it. */
    void SetMainPreviewMaterial(const GUID& materialGuid);

    /** Sprite addressing for the main preview's cell, false while it owns none. */
    bool TryGetMainPreviewBinding(GraphNodePreviewBinding& out) const;

    /** Spins the main preview's sphere. */
    void SetMainPreviewYaw(float yawRadians);

    /** The key the main preview's cell is filed under. Not a node id: node ids
        are "node_N" from the document, so the two can never collide. */
    static constexpr const char* kMainPreviewKey = "__graph_main_preview";

    /** Pushes the model's current parameter values into the node preview
        materials and redraws the atlas. Values reach the shader as uniforms, so
        this is the whole cost of a scrub: no compile, no file, no rebind. */
    void SyncValues(const EditorContext* ctx, const Graph::Model& model);

    /** A rebuild rebinds the whole canvas, so it is deferred while a pointer
        gesture owns the mouse; the atlas owns that policy. */
    GraphPreviewAtlas& Atlas() { return m_Atlas; }

  private:
    /** Points the atlas at the current node set. Rebinds the canvas when cell
        assignment moved, because a moved cell invalidates every bound sprite
        rect and a node would otherwise paint a cell that now belongs to
        another. */
    void SyncRequests(const EditorContext* ctx, const Graph::Model& model, GraphCanvas* canvas,
                      bool includeNodes);

    GraphPreviewAtlas m_Atlas;
    MaterialGraphNodePreviewMaterials m_NodeMaterials;
    /** The graph's live preview material, drawn into its own cell. */
    GUID m_MainPreviewMaterial{};
    RenderGraphTickRegistry::Handle m_Tick = RenderGraphTickRegistry::kInvalidHandle;
    bool m_Initialized = false;
    const EditorContext* m_Context = nullptr;
};

} // namespace Editor
} // namespace GameEngine
