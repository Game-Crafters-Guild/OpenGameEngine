#pragma once

#include <cstddef>

namespace GameEngine::Memory::Detail
{

/// The allocation hook's entry into the process's one allocation counter
/// (`Source/AllocationCounter.cpp`). Every global `operator new` replacement in
/// `Memory/AllocationHook.inl` calls it once per allocation, before allocating.
///
/// Counts the allocation on the calling thread, for the innermost
/// `AllocationSiteTag` of that thread and for every open process window. Returns
/// false, counting nothing, when the calling thread's armed
/// `Testing::ScopedAllocationFault` denies an allocation of `bytes`; the hook then
/// throws `std::bad_alloc`, or returns null from a `nothrow` form.
///
/// A plain function with no `GE_API`: on Windows it reaches `Engine.dll`'s export
/// table through the generated `exports.def`, so every image's hook counts into the
/// counter in `Engine.dll`; on macOS and Linux it is exported by default visibility.
bool OnAllocation(std::size_t bytes);

} // namespace GameEngine::Memory::Detail
