#pragma once

#include "Types/StringUtils.h"
#include "Types/Types.h"

#include <string_view>

namespace GameEngine {
namespace Components {

// Simple entity name component for editor-facing labels.
// Uses a fixed-size char buffer to satisfy ECS component constraints
// (trivially copyable, standard layout, no pointers).
struct Name {
    // The entity's label, not a feature: it has no off state.
    static constexpr bool NotToggleable = true;

    // Not guaranteed to be null-terminated: a raw component write
    // (GE_ECSABI_SetComponentBytes) can fill all 64 bytes. Read it through View().
    char value[64];

    // The name up to its first null byte, or all of value when there is none.
    [[nodiscard]] std::string_view View() const { return FixedStringView(value); }
};

} // namespace Components
} // namespace GameEngine
