#pragma once

#include "Mathematics/Vector3.h"
#include "SplineGeometry/PieceMesh.h"
#include "Types/Types.h"

namespace GameEngine::SplineGeometry
{

// A plane in a mesh's local frame. A point p is on it where
// Dot(Normal, p) == Offset; the side Normal points to is the side a cut
// removes. Normal need not be unit length.
struct CutPlane
{
    Mathematics::Vector3 Normal{0.0f, 0.0f, 1.0f};
    float32 Offset = 0.0f;
};

struct MeshCutResult
{
    PieceMesh Mesh;
    // Closed loops the plane cut through the surface, each closed with a cap.
    uint32 CappedLoops = 0;
    // Chains of cut edges that do not close — the piece has a hole or an
    // unwelded seam where the plane crosses it. Left open: a cap on an open
    // chain has no outline to fill.
    uint32 OpenLoops = 0;
};

// Removes everything on the plane's positive side and caps each closed loop
// the cut leaves in the surface.
//
// Triangles the plane crosses are clipped, and the new vertices on their
// edges carry every attribute interpolated along that edge. The cut edges are
// chained into loops by position, so a hard edge whose vertices are split for
// their normals or UVs still closes. Each closed loop is capped by ear
// clipping — an outline with holes inside it (a hollow piece cut across) is
// capped as one polygon with those holes — with the cap's normal along the
// plane's and each cap vertex taking the colour of the cut edge it sits on.
// The cap's UV continues the unwrap of one cut face across the edge it shares
// with the cap, at the piece's texel density, and its tangent frame follows
// that unwrap. A triangle lying in the plane is dropped: the cap replaces it.
//
// A plane that leaves every vertex on its kept side returns the mesh
// unchanged, bit for bit.
MeshCutResult CutMeshByPlane(const PieceMesh& mesh, const CutPlane& plane);

} // namespace GameEngine::SplineGeometry
