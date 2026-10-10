#include "Pathfinding/PathBuffer.h"

#include <algorithm>
#include <cstring>

namespace GameEngine::Pathfinding
{

PathHandle PathBuffer::AllocatePath(const PathPoint* points, uint32 count)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    uint32 index = 0;
    if (!m_FreeList.empty())
    {
        index = m_FreeList.back();
        m_FreeList.pop_back();
    }
    else
    {
        index = static_cast<uint32>(m_Entries.size());
        m_Entries.emplace_back();
    }

    Entry& entry = m_Entries[index];
    entry.Points.assign(points, points + count);
    entry.Generation = m_NextGeneration++;
    entry.Active = true;

    return PathHandle{index, entry.Generation};
}

void PathBuffer::FreePath(PathHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Entries.size())
        return;

    Entry& entry = m_Entries[handle.Index];
    if (entry.Generation != handle.Generation || !entry.Active)
        return;

    entry.Active = false;
    entry.Points.clear();
    m_FreeList.push_back(handle.Index);
}

uint32 PathBuffer::GetPointCount(PathHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Entries.size())
        return 0;

    const Entry& entry = m_Entries[handle.Index];
    if (entry.Generation != handle.Generation || !entry.Active)
        return 0;

    return static_cast<uint32>(entry.Points.size());
}

uint32 PathBuffer::CopyPoints(PathHandle handle, PathPoint* outBuffer, uint32 bufferCapacity) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Entries.size())
        return 0;

    const Entry& entry = m_Entries[handle.Index];
    if (entry.Generation != handle.Generation || !entry.Active)
        return 0;

    uint32 count = static_cast<uint32>(entry.Points.size());
    uint32 toCopy = (count < bufferCapacity) ? count : bufferCapacity;
    std::memcpy(outBuffer, entry.Points.data(), toCopy * sizeof(PathPoint));
    return toCopy;
}

PathPoint PathBuffer::GetPoint(PathHandle handle, uint32 index) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Entries.size())
        return PathPoint{0.0f, 0.0f, 0.0f};

    const Entry& entry = m_Entries[handle.Index];
    if (entry.Generation != handle.Generation || !entry.Active)
        return PathPoint{0.0f, 0.0f, 0.0f};

    if (index >= entry.Points.size())
        return PathPoint{0.0f, 0.0f, 0.0f};

    return entry.Points[index];
}

void PathBuffer::Clear()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    m_Entries.clear();
    m_FreeList.clear();
}

} // namespace GameEngine::Pathfinding
