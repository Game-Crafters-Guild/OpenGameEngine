#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace GameEngine::JsonFields
{
/// The string member `name` of `object`; empty when it is missing or not a string.
inline std::string String(const nlohmann::json& object, const char* name)
{
    const auto it = object.find(name);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

/// The unsigned integer member `name` of `object`; zero when it is missing or not
/// an unsigned integer.
inline uint64_t Count(const nlohmann::json& object, const char* name)
{
    const auto it = object.find(name);
    return it != object.end() && it->is_number_unsigned() ? it->get<uint64_t>() : 0;
}

/// The object member `name` of `object`; an empty object when it is missing or
/// not an object.
inline const nlohmann::json& Object(const nlohmann::json& object, const char* name)
{
    static const nlohmann::json kEmpty = nlohmann::json::object();
    const auto it = object.find(name);
    return it != object.end() && it->is_object() ? *it : kEmpty;
}
} // namespace GameEngine::JsonFields
