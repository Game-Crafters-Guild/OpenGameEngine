#pragma once

#include "Animation/AnimGraphNode.h"
#include "Animation/AnimParam.h"
#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/RootMotionExtractor.h"
#include "Types/StringId.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace GameEngine
{
namespace Animation
{

// Library eval object for a pose-graph. Not an ECS component (owns a unique_ptr
// and a map). Hosted from a POD handle on Animator; GraphStore owns instances.
struct AnimationGraphPlayer
{
    std::unique_ptr<AnimGraphNode> RootNode;
    std::unordered_map<::GameEngine::StringId, GraphParam> Parameters;
    RootMotionExtractor RootMotion;

    void SetParameter(std::string_view name, ParamValue value)
    {
        SetParameter(HashStringId(name), std::move(value), name);
    }

    void SetParameter(::GameEngine::StringId id, ParamValue value, std::string_view name = {})
    {
        GraphParam& param = Parameters[id];
        if (param.Name.empty() && !name.empty())
            param.Name.assign(name);
        param.Value = std::move(value);
    }

    void SetFloat(::GameEngine::StringId id, float value)
    {
        SetParameter(id, ParamValue{value});
    }

    void SetBool(::GameEngine::StringId id, bool value)
    {
        SetParameter(id, ParamValue{value});
    }

    void SetTrigger(::GameEngine::StringId id)
    {
        SetBool(id, true);
    }

    void ResetTrigger(::GameEngine::StringId id)
    {
        SetBool(id, false);
    }

    bool TryGetFloat(::GameEngine::StringId id, float& outValue) const
    {
        auto it = Parameters.find(id);
        if (it == Parameters.end())
            return false;
        outValue = ParamAsFloat(it->second.Value);
        return true;
    }

    bool TryGetBool(::GameEngine::StringId id, bool& outValue) const
    {
        auto it = Parameters.find(id);
        if (it == Parameters.end())
            return false;
        if (const auto* b = std::get_if<bool>(&it->second.Value))
        {
            outValue = *b;
            return true;
        }
        outValue = ParamAsFloat(it->second.Value) != 0.0f;
        return true;
    }

    const ParamValue* GetParameter(::GameEngine::StringId id) const
    {
        auto it = Parameters.find(id);
        if (it == Parameters.end())
            return nullptr;
        return &it->second.Value;
    }

    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
    {
        ctx.Parameters = &Parameters;
        if (RootNode)
            RootNode->Evaluate(ctx, outPose);
        else
            outPose.Resize(0);
    }
};

} // namespace Animation
} // namespace GameEngine
