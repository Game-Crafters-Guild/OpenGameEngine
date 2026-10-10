#include "TerrainECS/TerrainService.h"
#include "TerrainCollisionReadiness.h"

#include "CBTTerrain/CBTPlanetShading.h" // SampleSphereSculptByDir (brush cursor sculpt sample)
#include "CBTTerrain/SphereSculptSerialization.h" // .tsculpt blob codec (persistence)
#include "PhysicsECS/HeightFieldDataProvider.h"
#include "Terrain/CDLODSelection.h"
#include "TerrainECS/RawHeightmap.h"
#include "TerrainECS/TerrainSizingPlan.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/HeightPageStoreLoader.h"
#include "Components/Terrain/Terrain.h"
#include "Core/Engine.h"
#include "Assets/AssetManager.h"
#include "Assets/BinaryAsset.h"
#include "JobSystem/JobChannel.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace GameEngine::TerrainECS
{

// Default planet radius the sculpt page store is configured to at Initialize (the canonical
// PlanetColliderParams / demo default) until the render/modifier path pushes the real radius.
static constexpr float32 kDefaultPlanetSculptRadius = 2000.0f;

void ResetSplatmap(
    const Terrain::HeightfieldData& heightfield,
    std::vector<uint8>& outSplatmap,
    uint32& outWidth, uint32& outHeight)
{
    const uint32 w = heightfield.GetWidth();
    const uint32 h = heightfield.GetHeight();
    if (w == 0 || h == 0)
        return;

    outWidth = w;
    outHeight = h;
    outSplatmap.assign(static_cast<size_t>(w) * h * 4, 0);
}

void ResetSplatmapRegion(
    const Terrain::HeightfieldData& heightfield,
    std::vector<uint8>& splatmap,
    int32 minX, int32 minZ, int32 maxX, int32 maxZ)
{
    const uint32 w = heightfield.GetWidth();
    const uint32 h = heightfield.GetHeight();
    if (w == 0 || h == 0 || splatmap.size() != static_cast<size_t>(w) * h * 4)
        return;

    const uint32 x0 = static_cast<uint32>(std::max(minX, 0));
    const uint32 z0 = static_cast<uint32>(std::max(minZ, 0));
    const uint32 x1 = static_cast<uint32>(std::min(maxX, static_cast<int32>(w) - 1));
    const uint32 z1 = static_cast<uint32>(std::min(maxZ, static_cast<int32>(h) - 1));
    if (x0 > x1 || z0 > z1)
        return;

    const size_t spanBytes = static_cast<size_t>(x1 - x0 + 1) * 4;
    for (uint32 z = z0; z <= z1; ++z)
        std::memset(splatmap.data() + (static_cast<size_t>(z) * w + x0) * 4, 0, spanBytes);
}

// IEEE 754 float32 to float16 conversion (round-to-nearest-even).
static uint16 FloatToHalf(float32 value)
{
    uint32 f;
    std::memcpy(&f, &value, sizeof(f));
    uint32 sign = (f >> 16) & 0x8000;
    int32 exp = static_cast<int32>((f >> 23) & 0xFF) - 127 + 15;
    uint32 frac = f & 0x7FFFFF;
    if (exp <= 0)
        return static_cast<uint16>(sign); // flush to zero
    if (exp >= 31)
        return static_cast<uint16>(sign | 0x7C00); // infinity
    return static_cast<uint16>(sign | (static_cast<uint32>(exp) << 10) | (frac >> 13));
}

void GenerateNormalmapFromHeightfield(
    const Terrain::HeightfieldData& heightfield,
    float32 worldSizeX, float32 worldSizeZ,
    float32 heightScale,
    std::vector<uint8>& outNormalmap,
    uint32& outWidth, uint32& outHeight,
    const Terrain::HeightfieldData* neighborLeft,
    const Terrain::HeightfieldData* neighborRight,
    const Terrain::HeightfieldData* neighborUp,
    const Terrain::HeightfieldData* neighborDown)
{
    const uint32 w = heightfield.GetWidth();
    const uint32 h = heightfield.GetHeight();
    if (w == 0 || h == 0)
        return;

    outWidth = w;
    outHeight = h;
    // R16G16_FLOAT: 4 bytes per texel (two float16 values).
    outNormalmap.resize(static_cast<size_t>(w) * h * 4);

    // Divide spacing by HeightScale so the central-difference gradient
    // reflects actual world-space height: dh/dx = HeightScale * (hR - hL) / spacing.
    const float32 hs = std::max(heightScale, 0.001f);
    const float32 spacingX = worldSizeX / (static_cast<float32>(w > 1 ? w - 1 : 1) * hs);
    const float32 spacingZ = worldSizeZ / (static_cast<float32>(h > 1 ? h - 1 : 1) * hs);

    auto* out16 = reinterpret_cast<uint16*>(outNormalmap.data());

    for (uint32 z = 0; z < h; ++z)
    {
        for (uint32 x = 0; x < w; ++x)
        {
            auto normal = heightfield.ComputeNormalWithNeighbor(
                static_cast<int32>(x), static_cast<int32>(z),
                spacingX, spacingZ,
                neighborLeft, neighborRight, neighborUp, neighborDown);

            const size_t idx = (static_cast<size_t>(z) * w + x) * 2;
            out16[idx + 0] = FloatToHalf(normal.x);
            out16[idx + 1] = FloatToHalf(normal.z);
        }
    }
}

void GenerateNormalmapRegionFromHeightfield(
    const Terrain::HeightfieldData& heightfield,
    float32 worldSizeX, float32 worldSizeZ,
    float32 heightScale,
    std::vector<uint8>& normalmap,
    int32 minX, int32 minZ, int32 maxX, int32 maxZ,
    const Terrain::HeightfieldData* neighborLeft,
    const Terrain::HeightfieldData* neighborRight,
    const Terrain::HeightfieldData* neighborUp,
    const Terrain::HeightfieldData* neighborDown)
{
    const uint32 w = heightfield.GetWidth();
    const uint32 h = heightfield.GetHeight();
    if (w == 0 || h == 0 || normalmap.size() != static_cast<size_t>(w) * h * 4)
        return;

    const uint32 x0 = static_cast<uint32>(std::max(minX, 0));
    const uint32 z0 = static_cast<uint32>(std::max(minZ, 0));
    const uint32 x1 = static_cast<uint32>(std::min(maxX, static_cast<int32>(w) - 1));
    const uint32 z1 = static_cast<uint32>(std::min(maxZ, static_cast<int32>(h) - 1));
    if (x0 > x1 || z0 > z1)
        return;

    // Same spacing math as the full generator (see comment there).
    const float32 hs = std::max(heightScale, 0.001f);
    const float32 spacingX = worldSizeX / (static_cast<float32>(w > 1 ? w - 1 : 1) * hs);
    const float32 spacingZ = worldSizeZ / (static_cast<float32>(h > 1 ? h - 1 : 1) * hs);

    auto* out16 = reinterpret_cast<uint16*>(normalmap.data());

    for (uint32 z = z0; z <= z1; ++z)
    {
        for (uint32 x = x0; x <= x1; ++x)
        {
            auto normal = heightfield.ComputeNormalWithNeighbor(
                static_cast<int32>(x), static_cast<int32>(z),
                spacingX, spacingZ,
                neighborLeft, neighborRight, neighborUp, neighborDown);

            const size_t idx = (static_cast<size_t>(z) * w + x) * 2;
            out16[idx + 0] = FloatToHalf(normal.x);
            out16[idx + 1] = FloatToHalf(normal.z);
        }
    }
}

namespace
{
// Source coordinate + interpolation weight for one destination texel of a
// dstDim upsample of a srcDim source. dst 0 maps to src 0 and dst (dstDim-1) to
// src (srcDim-1) exactly (fx==0 at both ends), so shared tile edges stay bit-
// identical. srcHi is clamped so the last texel does not read out of bounds.
struct UpsampleTap
{
    uint32 srcLo;
    uint32 srcHi;
    float32 frac;
};

UpsampleTap ComputeUpsampleTap(uint32 dstIndex, uint32 srcDim, uint32 dstDim)
{
    const float32 pos = static_cast<float32>(dstIndex) *
                        static_cast<float32>(srcDim - 1) / static_cast<float32>(dstDim - 1);
    const uint32 lo = static_cast<uint32>(pos);
    const uint32 hi = std::min(lo + 1u, srcDim - 1u);
    return {lo, hi, pos - static_cast<float32>(lo)};
}
} // namespace

void UpsampleHeightfieldBilinear(const float32* src, uint32 srcDim,
                                 std::vector<float32>& out, uint32 dstDim)
{
    if (!src || srcDim < 2 || dstDim < 2)
        return;
    out.resize(static_cast<size_t>(dstDim) * dstDim);

    std::vector<UpsampleTap> taps(dstDim);
    for (uint32 i = 0; i < dstDim; ++i)
        taps[i] = ComputeUpsampleTap(i, srcDim, dstDim);

    for (uint32 z = 0; z < dstDim; ++z)
    {
        const UpsampleTap& tz = taps[z];
        const float32* row0 = src + static_cast<size_t>(tz.srcLo) * srcDim;
        const float32* row1 = src + static_cast<size_t>(tz.srcHi) * srcDim;
        float32* dstRow = out.data() + static_cast<size_t>(z) * dstDim;
        for (uint32 x = 0; x < dstDim; ++x)
        {
            const UpsampleTap& tx = taps[x];
            const float32 a = row0[tx.srcLo] + (row0[tx.srcHi] - row0[tx.srcLo]) * tx.frac;
            const float32 b = row1[tx.srcLo] + (row1[tx.srcHi] - row1[tx.srcLo]) * tx.frac;
            dstRow[x] = a + (b - a) * tz.frac;
        }
    }
}

void UpsampleSplatmapBilinear(const uint8* src, uint32 srcDim,
                              std::vector<uint8>& out, uint32 dstDim)
{
    if (!src || srcDim < 2 || dstDim < 2)
        return;
    out.resize(static_cast<size_t>(dstDim) * dstDim * 4);

    std::vector<UpsampleTap> taps(dstDim);
    for (uint32 i = 0; i < dstDim; ++i)
        taps[i] = ComputeUpsampleTap(i, srcDim, dstDim);

    for (uint32 z = 0; z < dstDim; ++z)
    {
        const UpsampleTap& tz = taps[z];
        const uint8* row0 = src + static_cast<size_t>(tz.srcLo) * srcDim * 4;
        const uint8* row1 = src + static_cast<size_t>(tz.srcHi) * srcDim * 4;
        uint8* dstRow = out.data() + static_cast<size_t>(z) * dstDim * 4;
        for (uint32 x = 0; x < dstDim; ++x)
        {
            const UpsampleTap& tx = taps[x];
            const uint32 lo0 = tx.srcLo * 4, hi0 = tx.srcHi * 4;
            for (uint32 c = 0; c < 4; ++c)
            {
                const float32 a = static_cast<float32>(row0[lo0 + c]) +
                    (static_cast<float32>(row0[hi0 + c]) - static_cast<float32>(row0[lo0 + c])) * tx.frac;
                const float32 b = static_cast<float32>(row1[lo0 + c]) +
                    (static_cast<float32>(row1[hi0 + c]) - static_cast<float32>(row1[lo0 + c])) * tx.frac;
                const float32 v = a + (b - a) * tz.frac;
                dstRow[x * 4 + c] = static_cast<uint8>(std::clamp(v + 0.5f, 0.0f, 255.0f));
            }
        }
    }
}

CoarsePatchRect ComputeCoarsePatchRect(uint32 regionDim, const CoarsePatchNeighbors& n)
{
    CoarsePatchRect r;
    if (regionDim < 2)
        return r;
    // Drop the shared boundary texel on any side whose neighbor is resident Full —
    // that texel belongs to the Full neighbor's edge (exact + modifiers).
    r.X0 = n.LeftFull ? 1u : 0u;
    r.Z0 = n.TopFull ? 1u : 0u;
    const uint32 x1 = n.RightFull ? (regionDim - 1u) : regionDim;   // exclusive
    const uint32 z1 = n.BottomFull ? (regionDim - 1u) : regionDim;  // exclusive
    r.Width = (x1 > r.X0) ? (x1 - r.X0) : 0u;
    r.Height = (z1 > r.Z0) ? (z1 - r.Z0) : 0u;
    return r;
}

void TerrainData::ResetSplatmapAndCommitRange()
{
    // The range is still computed here even though the reset does not read it:
    // it is the domain the HeightNormalized rule conditions normalize against,
    // and region regens are only exact while it is unchanged.
    float32 minH = 1e30f;
    float32 maxH = -1e30f;
    const uint32 w = Heightfield.GetWidth();
    const uint32 h = Heightfield.GetHeight();
    for (uint32 z = 0; z < h; ++z)
        for (uint32 x = 0; x < w; ++x)
        {
            const float32 s = Heightfield.GetSample(x, z);
            minH = std::min(minH, s);
            maxH = std::max(maxH, s);
        }
    if (w == 0 || h == 0)
    {
        minH = 0.0f;
        maxH = 0.0f;
    }

    ResetSplatmap(Heightfield, Splatmap, SplatmapWidth, SplatmapHeight);
    SplatBakeMinH = minH;
    SplatBakeMaxH = maxH;
    SplatBakeRangeValid = true;
    SplatmapDirty = true;
    // Every texel was reset against the (possibly new) global range — the GPU
    // upload must be full-texture, never a height-rect band.
    SplatmapFullDirty = true;
}

std::unique_ptr<TerrainService> TerrainService::s_Instance;
uint64 TerrainService::s_Generation = 0;

TerrainService::TerrainService() = default;
TerrainService::~TerrainService() = default;

JobSystem::JobChannel& TerrainService::BakeStoreChannel(JobSystem::WorkStealingThreadPool& jobSystem)
{
    if (!m_BakeStoreChannel)
    {
        m_BakeStoreJobSystem = &jobSystem;
        m_BakeStoreChannel = std::make_unique<JobSystem::JobChannel>(
            jobSystem, JobSystem::JobChannelDesc{.Name = "Terrain bake store", .MaxRunning = 1});
    }
    assert(m_BakeStoreJobSystem == &jobSystem &&
           "TerrainService: the bake store channel serves one job system; a second one passed it");
    return *m_BakeStoreChannel;
}

TerrainHeightPages& TerrainService::GetHeightPages()
{
    if (!m_HeightPages)
    {
        HeightPageProfile profile = PlatformHeightPageProfile();
        if (const char* overrideSlots = std::getenv("GE_TERRAIN_PAGE_SLOTS"))
            profile.Slots = std::max(1u, static_cast<uint32>(std::strtoul(overrideSlots, nullptr, 10)));
        // No reader threads: the residency reads cooked pages on Background jobs, as it evaluates
        // generated ones.
        m_HeightPages = std::make_unique<TerrainHeightPages>(&EngineCore::GetInstance().GetJobSystem(), nullptr, profile);
    }
    return *m_HeightPages;
}

HeightPageStoreLoader& TerrainService::GetHeightPageStores()
{
    if (!m_HeightPageStores)
        m_HeightPageStores = std::make_unique<HeightPageStoreLoader>();
    return *m_HeightPageStores;
}

void TerrainService::SetHeightPageStoreLocation(HeightPageStoreLocation location)
{
    GetHeightPageStores().SetLocation(std::move(location));
}

void TerrainService::Initialize()
{
    assert(!s_Instance && "TerrainService already initialized");
    s_Instance = std::make_unique<TerrainService>();
    ++s_Generation;

    // Configure the sculpt page store to the default planet radius so a direct dab (tests, or an
    // edit before the render/modifier path pushes the real radius) has a working virtual grid. The
    // render + modifier paths call ConfigurePlanetSculpt(actualRadius) before their first edit, which
    // reconfigures the dim (frozen once content exists).
    s_Instance->ConfigurePlanetSculpt(kDefaultPlanetSculptRadius);

    // Hot-reload lane for stamp masks and base heightmaps. Initialize runs
    // from EnableRenderingLoop, after the AssetManager exists; standalone
    // (test) construction leaves the lane inactive.
    s_Instance->InstallAssetReloadInvalidators();

    // Report terrain whose collider is still to be provisioned, so a consumer
    // asking whether heightfield collision is current waits for it.
    PhysicsECS::AddPendingHeightFieldSource(&FindPendingTerrainCollision);

    // Register as the heightfield data provider for PhysicsInitSystem. The
    // handle is either a single-terrain slot index or a per-tile physics handle
    // (kTilePhysicsHandleBit set); both resolve to a heightfield + version +
    // dirty-region log, so the region-scoped in-place collider path is
    // identical for single terrains and tiles.
    PhysicsECS::SetHeightFieldDataProvider([](uint32 handle, uint32 generation, uint64 sinceVersion) -> PhysicsECS::HeightFieldData
    {
        auto* svc = TerrainService::TryGet();
        if (!svc)
            return {};

        // Resolve the raw sample grid + version + dirty log for whichever addressing
        // scheme the handle carries: a per-tile heightfield, a per-face planet collider
        // buffer, or a single-terrain heightfield. All three feed the same region tail.
        const float32* samples = nullptr;
        uint32 sampleCount = 0;
        uint64 version = 0;
        const DirtyRegionLog* dirtyLog = nullptr;
        if (handle & TerrainService::kTilePhysicsHandleBit)
        {
            if (TerrainTileData* tile = svc->ResolveTilePhysicsTile(handle, generation);
                tile && !tile->Heightfield.IsEmpty())
            {
                samples = tile->Heightfield.GetRawSamples();
                sampleCount = std::min(tile->Heightfield.GetWidth(), tile->Heightfield.GetHeight());
                version = tile->HeightfieldVersion;
                dirtyLog = &tile->HeightfieldDirtyLog;
            }
        }
        else if (handle & TerrainService::kPlanetFacePhysicsHandleBit)
        {
            if (const auto* face = svc->ResolvePlanetFaceCollider(handle, generation))
            {
                samples = face->Samples.data();
                sampleCount = face->Dim;
                version = face->Version;
                dirtyLog = &face->DirtyLog;
            }
        }
        else if (TerrainData* data = svc->GetTerrainData(TerrainHandle{handle, generation});
                 data && !data->Heightfield.IsEmpty())
        {
            samples = data->Heightfield.GetRawSamples();
            sampleCount = std::min(data->Heightfield.GetWidth(), data->Heightfield.GetHeight());
            version = data->HeightfieldVersion;
            dirtyLog = &data->HeightfieldDirtyLog;
        }

        if (!samples || sampleCount < 4 || version == 0)
            return {};

        PhysicsECS::HeightFieldData out;
        out.samples = samples;
        out.sampleCount = sampleCount;
        out.version = version;

        // Physics keeps its own cursor into the region log (its shape's
        // last-built version); the union of everything newer is the region
        // its collider is missing.
        DirtyRegionLog::Region region;
        if (dirtyLog && dirtyLog->CollectSince(sinceVersion, region))
        {
            const int32 gridN = static_cast<int32>(out.sampleCount);
            region.MinX = std::clamp(region.MinX, 0, gridN);
            region.MinZ = std::clamp(region.MinZ, 0, gridN);
            region.MaxX = std::clamp(region.MaxX, 0, gridN);
            region.MaxZ = std::clamp(region.MaxZ, 0, gridN);
            if (!region.IsEmpty())
            {
                out.hasRegion = true;
                out.regionMinX = region.MinX;
                out.regionMinZ = region.MinZ;
                out.regionMaxX = region.MaxX;
                out.regionMaxZ = region.MaxZ;
            }
        }
        return out;
    });
}

void TerrainService::Shutdown()
{
    PhysicsECS::RemovePendingHeightFieldSource(&FindPendingTerrainCollision);
    PhysicsECS::SetHeightFieldDataProvider(nullptr);
    s_Instance.reset();
    ++s_Generation;
}

TerrainService& TerrainService::Get()
{
    assert(s_Instance && "TerrainService not initialized");
    return *s_Instance;
}

TerrainService* TerrainService::TryGet()
{
    return s_Instance.get();
}

bool TerrainService::IsInitialized()
{
    return s_Instance != nullptr;
}

uint64 TerrainService::GetGeneration()
{
    return s_Generation;
}

// ---- Base-source heightmap assets + asset-reload lane ----

void TerrainService::InstallAssetReloadInvalidators()
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;

    auto& dispatcher = engine.GetAssetManager().GetEventDispatcher();

    // ContentEvents: these caches decode straight from disk, so a plain file
    // edit dispatches AssetModified — never AssetReloaded, which fires only
    // for assets resident in the AssetManager's loaded set (our one-shot
    // TextureAsset / HeightfieldData decodes never enter it) — and
    // AssetCreated re-arms negative-cached decode failures when the file
    // appears. Handlers only ENQUEUE: every event here can fire on the
    // file-watcher thread; the modifier system drains on the main thread and
    // evicts the decoded caches there.
    //
    // TRACKED: AssetEventDispatcher::RemoveCallback doesn't fence an
    // in-flight dispatch on another thread, so a shutdown window exists where
    // a watcher-thread event can invoke this handler mid-teardown. Engine-wide
    // dispatcher flaw (every subscriber shares it) — not closable locally.
    m_TextureReloadInvalidator = AssetReloadInvalidator(
        dispatcher, AssetType::Texture,
        [this](const GUID& guid) { EnqueueAssetInvalidation(guid); },
        AssetReloadInvalidator::EventSet::ContentEvents);

    m_HeightmapReloadInvalidator = AssetReloadInvalidator(
        dispatcher, AssetType::TerrainHeightmap,
        [this](const GUID& guid) { EnqueueAssetInvalidation(guid); },
        AssetReloadInvalidator::EventSet::ContentEvents);

    // Zone payloads decode straight from disk too, so a .tzone edited on disk
    // (or reverted by another tool) evicts the resident payload and re-bakes.
    m_ZonePayloadReloadInvalidator = AssetReloadInvalidator(
        dispatcher, AssetType::TerrainZoneData,
        [this](const GUID& guid) { EnqueueAssetInvalidation(guid); },
        AssetReloadInvalidator::EventSet::ContentEvents);
}

void TerrainService::EnqueueAssetInvalidation(const GUID& guid)
{
    std::lock_guard lock(m_PendingAssetInvalidationsMutex);
    m_PendingAssetInvalidations.push_back(guid);
}

std::vector<GUID> TerrainService::TakePendingAssetInvalidations()
{
    // TRACKED: single-consumer queue — TerrainModifierSystem drains it and
    // evicts BOTH caches, including the mask cache that lives per-system. A
    // second consumer (another world's modifier system, a future zone-payload
    // cache) would starve; needs per-consumer cursors if one appears.
    std::lock_guard lock(m_PendingAssetInvalidationsMutex);
    return std::exchange(m_PendingAssetInvalidations, {});
}

std::shared_ptr<const Terrain::HeightfieldData> TerrainService::ResolveHeightmapAsset(const GUID& guid)
{
    if (guid.IsNull())
        return nullptr;

    // A failed decode caches a null entry, so a hit returns null for it too.
    if (const auto it = m_HeightmapCache.find(guid); it != m_HeightmapCache.end())
        return it->second.Heightfield;

    DecodedHeightmapEntry entry;
    entry.ContentVersion = m_HeightmapVersions[guid];
    Terrain::HeightfieldData decoded;
    bool loadFailed = true;

    // TRACKED: decode size is unbounded (here and in the modifier system's
    // stamp-mask decode) — a multi-hundred-MB source stalls the gather-time
    // sync decode and stays resident in the cache; needs a size cap / async
    // decode lane if imports of that scale appear.
    auto& engine = EngineCore::GetInstance();
    if (engine.IsInitialized())
    {
        AssetMetadata meta{};
        if (engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(guid, meta) && !meta.Path.empty())
        {
            std::string ext = meta.Path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (ext == ".r16" || ext == ".r32")
            {
                // A raw file records no grid: its width and height are the asset's import
                // settings (square when unset). RawHeightmap.h says why a guessed grid is wrong.
                RawHeightmapLayout declared;
                entry.DecodeError = ResolveDeclaredRawHeightmapLayout(
                    ReadRawHeightmapSettings(engine.GetAssetManager().GetRegistry(), meta.Path), declared);
                if (entry.DecodeError.empty())
                    entry.DecodeError = DecodeRawHeightmap(meta.Path, declared.Width, declared.Height, decoded);
                loadFailed = !entry.DecodeError.empty();
            }
            else
            {
                // Texture-typed heightmap reference (any image the decoder reads).
                // stb's 16-bit path keeps full PNG16 precision and promotes 8-bit
                // sources. A PNG decodes whole, so one past the decoder's limit is
                // refused with the fix; other formats have no such check.
                if (ext == ".png")
                {
                    Terrain::PngHeightmapSize pngSize;
                    entry.DecodeError = Terrain::ResolvePngHeightmapSize(meta.Path, pngSize);
                }
                loadFailed = !entry.DecodeError.empty() || !decoded.LoadFromPNG16(meta.Path);
                if (loadFailed && entry.DecodeError.empty())
                    entry.DecodeError = "the image could not be decoded";
            }
        }
        else
        {
            entry.DecodeError = "the asset is not in the asset database";
        }

        if (loadFailed)
        {
            // A user's data, refused with the fix the inspector also shows; not an engine fault.
            Logger::Log::Warning("Terrain base heightmap {} ({}) did not decode: {}. The terrain's base is flat "
                               "until it does.",
                               meta.Path.filename().string(), guid.ToString(), entry.DecodeError);
        }
    }

    if (!loadFailed)
        entry.Heightfield = std::make_shared<const Terrain::HeightfieldData>(std::move(decoded));
    const auto ins = m_HeightmapCache.emplace(guid, std::move(entry)).first;
    return ins->second.Heightfield;
}

std::string TerrainService::GetHeightmapDecodeError(const GUID& guid) const
{
    const auto it = m_HeightmapCache.find(guid);
    return it != m_HeightmapCache.end() ? it->second.DecodeError : std::string{};
}

uint64 TerrainService::GetHeightmapContentVersion(const GUID& guid) const
{
    const auto it = m_HeightmapVersions.find(guid);
    return it != m_HeightmapVersions.end() ? it->second : 0;
}

bool TerrainService::EvictDecodedHeightmap(const GUID& guid)
{
    const bool referenced = m_HeightmapCache.count(guid) != 0 || m_HeightmapVersions.count(guid) != 0;
    if (!referenced)
        return false;
    m_HeightmapCache.erase(guid);
    ++m_HeightmapVersions[guid];
    return true;
}

void TerrainService::SeedDecodedHeightmapForTests(const GUID& guid, Terrain::HeightfieldData heightfield)
{
    DecodedHeightmapEntry entry;
    entry.Heightfield = std::make_shared<const Terrain::HeightfieldData>(std::move(heightfield));
    entry.ContentVersion = m_HeightmapVersions[guid];
    m_HeightmapCache[guid] = std::move(entry);
}

TiledTerrainBase TerrainService::ResolveTiledTerrainBase(Components::TerrainBaseSource source,
                                                         const GUID& heightmapGuid)
{
    TiledTerrainBase base;
    base.Source = source;
    if (source != Components::TerrainBaseSource::HeightmapAsset)
        return base;
    base.HeightmapGuid = heightmapGuid;
    base.HeightmapContentVersion = GetHeightmapContentVersion(heightmapGuid);
    base.Heightmap = ResolveHeightmapAsset(heightmapGuid);
    return base;
}

bool TerrainService::IsTiledTerrainBaseCurrent(const TiledTerrainBase& base,
                                               Components::TerrainBaseSource source,
                                               const GUID& heightmapGuid) const
{
    if (base.Source != source)
        return false;
    if (source != Components::TerrainBaseSource::HeightmapAsset)
        return true;
    return base.HeightmapGuid == heightmapGuid &&
           base.HeightmapContentVersion == GetHeightmapContentVersion(heightmapGuid);
}

TiledTerrainConfig TerrainService::BuildTiledTerrainConfig(const Components::Terrain& terrain)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = terrain.SizeX;
    config.WorldSizeZ = terrain.SizeZ;
    config.HeightScale = terrain.HeightScale;
    config.SamplesPerMeter = terrain.SamplesPerMeter;
    config.PatchGridSize = Terrain::kDefaultGridSize;
    config.LODRangeScale = Terrain::kDefaultLODRangeScale;
    config.StreamingRadius = terrain.StreamingRadius;
    config.Base = ResolveTiledTerrainBase(terrain.BaseSource, terrain.TerrainAssetGuid.ToGuid());
    return config;
}

TiledTerrainEdit TerrainService::CompareTiledTerrainConfig(const TiledTerrainConfig& config,
                                                           const Components::Terrain& terrain) const
{
    TiledTerrainEdit edit;
    edit.ResolutionChanged = config.SamplesPerMeter != terrain.SamplesPerMeter;
    edit.SizeChanged = config.WorldSizeX != terrain.SizeX || config.WorldSizeZ != terrain.SizeZ;
    edit.BaseChanged =
        !IsTiledTerrainBaseCurrent(config.Base, terrain.BaseSource, terrain.TerrainAssetGuid.ToGuid());
    return edit;
}

// ---- Zone payload store ----

TerrainZonePayload* TerrainService::GetZonePayload(const GUID& guid)
{
    const auto it = m_ZonePayloads.find(guid);
    return it != m_ZonePayloads.end() ? &it->second : nullptr;
}

bool TerrainService::AnyZonePayloadNeedsSave() const
{
    for (const auto& [guid, payload] : m_ZonePayloads)
    {
        if (payload.NeedsSave)
            return true;
    }
    return false;
}

void TerrainService::ClearZonePayloads()
{
    if (m_ZonePayloads.empty())
        return;
    m_ZonePayloads.clear();
    ++m_ZonePayloadEditEpoch;
}

const TerrainZonePayload* TerrainService::ResolveZonePayload(const GUID& guid)
{
    if (guid.IsNull())
        return nullptr;

    if (const auto it = m_ZonePayloads.find(guid); it != m_ZonePayloads.end())
        return &it->second;

    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return nullptr;

    AssetMetadata meta{};
    if (!engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(guid, meta) || meta.Path.empty())
        return nullptr;

    BinaryAsset blob(guid, meta.Path, AssetType::TerrainZoneData);
    TerrainZonePayload payload;
    if (!blob.Load() || !DecodeZonePayload(blob.GetData(), blob.GetDataSize(), payload))
    {
        Logger::Log::Warning("Terrain zone payload {} failed to decode", guid.ToString());
        return nullptr;
    }

    return &m_ZonePayloads.emplace(guid, std::move(payload)).first->second;
}

TerrainZonePayload& TerrainService::EnsureZonePayload(const GUID& guid, ZonePayloadFormat format,
                                                      uint32 width, uint32 height)
{
    TerrainZonePayload& payload = m_ZonePayloads[guid];
    payload.Allocate(format, width, height);
    payload.DataVersion = 1;
    payload.ClearDirty();
    payload.NeedsSave = true;
    ++m_ZonePayloadEditEpoch;
    return payload;
}

void TerrainService::NotifyZonePayloadEdited(const GUID& guid, int32 minX, int32 minZ,
                                             int32 maxX, int32 maxZ)
{
    auto it = m_ZonePayloads.find(guid);
    if (it == m_ZonePayloads.end())
        return;
    TerrainZonePayload& payload = it->second;
    ++payload.DataVersion;
    payload.MarkDirtyTexels(minX, minZ, maxX, maxZ);
    payload.NeedsSave = true;
    ++m_ZonePayloadEditEpoch;
}

void TerrainService::ClearZonePayloadDirty(const GUID& guid)
{
    if (auto it = m_ZonePayloads.find(guid); it != m_ZonePayloads.end())
        it->second.ClearDirty();
}

bool TerrainService::EvictZonePayload(const GUID& guid)
{
    if (m_ZonePayloads.erase(guid) == 0)
        return false;
    ++m_ZonePayloadEditEpoch;
    return true;
}

void TerrainService::SetZonePayload(const GUID& guid, TerrainZonePayload payload)
{
    TerrainZonePayload& slot = m_ZonePayloads[guid];
    const uint64 nextVersion = slot.DataVersion + 1;
    slot = std::move(payload);
    slot.DataVersion = nextVersion; // monotonic so the bake diff sees a change
    slot.ClearDirty();              // no sub-rect: re-bake the whole footprint
    slot.NeedsSave = true;
    ++m_ZonePayloadEditEpoch;
}

void TerrainService::SeedZonePayloadForTests(const GUID& guid, TerrainZonePayload payload)
{
    m_ZonePayloads[guid] = std::move(payload);
    ++m_ZonePayloadEditEpoch;
}

void TerrainService::SnapshotZonePayloadsForPlay()
{
    m_ZonePayloadPlaySnapshot = m_ZonePayloads;
    m_HasZonePayloadPlaySnapshot = true;
}

void TerrainService::RestoreZonePayloadsFromPlaySnapshot()
{
    if (!m_HasZonePayloadPlaySnapshot)
        return;
    m_ZonePayloads = std::move(m_ZonePayloadPlaySnapshot);
    m_ZonePayloadPlaySnapshot.clear();
    m_HasZonePayloadPlaySnapshot = false;
    // Force the next bake to re-read the reverted payloads.
    ++m_ZonePayloadEditEpoch;
}

TiledRenderExtent ComputeTiledRenderExtent(const TiledTerrainConfig& config)
{
    TiledRenderExtent e{};
    e.TilesPerAxisX = config.TilesPerAxisX;
    e.TilesPerAxisZ = config.TilesPerAxisZ;
    e.TileInteriorX = config.TileConfig.HeightmapWidth > 0 ? config.TileConfig.HeightmapWidth - 1 : 0;
    e.TileInteriorZ = config.TileConfig.HeightmapHeight > 0 ? config.TileConfig.HeightmapHeight - 1 : 0;
    e.UnifiedWidth = e.TilesPerAxisX * e.TileInteriorX + 1;
    e.UnifiedHeight = e.TilesPerAxisZ * e.TileInteriorZ + 1;
    e.WorldSizeX = static_cast<float32>(e.TilesPerAxisX) * config.TileWorldSize;
    e.WorldSizeZ = static_cast<float32>(e.TilesPerAxisZ) * config.TileWorldSize;
    return e;
}

bool SampleTiledHeightNormalized(const TiledTerrainData& tiled,
                                 float32 worldX, float32 worldZ, float32& outHeight)
{
    const float32 tileWorld = tiled.Config.TileWorldSize;
    if (tileWorld <= 0.0f)
        return false;

    // World XZ -> tile grid index (floor). Reject points outside the tile grid.
    const float32 relX = worldX - tiled.WorldOriginX;
    const float32 relZ = worldZ - tiled.WorldOriginZ;
    if (relX < 0.0f || relZ < 0.0f)
        return false;
    const int32 tileX = static_cast<int32>(std::floor(relX / tileWorld));
    const int32 tileZ = static_cast<int32>(std::floor(relZ / tileWorld));
    if (tileX >= static_cast<int32>(tiled.Config.TilesPerAxisX) ||
        tileZ >= static_cast<int32>(tiled.Config.TilesPerAxisZ))
        return false;

    auto it = tiled.Tiles.find(TileCoord{tileX, tileZ});
    if (it == tiled.Tiles.end() || !it->second || it->second->LodState == TileLodState::Empty)
        return false;
    const TerrainTileData& tile = *it->second;
    if (tile.Heightfield.IsEmpty())
        return false;

    // Tile-local UV in [0,1]; the tile's heightfield spans exactly its world area.
    const float32 u = std::clamp((worldX - tile.WorldOriginX) / tileWorld, 0.0f, 1.0f);
    const float32 v = std::clamp((worldZ - tile.WorldOriginZ) / tileWorld, 0.0f, 1.0f);
    outHeight = tile.Heightfield.SampleBilinear(u, v);
    return true;
}

void FillHeightfieldBaseRegion(Terrain::HeightfieldData& heightfield,
                               Components::TerrainBaseSource source,
                               const Terrain::HeightfieldData* heightmap,
                               int32 minX, int32 minZ, int32 maxX, int32 maxZ)
{
    const uint32 w = heightfield.GetWidth();
    const uint32 h = heightfield.GetHeight();
    if (w == 0 || h == 0)
        return;

    minX = std::max(minX, 0);
    minZ = std::max(minZ, 0);
    maxX = std::min(maxX, static_cast<int32>(w - 1));
    maxZ = std::min(maxZ, static_cast<int32>(h - 1));
    if (minX > maxX || minZ > maxZ)
        return;

    switch (source)
    {
    case Components::TerrainBaseSource::ProceduralNoise:
        heightfield.FillRegionWithNoise(kBaseNoiseFrequency, kBaseNoiseAmplitude,
                                        minX, minZ, maxX, maxZ,
                                        kBaseNoiseOctaves, kBaseNoiseSeed);
        return;

    case Components::TerrainBaseSource::HeightmapAsset:
        if (heightmap != nullptr && !heightmap->IsEmpty())
        {
            // Global UV per destination sample -> bilinear source sample.
            // Purely coordinate-dependent, so region fills are exact subsets
            // of full fills (region-bake parity).
            const float32 invW = w > 1 ? 1.0f / static_cast<float32>(w - 1) : 0.0f;
            const float32 invH = h > 1 ? 1.0f / static_cast<float32>(h - 1) : 0.0f;
            for (int32 z = minZ; z <= maxZ; ++z)
                for (int32 x = minX; x <= maxX; ++x)
                    heightfield.SetSample(static_cast<uint32>(x), static_cast<uint32>(z),
                                          heightmap->SampleBilinear(static_cast<float32>(x) * invW,
                                                                    static_cast<float32>(z) * invH));
            return;
        }
        // Missing or undecodable source: flat, so the problem is visible.
        [[fallthrough]];

    case Components::TerrainBaseSource::Flat:
    default:
        for (int32 z = minZ; z <= maxZ; ++z)
            for (int32 x = minX; x <= maxX; ++x)
                heightfield.SetSample(static_cast<uint32>(x), static_cast<uint32>(z), 0.0f);
        return;
    }
}

void FillTiledBaseRegion(Terrain::HeightfieldData& field, const TiledTerrainConfig& config,
                         float32 terrainOriginX, float32 terrainOriginZ,
                         float32 fieldOriginX, float32 fieldOriginZ,
                         float32 fieldSizeX, float32 fieldSizeZ,
                         int32 minX, int32 minZ, int32 maxX, int32 maxZ)
{
    const uint32 w = field.GetWidth();
    const uint32 h = field.GetHeight();
    if (w < 2 || h < 2)
        return;

    minX = std::max(minX, 0);
    minZ = std::max(minZ, 0);
    maxX = std::min(maxX, static_cast<int32>(w - 1));
    maxZ = std::min(maxZ, static_cast<int32>(h - 1));
    if (minX > maxX || minZ > maxZ)
        return;

    const TiledTerrainBase& base = config.Base;
    switch (base.Source)
    {
    case Components::TerrainBaseSource::ProceduralNoise:
        field.FillRegionWithNoiseWorldSpace(kTileNoiseFrequency, kTileNoiseAmplitude,
                                            fieldOriginX, fieldOriginZ, fieldSizeX, fieldSizeZ,
                                            minX, minZ, maxX, maxZ,
                                            kTileNoiseOctaves, kTileNoiseSeed);
        return;

    case Components::TerrainBaseSource::HeightmapAsset:
        if (base.Heightmap != nullptr && !base.Heightmap->IsEmpty() && config.WorldSizeX > 0.0f &&
            config.WorldSizeZ > 0.0f)
        {
            // Footprint UV of each sample: its world position relative to the terrain's
            // corner over the authored size, the mapping an untiled terrain's sample
            // index / (dim - 1) resolves to. Clamped, because SampleBilinear reads out of
            // range past [0, 1] and the tile grid can overhang the footprint.
            const Terrain::HeightfieldData& heightmap = *base.Heightmap;
            const float32 spacingX = fieldSizeX / static_cast<float32>(w - 1);
            const float32 spacingZ = fieldSizeZ / static_cast<float32>(h - 1);
            for (int32 z = minZ; z <= maxZ; ++z)
            {
                const float32 worldZ = fieldOriginZ + static_cast<float32>(z) * spacingZ;
                const float32 v = std::clamp((worldZ - terrainOriginZ) / config.WorldSizeZ, 0.0f, 1.0f);
                for (int32 x = minX; x <= maxX; ++x)
                {
                    const float32 worldX = fieldOriginX + static_cast<float32>(x) * spacingX;
                    const float32 u =
                        std::clamp((worldX - terrainOriginX) / config.WorldSizeX, 0.0f, 1.0f);
                    field.SetSample(static_cast<uint32>(x), static_cast<uint32>(z),
                                    heightmap.SampleBilinear(u, v));
                }
            }
            return;
        }
        // Missing or undecodable source: flat, so the problem is visible.
        [[fallthrough]];

    case Components::TerrainBaseSource::Flat:
    default:
        for (int32 z = minZ; z <= maxZ; ++z)
            for (int32 x = minX; x <= maxX; ++x)
                field.SetSample(static_cast<uint32>(x), static_cast<uint32>(z), 0.0f);
        return;
    }
}

void FillAtlasCoarseBaseField(Terrain::HeightfieldData& field, const TiledTerrainData& tiled)
{
    const TiledRenderExtent extent = ComputeTiledRenderExtent(tiled.Config);
    FillTiledBaseRegion(field, tiled.Config, tiled.WorldOriginX, tiled.WorldOriginZ,
                        tiled.WorldOriginX, tiled.WorldOriginZ, extent.WorldSizeX, extent.WorldSizeZ,
                        0, 0, static_cast<int32>(field.GetWidth()) - 1,
                        static_cast<int32>(field.GetHeight()) - 1);
}

TerrainHandle TerrainService::CreateTerrain(Terrain::TerrainConfig config)
{
    // C-#7: Config validation
    if (config.WorldSizeX <= 0.0f || config.WorldSizeZ <= 0.0f)
        return TerrainHandle{};
    if (config.HeightScale <= 0.0f)
        config.HeightScale = 1.0f;
    if (config.HeightmapWidth < 3)
        config.HeightmapWidth = 3;
    if (config.HeightmapHeight < 3)
        config.HeightmapHeight = 3;
    if (config.HeightmapWidth > Terrain::kMaxHeightmapDimension)
        config.HeightmapWidth = Terrain::kMaxHeightmapDimension;
    if (config.HeightmapHeight > Terrain::kMaxHeightmapDimension)
        config.HeightmapHeight = Terrain::kMaxHeightmapDimension;
    if (config.LODLevels == 0)
        return TerrainHandle{};
    config.LODLevels = std::min(config.LODLevels, Terrain::kMaxLODLevels);
    if (config.LODRangeScale < 0.1f) config.LODRangeScale = 0.1f;
    if (config.PatchGridSize < Terrain::kMinPatchSize) config.PatchGridSize = Terrain::kMinPatchSize;
    if (config.PatchGridSize > Terrain::kMaxPatchSize) config.PatchGridSize = Terrain::kMaxPatchSize;

    std::lock_guard<std::mutex> lock(m_Mutex);

    uint32 index;
    if (!m_FreeList.empty())
    {
        index = m_FreeList.back();
        m_FreeList.pop_back();
    }
    else
    {
        index = static_cast<uint32>(m_Slots.size());
        m_Slots.emplace_back();
    }

    auto& slot = m_Slots[index];
    slot.Generation++;
    slot.Active = true;
    slot.Data = std::make_shared<TerrainData>();
    slot.Data->Config = config;
    slot.Data->Heightfield.Resize(config.HeightmapWidth, config.HeightmapHeight, 0.0f);
    slot.Data->Quadtree.Build(slot.Data->Heightfield, config.LODLevels);
    slot.Data->MarkFullDirty();

    return TerrainHandle{index, slot.Generation};
}

void TerrainService::DestroyTerrain(TerrainHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Slots.size())
        return;

    auto& slot = m_Slots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    slot.Active = false;
    slot.Data.reset();
    m_FreeList.push_back(handle.Index);
}

TerrainData* TerrainService::GetTerrainData(TerrainHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Slots.size())
        return nullptr;

    auto& slot = m_Slots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return nullptr;

    return slot.Data.get();
}

const TerrainData* TerrainService::GetTerrainData(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Slots.size())
        return nullptr;

    const auto& slot = m_Slots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return nullptr;

    return slot.Data.get();
}

std::shared_ptr<const TerrainData> TerrainService::ShareTerrainData(TerrainHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Slots.size())
        return nullptr;

    const auto& slot = m_Slots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return nullptr;

    return slot.Data;
}

uint32 TerrainService::GetActiveTerrainCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    uint32 count = 0;
    for (const auto& slot : m_Slots)
    {
        if (slot.Active)
            ++count;
    }
    return count;
}

uint32 TerrainService::GetActiveTiledTerrainCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    uint32 count = 0;
    for (const auto& slot : m_TiledSlots)
    {
        if (slot.Active)
            ++count;
    }
    return count;
}

uint32 TerrainService::GetTerrainSlotCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return static_cast<uint32>(m_Slots.size());
}

uint32 TerrainService::GetTiledTerrainSlotCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return static_cast<uint32>(m_TiledSlots.size());
}

void TerrainService::RebuildQuadtree(TerrainHandle handle)
{
    // Hold mutex for the entire rebuild to prevent use-after-free
    // if DestroyTerrain is called concurrently.
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Slots.size())
        return;
    auto& slot = m_Slots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    slot.Data->Quadtree.Build(slot.Data->Heightfield, slot.Data->Config.LODLevels);
}

bool TerrainService::PatchQuadtreeRegion(TerrainHandle handle,
                                         int32 minSampleX, int32 minSampleZ,
                                         int32 maxSampleX, int32 maxSampleZ,
                                         float32& outMinH, float32& outMaxH)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_Slots.size())
        return false;
    auto& slot = m_Slots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return false;

    auto& data = *slot.Data;
    if (!data.Quadtree.IsBuilt())
        data.Quadtree.Build(data.Heightfield, data.Config.LODLevels);
    else
        data.Quadtree.PatchRegion(data.Heightfield, minSampleX, minSampleZ, maxSampleX, maxSampleZ);

    return data.Quadtree.TryGetGlobalHeightRange(
        data.Heightfield.GetWidth(), data.Heightfield.GetHeight(), outMinH, outMaxH);
}

// ---- Tiled terrain implementation ----

TiledTerrainHandle TerrainService::CreateTiledTerrain(TiledTerrainConfig config)
{
    if (config.WorldSizeX <= 0.0f || config.WorldSizeZ <= 0.0f)
        return {};
    config.SamplesPerMeter = std::max(config.SamplesPerMeter, 0.01f);
    config.PatchGridSize = std::clamp(config.PatchGridSize, Terrain::kMinPatchSize, Terrain::kMaxPatchSize);

    // Tile geometry comes from the shared sizing derivation, so the grid built here is the same
    // grid the inspector predicts and the height-source rule is tested against. Tiles are square
    // and capped at the per-tile sample ceiling; larger tiles load unacceptably slowly on the
    // main thread. HeightScale is re-applied here only because it does not affect geometry.
    const TerrainSizingPlan plan =
        DeriveTerrainSizingPlan(config.WorldSizeX, config.WorldSizeZ, config.SamplesPerMeter,
                                config.PatchGridSize, config.LODRangeScale);

    config.TileConfig = Terrain::TerrainConfig::FromSamplesPerMeter(
        plan.TileWorldSize, plan.TileWorldSize, config.HeightScale,
        config.SamplesPerMeter, config.PatchGridSize, config.LODRangeScale);

    config.TileWorldSize = plan.TileWorldSize;
    config.TilesPerAxisX = plan.TilesPerAxisX;
    config.TilesPerAxisZ = plan.TilesPerAxisZ;

    // Auto-compute streaming radius (world metres). The resident window MUST stay
    // anchored in world space independent of texel density (SamplesPerMeter): a higher
    // SPM subdivides each tile into a finer grid, which shrinks tileWorld
    // (= (kMaxTileResolution-1)/spm). Deriving the window from the actual tileWorld
    // therefore HALVED it when SPM went 1->2 on a fixed-size terrain (e.g. 4096 m:
    // 3072 -> 1536 m), so previously-resident bands fell out of the window mid-session
    // and rendered as sky-through-terrain tears. Anchor the window to a REFERENCE tile
    // sized at kReferenceSamplesPerMeter so the coarsest-LOD-range derivation is a pure
    // function of world size, patch grid, and LOD range scale — identical for every SPM.
    // (LOD level count is already SPM-invariant for a multi-tile terrain, since each tile
    // targets the same ~kMaxTileResolution; recomputing it at the reference density keeps
    // the single-tile small-terrain case invariant too.) An explicitly user-set
    // StreamingRadius (> 0) stays authoritative and is never overridden here.
    if (config.StreamingRadius <= 0.0f)
    {
        static constexpr float32 kReferenceSamplesPerMeter = 1.0f;
        static constexpr float32 kStreamingRadiusMargin = 1.5f; // 50% beyond coarsest LOD
        const float32 referenceTileWorld = std::min(
            {config.WorldSizeX, config.WorldSizeZ,
             static_cast<float32>(Terrain::kMaxTileResolution - 1) / kReferenceSamplesPerMeter});
        const Terrain::TerrainConfig referenceTile = Terrain::TerrainConfig::FromSamplesPerMeter(
            referenceTileWorld, referenceTileWorld, config.HeightScale,
            kReferenceSamplesPerMeter, config.PatchGridSize, config.LODRangeScale);
        const uint32 finestNodes = 1u << (referenceTile.LODLevels - 1);
        const float32 patchSize = referenceTileWorld / static_cast<float32>(finestNodes);
        const float32 baseRange = patchSize * config.LODRangeScale;
        float32 maxRange = baseRange;
        for (uint32 i = 1; i < referenceTile.LODLevels; ++i)
            maxRange = Terrain::CDLODSelection::ComputeLODRange(i, baseRange, config.LODRangeScale);
        config.StreamingRadius = maxRange * kStreamingRadiusMargin;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);

    uint32 index;
    if (!m_TiledFreeList.empty())
    {
        index = m_TiledFreeList.back();
        m_TiledFreeList.pop_back();
    }
    else
    {
        index = static_cast<uint32>(m_TiledSlots.size());
        m_TiledSlots.emplace_back();
    }

    auto& slot = m_TiledSlots[index];
    slot.Generation++;
    slot.Active = true;
    slot.Data = std::make_unique<TiledTerrainData>();
    slot.Data->Config = config;

    return TiledTerrainHandle{index, slot.Generation};
}

void TerrainService::DestroyTiledTerrain(TiledTerrainHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    // Destroy the global GPU TerrainData slot (holds the global quadtree) before
    // freeing the tiled data. Tiles no longer own per-tile GPU slots — the whole
    // terrain renders through one unified texture set on the render feature.
    if (slot.Data && (slot.Data->GlobalGpuHandleIndex != 0 || slot.Data->GlobalGpuHandleGeneration != 0))
    {
        TerrainHandle gpuH{slot.Data->GlobalGpuHandleIndex, slot.Data->GlobalGpuHandleGeneration};
        if (gpuH.Index < m_Slots.size())
        {
            auto& gpuSlot = m_Slots[gpuH.Index];
            if (gpuSlot.Active && gpuSlot.Generation == gpuH.Generation)
            {
                gpuSlot.Active = false;
                gpuSlot.Data.reset();
                m_FreeList.push_back(gpuH.Index);
            }
        }
    }

    // Release any tile-physics slots addressing this terrain. The collider
    // entities are destroyed elsewhere (TerrainPhysicsSystem sweep or snapshot
    // restore); without this, their slots would leak once the terrain is gone.
    for (uint32 i = 0; i < m_TilePhysicsSlots.size(); ++i)
    {
        auto& phys = m_TilePhysicsSlots[i];
        if (phys.Active && phys.Tiled == handle)
        {
            phys.Active = false;
            ++phys.Generation; // stale handles resolving here now fail
            m_TilePhysicsFreeList.push_back(i);
        }
    }

    slot.Active = false;
    slot.Data.reset();
    m_TiledFreeList.push_back(handle.Index);
}

TiledTerrainData* TerrainService::GetTiledTerrainData(TiledTerrainHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return nullptr;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return nullptr;

    return slot.Data.get();
}

TerrainService::TilePhysicsHandle
TerrainService::AcquireTilePhysicsHandle(TiledTerrainHandle tiled, TileCoord coord)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    uint32 index;
    if (!m_TilePhysicsFreeList.empty())
    {
        index = m_TilePhysicsFreeList.back();
        m_TilePhysicsFreeList.pop_back();
    }
    else
    {
        index = static_cast<uint32>(m_TilePhysicsSlots.size());
        m_TilePhysicsSlots.emplace_back();
    }

    auto& slot = m_TilePhysicsSlots[index];
    slot.Tiled = tiled;
    slot.Coord = coord;
    slot.Active = true;
    ++slot.Generation; // distinct from any prior handle into this slot

    return TilePhysicsHandle{index | kTilePhysicsHandleBit, slot.Generation};
}

void TerrainService::ReleaseTilePhysicsHandle(uint32 handleIndex, uint32 generation)
{
    if ((handleIndex & kTilePhysicsHandleBit) == 0)
        return;
    const uint32 index = handleIndex & ~kTilePhysicsHandleBit;

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (index >= m_TilePhysicsSlots.size())
        return;
    auto& slot = m_TilePhysicsSlots[index];
    if (!slot.Active)
        return;
    if (slot.Generation != generation)
    {
        // A stale handle: the slot was already released and re-acquired by a
        // different tile. Releasing on the index alone would silently free the
        // current owner (the same silent-unbind class the material binder
        // closed). Refuse and make it loud.
        Logger::Log::Warning(
            "TerrainService::ReleaseTilePhysicsHandle stale handle ignored "
            "(index={}, expected gen={}, got gen={})",
            index, slot.Generation, generation);
        return;
    }
    slot.Active = false;
    ++slot.Generation; // stale handles resolving against this slot now fail
    m_TilePhysicsFreeList.push_back(index);
}

void TerrainService::ReleaseAllTilePhysicsHandles()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_TilePhysicsSlots.clear();
    m_TilePhysicsFreeList.clear();
}

uint32 TerrainService::GetTilePhysicsSlotCountForTests() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return static_cast<uint32>(m_TilePhysicsSlots.size());
}

TerrainTileData* TerrainService::ResolveTilePhysicsTile(uint32 handleIndex, uint32 generation)
{
    if ((handleIndex & kTilePhysicsHandleBit) == 0)
        return nullptr;
    const uint32 index = handleIndex & ~kTilePhysicsHandleBit;

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (index >= m_TilePhysicsSlots.size())
        return nullptr;
    const auto& slot = m_TilePhysicsSlots[index];
    if (!slot.Active || slot.Generation != generation)
        return nullptr;

    // Resolve the live tiled terrain + tile directly (already under the lock).
    if (slot.Tiled.Index >= m_TiledSlots.size())
        return nullptr;
    auto& tiledSlot = m_TiledSlots[slot.Tiled.Index];
    if (!tiledSlot.Active || tiledSlot.Generation != slot.Tiled.Generation || !tiledSlot.Data)
        return nullptr;

    auto& tiles = tiledSlot.Data->Tiles;
    auto it = tiles.find(slot.Coord);
    if (it == tiles.end() || !it->second)
        return nullptr;
    return it->second.get();
}

// ---- Per-face planet collider registry (planet-collider slice) ----

TerrainService::PlanetFacePhysicsHandle TerrainService::AcquirePlanetFacePhysicsHandle()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    uint32 index;
    if (!m_PlanetFacePhysicsFreeList.empty())
    {
        index = m_PlanetFacePhysicsFreeList.back();
        m_PlanetFacePhysicsFreeList.pop_back();
    }
    else
    {
        index = static_cast<uint32>(m_PlanetFacePhysicsSlots.size());
        m_PlanetFacePhysicsSlots.emplace_back();
    }

    auto& slot = m_PlanetFacePhysicsSlots[index];
    slot.Data = PlanetFaceColliderData{}; // fresh buffer + version 0
    slot.Active = true;
    ++slot.Generation; // distinct from any prior handle into this slot

    return PlanetFacePhysicsHandle{index | kPlanetFacePhysicsHandleBit, slot.Generation};
}

void TerrainService::ReleasePlanetFacePhysicsHandle(uint32 handleIndex, uint32 generation)
{
    if ((handleIndex & kPlanetFacePhysicsHandleBit) == 0)
        return;
    const uint32 index = handleIndex & ~kPlanetFacePhysicsHandleBit;

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (index >= m_PlanetFacePhysicsSlots.size())
        return;
    auto& slot = m_PlanetFacePhysicsSlots[index];
    if (!slot.Active)
        return;
    if (slot.Generation != generation)
    {
        // Stale handle: the slot was released and re-acquired by a different face.
        // Releasing on the index alone would silently free the current owner (the
        // material-binder silent-unbind class). Refuse and make it loud.
        Logger::Log::Warning(
            "TerrainService::ReleasePlanetFacePhysicsHandle stale handle ignored "
            "(index={}, expected gen={}, got gen={})",
            index, slot.Generation, generation);
        return;
    }
    slot.Active = false;
    slot.Data = PlanetFaceColliderData{}; // free the buffer immediately
    ++slot.Generation; // stale handles resolving against this slot now fail
    m_PlanetFacePhysicsFreeList.push_back(index);
}

void TerrainService::ReleaseAllPlanetFacePhysicsHandles()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_PlanetFacePhysicsSlots.clear();
    m_PlanetFacePhysicsFreeList.clear();
}

TerrainService::PlanetFaceColliderData*
TerrainService::ResolvePlanetFaceColliderForWrite(uint32 handleIndex, uint32 generation, uint32 dim)
{
    if ((handleIndex & kPlanetFacePhysicsHandleBit) == 0)
        return nullptr;
    const uint32 index = handleIndex & ~kPlanetFacePhysicsHandleBit;

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (index >= m_PlanetFacePhysicsSlots.size())
        return nullptr;
    auto& slot = m_PlanetFacePhysicsSlots[index];
    if (!slot.Active || slot.Generation != generation)
        return nullptr;

    if (slot.Data.Dim != dim || slot.Data.Samples.size() != static_cast<size_t>(dim) * dim)
    {
        slot.Data.Samples.assign(static_cast<size_t>(dim) * dim, 0.0f);
        slot.Data.Dim = dim;
    }
    return &slot.Data;
}

const TerrainService::PlanetFaceColliderData*
TerrainService::ResolvePlanetFaceCollider(uint32 handleIndex, uint32 generation) const
{
    if ((handleIndex & kPlanetFacePhysicsHandleBit) == 0)
        return nullptr;
    const uint32 index = handleIndex & ~kPlanetFacePhysicsHandleBit;

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (index >= m_PlanetFacePhysicsSlots.size())
        return nullptr;
    const auto& slot = m_PlanetFacePhysicsSlots[index];
    if (!slot.Active || slot.Generation != generation || slot.Data.Samples.empty())
        return nullptr;
    return &slot.Data;
}

void TerrainService::CommitPlanetFaceFull(uint32 handleIndex, uint32 generation)
{
    if ((handleIndex & kPlanetFacePhysicsHandleBit) == 0)
        return;
    const uint32 index = handleIndex & ~kPlanetFacePhysicsHandleBit;

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (index >= m_PlanetFacePhysicsSlots.size())
        return;
    auto& slot = m_PlanetFacePhysicsSlots[index];
    if (!slot.Active || slot.Generation != generation)
        return;
    ++slot.Data.Version;
    const int32 n = static_cast<int32>(slot.Data.Dim);
    slot.Data.DirtyLog.Append(slot.Data.Version, DirtyRegionLog::Region{0, 0, n, n});
}

void TerrainService::CommitPlanetFaceRegion(uint32 handleIndex, uint32 generation, int32 minX,
                                            int32 minZ, int32 maxX, int32 maxZ)
{
    if ((handleIndex & kPlanetFacePhysicsHandleBit) == 0)
        return;
    const uint32 index = handleIndex & ~kPlanetFacePhysicsHandleBit;

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (index >= m_PlanetFacePhysicsSlots.size())
        return;
    auto& slot = m_PlanetFacePhysicsSlots[index];
    if (!slot.Active || slot.Generation != generation)
        return;
    ++slot.Data.Version;
    slot.Data.DirtyLog.Append(slot.Data.Version, DirtyRegionLog::Region{minX, minZ, maxX, maxZ});
}

uint32 TerrainService::GetPlanetFacePhysicsSlotCountForTests() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return static_cast<uint32>(m_PlanetFacePhysicsSlots.size());
}

// ---- Editable spherical sculpt (planet editing) ----

namespace
{
// Union an edit's (face-local) UV rect into a per-face accumulator.
void UnionPlanetSculptFaceRect(TerrainService::PlanetSculptFaceRect& acc, float32 minU,
                               float32 minV, float32 maxU, float32 maxV)
{
    if (!acc.Touched)
    {
        acc = TerrainService::PlanetSculptFaceRect{true, minU, minV, maxU, maxV};
        return;
    }
    acc.MinU = std::min(acc.MinU, minU);
    acc.MinV = std::min(acc.MinV, minV);
    acc.MaxU = std::max(acc.MaxU, maxU);
    acc.MaxV = std::max(acc.MaxV, maxV);
}
} // namespace

void TerrainService::AccumulateSphereSculptDirtyFaces(const CBTTerrain::SphereEditRegions& regions)
{
    // Caller holds m_SphereSculptMutex. Every edit feeds BOTH per-face unions — physics (drained
    // face-by-face) and render (drained whole, clear-on-read) — so neither consumer can miss a dab
    // the other already consumed, and multiple dabs between a consumer's reads accumulate.
    for (uint32 i = 0; i < regions.Count; ++i)
    {
        const uint32 face = regions.Rects[i].Face;
        if (face >= kPlanetSculptFaceCount)
            continue;
        UnionPlanetSculptFaceRect(m_PlanetSculptFaces[face], regions.Rects[i].MinU,
                                  regions.Rects[i].MinV, regions.Rects[i].MaxU, regions.Rects[i].MaxV);
        UnionPlanetSculptFaceRect(m_RenderSculptFaces[face], regions.Rects[i].MinU,
                                  regions.Rects[i].MinV, regions.Rects[i].MaxU, regions.Rects[i].MaxV);
    }
}

float32 TerrainService::SphereSculptDabFootprintTexels(float32 angularRadius, uint32 virtualDim)
{
    constexpr float32 kHalfPi = 1.57079632679489661923f; // a cube face spans pi/2 rad across virtualDim texels
    const float32 diameter = 2.0f * std::max(angularRadius, 0.0f);
    return diameter / kHalfPi * static_cast<float32>(virtualDim);
}

void TerrainService::ConfigurePlanetSculpt(float32 planetRadius)
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    m_PlanetSculptRadius = planetRadius;
    const CBTTerrain::SphereEditRegions remapped = m_SphereSculpt.Configure(
        CBTTerrain::MakeSphereSculptGeometry(planetRadius, CBTTerrain::ResolveSculptPagePoolCount()));
    // A radius change on a sculpted planet REMAPS the store (Configure resamples the authored
    // content onto the new radius-scaled grid). A remap IS an edit: feed its covering regions into
    // the same per-face dirty unions a dab feeds, so Classify re-tessellates the remapped footprint
    // and the physics colliders re-sample it. The version advance inside Configure re-arms the GPU
    // upload gate and the editing-frames forced VertexEval exactly like any other edit.
    if (remapped.Count > 0u)
    {
        AccumulateSphereSculptDirtyFaces(remapped);
        // Un-stick the one-shot sub-texel warning too: the dim just changed, so the old verdict
        // (brush footprint vs texel size) no longer applies at the new resolution.
        m_SphereSculptSubTexelWarned = false;
    }
}

void TerrainService::ResetPlanetSculpt()
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    m_SphereSculpt.Reset();
    m_PlanetSculptFaces = {};
    m_RenderSculptFaces = {};
    m_SphereSculptSubTexelWarned = false;
    m_PlanetSculptRadius = 0.0f; // the next ConfigurePlanetSculpt re-derives for the new planet
    // The analytic set is derived per-planet state too. Keep its version counter monotonic (a
    // non-empty set clearing IS a change the render/physics gates must see).
    if (m_SphereAnalyticCount > 0u)
    {
        m_SphereAnalytic = {};
        m_SphereAnalyticCount = 0u;
        ++m_SphereAnalyticVersion;
    }
    // A mid-stroke transient dies with its planet (the queued dabs would target the wrong store).
    if (m_SphereTransientDabCount > 0u || !m_SphereStrokeQueue.empty())
    {
        m_SphereStrokeQueue.clear();
        m_SphereTransientDabs = {};
        m_SphereTransientQueueEnd = {};
        m_SphereTransientDabCount = 0u;
        ++m_SphereAnalyticVersion;
    }
    // Persistence bookkeeping resets with the content: the next EnsureSphereSculptLoaded for
    // a (re)created planet re-restores its payload, and a content-free store needs no save.
    m_SphereSculptLoadedGuid = GUID{};
    m_SphereSculptSavedVersion = 0;
    m_SphereSculptLoadFailedGuid = GUID{};
    m_SphereSculptDabDirty = false;
}

CBTTerrain::SphereEditRegions TerrainService::ApplySphereSculptDab(float32 cx, float32 cy, float32 cz,
                                                                   float32 angularRadius,
                                                                   float32 strength, bool lower)
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    return ApplySphereSculptDabLocked(cx, cy, cz, angularRadius, strength, lower);
}

CBTTerrain::SphereEditRegions TerrainService::ApplySphereSculptDabLocked(float32 cx, float32 cy,
                                                                         float32 cz,
                                                                         float32 angularRadius,
                                                                         float32 strength,
                                                                         bool lower)
{
    CBTTerrain::SphereEditRegions regions =
        m_SphereSculpt.ApplyDab(cx, cy, cz, angularRadius, strength, lower);
    AccumulateSphereSculptDirtyFaces(regions);
    if (regions.Count > 0u)
        m_SphereSculptDabDirty = true; // freehand strokes are the unrecoverable content

    // Honest gate: a dab smaller than one texel EVEN AT MAX ESCALATION writes nothing visible
    // (the S4 adaptive levels make sub-BASE-texel brushes storable up to 2^maxLevel x density;
    // beyond that — the few-metre-at-Earth regime under the page-table cap — the store still
    // cannot hold the shape, and escalation deliberately does not fire).
    const uint32 virtualDim = m_SphereSculpt.Geometry().VirtualDim;
    const float32 escalatedFootprint =
        SphereSculptDabFootprintTexels(angularRadius, virtualDim) *
        static_cast<float32>(1u << m_SphereSculpt.MaxPageLevel());
    if (!m_SphereSculptSubTexelWarned && strength != 0.0f && escalatedFootprint < 1.0f)
    {
        m_SphereSculptSubTexelWarned = true;
        Logger::Log::Warning(
            "Planet sculpt dab is sub-texel even at max page escalation (footprint ~{:.2f} texels "
            "at 2^{}x the base grid; page-table dim is {}/face). The brush writes less than one "
            "texel and is invisible — use a larger brush or a smaller radius.",
            escalatedFootprint, m_SphereSculpt.MaxPageLevel(), virtualDim);
    }
    return regions;
}

bool TerrainService::AnalyticModifiersEnabled()
{
    // Read fresh (not static-once) deliberately: the flag folds into the terrain-state hash, so a
    // mid-session flip is a state change (TerrainModifierSystem forces the clearing full re-bake)
    // — and the in-process A/B is what the dark-ship oracles toggle.
    const char* v = std::getenv("GE_TERRAIN_ANALYTIC_MODIFIERS");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

CBTTerrain::SphereEditRegions TerrainService::ApplySphereSculptDabStroked(
    float32 cx, float32 cy, float32 cz, float32 angularRadius, float32 strength, bool lower)
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    if (!AnalyticModifiersEnabled())
        return ApplySphereSculptDabLocked(cx, cy, cz, angularRadius, strength, lower);

    // Degenerate inputs mirror ApplyDab's no-op exactly (no queue entry, no version bump). An
    // unconfigured radius (no ConfigurePlanetSculpt yet) falls back to the store path — the
    // transient's tangent-plane frame needs metres.
    const float32 len = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (m_SphereSculpt.Geometry().PoolPageCount == 0u || len <= 0.0f || angularRadius <= 0.0f)
        return CBTTerrain::SphereEditRegions{};
    if (m_PlanetSculptRadius <= 0.0f)
        return ApplySphereSculptDabLocked(cx, cy, cz, angularRadius, strength, lower);

    // The modifier placements own their slots first; the stroke folds into the remainder. A
    // 16-flatten planet leaves no transient budget — the dab goes straight to the store (the
    // same honest overflow fallback the S2 modifier publish uses).
    const uint32 budget = CBTTerrain::kMaxSphereAnalyticModifiers - m_SphereAnalyticCount;
    if (budget == 0u)
        return ApplySphereSculptDabLocked(cx, cy, cz, angularRadius, strength, lower);

    const float32 amplitude = (lower ? -1.0f : 1.0f) * strength;
    const float32 nx = cx / len, ny = cy / len, nz = cz / len;
    CBTTerrain::SphereAnalyticDab dab = CBTTerrain::MakeSphereAnalyticDab(
        nx, ny, nz, angularRadius, amplitude, m_PlanetSculptRadius);
    if (!dab.Valid)
        return ApplySphereSculptDabLocked(cx, cy, cz, angularRadius, strength, lower);

    // Merge a bitwise-identical centre/radius into the newest transient: k held dabs at one spot
    // are EXACTLY one dab of k-fold amplitude (the falloff is a pure function of angle), so a
    // press-and-hold never consumes budget. Different centres append; overflow commits the
    // oldest half to the store (commit-as-you-go — FIFO order preserved, so the final store
    // content matches an immediate-write stroke byte for byte).
    CBTTerrain::SphereAnalyticDab* last =
        m_SphereTransientDabCount > 0u ? &m_SphereTransientDabs[m_SphereTransientDabCount - 1u]
                                       : nullptr;
    if (last != nullptr && last->N == dab.N && last->AngularRadius == dab.AngularRadius)
    {
        last->Amplitude += amplitude;
    }
    else
    {
        if (m_SphereTransientDabCount >= budget)
            FlushSphereStrokePrefixLocked((m_SphereTransientDabCount + 1u) / 2u);
        m_SphereTransientDabs[m_SphereTransientDabCount] = dab;
        ++m_SphereTransientDabCount;
    }
    m_SphereStrokeQueue.push_back({cx, cy, cz, angularRadius, strength, lower});
    m_SphereTransientQueueEnd[m_SphereTransientDabCount - 1u] =
        static_cast<uint32>(m_SphereStrokeQueue.size());

    // A transient edit IS an edit: the dab's cap regions feed the same render + physics dirty
    // unions a store write feeds (Classify re-tess + collider refresh track the held stroke),
    // and the analytic version re-arms the upload gates.
    CBTTerrain::SphereEditRegions regions =
        CBTTerrain::ClassifySphereCapEdit(nx, ny, nz, angularRadius);
    AccumulateSphereSculptDirtyFaces(regions);
    ++m_SphereAnalyticVersion;
    return regions;
}

void TerrainService::FlushSphereStrokePrefixLocked(uint32 entries)
{
    entries = std::min(entries, m_SphereTransientDabCount);
    if (entries == 0u)
        return;
    const uint32 queueEnd = m_SphereTransientQueueEnd[entries - 1u];
    for (uint32 i = 0; i < queueEnd; ++i)
    {
        const PendingSphereDab& d = m_SphereStrokeQueue[i];
        ApplySphereSculptDabLocked(d.Cx, d.Cy, d.Cz, d.AngularRadius, d.Strength, d.Lower);
    }
    m_SphereStrokeQueue.erase(m_SphereStrokeQueue.begin(),
                              m_SphereStrokeQueue.begin() + queueEnd);
    const uint32 remaining = m_SphereTransientDabCount - entries;
    for (uint32 i = 0; i < remaining; ++i)
    {
        m_SphereTransientDabs[i] = m_SphereTransientDabs[i + entries];
        m_SphereTransientQueueEnd[i] = m_SphereTransientQueueEnd[i + entries] - queueEnd;
    }
    for (uint32 i = remaining; i < m_SphereTransientDabCount; ++i)
    {
        m_SphereTransientDabs[i] = CBTTerrain::SphereAnalyticDab{};
        m_SphereTransientQueueEnd[i] = 0u;
    }
    m_SphereTransientDabCount = remaining;
    ++m_SphereAnalyticVersion; // the transient set shrank — re-arm the upload/sample gates
}

void TerrainService::CommitSphereSculptStroke()
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    if (m_SphereStrokeQueue.empty() && m_SphereTransientDabCount == 0u)
        return;
    // Replay the whole remaining queue through the normal dab path, in order: the store receives
    // the SAME ApplyDab calls (arguments and sequence) an immediate-write stroke would have
    // issued, so content, page allocation order and the lazily-captured stroke pre-images are
    // byte-identical to pre-S3 — and #630's undo entry is built from exactly these writes.
    for (const PendingSphereDab& d : m_SphereStrokeQueue)
        ApplySphereSculptDabLocked(d.Cx, d.Cy, d.Cz, d.AngularRadius, d.Strength, d.Lower);
    m_SphereStrokeQueue.clear();
    if (m_SphereTransientDabCount > 0u)
    {
        for (uint32 i = 0; i < m_SphereTransientDabCount; ++i)
        {
            m_SphereTransientDabs[i] = CBTTerrain::SphereAnalyticDab{};
            m_SphereTransientQueueEnd[i] = 0u;
        }
        m_SphereTransientDabCount = 0u;
        ++m_SphereAnalyticVersion; // the preview hand-off to the store is itself an edit
    }
}

uint32 TerrainService::CopyPlanetTransientDabs(
    std::array<CBTTerrain::SphereAnalyticDab, CBTTerrain::kMaxSphereAnalyticModifiers>& out) const
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    out = m_SphereTransientDabs;
    return m_SphereTransientDabCount;
}

uint64 TerrainService::CopySphereSculptUpload(std::vector<float32>& pool,
                                              std::vector<uint32>& pageTable,
                                              CBTTerrain::SphereSculptGeometry& geom) const
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    pool = m_SphereSculpt.Pool();
    pageTable = m_SphereSculpt.PageTable();
    geom = m_SphereSculpt.Geometry();
    return m_SphereSculpt.Version() + m_SphereAnalyticVersion; // combined (S2) — matches SphereSculptVersion
}

uint64 TerrainService::SphereSculptVersion() const
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    // Combined: store-layer edits + analytic placement-set changes. Both are monotonic, so the sum
    // is — every gate keyed on this (GPU upload ring, Classify drive, physics refresh) re-arms on
    // an analytic edit exactly like a store write (an edit IS an edit).
    return m_SphereSculpt.Version() + m_SphereAnalyticVersion;
}

bool TerrainService::HasSphereSculptEdits() const
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    return m_SphereSculpt.HasEdits() || m_SphereAnalyticCount > 0u ||
           m_SphereTransientDabCount > 0u;
}

uint32 TerrainService::SphereSculptAllocatedPages() const
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    return m_SphereSculpt.AllocatedPageCount();
}

uint32 TerrainService::SphereSculptEscalatedPages() const
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    return m_SphereSculpt.EscalatedPageCount();
}

void TerrainService::SetPlanetAnalyticModifiers(const CBTTerrain::SphereAnalyticFlatten* items,
                                                uint32 count)
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    count = std::min(count, CBTTerrain::kMaxSphereAnalyticModifiers);
    // Field-wise compare (not memcmp — the struct carries padding whose bytes copies need not
    // preserve): an unchanged publish (every bake republishes the current set) is a no-op, so idle
    // bakes never churn the version.
    auto sameFlatten = [](const CBTTerrain::SphereAnalyticFlatten& a,
                          const CBTTerrain::SphereAnalyticFlatten& b) {
        return a.N == b.N && a.E1 == b.E1 && a.E2 == b.E2 && a.Radius == b.Radius &&
               a.Falloff == b.Falloff && a.TargetRadius == b.TargetRadius &&
               a.PlanetRadius == b.PlanetRadius && a.Valid == b.Valid;
    };
    bool same = count == m_SphereAnalyticCount;
    for (uint32 i = 0; same && i < count; ++i)
        same = sameFlatten(m_SphereAnalytic[i], items[i]);
    if (same)
        return;
    for (uint32 i = 0; i < count; ++i)
        m_SphereAnalytic[i] = items[i];
    for (uint32 i = count; i < m_SphereAnalyticCount; ++i)
        m_SphereAnalytic[i] = CBTTerrain::SphereAnalyticFlatten{};
    m_SphereAnalyticCount = count;
    ++m_SphereAnalyticVersion;
}

uint32 TerrainService::CopyPlanetAnalyticModifiers(
    std::array<CBTTerrain::SphereAnalyticFlatten, CBTTerrain::kMaxSphereAnalyticModifiers>& out)
    const
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    out = m_SphereAnalytic;
    return m_SphereAnalyticCount;
}

CBTTerrain::SphereSculptGeometry TerrainService::GetPlanetSculptGeometry() const
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    return m_SphereSculpt.Geometry();
}

bool TerrainService::ConsumeSphereSculptDirtyRegions(CBTTerrain::SphereEditRegions& out)
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    CBTTerrain::SphereEditRegions drained{};
    for (uint32 face = 0; face < kPlanetSculptFaceCount; ++face)
    {
        PlanetSculptFaceRect& r = m_RenderSculptFaces[face];
        if (!r.Touched)
            continue;
        drained.Rects[drained.Count++] =
            CBTTerrain::SphereFaceUVRect{face, r.MinU, r.MinV, r.MaxU, r.MaxV};
        r = PlanetSculptFaceRect{}; // clear-on-read
    }
    if (drained.Count == 0u)
        return false;
    out = drained;
    return true;
}

float32 TerrainService::SampleSphereSculptHeight(float32 dx, float32 dy, float32 dz,
                                                 float32 reliefAtDir) const
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    if (!m_SphereSculpt.HasEdits() && m_SphereAnalyticCount == 0u &&
        m_SphereTransientDabCount == 0u)
        return 0.0f;
    const CBTTerrain::SphereAnalyticModifierSet set{m_SphereAnalytic.data(), m_SphereAnalyticCount,
                                                    m_SphereTransientDabs.data(),
                                                    m_SphereTransientDabCount};
    return CBTTerrain::SampleSphereSculptComposed(m_SphereSculpt.MakeSampler(), set, dx, dy, dz,
                                                  reliefAtDir);
}

TerrainService::PlanetSculptMirror TerrainService::GetPlanetSculptMirror() const
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    PlanetSculptMirror out;
    // The sampler view points into service-owned page storage; safe because the mirror is read only
    // by TerrainPhysicsSystem, which runs after the edit systems in the same extraction wave on the
    // same thread (no concurrent mutation). The render-thread upload only READS the store under the
    // same mutex, so no writer tears it while physics holds the view.
    if (m_SphereSculpt.HasEdits())
        out.Sampler = m_SphereSculpt.MakeSampler();
    out.Version = m_SphereSculpt.Version() + m_SphereAnalyticVersion; // combined (S2) — matches SphereSculptVersion
    out.Faces = m_PlanetSculptFaces;
    out.Analytic = m_SphereAnalytic;
    out.AnalyticCount = m_SphereAnalyticCount;
    out.TransientDabs = m_SphereTransientDabs;
    out.TransientDabCount = m_SphereTransientDabCount;
    return out;
}

void TerrainService::ClearPlanetSculptDirtyFace(uint32 face)
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    if (face < kPlanetSculptFaceCount)
        m_PlanetSculptFaces[face] = PlanetSculptFaceRect{};
}

void TerrainService::BeginSphereSculptStrokeCapture()
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    m_SphereSculpt.BeginStrokeCapture();
}

std::vector<CBTTerrain::SphereSculptPageState> TerrainService::TakeSphereSculptStrokeCapture()
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    return m_SphereSculpt.TakeStrokeCapture();
}

std::vector<CBTTerrain::SphereSculptPageState> TerrainService::SnapshotSphereSculptPages(
    const std::vector<CBTTerrain::SphereSculptPageState>& keys) const
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    return m_SphereSculpt.SnapshotPages(keys);
}

void TerrainService::RestoreSphereSculptPages(
    const std::vector<CBTTerrain::SphereSculptPageState>& pages)
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    const CBTTerrain::SphereEditRegions regions = m_SphereSculpt.RestoreDabPages(pages);
    AccumulateSphereSculptDirtyFaces(regions);
    if (regions.Count > 0u)
        m_SphereSculptDabDirty = true; // a stroke undo/redo mutates the dab layer too
}

void TerrainService::SeedPlanetSculptMirrorForTests(float32 cx, float32 cy, float32 cz,
                                                    float32 angularRadius, float32 strength,
                                                    bool lower)
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    CBTTerrain::SphereEditRegions regions =
        m_SphereSculpt.ApplyDab(cx, cy, cz, angularRadius, strength, lower);
    AccumulateSphereSculptDirtyFaces(regions);
}

// ---- Sphere sculpt persistence (.tsculpt save/load) ----

bool TerrainService::SphereSculptNeedsSave() const
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    return m_SphereSculptDabDirty;
}

std::vector<uint8> TerrainService::EncodeSphereSculptBlob(uint64& outVersion) const
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    outVersion = m_SphereSculpt.Version();
    return CBTTerrain::EncodeSphereSculpt(m_SphereSculpt.Geometry(), m_SphereSculpt.ExportPages());
}

void TerrainService::MarkSphereSculptSaved(const GUID& guid, uint64 version)
{
    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    m_SphereSculptLoadedGuid = guid;
    m_SphereSculptSavedVersion = version;
    m_SphereSculptLoadFailedGuid = GUID{};
    // Clear the dab-dirty flag only when no dab landed since the encoded snapshot (all main
    // thread today, so the versions match; the guard keeps a future mid-save edit dirty).
    if (m_SphereSculpt.Version() == version)
        m_SphereSculptDabDirty = false;
}

bool TerrainService::RestoreSphereSculptFromBlob(const uint8* data, std::size_t size,
                                                 const GUID& guid)
{
    CBTTerrain::SphereSculptGeometry savedGeom{};
    std::vector<CBTTerrain::SphereSculptPageContent> pages;
    if (!CBTTerrain::DecodeSphereSculpt(data, size, savedGeom, pages))
        return false;

    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    // Restore at the SAVED grid with the LIVE pool count (the GPU pool ring is sized from the
    // live resolve, not the saving machine's). On the load path the store is empty (the planet
    // identity reset preceded us) so Configure adopts the grid freely; if an early modifier
    // bake already wrote content, Configure carries it across via the normal remap and the
    // import overwrites the saved pages wholesale — the next bake re-derives the modifier
    // layer either way (SET semantics). When the live component's radius derives a DIFFERENT
    // grid, the per-frame ConfigurePlanetSculpt afterwards remaps the restored content through
    // the same resize path a live radius edit takes — load composes with resize, no third path.
    CBTTerrain::SphereSculptGeometry geom = savedGeom;
    geom.PoolPageCount = CBTTerrain::ResolveSculptPagePoolCount();
    const CBTTerrain::SphereEditRegions remapped = m_SphereSculpt.Configure(geom);
    if (remapped.Count > 0u)
        AccumulateSphereSculptDirtyFaces(remapped);

    const CBTTerrain::SphereEditRegions imported = m_SphereSculpt.ImportPages(pages);
    AccumulateSphereSculptDirtyFaces(imported);

    m_SphereSculptLoadedGuid = guid;
    m_SphereSculptSavedVersion = m_SphereSculpt.Version();
    m_SphereSculptLoadFailedGuid = GUID{};
    m_SphereSculptDabDirty = false; // freshly loaded content carries no unsaved strokes
    Logger::Log::Info(
        "Sphere sculpt: restored {} authored page(s) at saved grid {}/face (pool {}) — load "
        "re-enters the edit path (re-tess + physics).",
        pages.size(), savedGeom.VirtualDim, geom.PoolPageCount);
    return true;
}

void TerrainService::EnsureSphereSculptLoaded(const GUID& guid)
{
    if (guid.IsNull())
        return;
    {
        std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
        if (m_SphereSculptLoadedGuid == guid)
            return;
        if (m_SphereSculptLoadFailedGuid == guid)
            return; // warned once already; a different guid re-arms the warning
    }

    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;

    // A resolve miss retries silently (a cheap registry lookup): at startup the scene can open
    // before the asset scan has registered the sidecar, and hard-caching that transient miss
    // would strand the planet unsculpted for the session. Only a real load/decode failure
    // (corrupt or unreadable file — it won't fix itself) negative-caches, warn-once.
    AssetMetadata meta{};
    if (!engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(guid, meta) || meta.Path.empty())
        return;

    BinaryAsset blob(guid, meta.Path, AssetType::TerrainSphereSculptData);
    if (blob.Load() && RestoreSphereSculptFromBlob(blob.GetData(), blob.GetDataSize(), guid))
        return;

    std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
    m_SphereSculptLoadFailedGuid = guid;
    Logger::Log::Warning(
        "Sphere sculpt payload {} ({}) failed to load/decode — the planet loads without its "
        "sculpted heights (the scene keeps the reference; re-saving the scene rewrites it)",
        guid.ToString(), meta.Path.string());
}

TerrainTileData* TerrainService::LoadTile(TiledTerrainHandle handle, TileCoord coord)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return nullptr;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return nullptr;

    auto& tiled = *slot.Data;

    // Already loaded?
    auto it = tiled.Tiles.find(coord);
    if (it != tiled.Tiles.end())
        return it->second.get();

    // Out of bounds?
    if (coord.X < 0 || coord.Z < 0 ||
        coord.X >= static_cast<int32>(tiled.Config.TilesPerAxisX) ||
        coord.Z >= static_cast<int32>(tiled.Config.TilesPerAxisZ))
        return nullptr;

    auto tile = std::make_unique<TerrainTileData>();
    tile->Coord = coord;
    tile->Config = tiled.Config.TileConfig;
    tile->WorldOriginX = tiled.WorldOriginX + coord.X * tiled.Config.TileWorldSize;
    tile->WorldOriginZ = tiled.WorldOriginZ + coord.Z * tiled.Config.TileWorldSize;

    const float32 tileWorld = tiled.Config.TileWorldSize;
    tile->Heightfield.Resize(tile->Config.HeightmapWidth, tile->Config.HeightmapHeight, 0.0f);

    // Fill from the terrain's base. It resolves per world position, so adjacent
    // tiles produce identical values at their shared boundary.
    FillTiledBaseRegion(tile->Heightfield, tiled.Config, tiled.WorldOriginX, tiled.WorldOriginZ,
                        tile->WorldOriginX, tile->WorldOriginZ, tileWorld, tileWorld, 0, 0,
                        static_cast<int32>(tile->Config.HeightmapWidth) - 1,
                        static_cast<int32>(tile->Config.HeightmapHeight) - 1);

    tile->MarkFullDirty();

    // Size the splatmap to the tile and leave it unbaked; the modifier bake that
    // follows a tile's arrival composites the surface rules over it. Until then the
    // tile reads as channel 0 rather than as a guess about its own materials.
    ResetSplatmap(tile->Heightfield, tile->Splatmap,
                  tile->SplatmapWidth, tile->SplatmapHeight);
    tile->SplatmapDirty = true;

    auto* ptr = tile.get();
    tiled.Tiles[coord] = std::move(tile);

    // Mark global quadtree and height range for rebuild.
    ++tiled.Revision;
    tiled.QuadtreeDirty = true;
    tiled.GlobalHeightRangeDirty = true;

    return ptr;
}

void TerrainService::UnloadTile(TiledTerrainHandle handle, TileCoord coord)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    if (slot.Data->Tiles.erase(coord) > 0)
    {
        ++slot.Data->Revision;
        slot.Data->QuadtreeDirty = true;
        slot.Data->GlobalHeightRangeDirty = true;
    }
}

bool TerrainService::IsTileLoaded(TiledTerrainHandle handle, TileCoord coord) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return false;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return false;

    return slot.Data->Tiles.count(coord) > 0;
}

void TerrainService::RebuildGlobalQuadtree(TiledTerrainHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    auto& tiled = *slot.Data;
    const auto& cfg = tiled.Config;

    // Each tile's finest level has tileFinestNodes nodes per axis.
    const uint32 tileFinestNodes = 1u << (cfg.TileConfig.LODLevels - 1);
    const uint32 maxTilesPerAxis = std::max(cfg.TilesPerAxisX, cfg.TilesPerAxisZ);

    // Find globalLODLevels such that 2^(G-1) >= tileFinestNodes * maxTilesPerAxis.
    const uint32 totalFinestNeeded = tileFinestNodes * maxTilesPerAxis;
    uint32 globalLODLevels = 1;
    while ((1u << (globalLODLevels - 1)) < totalFinestNeeded && globalLODLevels < Terrain::kMaxLODLevels)
        ++globalLODLevels;

    tiled.GlobalQuadtree.AllocateLevels(globalLODLevels);

    // Populate finest-level nodes from each loaded tile's heightfield.
    // Each finest-level node covers samplesPerNode heightfield samples.
    const uint32 samplesPerNodeX = std::max(1u, (cfg.TileConfig.HeightmapWidth - 1) / tileFinestNodes);
    const uint32 samplesPerNodeZ = std::max(1u, (cfg.TileConfig.HeightmapHeight - 1) / tileFinestNodes);

    for (auto& [coord, tilePtr] : tiled.Tiles)
    {
        const auto& hf = tilePtr->Heightfield;

        for (uint32 nz = 0; nz < tileFinestNodes; ++nz)
        {
            for (uint32 nx = 0; nx < tileFinestNodes; ++nx)
            {
                const int32 startX = static_cast<int32>(nx * samplesPerNodeX);
                const int32 startZ = static_cast<int32>(nz * samplesPerNodeZ);

                float32 minH, maxH;
                hf.GetMinMax(startX, startZ,
                             static_cast<int32>(samplesPerNodeX + 1),
                             static_cast<int32>(samplesPerNodeZ + 1),
                             minH, maxH);

                const uint32 globalNodeX = coord.X * tileFinestNodes + nx;
                const uint32 globalNodeZ = coord.Z * tileFinestNodes + nz;

                if (globalNodeX < tiled.GlobalQuadtree.GetNodesPerAxisAtLevel(0) &&
                    globalNodeZ < tiled.GlobalQuadtree.GetNodesPerAxisAtLevel(0))
                {
                    tiled.GlobalQuadtree.SetNodeMinMax(0, globalNodeX, globalNodeZ, minH, maxH);
                }
            }
        }
    }

    tiled.GlobalQuadtree.BuildCoarseLevelsFromChildren();
    tiled.QuadtreeDirty = false;
}

// ---- Async tile API (used by TileStreamingManager) ----

TerrainTileData* TerrainService::CreateEmptyTile(TiledTerrainHandle handle, TileCoord coord)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return nullptr;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return nullptr;

    auto& tiled = *slot.Data;

    // Already exists?
    auto it = tiled.Tiles.find(coord);
    if (it != tiled.Tiles.end())
        return it->second.get();

    // Out of bounds?
    if (coord.X < 0 || coord.Z < 0 ||
        coord.X >= static_cast<int32>(tiled.Config.TilesPerAxisX) ||
        coord.Z >= static_cast<int32>(tiled.Config.TilesPerAxisZ))
        return nullptr;

    auto tile = std::make_unique<TerrainTileData>();
    tile->Coord = coord;
    tile->Config = tiled.Config.TileConfig;
    tile->WorldOriginX = tiled.WorldOriginX + coord.X * tiled.Config.TileWorldSize;
    tile->WorldOriginZ = tiled.WorldOriginZ + coord.Z * tiled.Config.TileWorldSize;
    tile->LodState = TileLodState::Empty;
    tile->HeightfieldDirty = false; // No data yet

    auto* ptr = tile.get();
    tiled.Tiles[coord] = std::move(tile);
    ++tiled.Revision;
    return ptr;
}

void TerrainService::SetTileHeightfield(TiledTerrainHandle handle, TileCoord coord,
                                        Terrain::HeightfieldData&& heightfield,
                                        float32 minH, float32 maxH)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    auto it = slot.Data->Tiles.find(coord);
    if (it == slot.Data->Tiles.end() || !it->second)
        return;

    auto& tile = *it->second;
    const bool isFullRes = (heightfield.GetWidth() == tile.Config.HeightmapWidth &&
                            heightfield.GetHeight() == tile.Config.HeightmapHeight);

    tile.Heightfield = std::move(heightfield);
    tile.CachedMinH = minH;
    tile.CachedMaxH = maxH;
    // Streamed content replaced the heights wholesale; the modifier system's
    // per-block height range grid (if any was built by an earlier edit) is now
    // stale. Invalidate it so the next region range refresh rebuilds it from these
    // heights instead of patching a stale grid.
    tile.HeightBlockDim = 0;
    tile.LodState = isFullRes ? TileLodState::Full : TileLodState::Coarse;
    // Fresh streamed content carries the terrain's base only — the authored modifiers
    // must be (re)applied to it by the next modifier bake, scoped to this tile.
    tile.ModifiersApplied = false;
    tile.CoarseSplatBaked = false;
    tile.MarkFullDirty();
    ++slot.Data->Revision;
}

void TerrainService::SetTileMaps(TiledTerrainHandle handle, TileCoord coord,
                                 std::vector<uint8>&& splatmap, uint32 splatW, uint32 splatH,
                                 std::vector<uint8>&& normalmap, uint32 normW, uint32 normH)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    auto it = slot.Data->Tiles.find(coord);
    if (it == slot.Data->Tiles.end() || !it->second)
        return;

    auto& tile = *it->second;
    tile.Splatmap = std::move(splatmap);
    tile.SplatmapWidth = splatW;
    tile.SplatmapHeight = splatH;
    tile.SplatmapDirty = true;

    tile.Normalmap = std::move(normalmap);
    tile.NormalmapWidth = normW;
    tile.NormalmapHeight = normH;
    // The cook-generated normal is valid for the tile's current heights, so record its version. The
    // extraction only re-generates the normal (an O(tileRes^2) cost) when a later modifier bake bumps
    // HeightfieldVersion past this — a freshly-streamed tile skips the re-gen entirely.
    tile.NormalmapVersion = tile.HeightfieldVersion;
    ++slot.Data->Revision;
}

void TerrainService::PatchTileInGlobalQuadtree(TiledTerrainHandle handle, TileCoord coord)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    auto& tiled = *slot.Data;
    const auto& cfg = tiled.Config;

    auto tileIt = tiled.Tiles.find(coord);
    if (tileIt == tiled.Tiles.end() || !tileIt->second)
        return;

    if (!tiled.GlobalQuadtree.IsBuilt())
    {
        // Quadtree not yet allocated — do a full rebuild instead.
        // (This will be followed by FinalizeGlobalQuadtreeUpdates anyway.)
        tiled.QuadtreeDirty = true;
        return;
    }

    const uint32 tileFinestNodes = 1u << (cfg.TileConfig.LODLevels - 1);
    const uint32 samplesPerNodeX = std::max(1u, (cfg.TileConfig.HeightmapWidth - 1) / tileFinestNodes);
    const uint32 samplesPerNodeZ = std::max(1u, (cfg.TileConfig.HeightmapHeight - 1) / tileFinestNodes);

    const auto& hf = tileIt->second->Heightfield;

    for (uint32 nz = 0; nz < tileFinestNodes; ++nz)
    {
        for (uint32 nx = 0; nx < tileFinestNodes; ++nx)
        {
            const int32 startX = static_cast<int32>(nx * samplesPerNodeX);
            const int32 startZ = static_cast<int32>(nz * samplesPerNodeZ);

            float32 nodeMinH, nodeMaxH;
            hf.GetMinMax(startX, startZ,
                         static_cast<int32>(samplesPerNodeX + 1),
                         static_cast<int32>(samplesPerNodeZ + 1),
                         nodeMinH, nodeMaxH);

            const uint32 globalNodeX = coord.X * tileFinestNodes + nx;
            const uint32 globalNodeZ = coord.Z * tileFinestNodes + nz;

            if (globalNodeX < tiled.GlobalQuadtree.GetNodesPerAxisAtLevel(0) &&
                globalNodeZ < tiled.GlobalQuadtree.GetNodesPerAxisAtLevel(0))
            {
                tiled.GlobalQuadtree.SetNodeMinMax(0, globalNodeX, globalNodeZ, nodeMinH, nodeMaxH);
            }
        }
    }
}

void TerrainService::PatchTileInGlobalQuadtreePrecomputed(
    TiledTerrainHandle handle, TileCoord coord,
    const NodeMinMax* nodes, uint32 nodesPerAxis)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    auto& tiled = *slot.Data;

    if (!tiled.GlobalQuadtree.IsBuilt())
    {
        tiled.QuadtreeDirty = true;
        return;
    }

    const uint32 tileFinestNodes = 1u << (tiled.Config.TileConfig.LODLevels - 1);
    const uint32 actualNodes = std::min(nodesPerAxis, tileFinestNodes);

    for (uint32 nz = 0; nz < actualNodes; ++nz)
    {
        for (uint32 nx = 0; nx < actualNodes; ++nx)
        {
            const auto& node = nodes[nz * nodesPerAxis + nx];
            const uint32 globalNodeX = coord.X * tileFinestNodes + nx;
            const uint32 globalNodeZ = coord.Z * tileFinestNodes + nz;

            if (globalNodeX < tiled.GlobalQuadtree.GetNodesPerAxisAtLevel(0) &&
                globalNodeZ < tiled.GlobalQuadtree.GetNodesPerAxisAtLevel(0))
            {
                tiled.GlobalQuadtree.SetNodeMinMax(0, globalNodeX, globalNodeZ,
                                                    node.MinH, node.MaxH);
            }
        }
    }
}

void TerrainService::FinalizeGlobalQuadtreeUpdates(TiledTerrainHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (handle.Index >= m_TiledSlots.size())
        return;
    auto& slot = m_TiledSlots[handle.Index];
    if (!slot.Active || slot.Generation != handle.Generation)
        return;

    auto& tiled = *slot.Data;

    if (tiled.QuadtreeDirty)
    {
        // Full rebuild needed (first allocation or structural change).
        // Release lock and call the full rebuild which will re-acquire it.
        // Actually, since RebuildGlobalQuadtree also takes the lock, we need
        // to do the work inline here to avoid deadlock.
        const auto& cfg = tiled.Config;
        const uint32 tileFinestNodes = 1u << (cfg.TileConfig.LODLevels - 1);
        const uint32 maxTilesPerAxis = std::max(cfg.TilesPerAxisX, cfg.TilesPerAxisZ);
        const uint32 totalFinestNeeded = tileFinestNodes * maxTilesPerAxis;
        uint32 globalLODLevels = 1;
        while ((1u << (globalLODLevels - 1)) < totalFinestNeeded && globalLODLevels < Terrain::kMaxLODLevels)
            ++globalLODLevels;

        tiled.GlobalQuadtree.AllocateLevels(globalLODLevels);

        const uint32 samplesPerNodeX = std::max(1u, (cfg.TileConfig.HeightmapWidth - 1) / tileFinestNodes);
        const uint32 samplesPerNodeZ = std::max(1u, (cfg.TileConfig.HeightmapHeight - 1) / tileFinestNodes);

        for (auto& [coord, tilePtr] : tiled.Tiles)
        {
            if (!tilePtr || tilePtr->LodState == TileLodState::Empty)
                continue;

            const auto& hf = tilePtr->Heightfield;
            for (uint32 nz = 0; nz < tileFinestNodes; ++nz)
            {
                for (uint32 nx = 0; nx < tileFinestNodes; ++nx)
                {
                    float32 nodeMinH, nodeMaxH;
                    hf.GetMinMax(static_cast<int32>(nx * samplesPerNodeX),
                                 static_cast<int32>(nz * samplesPerNodeZ),
                                 static_cast<int32>(samplesPerNodeX + 1),
                                 static_cast<int32>(samplesPerNodeZ + 1),
                                 nodeMinH, nodeMaxH);

                    const uint32 globalNodeX = coord.X * tileFinestNodes + nx;
                    const uint32 globalNodeZ = coord.Z * tileFinestNodes + nz;

                    if (globalNodeX < tiled.GlobalQuadtree.GetNodesPerAxisAtLevel(0) &&
                        globalNodeZ < tiled.GlobalQuadtree.GetNodesPerAxisAtLevel(0))
                    {
                        tiled.GlobalQuadtree.SetNodeMinMax(0, globalNodeX, globalNodeZ, nodeMinH, nodeMaxH);
                    }
                }
            }
        }

        tiled.QuadtreeDirty = false;
        tiled.DirtyQuadtreeTiles.clear(); // full rebuild subsumes any pending patches
    }
    else if (!tiled.DirtyQuadtreeTiles.empty() && !tiled.GlobalQuadtree.IsBuilt())
    {
        // Edited tiles pending but no base quadtree to patch yet (edit before the
        // initial build): fall back to a full rebuild on the next sync.
        tiled.QuadtreeDirty = true;
    }
    else if (!tiled.DirtyQuadtreeTiles.empty())
    {
        // Incremental patch: only the tiles whose heights changed since the last
        // sync are rescanned. The per-tile node min/max computed here is identical
        // to the full-rebuild loop above, so untouched tiles' nodes stay exactly
        // as a full rebuild would leave them — no O(all-samples) rescan per dab.
        const auto& cfg = tiled.Config;
        const uint32 tileFinestNodes = 1u << (cfg.TileConfig.LODLevels - 1);
        const uint32 samplesPerNodeX = std::max(1u, (cfg.TileConfig.HeightmapWidth - 1) / tileFinestNodes);
        const uint32 samplesPerNodeZ = std::max(1u, (cfg.TileConfig.HeightmapHeight - 1) / tileFinestNodes);
        const uint32 nodesPerAxis = tiled.GlobalQuadtree.GetNodesPerAxisAtLevel(0);

        for (const TileCoord& coord : tiled.DirtyQuadtreeTiles)
        {
            auto tileIt = tiled.Tiles.find(coord);
            if (tileIt == tiled.Tiles.end() || !tileIt->second ||
                tileIt->second->LodState == TileLodState::Empty)
                continue;

            const auto& hf = tileIt->second->Heightfield;
            for (uint32 nz = 0; nz < tileFinestNodes; ++nz)
            {
                for (uint32 nx = 0; nx < tileFinestNodes; ++nx)
                {
                    float32 nodeMinH, nodeMaxH;
                    hf.GetMinMax(static_cast<int32>(nx * samplesPerNodeX),
                                 static_cast<int32>(nz * samplesPerNodeZ),
                                 static_cast<int32>(samplesPerNodeX + 1),
                                 static_cast<int32>(samplesPerNodeZ + 1),
                                 nodeMinH, nodeMaxH);

                    const uint32 globalNodeX = coord.X * tileFinestNodes + nx;
                    const uint32 globalNodeZ = coord.Z * tileFinestNodes + nz;

                    if (globalNodeX < nodesPerAxis && globalNodeZ < nodesPerAxis)
                        tiled.GlobalQuadtree.SetNodeMinMax(0, globalNodeX, globalNodeZ, nodeMinH, nodeMaxH);
                }
            }
        }
        tiled.DirtyQuadtreeTiles.clear();
    }

    tiled.GlobalQuadtree.BuildCoarseLevelsFromChildren();
}

} // namespace GameEngine::TerrainECS
