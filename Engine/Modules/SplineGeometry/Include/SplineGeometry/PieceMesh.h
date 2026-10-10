#pragma once

#include "Mathematics/Vector4.h"
#include "SplineGeometry/SplineVertex.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::SplineGeometry
{

// An indexed triangle list in a kit piece's own local frame, as the geometry
// that reshapes a piece reads and writes it: the module's vertex, plus the
// piece's vertex colour where it has one.
struct PieceMesh
{
    std::vector<SplineVertex> Vertices;
    // Empty, or one RGBA colour per vertex.
    std::vector<Mathematics::Vector4> Colors;
    std::vector<uint32> Indices;

    bool HasColors() const { return !Colors.empty() && Colors.size() == Vertices.size(); }
};

} // namespace GameEngine::SplineGeometry
