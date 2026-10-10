#pragma once

#include "Mathematics/Curve.h"

#include <cstdint>
#include <vector>

namespace GameEngine
{
// Editor-side helpers for tangent (smooth-key) editing in a CurveField. Pure operations on
// a Time-sorted CurveKey vector; the slope math itself lives in Math::ResolveKeySlope. The
// screen-space geometry of the tangent arms stays in CurveField (it owns the transforms).
namespace CurveEdit
{

// Synthetic handle IDs the curve editor passes around: a point handle is its key index
// (>= 0); a key's in/out tangent handles are encoded above the point-index range so one
// hit-test can return either kind.
inline constexpr int kInTangentBase = 1 << 14;  // 16384
inline constexpr int kOutTangentBase = 1 << 15; // 32768

inline int InTangentHandle(int keyIndex) { return kInTangentBase + keyIndex; }
inline int OutTangentHandle(int keyIndex) { return kOutTangentBase + keyIndex; }
inline bool IsTangentHandle(int handle) { return handle >= kInTangentBase; }
inline bool IsIncomingTangent(int handle) { return handle >= kInTangentBase && handle < kOutTangentBase; }
inline int TangentHandleKey(int handle)
{
    return handle >= kOutTangentBase ? handle - kOutTangentBase : handle - kInTangentBase;
}

// Does the segment that starts at key `segStart` render as a smooth (Hermite) curve?
bool SegmentIsSmooth(const std::vector<Math::CurveKey>& keys, int segStart);

// Whether either segment touching `keyIndex` is smooth (so this key shows tangent arms).
bool KeyHasTangents(const std::vector<Math::CurveKey>& keys, int keyIndex);

// Set a key's interpolation + tangent mode, seeding the stored In/OutTangent from the
// current auto slope so switching to Mirrored/Manual/Broken does not visually jump.
void SetKeyMode(std::vector<Math::CurveKey>& keys, int keyIndex,
                Math::CurveInterp interp, Math::CurveTangentMode mode);

// Apply a dragged tangent slope to a key. When `broken` is false the in/out tangents are kept equal
// (Mirrored); when true only the dragged side is set and the key switches to Broken. The CurveField
// caller passes broken when the key is already in Broken mode. Forces the touching segment(s) to
// Smooth so the tangent is visible.
void ApplyTangentSlope(std::vector<Math::CurveKey>& keys, int keyIndex, bool incoming,
                       float slope, bool broken);

} // namespace CurveEdit
} // namespace GameEngine
