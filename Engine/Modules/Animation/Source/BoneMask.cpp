#include "Animation/BoneMask.h"

namespace GameEngine::Animation
{

void BoneMask::Resize(uint32_t boneCount, float defaultWeight)
{
    Weights.assign(boneCount, defaultWeight);
}

BoneMask BoneMask::CreateFromBoneAndDescendants(
    uint32_t rootBoneIndex,
    const std::vector<int32_t>& parentIndices,
    uint32_t totalBoneCount)
{
    BoneMask mask;
    mask.Weights.assign(totalBoneCount, 0.0f);

    if (rootBoneIndex >= totalBoneCount)
        return mask;

    mask.Weights[rootBoneIndex] = 1.0f;

    // Walk forward through the bone array. Because children always have a higher index
    // than their parent in a topologically sorted skeleton, a single forward pass
    // propagates inclusion from the root bone to all descendants.
    for (uint32_t i = rootBoneIndex + 1; i < totalBoneCount; ++i)
    {
        int32_t parent = (i < static_cast<uint32_t>(parentIndices.size())) ? parentIndices[i] : -1;
        if (parent >= 0 && mask.Weights[static_cast<uint32_t>(parent)] > 0.0f)
            mask.Weights[i] = 1.0f;
    }

    return mask;
}

} // namespace GameEngine::Animation
