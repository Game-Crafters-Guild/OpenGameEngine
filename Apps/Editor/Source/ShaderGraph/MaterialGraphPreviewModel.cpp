#include "ShaderGraph/MaterialGraphPreviewModel.h"

#include "Logger/Logger.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <vector>

namespace GameEngine {
namespace Editor {
namespace {

/* The generic user block is four vec4s, registered by the engine as the scalars
   user0..user15. One scalar per promoted pin default. */
constexpr std::uint32_t kUserScalarLanes = 16;

/* Parameters the bridge and compiler consume as IDENTITY rather than value:
   "slot" is stoi'd into a parameter binding, the rest name a variable, a
   texture or a swizzle. Rewriting one to a lane read would break the binding,
   not make it live. */
bool IsStructuralParameter(std::string_view key)
{
    return key == "slot" || key == "component" || key == "swizzle" || key == "variableName"
           || key == "texture";
}

/** A pin fed by a wire ignores its default, so promoting that default would burn
    a lane the shader never reads. */
bool PinHasIncomingEdge(const Graph::Model& model, const std::string& nodeId,
                        const std::string& pin)
{
    for (const Graph::Edge& link : model.Links)
    {
        if (link.TargetNodeId == nodeId && link.TargetPortId == pin)
            return true;
    }
    return false;
}

/** `Mat.uUser<lane/4>.<component>` — the read the surface compiles to. */
std::string LaneExpression(std::uint32_t lane)
{
    constexpr char kComponents[] = {'x', 'y', 'z', 'w'};
    return "Mat.uUser" + std::to_string(lane / 4) + "." + kComponents[lane % 4];
}

/** The material property key the engine registers for that same lane. */
std::string LanePropertyKey(std::uint32_t lane)
{
    return "user" + std::to_string(lane);
}

/** Numeric-aware id order: "node_9" sorts before "node_10". Lanes are handed out
    in this order rather than in model order, so appending a node — which always
    takes the next index — leaves every existing node's lane where it was. Model
    order would shift them all and rewrite every surface for one added node. */
bool NodeIdLess(const std::string& a, const std::string& b)
{
    std::size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i])
        ++i;
    const bool digitsA = i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]));
    const bool digitsB = i < b.size() && std::isdigit(static_cast<unsigned char>(b[i]));
    if (digitsA && digitsB)
    {
        const auto runLength = [](const std::string& s, std::size_t from) {
            std::size_t n = from;
            while (n < s.size() && std::isdigit(static_cast<unsigned char>(s[n])))
                ++n;
            return n - from;
        };
        const std::size_t lenA = runLength(a, i);
        const std::size_t lenB = runLength(b, i);
        if (lenA != lenB)
            return lenA < lenB;
    }
    return a < b;
}

} // namespace

MaterialGraphPreviewModel MakeMaterialGraphPreviewModel(const Graph::Model& model)
{
    MaterialGraphPreviewModel preview;
    preview.Model = model;

    std::vector<Graph::Node*> ordered;
    ordered.reserve(preview.Model.Nodes.size());
    for (Graph::Node& node : preview.Model.Nodes)
        ordered.push_back(&node);
    std::sort(ordered.begin(), ordered.end(),
              [](const Graph::Node* a, const Graph::Node* b) { return NodeIdLess(a->Id, b->Id); });

    std::uint32_t lane = 0;
    for (Graph::Node* nodePtr : ordered)
    {
        Graph::Node& node = *nodePtr;

        /* Every numeric pin default, not just the ones on Constant nodes: a
           Fresnel's power or a Multiply's operand is typed into the node the
           same way and has to go live the same way. Keys are taken in sorted
           order so the assignment does not depend on parameter insertion. */
        std::vector<std::string> keys;
        keys.reserve(node.Parameters.size());
        for (const auto& [key, value] : node.Parameters)
            keys.push_back(key);
        std::sort(keys.begin(), keys.end());

        for (const std::string& pin : keys)
        {
            if (IsStructuralParameter(pin))
                continue;
            if (PinHasIncomingEdge(preview.Model, node.Id, pin))
                continue;

            const auto it = node.Parameters.find(pin);
            if (it == node.Parameters.end())
                continue;

            /* Node parameters are authored as text, so "is this a number" is a
               parse, not a kind check. Asking twice with different fallbacks is
               what separates a value that parsed from one that did not — and it
               goes through GraphValue's own parser rather than a second one. */
            const double value = it->second.AsFloat(0.0);
            if (value != it->second.AsFloat(1.0))
                continue;

            if (lane >= kUserScalarLanes)
            {
                ++preview.UnboundScalars;
                continue;
            }

            preview.LaneValues.emplace_back(LanePropertyKey(lane), static_cast<float>(value));
            // A String value: the compiler splices a pin default into the call
            // verbatim, so this becomes the argument expression itself.
            node.Parameters[pin] = LaneExpression(lane);
            ++lane;
        }
    }

    /* Past the budget a value keeps its literal and therefore still costs a
       recompile when edited — the same graph then behaves two different ways
       depending on which node you drag. Say so once rather than let the user
       discover it as flakiness. */
    if (preview.UnboundScalars > 0)
    {
        Logger::Log::Warning(
            "MaterialGraphPreview: {} constant value(s) past the {}-lane budget keep literals; "
            "editing those still recompiles the preview.",
            preview.UnboundScalars, kUserScalarLanes);
    }

    return preview;
}

void ApplyPreviewLaneValues(const MaterialGraphPreviewModel& preview, MaterialDocument& doc)
{
    for (const auto& [key, value] : preview.LaneValues)
        doc.properties[key] = value;
}

} // namespace Editor
} // namespace GameEngine
