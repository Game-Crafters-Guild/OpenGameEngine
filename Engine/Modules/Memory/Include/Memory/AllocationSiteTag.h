#pragma once

#include "Memory/AllocationCounter.h"

#include <cstdint>

namespace GameEngine::Memory
{

/// Attributes the calling thread's allocations to a named site while the tag is the
/// thread's innermost one: an allocation is counted for the innermost tag only, and
/// an outer tag resumes counting when the inner one closes. Allocations on other
/// threads, including units this thread forks, are not attributed.
///
///     Memory::AllocationSiteTag site("Query::Parallel");
///     query.Parallel(body);
///     report(site.Name(), site.Count());
///
/// Opened and closed on one thread, in LIFO order (asserted in Debug).
class AllocationSiteTag
{
  public:
    /// `name` is not copied: pass a string that outlives the tag (a literal).
    explicit AllocationSiteTag(const char* name);
    ~AllocationSiteTag();

    AllocationSiteTag(const AllocationSiteTag&) = delete;
    AllocationSiteTag& operator=(const AllocationSiteTag&) = delete;
    AllocationSiteTag(AllocationSiteTag&&) = delete;
    AllocationSiteTag& operator=(AllocationSiteTag&&) = delete;

    const char* Name() const { return m_Name; }

    /// Allocations attributed to this site since construction.
    std::uint64_t Count() const { return m_Count; }

  private:
    friend bool Detail::OnAllocation(std::size_t bytes);

    const char* m_Name;
    AllocationSiteTag* m_Outer = nullptr;
    std::uint64_t m_Count = 0;
};

} // namespace GameEngine::Memory
