#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <chrono>

namespace Logger {

// Basic type definitions
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

// String types
using String = std::string;

// Container types
template<typename T>
using Vector = std::vector<T>;

// Smart pointer types
template<typename T>
using UniquePtr = std::unique_ptr<T>;

template<typename T>
using SharedPtr = std::shared_ptr<T>;

template<typename T>
using WeakPtr = std::weak_ptr<T>;

// Function types
template<typename T>
using Function = std::function<T>;

// Time types - use system_clock for timestamps that can be converted to time_t
using TimePoint = std::chrono::system_clock::time_point;
using Duration = std::chrono::system_clock::duration;

// Utility macros for preventing copy/assignment
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
    TypeName(const TypeName&) = delete; \
    TypeName& operator=(const TypeName&) = delete

// Utility function for creating unique pointers
template<typename T, typename... Args>
UniquePtr<T> MakeUnique(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

// Utility function for creating shared pointers
template<typename T, typename... Args>
SharedPtr<T> MakeShared(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
}

} // namespace Logger
