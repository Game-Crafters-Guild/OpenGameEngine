// Fault injection: which allocation a ScopedAllocationFault denies, and how a denial surfaces.

#include "Memory/AllocationCountScope.h"
#include "Memory/Testing/ScopedAllocationFault.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <new>

namespace
{
using GameEngine::Memory::AllocationCountScope;
using GameEngine::Memory::CountWindow;
using GameEngine::Memory::Testing::FaultRepeat;
using GameEngine::Memory::Testing::ScopedAllocationFault;

constexpr std::size_t kMatchBytes = 48;
constexpr std::size_t kOtherBytes = 96;

/// True when an allocation of `bytes` succeeded; a denied one throws std::bad_alloc.
bool TryAllocate(std::size_t bytes)
{
    try
    {
        ::operator delete(::operator new(bytes));
        return true;
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
}
} // namespace

// The failOnMatch-th allocation of the matched size fails once; other sizes and later matches
// succeed.
TEST(ScopedAllocationFaultTests, AOnceFaultDeniesOnlyTheNthMatch)
{
    ScopedAllocationFault fault{kMatchBytes, 2, FaultRepeat::Once};
    EXPECT_TRUE(TryAllocate(kMatchBytes));
    EXPECT_TRUE(TryAllocate(kOtherBytes));
    EXPECT_FALSE(fault.Fired());
    EXPECT_FALSE(TryAllocate(kMatchBytes));
    EXPECT_TRUE(fault.Fired());
    EXPECT_TRUE(TryAllocate(kMatchBytes));
    EXPECT_EQ(fault.DeniedCount(), 1u);
}

// matchBytes 0 matches every size, and an Every fault denies each match from the nth on.
TEST(ScopedAllocationFaultTests, AnEveryFaultOfAnySizeDeniesEachMatchFromTheNthOn)
{
    ScopedAllocationFault fault{0, 1, FaultRepeat::Every};
    EXPECT_FALSE(TryAllocate(kMatchBytes));
    EXPECT_FALSE(TryAllocate(kOtherBytes));
    EXPECT_FALSE(TryAllocate(kMatchBytes));
    EXPECT_EQ(fault.DeniedCount(), 3u);
}

// A nothrow form returns null instead of throwing, and a denied allocation is not counted.
TEST(ScopedAllocationFaultTests, ADeniedNothrowAllocationReturnsNullAndIsNotCounted)
{
    const AllocationCountScope allocations(CountWindow::ThisThread);
    void* memory = nullptr;
    {
        ScopedAllocationFault fault{kMatchBytes, 1, FaultRepeat::Once};
        memory = ::operator new(kMatchBytes, std::nothrow);
        EXPECT_TRUE(fault.Fired());
    }

    EXPECT_EQ(memory, nullptr);
    EXPECT_EQ(allocations.Count(), 0u);
}
