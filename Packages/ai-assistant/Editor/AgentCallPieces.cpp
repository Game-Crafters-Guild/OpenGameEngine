#include "AgentCallPieces.h"

#include "AssistantActionLedger.h"
#include "AssistantTools.h"

#include "Types/FormatNumber.h"

#include <nlohmann/json.hpp>

#include <array>

namespace GameEngine
{
namespace
{
constexpr std::array<const char*, 4> kAxes = {"x", "y", "z", "w"};
// Arguments that name an entity by its id.
constexpr std::string_view kEntityArgument = "entityId";
// The argument create_entity names its entity with; the link replaces it once the editor
// answered with the entity's id.
constexpr std::string_view kCreatedName = "name";

void Append(AgentCallPieces& to, const AgentCallPieces& pieces)
{
    to.insert(to.end(), pieces.begin(), pieces.end());
}

std::optional<uint32_t> EntityIdOf(const nlohmann::json& value)
{
    if (!value.is_number_unsigned() && !value.is_number_integer())
        return std::nullopt;
    const int64_t id = value.get<int64_t>();
    if (id < 0 || id > 0xFFFFFFFFll)
        return std::nullopt;
    return static_cast<uint32_t>(id);
}

// A scalar as words: strings bare, numbers as rows show them, anything else as JSON.
std::string ScalarText(const nlohmann::json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_number_float())
        return FormatFixed(value.get<double>(), kAgentCallDecimals);
    return value.dump();
}

// One argument's value: an entity, a vector, an object's keys (muted) and values, or words.
AgentCallPieces ValuePieces(std::string_view key, const nlohmann::json& value)
{
    if (key == kEntityArgument)
        if (const std::optional<uint32_t> id = EntityIdOf(value))
            return {AgentCallPiece::Entity(*id)};
    if (const std::optional<std::vector<double>> vector = AgentCallVector(value))
        return {AgentCallPiece::VectorOf(*vector)};
    if (!value.is_object())
        return {AgentCallPiece::Words(ScalarText(value))};
    AgentCallPieces pieces;
    for (const auto& [field, fieldValue] : value.items())
    {
        if (!pieces.empty())
            pieces.push_back(AgentCallPiece::MutedWords(","));
        pieces.push_back(AgentCallPiece::MutedWords(field));
        Append(pieces, ValuePieces(field, fieldValue));
    }
    return pieces;
}

// The entity create_entity made, from the editor's answer ({"ok": true, "result":
// {"entityId": N}}); nullopt before the answer, for a refusal, or for a cut one.
std::optional<uint32_t> CreatedEntity(const AssistantAction& action)
{
    if (action.Tool != "create_entity" || action.Result.empty())
        return std::nullopt;
    const nlohmann::json response = nlohmann::json::parse(action.Result, nullptr, false);
    if (!response.is_object() || !response.value("ok", false))
        return std::nullopt;
    const auto result = response.find("result");
    if (result == response.end() || !result->is_object())
        return std::nullopt;
    const auto id = result->find(kEntityArgument);
    return id == result->end() ? std::nullopt : EntityIdOf(*id);
}

AgentCallPieces Summary(std::string_view tool, const nlohmann::json& arguments, std::optional<uint32_t> created)
{
    const AssistantTool* row = AssistantTools::Find(tool);
    AgentCallPieces pieces;
    if (!row || !arguments.is_object())
        return pieces;
    for (std::string_view argument : row->SubjectArguments)
    {
        if (argument.empty())
            continue;
        const auto value = arguments.find(argument);
        const bool linked = created && argument == kCreatedName;
        if (!linked && (value == arguments.end() || value->is_null() || (value->is_object() && value->empty())))
            continue;
        if (!pieces.empty())
            pieces.push_back(AgentCallPiece::MutedWords("·"));
        if (linked)
            pieces.push_back(AgentCallPiece::Entity(*created));
        else
            Append(pieces, ValuePieces(argument, *value));
    }
    return pieces;
}

void JsonLines(const nlohmann::json& value, const std::string& indent, AgentCallPieces line,
               std::vector<AgentCallPieces>& lines)
{
    if (const std::optional<std::vector<double>> vector = AgentCallVector(value))
    {
        line.push_back(AgentCallPiece::VectorOf(*vector));
        lines.push_back(std::move(line));
        return;
    }
    if (!value.is_structured() || value.empty())
    {
        line.push_back(AgentCallPiece::Words(value.is_string() ? "\"" + value.get<std::string>() + "\"" : value.dump()));
        lines.push_back(std::move(line));
        return;
    }
    const bool object = value.is_object();
    line.push_back(AgentCallPiece::Words(object ? "{" : "["));
    lines.push_back(std::move(line));
    const std::string inner = indent + "  ";
    for (const auto& [key, item] : value.items())
    {
        AgentCallPieces itemLine{AgentCallPiece{AgentCallPiece::Kind::Indent, inner, 0, {}}};
        if (object)
            itemLine.push_back(AgentCallPiece::MutedWords(key + ":"));
        JsonLines(item, inner, std::move(itemLine), lines);
    }
    AgentCallPieces close;
    if (!indent.empty())
        close.push_back(AgentCallPiece{AgentCallPiece::Kind::Indent, indent, 0, {}});
    close.push_back(AgentCallPiece::Words(object ? "}" : "]"));
    lines.push_back(std::move(close));
}
} // namespace

AgentCallPiece AgentCallPiece::Words(std::string text)
{
    return {Kind::Text, std::move(text), 0, {}};
}

AgentCallPiece AgentCallPiece::MutedWords(std::string text)
{
    return {Kind::Muted, std::move(text), 0, {}};
}

AgentCallPiece AgentCallPiece::Entity(uint32_t id)
{
    return {Kind::Entity, {}, id, {}};
}

AgentCallPiece AgentCallPiece::VectorOf(std::vector<double> components)
{
    return {Kind::Vector, {}, 0, std::move(components)};
}

std::string AgentCallPiecesText(const AgentCallPieces& pieces)
{
    std::string text;
    for (const AgentCallPiece& piece : pieces)
    {
        if (!text.empty())
            text += ' ';
        switch (piece.Type)
        {
        case AgentCallPiece::Kind::Text:
        case AgentCallPiece::Kind::Muted:
        case AgentCallPiece::Kind::Indent:
            text += piece.Text;
            break;
        case AgentCallPiece::Kind::Entity:
            text += "#" + std::to_string(piece.EntityId);
            break;
        case AgentCallPiece::Kind::Vector:
            for (size_t axis = 0; axis < piece.Components.size(); ++axis)
                text += std::string(axis == 0 ? "" : " ") + kAxes[axis] + " " + FormatFixed(piece.Components[axis], kAgentCallDecimals);
            break;
        }
    }
    return text;
}

std::optional<std::vector<double>> AgentCallVector(const nlohmann::json& value)
{
    if (!value.is_object() || value.size() < 2 || value.size() > 4)
        return std::nullopt;
    std::vector<double> components;
    for (size_t axis = 0; axis < kAxes.size(); ++axis)
    {
        const auto component = value.find(kAxes[axis]);
        if (component == value.end())
            break;
        if (!component->is_number())
            return std::nullopt;
        components.push_back(component->get<double>());
    }
    // Every member is an axis, and the axes run from x without a gap.
    if (components.size() != value.size())
        return std::nullopt;
    return components;
}

AgentCallPieces AgentCallSummary(const AssistantAction& action)
{
    return Summary(action.Tool, nlohmann::json::parse(action.Arguments, nullptr, false), CreatedEntity(action));
}

AgentCallPieces AgentCallSummary(std::string_view tool, std::string_view inputJson)
{
    return Summary(tool, nlohmann::json::parse(inputJson, nullptr, false), std::nullopt);
}

std::vector<AgentCallPieces> AgentCallJsonLines(const nlohmann::json& value)
{
    std::vector<AgentCallPieces> lines;
    JsonLines(value, "", {}, lines);
    return lines;
}
} // namespace GameEngine
