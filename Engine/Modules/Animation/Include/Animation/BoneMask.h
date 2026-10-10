#pragma once

#include <cstdint>
#include <vector>

namespace GameEngine::Animation
{

// Per-bone weight array (0.0 = fully masked out, 1.0 = fully included).
// Used by LayeredBlendNode and AdditiveBlendNode to control which bones a layer affects.
struct BoneMask
{
    std::vector<float> Weights;

    void Resize(uint32_t boneCount, float defaultWeight = 1.0f);

    // Build a mask that includes the given root bone and all its descendants.
    // Bones under rootBoneIndex get weight 1.0; everything else gets 0.0.
    static BoneMask CreateFromBoneAndDescendants(
        uint32_t rootBoneIndex,
        const std::vector<int32_t>& parentIndices,
        uint32_t totalBoneCount);
};

} // namespace GameEngine::Animation
