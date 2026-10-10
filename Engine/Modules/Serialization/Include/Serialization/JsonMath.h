#pragma once

#include "Mathematics/Vector3.h"
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace GameEngine::Serialization
{
/// Reads exactly three numeric coordinates; malformed values throw.
inline Mathematics::Vector3 ReadVector3(const nlohmann::json& value)
{
    if (!value.is_array() || value.size() != 3)
        throw std::runtime_error("Expected three coordinates");
    return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
}

/// Reads an integer without accepting negative values, fractions or overflow.
inline uint32_t ReadUInt32(const nlohmann::json& value)
{
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
        value.get<uint64_t>() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Expected an unsigned 32-bit integer");
    return value.get<uint32_t>();
}
}
