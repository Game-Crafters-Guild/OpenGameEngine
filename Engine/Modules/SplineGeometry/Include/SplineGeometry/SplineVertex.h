#pragma once

#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"

namespace GameEngine::SplineGeometry
{

// Mirrors Assets::Vertex field for field (Position[3], Normal[3], TexCoords[2],
// Tangent[4]) so the conversion at the engine boundary is a copy, while this
// module stays free of the asset and rendering headers and remains unit
// testable on its own.
//
// Shared by every emitter in the module — the swept strip and the region fill
// both produce it, and the controller's conversion to the engine mesh does not
// care which one built it.
struct SplineVertex
{
    Mathematics::Vector3 Position{};
    Mathematics::Vector3 Normal{};
    Mathematics::Vector2 UV{};
    // xyz direction plus handedness in w, the glTF convention the engine's
    // vertex already uses. Analytic rather than derived by a MikkTSpace pass:
    // both emitters know the surface frame they are placing a vertex into.
    Mathematics::Vector4 Tangent{0.0f, 0.0f, 1.0f, 1.0f};
};

} // namespace GameEngine::SplineGeometry
