#pragma once

#include "Mathematics/Geometry.h"
#include "Types/Types.h"

namespace GameEngine {
struct Mesh;

enum class MeshLODGeometryIssue : uint8 {
    None,
    InvalidBaseBounds,
    UnsupportedStreams,
    InvalidLowerGeometry,
    EmptyLevel
};

// Derived from the current mesh; never serialized. Bounds retains the LOD0
// center so enlarging the culling envelope does not move the LOD reference.
// LevelCount is a contiguous prefix (including LOD0), or zero for an empty base
// or invalid base bounds. Base geometry retains the importer's validation
// contract. Unsupported/invalid lower geometry falls back to LOD0; an empty level
// terminates the prefix without renumbering subsequent authored thresholds.
struct MeshLODGeometry {
    Mathematics::BoundingBox ReferenceBounds{};
    Mathematics::BoundingBox Bounds{};
    uint32 LevelCount = 0;
    MeshLODGeometryIssue Issue = MeshLODGeometryIssue::None;
};

MeshLODGeometry ResolveMeshLODGeometry(const Mesh& mesh);
} // namespace GameEngine
