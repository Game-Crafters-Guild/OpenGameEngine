// Compile the actual scheduling, PNG publication, slot release, import release, idle and
// folder-bake queue methods. Only the device,
// registry and job host are deterministic substitutes; no copy of the slot loop
// or persistence implementation lives here. Reintroducing its whole-sweep return
// must fail this test. This is not a live GPU/orbit integration test.
#include "Thumbnails/ModelThumbnailCachePolicy.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <cmath>
#include <filesystem>
#include <future>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace ThumbnailPersistenceHost
{
using GameEngine::GUID;
namespace ThumbnailCachePolicy = GameEngine::ThumbnailCachePolicy;
namespace Rendering
{
enum class TextureFormat { RGBA8_UNORM, RGBA8_SRGB, R16G16B16A16_FLOAT };
enum class ResourceState { ShaderResource };
struct IDevice {};
struct ViewReadbackResult
{
    std::vector<uint8_t> pixels;
    uint32_t width = 4, height = 4;
    TextureFormat format = TextureFormat::RGBA8_UNORM;
};
struct Ticket
{
    // Every byte of the readback; 0 makes a frame with no covered pixel.
    inline static uint8_t s_Fill = 255;
    bool IsConsumed() const { return false; }
    bool TryGet(ViewReadbackResult& result)
    {
        result.pixels.assign(4 * 4 * 4, s_Fill);
        return true;
    }
};
namespace RenderGraph
{
struct RGFrame
{
    int importedTexture = -1;
    // The read-back slot is put back in its sampled state after the copy (the real capture's
    // contract, RequestDeviceTextureReadbackRG's MarkOutput of the declared resting state).
    bool slotRestoredAfterCopy = false;
};
}
std::shared_ptr<Ticket> RequestDeviceTextureReadbackRG(IDevice*, RenderGraph::RGFrame& frame, int texture,
                                                     ResourceState restingState, const char*)
{
    frame.importedTexture = texture;
    frame.slotRestoredAfterCopy = restingState == ResourceState::ShaderResource;
    return std::make_shared<Ticket>();
}
}
namespace UI
{
struct UITextureSpace { static UITextureSpace DisplayLinearSdr() { return {}; } };
bool IsEncodedAtRest(UITextureSpace) { return false; }
}
namespace Logger
{
struct Log
{
    template<class... T> static void Warning(const char*, T&&...) {}
    template<class... T> static void Info(const char*, T&&...) {}
};
}
namespace JobSystem { enum class JobPriority { Background }; }
struct EngineCore
{
    static EngineCore& GetInstance() { static EngineCore instance; return instance; }
    EngineCore& GetJobSystem() { return *this; }
    template<class F> void EnqueueWork(F&& work, JobSystem::JobPriority) { work(); }
};
namespace FileSystem
{
std::filesystem::path MakeTemporarySiblingPath(const std::filesystem::path& path)
{
    return path.string() + ".pending";
}
bool PublishFile(const std::filesystem::path& from, const std::filesystem::path& to)
{
    std::error_code error;
    std::filesystem::rename(from, to, error);
    return !error;
}
}
struct AssetMetadata { std::filesystem::path Path; };
struct Registry
{
    std::filesystem::path source;
    bool TryGetAssetMetadata(const GUID&, AssetMetadata& meta) { meta.Path = source; return true; }
};
struct Asset {};
struct AssetManager
{
    Registry registry;
    Registry& GetRegistry() { return registry; }
    std::vector<GUID> releasedTransient;
    std::unordered_set<GUID> loadSuppressed;
    bool IsLoadSuppressed(const GUID& guid) const { return loadSuppressed.contains(guid); }
    bool ReleaseTransientAsset(const GUID& guid)
    {
        releasedTransient.push_back(guid);
        return true;
    }
};
struct RenderServices
{
    Rendering::IDevice device;
    Rendering::IDevice* GetDevice() { return &device; }
};
constexpr uint32_t s_ThumbnailResolutionPx = 4;
constexpr uint32_t kDiskCachePngLongEdge = 256;
GUID s_OrbitFocusGuid;
GUID s_MaterialOrbitFocusGuid;
GUID s_LensFlareFocusGuid;
bool s_MaterialOrbitFocusIblEnabled = true;
GUID s_PendingMaterialFocusGuid;
bool s_PendingMaterialFocusIblEnabled = true;
std::chrono::steady_clock::time_point s_PendingMaterialFocusSince;

struct ModelThumbnailHandler
{
    struct Slot
    {
        GUID guid;
        bool occupied = true, isMaterial = false, isLensFlare = false;
        bool ready = true, inFlight = false, deviceTexInitialized = true, showsBakedImage = false;
        bool bakeOnly = false, restoresBakedImage = false;
        std::chrono::steady_clock::time_point bakeClockStart;
        bool previewIblEnabled = true, materialRenderSettled = false, renderGaveUp = false;
        uint32_t texWidth = 4, texHeight = 4;
        int deviceTex = 0;
        std::shared_future<std::shared_ptr<Asset>> modelImport;
    };
    static constexpr size_t kMaxResidentSlots = 256;
    static constexpr uint32_t kQuietTicksBeforeIdle = 120;
    static constexpr size_t kMaxBakeItemsInspectedPerTick = 8;
    static constexpr size_t kFirstQueueLane = 1;
    static constexpr size_t kRenderLaneCount = 4;
    struct RenderLane
    {
        GUID spawnedModelGuid;
        size_t inFlightSlot = kMaxResidentSlots;
        uint32_t spawnedSettleFrames = 0;
        const GUID& SpawnedGuid() const { return spawnedModelGuid; }
    };
    struct WindowState
    {
        GUID spawnedModelGuid;
        uint32_t spawnedSettleFrames = 0;
        std::vector<Slot> slots;
        std::unordered_map<GUID, size_t> guidToSlot;
        std::vector<RenderLane> lanes;
        std::vector<GUID> pending;
        uint32_t quietTicks = 0;
        std::unordered_map<GUID, size_t> guidToMaterialSlot, guidToMaterialNoIblSlot;
        bool IsModelSettling(const GUID& guid) const
        {
            return ThumbnailCachePolicy::IsSpawnSettling(guid, spawnedModelGuid, spawnedSettleFrames);
        }
    };
    struct PendingDiskCache
    {
        GUID guid;
        std::filesystem::path outputPath, sourcePath;
        std::shared_ptr<Rendering::Ticket> ticket;
        UI::UITextureSpace srcSpace;
        uint32_t maxPngLongEdge;
    };
    std::filesystem::path m_DiskCacheRoot;
    RenderServices* m_RenderServices;
    AssetManager* m_AssetManager;
    std::optional<PendingDiskCache> m_PendingDiskCache;
    std::unordered_set<GUID> m_DiskCacheRequested;
    std::filesystem::path ComputeModelCachePath(const GUID& guid) const
    {
        return m_DiskCacheRoot / (guid.ToString() + ".png");
    }
    std::filesystem::path ComputeMaterialCachePath(const GUID& guid, bool previewIblEnabled) const
    {
        return m_DiskCacheRoot / ("material-" + guid.ToString() + (previewIblEnabled ? "" : "-noibl") + ".png");
    }
    static bool IsEphemeralPreviewMaterialPath(const std::filesystem::path&) { return false; }
    void MaybeStartDiskCacheReadbackRG(WindowState&, Rendering::RenderGraph::RGFrame&);
    void PromotePendingMaterialFocus();
    void PollDiskCacheReadback();
    static bool IsFullyTransparent(const std::vector<uint8_t>& pixels, Rendering::TextureFormat format);
    // The handler's policy: retry an asset's first empty capture only.
    std::unordered_set<GUID> emptyRetried;
    bool RetryEmptyCapture(const GUID& guid) { return emptyRetried.insert(guid).second; }

    std::map<uint64_t, WindowState> m_Windows;
    mutable std::mutex m_ServedByPngMutex;
    std::vector<GUID> m_ServedByPng;
    void ReleaseGridSlotServedByPng(const GUID& guid);
    size_t PendingGridSlotReleaseCount() const;
    void ReleaseGridSlotsServedByPng();
    // What the slot release asked of the production teardown it calls.
    std::vector<GUID> droppedBakedImages;
    std::vector<size_t> releasedSlots;
    void DropBakedImage(WindowState&, Slot& slot) { droppedBakedImages.push_back(slot.guid); }
    void ReleaseSlotAssignment(WindowState& ws, size_t slotIdx)
    {
        ws.guidToSlot.erase(ws.slots[slotIdx].guid);
        releasedSlots.push_back(slotIdx);
    }

    struct DeferredModelRelease
    {
        GUID guid;
        std::shared_future<std::shared_ptr<Asset>> modelImport;
    };
    std::vector<DeferredModelRelease> m_DeferredModelReleases;
    bool ReleaseSlotModel(Slot& slot);
    bool ReleaseModelImport(const GUID& guid, std::shared_future<std::shared_ptr<Asset>>& modelImport);
    bool IsModelImportHeld(const GUID& guid) const;
    void DeferModelRelease(Slot& slot);
    void ReleaseDeferredModelImports();
    void ReleaseAllModelImports();
    void TeardownSpawnedModel(RenderLane& lane) { lane.spawnedModelGuid = GUID{}; }

    bool HasSlotWork(WindowState& ws, bool imagesArrived);

    struct BakeItem
    {
        GUID guid;
        bool isMaterial = false;
    };
    struct BakeBatch
    {
        size_t skipped = 0;
        std::vector<GUID> failed;
        bool active = false;
    };
    std::deque<BakeItem> m_BakeQueue;
    std::unordered_set<GUID> m_BakeQueued;
    BakeBatch m_BakeBatch;
    static bool GetPreviewIblEnabled() { return true; }
    // Every call stands for the two file queries the production check makes.
    mutable size_t pngQueries = 0;
    bool HasCurrentCachedPng(const GUID&, bool) const
    {
        ++pngQueries;
        return true;
    }
    size_t AcquireSlotForGuid(WindowState& ws, const GUID& guid, bool = false, bool = false,
                              std::nullopt_t = std::nullopt)
    {
        if (const auto it = ws.guidToSlot.find(guid); it != ws.guidToSlot.end())
            return it->second;
        for (size_t i = 0; i < ws.slots.size(); ++i)
        {
            if (!ws.slots[i].occupied)
            {
                ws.slots[i] = Slot{};
                ws.slots[i].guid = guid;
                ws.slots[i].ready = false;
                ws.guidToSlot[guid] = i;
                return i;
            }
        }
        return kMaxResidentSlots;
    }
    void EnqueuePending(WindowState& ws, const GUID& guid, bool, bool = false, std::nullopt_t = std::nullopt)
    {
        ws.pending.push_back(guid);
    }
    std::vector<GUID> importedModels;
    void AcquireSlotModel(Slot&, const GUID& guid) { importedModels.push_back(guid); }
    void RequestModelTile(WindowState& ws, const GUID& guid, bool listStatic);
    void StartQueuedBakes(WindowState& ws);
    void NoteBakeFailure(const GUID& guid) { m_BakeBatch.failed.push_back(guid); }
};
#include "ThumbnailCacheScheduleExtracted.h"
#include "ThumbnailCachePersistExtracted.h"
#include "ThumbnailCacheReleaseExtracted.h"
#include "ThumbnailCacheImportsExtracted.h"
#include "ThumbnailCacheIdleExtracted.h"
#include "ThumbnailCacheBakesExtracted.h"
}

TEST(ThumbnailPersistence, ReadyGridSlotPublishesPngWhileAnotherModelIsUnsettled)
{
    using namespace ThumbnailPersistenceHost;
    const auto root = std::filesystem::temp_directory_path() / ("thumb-persist-" + GUID::Generate().ToString());
    struct Cleanup
    {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); s_OrbitFocusGuid = GUID{}; }
    } cleanup{root};
    std::filesystem::create_directories(root);
    AssetManager assets;
    assets.registry.source = root / "source.gltf";
    std::ofstream(assets.registry.source) << "{}";
    RenderServices services;
    ModelThumbnailHandler handler{root / "cache", &services, &assets, {}, {}};
    ModelThumbnailHandler::WindowState window;
    const GUID focused = GUID::Generate(), ready = GUID::Generate();
    window.spawnedModelGuid = focused;
    s_OrbitFocusGuid = focused;
    ModelThumbnailHandler::Slot first, second;
    first.guid = focused; first.deviceTex = 1;
    second.guid = ready; second.deviceTex = 2;
    window.slots = {first, second};
    Rendering::RenderGraph::RGFrame frame;
    handler.MaybeStartDiskCacheReadbackRG(window, frame);
    handler.PollDiskCacheReadback();
    EXPECT_EQ(frame.importedTexture, 2);
    EXPECT_FALSE(std::filesystem::exists(handler.ComputeModelCachePath(focused)));
    ASSERT_TRUE(std::filesystem::exists(handler.ComputeModelCachePath(ready)));
    int width = 0, height = 0, channels = 0;
    auto* pixels = stbi_load(handler.ComputeModelCachePath(ready).string().c_str(), &width, &height, &channels, 4);
    ASSERT_NE(pixels, nullptr);
    EXPECT_EQ(width, 4); EXPECT_EQ(height, 4);
    EXPECT_EQ(pixels[0], 255); EXPECT_EQ(pixels[3], 255);
    stbi_image_free(pixels);
}

// A material tile is cached only from a settled render: its first renders can
// miss the pass variant, and the focused slot shows the orbiting preview.
TEST(ThumbnailPersistence, OnlySettledUnfocusedMaterialSlotPublishesPng)
{
    using namespace ThumbnailPersistenceHost;
    const auto root = std::filesystem::temp_directory_path() / ("thumb-persist-" + GUID::Generate().ToString());
    struct Cleanup
    {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); s_MaterialOrbitFocusGuid = GUID{}; }
    } cleanup{root};
    std::filesystem::create_directories(root);
    AssetManager assets;
    assets.registry.source = root / "surface.material";
    std::ofstream(assets.registry.source) << "{}";
    RenderServices services;
    ModelThumbnailHandler handler{root / "cache", &services, &assets, {}, {}};
    ModelThumbnailHandler::WindowState window;
    const GUID unsettled = GUID::Generate(), focused = GUID::Generate(), settled = GUID::Generate();
    s_MaterialOrbitFocusGuid = focused;
    ModelThumbnailHandler::Slot first, second, third;
    first.guid = unsettled; first.deviceTex = 1; first.isMaterial = true;
    second.guid = focused; second.deviceTex = 2; second.isMaterial = true; second.materialRenderSettled = true;
    third.guid = settled; third.deviceTex = 3; third.isMaterial = true; third.materialRenderSettled = true;
    window.slots = {first, second, third};
    Rendering::RenderGraph::RGFrame frame;
    handler.MaybeStartDiskCacheReadbackRG(window, frame);
    handler.PollDiskCacheReadback();
    EXPECT_EQ(frame.importedTexture, 3);
    EXPECT_FALSE(std::filesystem::exists(handler.ComputeMaterialCachePath(unsettled, true)));
    EXPECT_FALSE(std::filesystem::exists(handler.ComputeMaterialCachePath(focused, true)));
    EXPECT_TRUE(std::filesystem::exists(handler.ComputeMaterialCachePath(settled, true)));
}

// A material the Asset View focuses before its tile was ever cached keeps rendering the
// square tile until that tile's PNG is written, then takes the focus: the focused,
// panel-shaped render is never cached, and the tile and the preview share one slot.
TEST(ThumbnailPersistence, AMaterialFocusedBeforeItsFirstBakeWritesItsTileFirst)
{
    using namespace ThumbnailPersistenceHost;
    const auto root = std::filesystem::temp_directory_path() / ("thumb-persist-" + GUID::Generate().ToString());
    struct Cleanup
    {
        std::filesystem::path root;
        ~Cleanup()
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
            s_MaterialOrbitFocusGuid = GUID{};
            s_PendingMaterialFocusGuid = GUID{};
        }
    } cleanup{root};
    std::filesystem::create_directories(root);
    AssetManager assets;
    assets.registry.source = root / "gold.material";
    std::ofstream(assets.registry.source) << "{}";
    RenderServices services;
    ModelThumbnailHandler handler{root / "cache", &services, &assets, {}, {}};
    ModelThumbnailHandler::WindowState window;
    const GUID gold = GUID::Generate();
    s_PendingMaterialFocusGuid = gold;
    s_PendingMaterialFocusSince = std::chrono::steady_clock::now();
    ModelThumbnailHandler::Slot slot;
    slot.guid = gold; slot.deviceTex = 1; slot.isMaterial = true; slot.materialRenderSettled = true;
    window.slots = {slot};

    Rendering::RenderGraph::RGFrame frame;
    handler.MaybeStartDiskCacheReadbackRG(window, frame);
    handler.PollDiskCacheReadback();
    EXPECT_EQ(frame.importedTexture, 1) << "the waiting focus leaves the tile cacheable";
    EXPECT_TRUE(std::filesystem::exists(handler.ComputeMaterialCachePath(gold, true)));
    EXPECT_TRUE(s_MaterialOrbitFocusGuid.IsNull()) << "no focus before the tile is taken";

    Rendering::RenderGraph::RGFrame next;
    handler.MaybeStartDiskCacheReadbackRG(window, next);
    EXPECT_EQ(s_MaterialOrbitFocusGuid, gold) << "the focus follows once the tile is written";
    EXPECT_TRUE(s_PendingMaterialFocusGuid.IsNull());
}

// A focused material whose tile is never cached (nothing can render it) does not keep the
// Asset View waiting: past the limit the focus is taken without the PNG; within it, it waits.
TEST(ThumbnailPersistence, AMaterialFocusTakesFocusWithoutItsTileAfterTheLimit)
{
    using namespace ThumbnailPersistenceHost;
    const auto root = std::filesystem::temp_directory_path() / ("thumb-persist-" + GUID::Generate().ToString());
    struct Cleanup
    {
        std::filesystem::path root;
        ~Cleanup()
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
            s_MaterialOrbitFocusGuid = GUID{};
            s_PendingMaterialFocusGuid = GUID{};
        }
    } cleanup{root};
    std::filesystem::create_directories(root);
    AssetManager assets;
    RenderServices services;
    ModelThumbnailHandler handler{root / "cache", &services, &assets, {}, {}};
    ModelThumbnailHandler::WindowState window;
    const GUID gold = GUID::Generate();
    Rendering::RenderGraph::RGFrame frame;

    s_PendingMaterialFocusGuid = gold;
    s_PendingMaterialFocusSince = std::chrono::steady_clock::now();
    handler.MaybeStartDiskCacheReadbackRG(window, frame);
    EXPECT_TRUE(s_MaterialOrbitFocusGuid.IsNull()) << "within the limit the focus waits for the tile";
    EXPECT_EQ(s_PendingMaterialFocusGuid, gold);

    s_PendingMaterialFocusSince = std::chrono::steady_clock::now() - std::chrono::seconds(4);
    handler.MaybeStartDiskCacheReadbackRG(window, frame);
    EXPECT_EQ(s_MaterialOrbitFocusGuid, gold) << "past the limit the focus is taken without the PNG";
    EXPECT_TRUE(s_PendingMaterialFocusGuid.IsNull());
}

// A render made after the readiness wait gave up does not show the model, so it
// is never cached; the ready slot beside it still is.
TEST(ThumbnailPersistence, RenderThatGaveUpIsNotCached)
{
    using namespace ThumbnailPersistenceHost;
    const auto root = std::filesystem::temp_directory_path() / ("thumb-persist-" + GUID::Generate().ToString());
    struct Cleanup
    {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } cleanup{root};
    std::filesystem::create_directories(root);
    AssetManager assets;
    assets.registry.source = root / "source.gltf";
    std::ofstream(assets.registry.source) << "{}";
    RenderServices services;
    ModelThumbnailHandler handler{root / "cache", &services, &assets, {}, {}};
    ModelThumbnailHandler::WindowState window;
    ModelThumbnailHandler::Slot gaveUp, ready;
    gaveUp.guid = GUID::Generate(); gaveUp.deviceTex = 1; gaveUp.renderGaveUp = true;
    ready.guid = GUID::Generate(); ready.deviceTex = 2;
    window.slots = {gaveUp, ready};
    Rendering::RenderGraph::RGFrame frame;
    handler.MaybeStartDiskCacheReadbackRG(window, frame);
    handler.PollDiskCacheReadback();
    EXPECT_EQ(frame.importedTexture, 2);
    EXPECT_FALSE(std::filesystem::exists(handler.ComputeModelCachePath(gaveUp.guid)));
    EXPECT_TRUE(std::filesystem::exists(handler.ComputeModelCachePath(ready.guid)));
}

// A frame with no covered pixel is not persisted the first time: the asset
// renders again, and only a second empty frame is written.
TEST(ThumbnailPersistence, EmptyCaptureIsPersistedOnlyWhenItRepeats)
{
    using namespace ThumbnailPersistenceHost;
    const auto root = std::filesystem::temp_directory_path() / ("thumb-persist-" + GUID::Generate().ToString());
    struct Cleanup
    {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); Rendering::Ticket::s_Fill = 255; }
    } cleanup{root};
    std::filesystem::create_directories(root);
    AssetManager assets;
    assets.registry.source = root / "source.gltf";
    std::ofstream(assets.registry.source) << "{}";
    RenderServices services;
    ModelThumbnailHandler handler{root / "cache", &services, &assets, {}, {}};
    ModelThumbnailHandler::WindowState window;
    ModelThumbnailHandler::Slot slot;
    slot.guid = GUID::Generate();
    slot.deviceTex = 1;
    window.slots = {slot};
    Rendering::Ticket::s_Fill = 0;
    Rendering::RenderGraph::RGFrame frame;

    handler.MaybeStartDiskCacheReadbackRG(window, frame);
    handler.PollDiskCacheReadback();
    EXPECT_FALSE(std::filesystem::exists(handler.ComputeModelCachePath(slot.guid)));

    handler.m_DiskCacheRequested.erase(slot.guid);
    handler.MaybeStartDiskCacheReadbackRG(window, frame);
    handler.PollDiskCacheReadback();
    EXPECT_TRUE(std::filesystem::exists(handler.ComputeModelCachePath(slot.guid)));
}

// A grid tile the browser shows from its PNG gives its slot back, so the tile
// holds one UI texture, never the slot's and the PNG's (the registry filled at
// 512 with two per tile). The focused preview, a render in flight and a folder
// bake keep their slots.
TEST(ThumbnailSlotRelease, TileServedByItsPngFreesItsGridSlot)
{
    using namespace ThumbnailPersistenceHost;
    struct Cleanup
    {
        ~Cleanup() { s_OrbitFocusGuid = GUID{}; }
    } cleanup;
    ModelThumbnailHandler handler{};
    ModelThumbnailHandler::WindowState window;
    window.slots.resize(4);
    for (size_t i = 0; i < window.slots.size(); ++i)
    {
        window.slots[i].guid = GUID::Generate();
        window.guidToSlot[window.slots[i].guid] = i;
    }
    window.slots[0].showsBakedImage = true;
    s_OrbitFocusGuid = window.slots[1].guid;
    window.slots[2].inFlight = true;
    window.slots[3].bakeOnly = true;
    const GUID notResident = GUID::Generate();
    handler.m_Windows[1] = window;
    for (const auto& slot : window.slots)
        handler.ReleaseGridSlotServedByPng(slot.guid);
    handler.ReleaseGridSlotServedByPng(notResident);

    handler.ReleaseGridSlotsServedByPng();

    const auto& slots = handler.m_Windows[1].slots;
    EXPECT_EQ(handler.releasedSlots, std::vector<size_t>{0});
    EXPECT_EQ(handler.droppedBakedImages, std::vector<GUID>{slots[0].guid});
    EXPECT_FALSE(slots[0].occupied);
    EXPECT_TRUE(slots[1].occupied && slots[2].occupied && slots[3].occupied);
    EXPECT_TRUE(handler.m_ServedByPng.empty());
}

namespace
{
std::shared_future<std::shared_ptr<ThumbnailPersistenceHost::Asset>> ImportedModel()
{
    std::promise<std::shared_ptr<ThumbnailPersistenceHost::Asset>> promise;
    promise.set_value(std::make_shared<ThumbnailPersistenceHost::Asset>());
    return promise.get_future().share();
}
} // namespace

// A slot that moves on while a lane renders its model keeps the transient
// import until the lane finishes, then unloads it once; dropping the import
// left the model resident for the session.
TEST(ThumbnailImportRelease, SlotReleasedMidRenderUnloadsItsModelOnceTheLaneFinishes)
{
    using namespace ThumbnailPersistenceHost;
    AssetManager assets;
    ModelThumbnailHandler handler{};
    handler.m_AssetManager = &assets;
    auto& window = handler.m_Windows[1];
    window.slots.resize(1);
    window.slots[0].guid = GUID::Generate();
    window.slots[0].modelImport = ImportedModel();
    window.lanes.push_back({window.slots[0].guid, 0});
    const GUID model = window.slots[0].guid;

    ASSERT_FALSE(handler.ReleaseSlotModel(window.slots[0]));
    handler.DeferModelRelease(window.slots[0]);
    EXPECT_FALSE(window.slots[0].modelImport.valid());
    handler.ReleaseDeferredModelImports();
    EXPECT_TRUE(assets.releasedTransient.empty()) << "the lane still renders the model";

    window.lanes[0].inFlightSlot = ModelThumbnailHandler::kMaxResidentSlots;
    handler.ReleaseDeferredModelImports();
    EXPECT_EQ(assets.releasedTransient, std::vector<GUID>{model});
    EXPECT_TRUE(handler.m_DeferredModelReleases.empty());
}

// A project switch lets go of every slot's import and unloads each model once,
// including one that two windows imported.
TEST(ThumbnailImportRelease, ProjectResetUnloadsEveryImportedModel)
{
    using namespace ThumbnailPersistenceHost;
    AssetManager assets;
    ModelThumbnailHandler handler{};
    handler.m_AssetManager = &assets;
    const GUID shared = GUID::Generate(), single = GUID::Generate();
    for (uint64_t windowId : {1u, 2u})
    {
        auto& window = handler.m_Windows[windowId];
        window.slots.resize(2);
        window.slots[0].guid = shared;
        window.slots[0].modelImport = ImportedModel();
        window.slots[1].guid = windowId == 1 ? single : GUID::Generate();
        if (windowId == 1)
            window.slots[1].modelImport = ImportedModel();
    }

    handler.ReleaseAllModelImports();

    std::vector<GUID> released = assets.releasedTransient;
    std::vector<GUID> expected{shared, single};
    std::sort(released.begin(), released.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(released, expected);
    for (const auto& [windowId, window] : handler.m_Windows)
    {
        for (const auto& slot : window.slots)
            EXPECT_FALSE(slot.modelImport.valid());
    }
}

// A window with nothing queued, rendering, settling, focused or arriving stops
// its per-slot passes once its last activity is kQuietTicksBeforeIdle ticks
// old, and any new work starts them again at once.
TEST(ThumbnailIdle, WindowWithNothingToDoStopsItsSlotPasses)
{
    using namespace ThumbnailPersistenceHost;
    ModelThumbnailHandler handler{};
    ModelThumbnailHandler::WindowState window;
    window.lanes.resize(ModelThumbnailHandler::kRenderLaneCount);
    for (uint32_t tick = 0; tick < ModelThumbnailHandler::kQuietTicksBeforeIdle; ++tick)
        ASSERT_TRUE(handler.HasSlotWork(window, false)) << "tick " << tick;
    EXPECT_FALSE(handler.HasSlotWork(window, false));
    EXPECT_FALSE(handler.HasSlotWork(window, false));

    EXPECT_TRUE(handler.HasSlotWork(window, true)) << "a decoded PNG arrived";
    window.quietTicks = ModelThumbnailHandler::kQuietTicksBeforeIdle;
    window.pending.push_back(GUID::Generate());
    EXPECT_TRUE(handler.HasSlotWork(window, false)) << "a request is queued";
    window.pending.clear();
    window.quietTicks = ModelThumbnailHandler::kQuietTicksBeforeIdle;
    window.lanes[1].inFlightSlot = 0;
    EXPECT_TRUE(handler.HasSlotWork(window, false)) << "a lane renders";
    window.lanes[1].inFlightSlot = ModelThumbnailHandler::kMaxResidentSlots;
    EXPECT_TRUE(handler.HasSlotWork(window, false)) << "the quiet count restarts after work";
}

// A folder bake whose PNGs are current drains a bounded number of entries per
// tick, and asks the disk only about the ones this session has not seen
// written or served.
TEST(ThumbnailBakeQueue, SkippingCurrentPngsIsBoundedPerTickAndQueriesOnlyUnknownOnes)
{
    using namespace ThumbnailPersistenceHost;
    ModelThumbnailHandler handler{};
    handler.m_DiskCacheRoot = std::filesystem::temp_directory_path();
    ModelThumbnailHandler::WindowState window;
    window.slots.resize(2);
    window.slots[1].occupied = false;
    constexpr size_t kItems = 300;
    size_t unknown = 0;
    for (size_t i = 0; i < kItems; ++i)
    {
        const GUID guid = GUID::Generate();
        handler.m_BakeQueue.push_back({guid, false});
        handler.m_BakeQueued.insert(guid);
        if (i % 2 == 0)
            handler.m_DiskCacheRequested.insert(guid);
        else
            ++unknown;
    }

    handler.StartQueuedBakes(window);
    EXPECT_EQ(handler.m_BakeQueue.size(), kItems - ModelThumbnailHandler::kMaxBakeItemsInspectedPerTick);
    EXPECT_EQ(handler.m_BakeBatch.skipped, ModelThumbnailHandler::kMaxBakeItemsInspectedPerTick);
    EXPECT_EQ(handler.pngQueries, ModelThumbnailHandler::kMaxBakeItemsInspectedPerTick / 2);

    while (!handler.m_BakeQueue.empty())
        handler.StartQueuedBakes(window);
    EXPECT_EQ(handler.m_BakeBatch.skipped, kItems);
    EXPECT_EQ(handler.pngQueries, unknown);
    EXPECT_TRUE(window.pending.empty()) << "a current PNG is never baked again";
}

// A model with a visible tile is counted by that tile's outcome: a failed bake
// when the model does not load or its render gave up, current when its PNG is
// written, and not counted yet (asked again later) while the tile still draws.
TEST(ThumbnailBakeQueue, VisibleTileCountsByItsOutcome)
{
    using namespace ThumbnailPersistenceHost;
    AssetManager assets;
    ModelThumbnailHandler handler{};
    handler.m_AssetManager = &assets;
    handler.m_DiskCacheRoot = std::filesystem::temp_directory_path();
    ModelThumbnailHandler::WindowState window;
    window.slots.resize(5);
    window.slots[4].occupied = false;
    for (size_t i = 0; i < 4; ++i)
    {
        window.slots[i].guid = GUID::Generate();
        window.guidToSlot[window.slots[i].guid] = i;
        handler.m_BakeQueue.push_back({window.slots[i].guid, false});
    }
    assets.loadSuppressed.insert(window.slots[0].guid);
    window.slots[1].renderGaveUp = true;
    handler.m_DiskCacheRequested.insert(window.slots[2].guid);
    window.slots[3].ready = false;

    handler.StartQueuedBakes(window);

    EXPECT_EQ(handler.m_BakeBatch.failed, (std::vector<GUID>{window.slots[0].guid, window.slots[1].guid}));
    EXPECT_EQ(handler.m_BakeBatch.skipped, 1u);
    ASSERT_EQ(handler.m_BakeQueue.size(), 1u) << "the tile still drawing is asked about again";
    EXPECT_EQ(handler.m_BakeQueue.front().guid, window.slots[3].guid);
    EXPECT_TRUE(window.pending.empty());
}

// A tile freed because the browser showed its PNG and then bound to its slot
// again (the UI asks for the engine name) gets the baked PNG back, with no new
// import and no render; a model whose PNG this session has not seen renders.
TEST(ThumbnailSlotRelease, ReleasedTileBoundAgainRestoresItsPng)
{
    using namespace ThumbnailPersistenceHost;
    ModelThumbnailHandler handler{};
    auto& window = handler.m_Windows[1];
    window.slots.resize(2);
    window.slots[0].guid = GUID::Generate();
    window.guidToSlot[window.slots[0].guid] = 0;
    window.slots[0].showsBakedImage = true;
    window.slots[1].occupied = false;
    const GUID served = window.slots[0].guid;
    handler.ReleaseGridSlotServedByPng(served);
    handler.ReleaseGridSlotsServedByPng();
    ASSERT_FALSE(window.slots[0].occupied);

    handler.RequestModelTile(window, served, false);
    const auto& restored = window.slots[window.guidToSlot.at(served)];
    EXPECT_TRUE(restored.restoresBakedImage);
    EXPECT_TRUE(window.pending.empty());
    EXPECT_TRUE(handler.importedModels.empty());

    const GUID unseen = GUID::Generate();
    window.guidToSlot.erase(served);
    window.slots[0].occupied = false;
    handler.RequestModelTile(window, unseen, false);
    EXPECT_EQ(window.pending, std::vector<GUID>{unseen});
    EXPECT_EQ(handler.importedModels, std::vector<GUID>{unseen});
}

// The copy leaves the slot TRANSFER_SRC. The grid's sample and a re-render (an empty capture is
// rendered again) import the slot as shader-readable in later frames, so the readback must put it
// back in that state; a slot left TRANSFER_SRC fails validation at the next draw that samples it.
TEST(ThumbnailPersistence, TheDiskCacheReadbackPutsTheSlotBackInItsSampledState)
{
    using namespace ThumbnailPersistenceHost;
    const auto root = std::filesystem::temp_directory_path() / ("thumb-persist-" + GUID::Generate().ToString());
    struct Cleanup
    {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } cleanup{root};
    std::filesystem::create_directories(root);
    AssetManager assets;
    assets.registry.source = root / "source.gltf";
    std::ofstream(assets.registry.source) << "{}";
    RenderServices services;
    ModelThumbnailHandler handler{root / "cache", &services, &assets, {}, {}};
    ModelThumbnailHandler::WindowState window;
    ModelThumbnailHandler::Slot slot;
    slot.guid = GUID::Generate();
    slot.deviceTex = 5;
    window.slots = {slot};
    Rendering::RenderGraph::RGFrame frame;
    handler.MaybeStartDiskCacheReadbackRG(window, frame);
    EXPECT_EQ(frame.importedTexture, 5);
    EXPECT_TRUE(frame.slotRestoredAfterCopy) << "the readback leaves the slot in the copy's TRANSFER_SRC layout";
}

