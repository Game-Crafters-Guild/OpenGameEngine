#include "Graph/GameLogicGraphCompiler.h"

#include "Graph/GameLogicActionCatalog.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace GameEngine
{

namespace
{

std::string CppString(const std::string& value)
{
    std::string out = "\"";
    for (char c : value)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    out += "\"";
    return out;
}

bool IsRetiredAnimatorAction(const std::string& typeId)
{
    return typeId == "PlayAnimation"
        || typeId == "SetAnimatorBool"
        || typeId == "SetAnimatorFloat"
        || typeId == "SetAnimatorTrigger";
}

std::string SanitizeIdentifier(std::string value)
{
    if (value.empty())
        value = "ExecuteGameLogicGraph";
    for (char& c : value)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            c = '_';
    }
    if (std::isdigit(static_cast<unsigned char>(value.front())))
        value.insert(value.begin(), '_');
    return value;
}

std::string TrimCopy(const std::string& value)
{
    auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; });
    auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c) != 0; }).base();
    if (begin >= end)
        return {};
    return std::string(begin, end);
}

bool IsTrueText(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    value = TrimCopy(value);
    return value == "true" || value == "1" || value == "yes" || value == "on";
}

std::string NumberOrFallback(const std::string& value, const char* fallback)
{
    const std::string trimmed = TrimCopy(value);
    if (trimmed.empty())
        return fallback;
    char* end = nullptr;
    (void)std::strtof(trimmed.c_str(), &end);
    return (end && *end == '\0') ? trimmed : fallback;
}

std::string FloatLiteral(const std::string& value, const char* fallback = "0")
{
    std::string number = NumberOrFallback(value, fallback);
    if (number.find('.') == std::string::npos &&
        number.find('e') == std::string::npos &&
        number.find('E') == std::string::npos)
    {
        number += ".0";
    }
    return number + "f";
}

std::string IntOrFallback(const std::string& value, const char* fallback)
{
    const std::string trimmed = TrimCopy(value);
    if (trimmed.empty())
        return fallback;
    char* end = nullptr;
    (void)std::strtol(trimmed.c_str(), &end, 10);
    return (end && *end == '\0') ? trimmed : fallback;
}

std::string Vec3Literal(std::string value)
{
    for (char& c : value)
    {
        if (c == ',' || c == ';' || c == '(' || c == ')')
            c = ' ';
    }

    std::istringstream in(value);
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    (void)(in >> x);
    (void)(in >> y);
    (void)(in >> z);

    std::ostringstream out;
    out << "GameLogicVec3{" << FloatLiteral(std::to_string(x)) << ", "
        << FloatLiteral(std::to_string(y)) << ", "
        << FloatLiteral(std::to_string(z)) << "}";
    return out.str();
}

bool IsValueOnlyNode(const std::string& typeId)
{
    static const std::unordered_set<std::string> kValueNodes = {
        "GetVariable", "FindEntityByName", "GetPosition", "GetInputAction", "GetInputAxis",
        "BoolOperator", "BoolNot",
        "FloatOperator", "FloatClamp", "FloatLerp", "FloatAbs", "FloatRound", "FloatSign", "RandomFloat",
        "IntOperator", "RandomInt",
        "MakeVector3", "GetVector3XYZ", "Vector3Operator", "Vector3Scale", "Vector3Normalize",
        "Vector3Magnitude", "Vector3Distance", "Vector3Dot", "Vector3Cross", "Vector3Lerp",
        "GetTime", "GetDeltaTime",
    };
    return kValueNodes.count(typeId) != 0;
}

class FlowCompiler
{
public:
    FlowCompiler(const Graph::Model& model, GameLogicGraphCompileResult& result)
        : m_Model(model), m_Result(result)
    {
    }

    std::string Build(const std::string& functionName)
    {
        const Graph::Node* entry = nullptr;
        for (const Graph::Node& node : m_Model.Nodes)
        {
            if (node.TypeId == "Entry")
            {
                entry = &node;
                break;
            }
        }

        if (!entry)
        {
            m_Result.errors.push_back("Game logic graph requires an Entry node.");
            return {};
        }

        m_Out << "#include \"Graph/GameLogicRuntime.h\"\n\n";
        EmitCustomActionForwardDeclarations();
        m_Out << "void " << SanitizeIdentifier(functionName) << "(GameEngine::GameLogicRuntimeContext& ctx)\n";
        m_Out << "{\n";
        m_Out << "    using namespace GameEngine;\n";
        m_Out << "    (void)ctx;\n";
        EmitFromOutput(*entry, "out");
        m_Out << "}\n";
        return m_Out.str();
    }

private:
    std::string Param(const Graph::Node& node, const std::string& key, const std::string& fallback = {}) const
    {
        if (auto it = node.Parameters.find(key); it != node.Parameters.end())
            return it->second.ToString();
        if (const GameLogicActionSpec* spec = FindGameLogicActionSpec(node.TypeId))
        {
            for (const GameLogicActionParameter& param : spec->Parameters)
            {
                if (param.Id == key)
                    return param.DefaultValue;
            }
        }
        return fallback;
    }

    const Graph::Edge* IncomingLink(const Graph::Node& node, const std::string& portId) const
    {
        for (const Graph::Edge& link : m_Model.Links)
        {
            if (link.TargetNodeId == node.Id && link.TargetPortId == portId)
                return &link;
        }
        return nullptr;
    }

    const Graph::Edge* OutgoingLink(const Graph::Node& node, const std::string& portId) const
    {
        for (const Graph::Edge& link : m_Model.Links)
        {
            if (link.SourceNodeId == node.Id && link.SourcePortId == portId)
                return &link;
        }
        return nullptr;
    }

    std::string ValueExpr(const Graph::Node& node, const std::string& portId, const std::string& type,
                          const std::string& fallbackKey) const
    {
        if (const Graph::Edge* link = IncomingLink(node, portId))
        {
            if (const Graph::Node* source = m_Model.FindNode(link->SourceNodeId))
                return SourceValueExpr(*source, link->SourcePortId, type);
        }
        return LiteralForType(type, Param(node, fallbackKey.empty() ? portId : fallbackKey));
    }

    std::string SourceValueExpr(const Graph::Node& source, const std::string& sourcePortId, const std::string& targetType) const
    {
        if (source.TypeId == "GetVariable")
        {
            const std::string name = Param(source, "variableName", "value");
            if (targetType == "bool")
                return "ctx.GetBoolVariable(" + CppString(name) + ")";
            if (targetType == "float")
                return "ctx.GetFloatVariable(" + CppString(name) + ")";
            return "ctx.GetStringVariable(" + CppString(name) + ")";
        }
        if (source.TypeId == "FindEntityByName")
            return "ctx.FindEntityByName(" + CppString(Param(source, "name", "Entity")) + ")";
        if (source.TypeId == "GetPosition")
            return "ctx.GetPosition(" + EntityExpr(source, "entity") + ")";
        if (source.TypeId == "GetInputAction")
            return "ctx.GetInputAction(" + CppString(Param(source, "action", "Jump")) + ")";
        if (source.TypeId == "GetInputAxis")
            return "ctx.GetInputAxis(" + CppString(Param(source, "axis", "Horizontal")) + ")";
        if (source.TypeId == "BoolOperator")
            return "ctx.BoolOperator(" + ValueExpr(source, "a", "bool", "a") + ", " +
                   ValueExpr(source, "b", "bool", "b") + ", " + CppString(Param(source, "operation", "and")) + ")";
        if (source.TypeId == "BoolNot")
            return "ctx.BoolNot(" + ValueExpr(source, "value", "bool", "value") + ")";
        if (source.TypeId == "FloatOperator")
            return "ctx.FloatOperator(" + ValueExpr(source, "a", "float", "a") + ", " +
                   ValueExpr(source, "b", "float", "b") + ", " + CppString(Param(source, "operation", "add")) + ")";
        if (source.TypeId == "FloatClamp")
            return "ctx.FloatClamp(" + ValueExpr(source, "value", "float", "value") + ", " +
                   ValueExpr(source, "min", "float", "min") + ", " + ValueExpr(source, "max", "float", "max") + ")";
        if (source.TypeId == "FloatLerp")
            return "ctx.FloatLerp(" + ValueExpr(source, "a", "float", "a") + ", " +
                   ValueExpr(source, "b", "float", "b") + ", " + ValueExpr(source, "t", "float", "t") + ")";
        if (source.TypeId == "FloatAbs")
            return "ctx.FloatAbs(" + ValueExpr(source, "value", "float", "value") + ")";
        if (source.TypeId == "FloatRound")
            return "ctx.FloatRound(" + ValueExpr(source, "value", "float", "value") + ")";
        if (source.TypeId == "FloatSign")
            return "ctx.FloatSign(" + ValueExpr(source, "value", "float", "value") + ")";
        if (source.TypeId == "RandomFloat")
            return "ctx.RandomFloat(" + ValueExpr(source, "min", "float", "min") + ", " +
                   ValueExpr(source, "max", "float", "max") + ")";
        if (source.TypeId == "IntOperator")
            return "ctx.IntOperator(" + ValueExpr(source, "a", "int", "a") + ", " +
                   ValueExpr(source, "b", "int", "b") + ", " + CppString(Param(source, "operation", "add")) + ")";
        if (source.TypeId == "RandomInt")
            return "ctx.RandomInt(" + ValueExpr(source, "min", "int", "min") + ", " +
                   ValueExpr(source, "max", "int", "max") + ")";
        if (source.TypeId == "MakeVector3")
            return "ctx.MakeVector3(" + ValueExpr(source, "x", "float", "x") + ", " +
                   ValueExpr(source, "y", "float", "y") + ", " + ValueExpr(source, "z", "float", "z") + ")";
        if (source.TypeId == "GetVector3XYZ")
        {
            const std::string vectorExpr = ValueExpr(source, "vector", "float3", "vector");
            if (sourcePortId == "x")
                return "(" + vectorExpr + ").X";
            if (sourcePortId == "y")
                return "(" + vectorExpr + ").Y";
            return "(" + vectorExpr + ").Z";
        }
        if (source.TypeId == "Vector3Operator")
            return "ctx.Vector3Operator(" + ValueExpr(source, "a", "float3", "a") + ", " +
                   ValueExpr(source, "b", "float3", "b") + ", " + CppString(Param(source, "operation", "add")) + ")";
        if (source.TypeId == "Vector3Scale")
            return "ctx.Vector3Scale(" + ValueExpr(source, "vector", "float3", "vector") + ", " +
                   ValueExpr(source, "scale", "float", "scale") + ")";
        if (source.TypeId == "Vector3Normalize")
            return "ctx.Vector3Normalize(" + ValueExpr(source, "vector", "float3", "vector") + ")";
        if (source.TypeId == "Vector3Magnitude")
            return "ctx.Vector3Magnitude(" + ValueExpr(source, "vector", "float3", "vector") + ")";
        if (source.TypeId == "Vector3Distance")
            return "ctx.Vector3Distance(" + ValueExpr(source, "a", "float3", "a") + ", " +
                   ValueExpr(source, "b", "float3", "b") + ")";
        if (source.TypeId == "Vector3Dot")
            return "ctx.Vector3Dot(" + ValueExpr(source, "a", "float3", "a") + ", " +
                   ValueExpr(source, "b", "float3", "b") + ")";
        if (source.TypeId == "Vector3Cross")
            return "ctx.Vector3Cross(" + ValueExpr(source, "a", "float3", "a") + ", " +
                   ValueExpr(source, "b", "float3", "b") + ")";
        if (source.TypeId == "Vector3Lerp")
            return "ctx.Vector3Lerp(" + ValueExpr(source, "a", "float3", "a") + ", " +
                   ValueExpr(source, "b", "float3", "b") + ", " + ValueExpr(source, "t", "float", "t") + ")";
        if (source.TypeId == "GetTime")
            return "ctx.GetTime()";
        if (source.TypeId == "GetDeltaTime")
            return "ctx.GetDeltaTime()";
        return LiteralForType(targetType, Param(source, sourcePortId));
    }

    std::string LiteralForType(const std::string& type, const std::string& value) const
    {
        if (type == "bool")
            return IsTrueText(value) ? "true" : "false";
        if (type == "float")
            return FloatLiteral(value);
        if (type == "int")
            return IntOrFallback(value, "0");
        if (type == "float3")
            return Vec3Literal(value);
        if (type == "entity")
        {
            if (TrimCopy(value).empty() || value == "self")
                return "ctx.Self()";
            return "ctx.FindEntityByName(" + CppString(value) + ")";
        }
        return CppString(value);
    }

    std::string EntityExpr(const Graph::Node& node, const std::string& portId) const
    {
        return ValueExpr(node, portId, "entity", portId);
    }

    void EmitCustomActionForwardDeclarations()
    {
        std::vector<std::string> customTypes;
        for (const Graph::Node& node : m_Model.Nodes)
        {
            if (FindGameLogicActionSpec(node.TypeId)
                || node.TypeId == "Entry"
                || IsRetiredAnimatorAction(node.TypeId))
                continue;
            const std::string name = SanitizeIdentifier(node.TypeId);
            if (std::find(customTypes.begin(), customTypes.end(), name) == customTypes.end())
                customTypes.push_back(name);
        }

        if (customTypes.empty())
            return;

        m_Out << "namespace GameEngine::GameGraphActions\n";
        m_Out << "{\n";
        for (const std::string& type : customTypes)
            m_Out << "void " << type << "(GameLogicRuntimeContext& ctx);\n";
        m_Out << "} // namespace GameEngine::GameGraphActions\n\n";
    }

    void EmitLine(const std::string& text)
    {
        m_Out << "    " << text << "\n";
    }

    void EmitFromOutput(const Graph::Node& node, const std::string& portId)
    {
        if (const Graph::Edge* link = OutgoingLink(node, portId))
        {
            if (const Graph::Node* target = m_Model.FindNode(link->TargetNodeId))
                EmitNode(*target);
        }
    }

    void EmitSequenceOutputs(const Graph::Node& node)
    {
        std::vector<std::pair<int, std::string>> outputs;
        for (const Graph::Port& port : node.Ports)
        {
            if (port.Direction != Graph::PortDirection::Out || port.Id.rfind("out", 0) != 0)
                continue;

            const std::string suffix = port.Id.substr(3);
            if (suffix.empty() || std::any_of(suffix.begin(), suffix.end(), [](unsigned char c) { return !std::isdigit(c); }))
                continue;

            outputs.emplace_back(std::atoi(suffix.c_str()), port.Id);
        }

        std::sort(outputs.begin(), outputs.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first)
                return a.first < b.first;
            return a.second < b.second;
        });

        for (const auto& [_, portId] : outputs)
            EmitFromOutput(node, portId);
    }

    void EmitNode(const Graph::Node& node)
    {
        if (m_ActiveNodes.count(node.Id) != 0)
        {
            EmitLine("ctx.TraceAction(\"cycle:" + node.TypeId + "\");");
            return;
        }

        m_ActiveNodes.insert(node.Id);
        if (node.TypeId == "Sequence")
        {
            EmitSequenceOutputs(node);
        }
        else if (node.TypeId == "Branch")
        {
            EmitLine("if (" + ValueExpr(node, "condition", "bool", "condition") + ")");
            EmitLine("{");
            EmitFromOutput(node, "true");
            EmitLine("}");
            EmitLine("else");
            EmitLine("{");
            EmitFromOutput(node, "false");
            EmitLine("}");
        }
        else if (node.TypeId == "CompareFloat")
        {
            EmitLine("if (ctx.CompareFloat(" + ValueExpr(node, "a", "float", "a") + ", " +
                     ValueExpr(node, "b", "float", "b") + ", " + CppString(Param(node, "comparison", "greater")) + "))");
            EmitLine("{");
            EmitFromOutput(node, "true");
            EmitLine("}");
            EmitLine("else");
            EmitLine("{");
            EmitFromOutput(node, "false");
            EmitLine("}");
        }
        else if (node.TypeId == "CompareBool")
        {
            EmitLine("if (" + ValueExpr(node, "value", "bool", "value") + " == " + LiteralForType("bool", Param(node, "expected", "true")) + ")");
            EmitLine("{");
            EmitFromOutput(node, "true");
            EmitLine("}");
            EmitLine("else");
            EmitLine("{");
            EmitFromOutput(node, "false");
            EmitLine("}");
        }
        else if (node.TypeId == "CompareInt")
        {
            EmitLine("if (ctx.CompareInt(" + ValueExpr(node, "a", "int", "a") + ", " +
                     ValueExpr(node, "b", "int", "b") + ", " + CppString(Param(node, "comparison", "equal")) + "))");
            EmitLine("{");
            EmitFromOutput(node, "true");
            EmitLine("}");
            EmitLine("else");
            EmitLine("{");
            EmitFromOutput(node, "false");
            EmitLine("}");
        }
        else if (node.TypeId == "Raycast")
        {
            EmitLine("if (ctx.Raycast(" + ValueExpr(node, "origin", "float3", "origin") + ", " +
                     ValueExpr(node, "direction", "float3", "direction") + ", " +
                     ValueExpr(node, "distance", "float", "distance") + "))");
            EmitLine("{");
            EmitFromOutput(node, "hit");
            EmitLine("}");
            EmitLine("else");
            EmitLine("{");
            EmitFromOutput(node, "miss");
            EmitLine("}");
        }
        else if (node.TypeId == "CollisionEvent")
        {
            EmitLine("if (ctx.CollisionEvent(" + EntityExpr(node, "entity") + ", " +
                     CppString(Param(node, "phase", "enter")) + ", " +
                     ValueExpr(node, "tag", "string", "tag") + "))");
            EmitLine("{");
            EmitFromOutput(node, "hit");
            EmitLine("}");
            EmitLine("else");
            EmitLine("{");
            EmitFromOutput(node, "miss");
            EmitLine("}");
        }
        else if (node.TypeId == "TriggerEvent")
        {
            EmitLine("if (ctx.TriggerEvent(" + EntityExpr(node, "entity") + ", " +
                     CppString(Param(node, "phase", "enter")) + ", " +
                     ValueExpr(node, "tag", "string", "tag") + "))");
            EmitLine("{");
            EmitFromOutput(node, "entered");
            EmitLine("}");
            EmitLine("else");
            EmitLine("{");
            EmitFromOutput(node, "miss");
            EmitLine("}");
        }
        else if (node.TypeId == "Finish")
        {
            EmitLine("return;");
        }
        else if (node.TypeId == "State" || node.TypeId == "AnyState" || node.TypeId == "Transition")
        {
            /* FSM structural nodes: GraphFsmRuntime interprets state machines; the compiled
               action script must not walk through them as imperative flow. */
        }
        else
        {
            EmitAction(node);
            EmitFromOutput(node, "out");
        }
        m_ActiveNodes.erase(node.Id);
    }

    void EmitAction(const Graph::Node& node)
    {
        if (node.TypeId == "Delay")
            EmitLine("ctx.Delay(" + ValueExpr(node, "seconds", "float", "seconds") + ");");
        else if (node.TypeId == "SendEvent")
            EmitLine("ctx.SendEvent(" + ValueExpr(node, "eventName", "string", "eventName") + ");");
        else if (node.TypeId == "SetVariable")
            EmitLine("ctx.SetVariable(" + CppString(Param(node, "variableName", "value")) + ", " + ValueExpr(node, "value", "string", "value") + ");");
        else if (node.TypeId == "PlaySound")
            EmitLine("ctx.PlaySound(" + ValueExpr(node, "clipGuid", "string", "clipGuid") + ", " +
                     ValueExpr(node, "volume", "float", "volume") + ", " +
                     ValueExpr(node, "pitch", "float", "pitch") + ", " +
                     ValueExpr(node, "loop", "bool", "loop") + ", " +
                     ValueExpr(node, "spatialized", "bool", "spatialized") + ", " +
                     ValueExpr(node, "position", "float3", "position") + ", " +
                     LiteralForType("int", Param(node, "worldId", "0")) + ", " +
                     LiteralForType("int", Param(node, "bus", "2")) + ");");
        else if (node.TypeId == "StopAllSounds")
            EmitLine("ctx.StopAllSounds();");
        else if (node.TypeId == "SetBusVolume")
            EmitLine("ctx.SetBusVolume(" + ValueExpr(node, "bus", "int", "bus") + ", " + ValueExpr(node, "volume", "float", "volume") + ");");
        else if (node.TypeId == "CreateEntity")
            EmitLine("(void)ctx.CreateEntity(" + ValueExpr(node, "name", "string", "name") + ");");
        else if (node.TypeId == "DestroyEntity")
            EmitLine("ctx.DestroyEntity(" + EntityExpr(node, "entity") + ");");
        else if (node.TypeId == "SetEntityEnabled")
            EmitLine("ctx.SetEntityEnabled(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "enabled", "bool", "enabled") + ");");
        else if (node.TypeId == "SetPosition")
            EmitLine("ctx.SetPosition(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "position", "float3", "position") + ");");
        else if (node.TypeId == "Translate")
            EmitLine("ctx.Translate(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "delta", "float3", "delta") + ");");
        else if (node.TypeId == "Rotate")
            EmitLine("ctx.Rotate(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "euler", "float3", "euler") + ");");
        else if (node.TypeId == "LookAt")
            EmitLine("ctx.LookAt(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "target", "float3", "target") + ");");
        else if (node.TypeId == "AddForce")
            EmitLine("ctx.AddForce(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "force", "float3", "force") + ");");
        else if (node.TypeId == "SetVelocity")
            EmitLine("ctx.SetVelocity(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "velocity", "float3", "velocity") + ");");
        else if (node.TypeId == "PlayAnimation")
            EmitLine("ctx.PlayAnimation(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "clipGuid", "string", "clipGuid") + ", " +
                     LiteralForType("bool", Param(node, "loop", "false")) + ");");
        else if (node.TypeId == "SetAnimatorBool")
            EmitLine("ctx.SetAnimatorBool(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "name", "string", "name") + ", " +
                     ValueExpr(node, "value", "bool", "value") + ");");
        else if (node.TypeId == "SetAnimatorFloat")
            EmitLine("ctx.SetAnimatorFloat(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "name", "string", "name") + ", " +
                     ValueExpr(node, "value", "float", "value") + ");");
        else if (node.TypeId == "SetAnimatorTrigger")
            EmitLine("ctx.SetAnimatorTrigger(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "name", "string", "name") + ");");
        else if (node.TypeId == "LoadScene")
            EmitLine("ctx.LoadScene(" + ValueExpr(node, "scene", "string", "scene") + ");");
        else if (node.TypeId == "ReloadScene")
            EmitLine("ctx.ReloadScene();");
        else if (node.TypeId == "SetTimeScale")
            EmitLine("ctx.SetTimeScale(" + ValueExpr(node, "scale", "float", "scale") + ");");
        else if (node.TypeId == "QuitApplication")
            EmitLine("ctx.QuitApplication();");
        else if (node.TypeId == "SetSkyTimeOfDay")
            EmitLine("ctx.SetSkyTimeOfDay(" + ValueExpr(node, "hours", "float", "hours") + ", " +
                     ValueExpr(node, "animate", "bool", "animate") + ", " +
                     ValueExpr(node, "cycleSeconds", "float", "cycleSeconds") + ");");
        else if (node.TypeId == "ShowPanel")
            EmitLine("ctx.ShowPanel(" + ValueExpr(node, "panel", "string", "panel") + ");");
        else if (node.TypeId == "HidePanel")
            EmitLine("ctx.HidePanel(" + ValueExpr(node, "panel", "string", "panel") + ");");
        else if (node.TypeId == "SetText")
            EmitLine("ctx.SetText(" + ValueExpr(node, "element", "string", "element") + ", " + ValueExpr(node, "text", "string", "text") + ");");
        else if (node.TypeId == "SetLightIntensity")
            EmitLine("ctx.SetLightIntensity(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "intensity", "float", "intensity") + ");");
        else if (node.TypeId == "SetCameraActive")
            EmitLine("ctx.SetCameraActive(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "active", "bool", "active") + ");");
        else if (node.TypeId == "ThirdPersonCameraFollow")
            EmitLine("ctx.ThirdPersonCameraFollow(" + EntityExpr(node, "camera") + ", " + EntityExpr(node, "target") + ", " +
                     ValueExpr(node, "offset", "float3", "offset") + ", " +
                     ValueExpr(node, "lookOffset", "float3", "lookOffset") + ", " +
                     ValueExpr(node, "positionSmoothing", "float", "positionSmoothing") + ", " +
                     ValueExpr(node, "rotationSmoothing", "float", "rotationSmoothing") + ");");
        else if (node.TypeId == "MoveTo")
            EmitLine("ctx.MoveTo(" + EntityExpr(node, "entity") + ", " + ValueExpr(node, "target", "float3", "target") + ");");
        else if (node.TypeId == "StopMove")
            EmitLine("ctx.StopMove(" + EntityExpr(node, "entity") + ");");
        else if (node.TypeId == "Log")
            EmitLine("ctx.Log(" + ValueExpr(node, "message", "string", "message") + ");");
        else if (node.TypeId == "DrawDebugLine")
            EmitLine("ctx.DrawDebugLine(" + ValueExpr(node, "from", "float3", "from") + ", " +
                     ValueExpr(node, "to", "float3", "to") + ", " + LiteralForType("float", Param(node, "duration", "0")) + ");");
        else if (!IsValueOnlyNode(node.TypeId))
            EmitLine("GameGraphActions::" + SanitizeIdentifier(node.TypeId) + "(ctx);");
    }

    const Graph::Model& m_Model;
    GameLogicGraphCompileResult& m_Result;
    std::ostringstream m_Out;
    std::unordered_set<std::string> m_ActiveNodes;
};

} // namespace

GameLogicGraphCompileResult GameLogicGraphCompiler::CompileToCpp(const Graph::Model& model,
                                                                 const std::string& functionName)
{
    GameLogicGraphCompileResult result{};
    if (model.KindId != Graph::kKindIdGameLogic)
    {
        result.errors.push_back("GameLogicGraphCompiler only accepts game_logic graphs.");
        return result;
    }

    FlowCompiler compiler(model, result);
    result.cppSource = compiler.Build(functionName);
    result.success = result.errors.empty();
    if (!result.success)
        result.cppSource.clear();
    return result;
}

} // namespace GameEngine
