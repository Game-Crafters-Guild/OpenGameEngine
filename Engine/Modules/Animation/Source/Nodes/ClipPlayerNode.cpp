#include "Animation/Nodes/ClipPlayerNode.h"

#include "Animation/AnimationEvent.h"
#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Assets/AnimationClip.h"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace GameEngine
{
namespace Animation
{

namespace
{

// Hermite spline evaluation matching the engine's existing cubic spline sampling.
float EvalCubicHermite(float v0, float outTangent, float outWeight,
                       float v1, float inTangent, float inWeight,
                       float dt, float alpha)
{
    float c0dt = dt * glm::clamp(outWeight, 0.05f, 0.49f);
    float c1dt = dt * glm::clamp(inWeight, 0.05f, 0.49f);
    float c0 = v0 + outTangent * c0dt;
    float c1 = v1 - inTangent * c1dt;
    float u2 = alpha * alpha;
    float u3 = u2 * alpha;
    float inv = 1.0f - alpha;
    float inv2 = inv * inv;
    float inv3 = inv2 * inv;
    return inv3 * v0 + 3.0f * inv2 * alpha * c0 + 3.0f * inv * u2 * c1 + u3 * v1;
}

void SampleChannel(const AnimChannel& channel, float time, AnimationPose& pose)
{
    uint32_t boneIndex = channel.boneIndex;
    if (channel.path == AnimPath::MorphWeight)
        return;
    if (boneIndex >= pose.BoneCount || channel.keys.empty())
        return;

    const auto& keys = channel.keys;

    size_t hi = 0;
    while (hi < keys.size() && keys[hi].time < time)
        ++hi;
    size_t lo = (hi == 0) ? 0 : (hi - 1);
    if (hi >= keys.size())
        hi = keys.size() - 1;

    const AnimKeyframe& k0 = keys[lo];
    const AnimKeyframe& k1 = keys[hi];
    float dt = k1.time - k0.time;
    float alpha = (dt > 0.0f) ? glm::clamp((time - k0.time) / dt, 0.0f, 1.0f) : 0.0f;
    bool useStep = (channel.interp == AnimInterp::Step) || (dt <= 0.0f);

    if (channel.path == AnimPath::Translation)
    {
        glm::vec3 v0(k0.translation[0], k0.translation[1], k0.translation[2]);
        if (useStep)
        {
            pose.Positions[boneIndex] = Mathematics::Vector3(v0.x, v0.y, v0.z);
        }
        else if (channel.interp == AnimInterp::CubicSpline)
        {
            pose.Positions[boneIndex] = Mathematics::Vector3(
                EvalCubicHermite(v0.x, k0.outTangent[0], k0.outWeight[0], k1.translation[0], k1.inTangent[0], k1.inWeight[0], dt, alpha),
                EvalCubicHermite(v0.y, k0.outTangent[1], k0.outWeight[1], k1.translation[1], k1.inTangent[1], k1.inWeight[1], dt, alpha),
                EvalCubicHermite(v0.z, k0.outTangent[2], k0.outWeight[2], k1.translation[2], k1.inTangent[2], k1.inWeight[2], dt, alpha));
        }
        else
        {
            glm::vec3 v1(k1.translation[0], k1.translation[1], k1.translation[2]);
            glm::vec3 result = glm::mix(v0, v1, alpha);
            pose.Positions[boneIndex] = Mathematics::Vector3(result.x, result.y, result.z);
        }
    }
    else if (channel.path == AnimPath::Scale)
    {
        glm::vec3 v0(k0.scale[0], k0.scale[1], k0.scale[2]);
        if (useStep)
        {
            pose.Scales[boneIndex] = Mathematics::Vector3(v0.x, v0.y, v0.z);
        }
        else if (channel.interp == AnimInterp::CubicSpline)
        {
            pose.Scales[boneIndex] = Mathematics::Vector3(
                EvalCubicHermite(v0.x, k0.outTangent[0], k0.outWeight[0], k1.scale[0], k1.inTangent[0], k1.inWeight[0], dt, alpha),
                EvalCubicHermite(v0.y, k0.outTangent[1], k0.outWeight[1], k1.scale[1], k1.inTangent[1], k1.inWeight[1], dt, alpha),
                EvalCubicHermite(v0.z, k0.outTangent[2], k0.outWeight[2], k1.scale[2], k1.inTangent[2], k1.inWeight[2], dt, alpha));
        }
        else
        {
            glm::vec3 v1(k1.scale[0], k1.scale[1], k1.scale[2]);
            glm::vec3 result = glm::mix(v0, v1, alpha);
            pose.Scales[boneIndex] = Mathematics::Vector3(result.x, result.y, result.z);
        }
    }
    else // Rotation
    {
        glm::quat q0(k0.rotation[3], k0.rotation[0], k0.rotation[1], k0.rotation[2]);
        if (useStep)
        {
            pose.Rotations[boneIndex] = Mathematics::Quaternion(q0);
        }
        else if (channel.interp == AnimInterp::CubicSpline)
        {
            glm::quat q(
                EvalCubicHermite(q0.w, k0.outTangent[3], k0.outWeight[3], k1.rotation[3], k1.inTangent[3], k1.inWeight[3], dt, alpha),
                EvalCubicHermite(q0.x, k0.outTangent[0], k0.outWeight[0], k1.rotation[0], k1.inTangent[0], k1.inWeight[0], dt, alpha),
                EvalCubicHermite(q0.y, k0.outTangent[1], k0.outWeight[1], k1.rotation[1], k1.inTangent[1], k1.inWeight[1], dt, alpha),
                EvalCubicHermite(q0.z, k0.outTangent[2], k0.outWeight[2], k1.rotation[2], k1.inTangent[2], k1.inWeight[2], dt, alpha));
            pose.Rotations[boneIndex] = Mathematics::Quaternion(glm::normalize(q));
        }
        else
        {
            glm::quat q1(k1.rotation[3], k1.rotation[0], k1.rotation[1], k1.rotation[2]);
            pose.Rotations[boneIndex] = Mathematics::Quaternion(glm::slerp(q0, q1, alpha));
        }
    }
}

} // anonymous namespace

void ClipPlayerNode::SetClip(std::shared_ptr<AnimationClip> clip)
{
    m_Clip = std::move(clip);
    m_Time = 0.0f;
    if (m_Clip)
        m_ClipGuid = m_Clip->GetGUID();
}

void ClipPlayerNode::SetClipGuid(const GUID& guid)
{
    m_ClipGuid = guid;
}

const GUID& ClipPlayerNode::GetClipGuid() const
{
    return m_ClipGuid;
}

void ClipPlayerNode::SetSpeed(float speed)
{
    m_Speed = std::isfinite(speed) ? speed : 1.0f;
}

void ClipPlayerNode::SetLooping(bool loop)
{
    m_Loop = loop;
}

void ClipPlayerNode::SetTime(float time)
{
    m_Time = time;
}

float ClipPlayerNode::GetTime() const
{
    return m_Time;
}

float ClipPlayerNode::GetDuration() const
{
    if (!m_Clip)
        return 0.0f;
    return m_Clip->GetDuration();
}

float ClipPlayerNode::GetNormalizedTime() const
{
    float duration = GetDuration();
    if (duration <= 0.0f)
        return 0.0f;
    return m_Time / duration;
}

float ClipPlayerNode::GetSpeed() const { return m_Speed; }
bool ClipPlayerNode::GetLooping() const { return m_Loop; }
const std::shared_ptr<AnimationClip>& ClipPlayerNode::GetClip() const { return m_Clip; }

void ClipPlayerNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (!m_Clip)
    {
        outPose.Resize(0);
        return;
    }

    // Advance playback time. Non-finite speed/dt would livelock a
    // subtract-while wrap; fmod is O(1) for huge finite values too.
    const float previousTime = m_Time;
    float dt = std::max(0.0f, ctx.DeltaTime) * std::max(0.0f, m_Speed);
    if (!std::isfinite(dt))
        dt = 0.0f;
    m_Time += dt;
    if (!std::isfinite(m_Time))
        m_Time = 0.0f;

    float duration = m_Clip->GetDuration();
    PlaybackStepKind stepKind = PlaybackStepKind::Forward;
    if (duration > 0.0f)
    {
        if (m_Loop)
        {
            // A step that did not move crosses nothing, even sitting on the lap's end.
            if (m_Time >= duration && m_Time != previousTime)
                stepKind = m_Time - std::max(previousTime, 0.0f) >= duration ? PlaybackStepKind::WholeLap
                                                                             : PlaybackStepKind::Wrapped;
            m_Time = std::fmod(m_Time, duration);
            if (m_Time < 0.0f)
                m_Time += duration;
        }
        else if (m_Time > duration)
        {
            m_Time = duration;
        }
    }

    if (ctx.Events && ctx.Weight > 0.0f)
    {
        const PlaybackStep step{previousTime, m_Time, 0.0f, duration, stepKind};
        CollectCrossedEvents(m_Clip->GetEventTrack(), step, *ctx.Events);
    }

    // Determine bone count from clip channels
    uint32_t maxBone = 0;
    for (const auto& channel : m_Clip->GetChannels())
    {
        if (channel.boneIndex >= maxBone)
            maxBone = channel.boneIndex + 1;
    }

    outPose.Resize(maxBone);

    // Sample each channel into the pose
    for (const auto& channel : m_Clip->GetChannels())
        SampleChannel(channel, m_Time, outPose);
}

} // namespace Animation
} // namespace GameEngine
