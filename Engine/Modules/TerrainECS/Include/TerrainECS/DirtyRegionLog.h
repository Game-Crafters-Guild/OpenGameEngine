#pragma once

#include "Types/Types.h"

#include <algorithm>
#include <array>

namespace GameEngine::TerrainECS
{

// Version-tagged dirty-region journal for one field (e.g. a terrain's
// heightfield). Producers append one rect per version bump; each consumer
// keeps its own cursor (the last version it consumed) and unions everything
// newer. This replaces the single clearable dirty rect, which could serve
// only one consumer — whoever cleared it first starved everyone downstream
// (the render extraction runs before physics in the frame, so physics would
// always read an empty rect).
class DirtyRegionLog
{
public:
    struct Region
    {
        int32 MinX = 0, MinZ = 0;
        int32 MaxX = 0, MaxZ = 0; // exclusive

        bool IsEmpty() const { return MaxX <= MinX || MaxZ <= MinZ; }

        void Union(const Region& o)
        {
            if (o.IsEmpty())
                return;
            if (IsEmpty())
            {
                *this = o;
                return;
            }
            MinX = std::min(MinX, o.MinX);
            MinZ = std::min(MinZ, o.MinZ);
            MaxX = std::max(MaxX, o.MaxX);
            MaxZ = std::max(MaxZ, o.MaxZ);
        }

        bool Contains(const Region& o) const
        {
            return o.IsEmpty() || (MinX <= o.MinX && MinZ <= o.MinZ &&
                                   MaxX >= o.MaxX && MaxZ >= o.MaxZ);
        }
    };

    // Record the region touched by `version`. Versions must be appended in
    // ascending order (the producer bumps a monotonic counter per edit).
    // When the log is full, the two oldest entries coalesce — the log never
    // forgets a region, it only answers stale cursors more conservatively.
    void Append(uint64 version, const Region& region)
    {
        if (m_Count == kCapacity)
        {
            m_Entries[1].Rect.Union(m_Entries[0].Rect);
            for (uint32 i = 1; i < kCapacity; ++i)
                m_Entries[i - 1] = m_Entries[i];
            --m_Count;
        }
        m_Entries[m_Count++] = Entry{version, region};
    }

    // Union of all regions recorded after `sinceVersion` into `out`.
    // Returns false when nothing newer exists (out is untouched).
    bool CollectSince(uint64 sinceVersion, Region& out) const
    {
        bool any = false;
        for (uint32 i = m_Count; i-- > 0;)
        {
            const Entry& e = m_Entries[i];
            if (e.Version <= sinceVersion)
                break; // entries are version-ascending
            if (!any)
            {
                out = e.Rect;
                any = true;
            }
            else
            {
                out.Union(e.Rect);
            }
        }
        return any;
    }

    uint64 LatestVersion() const { return m_Count ? m_Entries[m_Count - 1].Version : 0; }
    uint32 EntryCount() const { return m_Count; }

private:
    struct Entry
    {
        uint64 Version = 0;
        Region Rect;
    };

    static constexpr uint32 kCapacity = 16;

    std::array<Entry, kCapacity> m_Entries{};
    uint32 m_Count = 0;
};

} // namespace GameEngine::TerrainECS
