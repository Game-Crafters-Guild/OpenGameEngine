#include "ECS/CachedQuery.h"

#include "ECS/ArchetypeTable.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Components.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <vector>

namespace GameEngine::ECS
{

CachedQuery::CachedQuery(World* world,
                         std::vector<ComponentTypeId> required,
                         std::vector<ComponentTypeId> excluded)
    : m_World(world)
    , m_RequiredTypeIds(std::move(required))
{
    for (auto id : m_RequiredTypeIds)
        m_Required.Add(id);
    for (auto id : excluded)
        m_Excluded.Add(id);
    m_HasExcluded = !excluded.empty();

    const ComponentTypeId disabledId = GetComponentTypeId<Disabled>();
    const ComponentTypeId inHierarchyId = GetComponentTypeId<DisabledInHierarchy>();
    if (!m_Required.Contains(disabledId) && !m_Required.Contains(inHierarchyId))
    {
        m_DisabledExclude.Add(disabledId);
        m_DisabledExclude.Add(inHierarchyId);
    }
    for (ComponentTypeId id : m_RequiredTypeIds)
    {
        if (const ComponentRegistry::ComponentInfo* info = ComponentRegistry::GetComponentInfo(id))
            m_DisabledExclude.Add(ComponentDisabledTypeId(info->Name));
    }
    for (ComponentTypeId id : m_RequiredTypeIds)
        m_DisabledExclude.Remove(id);
}

void CachedQuery::Refresh()
{
    if (!m_World)
        return;

    const std::size_t currentVersion = m_World->GetStructuralChangeVersion();
    if (m_HasRefreshed && m_CachedVersion == currentVersion)
        return;

    ArchetypeFilter filter;
    filter.Required = &m_Required;
    filter.Exclude = m_HasExcluded ? &m_Excluded : nullptr;
    filter.DisabledExclude = &m_DisabledExclude;
    m_World->CollectMatchingArchetypes(filter, m_CachedArchetypes);
    // Managed iteration indexes this list; an archetype with no entities has no chunk to hand out.
    std::erase_if(m_CachedArchetypes,
                  [](const Archetype* archetype) { return archetype->GetEntityCount() == 0; });

    m_CachedVersion = currentVersion;
    m_HasRefreshed = true;
    ResolveChangedColumns();
}

void CachedQuery::SetChangedFilter(std::vector<ComponentTypeId> types)
{
    m_ChangedTypes = std::move(types);
    ResolveChangedColumns();
}

void CachedQuery::ResolveChangedColumns()
{
    m_ChangedColumns.clear();
    if (m_ChangedTypes.empty())
        return;
    m_ChangedColumns.reserve(m_CachedArchetypes.size() * m_ChangedTypes.size());
    for (auto* archetype : m_CachedArchetypes)
    {
        for (ComponentTypeId typeId : m_ChangedTypes)
        {
            const int col = (archetype && archetype->HasTable())
                                ? archetype->GetTable().FindColumnIndex(typeId)
                                : -1;
            m_ChangedColumns.push_back(col);
        }
    }
}

bool CachedQuery::ChunkChangedSince(std::size_t archetypeIndex, std::size_t chunkIndex,
                                    uint64_t gate) const
{
    if (m_ChangedTypes.empty())
        return true; // no filter — every chunk passes

    auto* archetype = GetArchetype(archetypeIndex);
    if (!archetype || !archetype->HasTable())
        return false;

    const ArchetypeTable& table = archetype->GetTable();
    if (chunkIndex >= table.GetChunkCount())
        return false;

    const std::size_t base = archetypeIndex * m_ChangedTypes.size();
    if (base + m_ChangedTypes.size() > m_ChangedColumns.size())
        return true; // filter set before refresh resolved columns — be conservative

    return table.AnyColumnChangedSince(
        chunkIndex,
        std::span<const int>(m_ChangedColumns.data() + base, m_ChangedTypes.size()), gate);
}

Archetype* CachedQuery::GetArchetype(std::size_t index) const
{
    if (index >= m_CachedArchetypes.size())
        return nullptr;
    return m_CachedArchetypes[index];
}

bool CachedQuery::ArchetypeHasComponent(std::size_t archetypeIndex, ComponentTypeId typeId) const
{
    auto* archetype = GetArchetype(archetypeIndex);
    if (!archetype || !archetype->GetSignature().Contains(typeId))
        return false;
    const ComponentRegistry::ComponentInfo* info = ComponentRegistry::GetComponentInfo(typeId);
    return !info || !archetype->GetSignature().Contains(ComponentDisabledTypeId(info->Name));
}

std::pair<uint32_t*, std::size_t> CachedQuery::GetChunkEntityIds(std::size_t archetypeIndex,
                                                                  std::size_t chunkIndex)
{
    auto* archetype = GetArchetype(archetypeIndex);
    if (!archetype || !archetype->HasTable())
        return {nullptr, 0};

    auto chunks = archetype->GetTable().GetChunks();
    if (chunkIndex >= chunks.size())
        return {nullptr, 0};

    auto& chunk = chunks[chunkIndex];
    uint32_t count = chunk.GetCount();
    if (count == 0)
        return {nullptr, 0};

    const EntityHandle* handles = chunk.GetEntityHandles();
    m_EntityIdScratch.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        m_EntityIdScratch[i] = handles[i].id;

    return {m_EntityIdScratch.data(), count};
}

std::pair<uint32_t*, std::size_t> CachedQuery::GetEntityIdSlice(std::size_t archetypeIndex,
                                                                std::size_t entityOffset,
                                                                std::size_t maxCount)
{
    auto* archetype = GetArchetype(archetypeIndex);
    if (!archetype || !archetype->HasTable())
        return {nullptr, 0};

    // Walk chunks with a running prefix count and copy only the requested
    // window. Collecting every entity handle in the archetype per call made
    // a sliced pass O(N^2) in copies plus an O(N) transient allocation.
    m_EntityIdScratch.clear();

    std::size_t toSkip = entityOffset;
    for (auto& chunk : archetype->GetTable().GetChunks())
    {
        const std::size_t count = chunk.GetCount();
        if (count == 0)
            continue;
        if (toSkip >= count)
        {
            toSkip -= count;
            continue;
        }

        const EntityHandle* handles = chunk.GetEntityHandles();
        for (std::size_t i = toSkip; i < count && m_EntityIdScratch.size() < maxCount; ++i)
            m_EntityIdScratch.push_back(handles[i].id);
        toSkip = 0;

        if (m_EntityIdScratch.size() == maxCount)
            break;
    }

    if (m_EntityIdScratch.empty())
        return {nullptr, 0};

    return {m_EntityIdScratch.data(), m_EntityIdScratch.size()};
}

} // namespace GameEngine::ECS
