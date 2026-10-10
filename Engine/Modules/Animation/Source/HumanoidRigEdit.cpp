#include "Animation/HumanoidRigEdit.h"

#include "Animation/AnimationClip.h"
#include "Animation/SkeletonData.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace GameEngine
{
namespace Animation
{

namespace
{

constexpr float kRigEditPi = 3.14159265358979323846f;
constexpr float kRigEditRadToDeg = 180.0f / kRigEditPi;

// Snap-to-identity threshold guard so retargeting math treats T-pose-ish
// captures as "no fix-up". Mirrors HumanoidImport::BakeRetargetPoseFromAPose.
bool IsWithinIdentity(const glm::quat& q, float epsilonDeg)
{
    const float w = std::clamp(std::fabs(q.w), 0.0f, 1.0f);
    const float angleRad = 2.0f * std::acos(w);
    return angleRad * kRigEditRadToDeg < epsilonDeg;
}

// Linear interpolation between two quaternion samples (short-arc). Sufficient
// for the editor's bake-from-frame feature; precision matches the runtime
// SampleAnimationPose Linear path closely enough for retarget-pose authoring.
glm::quat ShortLerpQuat(const glm::quat& a, const glm::quat& b, float t)
{
    glm::quat bb = b;
    if (glm::dot(a, b) < 0.0f)
        bb = -b;
    glm::quat r = glm::normalize(glm::quat(
        a.w + (bb.w - a.w) * t,
        a.x + (bb.x - a.x) * t,
        a.y + (bb.y - a.y) * t,
        a.z + (bb.z - a.z) * t));
    return r;
}

// Linear sample of a rotation channel at `time`. Steps and cubic spline
// interp fall back to nearest-key lookup since the editor authoring path
// only needs frame-accurate fidelity, not the runtime's spline support.
glm::quat SampleRotationChannel(const AnimChannel& ch, float time)
{
    if (ch.keys.empty())
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (ch.keys.size() == 1)
    {
        const auto& k = ch.keys.front();
        return glm::quat(k.rotation[3], k.rotation[0], k.rotation[1], k.rotation[2]);
    }

    if (time <= ch.keys.front().time)
    {
        const auto& k = ch.keys.front();
        return glm::quat(k.rotation[3], k.rotation[0], k.rotation[1], k.rotation[2]);
    }
    if (time >= ch.keys.back().time)
    {
        const auto& k = ch.keys.back();
        return glm::quat(k.rotation[3], k.rotation[0], k.rotation[1], k.rotation[2]);
    }

    // Lower bound search; channels are time-sorted by contract.
    const auto it = std::upper_bound(
        ch.keys.begin(), ch.keys.end(), time,
        [](float t, const AnimKeyframe& k) { return t < k.time; });
    const AnimKeyframe& hi = *it;
    const AnimKeyframe& lo = *(it - 1);

    const float dt = std::max(1e-6f, hi.time - lo.time);
    const float a = std::clamp((time - lo.time) / dt, 0.0f, 1.0f);

    const glm::quat qLo(lo.rotation[3], lo.rotation[0], lo.rotation[1], lo.rotation[2]);
    const glm::quat qHi(hi.rotation[3], hi.rotation[0], hi.rotation[1], hi.rotation[2]);
    return ShortLerpQuat(qLo, qHi, a);
}

} // namespace

void ForceRetargetPoseToIdentity(HumanoidRig& rig)
{
    auto& bm = rig.BoneMapMutable();
    for (auto& m : bm)
        m.RetargetPoseRotation = Mathematics::Quaternion::Identity();
}

void CaptureRetargetPoseFromPreview(HumanoidRig& rig,
                                    const std::vector<Mathematics::Quaternion>& localRotations,
                                    float epsilonDeg)
{
    auto& bm = rig.BoneMapMutable();
    if (localRotations.size() != bm.size())
        return;

    for (size_t i = 0; i < bm.size(); ++i)
    {
        const glm::quat& q = localRotations[i].GetGLM();
        if (IsWithinIdentity(q, epsilonDeg))
            bm[i].RetargetPoseRotation = Mathematics::Quaternion::Identity();
        else
            bm[i].RetargetPoseRotation = localRotations[i];
    }
}

int BakeRetargetPoseFromClipFrame(HumanoidRig& rig,
                                  const AnimationClip& clip,
                                  const SkeletonData& skel,
                                  float timeSeconds,
                                  float epsilonDeg)
{
    auto& bm = rig.BoneMapMutable();
    if (bm.empty() || clip.GetChannels().empty())
        return 0;

    // Index the clip's rotation channels by target name once. The clip is
    // small (typ. < 200 channels) and this saves an O(N*M) string-compare in
    // the per-mapping loop below.
    std::unordered_map<std::string, const AnimChannel*> rotByName;
    rotByName.reserve(clip.GetChannels().size());
    for (const auto& ch : clip.GetChannels())
    {
        if (ch.path != AnimPath::Rotation)
            continue;
        if (!ch.targetName.empty())
            rotByName.emplace(ch.targetName, &ch);
    }

    int updated = 0;
    for (auto& m : bm)
    {
        if (m.SourceBoneName.empty())
            continue;
        auto it = rotByName.find(m.SourceBoneName);
        if (it == rotByName.end())
            continue;

        const glm::quat sampled = SampleRotationChannel(*it->second, timeSeconds);
        if (IsWithinIdentity(sampled, epsilonDeg))
            m.RetargetPoseRotation = Mathematics::Quaternion::Identity();
        else
            m.RetargetPoseRotation = Mathematics::Quaternion(sampled.w, sampled.x, sampled.y, sampled.z);
        ++updated;
    }

    // skel is reserved for future per-bone-index resolution; keep the
    // parameter so the inspector can pass it through in case the clip's
    // channels are bound by index rather than name (older importers).
    (void)skel;
    return updated;
}

} // namespace Animation
} // namespace GameEngine
