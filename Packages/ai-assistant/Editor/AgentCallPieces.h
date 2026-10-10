#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{
struct AssistantAction;

/// One piece of a call row's summary or of a line of its Details, as the row draws it:
/// words, muted words (a key, a separator), an entity (a link to it), or a vector (its
/// components with muted axis labels).
struct AgentCallPiece
{
    enum class Kind : uint8_t
    {
        Text,
        Muted,
        Entity,
        Vector,
        /// The indentation of a Details line: Text holds one space per column.
        Indent,
    };

    Kind Type = Kind::Text;
    /// Text and Muted: the words.
    std::string Text;
    /// Entity: the entity's id, as the editor's tools name it.
    uint32_t EntityId = 0;
    /// Vector: x, y, z and, for a quaternion, w.
    std::vector<double> Components;

    bool operator==(const AgentCallPiece&) const = default;

    static AgentCallPiece Words(std::string text);
    static AgentCallPiece MutedWords(std::string text);
    static AgentCallPiece Entity(uint32_t id);
    static AgentCallPiece VectorOf(std::vector<double> components);
};

using AgentCallPieces = std::vector<AgentCallPiece>;

/// The decimals a call row shows of a number (FormatFixed, trailing zeros trimmed).
inline constexpr int kAgentCallDecimals = 3;

/// The pieces as plain text: an entity as "#<id>", a vector as "x 12 y 0 z 4" (numbers at
/// most three decimals, FormatFixed), for tests and accessibility text.
std::string AgentCallPiecesText(const AgentCallPieces& pieces);

/// `value` as a vector when it is an object of numbers named from x, y, z and w (at least
/// two of x, y, z); its components in x, y, z, w order.
std::optional<std::vector<double>> AgentCallVector(const nlohmann::json& value);

/// The summary of a call the editor recorded: its subject arguments, joined by a muted
/// " · ": an entity id as a link, the entity a create_entity made as a link (from the
/// editor's answer), a vector or an object's vectors with their keys muted, other values
/// as words. Never raw JSON.
AgentCallPieces AgentCallSummary(const AssistantAction& action);
/// The summary of a call the provider reported with `inputJson`, the same way.
AgentCallPieces AgentCallSummary(std::string_view tool, std::string_view inputJson);

/// `value` pretty-printed as lines of pieces: two spaces per level, keys muted, a vector
/// on its key's line as a vector.
std::vector<AgentCallPieces> AgentCallJsonLines(const nlohmann::json& value);
} // namespace GameEngine
