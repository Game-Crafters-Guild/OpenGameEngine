// The process's one allocation counter: the state behind AllocationCountScope,
// AllocationSiteTag and Testing::ScopedAllocationFault, fed by every image's
// allocation hook through Detail::OnAllocation.
//
// One copy per process. The Memory module compiles it into Engine; a test executable
// that links neither Engine nor the Memory module compiles its own copy. A target that
// links either sees GE_ALLOCATION_COUNTER_LINKED (Memory's interface definition, which
// Engine re-exports) and compiles this file empty, so its hook and its scopes reach the
// linked copy instead of a second counter that the linked images would never feed.
//
// No exported data and no exported thread_local: the state below has internal linkage,
// and the functions are plain (no GE_API), exported on Windows through Engine.dll's
// generated exports.def and on POSIX through default visibility.

#if !defined(GE_ALLOCATION_COUNTER_LINKED)

#include "Memory/AllocationCountScope.h"
#include "Memory/AllocationCounter.h"
#include "Memory/AllocationSiteTag.h"
#include "Memory/Testing/ScopedAllocationFault.h"
#include "Types/FalseSharing.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace GameEngine::Memory
{
namespace
{

/// One thread's counter state. Trivially destructible and constant-initialized, so the
/// hook may run on any thread at any time, static destruction and thread exit included.
struct ThreadCounters
{
    std::uint64_t Allocations = 0;
    AllocationSiteTag* Site = nullptr;
    Testing::ScopedAllocationFault* Fault = nullptr;
};

constinit thread_local ThreadCounters t_Counters;
/// Open process windows, read on every allocation. Padded so it never shares a line with
/// s_ProcessAllocations, which open windows write from every allocating thread.
constinit FalseSharingPadded<std::atomic<std::uint32_t>> s_ProcessWindows;
/// Incremented only while a process window is open, so with none open an allocation
/// costs one relaxed load of s_ProcessWindows here.
constinit FalseSharingPadded<std::atomic<std::uint64_t>> s_ProcessAllocations;

/// True when the fault denies this allocation. Every match before the failOnMatch-th
/// passes; from then on a Once fault denies the first match only, an Every fault each.
bool FaultDenies(std::size_t matchBytes, std::uint32_t& matchesBeforeFailure, Testing::FaultRepeat repeat,
                 std::uint32_t deniedCount, std::size_t bytes)
{
    if (matchBytes != 0 && matchBytes != bytes)
        return false;
    if (matchesBeforeFailure != 0)
    {
        --matchesBeforeFailure;
        return false;
    }
    return repeat == Testing::FaultRepeat::Every || deniedCount == 0;
}

} // namespace

namespace Detail
{

bool OnAllocation(std::size_t bytes)
{
    ThreadCounters& counters = t_Counters;
    if (Testing::ScopedAllocationFault* fault = counters.Fault)
    {
        if (FaultDenies(fault->m_MatchBytes, fault->m_MatchesBeforeFailure, fault->m_Repeat, fault->m_DeniedCount,
                        bytes))
        {
            ++fault->m_DeniedCount;
            return false;
        }
    }
    ++counters.Allocations;
    if (AllocationSiteTag* site = counters.Site)
        ++site->m_Count;
    if (s_ProcessWindows.Value.load(std::memory_order_relaxed) != 0)
        s_ProcessAllocations.Value.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace Detail

// The window's open count is raised before the start value is read, so an allocation
// ordered after construction (on this thread, or on a thread a later fork or signal
// reaches) both sees the window open and lands after the start value. Relaxed suffices:
// that ordering comes from the program's own synchronization, which a join or a wait
// provides before Count() reads.
AllocationCountScope::AllocationCountScope(CountWindow window)
    : m_Window(window)
    , m_Thread(&t_Counters)
{
    if (window == CountWindow::Process)
    {
        s_ProcessWindows.Value.fetch_add(1, std::memory_order_relaxed);
        m_Start = s_ProcessAllocations.Value.load(std::memory_order_relaxed);
    }
    else
    {
        m_Start = t_Counters.Allocations;
    }
}

AllocationCountScope::~AllocationCountScope()
{
    if (m_Window == CountWindow::Process)
        s_ProcessWindows.Value.fetch_sub(1, std::memory_order_relaxed);
}

std::uint64_t AllocationCountScope::Count() const
{
    if (m_Window == CountWindow::Process)
        return s_ProcessAllocations.Value.load(std::memory_order_relaxed) - m_Start;
    assert(m_Thread == &t_Counters &&
           "a ThisThread AllocationCountScope counts its constructing thread; read it there, or open a "
           "CountWindow::Process scope to count every thread");
    return t_Counters.Allocations - m_Start;
}

AllocationSiteTag::AllocationSiteTag(const char* name)
    : m_Name(name)
    , m_Outer(t_Counters.Site)
{
    t_Counters.Site = this;
}

AllocationSiteTag::~AllocationSiteTag()
{
    assert(t_Counters.Site == this &&
           "AllocationSiteTags close on the thread that opened them, innermost first; declare each tag in "
           "the scope it measures");
    t_Counters.Site = m_Outer;
}

namespace Testing
{

ScopedAllocationFault::ScopedAllocationFault(std::size_t matchBytes, std::uint32_t failOnMatch, FaultRepeat repeat)
    : m_MatchBytes(matchBytes)
    , m_MatchesBeforeFailure(failOnMatch - 1)
    , m_Repeat(repeat)
{
    assert(failOnMatch != 0 && "ScopedAllocationFault's failOnMatch counts from 1: 1 fails the first match");
    assert(t_Counters.Fault == nullptr &&
           "this thread already has an armed ScopedAllocationFault; close it before arming another");
    t_Counters.Fault = this;
}

ScopedAllocationFault::~ScopedAllocationFault()
{
    assert(t_Counters.Fault == this && "a ScopedAllocationFault is destroyed on the thread that armed it");
    t_Counters.Fault = nullptr;
}

} // namespace Testing

} // namespace GameEngine::Memory

#endif // !defined(GE_ALLOCATION_COUNTER_LINKED)
