#include "Placement/FenceMitreVariants.h"

#include "Assets/ModelAsset.h"
#include "Placement/SplineChunkCommit.h"
#include "SplineGeometry/PieceMesh.h"

#include <algorithm>
#include <limits>
#include <map>

namespace GameEngine::Editor
{
namespace
{

using Mathematics::Vector3;

// The mesh key a variant registers under: the recipe entity, the model the
// piece is drawn from and its content, and every quantised number the cut is
// made of.
Rendering::MeshGPUKey VariantMeshKey(uint32 entityId, const FenceMitreSource& source,
                                     const SplineLayout::MitreShape& shape)
{
    std::string name = "engine/mesh/spline-fence-mitre/" + std::to_string(entityId) + "/" +
                       source.Model.ToString() + "/" + std::to_string(source.Content) + "/" +
                       std::to_string(shape.LengthScaleThousandths);
    for (const SplineLayout::MitreEndKey* end : {&shape.Start, &shape.End})
    {
        name += "/" + std::to_string(end->Mitred ? 1 : 0) + "," + std::to_string(end->TurnDeciDegrees) +
                "," + std::to_string(end->InsetTenthMillimetres) + "," +
                std::to_string(end->TopInsetTenthMillimetres) + "," +
                std::to_string(end->StationLocalYTenthMillimetres);
    }
    return {GUID::Derive(GUID::Null(), name), 0u};
}

SplineGeometry::PieceMesh ToPieceMesh(const Mesh& mesh)
{
    SplineGeometry::PieceMesh piece;
    piece.Vertices.resize(mesh.Vertices.size());
    const bool colours = mesh.HasColor0();
    if (colours)
        piece.Colors.resize(mesh.Vertices.size());
    for (size_t i = 0; i < mesh.Vertices.size(); ++i)
    {
        const Vertex& src = mesh.Vertices[i];
        SplineGeometry::SplineVertex& dst = piece.Vertices[i];
        dst.Position = Vector3(src.Position[0], src.Position[1], src.Position[2]);
        dst.Normal = Vector3(src.Normal[0], src.Normal[1], src.Normal[2]);
        dst.UV = Mathematics::Vector2(src.TexCoords[0], src.TexCoords[1]);
        dst.Tangent = Mathematics::Vector4(src.Tangent[0], src.Tangent[1], src.Tangent[2], src.Tangent[3]);
        if (colours)
        {
            piece.Colors[i] = Mathematics::Vector4(mesh.Color0[i * 4u], mesh.Color0[i * 4u + 1u],
                                                   mesh.Color0[i * 4u + 2u], mesh.Color0[i * 4u + 3u]);
        }
    }
    piece.Indices.assign(mesh.Indices.begin(), mesh.Indices.end());
    return piece;
}

// The variant as the registry takes it. Carries the source's vertex colours
// when it has them; the other optional streams and the LOD chain are the
// uncut piece's and do not describe the cut one, so the variant draws its
// cut LOD0 at every distance.
Mesh ToEngineMesh(const SplineGeometry::PieceMesh& piece, const Mesh& source, std::string name)
{
    Mesh mesh;
    mesh.Name = std::move(name);
    mesh.MaterialIndex = source.MaterialIndex;
    mesh.Vertices.resize(piece.Vertices.size());
    const bool colours = source.HasColor0() && piece.HasColors();
    if (colours)
        mesh.Color0.resize(piece.Vertices.size() * 4u);
    Vector3 lo(std::numeric_limits<float32>::max(), std::numeric_limits<float32>::max(),
               std::numeric_limits<float32>::max());
    Vector3 hi = lo * -1.0f;
    for (size_t i = 0; i < piece.Vertices.size(); ++i)
    {
        const SplineGeometry::SplineVertex& src = piece.Vertices[i];
        Vertex& dst = mesh.Vertices[i];
        dst.Position[0] = src.Position.x;
        dst.Position[1] = src.Position.y;
        dst.Position[2] = src.Position.z;
        dst.Normal[0] = src.Normal.x;
        dst.Normal[1] = src.Normal.y;
        dst.Normal[2] = src.Normal.z;
        dst.TexCoords[0] = src.UV.x;
        dst.TexCoords[1] = src.UV.y;
        dst.Tangent[0] = src.Tangent.x;
        dst.Tangent[1] = src.Tangent.y;
        dst.Tangent[2] = src.Tangent.z;
        dst.Tangent[3] = src.Tangent.w;
        if (colours)
        {
            const Mathematics::Vector4& colour = piece.Colors[i];
            mesh.Color0[i * 4u] = colour.x;
            mesh.Color0[i * 4u + 1u] = colour.y;
            mesh.Color0[i * 4u + 2u] = colour.z;
            mesh.Color0[i * 4u + 3u] = colour.w;
        }
        lo = Vector3(std::min(lo.x, src.Position.x), std::min(lo.y, src.Position.y),
                     std::min(lo.z, src.Position.z));
        hi = Vector3(std::max(hi.x, src.Position.x), std::max(hi.y, src.Position.y),
                     std::max(hi.z, src.Position.z));
    }
    mesh.Indices.assign(piece.Indices.begin(), piece.Indices.end());
    // Bounds are folded into the content hash the registry keys its in-place
    // re-upload on, so they are set before registration.
    mesh.MinBounds[0] = lo.x;
    mesh.MinBounds[1] = lo.y;
    mesh.MinBounds[2] = lo.z;
    mesh.MaxBounds[0] = hi.x;
    mesh.MaxBounds[1] = hi.y;
    mesh.MaxBounds[2] = hi.z;
    return mesh;
}

uint64 MeshBytes(const Mesh& mesh)
{
    return mesh.Vertices.size() * sizeof(Vertex) + mesh.Indices.size() * sizeof(uint32) +
           mesh.Color0.size() * sizeof(float);
}

// How many of the plan's pieces draw one of the variants the budget skipped.
uint32 PiecesDrawing(const std::vector<uint32>& pieceVariants, const std::vector<uint8>& skipped)
{
    uint32 count = 0;
    for (const uint32 variant : pieceVariants)
    {
        if (variant != SplineLayout::kNoMitreVariant && skipped[variant] != 0u)
            ++count;
    }
    return count;
}

// A byte budget as the author reads it: whole MB, or whole KB below 1 MB.
std::string BudgetSize(uint64 bytes)
{
    constexpr uint64 kKilobyte = 1024ull;
    constexpr uint64 kMegabyte = kKilobyte * kKilobyte;
    if (bytes < kMegabyte)
        return std::to_string(bytes / kKilobyte) + " KB";
    return std::to_string(bytes / kMegabyte) + " MB";
}

// The pool piece a variant is cut from: the one its role and slot name.
const FenceMitreSource& SourceOf(const FenceMitreSources& sources,
                                 const SplineLayout::MitreVariantKey& variant)
{
    switch (variant.Role)
    {
    case SplineLayout::MitrePieceRole::Crest:
        return sources.Crests[variant.PoolSlot];
    case SplineLayout::MitrePieceRole::Gate:
        return sources.Gates[variant.PoolSlot];
    case SplineLayout::MitrePieceRole::Span:
    default:
        return sources.Spans[variant.PoolSlot];
    }
}

} // namespace

FenceMitreVariants CommitFenceMitreVariants(Rendering::MeshGPURegistry& registry,
                                            SplineChunkCommit& commit, FenceMitreCuts& cuts,
                                            uint32 entityId,
                                            const SplineLayout::MitreVariantPlan& plan,
                                            const FenceMitreSources& sources,
                                            uint64 byteBudget)
{
    FenceMitreVariants out;
    out.Meshes.resize(plan.Variants.size());
    FenceMitreCuts reached;
    // The first plan variant each mesh key was made for: two pool slots
    // holding one model name one mesh, registered and counted once.
    std::unordered_map<Rendering::MeshGPUKey, size_t> variantOfKey;
    // One line per mesh the cut found open, however many of its variants.
    std::map<std::string, uint32> openLoopsByMesh;
    uint64 bytes = 0;
    bool budgetSpent = false;
    std::vector<uint8> skipped(plan.Variants.size(), 0u);
    for (size_t v = 0; v < plan.Variants.size(); ++v)
    {
        if (budgetSpent)
        {
            skipped[v] = 1u;
            continue;
        }
        const SplineLayout::MitreVariantKey& variant = plan.Variants[v];
        const FenceMitreSource& source = SourceOf(sources, variant);
        // Nothing to cut: the handle stays invalid and the span draws uncut.
        if (!source.SourceMesh)
            continue;
        const Rendering::MeshGPUKey key = VariantMeshKey(entityId, source, variant.Shape);
        if (const auto same = variantOfKey.find(key); same != variantOfKey.end())
        {
            out.Meshes[v] = out.Meshes[same->second];
            continue;
        }
        variantOfKey.emplace(key, v);
        FenceMitreMesh& drawn = out.Meshes[v];
        const auto known = cuts.find(key);
        FenceMitreCut cut;
        Rendering::MeshGPUHandle kept;
        const bool keep = known != cuts.end() && bytes + known->second.Bytes <= byteBudget &&
                          commit.Keep(key, kept);
        if (keep)
        {
            cut = known->second;
            drawn.Handle = kept;
        }
        else
        {
            const SplineLayout::MitreVariant variantMesh = SplineLayout::BuildMitreVariant(
                ToPieceMesh(*source.SourceMesh), source.Bounds, variant.Shape);
            const Mesh mesh = ToEngineMesh(variantMesh.Mesh, *source.SourceMesh,
                                           "SplineFenceMitre_" + std::to_string(entityId) + "_" +
                                               std::to_string(v));
            cut.Bytes = MeshBytes(mesh);
            if (bytes + cut.Bytes > byteBudget)
            {
                budgetSpent = true;
                skipped[v] = 1u;
                continue;
            }
            drawn.Handle = commit.Register(registry, key, mesh);
            cut.Bounds = Mathematics::BoundingBox::FromMinMax(
                Vector3(mesh.MinBounds[0], mesh.MinBounds[1], mesh.MinBounds[2]),
                Vector3(mesh.MaxBounds[0], mesh.MaxBounds[1], mesh.MaxBounds[2]));
            cut.OpenLoops = variantMesh.OpenLoops;
        }
        bytes += cut.Bytes;
        drawn.Bounds = cut.Bounds;
        reached[key] = cut;
        if (cut.OpenLoops > 0u)
        {
            const std::string name = "'" + source.Name + "' (" + source.Model.ToString() + ")";
            uint32& worst = openLoopsByMesh[name];
            worst = std::max(worst, cut.OpenLoops);
        }
    }
    commit.RetireUnreached(registry);
    cuts = std::move(reached);

    for (const auto& [name, loops] : openLoopsByMesh)
    {
        out.Report.push_back("Fence: the mesh " + name + " is open where a join cuts it (" +
                             std::to_string(loops) +
                             " open edge chain(s)), so its cut end shows a hole; close the mesh "
                             "or plant a post at those joins");
    }
    out.PiecesPastBudget =
        PiecesDrawing(plan.SpanVariants, skipped) + PiecesDrawing(plan.CrestVariants, skipped);
    if (out.PiecesPastBudget > 0u)
    {
        out.Report.push_back(
            "Fence: " + std::to_string(out.PiecesPastBudget) + " mitred piece(s) are past the " +
            BudgetSize(byteBudget) +
            " budget for cut join variants and draw uncut, so their tops overlap at the join — "
            "every distinct turn is its own variant: smooth the curve so its joins repeat, or plant "
            "posts at the joins");
    }
    return out;
}

FencePoolPiece MitreVariantPiece(const FencePoolPiece& pool, const FenceMitreMesh& variant)
{
    FencePoolPiece piece = pool;
    piece.Renderer.meshGpuHandleId = static_cast<uint64>(variant.Handle);
    piece.Bounds.Box = variant.Bounds;
    return piece;
}

uint64 FencePoolContentDigest(const Rendering::MeshGPURegistry& registry,
                              std::span<const GUID> models)
{
    constexpr uint64 kFnvPrime = 0x100000001b3ull;
    uint64 digest = 0xcbf29ce484222325ull;
    const Rendering::MeshGPURegistry::TableScope scope(registry);
    for (const GUID& model : models)
    {
        const Rendering::MeshGPUEntry* entry = registry.FindByKey({model, 0u});
        digest = (digest ^ (entry ? entry->contentHash : 0ull)) * kFnvPrime;
    }
    return digest;
}

} // namespace GameEngine::Editor
