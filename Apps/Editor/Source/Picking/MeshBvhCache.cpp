#include "Picking/MeshBvhCache.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/ModelAsset.h"
#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Mathematics/Vector3.h"
#include "MeshPicking/MeshBvh.h"
#include "MeshPicking/PickTypes.h"

#include "Picking/MeshBvhDiskFormat.h"

namespace GameEngine::Editor::Picking
{

namespace
{

// Vertex layout assumption: Position is the first field (offset 0) and is
// three contiguous floats. We rely on this to view ModelAsset vertex data as
// Vector3 positions at sizeof(Vertex) stride without copying.
static_assert(offsetof(Vertex, Position) == 0,
              "MeshBvhCache assumes Vertex::Position is at offset 0");
static_assert(sizeof(Vertex::Position) == sizeof(Mathematics::Vector3),
              "MeshBvhCache assumes Vertex::Position is 3 contiguous floats");

struct Key
{
    GUID    Guid;
    uint32  Submesh = 0u;
    bool operator==(const Key& other) const noexcept
    {
        return Submesh == other.Submesh && Guid == other.Guid;
    }
};

struct KeyHash
{
    size_t operator()(const Key& k) const noexcept
    {
        // hash_combine: bare XOR with submesh*constant collapses to just
        // hash(guid) when submesh==0 (the common case), so two distinct
        // GUIDs that hash-collide can't be distinguished by submesh.
        size_t h = std::hash<GUID>{}(k.Guid);
        h ^= std::hash<uint32>{}(k.Submesh) + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

MeshPicking::MeshView ViewFromMesh(const Mesh& mesh)
{
    MeshPicking::MeshView v;
    if (mesh.Vertices.empty() || mesh.Indices.empty())
        return v;
    v.Positions    = reinterpret_cast<const Mathematics::Vector3*>(&mesh.Vertices[0].Position[0]);
    v.VertexStride = sizeof(Vertex);
    v.VertexCount  = static_cast<uint32>(mesh.Vertices.size());
    v.Indices      = mesh.Indices.data();
    v.IndexCount   = static_cast<uint32>(mesh.Indices.size());
    return v;
}

using BvhPtr = std::shared_ptr<const MeshPicking::MeshBvh>;

// Where a build reads and writes the disk cache: the project's cache root, which only the
// thread that asks for the build resolves, because it reads the asset registry. The build
// itself touches no engine service. The file's key is the geometry itself (ComputeGeometryHash),
// computed by the build.
struct DiskPlan
{
    std::filesystem::path CacheRoot;
    bool                  Valid = false;
};

DiskPlan PlanDisk(const GUID& guid, uint32 submesh, const Mesh& source)
{
    DiskPlan plan;
    const MeshPicking::MeshView view = ViewFromMesh(source);
    // Skinned meshes are cached too: the bind-pose BVH is the correct BVH for current
    // bind-pose-only picking. When deformed picking lands later, bumping kBvhBuilderVersion in
    // MeshBvhDiskFormat.h forces a re-bake.
    if (!view.IsValid() || view.TriangleCount() < kDiskCacheTriThreshold)
        return plan;
    const auto root = EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetCacheRoot(guid);
    if (!root.has_value())
    {
        Logger::Log::Info("[MeshBvh] disk skipped {} submesh {}: no cache root "
                          "(likely a primitive or unregistered asset)",
                          guid.ToString(), submesh);
        return plan;
    }
    plan.CacheRoot = *root;
    plan.Valid = true;
    return plan;
}

// What a build produced, and what to write to the disk cache once it is published.
struct BuildOutcome
{
    BvhPtr Bvh; // null for an empty or invalid mesh
    bool   NeedsPersist = false;
    uint64 GeometryHash = 0;
};

// The BVH for `view`: loaded from the disk cache when the plan has a file for this exact
// geometry, else built. Runs on any thread.
BuildOutcome LoadOrBuild(const GUID& guid, uint32 submesh, const MeshPicking::MeshView& view, const DiskPlan& plan)
{
    BuildOutcome outcome;
    if (!view.IsValid())
        return outcome;
    if (plan.Valid)
    {
        outcome.GeometryHash = ComputeGeometryHash(view);
        auto loaded = std::make_shared<MeshPicking::MeshBvh>();
        if (TryLoadBvh(plan.CacheRoot, guid, submesh, outcome.GeometryHash, view, *loaded) && !loaded->IsEmpty())
        {
            Logger::Log::Info("[MeshBvh] disk hit {} submesh {} ({} tris)", guid.ToString(), submesh,
                              view.TriangleCount());
            outcome.Bvh = std::move(loaded);
            return outcome;
        }
    }
    auto built = std::make_shared<MeshPicking::MeshBvh>(MeshPicking::MeshBvh::Build(view));
    if (built->IsEmpty())
        return outcome;
    outcome.Bvh = std::move(built);
    // Persist after publishing: readers never wait on disk I/O.
    outcome.NeedsPersist = plan.Valid;
    return outcome;
}

// The positions and indices a build job reads: its own copy, so the asset can reload or
// unload on the main thread while the job runs.
struct OwnedGeometry
{
    std::vector<Mathematics::Vector3> Positions;
    std::vector<uint32>               Indices;
};

OwnedGeometry CopyGeometry(const Mesh& mesh)
{
    OwnedGeometry geometry;
    geometry.Positions.reserve(mesh.Vertices.size());
    for (const Vertex& vertex : mesh.Vertices)
        geometry.Positions.emplace_back(vertex.Position[0], vertex.Position[1], vertex.Position[2]);
    geometry.Indices.assign(mesh.Indices.begin(), mesh.Indices.end());
    return geometry;
}

MeshPicking::MeshView ViewFromGeometry(const OwnedGeometry& geometry)
{
    MeshPicking::MeshView v;
    if (geometry.Positions.empty() || geometry.Indices.empty())
        return v;
    v.Positions    = geometry.Positions.data();
    v.VertexStride = sizeof(Mathematics::Vector3);
    v.VertexCount  = static_cast<uint32>(geometry.Positions.size());
    v.Indices      = geometry.Indices.data();
    v.IndexCount   = static_cast<uint32>(geometry.Indices.size());
    return v;
}

}

struct MeshBvhCache::Impl
{
    // One build per key at a time. Whoever starts the build runs it and publishes. A build a
    // job was handed is started by the job, or, when a GetOrBuild needs the key before the job
    // was reached, by that GetOrBuild on its own thread; everyone else either waits on the
    // flight's future (GetOrBuild) or reports the key pending (TryGet).
    struct Claim
    {
        BvhPtr                                 Cached;
        bool                                   IsCached = false;
        // True when the key already had a build in flight; the fields below then name it.
        bool                                   JoinedFlight = false;
        std::shared_future<BvhPtr>             Future;
        std::shared_ptr<std::promise<BvhPtr>>  Done;
        // Set by whoever starts the build (compare-exchange false -> true); a new claim's
        // build is unstarted until its claimant starts it.
        std::shared_ptr<std::atomic<bool>>     Started;
        uint64                                 Ticket = 0;
    };

    // A build handed to the job system. It owns everything it reads.
    struct BuildJob
    {
        Key                                    BuildKey;
        uint64                                 Ticket = 0;
        DiskPlan                               Disk;
        OwnedGeometry                          Geometry;
        std::shared_ptr<std::promise<BvhPtr>>  Done;
        std::shared_ptr<std::atomic<bool>>     Started;
        double                                 CopyMs = 0.0;
    };

    // Claims an unstarted build for the caller: true for exactly one caller per flight.
    static bool TryStart(std::atomic<bool>& started)
    {
        bool expected = false;
        return started.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }

    mutable std::shared_mutex                              Mutex;
    // A null entry is a mesh with no BVH (empty or invalid), remembered so it is not rebuilt.
    std::unordered_map<Key, BvhPtr, KeyHash>               Map;
    // The build in flight for a key, named by the ticket its claim drew. Clear and
    // InvalidateAsset drop the entries they invalidate, so the next lookup claims a new build of
    // the new geometry, and the dropped build, finding its ticket gone, wakes its own waiters but
    // publishes nothing.
    struct Flight
    {
        std::shared_future<BvhPtr>             Future;
        std::shared_ptr<std::promise<BvhPtr>>  Done;
        std::shared_ptr<std::atomic<bool>>     Started;
        uint64                                 Ticket = 0;
    };
    std::unordered_map<Key, Flight, KeyHash>               InFlight;
    uint64                                                 NextTicket = 0;

    Claim ClaimBuild(const Key& key);
    bool Publish(const Key& key, uint64 ticket, const BvhPtr& result, std::promise<BvhPtr>& done);
    void Abandon(const Key& key, uint64 ticket, std::promise<BvhPtr>& done, std::exception_ptr error);
    // Under Mutex: whether `ticket` still names the build in flight for `key`.
    bool OwnsFlight(const Key& key, uint64 ticket) const;
    void RunBuildJob(BuildJob& job);
};

MeshBvhCache& MeshBvhCache::Instance()
{
    static MeshBvhCache s_Instance;
    return s_Instance;
}

MeshBvhCache::Impl& MeshBvhCache::GetImpl()
{
    static Impl s_Impl;
    return s_Impl;
}

MeshBvhCache::Impl::Claim MeshBvhCache::Impl::ClaimBuild(const Key& key)
{
    Claim claim;
    std::unique_lock lock(Mutex);
    if (auto cached = Map.find(key); cached != Map.end())
    {
        claim.Cached = cached->second;
        claim.IsCached = true;
        return claim;
    }
    if (auto inflight = InFlight.find(key); inflight != InFlight.end())
    {
        claim.JoinedFlight = true;
        claim.Future = inflight->second.Future;
        claim.Done = inflight->second.Done;
        claim.Started = inflight->second.Started;
        claim.Ticket = inflight->second.Ticket;
        return claim;
    }
    claim.Done = std::make_shared<std::promise<BvhPtr>>();
    claim.Future = claim.Done->get_future().share();
    claim.Started = std::make_shared<std::atomic<bool>>(false);
    claim.Ticket = ++NextTicket;
    InFlight.emplace(key, Flight{claim.Future, claim.Done, claim.Started, claim.Ticket});
    return claim;
}

bool MeshBvhCache::Impl::OwnsFlight(const Key& key, uint64 ticket) const
{
    const auto inflight = InFlight.find(key);
    return inflight != InFlight.end() && inflight->second.Ticket == ticket;
}

// Publishes a finished build to the map FIRST, then wakes its waiters. Returns whether the
// map took it: false when the asset was invalidated while it built.
bool MeshBvhCache::Impl::Publish(const Key& key, uint64 ticket, const BvhPtr& result,
                                 std::promise<BvhPtr>& done)
{
    bool published = false;
    {
        std::unique_lock lock(Mutex);
        if (OwnsFlight(key, ticket))
        {
            Map[key] = result;
            InFlight.erase(key);
            published = true;
        }
    }
    done.set_value(result);
    return published;
}

// A build that threw: frees the key for a later attempt and hands the exception to the
// waiters, which turn it into a miss.
void MeshBvhCache::Impl::Abandon(const Key& key, uint64 ticket, std::promise<BvhPtr>& done,
                                 std::exception_ptr error)
{
    {
        std::unique_lock lock(Mutex);
        if (OwnsFlight(key, ticket))
            InFlight.erase(key);
    }
    try { done.set_exception(error); } catch (...) {}
}

void MeshBvhCache::Impl::RunBuildJob(BuildJob& job)
{
    // A GetOrBuild that needed the key before this job was reached builds it on its own thread.
    if (!TryStart(*job.Started))
        return;
    const auto start = std::chrono::steady_clock::now();
    const MeshPicking::MeshView view = ViewFromGeometry(job.Geometry);
    BuildOutcome outcome;
    try
    {
        outcome = LoadOrBuild(job.BuildKey.Guid, job.BuildKey.Submesh, view, job.Disk);
    }
    catch (const std::exception& error)
    {
        // Remembered as a mesh with no BVH, so every hover does not start the same failing
        // build again; invalidating the asset retries.
        Logger::Log::Error("[MeshBvh] building {} submesh {} failed: {}", job.BuildKey.Guid.ToString(),
                           job.BuildKey.Submesh, error.what());
    }
    catch (...)
    {
        Logger::Log::Error("[MeshBvh] building {} submesh {} failed", job.BuildKey.Guid.ToString(),
                           job.BuildKey.Submesh);
    }
    const double jobMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    Logger::Log::Info("[MeshBvh] job for {} submesh {} ({} tris): {:.1f} ms on a worker after a {:.1f} ms copy "
                      "on the requesting thread",
                      job.BuildKey.Guid.ToString(), job.BuildKey.Submesh, view.TriangleCount(), jobMs, job.CopyMs);
    if (Publish(job.BuildKey, job.Ticket, outcome.Bvh, *job.Done) && outcome.NeedsPersist)
        PersistBvh(job.Disk.CacheRoot, job.BuildKey.Guid, job.BuildKey.Submesh, outcome.GeometryHash, *outcome.Bvh);
}

MeshBvhLookup MeshBvhCache::TryGet(const GUID& guid, uint32 submesh, const Mesh& source)
{
    Impl& impl = GetImpl();
    const Key key{guid, submesh};
    Impl::Claim claim = impl.ClaimBuild(key);
    if (claim.IsCached)
        return {claim.Cached, false};
    if (claim.JoinedFlight)
        return {nullptr, true};

    auto job = std::make_shared<Impl::BuildJob>();
    try
    {
        const auto start = std::chrono::steady_clock::now();
        job->BuildKey = key;
        job->Ticket = claim.Ticket;
        job->Done = claim.Done;
        job->Started = claim.Started;
        job->Disk = PlanDisk(guid, submesh, source);
        job->Geometry = CopyGeometry(source);
        job->CopyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    catch (...)
    {
        // A GetOrBuild may have started the build in the meantime; it then publishes.
        if (Impl::TryStart(*claim.Started))
            impl.Abandon(key, claim.Ticket, *claim.Done, std::current_exception());
        return {nullptr, false};
    }
    // The job reads only what it owns and the process-lifetime cache, so it needs no join: the
    // job system runs every queued bare task before it shuts down.
    EngineCore::GetInstance().GetJobSystem().EnqueueWork([job]() { GetImpl().RunBuildJob(*job); },
                                                         JobSystem::JobPriority::Background);
    return {nullptr, true};
}

std::shared_ptr<const MeshPicking::MeshBvh> MeshBvhCache::GetOrBuild(const GUID& guid,
                                                                    uint32 submesh,
                                                                    const Mesh& source)
{
    Impl& impl = GetImpl();
    const Key key{guid, submesh};
    Impl::Claim claim = impl.ClaimBuild(key);
    if (claim.IsCached)
        return claim.Cached;

    // The key's build is this caller's when it claimed the key now, or when the key's job has
    // not started yet: a caller that waited for that job would wait for every job queued ahead
    // of it on the Background lane. The job then finds the build started and returns. A build
    // that has started is waited for, which is bounded by one build; any exception
    // (broken_promise, build failure) becomes nullptr so the cache surface never throws and
    // picking treats it as a miss.
    if (!Impl::TryStart(*claim.Started))
    {
        try { return claim.Future.get(); }
        catch (...) { return nullptr; }
    }

    DiskPlan plan;
    BuildOutcome outcome;
    try
    {
        plan = PlanDisk(guid, submesh, source);
        outcome = LoadOrBuild(guid, submesh, ViewFromMesh(source), plan);
    }
    catch (...)
    {
        // Not rethrown: GetOrBuild is contracted to never throw so picking entry points stay
        // panic-free.
        impl.Abandon(key, claim.Ticket, *claim.Done, std::current_exception());
        return nullptr;
    }
    // A concurrent InvalidateAsset can race with the persist; in the worst case it leaves a
    // .meshbvh keyed by the old geometry's hash, which the new geometry never matches.
    if (impl.Publish(key, claim.Ticket, outcome.Bvh, *claim.Done) && outcome.NeedsPersist)
        PersistBvh(plan.CacheRoot, guid, submesh, outcome.GeometryHash, *outcome.Bvh);
    return outcome.Bvh;
}

void MeshBvhCache::InvalidateAsset(const GUID& guid,
                                   const std::filesystem::path& assetPath)
{
    Impl& impl = GetImpl();
    {
        std::unique_lock lock(impl.Mutex);
        std::erase_if(impl.Map, [&guid](const auto& entry) { return entry.first.Guid == guid; });
        std::erase_if(impl.InFlight, [&guid](const auto& entry) { return entry.first.Guid == guid; });
    }

    // Disk cleanup. The AssetDestroyed event arrives after the GUID has
    // been erased from m_Assets, so TryGetCacheRoot(guid) returns nullopt
    // for it. Fall through to the path-based overload, which only needs
    // the source's root containment to resolve.
    auto& mgr = EngineCore::GetInstance().GetAssetManager();
    auto& registry = mgr.GetRegistry();
    auto root = registry.TryGetCacheRoot(guid);
    if (!root.has_value() && !assetPath.empty())
        root = registry.TryGetCacheRoot(assetPath);
    if (!root.has_value())
        return;  // Source can't be resolved; orphan stays for startup sweep.
    InvalidateBvhCacheFiles(*root, guid);
}

void MeshBvhCache::Clear()
{
    Impl& impl = GetImpl();
    std::unique_lock lock(impl.Mutex);
    impl.Map.clear();
    impl.InFlight.clear();
}

size_t MeshBvhCache::Size() const
{
    Impl& impl = GetImpl();
    std::shared_lock lock(impl.Mutex);
    return impl.Map.size();
}

}
