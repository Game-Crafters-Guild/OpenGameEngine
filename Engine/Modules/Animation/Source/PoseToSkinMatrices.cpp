#include "Animation/PoseToSkinMatrices.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>

namespace GameEngine
{
namespace Animation
{

namespace
{

glm::mat4 BuildLocalTRS(const glm::vec3& t, const glm::quat& r, const glm::vec3& s)
{
    return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
}

void StoreColumnMajor(const glm::mat4& matrix, float* out)
{
    std::memcpy(out, glm::value_ptr(matrix), 16 * sizeof(float));
}

} // namespace

void BuildPoseToSkinMatrices(const SkeletonData& skeleton,
                             std::vector<float>* outNodeWorldMatrices,
                             std::vector<float>* outCompactSkinMatrices,
                             PoseSampleWorkspace& ws)
{
    const uint32_t bones = skeleton.BoneCount;

    ws.Local.clear();
    ws.Local.resize(bones, glm::mat4(1.0f));
    const bool haveRestLocal = skeleton.RestLocalMatrix.size() >= static_cast<size_t>(bones) * 16u;
    for (uint32_t i = 0; i < bones; ++i)
    {
        const bool anyChanged = (ws.ChangedTranslation[i] | ws.ChangedRotation[i] | ws.ChangedScale[i]) != 0u;
        if (haveRestLocal && !anyChanged)
            ws.Local[i] = glm::make_mat4(&skeleton.RestLocalMatrix[i * 16u]);
        else
            ws.Local[i] = BuildLocalTRS(ws.Pose[i].t, ws.Pose[i].r, ws.Pose[i].s);
    }

    ws.World.clear();
    ws.World.resize(bones, glm::mat4(1.0f));
    ws.Built.clear();
    ws.Built.resize(bones, uint8_t{0});

    auto buildWorld = [&](auto&& self, uint32_t nodeIndex) -> void
    {
        if (nodeIndex >= bones || ws.Built[nodeIndex])
            return;
        const int parentIndex = (nodeIndex < skeleton.Parent.size()) ? skeleton.Parent[nodeIndex] : -1;
        if (parentIndex >= 0)
        {
            self(self, static_cast<uint32_t>(parentIndex));
            ws.World[nodeIndex] = ws.World[static_cast<size_t>(parentIndex)] * ws.Local[nodeIndex];
        }
        else
        {
            ws.World[nodeIndex] = ws.Local[nodeIndex];
        }
        ws.Built[nodeIndex] = 1u;
    };
    for (uint32_t i = 0; i < bones; ++i)
        buildWorld(buildWorld, i);

    if (outNodeWorldMatrices)
    {
        outNodeWorldMatrices->resize(static_cast<size_t>(bones) * 16u);
        for (uint32_t i = 0; i < bones; ++i)
            StoreColumnMajor(ws.World[i], outNodeWorldMatrices->data() + static_cast<size_t>(i) * 16u);
    }

    if (!outCompactSkinMatrices)
        return;

    const uint32_t jointCount =
        (skeleton.SkinJointCount > 0 && skeleton.JointNodes.size() == skeleton.SkinJointCount)
            ? skeleton.SkinJointCount
            : bones;
    outCompactSkinMatrices->resize(static_cast<size_t>(jointCount) * 16u);

    const glm::mat4 meshRoot = glm::make_mat4(skeleton.MeshRootWorld);
    const glm::mat4 meshRootInverse = glm::inverse(meshRoot);
    for (uint32_t jointIndex = 0; jointIndex < jointCount; ++jointIndex)
    {
        const uint32_t nodeIndex = (jointIndex < skeleton.JointNodes.size()) ? skeleton.JointNodes[jointIndex] : jointIndex;
        if (nodeIndex >= bones)
            continue;

        const glm::mat4 inverseBind = glm::make_mat4(&skeleton.InverseBind[static_cast<size_t>(nodeIndex) * 16u]);
        const glm::mat4 modelRelative = meshRootInverse * ws.World[nodeIndex];
        const glm::mat4 skinMatrix = modelRelative * inverseBind;
        StoreColumnMajor(skinMatrix, outCompactSkinMatrices->data() + static_cast<size_t>(jointIndex) * 16u);
    }
}

void EmitSkinMatricesFromAnimationPose(const SkeletonData& skeleton,
                                       const AnimationPose& localPose,
                                       std::vector<float>& outCompactSkinMatrices,
                                       PoseSampleWorkspace& ws)
{
    const uint32_t boneCount = skeleton.BoneCount;
    if (boneCount == 0)
    {
        outCompactSkinMatrices.clear();
        return;
    }

    ws.Pose.assign(boneCount, PoseSampleWorkspace::TRS{});
    for (uint32_t i = 0; i < boneCount && i < localPose.BoneCount; ++i)
    {
        const auto& p = localPose.Positions[i];
        const auto& q = localPose.Rotations[i].GetGLM();
        const auto& s = localPose.Scales[i];
        ws.Pose[i].t = glm::vec3(p.x, p.y, p.z);
        ws.Pose[i].r = q;
        ws.Pose[i].s = glm::vec3(s.x, s.y, s.z);
    }
    ws.ChangedTranslation.assign(boneCount, 1u);
    ws.ChangedRotation.assign(boneCount, 1u);
    ws.ChangedScale.assign(boneCount, 1u);

    BuildPoseToSkinMatrices(skeleton, nullptr, &outCompactSkinMatrices, ws);
}

} // namespace Animation
} // namespace GameEngine
