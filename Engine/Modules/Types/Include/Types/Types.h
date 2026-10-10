#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <chrono>

#include <ankerl/unordered_dense.h>

#include "Types/Color.h"
#include "Types/StringId.h"

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

// Open-addressed flat hash map (ankerl::unordered_dense). Faster lookup than
// std::unordered_map (~3-5x in steady state) and better cache behavior — at the
// cost of iterator stability across insert/erase. Use this for hot paths
// (asset registry, scene queries, GPU resource lookups). Use HashMap when you
// need iterator/pointer stability across mutations.
//
// Hash defaults to std::hash<K> so existing user-defined std::hash<T>
// specializations (GUID, StringId, etc.) keep working without churn.
template<typename K, typename V,
         typename Hash = std::hash<K>,
         typename Eq = std::equal_to<K>>
using FastHashMap = ankerl::unordered_dense::map<K, V, Hash, Eq>;

template<typename T,
         typename Hash = std::hash<T>,
         typename Eq = std::equal_to<T>>
using FastHashSet = ankerl::unordered_dense::set<T, Hash, Eq>;

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
