#include "MarkupECS/MarkupRegionDisplayCache.h"

#include <algorithm>
#include <vector>

namespace GameEngine::MarkupECS
{

const MarkupRegionDisplayCache::Entry& MarkupRegionDisplayCache::Resolve(ECS::EntityHandle region,
                                                                          const MarkupRegionGroundKey& key,
                                                                          float32 extrudeHeight, uint64 frame,
                                                                          const MemberKeyReader& readMember,
                                                                          const GroundBuilder& buildGround)
{
    for (uint32 i = 0; i < key.MemberCount; ++i)
        RefreshMember(key.Members[i].Entity, readMember);
    std::unique_ptr<Entry>& slot = m_Entries.GetOrInsert(region.id);
    const bool fresh = !slot;
    if (fresh)
        slot = std::make_unique<Entry>();
    Entry& entry = *slot;
    const bool groundChanged = fresh || !(entry.GroundKey == key) || entry.MemberChanged;
    if (groundChanged)
    {
        if (!fresh)
            UnlistMembers(region.id, entry.GroundKey);
        ListMembers(region.id, key, readMember);
        entry.GroundKey = key;
        entry.MemberChanged = false;
        entry.Ground = buildGround();
        ++m_GroundBuilds;
    }
    if (groundChanged || entry.ExtrudeHeight != extrudeHeight)
    {
        entry.ExtrudeHeight = extrudeHeight;
        entry.Mesh = BuildMarkupRegionMesh(entry.Ground, extrudeHeight);
        entry.MeshRevision = m_NextMeshRevision++;
        entry.ChangedFrame = frame;
        ++m_MeshBuilds;
    }
    entry.LastSeenFrame = frame;
    return entry;
}

const MarkupRegionDisplayCache::Entry* MarkupRegionDisplayCache::Find(ECS::EntityHandle region) const
{
    const std::unique_ptr<Entry>* slot = m_Entries.Find(region.id);
    return slot ? slot->get() : nullptr;
}

void MarkupRegionDisplayCache::EvictUnseen(uint64 frame)
{
    m_Entries.EraseIf([this, frame](uint32 region, const std::unique_ptr<Entry>& entry) {
        const bool evicted = frame > entry->LastSeenFrame + kEvictAfterFrames;
        if (evicted)
            UnlistMembers(region, entry->GroundKey);
        return evicted;
    });
}

void MarkupRegionDisplayCache::Clear()
{
    m_Entries.Clear();
    m_Members.Clear();
}

void MarkupRegionDisplayCache::RefreshMember(ECS::EntityHandle member, const MemberKeyReader& readMember)
{
    MemberState* state = m_Members.Find(member.id);
    if (!state)
        return; // listed by no built region yet: the region that lists it builds with it
    const MarkupFootprintKey key = readMember(member);
    if (key == state->Key)
        return;
    state->Key = key;
    for (const uint32 region : state->Regions)
    {
        if (std::unique_ptr<Entry>* entry = m_Entries.Find(region))
            (*entry)->MemberChanged = true;
    }
}

void MarkupRegionDisplayCache::ListMembers(uint32 region, const MarkupRegionGroundKey& key,
                                           const MemberKeyReader& readMember)
{
    for (uint32 i = 0; i < key.MemberCount; ++i)
    {
        const ECS::EntityHandle member = key.Members[i].Entity;
        MemberState* state = m_Members.Find(member.id);
        if (!state)
        {
            state = &m_Members.GetOrInsert(member.id);
            state->Key = readMember(member);
        }
        if (std::find(state->Regions.begin(), state->Regions.end(), region) == state->Regions.end())
            state->Regions.push_back(region);
    }
}

void MarkupRegionDisplayCache::UnlistMembers(uint32 region, const MarkupRegionGroundKey& key)
{
    for (uint32 i = 0; i < key.MemberCount; ++i)
    {
        const uint32 member = key.Members[i].Entity.id;
        MemberState* state = m_Members.Find(member);
        if (!state)
            continue;
        std::erase(state->Regions, region);
        if (state->Regions.empty())
            m_Members.Erase(member);
    }
}

} // namespace GameEngine::MarkupECS
