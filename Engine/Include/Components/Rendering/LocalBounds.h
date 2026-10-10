#pragma once

#include "Mathematics/Geometry.h"

namespace GameEngine {
namespace Components {

// Local-space culling envelope for an entity's mesh, plus rendering metadata.
// Box is the CONSERVATIVE envelope: it encloses every LOD level the mesh draws,
// not just LOD0, so a lower level that reaches outside LOD0's box survives the
// frustum edge. It is therefore not the right box to measure the visible
// silhouette with; LOD selection uses the mesh table's own LOD0 reference metric.
// Consumers that need world-space bounds should call box.TransformToAABB() with
// the entity's WorldTransform matrix.
// [DoNotSerialize] — Box is derived from mesh geometry, never authored: seeded at
// model-load resolve and re-derived by RefreshLocalBoundsAfterMeshReload when a
// hot-reload swaps geometry under the entity's mesh handle. DynamicObject and
// CastShadows ARE authored and survive that refresh.
struct LocalBounds {
    // Derived from the mesh: switching the renderer off is what hides it.
    static constexpr bool NotToggleable = true;

    Mathematics::BoundingBox Box;
    bool DynamicObject {true};
    bool CastShadows {true};
};

} // namespace Components
} // namespace GameEngine
