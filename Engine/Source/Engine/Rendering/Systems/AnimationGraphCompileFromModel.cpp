#include "ECSModules/Rendering/Systems/AnimationGraphCompileFromModel.h"

#include "Animation/AnimGraphNode.h"
#include "Animation/AnimParam.h"
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
#include "AssetCore/GUID.h"
#include "Graph/GraphModel.h"
#include "Graph/GraphValue.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine { namespace Engine::Renderer {

namespace {

using Animation::AdditiveBlendNode;
using Animation::AnimGraphNode;
using Animation::AnimationGraphPlayer;
using Animation::Blend2Node;
using Animation::BlendSpace1DNode;
using Animation::BlendSpace2DNode;
using Animation::ClipPlayerNode;
using Animation::FABRIKNode;
using Animation::LayeredBlendNode;
using Animation::LookAtNode;
using Animation::MontageSlotNode;
using Animation::ParamCompare;
using Animation::ParamValue;
using Animation::StateMachineNode;
using Animation::TransitionCondition;
using Animation::TwoBoneIKNode;
using Animation::TryParamCompareFromName;

constexpr std::string_view kTypeOutputPose = "OutputPose";
constexpr std::string_view kTypeClipPlayer = "ClipPlayer";
constexpr std::string_view kTypeBlend2 = "Blend2";
constexpr std::string_view kTypeLayeredBlend = "LayeredBlend";
constexpr std::string_view kTypeAdditiveBlend = "AdditiveBlend";
constexpr std::string_view kTypeTwoBoneIK = "TwoBoneIK";
constexpr std::string_view kTypeFabrik = "FABRIK";
constexpr std::string_view kTypeLookAt = "LookAt";
constexpr std::string_view kTypeMontageSlot = "MontageSlot";
constexpr std::string_view kTypeStateMachine = "StateMachine";
constexpr std::string_view kTypeBlendSpace1D = "BlendSpace1D";
constexpr std::string_view kTypeBlendSpace2D = "BlendSpace2D";
constexpr std::string_view kTypeState = "State";
constexpr std::string_view kTypeEntry = "Entry";
constexpr std::string_view kExtensionSubgraph = "subgraph";
constexpr std::string_view kExtensionBlendSpace1D = "blendSpace1D";
constexpr std::string_view kExtensionBlendSpace2D = "blendSpace2D";
constexpr size_t kMaxPoseCompileDepth = 64;
constexpr float kDefaultTransitionDuration = 0.2f;

const Graph::Edge* FindFirstInbound(const Graph::Model& model,
                                    const std::string& hostId,
                                    std::string_view portId)
{
    // Multiple wires to one port: first in Links order, rest ignored.
    for (const auto& link : model.Links)
    {
        if (link.TargetNodeId == hostId && link.TargetPortId == portId)
            return &link;
    }
    return nullptr;
}

const Graph::Edge* FindFirstOutbound(const Graph::Model& model,
                                     const std::string& hostId,
                                     std::string_view portId)
{
    for (const auto& link : model.Links)
    {
        if (link.SourceNodeId == hostId && link.SourcePortId == portId)
            return &link;
    }
    return nullptr;
}

void ApplyClipGuid(ClipPlayerNode& node, const Graph::GraphObject& obj)
{
    const std::string clipGuid = obj.GetString("clipGuid");
    if (clipGuid.empty())
        return;
    const GUID guid(clipGuid);
    if (!guid.IsNull())
        node.SetClipGuid(guid);
}

std::unique_ptr<AnimGraphNode> CompileNode(const Graph::Model& model,
                                           const std::string& nodeId,
                                           std::unordered_set<std::string>& visiting,
                                           bool allowStateMachine);

bool CompileOptionalPort(const Graph::Model& model,
                        const std::string& hostId,
                        std::string_view portId,
                        std::unordered_set<std::string>& visiting,
                        bool allowStateMachine,
                        std::unique_ptr<AnimGraphNode>& out)
{
    const Graph::Edge* link = FindFirstInbound(model, hostId, portId);
    if (!link)
    {
        out.reset();
        return true;
    }
    out = CompileNode(model, link->SourceNodeId, visiting, allowStateMachine);
    return static_cast<bool>(out);
}

bool TryLoadNestedModel(const Graph::Node& host, Graph::Model& out)
{
    const auto it = host.Extensions.find(kExtensionSubgraph);
    if (it == host.Extensions.end())
        return false;
    const std::string* json = it->second.TryString();
    if (!json || json->empty())
        return false;

    Graph::Model loaded;
    if (!Graph::FromJson(*json, loaded))
        return false;
    out = std::move(loaded);
    return true;
}

bool CompileStateMotion(const Graph::Node& state, std::unique_ptr<AnimGraphNode>& out)
{
    out.reset();
    if (state.Extensions.find(kExtensionSubgraph) == state.Extensions.end())
        return true;

    Graph::Model poseModel;
    if (!TryLoadNestedModel(state, poseModel))
        return false;

    const Graph::Node* output = nullptr;
    size_t outputCount = 0;
    const Graph::Node* leaf = nullptr;
    size_t leafCount = 0;
    for (const auto& node : poseModel.Nodes)
    {
        if (node.TypeId == kTypeOutputPose)
        {
            ++outputCount;
            output = &node;
        }
        else if (node.TypeId == kTypeClipPlayer || node.TypeId == kTypeBlendSpace1D)
        {
            ++leafCount;
            leaf = &node;
        }
    }

    if (outputCount > 1)
        return false;

    if (outputCount == 1)
    {
        const Graph::Edge* poseLink = FindFirstInbound(poseModel, output->Id, "pose");
        if (!poseLink)
            return false;
        std::unordered_set<std::string> visiting;
        out = CompileNode(poseModel, poseLink->SourceNodeId, visiting, false);
        return static_cast<bool>(out);
    }

    if (leafCount == 1)
    {
        std::unordered_set<std::string> visiting;
        out = CompileNode(poseModel, leaf->Id, visiting, false);
        return static_cast<bool>(out);
    }

    return true;
}

bool TryParseParamValue(const Graph::GraphValue& value, ParamValue& out)
{
    if (value.IsBool())
    {
        out = value.AsBool();
        return true;
    }
    if (value.IsInt())
    {
        const std::int64_t n = value.AsInt();
        if (n < std::numeric_limits<int32_t>::min() || n > std::numeric_limits<int32_t>::max())
            return false;
        out = static_cast<int32_t>(n);
        return true;
    }
    if (value.IsFloat())
    {
        const float n = static_cast<float>(value.AsFloat());
        if (!std::isfinite(n))
            return false;
        out = n;
        return true;
    }
    return false;
}

bool TryParseCondition(const Graph::GraphObject& obj, TransitionCondition& out)
{
    const std::string param = obj.GetString("param");
    if (param.empty())
        return false;
    ParamCompare op = ParamCompare::Equals;
    if (!TryParamCompareFromName(obj.GetString("op", "equals"), op))
        return false;
    const auto valueIt = obj.find("value");
    if (valueIt == obj.end())
        return false;
    ParamValue expected;
    if (!TryParseParamValue(valueIt->second, expected))
        return false;
    out = TransitionCondition::Make(param, op, std::move(expected));
    return true;
}

bool CompileStateTransitions(const Graph::Model& nested,
                             const std::vector<const Graph::Node*>& states,
                             StateMachineNode& sm)
{
    std::unordered_map<std::string, uint32_t> indexById;
    indexById.reserve(states.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(states.size()); ++i)
        indexById.emplace(states[i]->Id, i);

    for (const auto& link : nested.Links)
    {
        if (link.SourcePortId != "out" || link.TargetPortId != "in")
            continue;
        const auto fromIt = indexById.find(link.SourceNodeId);
        if (fromIt == indexById.end())
            continue;
        const auto toIt = indexById.find(link.TargetNodeId);
        if (toIt == indexById.end())
            return false;

        const auto condIt = link.Passthrough.find("conditions");
        if (condIt == link.Passthrough.end())
            continue;
        const std::vector<Graph::GraphValue>* list = condIt->second.TryList();
        if (!list)
            return false;

        std::vector<TransitionCondition> conditions;
        conditions.reserve(list->size());
        for (const Graph::GraphValue& entry : *list)
        {
            const Graph::GraphObject* obj = entry.TryObject();
            if (!obj)
                return false;
            TransitionCondition cond;
            if (!TryParseCondition(*obj, cond))
                return false;
            conditions.push_back(std::move(cond));
        }
        if (conditions.empty())
            continue;

        float duration = kDefaultTransitionDuration;
        const auto durIt = link.Passthrough.find("duration");
        if (durIt != link.Passthrough.end())
        {
            if (!durIt->second.IsNumber() || !std::isfinite(durIt->second.AsFloat()))
                return false;
            duration = static_cast<float>(durIt->second.AsFloat());
            if (!std::isfinite(duration) || duration < 0.f)
                return false;
        }
        sm.AddTransition(fromIt->second, toIt->second, duration, std::move(conditions));
    }
    return true;
}

std::unique_ptr<AnimGraphNode> CompileStateMachine(const Graph::Node& host)
{
    Graph::Model nested;
    if (!TryLoadNestedModel(host, nested))
        return nullptr;

    std::vector<const Graph::Node*> states;
    const Graph::Node* entry = nullptr;
    for (const auto& node : nested.Nodes)
    {
        if (node.TypeId == kTypeState)
            states.push_back(&node);
        else if (node.TypeId == kTypeEntry && !entry)
            entry = &node;
    }

    auto sm = std::make_unique<StateMachineNode>();
    for (const Graph::Node* state : states)
    {
        const std::string title = state->Parameters.GetString("title");
        const std::string name = title.empty() ? state->Id : title;
        std::unique_ptr<AnimGraphNode> motion;
        if (!CompileStateMotion(*state, motion))
            return nullptr;
        sm->AddState(name, std::move(motion));
    }

    uint32_t initial = 0;
    if (entry)
    {
        const Graph::Edge* outLink = FindFirstOutbound(nested, entry->Id, "out");
        if (outLink)
        {
            for (uint32_t i = 0; i < states.size(); ++i)
            {
                if (states[i]->Id == outLink->TargetNodeId)
                {
                    initial = i;
                    break;
                }
            }
        }
    }
    sm->SetActiveState(initial);
    if (!CompileStateTransitions(nested, states, *sm))
        return nullptr;
    return sm;
}

std::unique_ptr<AnimGraphNode> CompileBlendSpace1D(const Graph::Node& host)
{
    auto node = std::make_unique<BlendSpace1DNode>();
    node->SetParameterName(host.Parameters.GetString("parameter", "Speed"));

    const auto it = host.Extensions.find(kExtensionBlendSpace1D);
    if (it == host.Extensions.end())
        return node;
    const Graph::GraphObject* bag = it->second.TryObject();
    if (!bag)
        return node;
    const auto samplesIt = bag->find("samples");
    if (samplesIt == bag->end())
        return node;
    const std::vector<Graph::GraphValue>* list = samplesIt->second.TryList();
    if (!list)
        return node;

    for (const Graph::GraphValue& entry : *list)
    {
        const Graph::GraphObject* sample = entry.TryObject();
        if (!sample)
            continue;
        const auto posIt = sample->find("position");
        if (posIt == sample->end() || !posIt->second.IsNumber())
            continue;
        const float position = static_cast<float>(posIt->second.AsFloat());
        if (!std::isfinite(position))
            continue;
        auto clip = std::make_unique<ClipPlayerNode>();
        ApplyClipGuid(*clip, *sample);
        node->AddSample(std::move(clip), position);
    }
    node->Sort();
    return node;
}

std::unique_ptr<AnimGraphNode> CompileBlendSpace2D(const Graph::Node& host)
{
    auto node = std::make_unique<BlendSpace2DNode>();
    node->SetParameterNameX(host.Parameters.GetString("parameterX", "Speed"));
    node->SetParameterNameY(host.Parameters.GetString("parameterY", "Direction"));

    const auto it = host.Extensions.find(kExtensionBlendSpace2D);
    if (it == host.Extensions.end())
        return node;
    const Graph::GraphObject* bag = it->second.TryObject();
    if (!bag)
        return node;
    const auto samplesIt = bag->find("samples");
    if (samplesIt == bag->end())
        return node;
    const std::vector<Graph::GraphValue>* list = samplesIt->second.TryList();
    if (!list)
        return node;

    for (const Graph::GraphValue& entry : *list)
    {
        const Graph::GraphObject* sample = entry.TryObject();
        if (!sample)
            continue;
        const auto xIt = sample->find("x");
        const auto yIt = sample->find("y");
        if (xIt == sample->end() || yIt == sample->end() || !xIt->second.IsNumber() || !yIt->second.IsNumber())
            continue;
        const float x = static_cast<float>(xIt->second.AsFloat());
        const float y = static_cast<float>(yIt->second.AsFloat());
        if (!std::isfinite(x) || !std::isfinite(y))
            continue;
        auto clip = std::make_unique<ClipPlayerNode>();
        ApplyClipGuid(*clip, *sample);
        node->AddSample(std::move(clip), x, y);
    }
    return node;
}

std::unique_ptr<AnimGraphNode> CompileNode(const Graph::Model& model,
                                           const std::string& nodeId,
                                           std::unordered_set<std::string>& visiting,
                                           bool allowStateMachine)
{
    if (!visiting.insert(nodeId).second)
        return nullptr;
    if (visiting.size() > kMaxPoseCompileDepth)
    {
        visiting.erase(nodeId);
        return nullptr;
    }

    struct VisitGuard
    {
        std::unordered_set<std::string>& Visiting;
        const std::string& Id;
        ~VisitGuard() { Visiting.erase(Id); }
    } guard{visiting, nodeId};

    const Graph::Node* host = model.FindNode(nodeId);
    if (!host)
        return nullptr;

    const std::string_view typeId = host->TypeId;

    if (typeId == kTypeClipPlayer)
    {
        auto node = std::make_unique<ClipPlayerNode>();
        node->SetSpeed(static_cast<float>(host->Parameters.GetFloat("speed", 1.0)));
        node->SetLooping(host->Parameters.GetBool("looping", true));
        ApplyClipGuid(*node, host->Parameters);
        return node;
    }

    if (typeId == kTypeBlend2)
    {
        auto node = std::make_unique<Blend2Node>();
        node->SetWeight(static_cast<float>(host->Parameters.GetFloat("weight", 0.5)));
        std::unique_ptr<AnimGraphNode> inputA;
        std::unique_ptr<AnimGraphNode> inputB;
        if (!CompileOptionalPort(model, host->Id, "a", visiting, allowStateMachine, inputA) ||
            !CompileOptionalPort(model, host->Id, "b", visiting, allowStateMachine, inputB))
        {
            return nullptr;
        }
        node->SetInputA(std::move(inputA));
        node->SetInputB(std::move(inputB));
        return node;
    }

    if (typeId == kTypeLayeredBlend)
    {
        auto node = std::make_unique<LayeredBlendNode>();
        node->SetBlendWeight(static_cast<float>(host->Parameters.GetFloat("weight", 1.0)));
        std::unique_ptr<AnimGraphNode> base;
        std::unique_ptr<AnimGraphNode> overlay;
        if (!CompileOptionalPort(model, host->Id, "base", visiting, allowStateMachine, base) ||
            !CompileOptionalPort(model, host->Id, "layer", visiting, allowStateMachine, overlay))
        {
            return nullptr;
        }
        node->SetBase(std::move(base));
        node->SetOverlay(std::move(overlay));
        return node;
    }

    if (typeId == kTypeAdditiveBlend)
    {
        auto node = std::make_unique<AdditiveBlendNode>();
        node->SetWeight(static_cast<float>(host->Parameters.GetFloat("weight", 1.0)));
        std::unique_ptr<AnimGraphNode> base;
        std::unique_ptr<AnimGraphNode> additive;
        if (!CompileOptionalPort(model, host->Id, "base", visiting, allowStateMachine, base) ||
            !CompileOptionalPort(model, host->Id, "additive", visiting, allowStateMachine, additive))
        {
            return nullptr;
        }
        node->SetBase(std::move(base));
        node->SetAdditive(std::move(additive));
        return node;
    }

    if (typeId == kTypeTwoBoneIK)
    {
        auto node = std::make_unique<TwoBoneIKNode>();
        if (host->Parameters.contains("weight"))
            node->SetWeight(static_cast<float>(host->Parameters.GetFloat("weight", 1.0)));
        std::unique_ptr<AnimGraphNode> source;
        if (!CompileOptionalPort(model, host->Id, "pose", visiting, allowStateMachine, source))
            return nullptr;
        node->SetSource(std::move(source));
        return node;
    }

    if (typeId == kTypeFabrik)
    {
        auto node = std::make_unique<FABRIKNode>();
        if (host->Parameters.contains("weight"))
            node->SetWeight(static_cast<float>(host->Parameters.GetFloat("weight", 1.0)));
        std::unique_ptr<AnimGraphNode> source;
        if (!CompileOptionalPort(model, host->Id, "pose", visiting, allowStateMachine, source))
            return nullptr;
        node->SetSource(std::move(source));
        return node;
    }

    if (typeId == kTypeLookAt)
    {
        auto node = std::make_unique<LookAtNode>();
        if (host->Parameters.contains("weight"))
            node->SetWeight(static_cast<float>(host->Parameters.GetFloat("weight", 1.0)));
        std::unique_ptr<AnimGraphNode> source;
        if (!CompileOptionalPort(model, host->Id, "pose", visiting, allowStateMachine, source))
            return nullptr;
        node->SetSource(std::move(source));
        return node;
    }

    if (typeId == kTypeMontageSlot)
    {
        auto node = std::make_unique<MontageSlotNode>();
        std::unique_ptr<AnimGraphNode> source;
        if (!CompileOptionalPort(model, host->Id, "pose", visiting, allowStateMachine, source))
            return nullptr;
        node->SetSource(std::move(source));
        return node;
    }

    if (typeId == kTypeStateMachine)
    {
        if (!allowStateMachine)
            return nullptr;
        return CompileStateMachine(*host);
    }

    if (typeId == kTypeBlendSpace1D)
        return CompileBlendSpace1D(*host);

    if (typeId == kTypeBlendSpace2D)
        return CompileBlendSpace2D(*host);

    if (typeId == kTypeState || typeId == kTypeEntry)
        return nullptr;

    return nullptr;
}

void ApplyModelVariables(const Graph::Model& model, AnimationGraphPlayer& player)
{
    for (const auto& variable : model.Variables)
    {
        if (variable.Name.empty())
            continue;

        Graph::GraphValue typed;
        if (variable.Type == "float")
            typed = 0.0;
        else if (variable.Type == "int")
            typed = 0;
        else if (variable.Type == "bool")
            typed = false;
        else
            continue;

        if (!typed.AssignFromText(variable.Value))
            continue;

        if (typed.IsFloat())
        {
            player.SetParameter(variable.Name, ParamValue{static_cast<float>(typed.AsFloat())});
        }
        else if (typed.IsInt())
        {
            const std::int64_t value = typed.AsInt();
            if (value < std::numeric_limits<int32_t>::min() ||
                value > std::numeric_limits<int32_t>::max())
            {
                continue;
            }
            player.SetParameter(variable.Name, ParamValue{static_cast<int32_t>(value)});
        }
        else if (typed.IsBool())
        {
            player.SetParameter(variable.Name, ParamValue{typed.AsBool()});
        }
    }
}

} // namespace

bool LooksLikeAuthoringModel(const nlohmann::json& doc)
{
    return doc.is_object() && doc.contains("nodes") && doc["nodes"].is_array() &&
           !doc.contains("rootNode");
}

std::unique_ptr<Animation::AnimationGraphPlayer> CompileAuthoringModel(const nlohmann::json& doc)
{
    Graph::Model model;
    if (!Graph::FromJson(doc.dump(), model))
        return nullptr;

    const Graph::Node* output = nullptr;
    for (const auto& node : model.Nodes)
    {
        if (node.TypeId != kTypeOutputPose)
            continue;
        if (output)
            return nullptr;
        output = &node;
    }
    if (!output)
        return nullptr;

    const Graph::Edge* poseLink = FindFirstInbound(model, output->Id, "pose");
    if (!poseLink)
        return nullptr;

    std::unordered_set<std::string> visiting;
    auto root = CompileNode(model, poseLink->SourceNodeId, visiting, true);
    if (!root)
        return nullptr;

    auto player = std::make_unique<Animation::AnimationGraphPlayer>();
    player->RootNode = std::move(root);
    ApplyModelVariables(model, *player);
    return player;
}

} } // namespace GameEngine::Engine::Renderer
