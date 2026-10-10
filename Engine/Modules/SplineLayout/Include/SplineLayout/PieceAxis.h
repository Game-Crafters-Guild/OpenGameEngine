#pragma once

#include "Mathematics/Vector3.h"
#include "Types/Types.h"

namespace GameEngine::SplineLayout
{

// Which of a kit piece's local horizontal axes runs ALONG the path it is placed
// on. The other horizontal axis is then its thickness, across the path.
enum class PieceAxis : uint8
{
    X,
    Z,
};

// How far one horizontal extent must exceed the other before it counts as the
// piece's run direction. Below it the piece is near-square and its bounds carry
// no authoring intent: the measured footpath tile is 2.62 x 2.38 m (1.10) and
// the kits' standalone posts are 1.00-1.01, while every measured piece that
// genuinely runs somewhere clears it with room — 1.87 for the shortest hedge,
// 2.79 for the short stone wall, 15.8 for a fence panel.
inline constexpr float32 kPieceAxisDominance = 1.25f;

// The single rule for a piece's along-path axis. Orientation and length
// measurement both go through this, so neither can answer it differently:
// measuring a 2.47 x 0.157 m fence panel on the axis it was NOT laid along
// packed 15.7 pieces where one belonged.
//
// Near-square pieces fall back to Z, the engine's forward axis: their choice is
// modelling noise either way, and Z is what every placed piece used before one
// could declare otherwise.
inline PieceAxis ChoosePieceAxis(const Mathematics::Vector3& halfExtents)
{
    // A NaN extent compares false and lands on Z rather than propagating an
    // unanswered question into the pose basis.
    return halfExtents.x > halfExtents.z * kPieceAxisDominance ? PieceAxis::X : PieceAxis::Z;
}

// The piece's authored length: its extent along its own axis.
inline float32 PieceLength(const Mathematics::Vector3& halfExtents, PieceAxis axis)
{
    return (axis == PieceAxis::X ? halfExtents.x : halfExtents.z) * 2.0f;
}

// Per-local-axis scale for a piece stretched to fill its station gap. Only the
// along-path axis stretches, so which local axis carries the scale is the same
// question PieceLength answers — asked once, here, and read by both the pose
// basis and the emitted transform.
struct PieceAxisScale
{
    float32 X = 1.0f;
    float32 Z = 1.0f;
};

inline PieceAxisScale MakePieceAxisScale(PieceAxis axis, float32 lengthScale)
{
    PieceAxisScale scale;
    (axis == PieceAxis::X ? scale.X : scale.Z) = lengthScale;
    return scale;
}

} // namespace GameEngine::SplineLayout
