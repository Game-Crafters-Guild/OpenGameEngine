#pragma once

#include "Memory/AllocationCounter.h"

#include <cstddef>
#include <cstdint>

namespace GameEngine::Memory::Testing
{

/// Whether a `ScopedAllocationFault` denies one allocation or every match from then on.
enum class FaultRepeat : std::uint8_t
{
    Once,
    Every,
};

/// Fails allocations on the calling thread while it is armed, to test what code does
/// when `operator new` throws. The `failOnMatch`-th allocation whose size equals
/// `matchBytes` (0 matches every size) is denied, and with `FaultRepeat::Every` every
/// match after it too. A denied allocation throws `std::bad_alloc`, or returns null
/// from a `nothrow` form, and is not counted by any `AllocationCountScope`.
///
/// It reaches every image the allocation hook reaches (every test executable in every
/// configuration; the engine's images under `GE_DEBUG_INSTRUMENTATION`), because its
/// state is the one process counter's.
///
///     Memory::Testing::ScopedAllocationFault fault{sizeof(Node), 2, Memory::Testing::FaultRepeat::Once};
///     EXPECT_THROW(list.InsertTwo(), std::bad_alloc);
///     EXPECT_TRUE(fault.Fired());
///
/// Constructed, read and destroyed on one thread; one armed fault per thread (asserted
/// in Debug).
class ScopedAllocationFault
{
  public:
    /// `failOnMatch` counts from 1.
    ScopedAllocationFault(std::size_t matchBytes, std::uint32_t failOnMatch, FaultRepeat repeat);
    ~ScopedAllocationFault();

    ScopedAllocationFault(const ScopedAllocationFault&) = delete;
    ScopedAllocationFault& operator=(const ScopedAllocationFault&) = delete;
    ScopedAllocationFault(ScopedAllocationFault&&) = delete;
    ScopedAllocationFault& operator=(ScopedAllocationFault&&) = delete;

    /// True once an allocation has been denied.
    bool Fired() const { return m_DeniedCount != 0; }

    /// Allocations denied so far.
    std::uint32_t DeniedCount() const { return m_DeniedCount; }

  private:
    friend bool Detail::OnAllocation(std::size_t bytes);

    std::size_t m_MatchBytes;
    /// Matches still to pass before the first denial.
    std::uint32_t m_MatchesBeforeFailure;
    FaultRepeat m_Repeat;
    std::uint32_t m_DeniedCount = 0;
};

} // namespace GameEngine::Memory::Testing
