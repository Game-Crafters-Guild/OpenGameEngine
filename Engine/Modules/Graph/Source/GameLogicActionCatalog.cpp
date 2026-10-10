#include "Graph/GameLogicActionCatalog.h"

#include <algorithm>
#include <utility>

namespace GameEngine
{

namespace
{

Graph::Port In(std::string id, std::string type, std::string display)
{
    return {std::move(id), Graph::PortDirection::In, std::move(type), std::move(display)};
}

Graph::Port Out(std::string id, std::string type, std::string display)
{
    return {std::move(id), Graph::PortDirection::Out, std::move(type), std::move(display)};
}

GameLogicActionParameter Param(std::string id, std::string value)
{
    return {std::move(id), std::move(value)};
}

GameLogicActionParameter EnumParam(std::string id, std::string value,
                                   Graph::NodeParamOptions options)
{
    return {std::move(id), std::move(value), std::move(options)};
}

/* Option values match the runtime vocabularies exactly:
   CompareFloat/CompareInt (GameLogicRuntimeContext::CompareFloat/CompareInt),
   Bool/Float/Int/Vector3 operators (the matching *Operator methods), and
   Transition mode (GraphFsmRuntime::ParseConditionKind). */
Graph::NodeParamOptions ComparisonOptions()
{
    return {{"greater", "Greater"},
            {"greater_or_equal", "Greater or Equal"},
            {"less", "Less"},
            {"less_or_equal", "Less or Equal"},
            {"equal", "Equal"},
            {"not_equal", "Not Equal"}};
}

Graph::NodeParamOptions BoolOperationOptions()
{
    return {{"and", "And"},
            {"or", "Or"},
            {"xor", "Xor"},
            {"equal", "Equal"},
            {"not_equal", "Not Equal"}};
}

Graph::NodeParamOptions ArithmeticOptions(bool withModulo = false)
{
    Graph::NodeParamOptions options = {{"add", "Add"},
                                       {"subtract", "Subtract"},
                                       {"multiply", "Multiply"},
                                       {"divide", "Divide"},
                                       {"min", "Min"},
                                       {"max", "Max"}};
    if (withModulo)
        options.insert(options.begin() + 4, {"modulo", "Modulo"});
    return options;
}

Graph::NodeParamOptions TransitionModeOptions()
{
    return {{"always", "Always"},
            {"timer", "Timer"},
            {"event", "Event"},
            {"bool", "Bool"}};
}

std::vector<Graph::Port> FlowPorts()
{
    return {In("in", "flow", "In"), Out("out", "flow", "Out")};
}

std::vector<Graph::Port> FlowPortsWith(std::initializer_list<Graph::Port> ports)
{
    std::vector<Graph::Port> out = {In("in", "flow", "In")};
    out.insert(out.end(), ports.begin(), ports.end());
    out.push_back(Out("out", "flow", "Out"));
    return out;
}

const std::vector<GameLogicActionSpec>& BuildCatalog()
{
    static const std::vector<GameLogicActionSpec> catalog = {
        {"Entry", "Entry", "Flow", {Out("out", "flow", "Out")}, {}},
        {"Sequence", "Sequence", "Flow",
         {In("in", "flow", "In"), Out("out0", "flow", "Out 1"), Out("out1", "flow", "Out 2"), Out("out2", "flow", "Out 3")}, {}},
        {"Branch", "Branch", "Flow",
         {In("in", "flow", "In"), In("condition", "bool", "Condition"), Out("true", "flow", "True"), Out("false", "flow", "False")},
         {Param("condition", "false")}},
        {"Delay", "Delay", "Flow", FlowPortsWith({In("seconds", "float", "Seconds")}), {Param("seconds", "1")}},
        {"SendEvent", "Send Event", "Flow", FlowPortsWith({In("eventName", "string", "Event")}), {Param("eventName", "Event")}},
        {"Finish", "Finish", "Flow", {In("in", "flow", "In")}, {}},

        {"State", "State", "State Machine", FlowPorts(),
         {Param("playSound", ""), Param("playRandomSounds", "")}},
        {"AnyState", "Any State", "State Machine", {Out("out", "flow", "Out")}, {}},
        {"Transition", "Transition", "State Machine",
         {In("in", "flow", "In"), In("condition", "bool", "Condition"), Out("out", "flow", "Out")},
         {EnumParam("mode", "always", TransitionModeOptions()), Param("seconds", "0.5"), Param("event", ""),
          Param("boolVar", ""), Param("boolValue", "true")}},

        {"SetVariable", "Set Variable", "Variables", FlowPortsWith({In("value", "any", "Value")}),
         {Param("variableName", "playerScore"), Param("value", "0")}},
        {"GetVariable", "Get Variable", "Variables", {Out("out", "any", "Value")}, {Param("variableName", "playerScore")}},
        {"CompareFloat", "Compare Float", "Variables",
         {In("in", "flow", "In"), In("a", "float", "A"), In("b", "float", "B"), Out("true", "flow", "True"), Out("false", "flow", "False")},
         {Param("a", "0"), Param("b", "0"), EnumParam("comparison", "greater", ComparisonOptions())}},
        {"CompareBool", "Compare Bool", "Variables",
         {In("in", "flow", "In"), In("value", "bool", "Value"), Out("true", "flow", "True"), Out("false", "flow", "False")},
         {Param("value", "false"), Param("expected", "true")}},

        {"BoolOperator", "Bool Operator", "Logic",
         {In("a", "bool", "A"), In("b", "bool", "B"), Out("result", "bool", "Result")},
         {Param("a", "false"), Param("b", "false"), EnumParam("operation", "and", BoolOperationOptions())}},
        {"BoolNot", "Bool Not", "Logic", {In("value", "bool", "Value"), Out("result", "bool", "Result")},
         {Param("value", "false")}},
        {"CompareInt", "Compare Int", "Logic",
         {In("in", "flow", "In"), In("a", "int", "A"), In("b", "int", "B"), Out("true", "flow", "True"), Out("false", "flow", "False")},
         {Param("a", "0"), Param("b", "0"), EnumParam("comparison", "equal", ComparisonOptions())}},

        {"FloatOperator", "Float Operator", "Math",
         {In("a", "float", "A"), In("b", "float", "B"), Out("result", "float", "Result")},
         {Param("a", "0"), Param("b", "0"), EnumParam("operation", "add", ArithmeticOptions())}},
        {"FloatClamp", "Float Clamp", "Math",
         {In("value", "float", "Value"), In("min", "float", "Min"), In("max", "float", "Max"), Out("result", "float", "Result")},
         {Param("value", "0"), Param("min", "0"), Param("max", "1")}},
        {"FloatLerp", "Float Lerp", "Math",
         {In("a", "float", "A"), In("b", "float", "B"), In("t", "float", "T"), Out("result", "float", "Result")},
         {Param("a", "0"), Param("b", "1"), Param("t", "0.5")}},
        {"FloatAbs", "Float Abs", "Math", {In("value", "float", "Value"), Out("result", "float", "Result")},
         {Param("value", "0")}},
        {"FloatRound", "Float Round", "Math", {In("value", "float", "Value"), Out("result", "float", "Result")},
         {Param("value", "0")}},
        {"FloatSign", "Float Sign", "Math", {In("value", "float", "Value"), Out("result", "float", "Result")},
         {Param("value", "0")}},
        {"RandomFloat", "Random Float", "Math",
         {In("min", "float", "Min"), In("max", "float", "Max"), Out("result", "float", "Result")},
         {Param("min", "0"), Param("max", "1")}},
        {"IntOperator", "Int Operator", "Math",
         {In("a", "int", "A"), In("b", "int", "B"), Out("result", "int", "Result")},
         {Param("a", "0"), Param("b", "0"), EnumParam("operation", "add", ArithmeticOptions(/*withModulo=*/true))}},
        {"RandomInt", "Random Int", "Math",
         {In("min", "int", "Min"), In("max", "int", "Max"), Out("result", "int", "Result")},
         {Param("min", "0"), Param("max", "10")}},

        {"MakeVector3", "Make Vector3", "Vector3",
         {In("x", "float", "X"), In("y", "float", "Y"), In("z", "float", "Z"), Out("vector", "float3", "Vector")},
         {Param("x", "0"), Param("y", "0"), Param("z", "0")}},
        {"GetVector3XYZ", "Get Vector3 XYZ", "Vector3",
         {In("vector", "float3", "Vector"), Out("x", "float", "X"), Out("y", "float", "Y"), Out("z", "float", "Z")},
         {Param("vector", "0, 0, 0")}},
        {"Vector3Operator", "Vector3 Operator", "Vector3",
         {In("a", "float3", "A"), In("b", "float3", "B"), Out("result", "float3", "Result")},
         {Param("a", "0, 0, 0"), Param("b", "0, 0, 0"), EnumParam("operation", "add", ArithmeticOptions())}},
        {"Vector3Scale", "Vector3 Scale", "Vector3",
         {In("vector", "float3", "Vector"), In("scale", "float", "Scale"), Out("result", "float3", "Result")},
         {Param("vector", "0, 0, 0"), Param("scale", "1")}},
        {"Vector3Normalize", "Vector3 Normalize", "Vector3",
         {In("vector", "float3", "Vector"), Out("result", "float3", "Result")},
         {Param("vector", "0, 0, 0")}},
        {"Vector3Magnitude", "Vector3 Magnitude", "Vector3",
         {In("vector", "float3", "Vector"), Out("result", "float", "Result")},
         {Param("vector", "0, 0, 0")}},
        {"Vector3Distance", "Vector3 Distance", "Vector3",
         {In("a", "float3", "A"), In("b", "float3", "B"), Out("result", "float", "Result")},
         {Param("a", "0, 0, 0"), Param("b", "0, 0, 0")}},
        {"Vector3Dot", "Vector3 Dot", "Vector3",
         {In("a", "float3", "A"), In("b", "float3", "B"), Out("result", "float", "Result")},
         {Param("a", "0, 0, 0"), Param("b", "0, 0, 0")}},
        {"Vector3Cross", "Vector3 Cross", "Vector3",
         {In("a", "float3", "A"), In("b", "float3", "B"), Out("result", "float3", "Result")},
         {Param("a", "0, 0, 0"), Param("b", "0, 0, 0")}},
        {"Vector3Lerp", "Vector3 Lerp", "Vector3",
         {In("a", "float3", "A"), In("b", "float3", "B"), In("t", "float", "T"), Out("result", "float3", "Result")},
         {Param("a", "0, 0, 0"), Param("b", "0, 0, 0"), Param("t", "0.5")}},

        {"GetTime", "Get Time", "Time", {Out("time", "float", "Time")}, {}},
        {"GetDeltaTime", "Get Delta Time", "Time", {Out("deltaTime", "float", "Delta Time")}, {}},
        {"SetTimeScale", "Set Time Scale", "Time", FlowPortsWith({In("scale", "float", "Scale")}), {Param("scale", "1")}},

        {"QuitApplication", "Quit Application", "Application", FlowPorts(), {}},

        {"SetSkyTimeOfDay", "Set Sky Time Of Day", "Sky",
         FlowPortsWith({In("hours", "float", "Hours"), In("animate", "bool", "Animate"), In("cycleSeconds", "float", "Cycle Seconds")}),
         {Param("hours", "12"), Param("animate", "false"), Param("cycleSeconds", "120")}},

        {"PlaySound", "Play Sound", "Audio",
         FlowPortsWith({In("clipGuid", "string", "Clip"), In("volume", "float", "Volume"), In("pitch", "float", "Pitch"),
                       In("loop", "bool", "Loop"), In("spatialized", "bool", "3D"), In("position", "float3", "Position")}),
         {Param("clipGuid", ""), Param("volume", "1"), Param("pitch", "1"), Param("loop", "false"),
          Param("spatialized", "false"), Param("position", "0, 0, 0"), Param("worldId", "0"), Param("bus", "2")}},
        {"StopAllSounds", "Stop All Sounds", "Audio", FlowPorts(), {}},
        {"SetBusVolume", "Set Bus Volume", "Audio", FlowPortsWith({In("bus", "int", "Bus"), In("volume", "float", "Volume")}),
         {Param("bus", "0"), Param("volume", "1")}},

        {"CreateEntity", "Create Entity", "Game Object", FlowPortsWith({In("name", "string", "Name")}), {Param("name", "Entity")}},
        {"DestroyEntity", "Destroy Entity", "Game Object", FlowPortsWith({In("entity", "entity", "Entity")}), {Param("entity", "self")}},
        {"SetEntityEnabled", "Set Entity Enabled", "Game Object", FlowPortsWith({In("entity", "entity", "Entity"), In("enabled", "bool", "Enabled")}),
         {Param("entity", "self"), Param("enabled", "true")}},
        {"FindEntityByName", "Find Entity By Name", "Game Object", {In("name", "string", "Name"), Out("entity", "entity", "Entity")},
         {Param("name", "Player")}},

        {"SetPosition", "Set Position", "Transform", FlowPortsWith({In("entity", "entity", "Entity"), In("position", "float3", "Position")}),
         {Param("entity", "self"), Param("position", "0, 0, 0")}},
        {"GetPosition", "Get Position", "Transform", {In("entity", "entity", "Entity"), Out("position", "float3", "Position")},
         {Param("entity", "self")}},
        {"Translate", "Translate", "Transform", FlowPortsWith({In("entity", "entity", "Entity"), In("delta", "float3", "Delta")}),
         {Param("entity", "self"), Param("delta", "0, 0, 0")}},
        {"Rotate", "Rotate", "Transform", FlowPortsWith({In("entity", "entity", "Entity"), In("euler", "float3", "Euler")}),
         {Param("entity", "self"), Param("euler", "0, 0, 0")}},
        {"LookAt", "Look At", "Transform", FlowPortsWith({In("entity", "entity", "Entity"), In("target", "float3", "Target")}),
         {Param("entity", "self"), Param("target", "0, 0, 0")}},

        {"AddForce", "Add Force", "Physics", FlowPortsWith({In("entity", "entity", "Entity"), In("force", "float3", "Force")}),
         {Param("entity", "self"), Param("force", "0, 10, 0")}},
        {"SetVelocity", "Set Velocity", "Physics", FlowPortsWith({In("entity", "entity", "Entity"), In("velocity", "float3", "Velocity")}),
         {Param("entity", "self"), Param("velocity", "0, 0, 0")}},
        {"Raycast", "Raycast", "Physics",
         {In("in", "flow", "In"), In("origin", "float3", "Origin"), In("direction", "float3", "Direction"),
          In("distance", "float", "Distance"), Out("hit", "flow", "Hit"), Out("miss", "flow", "Miss")},
         {Param("origin", "0, 0, 0"), Param("direction", "0, 0, -1"), Param("distance", "100")}},
        {"CollisionEvent", "Collision Event", "Physics",
         {In("in", "flow", "In"), In("entity", "entity", "Entity"), In("tag", "string", "Tag"),
          Out("hit", "flow", "Hit"), Out("miss", "flow", "Miss")},
         {Param("entity", "self"), Param("phase", "enter"), Param("tag", "")}},
        {"TriggerEvent", "Trigger Event", "Physics",
         {In("in", "flow", "In"), In("entity", "entity", "Entity"), In("tag", "string", "Tag"),
          Out("entered", "flow", "Entered"), Out("miss", "flow", "Miss")},
         {Param("entity", "self"), Param("phase", "enter"), Param("tag", "")}},

        {"GetInputAction", "Get Input Action", "Input", {In("action", "string", "Action"), Out("pressed", "bool", "Pressed")},
         {Param("action", "Jump")}},
        {"GetInputAxis", "Get Input Axis", "Input", {In("axis", "string", "Axis"), Out("value", "float", "Value")},
         {Param("axis", "Horizontal")}},

        {"LoadScene", "Load Scene", "Scene", FlowPortsWith({In("scene", "string", "Scene")}), {Param("scene", "Main")}},
        {"ReloadScene", "Reload Scene", "Scene", FlowPorts(), {}},

        {"ShowPanel", "Show Panel", "UI", FlowPortsWith({In("panel", "string", "Panel")}), {Param("panel", "HUD")}},
        {"HidePanel", "Hide Panel", "UI", FlowPortsWith({In("panel", "string", "Panel")}), {Param("panel", "HUD")}},
        {"SetText", "Set Text", "UI", FlowPortsWith({In("element", "string", "Element"), In("text", "string", "Text")}),
         {Param("element", "Label"), Param("text", "")}},

        {"SetLightIntensity", "Set Light Intensity", "Rendering", FlowPortsWith({In("entity", "entity", "Light"), In("intensity", "float", "Intensity")}),
         {Param("entity", "self"), Param("intensity", "1")}},
        {"SetCameraActive", "Set Camera Active", "Rendering", FlowPortsWith({In("entity", "entity", "Camera"), In("active", "bool", "Active")}),
         {Param("entity", "self"), Param("active", "true")}},
        {"ThirdPersonCameraFollow", "Third Person Camera Follow", "Rendering",
         FlowPortsWith({In("camera", "entity", "Camera"),
                       In("target", "entity", "Target"),
                       In("offset", "float3", "Offset"),
                       In("lookOffset", "float3", "Look Offset"),
                       In("positionSmoothing", "float", "Position Smoothing"),
                       In("rotationSmoothing", "float", "Rotation Smoothing")}),
         {Param("camera", "Main Camera"),
          Param("target", "self"),
          Param("offset", "0, 2.6, -7"),
          Param("lookOffset", "0, 1.25, 0"),
          Param("positionSmoothing", "8"),
          Param("rotationSmoothing", "12")}},

        {"MoveTo", "Move To", "Navigation", FlowPortsWith({In("entity", "entity", "Entity"), In("target", "float3", "Target")}),
         {Param("entity", "self"), Param("target", "0, 0, 0")}},
        {"StopMove", "Stop Move", "Navigation", FlowPortsWith({In("entity", "entity", "Entity")}), {Param("entity", "self")}},

        {"Log", "Log", "Debug", FlowPortsWith({In("message", "string", "Message")}), {Param("message", "Hello")}},
        {"DrawDebugLine", "Draw Debug Line", "Debug", FlowPortsWith({In("from", "float3", "From"), In("to", "float3", "To")}),
         {Param("from", "0, 0, 0"), Param("to", "0, 1, 0"), Param("duration", "0")}},
    };
    return catalog;
}

} // namespace

const std::vector<GameLogicActionSpec>& GetGameLogicActionCatalog()
{
    return BuildCatalog();
}

const GameLogicActionSpec* FindGameLogicActionSpec(const std::string& typeId)
{
    const auto& catalog = GetGameLogicActionCatalog();
    const auto it = std::find_if(catalog.begin(), catalog.end(),
                                 [&typeId](const GameLogicActionSpec& spec) { return spec.TypeId == typeId; });
    return it == catalog.end() ? nullptr : &*it;
}

} // namespace GameEngine
