#include "Animation/SkeletonData.h"

#include "Types/StringId.h"

#include <queue>
#include <vector>

namespace GameEngine { namespace Animation {

void SkeletonData::BuildBoneNameLookup()
{
    BoneNameIds.assign(BoneCount, 0u);
    for (uint32 i = 0; i < BoneCount && i < BoneNames.size(); ++i)
    {
        if (!BoneNames[i].empty())
            BoneNameIds[i] = HashStringId(BoneNames[i]);
    }

    BoneNameLookup.clear();
    BoneNameLookup.reserve(BoneCount);
    for (uint32 i = 0; i < BoneCount && i < BoneNameIds.size(); ++i)
    {
        const StringId id = BoneNameIds[i];
        if (id != 0)
            BoneNameLookup.emplace(id, i);
    }
}

uint32 SkeletonData::ResolveBoneIndex(StringId nameId, uint32 fallbackIndex) const
{
    if (nameId == 0 || BoneNameLookup.empty())
        return fallbackIndex;
    const auto it = BoneNameLookup.find(nameId);
    return (it != BoneNameLookup.end()) ? it->second : fallbackIndex;
}

uint32 SkeletonData::JointClosureLevelCount() const
{
    return JointClosureLevelOffsets.empty() ? 0u : static_cast<uint32>(JointClosureLevelOffsets.size() - 1);
}

void SkeletonData::ComputeTopologicalSort()
{
    TopologicalOrder.clear();
    JointClosure.clear();
    JointClosureLevelOffsets.assign(1, 0u);
    if (BoneCount == 0)
        return;

    // Build children lists for BFS.
    std::vector<std::vector<uint32>> children(BoneCount);
    std::queue<uint32> frontier;
    for (uint32 i = 0; i < BoneCount; ++i)
    {
        const int32 p = (i < Parent.size()) ? Parent[i] : -1;
        if (p >= 0 && static_cast<uint32>(p) < BoneCount)
            children[static_cast<uint32>(p)].push_back(i);
        else
            frontier.push(i); // root bone
    }

    // Mark every joint and its ancestors; the walk up a chain stops at the
    // first bone already marked.
    const bool hasJointTable = SkinJointCount > 0 && JointNodes.size() == SkinJointCount;
    std::vector<uint8> inClosure(BoneCount, hasJointTable ? uint8{0} : uint8{1});
    if (hasJointTable)
    {
        for (uint32 bone : JointNodes)
        {
            while (bone < BoneCount && inClosure[bone] == 0u)
            {
                inClosure[bone] = 1u;
                const int32 parent = (bone < Parent.size()) ? Parent[bone] : -1;
                bone = (parent >= 0) ? static_cast<uint32>(parent) : BoneCount;
            }
        }
    }

    TopologicalOrder.reserve(BoneCount);
    while (!frontier.empty())
    {
        const size_t levelSize = frontier.size();
        for (size_t n = 0; n < levelSize; ++n)
        {
            uint32 bone = frontier.front();
            frontier.pop();
            TopologicalOrder.push_back(bone);
            if (inClosure[bone] != 0u)
                JointClosure.push_back(bone);
            for (uint32 child : children[bone])
                frontier.push(child);
        }
        // A closure bone's parent is a closure bone one level up, so the
        // closure fills the first levels of the hierarchy without a gap.
        if (JointClosure.size() > JointClosureLevelOffsets.back())
            JointClosureLevelOffsets.push_back(static_cast<uint32>(JointClosure.size()));
    }
}

}} // namespace GameEngine::Animation
