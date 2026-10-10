// Chunk queries, the page's batched tier: ge_query_begin, ge_query_next_chunk and ge_query_end
// (Apps/WebLibrary/ts/src/abi.ts; design: workbench designs/web/web-api.md 3.4). A query walks
// every chunk of the entities that have all of its components and hands the page the chunk's
// columns as addresses in module memory, so one call serves up to a chunk of entities instead of
// one call per entity per field.
//
// The read/write split is the ECS's: a read column is handed out through the const chunk access
// and never stamped; a write column is a write grant, stamped at visit, so a Changed<T> system
// (the transform hierarchy for Transform) sees the page's writes.

#include "AbiQuery.h"

#include "AbiEntities.h"
#include "AbiErrors.h"
#include "PageReflection.h"

#include "ECS/CachedQuery.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Entity.h"
#include "ECS/IComponentHandler.h"
#include "ECS/World.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace GameEngine::WebLibrary
{

namespace
{

// More columns than a page query needs; bounds the out arrays the page allocates.
constexpr uint32_t kMaxQueryColumns = 16;

// One query signature, kept across frames: the CachedQuery re-scans the archetypes only after a
// structural change, so a page that runs the same query every frame allocates nothing.
struct CachedPageQuery
{
    ECS::World* World = nullptr;
    std::vector<uint64_t> ReadTypeIds;
    std::vector<uint64_t> WriteTypeIds;
    std::unique_ptr<ECS::CachedQuery> Query;
};

// The query a page is walking, between ge_query_begin and its end.
struct LiveQuery
{
    CachedPageQuery* Query = nullptr;
    std::size_t Archetype = 0;
    std::size_t Chunk = 0;
};

std::vector<std::unique_ptr<CachedPageQuery>>& CachedQueries()
{
    static std::vector<std::unique_ptr<CachedPageQuery>> queries;
    return queries;
}

LiveQuery g_Live;

bool SameTypeIds(const std::vector<uint64_t>& ids, std::span<const uint64_t> other)
{
    return std::equal(ids.begin(), ids.end(), other.begin(), other.end());
}

CachedPageQuery& FindOrCreateQuery(ECS::World& world, std::span<const uint64_t> readIds,
                                   std::span<const uint64_t> writeIds)
{
    for (const std::unique_ptr<CachedPageQuery>& cached : CachedQueries())
    {
        if (cached->World == &world && SameTypeIds(cached->ReadTypeIds, readIds) &&
            SameTypeIds(cached->WriteTypeIds, writeIds))
        {
            return *cached;
        }
    }
    auto cached = std::make_unique<CachedPageQuery>();
    cached->World = &world;
    cached->ReadTypeIds.assign(readIds.begin(), readIds.end());
    cached->WriteTypeIds.assign(writeIds.begin(), writeIds.end());
    std::vector<ECS::ComponentTypeId> required(readIds.begin(), readIds.end());
    required.insert(required.end(), writeIds.begin(), writeIds.end());
    cached->Query = std::make_unique<ECS::CachedQuery>(&world, std::move(required), std::vector<ECS::ComponentTypeId>{});
    CachedQueries().push_back(std::move(cached));
    return *CachedQueries().back();
}

// Every id names a page component, and none repeats across the two lists. At most
// kMaxQueryColumns ids, checked on the stack: a page runs its queries every frame.
bool ValidateTypeIds(std::span<const uint64_t> readIds, std::span<const uint64_t> writeIds)
{
    std::array<uint64_t, kMaxQueryColumns> sorted{};
    const auto allEnd = std::copy(writeIds.begin(), writeIds.end(),
                                  std::copy(readIds.begin(), readIds.end(), sorted.begin()));
    const std::span<uint64_t> all(sorted.begin(), allEnd);
    for (const uint64_t typeId : all)
    {
        if (!FindPageComponent(typeId))
        {
            SetLastError("ge_query_begin: {} is not a component this engine reflects; ge_reflection_json lists the "
                         "ones it does.",
                         typeId);
            return false;
        }
    }
    std::sort(all.begin(), all.end());
    if (std::adjacent_find(all.begin(), all.end()) != all.end())
    {
        SetLastError("ge_query_begin: a component is listed twice; name each once, as read or as write.");
        return false;
    }
    return true;
}

uint32_t Address(const void* pointer)
{
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer));
}

} // namespace

bool RefuseWhileQueryRuns(std::string_view call)
{
    if (!g_Live.Query)
        return false;
    SetLastError("{} was called while a query is walking the world's chunks: a structural change or a frame would "
                 "move the memory the query hands out. Make it after the query ends (forEachChunk returns; in the "
                 "ABI, ge_query_end or the ge_query_next_chunk that returns 0).",
                 call);
    return true;
}

void ReleaseQueries()
{
    g_Live = {};
    CachedQueries().clear();
}

} // namespace GameEngine::WebLibrary

using namespace GameEngine;
using namespace GameEngine::WebLibrary;

extern "C"
{

/// Starts a query over the entities that have every component of `readTypeIds` and
/// `writeTypeIds` (uint64 arrays), and writes each column's stride (the component's size in
/// bytes; reads first, then writes) to `outStrides`. One query runs at a time.
EMSCRIPTEN_KEEPALIVE int32_t ge_query_begin(const uint64_t* readTypeIds, uint32_t readCount,
                                            const uint64_t* writeTypeIds, uint32_t writeCount,
                                            uint32_t* outStrides)
{
    const AbiCallScope scope("ge_query_begin");
    if (scope.Refused())
        return kFailed;
    if (g_Live.Query)
    {
        SetLastError("ge_query_begin: another query is still running; a query cannot start inside another's "
                     "forEachChunk.");
        return kFailed;
    }
    ECS::World* world = WorldForCall("ge_query_begin");
    if (!world)
        return kFailed;
    // Each count is bounded before the sum, which would otherwise wrap past the stack arrays.
    if (readCount > kMaxQueryColumns || writeCount > kMaxQueryColumns || readCount + writeCount == 0 ||
        readCount + writeCount > kMaxQueryColumns || (readCount && !readTypeIds) || (writeCount && !writeTypeIds) ||
        !outStrides)
    {
        SetLastError("ge_query_begin takes 1 to {} components in all and an array for their strides (got {} read and {} "
                     "written).",
                     kMaxQueryColumns, readCount, writeCount);
        return kFailed;
    }
    const std::span<const uint64_t> readIds(readTypeIds, readCount);
    const std::span<const uint64_t> writeIds(writeTypeIds, writeCount);
    if (!ValidateTypeIds(readIds, writeIds))
        return kFailed;
    CachedPageQuery& query = FindOrCreateQuery(*world, readIds, writeIds);
    query.Query->Refresh();
    const uint32_t columnCount = readCount + writeCount;
    for (uint32_t i = 0; i < columnCount; ++i)
    {
        const uint64_t typeId = i < readCount ? readIds[i] : writeIds[i - readCount];
        const ECS::IComponentHandler* handler = ECS::ComponentRegistry::GetHandler(typeId);
        outStrides[i] = handler ? static_cast<uint32_t>(handler->GetComponentSize()) : 0u;
    }
    g_Live = LiveQuery{&query, 0, 0};
    return kOk;
}

/// Advances the running query to its next non-empty chunk. Writes the address of the chunk's
/// entity ids (uint32) and then each column's address (reads first, then writes) to
/// `outColumns`, and the chunk's entity count to `outCount`; returns 1. Returns 0, ending the
/// query, when every chunk has been visited. A write column is stamped as written.
EMSCRIPTEN_KEEPALIVE int32_t ge_query_next_chunk(uint32_t* outColumns, uint32_t* outCount)
{
    const AbiCallScope scope("ge_query_next_chunk");
    if (scope.Refused())
        return kFailed;
    if (!g_Live.Query)
    {
        SetLastError("ge_query_next_chunk was called with no query running; start one with ge_query_begin.");
        return kFailed;
    }
    if (!outColumns || !outCount)
    {
        SetLastError("ge_query_next_chunk needs arrays for the column addresses and the count.");
        return kFailed;
    }
    CachedPageQuery& query = *g_Live.Query;
    ECS::CachedQuery& cached = *query.Query;
    for (; g_Live.Archetype < cached.GetArchetypeCount(); ++g_Live.Archetype, g_Live.Chunk = 0)
    {
        ECS::Archetype* archetype = cached.GetArchetype(g_Live.Archetype);
        for (; archetype && g_Live.Chunk < archetype->GetChunkCount(); ++g_Live.Chunk)
        {
            const auto [entityIds, count] = cached.GetChunkEntityIds(g_Live.Archetype, g_Live.Chunk);
            if (count == 0)
                continue;
            outColumns[0] = Address(entityIds);
            const ECS::Archetype& readOnly = *archetype;
            uint32_t column = 1;
            for (const uint64_t typeId : query.ReadTypeIds)
                outColumns[column++] = Address(readOnly.GetChunkDataRaw(typeId, g_Live.Chunk).first);
            for (const uint64_t typeId : query.WriteTypeIds)
                outColumns[column++] = Address(archetype->GetChunkDataRaw(typeId, g_Live.Chunk).first);
            *outCount = static_cast<uint32_t>(count);
            ++g_Live.Chunk;
            return 1;
        }
    }
    g_Live = {};
    return 0;
}

/// Ends the running query before its last chunk (a page callback that threw); a no-op when none
/// runs.
EMSCRIPTEN_KEEPALIVE int32_t ge_query_end()
{
    const AbiCallScope scope("ge_query_end");
    if (scope.Refused())
        return kFailed;
    g_Live = {};
    return kOk;
}

} // extern "C"
