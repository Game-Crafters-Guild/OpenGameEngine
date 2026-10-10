#include "Graph/GraphNodeRegistry.h"
#include "Graph/GraphTypeRegistry.h"
#include "Graph/GraphValue.h"
#include "Graph/GraphNodeIconStems.h"

namespace GameEngine {

namespace {

NodePortTemplate PoseOut()
{
    return {"poseOut", Graph::PortDirection::Out, "pose", "Pose"};
}

NodePortTemplate PoseIn(const char* id, const char* name)
{
    return {id, Graph::PortDirection::In, "pose", name};
}

NodePortTemplate TransitionOut()
{
    return {"out", Graph::PortDirection::Out, "transition", "Out"};
}

NodePortTemplate TransitionIn()
{
    return {"in", Graph::PortDirection::In, "transition", "In"};
}
} // namespace

namespace
{

constexpr NodeIconStemEntry kAnimationNodeIconStems[] = {
    {"AdditiveBlend", "blend"},
    {"AnyState", "state"},
    {"Blend2", "blend"},
    {"BlendSpace1D", "blendspace"},
    {"BlendSpace2D", "blendspace"},
    {"ClipPlayer", "clip"},
    {"Entry", "entry"},
    {"FABRIK", "ik"},
    {"LayeredBlend", "blend"},
    {"LookAt", "ik"},
    {"MontageSlot", "slot"},
    {"OutputPose", "output"},
    {"State", "state"},
    {"StateMachine", "statemachine"},
    {"TwoBoneIK", "ik"},
};

constexpr NodeIconStemEntry kAnimationCategoryIconStems[] = {
    {"IK", "category-transform"},
    {"Pose", "category-pose"},
    {"State Machine", "category-state"},
};

} // namespace

void GraphNodeRegistry::RegisterAnimationTypes()
{
    Graph::GraphTypeRegistry::Get().Register(
        {std::string(Graph::kKindIdAnimation), "Animation", ".animgraph"});

    Register(Graph::kKindIdAnimation,
             {"ClipPlayer", "Clip Player", "Pose", {PoseOut()},
              {{"clipGuid", Graph::GraphValue::FromGuid("")},
               {"speed", Graph::GraphValue(1.0f)},
               {"looping", Graph::GraphValue(true)}}});
    Register(Graph::kKindIdAnimation,
             {"Blend2", "Blend", "Pose",
              {PoseIn("a", "A"), PoseIn("b", "B"), PoseOut()},
              {{"weight", Graph::GraphValue(0.5f)}}});
    Register(Graph::kKindIdAnimation,
             {"BlendSpace1D", "Blend Space 1D", "Pose", {PoseOut()},
              {{"parameter", Graph::GraphValue("Speed")}}});
    Register(Graph::kKindIdAnimation,
             {"BlendSpace2D", "Blend Space 2D", "Pose", {PoseOut()},
              {{"parameterX", Graph::GraphValue("Speed")},
               {"parameterY", Graph::GraphValue("Direction")}}});
    Register(Graph::kKindIdAnimation,
             {"StateMachine", "State Machine", "Pose", {PoseOut()}, {}});
    Register(Graph::kKindIdAnimation,
             {"State", "State", "State", {TransitionIn(), TransitionOut()},
              {{"title", Graph::GraphValue("")}}});
    Register(Graph::kKindIdAnimation,
             {"Entry", "Entry", "State", {TransitionOut()},
              {{"title", Graph::GraphValue("Entry")}}});
    Register(Graph::kKindIdAnimation,
             {"LayeredBlend", "Layered Blend", "Pose",
              {PoseIn("base", "Base"), PoseIn("layer", "Layer"), PoseOut()},
              {{"weight", Graph::GraphValue(1.0f)}}});
    Register(Graph::kKindIdAnimation,
             {"AdditiveBlend", "Additive Blend", "Pose",
              {PoseIn("base", "Base"), PoseIn("additive", "Additive"), PoseOut()},
              {{"weight", Graph::GraphValue(1.0f)}}});
    Register(Graph::kKindIdAnimation,
             {"TwoBoneIK", "Two Bone IK", "IK",
              {PoseIn("pose", "Pose"), PoseOut()}, {}});
    Register(Graph::kKindIdAnimation,
             {"FABRIK", "FABRIK", "IK", {PoseIn("pose", "Pose"), PoseOut()}, {}});
    Register(Graph::kKindIdAnimation,
             {"LookAt", "Look At", "IK", {PoseIn("pose", "Pose"), PoseOut()}, {}});
    Register(Graph::kKindIdAnimation,
             {"MontageSlot", "Montage Slot", "Pose", {PoseOut()},
              {{"slot", Graph::GraphValue("Default")}}});
    Register(Graph::kKindIdAnimation,
             {"OutputPose", "Output Pose", "Pose", {PoseIn("pose", "Pose")}, {}});

    for (auto& [typeId, meta] : m_ByKindId[std::string(Graph::kKindIdAnimation)])
        meta.IconStem = std::string(FindIconStem(kAnimationNodeIconStems, typeId));
    for (const NodeIconStemEntry& entry : kAnimationCategoryIconStems)
        RegisterCategoryIcon(Graph::kKindIdAnimation, entry.TypeId, entry.Stem);
}

} // namespace GameEngine
