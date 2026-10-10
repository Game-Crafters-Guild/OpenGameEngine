#pragma once

#include <ankerl/unordered_dense.h>

#include <cstdint>
#include <string_view>

namespace GameEngine {

/**
 * @brief A transparent hash for string keys: a FastHashMap<String, V, StringHash, std::equal_to<>> finds a
 * key from a std::string_view or a C string without building a String for the lookup.
 *
 * A std::string key and a view of the same characters hash alike, which is what makes the lookup
 * transparent.
 */
struct StringHash {
    using is_transparent = void;
    using is_avalanching = void;

    uint64_t operator()(std::string_view text) const noexcept
    {
        return ankerl::unordered_dense::hash<std::string_view>{}(text);
    }
};

} // namespace GameEngine
