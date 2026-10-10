#pragma once

#include "AssetCore/GUID.h"

namespace GameEngine { struct Mesh; }

namespace GameEngine::Editor::Picking
{

// Resolve a built-in primitive (cube, sphere, capsule, plane) by its
// well-known GUID and return a process-cached CPU mesh. Returns nullptr if
// the GUID is not a primitive.
//
// Primitives are not stored as ModelAssets — PrimitiveGenerator stamps a
// well-known GUID into the MeshRenderer and uploads directly to
// MeshGPURegistry. For CPU picking we materialize the geometry on demand.
const Mesh* ResolvePrimitiveMesh(const GUID& guid);

}
