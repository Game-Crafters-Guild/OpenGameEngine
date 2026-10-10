#pragma once

// Builds the sparse MorphTarget (Assets/ModelAsset.h) that the glTF and FBX loaders store from the
// per-vertex deltas they read, so both loaders decide which vertices a target moves by one rule.

#include "Types/Types.h"

#include <span>

namespace GameEngine
{

struct MorphTarget;

/**
 * @brief The sparse form of one morph target given as per-vertex deltas.
 *
 * Each span holds x, y, z in engine axes for each of `vertexCount` vertices, or is empty when the
 * source authors no such deltas. The target lists, ascending, every vertex with a non-zero component
 * in any span, with its position delta (zero when only its normal or tangent moves) and, where the
 * span is not empty, its normal and tangent deltas.
 */
MorphTarget MakeSparseMorphTarget(String name, size_t vertexCount, std::span<const float> positionDeltas,
                                  std::span<const float> normalDeltas, std::span<const float> tangentDeltas);

} // namespace GameEngine
