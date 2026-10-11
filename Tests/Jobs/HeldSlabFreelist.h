#pragma once

#include "JobSystem/TaskSlabPool.h"

#include <vector>

namespace GameEngine::Tests
{

// Takes every slab the process-wide task-slab freelist holds and keeps it
// until destruction, so a test starts from an empty freelist without skewing
// the slab statistics (the slabs are released, not deleted). Acquires until
// the first heap allocation; on one thread with no other slab traffic the
// freelist's dequeue does not fail spuriously.
class HeldSlabFreelist
{
  public:
    HeldSlabFreelist()
    {
        for (;;)
        {
            const auto heapAllocsBefore = JobSystem::Detail::GetTaskSlabStatsForTests().HeapAllocs;
            m_Slabs.push_back(JobSystem::Detail::AcquireTaskSlab());
            if (JobSystem::Detail::GetTaskSlabStatsForTests().HeapAllocs != heapAllocsBefore)
                return;
        }
    }
    ~HeldSlabFreelist()
    {
        for (void* slab : m_Slabs)
            JobSystem::Detail::ReleaseTaskSlab(slab);
    }
    HeldSlabFreelist(const HeldSlabFreelist&) = delete;
    HeldSlabFreelist& operator=(const HeldSlabFreelist&) = delete;

  private:
    std::vector<void*> m_Slabs;
};

} // namespace GameEngine::Tests
