#include "Animation/Nodes/BlendSpace2DNode.h"

#include "Animation/AnimParam.h"
#include "Animation/EvaluationContext.h"
#include "Types/StringId.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numeric>

namespace GameEngine::Animation
{

namespace
{
    constexpr float kDistanceEpsilon = 1e-6f;
    constexpr float kMinWeight = 0.001f;
    // Phase 12 (audit §1.3): cap on the inline-array used for sample
    // weights. 2D blend-spaces typically blend 3-4 nearest neighbors;
    // 16 covers any sane authored grid without falling back to heap.
    // m_MaxBlendedSamples is the hot path; the m_Samples capacity does
    // NOT need to fit here — we only sort the first kMaxInlineSamples.
    constexpr std::size_t kMaxInlineSamples = 16;
} // namespace

void BlendSpace2DNode::AddSample(std::unique_ptr<AnimGraphNode> node, float x, float y)
{
    m_Samples.push_back({std::move(node), x, y});
}

void BlendSpace2DNode::SetParameter(float x, float y)
{
    m_ParamX = x;
    m_ParamY = y;
}

void BlendSpace2DNode::SetParameterNameX(std::string_view name)
{
    m_ParameterNameX.assign(name);
    m_ParameterIdX = HashStringId(name);
}

void BlendSpace2DNode::SetParameterNameY(std::string_view name)
{
    m_ParameterNameY.assign(name);
    m_ParameterIdY = HashStringId(name);
}

void BlendSpace2DNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (ctx.Parameters)
    {
        if (m_ParameterIdX != 0)
        {
            auto it = ctx.Parameters->find(m_ParameterIdX);
            if (it != ctx.Parameters->end())
                m_ParamX = ParamAsFloat(it->second.Value);
        }
        if (m_ParameterIdY != 0)
        {
            auto it = ctx.Parameters->find(m_ParameterIdY);
            if (it != ctx.Parameters->end())
                m_ParamY = ParamAsFloat(it->second.Value);
        }
    }

    if (m_Samples.empty())
    {
        return;
    }

    if (m_Samples.size() == 1)
    {
        if (m_Samples[0].Node)
            m_Samples[0].Node->Evaluate(ctx, outPose);
        else
            outPose.Resize(0);
        return;
    }

    // Compute distances from the parameter point to each sample.
    //
    // Phase 12 (audit §1.3): inline-array storage replaces the per-Evaluate
    // std::vector<SampleWeight>. For 2D blend grids the typical neighbor
    // count is 4; even a thick 4x4 grid stays under kMaxInlineSamples.
    // Sample counts above the cap fall through to the slower path that
    // truncates to kMaxInlineSamples — still allocation-free.
    struct SampleWeight
    {
        uint32_t Index;
        float Distance;
        float Weight;
    };

    std::array<SampleWeight, kMaxInlineSamples> candidates{};
    std::size_t candidateCount = 0;
    const std::size_t inputCount =
        std::min<std::size_t>(m_Samples.size(), kMaxInlineSamples);

    for (uint32_t i = 0; i < static_cast<uint32_t>(inputCount); ++i)
    {
        float dx = m_ParamX - m_Samples[i].X;
        float dy = m_ParamY - m_Samples[i].Y;
        float dist = std::sqrt(dx * dx + dy * dy);

        // If we land exactly on a sample, just evaluate it directly
        if (!m_Samples[i].Node)
            continue;

        if (dist < kDistanceEpsilon)
        {
            m_Samples[i].Node->Evaluate(ctx, outPose);
            return;
        }

        candidates[candidateCount++] = SampleWeight{i, dist, 0.0f};
    }

    // Keep only the K nearest samples. partial_sort over the active prefix.
    auto begin = candidates.begin();
    auto end = candidates.begin() + candidateCount;
    const std::size_t k =
        std::min<std::size_t>(static_cast<std::size_t>(m_MaxBlendedSamples),
                              candidateCount);
    std::partial_sort(begin, begin + k, end,
                      [](const SampleWeight& a, const SampleWeight& b)
                      { return a.Distance < b.Distance; });
    candidateCount = k;

    // Inverse distance weighting.
    for (std::size_t i = 0; i < candidateCount; ++i)
        candidates[i].Weight = 1.0f / candidates[i].Distance;

    float totalWeight = 0.0f;
    for (std::size_t i = 0; i < candidateCount; ++i)
        totalWeight += candidates[i].Weight;
    for (std::size_t i = 0; i < candidateCount; ++i)
        candidates[i].Weight /= totalWeight;

    // Remove negligible contributions (in-place compact).
    std::size_t writeIdx = 0;
    for (std::size_t i = 0; i < candidateCount; ++i)
    {
        if (candidates[i].Weight >= kMinWeight)
            candidates[writeIdx++] = candidates[i];
    }
    candidateCount = writeIdx;

    if (candidateCount == 0)
    {
        return;
    }

    // Re-normalize after pruning.
    totalWeight = 0.0f;
    for (std::size_t i = 0; i < candidateCount; ++i)
        totalWeight += candidates[i].Weight;
    for (std::size_t i = 0; i < candidateCount; ++i)
        candidates[i].Weight /= totalWeight;

    // Progressive blending: evaluate first sample into outPose, then blend in the rest.
    if (!m_Samples[candidates[0].Index].Node)
    {
        outPose.Resize(0);
        return;
    }
    EvaluateAtWeight(*m_Samples[candidates[0].Index].Node, ctx, candidates[0].Weight, outPose);
    float cumulativeWeight = candidates[0].Weight;

    for (std::size_t i = 1; i < candidateCount; ++i)
    {
        if (!m_Samples[candidates[i].Index].Node)
            continue;
        EvaluateAtWeight(*m_Samples[candidates[i].Index].Node, ctx, candidates[i].Weight, m_TempPoseA);
        cumulativeWeight += candidates[i].Weight;
        float blendAlpha = candidates[i].Weight / cumulativeWeight;
        AnimationPose::Blend(outPose, m_TempPoseA, blendAlpha, outPose);
    }
}

} // namespace GameEngine::Animation
