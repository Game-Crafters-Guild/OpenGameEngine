#pragma once

// The engine's one false-sharing distance and the wrapper that applies it.
//
// Data written by different threads is kept kFalseSharingSeparation bytes
// apart on every platform. 128 is the coherence line on Apple Silicon, and on
// x86 the adjacent-line prefetcher pulls 64-byte lines in 128-byte aligned
// pairs, so two writers 64 bytes apart still contend there. The value is fixed
// rather than taken from std::hardware_destructive_interference_size, which
// is a compiler choice rather than a hardware fact (256 under Apple Clang on
// arm64) and may change with compiler flags, an ABI hazard for a layout
// constant.
//
// Layout rules:
//  - A member whose size differs per platform (std::mutex,
//    std::condition_variable) goes in FalseSharingPadded, which needs no
//    per-platform arithmetic.
//  - Fixed-size atomic words that share an owner and an access pattern may be
//    grouped on one separation-aligned line by hand; pin such a layout with
//    offsetof and alignof static_asserts against kFalseSharingSeparation.
//  - Heap storage for an over-aligned type comes from the aligned operator
//    new (C++17), which `new` and the standard containers use automatically;
//    a custom or arena allocator must honour alignof(T) itself.

#include <cstddef>

namespace GameEngine
{

inline constexpr std::size_t kFalseSharingSeparation = 128;

static_assert((kFalseSharingSeparation & (kFalseSharingSeparation - 1)) == 0,
              "kFalseSharingSeparation must be a power of two to be an alignment");
static_assert(kFalseSharingSeparation >= alignof(std::max_align_t),
              "kFalseSharingSeparation must not weaken the fundamental alignment");

// A value alone on its kFalseSharingSeparation-aligned span: its size rounds up
// to a multiple of the separation, so neighbours in an array or a struct never
// share a line with it. Aggregate: `FalseSharingPadded<std::atomic<uint64>> c{};`
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // structure was padded due to alignment specifier: the padding is the point
#endif
template <typename T>
struct alignas(kFalseSharingSeparation) FalseSharingPadded
{
    T Value{};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace GameEngine
