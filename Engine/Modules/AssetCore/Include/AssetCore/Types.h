#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace GameEngine {

// Basic type aliases
using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;

using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

using float32 = float;
using float64 = double;

using String = std::string;

template<typename T>
using Vector = std::vector<T>;

template<typename T>
using SharedPtr = std::shared_ptr<T>;

template<typename T>
using UniquePtr = std::unique_ptr<T>;

template<typename T>
using WeakPtr = std::weak_ptr<T>;

// Utility macros
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
    TypeName(const TypeName&) = delete; \
    TypeName& operator=(const TypeName&) = delete

#define DISALLOW_MOVE_AND_ASSIGN(TypeName) \
    TypeName(TypeName&&) = delete; \
    TypeName& operator=(TypeName&&) = delete

#define DISALLOW_COPY_MOVE_AND_ASSIGN(TypeName) \
    DISALLOW_COPY_AND_ASSIGN(TypeName); \
    DISALLOW_MOVE_AND_ASSIGN(TypeName)

} // namespace GameEngine
