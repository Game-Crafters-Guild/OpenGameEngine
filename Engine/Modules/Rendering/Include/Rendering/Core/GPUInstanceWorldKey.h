#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Rendering
{

/// Folds a 64-bit world id into the 16-bit world tag that GPUInstance.flags
/// carries in bits 16..31 and that draw_command_scatter.comp compares against
/// the view's key. Instance stamping and view filtering must use this one fold:
/// a mismatch filters the world's instances out of its own views.
inline constexpr uint32_t PackWorldKey16(uint64_t worldId)
{
    return static_cast<uint32_t>((worldId ^ (worldId >> 32)) & 0xFFFFu);
}

} // namespace Rendering
} // namespace GameEngine
