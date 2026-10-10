#pragma once

#include "Types/Types.h"
#include "Mathematics/Vector3.h"

namespace GameEngine::Spline
{

// Interpolation method for the spline curve.
enum class SplineType : uint8
{
    CatmullRom = 0,  // Smooth, auto-tangent (recommended default)
    CubicBezier = 1, // Explicit tangent control via handles
    Linear = 2,      // Straight segments between points
};

// Control point for a spline.
//
// For CatmullRom: TangentIn/TangentOut are computed automatically from
// neighboring points and cached here. Authors only set Position + Radius.
// For CubicBezier: TangentIn/TangentOut are explicit control handle offsets
// (relative to Position) that the artist manipulates.
// For Linear: TangentIn/TangentOut are unused (zero).
struct SplineControlPoint
{
    Mathematics::Vector3 Position;  // Entity-local; callers apply the owner's WorldTransform
    float32 Radius = 5.0f;         // Swept radius (road half-width, tunnel radius)
    Mathematics::Vector3 TangentIn; // Incoming tangent (Bezier) or cached (CatmullRom)
    float32 Roll = 0.0f;           // Bank angle in radians around the forward axis
    Mathematics::Vector3 TangentOut;// Outgoing tangent (Bezier) or cached (CatmullRom)
    float32 _Pad0 = 0.0f;
    Mathematics::Vector3 Rotation;  // Per-point authored Euler rotation, in degrees.
    Mathematics::Vector3 Scale{1.0f, 1.0f, 1.0f}; // Per-point authored XYZ scale.
};

static_assert(sizeof(SplineControlPoint) == 72, "SplineControlPoint size changed unexpectedly");

// Result of evaluating a spline at a parametric position.
struct SplineFrame
{
    Mathematics::Vector3 Position;
    Mathematics::Vector3 Forward;   // Tangent direction (normalized)
    Mathematics::Vector3 Right;     // Perpendicular to forward, in XZ plane (unless rolled)
    Mathematics::Vector3 Up;        // Cross(forward, right)
    float32 Radius;                 // Interpolated radius at this point
    float32 Roll;                   // Interpolated roll at this point
    Mathematics::Vector3 Rotation;  // Interpolated authored Euler rotation, in degrees.
    Mathematics::Vector3 Scale{1.0f, 1.0f, 1.0f}; // Interpolated authored XYZ scale.
};

// Result of a closest-point-on-spline query.
struct ClosestPointResult
{
    float32 T;                      // Global parametric t in [0, 1]
    Mathematics::Vector3 Position;  // Closest point on the spline
    float32 Distance;               // Distance from query point to closest point
    uint32 SegmentIndex;            // Which segment contains the closest point
};

} // namespace GameEngine::Spline
