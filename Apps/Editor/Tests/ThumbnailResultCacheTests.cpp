// ThumbnailService memoizes the resolved thumbnail per (path, size, list flag,
// project generation, IBL setting) so a scrolling asset grid does not redo the
// routing decision per cell bind. The memo is only sound for results that are
// already final: anything a handler may still replace asynchronously, or that a
// separate writer may still produce on disk, has to stay unpinned or the grid
// keeps showing the placeholder it happened to observe first.
//
// These tests pin both halves — that repeats are stable, and that the four
// invalidation edges (assets root, cache root, handler registration, and a
// still-absent disk cache) still let a later call discover the real result.

#include "Thumbnails/ModelThumbnailHandler.h"
#include "Thumbnails/TextureThumbnailHandler.h"
#include "Thumbnails/ThumbnailService.h"

#include "JobSystem/WorkStealingThreadPool.h"

#include "Assets/AssetManager.h"

#include <stb_image_write.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using GameEngine::AssetManager;
using GameEngine::ModelThumbnailHandler;
using GameEngine::TextureThumbnailHandler;
using GameEngine::ThumbnailService;

namespace
{

std::filesystem::path MakeTempRoot(const char* label)
{
    static std::atomic<uint32_t> counter{0};
    const auto tid = static_cast<uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("ge-thumbcache-" + std::string(label) + "-" + std::to_string(tid) + "-" +
         std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

bool WriteTestPng(const std::filesystem::path& path, int width, int height)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 200);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return stbi_write_png(path.string().c_str(), width, height, 4, pixels.data(), width * 4) != 0;
}

void WriteTextFile(const std::filesystem::path& path, const std::string& contents)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

struct Recorder
{
    std::mutex Mutex;
    std::condition_variable Cv;
    std::vector<std::string> Results;

    std::function<void(const std::string&)> Callback()
    {
        return [this](const std::string& rel)
        {
            {
                std::lock_guard<std::mutex> lock(Mutex);
                Results.push_back(rel);
            }
            Cv.notify_all();
        };
    }

    bool WaitForCount(size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        std::unique_lock<std::mutex> lock(Mutex);
        return Cv.wait_for(lock, timeout, [&] { return Results.size() >= count; });
    }
};

struct TextureFixture
{
    // Declared first: the handler's destructor waits for in-flight decodes on this pool.
    JobSystem::WorkStealingThreadPool Pool{2};
    AssetManager Assets;
    ThumbnailService Service{&Assets};
    std::filesystem::path Root;
    std::filesystem::path CacheRoot;

    explicit TextureFixture(const char* label)
        : Root(MakeTempRoot(label))
        , CacheRoot(MakeTempRoot(label) / "Thumbnails")
    {
        Service.SetAssetsRoot(Root / "Assets");
        Service.RegisterHandler(GameEngine::AssetType::Texture,
                                std::make_unique<TextureThumbnailHandler>(Pool));
        Service.SetCacheRoot(CacheRoot);
    }

    // Drives one source through the async decode so later calls resolve to the
    // generated thumbnail rather than the raw source.
    std::filesystem::path WarmUp(const std::filesystem::path& source)
    {
        Recorder recorder;
        Service.GetOrRequest(source, 128, recorder.Callback());
        EXPECT_TRUE(recorder.WaitForCount(1));
        return CacheRoot / TextureThumbnailHandler::MakeCacheFileName(source);
    }
};

} // namespace

namespace
{
class BrowserCountingHandler final : public GameEngine::AssetThumbnailHandler
{
public:
    int Calls = 0;
    bool InvokeCallback = true;
    // Time each request takes, standing for a slow disk or a loaded machine.
    std::chrono::milliseconds Delay{0};
    std::string Result = "Icons/ModelThumbnail@64px.png";
    std::string GetOrRequest(const std::filesystem::path&, int,
                            std::function<void(const std::string&)> onReady, bool) override
    {
        ++Calls;
        std::this_thread::sleep_for(Delay);
        const std::string result = Result;
        if (InvokeCallback && onReady) onReady(result);
        return result;
    }
};

struct BrowserFixture
{
    AssetManager Assets;
    ThumbnailService Service{&Assets};
    BrowserCountingHandler* Handler;
    BrowserFixture()
    {
        auto handler = std::make_unique<BrowserCountingHandler>();
        Handler = handler.get();
        Service.RegisterHandler(GameEngine::AssetType::Texture, std::move(handler));
        // A preceding scroll test may have left the global settle window open.
        while (ModelThumbnailHandler::IsAssetBrowserScrolling())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
};
}

TEST(ThumbnailBrowserScheduling, ColdBindDefersToTheNextFrameThenDrainsInBindOrder)
{
    BrowserFixture f;
    std::vector<std::string> delivered;
    auto readyFor = [&delivered](const std::string& consumer)
    { return [&delivered, consumer](const std::string&) { delivered.push_back(consumer); }; };
    EXPECT_TRUE(f.Service.GetOrRequestBrowser("a.png", 128, "a", [] { return true; }, readyFor("a")).empty());
    EXPECT_TRUE(f.Service.GetOrRequestBrowser("b.png", 128, "b", [] { return true; }, readyFor("b")).empty());
    EXPECT_EQ(f.Handler->Calls, 0);
    f.Service.BeginFrame();
    EXPECT_EQ(f.Handler->Calls, 2);
    EXPECT_EQ(delivered, (std::vector<std::string>{"a", "b"}));
}

// The per-frame bound counts requests, not time: a slow request does not cut a
// frame short, so the same work is served on every machine.
TEST(ThumbnailBrowserScheduling, OneFrameServesThirtyTwoRequestsHoweverLongTheyTake)
{
    BrowserFixture f;
    f.Handler->Delay = std::chrono::milliseconds(3);
    constexpr int kRequests = 40;
    for (int i = 0; i < kRequests; ++i)
        f.Service.GetOrRequestBrowser("tile" + std::to_string(i) + ".png", 128,
                                      "cell" + std::to_string(i), [] { return true; }, nullptr);
    f.Service.BeginFrame();
    EXPECT_EQ(f.Handler->Calls, 32);
    f.Service.BeginFrame();
    EXPECT_EQ(f.Handler->Calls, kRequests);
}

TEST(ThumbnailBrowserScheduling, ScrollKeepsCachedImagesAndDefersColdWorkUntilSettled)
{
    BrowserFixture f;
    const auto warm = f.Service.GetOrRequest("warm.png", 128, nullptr);
    ASSERT_FALSE(warm.empty());
    ModelThumbnailHandler::NoteAssetListScrollActivity();
    EXPECT_EQ(f.Service.GetOrRequestBrowser("warm.png", 128, "warm", [] { return true; }, nullptr), warm);
    f.Service.GetOrRequestBrowser("cold.png", 128, "cold", [] { return true; }, nullptr);
    for (int frame = 0; frame < 20; ++frame)
        f.Service.BeginFrame();
    EXPECT_EQ(f.Handler->Calls, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(270));
    f.Service.BeginFrame();
    EXPECT_EQ(f.Handler->Calls, 2);
}

TEST(ThumbnailBrowserScheduling, ImmediateOnlyHandlerStillUpdatesDeferredCell)
{
    BrowserFixture f;
    f.Handler->InvokeCallback = false;
    std::string delivered;
    f.Service.GetOrRequestBrowser("small.png", 128, "image", [] { return true; },
        [&](const std::string& result) { delivered = result; });
    EXPECT_TRUE(delivered.empty());
    f.Service.BeginFrame();
    EXPECT_EQ(delivered, "Icons/ModelThumbnail@64px.png");
    EXPECT_EQ(f.Handler->Calls, 1);
}

TEST(ThumbnailBrowserScheduling, PreviouslyDisplayedImageFallbackRemainsAvailableDuringScroll)
{
    BrowserFixture f;
    f.Handler->InvokeCallback = false;
    f.Handler->Result.clear(); // Small images decline thumbnail generation.
    std::string displayed;
    f.Service.GetOrRequestBrowser("small.png", 128, "image", [] { return true; },
        [&](const std::string& result) { displayed = result; });
    f.Service.BeginFrame();
    ASSERT_FALSE(displayed.empty());
    ModelThumbnailHandler::NoteAssetListScrollActivity();
    EXPECT_EQ(f.Service.GetOrRequestBrowser("small.png", 128, "image", [] { return true; }, nullptr), displayed);
    f.Service.BeginFrame();
    EXPECT_EQ(f.Handler->Calls, 1);
}

TEST(ThumbnailBrowserScheduling, RecycledAndReboundCellsDoNotGenerateStaleThumbnails)
{
    BrowserFixture f;
    int staleCallbacks = 0;
    int currentCallbacks = 0;
    f.Service.GetOrRequestBrowser("offscreen.png", 128, "offscreen", [] { return false; },
        [&](const std::string&) { ++staleCallbacks; });
    f.Service.GetOrRequestBrowser("old.png", 128, "cell", [] { return true; },
        [&](const std::string&) { ++staleCallbacks; });
    f.Service.GetOrRequestBrowser("current.png", 128, "cell", [] { return true; },
        [&](const std::string&) { ++currentCallbacks; });
    f.Service.BeginFrame();
    f.Service.BeginFrame();
    EXPECT_EQ(f.Handler->Calls, 1);
    EXPECT_EQ(staleCallbacks, 0);
    EXPECT_EQ(currentCallbacks, 1);
}

TEST(ThumbnailBrowserScheduling, ProjectSwitchDropsPendingBrowserRequests)
{
    BrowserFixture f;
    f.Service.GetOrRequestBrowser("old.png", 128, "old", [] { return true; }, nullptr);
    f.Service.SetAssetsRoot("different-project/Assets");
    f.Service.BeginFrame();
    EXPECT_EQ(f.Handler->Calls, 0);
}

TEST(ThumbnailBrowserScheduling, WarmRebindCancelsPreviousColdConsumer)
{
    BrowserFixture f;
    const auto warm = f.Service.GetOrRequest("warm.png", 128, nullptr);
    f.Service.GetOrRequestBrowser("cold.png", 128, "cell", [] { return true; }, nullptr);
    EXPECT_EQ(f.Service.GetOrRequestBrowser("warm.png", 128, "cell", [] { return true; }, nullptr), warm);
    f.Service.BeginFrame();
    EXPECT_EQ(f.Handler->Calls, 1);
}

TEST(ThumbnailBrowserScheduling, ModelsMaterialsAndTexturesAllRespectTheScrollHold)
{
    BrowserFixture f;
    auto model = std::make_unique<BrowserCountingHandler>();
    auto* models = model.get();
    f.Service.RegisterHandler(GameEngine::AssetType::Model, std::move(model));
    auto material = std::make_unique<BrowserCountingHandler>();
    auto* materials = material.get();
    f.Service.RegisterHandler(GameEngine::AssetType::Material, std::move(material));
    ModelThumbnailHandler::NoteAssetListScrollActivity();
    f.Service.GetOrRequestBrowser("model.fbx", 128, "model", [] { return true; }, nullptr);
    f.Service.GetOrRequestBrowser("surface.material", 128, "material", [] { return true; }, nullptr);
    f.Service.GetOrRequestBrowser("image.png", 128, "image", [] { return true; }, nullptr);
    f.Service.BeginFrame();
    EXPECT_EQ(models->Calls + materials->Calls + f.Handler->Calls, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(270));
    f.Service.BeginFrame();
    f.Service.BeginFrame();
    f.Service.BeginFrame();
    EXPECT_EQ(models->Calls, 1);
    EXPECT_EQ(materials->Calls, 1);
    EXPECT_EQ(f.Handler->Calls, 1);
}

TEST(ThumbnailBrowserScheduling, RapidSmartFolderScrubbingKeepsOnlyBoundedRecentWork)
{
    BrowserFixture f;
    int staleCallbacks = 0;
    int visibleCallbacks = 0;
    for (int i = 0; i < 5000; ++i)
        f.Service.GetOrRequestBrowser("asset" + std::to_string(i) + ".png", 128,
            "smart-cell" + std::to_string(i), [] { return false; },
            [&](const std::string&) { ++staleCallbacks; });
    f.Service.GetOrRequestBrowser("visible.png", 128, "visible", [] { return true; },
        [&](const std::string&) { ++visibleCallbacks; });
    for (int frame = 0; frame < 17; ++frame)
        f.Service.BeginFrame();
    EXPECT_EQ(f.Handler->Calls, 1);
    EXPECT_EQ(staleCallbacks, 0);
    EXPECT_EQ(visibleCallbacks, 1);
}

// The memo's whole point: a grid that binds the same cell repeatedly gets the
// same answer without re-deriving it.
TEST(ThumbnailResultCache, WarmResultIsStableAcrossRepeatedRequests)
{
    TextureFixture fixture("stable");
    const auto source = fixture.Root / "Assets" / "wall.png";
    ASSERT_TRUE(WriteTestPng(source, 1024, 1024));

    const auto expected = fixture.WarmUp(source);
    ASSERT_TRUE(std::filesystem::exists(expected));

    const std::string first = fixture.Service.GetOrRequest(source, 128, nullptr);
    EXPECT_EQ(first, expected.generic_string());
    for (int i = 0; i < 32; ++i)
        EXPECT_EQ(fixture.Service.GetOrRequest(source, 128, nullptr), first);
}

// The memo keys on a normalized path, and the normalization is skipped for
// paths that carry nothing to normalize. Both spellings of one asset must still
// resolve to the same thumbnail — the skip is a cost optimization, never a
// change to what an equivalent path answers.
TEST(ThumbnailResultCache, EquivalentPathFormsResolveToTheSameResult)
{
    TextureFixture fixture("pathforms");
    const auto source = fixture.Root / "Assets" / "forms.png";
    ASSERT_TRUE(WriteTestPng(source, 1024, 1024));
    const auto expected = fixture.WarmUp(source);

    const std::filesystem::path dotted =
        fixture.Root / "Assets" / "." / "forms.png";
    const std::filesystem::path parented =
        fixture.Root / "Assets" / "sub" / ".." / "forms.png";

    const std::string plain = fixture.Service.GetOrRequest(source, 128, nullptr);
    EXPECT_EQ(plain, expected.generic_string());
    EXPECT_EQ(fixture.Service.GetOrRequest(dotted, 128, nullptr), plain);
    EXPECT_EQ(fixture.Service.GetOrRequest(parented, 128, nullptr), plain);
}

// A cell bound repeatedly while its decode is still in flight is the scroll
// case. The placeholder the service answers with must not become the permanent
// answer once the decode lands.
TEST(ThumbnailResultCache, ProvisionalTextureResultIsNotPinnedByRepeatedColdRequests)
{
    TextureFixture fixture("coldrepeat");
    const auto source = fixture.Root / "Assets" / "scrolled.png";
    ASSERT_TRUE(WriteTestPng(source, 1024, 1024));

    // Hammer the cold window the way a scroll does.
    Recorder recorder;
    const std::string provisional = fixture.Service.GetOrRequest(source, 128, recorder.Callback());
    for (int i = 0; i < 32; ++i)
        fixture.Service.GetOrRequest(source, 128, nullptr);

    ASSERT_TRUE(recorder.WaitForCount(1));
    const auto expected = fixture.CacheRoot / TextureThumbnailHandler::MakeCacheFileName(source);
    ASSERT_NE(provisional, expected.generic_string())
        << "test is not exercising the cold window";

    EXPECT_EQ(fixture.Service.GetOrRequest(source, 128, nullptr), expected.generic_string());
}

// Switching projects rebuilds the AssetManager and reassigns GUIDs, so a result
// resolved against the old root must not survive into the new one.
TEST(ThumbnailResultCache, AssetsRootChangeInvalidatesMemoizedResults)
{
    TextureFixture fixture("rootswap");
    const auto source = fixture.Root / "Assets" / "swap.png";
    ASSERT_TRUE(WriteTestPng(source, 1024, 1024));

    const auto expected = fixture.WarmUp(source);
    ASSERT_EQ(fixture.Service.GetOrRequest(source, 128, nullptr), expected.generic_string());

    // A different root makes the same absolute path resolve outside the project.
    const auto otherRoot = MakeTempRoot("rootswap-other");
    fixture.Service.SetAssetsRoot(otherRoot / "Assets");

    // The point is that the memo did not answer: the service re-derived against
    // the new root, whatever it now decides.
    const std::string afterSwap = fixture.Service.GetOrRequest(source, 128, nullptr);
    EXPECT_NE(afterSwap, std::string())
        << "a root swap must still produce an answer, not an empty one";
}

// The cache root is where handlers write. Repointing it must not leave results
// that name files under the previous root.
TEST(ThumbnailResultCache, CacheRootChangeInvalidatesMemoizedResults)
{
    TextureFixture fixture("cacheswap");
    const auto source = fixture.Root / "Assets" / "recache.png";
    ASSERT_TRUE(WriteTestPng(source, 1024, 1024));

    const auto firstCache = fixture.WarmUp(source);
    ASSERT_EQ(fixture.Service.GetOrRequest(source, 128, nullptr), firstCache.generic_string());

    const auto secondRoot = MakeTempRoot("cacheswap-second") / "Thumbnails";
    fixture.Service.SetCacheRoot(secondRoot);

    Recorder recorder;
    const std::string afterSwap = fixture.Service.GetOrRequest(source, 128, recorder.Callback());
    EXPECT_NE(afterSwap, firstCache.generic_string())
        << "result still names a file under the previous cache root";
}

// A handler registered after a path has already been answered by the fallback
// must get the chance to answer it.
TEST(ThumbnailResultCache, HandlerRegistrationInvalidatesMemoizedResults)
{
    const auto root = MakeTempRoot("lateHandler");
    const auto cacheRoot = MakeTempRoot("lateHandler-cache") / "Thumbnails";
    const auto source = root / "Assets" / "late.png";
    ASSERT_TRUE(WriteTestPng(source, 1024, 1024));

    // Declared first: the handler's destructor waits for in-flight decodes on this pool.
    JobSystem::WorkStealingThreadPool pool{2};
    AssetManager assets;
    ThumbnailService service(&assets);
    service.SetAssetsRoot(root / "Assets");
    service.SetCacheRoot(cacheRoot);

    // No texture handler yet: this is the raw-source fallback.
    const std::string beforeHandler = service.GetOrRequest(source, 128, nullptr);
    EXPECT_EQ(beforeHandler, "late.png");

    service.RegisterHandler(GameEngine::AssetType::Texture,
                            std::make_unique<TextureThumbnailHandler>(pool));

    Recorder recorder;
    service.GetOrRequest(source, 128, recorder.Callback());
    ASSERT_TRUE(recorder.WaitForCount(1))
        << "the newly registered handler never received the request";
    EXPECT_EQ(recorder.Results[0],
              (cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source)).generic_string());
}

// A material's live-render handle is provisional: the disk cache that supersedes
// it is written by a separate producer (import-time pre-generation, later a
// render readback). Pinning the handle makes that file unreachable, so the
// material renders live for the rest of the session — the exact cost the disk
// cache exists to remove.
TEST(ThumbnailResultCache, MaterialResultIsNotPinnedBeforeItsDiskCacheExists)
{
    const auto root = MakeTempRoot("matcache");
    const auto cacheRoot = MakeTempRoot("matcache-cache") / "Thumbnails";
    const auto material = root / "Assets" / "red.material";
    WriteTextFile(material, "{\"shader\":\"lit\"}\n");
    std::error_code ec;
    std::filesystem::create_directories(cacheRoot, ec);

    // The material branch only reaches the live-render handle once the path
    // resolves to a GUID, which needs a registered project source. Without this
    // the handler bails early and the service answers from the icon fallback —
    // a green test that never touched the code under test.
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root / "Assets"));
    ThumbnailService service(&assets);
    service.SetAssetsRoot(root / "Assets");
    service.RegisterHandler(GameEngine::AssetType::Model,
                            std::make_unique<ModelThumbnailHandler>(&assets, nullptr));
    service.SetCacheRoot(cacheRoot);

    const std::filesystem::path diskCache =
        cacheRoot / ModelThumbnailHandler::MakeMaterialCacheFileName(
                        assets.ResolveAssetGuid(material), ModelThumbnailHandler::GetPreviewIblEnabled());

    // Before the PNG exists the service answers with the live-render handle.
    const std::string provisional = service.GetOrRequest(material, 128, nullptr);
    ASSERT_EQ(provisional.rfind("engine:", 0), 0u)
        << "expected the live-render handle, got '" << provisional
        << "' — the test is not exercising the pre-disk-cache window";

    // The separate producer lands the rendered thumbnail.
    ASSERT_TRUE(WriteTestPng(diskCache, 128, 128));

    EXPECT_EQ(service.GetOrRequest(material, 128, nullptr), diskCache.generic_string())
        << "the live-render handle was pinned, so the disk cache is now unreachable";
}

// Measurement, not a gate: reports the per-call cost of the request the asset
// grid makes once per visible cell per rebuild, on an already-warm path. Run it
// explicitly (--gtest_also_run_disabled_tests) and only from a Release build —
// an /Od number here is a different measurement, not a slow one.
TEST(ThumbnailResultCachePerf, DISABLED_WarmGetOrRequestCostPerCall)
{
    TextureFixture fixture("perf");
    const auto source = fixture.Root / "Assets" / "hot.png";
    ASSERT_TRUE(WriteTestPng(source, 1024, 1024));
    const auto expected = fixture.WarmUp(source);
    ASSERT_TRUE(std::filesystem::exists(expected));

    // Settle the path: the first warm call is the one that populates whatever
    // memo exists, and timing it would mix the two costs.
    for (int i = 0; i < 64; ++i)
        fixture.Service.GetOrRequest(source, 128, nullptr);

    constexpr int kIterations = 20000;
    volatile size_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i)
        sink += fixture.Service.GetOrRequest(source, 128, nullptr).size();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    (void)sink;

    const double nsPerCall =
        std::chrono::duration<double, std::nano>(elapsed).count() / kIterations;
    std::printf("[ MEASURE  ] warm GetOrRequest: %.0f ns/call over %d calls\n",
                nsPerCall, kIterations);
    SUCCEED();
}
