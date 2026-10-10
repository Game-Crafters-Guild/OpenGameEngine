#pragma once

#include "Types/StringId.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace GameEngine
{
namespace Animation
{

// A parameter value that can drive state machine transitions and blend weights.
using ParamValue = std::variant<float, int32_t, bool>;

struct GraphParam
{
    std::string Name;
    ParamValue Value;
};

enum class ParamCompare : uint8_t
{
    Equals,
    NotEquals,
    Greater,
    Less,
    GreaterEqual,
    LessEqual
};

inline const char* ParamCompareName(ParamCompare op)
{
    switch (op)
    {
    case ParamCompare::NotEquals:
        return "notEquals";
    case ParamCompare::Greater:
        return "greaterThan";
    case ParamCompare::Less:
        return "lessThan";
    case ParamCompare::GreaterEqual:
        return "greaterOrEqual";
    case ParamCompare::LessEqual:
        return "lessOrEqual";
    case ParamCompare::Equals:
    default:
        return "equals";
    }
}

inline bool TryParamCompareFromName(std::string_view name, ParamCompare& outOp)
{
    if (name.empty() || name == "equals")
    {
        outOp = ParamCompare::Equals;
        return true;
    }
    if (name == "notEquals")
    {
        outOp = ParamCompare::NotEquals;
        return true;
    }
    if (name == "greaterThan" || name == "greater")
    {
        outOp = ParamCompare::Greater;
        return true;
    }
    if (name == "lessThan" || name == "less")
    {
        outOp = ParamCompare::Less;
        return true;
    }
    if (name == "greaterOrEqual" || name == "greaterEqual")
    {
        outOp = ParamCompare::GreaterEqual;
        return true;
    }
    if (name == "lessOrEqual")
    {
        outOp = ParamCompare::LessEqual;
        return true;
    }
    return false;
}

inline ParamCompare ParamCompareFromName(std::string_view name)
{
    ParamCompare op = ParamCompare::Equals;
    TryParamCompareFromName(name, op);
    return op;
}

inline float ParamAsFloat(const ParamValue& value)
{
    if (const auto* f = std::get_if<float>(&value))
        return *f;
    if (const auto* i = std::get_if<int32_t>(&value))
        return static_cast<float>(*i);
    if (const auto* b = std::get_if<bool>(&value))
        return *b ? 1.0f : 0.0f;
    return 0.0f;
}

inline bool CompareParam(ParamCompare op, const ParamValue& actual, const ParamValue& expected)
{
    switch (op)
    {
    case ParamCompare::Equals:
        return actual == expected;
    case ParamCompare::NotEquals:
        return actual != expected;
    case ParamCompare::Greater:
        return ParamAsFloat(actual) > ParamAsFloat(expected);
    case ParamCompare::Less:
        return ParamAsFloat(actual) < ParamAsFloat(expected);
    case ParamCompare::GreaterEqual:
        return ParamAsFloat(actual) >= ParamAsFloat(expected);
    case ParamCompare::LessEqual:
        return ParamAsFloat(actual) <= ParamAsFloat(expected);
    }
    return false;
}

// Declarative transition predicate. JSON round-trips param/op/value.
// ParamId is FNV-1a of ParamName (empty name ⇒ 0).
struct TransitionCondition
{
    std::string ParamName;
    ::GameEngine::StringId ParamId = 0;
    ParamCompare Op = ParamCompare::Equals;
    ParamValue Expected{};
    bool Valid = true;

    static TransitionCondition Make(std::string_view name, ParamCompare op, ParamValue expected)
    {
        TransitionCondition c;
        c.ParamName.assign(name);
        c.ParamId = HashStringId(name);
        c.Op = op;
        c.Expected = std::move(expected);
        c.Valid = true;
        return c;
    }

    static TransitionCondition Invalid()
    {
        TransitionCondition c;
        c.Valid = false;
        return c;
    }

    bool Matches(const ParamValue& actual) const
    {
        if (!Valid)
            return false;
        return CompareParam(Op, actual, Expected);
    }
};

} // namespace Animation
} // namespace GameEngine
