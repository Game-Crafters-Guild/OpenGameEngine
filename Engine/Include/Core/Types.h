#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>
#include <chrono>

namespace GameEngine {

// Basic types
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;

using float32 = float;
using float64 = double;

using String = std::string;
using WString = std::wstring;

// Time types
using TimePoint = std::chrono::high_resolution_clock::time_point;
using Duration = std::chrono::duration<float64>;

// Smart pointers
template<typename T>
using UniquePtr = std::unique_ptr<T>;

template<typename T>
using SharedPtr = std::shared_ptr<T>;

template<typename T>
using WeakPtr = std::weak_ptr<T>;

template<typename T, typename... Args>
constexpr UniquePtr<T> MakeUnique(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

template<typename T, typename... Args>
constexpr SharedPtr<T> MakeShared(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
}

// Containers
template<typename T>
using Vector = std::vector<T>;

template<typename K, typename V>
using HashMap = std::unordered_map<K, V>;

// Function types
template<typename T>
using Function = std::function<T>;

// Platform detection
#ifdef PLATFORM_STEAMDECK
    #define PLATFORM_NAME "Steam Deck"
#elif defined(PLATFORM_WINDOWS)
    #define PLATFORM_NAME "Windows"
#elif defined(PLATFORM_LINUX)
    #define PLATFORM_NAME "Linux"
#elif defined(PLATFORM_MACOS)
    #define PLATFORM_NAME "macOS"
#else
    #define PLATFORM_NAME "Unknown"
#endif

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
