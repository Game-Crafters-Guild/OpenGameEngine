#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Memory
{

// Allocation tracking categories for the engine's runtime allocation
// instrumentation. Each subsystem owns one entry; per-category byte
// counters live alongside the global allocator hook (see MemoryManager).
//
// Add entries at the end (before Count) and never reorder; values are
// stable identifiers used in profiling tools and CI gates.
enum class AllocationCategory : uint8_t
{
    Default = 0,
    Animation = 1,

    Count
};

// Lightweight per-category byte counter. Subsystems that own a long-lived
// pool (RetargetGPUDataStore, SkinPaletteAtlas, etc.) report their resident
// VRAM/RAM allocation here so CI can assert per-character budget without
// hooking the global allocator. Process-global; thread-safe via atomic
// stores. Read with GetCategoryBytes(); write deltas with TrackAllocation
// (positive add) and TrackDeallocation (positive subtract from a pool).
//
// Per-category counters live in the .cpp; static-initialization-order safe
// because the storage is a function-local static.
void     TrackAllocation(AllocationCategory category, uint64_t bytes);
void     TrackDeallocation(AllocationCategory category, uint64_t bytes);
uint64_t GetCategoryBytes(AllocationCategory category);
void     ResetCategoryBytes(AllocationCategory category);

} // namespace Memory
} // namespace GameEngine
