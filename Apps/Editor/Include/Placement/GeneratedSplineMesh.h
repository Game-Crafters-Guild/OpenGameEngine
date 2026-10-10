#pragma once

#include "Assets/ModelAsset.h"
#include "SplineGeometry/SplineVertex.h"

#include <string>

namespace GameEngine::Editor
{

// Copy a generated spline mesh into the engine's interleaved CPU mesh. The
// geometry module mirrors Assets::Vertex field for field precisely so this stays
// a copy rather than a conversion. Templated over the mesh shapes the module
// emits for the swept recipes — the swept strip and the filled region — which
// carry the same vertices, indices and bounds.
template <typename GeneratedMesh>
Mesh ToEngineMesh(const GeneratedMesh& generated, const std::string& name)
{
    Mesh mesh;
    mesh.Name = name;
    mesh.MaterialIndex = 0;
    mesh.Vertices.resize(generated.Vertices.size());
    for (size_t i = 0; i < generated.Vertices.size(); ++i)
    {
        const SplineGeometry::SplineVertex& src = generated.Vertices[i];
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
    }
    mesh.Indices.assign(generated.Indices.begin(), generated.Indices.end());
    // Bounds have no default initialiser and are folded into the content hash
    // the registry keys its in-place re-upload on, so leaving them unset is both
    // a wrong AABB and a corrupted change detector.
    mesh.MinBounds[0] = generated.MinBounds.x;
    mesh.MinBounds[1] = generated.MinBounds.y;
    mesh.MinBounds[2] = generated.MinBounds.z;
    mesh.MaxBounds[0] = generated.MaxBounds.x;
    mesh.MaxBounds[1] = generated.MaxBounds.y;
    mesh.MaxBounds[2] = generated.MaxBounds.z;
    return mesh;
}

} // namespace GameEngine::Editor
