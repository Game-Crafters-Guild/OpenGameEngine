#include "Animation/Nodes/MontageSlotNode.h"

#include "Animation/AnimationMontage.h"
#include "Animation/AnimationPose.h"
#include "Animation/BoneMask.h"
#include "Animation/EvaluationContext.h"
#include "Animation/MontageInstance.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::Animation
{

MontageSlotNode::MontageSlotNode()
    : m_MontagePose(new AnimationPose())
{
}

MontageSlotNode::~MontageSlotNode()
{
    delete m_MontagePose;
}

void MontageSlotNode::SetSource(std::unique_ptr<AnimGraphNode> node)
{
    m_Source = std::move(node);
}

const AnimGraphNode* MontageSlotNode::GetSource() const { return m_Source.get(); }

void MontageSlotNode::PlayMontage(const AnimationMontage* montage)
{
    if (m_ActiveMontage && !m_ActiveMontage->IsFinished())
    {
        m_ActiveMontage->RequestBlendOut();
    }

    m_ActiveMontage = std::make_unique<MontageInstance>(montage);
}

void MontageSlotNode::StopMontage()
{
    if (m_ActiveMontage)
    {
        m_ActiveMontage->RequestBlendOut();
    }
}

bool MontageSlotNode::IsPlaying() const
{
    return m_ActiveMontage && !m_ActiveMontage->IsFinished();
}

void MontageSlotNode::JumpToSection(const std::string& sectionName)
{
    if (m_ActiveMontage)
    {
        m_ActiveMontage->JumpToSection(sectionName);
    }
}

void MontageSlotNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (m_ActiveMontage && m_ActiveMontage->IsFinished())
        m_ActiveMontage.reset();

    // Update montage timing and blend weight before the source evaluates: the
    // blend weight sets the source's share of the output.
    if (m_ActiveMontage)
        m_ActiveMontage->Update(ctx.DeltaTime);
    const float blendWeight = m_ActiveMontage ? m_ActiveMontage->GetBlendWeight() : 0.0f;
    const BoneMask* mask = m_ActiveMontage ? m_ActiveMontage->GetMontage()->GetBoneMask() : nullptr;
    const bool masked = mask && !mask->Weights.empty();

    // A full-body montage covers the source by its blend weight; under a bone
    // mask the source shows through wherever the mask is below full, so it
    // keeps its weight.
    if (m_Source)
        EvaluateAtWeight(*m_Source, ctx, masked ? 1.0f : std::max(1.0f - blendWeight, 0.0f), outPose);

    // If no active montage, pass through unchanged
    if (!m_ActiveMontage)
        return;

    if (ctx.Events && ctx.Weight > 0.0f)
        m_ActiveMontage->CollectCrossedEvents(*ctx.Events);

    if (blendWeight <= 0.0f)
    {
        if (m_ActiveMontage->IsFinished())
            m_ActiveMontage.reset();
        return;
    }

    // Evaluate the montage's clip pose.
    m_MontagePose->Resize(outPose.BoneCount);
    m_ActiveMontage->EvaluatePose(ctx.DeltaTime, *m_MontagePose);

    if (masked)
    {
        // Per-bone masked blend.
        const uint32_t boneCount = std::min(outPose.BoneCount, m_MontagePose->BoneCount);
        const size_t maskCount = mask->Weights.size();
        for (uint32_t i = 0; i < boneCount; ++i)
        {
            const float maskWeight = (i < maskCount) ? mask->Weights[i] : 0.0f;
            const float boneWeight = std::clamp(maskWeight * blendWeight, 0.0f, 1.0f);
            if (boneWeight <= 0.0f)
                continue;

            const float invWeight = 1.0f - boneWeight;
            outPose.Positions[i] =
                outPose.Positions[i] * invWeight + m_MontagePose->Positions[i] * boneWeight;
            outPose.Scales[i] =
                outPose.Scales[i] * invWeight + m_MontagePose->Scales[i] * boneWeight;
            outPose.Rotations[i] = Mathematics::Quaternion::Slerp(
                outPose.Rotations[i], m_MontagePose->Rotations[i], boneWeight);
        }
    }
    else
    {
        // Full-body blend.
        AnimationPose::Blend(outPose, *m_MontagePose, blendWeight, outPose);
    }

    if (m_ActiveMontage->IsFinished())
        m_ActiveMontage.reset();
}

} // namespace GameEngine::Animation
