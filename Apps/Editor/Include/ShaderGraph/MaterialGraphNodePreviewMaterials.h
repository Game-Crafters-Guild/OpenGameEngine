#pragma once

#include "AssetCore/GUID.h"
#include "Graph/GraphModel.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace GameEngine {

struct EditorContext;

namespace Editor {

/**
 * The preview material behind each node of a material graph: the graph re-wired
 * so that node drives the Output, materialized to its own surface and
 * registered as a material the preview atlas can draw.
 *
 * Regenerating all of them costs one graph compile per node, so this keeps two
 * digests and does the work only when one moves: the model's surface digest
 * gates the whole pass, and each node's materialized source gates its own file
 * write and shader compile.
 *
 * Both digests are over the MODEL. The helper-node library the materializer
 * compiles against is loaded once per session and has no runtime invalidation
 * path, so nothing here watches it; whoever gives helper nodes live reload owes
 * this class a way to drop m_ModelDigest with it.
 */
class MaterialGraphNodePreviewMaterials
{
  public:
    struct SyncResult
    {
        /** A node gained or lost a material — the atlas needs new requests, and
            the canvas has to rebind because cell assignment moved. */
        bool SetChanged = false;
        /** At least one variant was regenerated, so its cell will redraw once
            the new pipeline publishes. */
        bool SourcesChanged = false;
    };

    /** Brings every node's preview material in line with `model`, regenerating
        only what changed. Cheap enough to call on every compile. */
    SyncResult Sync(const EditorContext* ctx, const Graph::Model& model);

    /** True when Sync would regenerate something. Lets a caller that cannot
        afford the rebind right now (a live pointer gesture) find out without
        paying for it. */
    bool NeedsSync(const Graph::Model& model) const;

    /** Pushes the model's current parameter values into the existing materials.
        Values reach the shader as uniforms, so this needs no shader work and no
        cache invalidation. Returns true when anything moved. */
    bool RefreshProperties(const EditorContext* ctx, const Graph::Model& model);

    /** Drops every material and both digests, so the next Sync rebuilds from
        nothing. Collapsing the expanded view does NOT do this — the materials
        outlive it so re-expanding costs nothing. */
    void Clear();

    const std::unordered_map<std::string, GUID>& Materials() const { return m_Materials; }
    bool Empty() const { return m_Materials.empty(); }

  private:
    struct Entry
    {
        GUID Material{};
        /** Hash of the GLSL this node's variant materialized to. */
        std::uint64_t SourceHash = 0;
    };

    std::unordered_map<std::string, Entry> m_Entries;
    /** Flattened view of m_Entries for the atlas, rebuilt whenever the set moves. */
    std::unordered_map<std::string, GUID> m_Materials;
    /** MaterialGraphSurfaceDigest of the PREVIEW projection the entries were
        built from, and
        whether there is one — a graph whose nodes all failed to materialize has
        no entries but has still been synced. */
    std::uint64_t m_ModelDigest = 0;
    bool m_HasModelDigest = false;
    /** Serialized parameter values the last property push saw. */
    std::string m_PropsJson;
};

} // namespace Editor
} // namespace GameEngine
