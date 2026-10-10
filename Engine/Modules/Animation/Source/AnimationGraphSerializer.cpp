#include "Animation/AnimationGraphSerializer.h"

#include "Animation/AnimGraphNode.h"
#include "Animation/AnimParam.h"
#include "Animation/AnimationEvent.h"
#include "Animation/AnimationGraphPlayer.h"
#include "Animation/AnimationMontage.h"
#include "Animation/BoneMask.h"
#include "Animation/Nodes/AdditiveBlendNode.h"
#include "Animation/Nodes/Blend2Node.h"
#include "Animation/Nodes/BlendSpace1DNode.h"
#include "Animation/Nodes/BlendSpace2DNode.h"
#include "Animation/Nodes/ClipPlayerNode.h"
#include "Animation/Nodes/FABRIKNode.h"
#include "Animation/Nodes/LayeredBlendNode.h"
#include "Animation/Nodes/LookAtNode.h"
#include "Animation/Nodes/MontageSlotNode.h"
#include "Animation/Nodes/StateMachineNode.h"
#include "Animation/Nodes/TwoBoneIKNode.h"

#include "Assets/AnimationClip.h"
#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"

#include <cmath>
#include <exception>
#include <nlohmann/json.hpp>

namespace GameEngine::Animation
{

using json = nlohmann::json;

// Type strings used as discriminators in the JSON "type" field.
static constexpr const char* kTypeClipPlayer = "ClipPlayer";
static constexpr const char* kTypeBlend2 = "Blend2";
static constexpr const char* kTypeStateMachine = "StateMachine";
static constexpr const char* kTypeBlendSpace1D = "BlendSpace1D";
static constexpr const char* kTypeBlendSpace2D = "BlendSpace2D";
static constexpr const char* kTypeMontageSlot = "MontageSlot";
static constexpr const char* kTypeLayeredBlend = "LayeredBlend";
static constexpr const char* kTypeAdditiveBlend = "AdditiveBlend";
static constexpr const char* kTypeTwoBoneIK = "TwoBoneIK";
static constexpr const char* kTypeFABRIK = "FABRIK";
static constexpr const char* kTypeLookAt = "LookAt";

static constexpr int32_t kFormatVersion = 1;

// ---------------------------------------------------------------------------
// Helpers for ParamValue (variant<float, int32_t, bool>)
// ---------------------------------------------------------------------------

static json SerializeParamValue(const ParamValue& value)
{
    return std::visit([](auto&& v) -> json { return v; }, value);
}

static std::string ParamValueTypeString(const ParamValue& value)
{
    if (std::holds_alternative<float>(value))
        return "float";
    if (std::holds_alternative<int32_t>(value))
        return "int";
    return "bool";
}

static ParamValue DeserializeParamValue(const json& j, const std::string& typeHint)
{
    if (typeHint == "float")
        return j.get<float>();
    if (typeHint == "int")
        return j.get<int32_t>();
    return j.get<bool>();
}

// Infer type from json value when no explicit type hint is available.
static ParamValue InferParamValue(const json& j)
{
    if (j.is_boolean())
        return j.get<bool>();
    if (j.is_number_integer())
        return j.get<int32_t>();
    return j.get<float>();
}

// ---------------------------------------------------------------------------
// Transition condition serialization
// ---------------------------------------------------------------------------

static json SerializeCondition(const TransitionCondition& cond)
{
    json j;
    j["param"] = cond.ParamName;
    j["op"] = ParamCompareName(cond.Op);
    j["value"] = SerializeParamValue(cond.Expected);
    return j;
}

static TransitionCondition DeserializeCondition(const json& j)
{
    if (!j.is_object())
        return TransitionCondition::Invalid();
    ParamCompare op = ParamCompare::Equals;
    const std::string opName = j.contains("op") && j["op"].is_string() ? j["op"].get<std::string>() : "equals";
    if (!TryParamCompareFromName(opName, op))
        return TransitionCondition::Invalid();
    return TransitionCondition::Make(
        j.value("param", ""),
        op,
        InferParamValue(j.contains("value") ? j["value"] : json()));
}

// ---------------------------------------------------------------------------
// AnimationEvent serialization helpers
// ---------------------------------------------------------------------------

static json SerializeEvent(const AnimationEvent& event)
{
    json j;
    j["time"] = event.Time;
    j["name"] = event.Name;

    std::visit([&j](auto&& data) {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, std::string>)
            j["data"] = data;
        else if constexpr (std::is_same_v<T, float>)
            j["data"] = data;
        else if constexpr (std::is_same_v<T, int32_t>)
            j["data"] = data;
        else if constexpr (std::is_same_v<T, bool>)
            j["data"] = data;
        // std::monostate: omit "data" key
    }, event.Data);

    return j;
}

static AnimationEvent DeserializeEvent(const json& j)
{
    AnimationEvent event;
    event.Time = j.value("time", 0.0f);
    event.Name = j.value("name", "");

    if (j.contains("data"))
    {
        const auto& d = j["data"];
        if (d.is_string())
            event.Data = d.get<std::string>();
        else if (d.is_boolean())
            event.Data = d.get<bool>();
        else if (d.is_number_integer())
            event.Data = d.get<int32_t>();
        else if (d.is_number_float())
            event.Data = d.get<float>();
    }

    return event;
}

// ---------------------------------------------------------------------------
// Node serialization (dispatch by dynamic_cast)
// ---------------------------------------------------------------------------

json AnimationGraphSerializer::SerializeNode(const AnimGraphNode* node)
{
    if (!node)
        return nullptr;

    json j;

    if (auto* clip = dynamic_cast<const ClipPlayerNode*>(node))
    {
        j["type"] = kTypeClipPlayer;

        const GUID clipGuid = clip->GetClip() ? clip->GetClip()->GetGUID() : clip->GetClipGuid();
        j["clipGuid"] = clipGuid.IsNull() ? "" : clipGuid.ToString();

        j["speed"] = clip->GetSpeed();
        j["loop"] = clip->GetLooping();
    }
    else if (auto* blend = dynamic_cast<const Blend2Node*>(node))
    {
        j["type"] = kTypeBlend2;
        j["weight"] = blend->GetWeight();
        j["inputA"] = SerializeNode(blend->GetInputA());
        j["inputB"] = SerializeNode(blend->GetInputB());
    }
    else if (auto* sm = dynamic_cast<const StateMachineNode*>(node))
    {
        j["type"] = kTypeStateMachine;

        json statesJson = json::array();
        json transitionsJson = json::array();

        const auto& states = sm->GetStates();
        for (uint32_t i = 0; i < states.size(); ++i)
        {
            json stateJson;
            stateJson["name"] = states[i].Name;
            stateJson["node"] = SerializeNode(states[i].Node.get());
            statesJson.push_back(stateJson);

            for (const auto& transition : states[i].Transitions)
            {
                json transJson;
                transJson["from"] = i;
                transJson["to"] = transition.TargetStateIndex;
                transJson["duration"] = transition.Duration;

                json conditionsJson = json::array();
                for (const auto& cond : transition.Conditions)
                    conditionsJson.push_back(SerializeCondition(cond));
                transJson["conditions"] = conditionsJson;

                transitionsJson.push_back(transJson);
            }
        }

        j["states"] = statesJson;
        j["transitions"] = transitionsJson;
    }
    else if (auto* bs1d = dynamic_cast<const BlendSpace1DNode*>(node))
    {
        j["type"] = kTypeBlendSpace1D;

        json samplesJson = json::array();
        for (const auto& sample : bs1d->GetSamples())
        {
            json sampleJson;
            sampleJson["position"] = sample.Position;
            sampleJson["node"] = SerializeNode(sample.Node.get());
            samplesJson.push_back(sampleJson);
        }
        j["samples"] = samplesJson;
        if (!bs1d->GetParameterName().empty())
            j["parameter"] = bs1d->GetParameterName();
    }
    else if (auto* bs2d = dynamic_cast<const BlendSpace2DNode*>(node))
    {
        j["type"] = kTypeBlendSpace2D;
        j["maxBlendedSamples"] = bs2d->GetMaxBlendedSamples();

        json samplesJson = json::array();
        for (const auto& sample : bs2d->GetSamples())
        {
            json sampleJson;
            sampleJson["x"] = sample.X;
            sampleJson["y"] = sample.Y;
            sampleJson["node"] = SerializeNode(sample.Node.get());
            samplesJson.push_back(sampleJson);
        }
        j["samples"] = samplesJson;
        if (!bs2d->GetParameterNameX().empty())
            j["parameterX"] = bs2d->GetParameterNameX();
        if (!bs2d->GetParameterNameY().empty())
            j["parameterY"] = bs2d->GetParameterNameY();
    }
    else if (auto* montageSlot = dynamic_cast<const MontageSlotNode*>(node))
    {
        j["type"] = kTypeMontageSlot;
        j["source"] = SerializeNode(montageSlot->GetSource());
    }
    else if (auto* layered = dynamic_cast<const LayeredBlendNode*>(node))
    {
        j["type"] = kTypeLayeredBlend;
        j["weight"] = layered->GetBlendWeight();
        j["base"] = SerializeNode(layered->GetBase());
        j["overlay"] = SerializeNode(layered->GetOverlay());

        const auto& mask = layered->GetMask();
        if (!mask.Weights.empty())
            j["maskWeights"] = mask.Weights;
    }
    else if (auto* additive = dynamic_cast<const AdditiveBlendNode*>(node))
    {
        j["type"] = kTypeAdditiveBlend;
        j["weight"] = additive->GetWeight();
        j["base"] = SerializeNode(additive->GetBase());
        j["additive"] = SerializeNode(additive->GetAdditive());
    }
    else if (auto* twoBone = dynamic_cast<const TwoBoneIKNode*>(node))
    {
        j["type"] = kTypeTwoBoneIK;
        j["rootBone"] = twoBone->GetRootBone();
        j["midBone"] = twoBone->GetMidBone();
        j["tipBone"] = twoBone->GetTipBone();
        j["weight"] = twoBone->GetWeight();
        j["source"] = SerializeNode(twoBone->GetSource());
    }
    else if (auto* fabrik = dynamic_cast<const FABRIKNode*>(node))
    {
        j["type"] = kTypeFABRIK;
        j["chain"] = fabrik->GetChain();
        j["weight"] = fabrik->GetWeight();
        j["maxIterations"] = fabrik->GetMaxIterations();
        j["tolerance"] = fabrik->GetTolerance();
        j["source"] = SerializeNode(fabrik->GetSource());
    }
    else if (auto* lookAt = dynamic_cast<const LookAtNode*>(node))
    {
        j["type"] = kTypeLookAt;
        j["boneIndex"] = lookAt->GetBoneIndex();
        j["weight"] = lookAt->GetWeight();
        j["aimAxis"] = {lookAt->GetAimAxis().x, lookAt->GetAimAxis().y, lookAt->GetAimAxis().z};
        j["maxAngle"] = lookAt->GetMaxAngle();
        j["source"] = SerializeNode(lookAt->GetSource());
    }

    return j;
}

// ---------------------------------------------------------------------------
// Node deserialization (dispatch by "type" string)
// ---------------------------------------------------------------------------

std::unique_ptr<AnimGraphNode> AnimationGraphSerializer::DeserializeNode(const json& j)
{
    if (j.is_null() || !j.is_object() || !j.contains("type") || !j["type"].is_string())
        return nullptr;

    std::string type = j["type"].get<std::string>();

    if (type == kTypeClipPlayer)
    {
        auto node = std::make_unique<ClipPlayerNode>();
        if (j.contains("clipGuid"))
        {
            const std::string guidText = j["clipGuid"].is_string() ? j["clipGuid"].get<std::string>() : "";
            if (!guidText.empty())
                node->SetClipGuid(GUID(guidText));
        }
        float speed = j.value("speed", 1.0f);
        if (!std::isfinite(speed))
            speed = 1.0f;
        node->SetSpeed(speed);
        node->SetLooping(j.value("loop", true));
        return node;
    }

    if (type == kTypeBlend2)
    {
        auto node = std::make_unique<Blend2Node>();
        node->SetWeight(j.value("weight", 0.0f));
        if (j.contains("inputA"))
            node->SetInputA(DeserializeNode(j["inputA"]));
        if (j.contains("inputB"))
            node->SetInputB(DeserializeNode(j["inputB"]));
        return node;
    }

    if (type == kTypeStateMachine)
    {
        auto node = std::make_unique<StateMachineNode>();

        if (j.contains("states"))
        {
            for (const auto& stateJson : j["states"])
            {
                std::string name = stateJson.value("name", "");
                auto childNode = DeserializeNode(stateJson.value("node", json()));
                node->AddState(std::move(name), std::move(childNode));
            }
        }

        if (j.contains("transitions"))
        {
            for (const auto& transJson : j["transitions"])
            {
                uint32_t from = transJson.value("from", 0u);
                uint32_t to = transJson.value("to", 0u);
                float duration = transJson.value("duration", 0.2f);

                std::vector<TransitionCondition> conditions;
                if (transJson.contains("conditions"))
                {
                    for (const auto& condJson : transJson["conditions"])
                        conditions.push_back(DeserializeCondition(condJson));
                }

                node->AddTransition(from, to, duration, std::move(conditions));
            }
        }

        return node;
    }

    if (type == kTypeBlendSpace1D)
    {
        auto node = std::make_unique<BlendSpace1DNode>();
        if (j.contains("samples"))
        {
            for (const auto& sampleJson : j["samples"])
            {
                float position = sampleJson.value("position", 0.0f);
                auto childNode = DeserializeNode(sampleJson.value("node", json()));
                if (childNode)
                    node->AddSample(std::move(childNode), position);
            }
            node->Sort();
        }
        if (j.contains("parameter") && j["parameter"].is_string())
            node->SetParameterName(j["parameter"].get<std::string>());
        return node;
    }

    if (type == kTypeBlendSpace2D)
    {
        auto node = std::make_unique<BlendSpace2DNode>();
        node->SetMaxBlendedSamples(j.value("maxBlendedSamples", 4u));

        if (j.contains("samples"))
        {
            for (const auto& sampleJson : j["samples"])
            {
                float x = sampleJson.value("x", 0.0f);
                float y = sampleJson.value("y", 0.0f);
                auto childNode = DeserializeNode(sampleJson.value("node", json()));
                if (childNode)
                    node->AddSample(std::move(childNode), x, y);
            }
        }
        if (j.contains("parameterX") && j["parameterX"].is_string())
            node->SetParameterNameX(j["parameterX"].get<std::string>());
        if (j.contains("parameterY") && j["parameterY"].is_string())
            node->SetParameterNameY(j["parameterY"].get<std::string>());
        return node;
    }

    if (type == kTypeMontageSlot)
    {
        auto node = std::make_unique<MontageSlotNode>();
        if (j.contains("source"))
        {
            auto sourceNode = DeserializeNode(j["source"]);
            if (sourceNode)
                node->SetSource(std::move(sourceNode));
        }
        return node;
    }

    if (type == kTypeLayeredBlend)
    {
        auto node = std::make_unique<LayeredBlendNode>();
        node->SetBlendWeight(j.value("weight", 1.0f));

        if (j.contains("base"))
        {
            auto baseNode = DeserializeNode(j["base"]);
            if (baseNode)
                node->SetBase(std::move(baseNode));
        }
        if (j.contains("overlay"))
        {
            auto overlayNode = DeserializeNode(j["overlay"]);
            if (overlayNode)
                node->SetOverlay(std::move(overlayNode));
        }
        if (j.contains("maskWeights"))
        {
            BoneMask mask;
            mask.Weights = j["maskWeights"].get<std::vector<float>>();
            node->SetMask(mask);
        }
        return node;
    }

    if (type == kTypeAdditiveBlend)
    {
        auto node = std::make_unique<AdditiveBlendNode>();
        node->SetWeight(j.value("weight", 1.0f));

        if (j.contains("base"))
        {
            auto baseNode = DeserializeNode(j["base"]);
            if (baseNode)
                node->SetBase(std::move(baseNode));
        }
        if (j.contains("additive"))
        {
            auto additiveNode = DeserializeNode(j["additive"]);
            if (additiveNode)
                node->SetAdditive(std::move(additiveNode));
        }
        return node;
    }

    if (type == kTypeTwoBoneIK)
    {
        auto node = std::make_unique<TwoBoneIKNode>();
        node->SetBoneIndices(
            j.value("rootBone", 0u),
            j.value("midBone", 0u),
            j.value("tipBone", 0u));
        node->SetWeight(j.value("weight", 1.0f));

        if (j.contains("source"))
        {
            auto sourceNode = DeserializeNode(j["source"]);
            if (sourceNode)
                node->SetSource(std::move(sourceNode));
        }
        return node;
    }

    if (type == kTypeFABRIK)
    {
        auto node = std::make_unique<FABRIKNode>();
        if (j.contains("chain"))
            node->SetChain(j["chain"].get<std::vector<uint32_t>>());
        node->SetWeight(j.value("weight", 1.0f));
        node->SetMaxIterations(j.value("maxIterations", 10u));
        node->SetTolerance(j.value("tolerance", 0.001f));

        if (j.contains("source"))
        {
            auto sourceNode = DeserializeNode(j["source"]);
            if (sourceNode)
                node->SetSource(std::move(sourceNode));
        }
        return node;
    }

    if (type == kTypeLookAt)
    {
        auto node = std::make_unique<LookAtNode>();
        node->SetBoneIndex(j.value("boneIndex", 0u));
        node->SetWeight(j.value("weight", 1.0f));
        node->SetMaxAngle(j.value("maxAngle", 1.0472f));

        if (j.contains("aimAxis") && j["aimAxis"].is_array() && j["aimAxis"].size() == 3)
        {
            node->SetAimAxis(Mathematics::Vector3(
                j["aimAxis"][0].get<float>(),
                j["aimAxis"][1].get<float>(),
                j["aimAxis"][2].get<float>()));
        }

        if (j.contains("source"))
        {
            auto sourceNode = DeserializeNode(j["source"]);
            if (sourceNode)
                node->SetSource(std::move(sourceNode));
        }
        return node;
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Graph player serialization
// ---------------------------------------------------------------------------

json AnimationGraphSerializer::Serialize(const AnimationGraphPlayer& player)
{
    json root;
    root["version"] = kFormatVersion;

    // Parameters with type info so we can reconstruct the variant on load.
    json params = json::object();
    for (const auto& [id, param] : player.Parameters)
    {
        json paramJson;
        paramJson["type"] = ParamValueTypeString(param.Value);
        paramJson["default"] = SerializeParamValue(param.Value);
        const std::string key = param.Name.empty() ? std::to_string(id) : param.Name;
        params[key] = paramJson;
    }
    root["parameters"] = params;

    root["rootNode"] = SerializeNode(player.RootNode.get());

    return root;
}

std::unique_ptr<AnimationGraphPlayer> AnimationGraphSerializer::Deserialize(const json& j)
{
    try
    {
        if (!j.is_object())
            return nullptr;

        auto player = std::make_unique<AnimationGraphPlayer>();

        if (j.contains("parameters") && j["parameters"].is_object())
        {
            for (auto& [name, paramJson] : j["parameters"].items())
            {
                if (!paramJson.is_object() || !paramJson.contains("default"))
                    continue;
                std::string type = paramJson.value("type", "float");
                player->SetParameter(name, DeserializeParamValue(paramJson["default"], type));
            }
        }

        if (j.contains("rootNode") && !j["rootNode"].is_null())
        {
            player->RootNode = DeserializeNode(j["rootNode"]);
            if (!player->RootNode)
                return nullptr;
        }

        return player;
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Montage serialization
// ---------------------------------------------------------------------------

json AnimationGraphSerializer::SerializeMontage(const AnimationMontage& montage)
{
    json root;
    root["clipGuid"] = montage.GetClipGuid();
    root["blendIn"] = montage.GetBlendInDuration();
    root["blendOut"] = montage.GetBlendOutDuration();
    root["playRate"] = montage.GetPlayRate();

    json sectionsJson = json::array();
    for (const auto& section : montage.GetSections())
    {
        json sectionJson;
        sectionJson["name"] = section.Name;
        sectionJson["start"] = section.StartTime;
        sectionJson["end"] = section.EndTime;
        if (!section.NextSection.empty())
            sectionJson["nextSection"] = section.NextSection;
        sectionsJson.push_back(sectionJson);
    }
    root["sections"] = sectionsJson;

    json eventsJson = json::array();
    for (const auto& event : montage.GetEventTrack().GetEvents())
        eventsJson.push_back(SerializeEvent(event));
    root["events"] = eventsJson;

    return root;
}

std::unique_ptr<AnimationMontage> AnimationGraphSerializer::DeserializeMontage(const json& j)
{
    if (!j.is_object())
        return nullptr;

    auto montage = std::make_unique<AnimationMontage>();

    montage->SetClipGuid(j.value("clipGuid", ""));
    montage->SetBlendInDuration(j.value("blendIn", kDefaultBlendInDuration));
    montage->SetBlendOutDuration(j.value("blendOut", kDefaultBlendOutDuration));
    montage->SetPlayRate(j.value("playRate", kDefaultPlayRate));

    if (j.contains("sections"))
    {
        for (const auto& sectionJson : j["sections"])
        {
            MontageSection section;
            section.Name = sectionJson.value("name", "");
            section.StartTime = sectionJson.value("start", 0.0f);
            section.EndTime = sectionJson.value("end", 0.0f);
            section.NextSection = sectionJson.value("nextSection", "");
            montage->AddSection(section);
        }
    }

    if (j.contains("events"))
    {
        for (const auto& eventJson : j["events"])
            montage->GetEventTrack().AddEvent(DeserializeEvent(eventJson));
    }

    return montage;
}

} // namespace GameEngine::Animation
