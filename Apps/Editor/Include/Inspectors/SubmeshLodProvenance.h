#pragma once

namespace GameEngine
{
struct Mesh;
}

namespace GameEngine::Editor
{

// What a submesh's LOD chain renders as, which is not always what its CPU data holds: the GPU
// upload admits levels only through ResolveMeshLODGeometry, and a refused chain keeps its CPU
// levels while drawing LOD0 alone. The inspector badges that divergence, so the classification
// asks the shared resolver instead of re-deriving the admission rule. Kept in its own TU, like
// the inspector's other notice helpers, so the rule the badge reports is reachable from a test
// rather than only from a running editor.
//
// A model may mix authored and generated chains across its submeshes, so provenance is per
// submesh and is derived from mesh state alone — no persistent label is stored.
enum class SubmeshLodProvenance
{
    Lod0Only,        // no simplified levels
    Generated,       // simplified levels, index-only (no authored vertex blocks)
    Authored,        // authored vertex blocks, honored by the GPU upload
    AuthoredDropped, // authored on the CPU but drawn LOD0-only
};

SubmeshLodProvenance ClassifySubmeshLod(const Mesh& mesh);

} // namespace GameEngine::Editor
