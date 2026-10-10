#pragma once

#include <cstdint>

namespace GameEngine::Memory
{

/// Which allocations an `AllocationCountScope` counts.
enum class CountWindow : std::uint8_t
{
    /// Allocations made on the thread that constructed the scope.
    ThisThread,
    /// Allocations made on every thread while the scope is open: the window for a path
    /// that forks units onto workers, whose allocations a `ThisThread` window misses.
    Process,
};

/// Counts global `operator new` calls (every form) from construction until `Count()`.
///
/// It sees the allocations of every image that carries the allocation hook: every test
/// executable in every configuration, and `Engine`, `Editor`, `Player`,
/// `GameEngine.Native`, `EditorSDK` and native user modules where
/// `GE_DEBUG_INSTRUMENTATION` is 1 (Debug, DebugFast). A Release production image
/// carries no hook, so its own allocations are not counted. Allocations that bypass
/// `operator new` (`std::malloc`, `std::aligned_alloc`) are never counted.
///
/// Counts are a measurement for tests and labs, never timing. Overlapping scopes each
/// read their own delta.
///
///     Memory::AllocationCountScope scope(Memory::CountWindow::Process);
///     RunTheFrame();
///     EXPECT_EQ(scope.Count(), 0u);
class AllocationCountScope
{
  public:
    /// Opens the window. No default: every caller states the reach it measures.
    explicit AllocationCountScope(CountWindow window);
    ~AllocationCountScope();

    AllocationCountScope(const AllocationCountScope&) = delete;
    AllocationCountScope& operator=(const AllocationCountScope&) = delete;
    AllocationCountScope(AllocationCountScope&&) = delete;
    AllocationCountScope& operator=(AllocationCountScope&&) = delete;

    /// Allocations counted in this window since construction. A `ThisThread` scope is
    /// read on the thread that constructed it (asserted in Debug).
    std::uint64_t Count() const;

  private:
    CountWindow m_Window;
    std::uint64_t m_Start = 0;
    /// The constructing thread's counter block, for the `ThisThread` read check.
    [[maybe_unused]] const void* m_Thread = nullptr;
};

} // namespace GameEngine::Memory
