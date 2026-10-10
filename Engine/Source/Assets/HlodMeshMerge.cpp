#include "Assets/HlodMeshMerge.h"

#include "Assets/ModelAsset.h"
#include "Logger/Logger.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace GameEngine {
namespace Hlod {

namespace {

// Bake one member's coarsest-LOD geometry into `out`, transformed by `xf`
// (column-major). Mirrors MergeStaticPrimitivesByMaterial's per-vertex math:
// position M·p, normal inverse-transpose (renormalized), tangent R (renormalized)
// with handedness flipped on a mirrored (det<0) transform, and index winding
// flipped to match. `outWantsColor` / `outWantsUv1` gate whether the optional
// streams are accumulated (decided once per material group).
void BakeMemberInto(Mesh& out, const MergeMember& member, bool outWantsColor, bool outWantsUv1) {
    const Mesh& src = *member.Source;
    const glm::mat4 M = glm::make_mat4(member.Transform);
    const glm::mat3 R(M);
    const glm::mat3 Rit = glm::transpose(glm::inverse(R));
    const bool flipWinding = glm::determinant(R) < 0.0f;

    const uint32 vertexBase = static_cast<uint32>(out.Vertices.size());
    // An own-vertex chosen level (authored LOD or generated attribute-honest
    // sloppy shell) carries LOD-LOCAL indices into its own block — bake THAT
    // block, not src.Vertices, or the local indices would address the wrong
    // vertices. Authored own-vertex colours use the same selected level.
    const bool ownVerts = member.ChosenLod >= 1u &&
                          (member.ChosenLod - 1u) < src.ExtraLODVertices.size() &&
                          !src.ExtraLODVertices[member.ChosenLod - 1u].empty();
    const Vector<Vertex>& srcVerts =
        ownVerts ? src.ExtraLODVertices[member.ChosenLod - 1u] : src.Vertices;
    const Vector<float>* srcColours = ownVerts
        ? (member.ChosenLod - 1u < src.ExtraLODColor0.size() ? &src.ExtraLODColor0[member.ChosenLod - 1u] : nullptr)
        : &src.Color0;
    const bool srcHasColor = srcColours && srcColours->size() == srcVerts.size() * 4u;
    const bool srcHasUv1 = !ownVerts && src.HasTexCoords1();

    for (size_t vi = 0; vi < srcVerts.size(); ++vi) {
        const Vertex& sv = srcVerts[vi];
        Vertex dv = sv;

        const glm::vec4 p(sv.Position[0], sv.Position[1], sv.Position[2], 1.0f);
        const glm::vec4 pT = M * p;
        dv.Position[0] = pT.x; dv.Position[1] = pT.y; dv.Position[2] = pT.z;

        const glm::vec3 n = Rit * glm::vec3(sv.Normal[0], sv.Normal[1], sv.Normal[2]);
        const float nLen = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (nLen > 1e-8f) {
            const float inv = 1.0f / nLen;
            dv.Normal[0] = n.x * inv; dv.Normal[1] = n.y * inv; dv.Normal[2] = n.z * inv;
        }

        const glm::vec3 t0(sv.Tangent[0], sv.Tangent[1], sv.Tangent[2]);
        if (t0.x * t0.x + t0.y * t0.y + t0.z * t0.z > 1e-12f) {
            const glm::vec3 t = R * t0;
            const float tLen = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
            if (tLen > 1e-8f) {
                const float inv = 1.0f / tLen;
                dv.Tangent[0] = t.x * inv; dv.Tangent[1] = t.y * inv; dv.Tangent[2] = t.z * inv;
            }
        }
        // Bitangent handedness: a mirrored transform flips cross(N,T) orientation.
        dv.Tangent[3] = flipWinding ? -sv.Tangent[3] : sv.Tangent[3];

        for (int c = 0; c < 3; ++c) {
            out.MinBounds[c] = std::min(out.MinBounds[c], dv.Position[c]);
            out.MaxBounds[c] = std::max(out.MaxBounds[c], dv.Position[c]);
        }
        out.Vertices.push_back(dv);

        if (outWantsColor) {
            if (srcHasColor) {
                const float* rgba = srcColours->data() + vi * 4u;
                out.Color0.insert(out.Color0.end(), rgba, rgba + 4u);
            } else {
                out.Color0.insert(out.Color0.end(), {1.0f, 1.0f, 1.0f, 1.0f});
            }
        }
        if (outWantsUv1) {
            if (srcHasUv1) {
                const float* uv = src.TexCoords1.data() + vi * 2u;
                out.TexCoords1.insert(out.TexCoords1.end(), uv, uv + 2u);
            } else {
                out.TexCoords1.insert(out.TexCoords1.end(), {0.0f, 0.0f});
            }
        }
    }

    const Vector<uint32>& indices = src.LODIndices(member.ChosenLod);
    const size_t triCount = indices.size() / 3;
    out.Indices.reserve(out.Indices.size() + triCount * 3);
    for (size_t ti = 0; ti < triCount; ++ti) {
        const uint32 i0 = indices[ti * 3 + 0] + vertexBase;
        const uint32 i1 = indices[ti * 3 + 1] + vertexBase;
        const uint32 i2 = indices[ti * 3 + 2] + vertexBase;
        if (flipWinding) {
            out.Indices.push_back(i0);
            out.Indices.push_back(i2);
            out.Indices.push_back(i1);
        } else {
            out.Indices.push_back(i0);
            out.Indices.push_back(i1);
            out.Indices.push_back(i2);
        }
    }
}

} // namespace

MergedProxy MergeClusterMembers(std::span<const MergeMember> members) {
    MergedProxy proxy;

    // Group member indices by material GUID; std::map keeps submesh emission in
    // ascending GUID order (determinism) and member indices ascending within.
    std::map<GUID, Vector<size_t>> byMaterial;
    for (size_t i = 0; i < members.size(); ++i) {
        if (members[i].Source != nullptr) {
            if (members[i].Source->HasOwnVertexLODs() && !members[i].Source->HasValidLODColor0()) {
                Logger::Log::Warning("HLOD: mesh '{}' has invalid per-level RGBA; refusing proxy merge.",
                                     members[i].Source->Name);
                return {};
            }
            byMaterial[members[i].MaterialGuid].push_back(i);
        }
    }

    uint32 submeshOrdinal = 0;
    for (auto& [materialGuid, group] : byMaterial) {
        // A submesh keeps an optional stream only if some member in the group
        // has it (matching the reference merge's group-level decision).
        bool wantColor = false;
        bool wantUv1 = false;
        for (size_t gi : group) {
            wantColor = wantColor || members[gi].Source->HasColor0();
            wantUv1 = wantUv1 || members[gi].Source->HasTexCoords1();
        }

        Mesh out;
        out.Name = "hlod_proxy_mat_" + std::to_string(submeshOrdinal);
        out.MaterialIndex = submeshOrdinal;
        out.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
        out.SourceNodeIndex = -1;
        out.MinBounds[0] = out.MinBounds[1] = out.MinBounds[2] = std::numeric_limits<float>::max();
        out.MaxBounds[0] = out.MaxBounds[1] = out.MaxBounds[2] = std::numeric_limits<float>::lowest();

        for (size_t gi : group)
            BakeMemberInto(out, members[gi], wantColor, wantUv1);

        proxy.CoreVertexBytes += static_cast<uint64>(out.Vertices.size()) * sizeof(Vertex);
        proxy.Submeshes.push_back(std::move(out));
        proxy.SubmeshMaterials.push_back(materialGuid);
        ++submeshOrdinal;
    }

    return proxy;
}

} // namespace Hlod
} // namespace GameEngine
