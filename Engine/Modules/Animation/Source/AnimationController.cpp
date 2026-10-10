#include "Animation/AnimationController.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <iterator>
#include <nlohmann/json.hpp>
#include <utility>

namespace GameEngine
{
namespace Animation
{

namespace
{

using json = nlohmann::json;

AssetType MotionAssetTypeFromString(const std::string& value)
{
    if (value == "Timeline") return AssetType::Timeline;
    if (value == "ClipSet") return AssetType::ClipSet;
    if (value == "AnimationLibrary") return AssetType::AnimationLibrary;
    if (value == "AnimationController") return AssetType::AnimationController;
    if (value == "SpriteFrames") return AssetType::SpriteFrames;
    return AssetType::Animation;
}

AnimationStateMotion LoadMotion(const json& doc)
{
    AnimationStateMotion motion;
    if (doc.contains("assetGuid") && doc["assetGuid"].is_string())
        motion.AssetGuid = GUID(doc["assetGuid"].get<std::string>());
    motion.Type = MotionAssetTypeFromString(doc.value("type", std::string("Animation")));
    motion.LibraryName = doc.value("libraryName", std::string());
    motion.Speed = doc.value("speed", 1.0f);
    motion.Loop = doc.value("loop", true);
    return motion;
}

json SaveMotion(const AnimationStateMotion& motion)
{
    json doc;
    doc["assetGuid"] = motion.AssetGuid.ToString();
    doc["type"] = AssetTypeToString(motion.Type);
    doc["libraryName"] = motion.LibraryName;
    doc["speed"] = motion.Speed;
    doc["loop"] = motion.Loop;
    return doc;
}

} // namespace

const char* AnimationParameterTypeToString(AnimationParameterType type)
{
    switch (type)
    {
        case AnimationParameterType::Float: return "Float";
        case AnimationParameterType::Int: return "Int";
        case AnimationParameterType::Bool: return "Bool";
        case AnimationParameterType::Trigger: return "Trigger";
    }
    return "Float";
}

AnimationParameterType AnimationParameterTypeFromString(const std::string& value)
{
    if (value == "Int") return AnimationParameterType::Int;
    if (value == "Bool") return AnimationParameterType::Bool;
    if (value == "Trigger") return AnimationParameterType::Trigger;
    return AnimationParameterType::Float;
}

const char* AnimationTransitionComparisonToString(AnimationTransitionComparison comparison)
{
    switch (comparison)
    {
        case AnimationTransitionComparison::Always: return "Always";
        case AnimationTransitionComparison::Equals: return "Equals";
        case AnimationTransitionComparison::NotEquals: return "NotEquals";
        case AnimationTransitionComparison::Greater: return "Greater";
        case AnimationTransitionComparison::Less: return "Less";
    }
    return "Always";
}

AnimationTransitionComparison AnimationTransitionComparisonFromString(const std::string& value)
{
    if (value == "Equals") return AnimationTransitionComparison::Equals;
    if (value == "NotEquals") return AnimationTransitionComparison::NotEquals;
    if (value == "Greater") return AnimationTransitionComparison::Greater;
    if (value == "Less") return AnimationTransitionComparison::Less;
    return AnimationTransitionComparison::Always;
}

const char* BlendTreeTypeToString(BlendTreeType type)
{
    switch (type)
    {
        case BlendTreeType::None: return "None";
        case BlendTreeType::Blend1D: return "Blend1D";
        case BlendTreeType::Blend2D: return "Blend2D";
    }
    return "None";
}

BlendTreeType BlendTreeTypeFromString(const std::string& value)
{
    if (value == "Blend1D") return BlendTreeType::Blend1D;
    if (value == "Blend2D") return BlendTreeType::Blend2D;
    return BlendTreeType::None;
}

bool AnimationController::Load()
{
    SetState(AssetState::Loading);
    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        Logger::Log::Error("AnimationController: cannot open '{}'", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return ParseJson(text);
}

bool AnimationController::LoadFromData(const Vector<uint8>& data)
{
    SetState(AssetState::Loading);
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    return ParseJson(text);
}

void AnimationController::Unload()
{
    m_Parameters.clear();
    m_States.clear();
    m_Transitions.clear();
    m_EntryState.clear();
    SetState(AssetState::Unloaded);
}

bool AnimationController::SaveToData(Vector<uint8>& outData) const
{
    const std::string serialized = SerializeJson();
    outData.assign(serialized.begin(), serialized.end());
    return true;
}

const AnimationControllerState* AnimationController::FindState(const std::string& name) const
{
    for (const auto& state : m_States)
    {
        if (state.Name == name)
            return &state;
    }
    return nullptr;
}

void AnimationController::SetDataForTest(std::vector<AnimationParameter> parameters,
                                         std::vector<AnimationControllerState> states,
                                         std::vector<AnimationControllerTransition> transitions,
                                         std::string entryState)
{
    m_Parameters = std::move(parameters);
    m_States = std::move(states);
    m_Transitions = std::move(transitions);
    m_EntryState = std::move(entryState);
}

bool AnimationController::ParseJson(const std::string& text)
{
    try
    {
        const auto doc = json::parse(text);
        if (!doc.is_object())
        {
            SetState(AssetState::Failed);
            return false;
        }

        m_EntryState = doc.value("entryState", std::string());
        m_Parameters.clear();
        m_States.clear();
        m_Transitions.clear();

        if (doc.contains("parameters") && doc["parameters"].is_array())
        {
            for (const auto& item : doc["parameters"])
            {
                AnimationParameter parameter;
                parameter.Name = item.value("name", std::string());
                parameter.Type = AnimationParameterTypeFromString(item.value("type", std::string("Float")));
                parameter.DefaultFloat = item.value("defaultFloat", 0.0f);
                parameter.DefaultInt = item.value("defaultInt", 0);
                parameter.DefaultBool = item.value("defaultBool", false);
                if (!parameter.Name.empty())
                    m_Parameters.push_back(std::move(parameter));
            }
        }

        if (doc.contains("states") && doc["states"].is_array())
        {
            for (const auto& item : doc["states"])
            {
                AnimationControllerState state;
                state.Name = item.value("name", std::string());
                if (item.contains("motion") && item["motion"].is_object())
                    state.Motion = LoadMotion(item["motion"]);
                if (item.contains("blendTree") && item["blendTree"].is_object())
                {
                    const auto& bt = item["blendTree"];
                    state.BlendTree.Type = BlendTreeTypeFromString(bt.value("type", std::string("None")));
                    state.BlendTree.ParameterX = bt.value("parameterX", std::string());
                    state.BlendTree.ParameterY = bt.value("parameterY", std::string());
                    if (bt.contains("children") && bt["children"].is_array())
                    {
                        for (const auto& childJson : bt["children"])
                        {
                            AnimationBlendTreeChild child;
                            if (childJson.contains("motion") && childJson["motion"].is_object())
                                child.Motion = LoadMotion(childJson["motion"]);
                            child.ThresholdX = childJson.value("thresholdX", 0.0f);
                            child.ThresholdY = childJson.value("thresholdY", 0.0f);
                            state.BlendTree.Children.push_back(std::move(child));
                        }
                    }
                }
                if (!state.Name.empty())
                    m_States.push_back(std::move(state));
            }
        }

        if (doc.contains("transitions") && doc["transitions"].is_array())
        {
            for (const auto& item : doc["transitions"])
            {
                AnimationControllerTransition transition;
                transition.FromState = item.value("fromState", std::string());
                transition.ToState = item.value("toState", std::string());
                transition.DurationSeconds = std::max(0.0f, item.value("durationSeconds", 0.2f));
                transition.ExitTimeSeconds = item.value("exitTimeSeconds", -1.0f);
                transition.CanInterrupt = item.value("canInterrupt", true);
                if (item.contains("conditions") && item["conditions"].is_array())
                {
                    for (const auto& condJson : item["conditions"])
                    {
                        AnimationTransitionConditionDef condition;
                        condition.Parameter = condJson.value("parameter", std::string());
                        condition.Comparison = AnimationTransitionComparisonFromString(condJson.value("comparison", std::string("Always")));
                        condition.FloatValue = condJson.value("floatValue", 0.0f);
                        condition.IntValue = condJson.value("intValue", 0);
                        condition.BoolValue = condJson.value("boolValue", false);
                        transition.Conditions.push_back(std::move(condition));
                    }
                }
                if (!transition.FromState.empty() && !transition.ToState.empty())
                    m_Transitions.push_back(std::move(transition));
            }
        }

        SetState(AssetState::Loaded);
        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("AnimationController: failed to parse '{}': {}", GetPath().string(), e.what());
        Unload();
        SetState(AssetState::Failed);
        return false;
    }
}

std::string AnimationController::SerializeJson() const
{
    json doc;
    doc["schemaVersion"] = kSchemaVersion;
    doc["assetType"] = "AnimationController";
    doc["entryState"] = m_EntryState;

    json parameters = json::array();
    for (const auto& parameter : m_Parameters)
    {
        parameters.push_back(json{
            {"name", parameter.Name},
            {"type", AnimationParameterTypeToString(parameter.Type)},
            {"defaultFloat", parameter.DefaultFloat},
            {"defaultInt", parameter.DefaultInt},
            {"defaultBool", parameter.DefaultBool}
        });
    }
    doc["parameters"] = std::move(parameters);

    json states = json::array();
    for (const auto& state : m_States)
    {
        json stateJson;
        stateJson["name"] = state.Name;
        stateJson["motion"] = SaveMotion(state.Motion);
        json blendTree;
        blendTree["type"] = BlendTreeTypeToString(state.BlendTree.Type);
        blendTree["parameterX"] = state.BlendTree.ParameterX;
        blendTree["parameterY"] = state.BlendTree.ParameterY;
        json children = json::array();
        for (const auto& child : state.BlendTree.Children)
        {
            children.push_back(json{
                {"motion", SaveMotion(child.Motion)},
                {"thresholdX", child.ThresholdX},
                {"thresholdY", child.ThresholdY}
            });
        }
        blendTree["children"] = std::move(children);
        stateJson["blendTree"] = std::move(blendTree);
        states.push_back(std::move(stateJson));
    }
    doc["states"] = std::move(states);

    json transitions = json::array();
    for (const auto& transition : m_Transitions)
    {
        json transitionJson;
        transitionJson["fromState"] = transition.FromState;
        transitionJson["toState"] = transition.ToState;
        transitionJson["durationSeconds"] = transition.DurationSeconds;
        transitionJson["exitTimeSeconds"] = transition.ExitTimeSeconds;
        transitionJson["canInterrupt"] = transition.CanInterrupt;
        json conditions = json::array();
        for (const auto& condition : transition.Conditions)
        {
            conditions.push_back(json{
                {"parameter", condition.Parameter},
                {"comparison", AnimationTransitionComparisonToString(condition.Comparison)},
                {"floatValue", condition.FloatValue},
                {"intValue", condition.IntValue},
                {"boolValue", condition.BoolValue}
            });
        }
        transitionJson["conditions"] = std::move(conditions);
        transitions.push_back(std::move(transitionJson));
    }
    doc["transitions"] = std::move(transitions);
    return doc.dump(2);
}

} // namespace Animation
} // namespace GameEngine
