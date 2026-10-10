// ThumbnailService is the seam that decides which handler owns an asset, and
// it is called once per visible cell on every panel rebuild. Two properties are
// pinned here: a texture asset reaches the texture handler at all (before this
// suite existed, textures fell through to the raw-source fallback), and the
// request path does not emit a log line per call.

#include "Thumbnails/FolderBakeReport.h"
#include "Thumbnails/TextureThumbnailHandler.h"
#include "Thumbnails/ThumbnailService.h"
#include "Thumbnails/ModelThumbnailHandler.h"
#include "Thumbnails/ModelThumbnailCachePolicy.h"

#include "JobSystem/WorkStealingThreadPool.h"

#include "Assets/AssetManager.h"
#include "FileSystem/FileSystem.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"

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
using GameEngine::TextureThumbnailHandler;
using GameEngine::ThumbnailService;

TEST(ThumbnailServiceRouting, SpawnSettlePolicy)
{
    using namespace GameEngine;
    const GUID orbit("11111111-1111-1111-1111-111111111111");
    const GUID readyGrid("22222222-2222-2222-2222-222222222222");
    EXPECT_TRUE(ThumbnailCachePolicy::IsSpawnSettling(orbit, orbit, 0));
    EXPECT_FALSE(ThumbnailCachePolicy::IsSpawnSettling(readyGrid, orbit, 0));
    EXPECT_FALSE(ThumbnailCachePolicy::IsSpawnSettling(orbit, orbit, ThumbnailCachePolicy::kSpawnSettleFrames));
    EXPECT_FALSE(ThumbnailCachePolicy::IsSpawnSettling(readyGrid, GUID{}, 0));
}

namespace
{

std::filesystem::path MakeTempRoot(const char* label)
{
    static std::atomic<uint32_t> counter{0};
    const auto tid = static_cast<uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("ge-thumbroute-" + std::string(label) + "-" + std::to_string(tid) + "-" +
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

// Counts log records whose text contains a needle, at any level the logger is
// currently emitting. Installed for the duration of one test.
class LogCounter
{
public:
    explicit LogCounter(std::string needle)
        : m_Needle(std::move(needle))
    {
        // Initialize first, and only then add the sink: AddSink recomputes the
        // effective level from Config::GlobalMinLevel, so a SetLogLevel done
        // beforehand is discarded and the records never reach any sink. It also
        // starts the drain thread that Flush() waits on.
        Logger::Log::Config config;
        config.GlobalMinLevel = Logger::LogLevel::Trace;
        Logger::Log::Initialize(config);

        auto sink = std::make_unique<Logger::CallbackSink>();
        m_Sink = sink.get();
        m_Id = m_Sink->RegisterCallback([this](const Logger::LogMessage& message)
        {
            if (message.Message.find(m_Needle) != std::string::npos)
                m_Count.fetch_add(1);
        });
        Logger::Log::AddSink(std::move(sink));
    }

    ~LogCounter()
    {
        if (m_Sink)
            m_Sink->UnregisterCallback(m_Id);
    }

    int Count() const
    {
        Logger::Log::Flush();
        return m_Count.load();
    }

private:
    std::string m_Needle;
    Logger::CallbackSink* m_Sink = nullptr;
    uint64_t m_Id = 0;
    std::atomic<int> m_Count{0};
};

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

// The service needs a non-null AssetManager to route at all. An unopened one
// answers "no metadata" for every path, which is exactly the case the service
// handles by falling back to extension-based typing.
struct ServiceFixture
{
    // Declared first: the handler's destructor waits for in-flight decodes on this pool.
    JobSystem::WorkStealingThreadPool Pool{2};
    AssetManager Assets;
    ThumbnailService Service{&Assets};
    std::filesystem::path Root;
    std::filesystem::path CacheRoot;

    explicit ServiceFixture(const char* label)
        : Root(MakeTempRoot(label))
        , CacheRoot(MakeTempRoot(label) / "Thumbnails")
    {
        Service.SetAssetsRoot(Root / "Assets");
        Service.RegisterHandler(GameEngine::AssetType::Texture,
                                std::make_unique<TextureThumbnailHandler>(Pool));
        Service.SetCacheRoot(CacheRoot);
    }
};

} // namespace

// The disk cache is keyed by GUID, which the service reads from registry
// metadata. Registers the source under the fixture's project root and returns
// its GUID, as the asset browser's registered listing does in production.
GameEngine::GUID RegisterSource(ServiceFixture& fixture, const std::filesystem::path& source)
{
    std::filesystem::create_directories(source.parent_path());
    if (!std::filesystem::exists(source))
        std::ofstream(source) << "fixture";
    EXPECT_TRUE(fixture.Assets.Initialize(fixture.Root / "Assets"));
    const GameEngine::GUID guid = fixture.Assets.ResolveAssetGuid(source);
    EXPECT_FALSE(guid.IsNull());
    return guid;
}

TEST(ThumbnailServiceRouting, ModelDiskCacheIsTileOnlyAndRejectsStaleSource)
{
    ServiceFixture fixture("modeldisk");
    const auto source = fixture.Root / "Assets" / "model.fbx";
    const GameEngine::GUID guid = RegisterSource(fixture, source);
    const auto cached = fixture.CacheRoot /
        GameEngine::ModelThumbnailHandler::MakeModelCacheFileName(guid);
    ASSERT_TRUE(WriteTestPng(cached, 1024, 1024));
    std::filesystem::last_write_time(cached, std::filesystem::last_write_time(source));
    EXPECT_EQ(fixture.Service.GetOrRequest(source, 128, nullptr), cached.generic_string());
    EXPECT_NE(fixture.Service.GetOrRequest(source, 512, nullptr), cached.generic_string());
    std::filesystem::last_write_time(source,
        std::filesystem::last_write_time(cached) + std::chrono::seconds(5));
    EXPECT_NE(fixture.Service.GetOrRequest(source, 128, nullptr), cached.generic_string());
}

// A model PNG of another bake version is never served, and the sweep deletes
// it without touching the current version's PNGs or any other cached file.
TEST(ThumbnailServiceRouting, ModelDiskCacheOfAnotherBakeVersionIsRemoved)
{
    ServiceFixture fixture("modeldiskversion");
    const auto source = fixture.Root / "Assets" / "model.fbx";
    const GameEngine::GUID guid = RegisterSource(fixture, source);
    const auto current = fixture.CacheRoot /
        GameEngine::ModelThumbnailHandler::MakeModelCacheFileName(guid);
    const auto older = fixture.CacheRoot / ("modelthumb_" + guid.ToString() + ".png");
    const auto material = fixture.CacheRoot /
        GameEngine::ModelThumbnailHandler::MakeMaterialCacheFileName(guid, true);
    ASSERT_NE(current, older);
    ASSERT_TRUE(WriteTestPng(older, 64, 64));
    ASSERT_TRUE(WriteTestPng(material, 64, 64));
    std::filesystem::last_write_time(older, std::filesystem::last_write_time(source));
    EXPECT_NE(fixture.Service.GetOrRequest(source, 128, nullptr), older.generic_string());

    ASSERT_TRUE(WriteTestPng(current, 64, 64));
    EXPECT_EQ(GameEngine::ModelThumbnailHandler::RemoveStaleModelCacheFiles(fixture.CacheRoot), 1u);
    EXPECT_FALSE(std::filesystem::exists(older));
    EXPECT_TRUE(std::filesystem::exists(current));
    EXPECT_TRUE(std::filesystem::exists(material));
}

// The persistence side names the PNG from registry metadata while the browser
// requests it with the spelling it found on disk. On a case-insensitive
// filesystem the registry folds both spellings to one GUID, so the PNG must be
// reachable from either; a path-hashed name only matched one of them.
TEST(ThumbnailServiceRouting, ModelDiskCacheIsReachableFromCaseVariantSpelling)
{
    if (GameEngine::FileSystem::IsCaseSensitive())
        GTEST_SKIP() << "case-variant spellings are distinct assets on this filesystem";
    ServiceFixture fixture("modeldiskcase");
    const auto source = fixture.Root / "Assets" / "Props" / "Crate.fbx";
    const GameEngine::GUID guid = RegisterSource(fixture, source);
    const auto cached = fixture.CacheRoot /
        GameEngine::ModelThumbnailHandler::MakeModelCacheFileName(guid);
    ASSERT_TRUE(WriteTestPng(cached, 1024, 1024));
    std::filesystem::last_write_time(cached, std::filesystem::last_write_time(source));

    const auto variant = fixture.Root / "Assets" / "props" / "crate.FBX";
    EXPECT_EQ(fixture.Service.GetOrRequest(variant, 128, nullptr), cached.generic_string());
}

TEST(ThumbnailServiceRouting, MaterialDiskCacheDoesNotReplaceLargePreview)
{
    ServiceFixture fixture("materialdisk");
    const auto source = fixture.Root / "Assets" / "surface.material";
    const GameEngine::GUID guid = RegisterSource(fixture, source);
    const auto cached = fixture.CacheRoot /
        GameEngine::ModelThumbnailHandler::MakeMaterialCacheFileName(
            guid, GameEngine::ModelThumbnailHandler::GetPreviewIblEnabled());
    ASSERT_TRUE(WriteTestPng(cached, 1024, 1024));
    EXPECT_EQ(fixture.Service.GetOrRequest(source, 128, nullptr), cached.generic_string());
    EXPECT_NE(fixture.Service.GetOrRequest(source, 512, nullptr), cached.generic_string());
}

TEST(ThumbnailServiceRouting, FullTexturePreviewRetainsRawSourceFallback)
{
    ServiceFixture fixture("fullpreview");
    const auto source = fixture.Root / "Assets" / "large.png";
    ASSERT_TRUE(WriteTestPng(source, 1024, 1024));
    EXPECT_EQ(fixture.Service.GetOrRequest(source, 512, nullptr), "large.png");
}

// The registered texture handler must actually receive texture assets, and its
// cache file must be what later requests resolve to.
TEST(ThumbnailServiceRouting, TextureAssetsReachTheTextureHandler)
{
    ServiceFixture fixture("route");
    const auto source = fixture.Root / "Assets" / "wall.png";
    ASSERT_TRUE(WriteTestPng(source, 1024, 1024));

    Recorder recorder;
    const std::string immediate = fixture.Service.GetOrRequest(source, 128, recorder.Callback());

    // Cold: keep the type icon until the small PNG arrives; never upload the
    // full-resolution source merely to fill a browser cell.
    EXPECT_TRUE(immediate.empty());

    ASSERT_TRUE(recorder.WaitForCount(1));
    const auto expected =
        fixture.CacheRoot / TextureThumbnailHandler::MakeCacheFileName(source);
    EXPECT_EQ(recorder.Results[0], expected.generic_string());
    EXPECT_TRUE(std::filesystem::exists(expected));

    // Warm: the request path now resolves to the thumbnail, not the source.
    const std::string warm = fixture.Service.GetOrRequest(source, 128, nullptr);
    EXPECT_EQ(warm, expected.generic_string());
}

// SetCacheRoot must reach a handler registered before it, and one registered
// after it — the propagation is what makes the cache work at all.
TEST(ThumbnailServiceRouting, CacheRootReachesHandlersRegisteredEitherSide)
{
    const auto root = MakeTempRoot("cacheroot");
    const auto cacheRoot = root / "Thumbnails";
    // Large enough to be worth a thumbnail: a source already at or below
    // kThumbLongEdge is declined, which would make this test pass on the
    // fallback path without ever exercising the cache root.
    const auto source = root / "Assets" / "late.png";
    ASSERT_TRUE(WriteTestPng(source, 1024, 1024));

    // Declared first: the handler's destructor waits for in-flight decodes on this pool.
    JobSystem::WorkStealingThreadPool pool{2};
    AssetManager assets;
    ThumbnailService service(&assets);
    service.SetAssetsRoot(root / "Assets");

    // Cache root first, handler second.
    service.SetCacheRoot(cacheRoot);
    service.RegisterHandler(GameEngine::AssetType::Texture,
                            std::make_unique<TextureThumbnailHandler>(pool));

    Recorder recorder;
    service.GetOrRequest(source, 128, recorder.Callback());
    ASSERT_TRUE(recorder.WaitForCount(1));
    EXPECT_EQ(recorder.Results[0],
              (cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source)).generic_string());
}

// Panels call GetOrRequest per cell per rebuild. The routing diagnostic must
// cost one line per path, not one per call — the interactive editor runs the
// logger at Debug, so demoting the level alone would not have stopped it.
TEST(ThumbnailServiceRouting, RequestPathLogsOncePerPathNotPerCall)
{
    LogCounter counter("ThumbnailService::GetOrRequest");

    ServiceFixture fixture("logspam");
    const auto source = fixture.Root / "Assets" / "repeat.png";
    ASSERT_TRUE(WriteTestPng(source, 128, 128));

    constexpr int kCalls = 40;
    for (int i = 0; i < kCalls; ++i)
        fixture.Service.GetOrRequest(source, 128, nullptr);

    EXPECT_EQ(counter.Count(), 1);
}

// A second distinct path is still worth a line: the memo suppresses repeats,
// not the diagnostic itself.
TEST(ThumbnailServiceRouting, EachDistinctPathStillLogsOnce)
{
    LogCounter counter("ThumbnailService::GetOrRequest");

    ServiceFixture fixture("logdistinct");
    for (int i = 0; i < 5; ++i)
    {
        const auto source = fixture.Root / "Assets" / ("m" + std::to_string(i) + ".material");
        for (int call = 0; call < 4; ++call)
            fixture.Service.GetOrRequest(source, 128, nullptr);
    }

    EXPECT_EQ(counter.Count(), 5);
}

// The asset browser states a folder bake's progress while it runs and its
// outcome after: failures named as such (inline when there are two or fewer)
// rather than folded into "already current", and files the database lists but
// the disk lacks named with what to do about them.
TEST(FolderBakeReport, DescribesProgressThenOutcome)
{
    using GameEngine::DescribeFolderBakeReport;
    GameEngine::FolderBakeReport report;
    EXPECT_EQ(DescribeFolderBakeReport(report), "");
    report.Queued = 333;
    report.Generated = 10;
    report.AlreadyCurrent = 100;
    report.Failed = {"Props/a.fbx"};
    report.Running = true;
    EXPECT_EQ(DescribeFolderBakeReport(report), "Generating thumbnails: 111 of 333");

    report.Running = false;
    report.Generated = 232;
    report.Failed = {"Props/a.fbx", "Props/b.fbx"};
    report.MissingFromDisk = {"Props/c.fbx"};
    EXPECT_EQ(DescribeFolderBakeReport(report),
              "Thumbnails: 232 generated, 100 already current. Could not generate a.fbx and b.fbx; the log says why. "
              "c.fbx is not on disk: refresh the folder");

    report.Failed = {"1.fbx", "2.fbx", "3.fbx"};
    report.MissingFromDisk = {"4.fbx", "5.fbx", "6.fbx"};
    EXPECT_EQ(DescribeFolderBakeReport(report),
              "Thumbnails: 232 generated, 100 already current. 3 could not be generated; the log names them and "
              "says why. 3 listed files are not on disk: refresh the folder");
}

// Tiles served from their PNG queue their models for slot release, and the next
// frame frees the queue even when no window ticks thumbnails (a browser that
// shows only baked tiles never creates one); the queue must not grow per bind.
TEST(ThumbnailServiceRouting, PngServedTilesDoNotPileUpWithoutAThumbnailWindow)
{
    ServiceFixture fixture("modeldiskrelease");
    auto handler = std::make_unique<GameEngine::ModelThumbnailHandler>(&fixture.Assets, nullptr);
    const GameEngine::ModelThumbnailHandler* models = handler.get();
    fixture.Service.RegisterHandler(GameEngine::AssetType::Model, std::move(handler));
    const auto source = fixture.Root / "Assets" / "model.fbx";
    const GameEngine::GUID guid = RegisterSource(fixture, source);
    const auto cached = fixture.CacheRoot / GameEngine::ModelThumbnailHandler::MakeModelCacheFileName(guid);
    ASSERT_TRUE(WriteTestPng(cached, 64, 64));
    std::filesystem::last_write_time(cached, std::filesystem::last_write_time(source));

    for (int bind = 0; bind < 20; ++bind)
        ASSERT_EQ(fixture.Service.GetOrRequest(source, 128, nullptr), cached.generic_string());
    fixture.Service.BeginFrame();

    EXPECT_EQ(models->PendingGridSlotReleaseCount(), 0u);
}

// A file the asset database still lists after it left the disk is not queued
// for a folder bake (there is nothing to render, and counting it as a failure
// blamed the bake); the report names it instead.
TEST(ThumbnailServiceRouting, FolderBakeReportsFilesMissingFromDiskInsteadOfQueueingThem)
{
    ServiceFixture fixture("folderbakemissing");
    fixture.Service.RegisterHandler(GameEngine::AssetType::Model,
                                    std::make_unique<GameEngine::ModelThumbnailHandler>(&fixture.Assets, nullptr));
    const auto present = fixture.Root / "Assets" / "Props" / "present.fbx";
    const auto deleted = fixture.Root / "Assets" / "Props" / "deleted.fbx";
    RegisterSource(fixture, present);
    std::filesystem::create_directories(deleted.parent_path());
    std::ofstream(deleted) << "fixture";
    ASSERT_FALSE(fixture.Assets.ResolveAssetGuid(deleted).IsNull());
    std::filesystem::remove(deleted);

    GameEngine::FolderBakeReport last;
    fixture.Service.SetFolderBakeListener([&last](const GameEngine::FolderBakeReport& report) { last = report; });
    EXPECT_EQ(fixture.Service.GenerateFolderThumbnails(fixture.Root / "Assets" / "Props"), 1u);
    EXPECT_EQ(last.Queued, 1u);
    ASSERT_EQ(last.MissingFromDisk.size(), 1u);
    EXPECT_EQ(last.MissingFromDisk[0].filename(), std::filesystem::path("deleted.fbx"));
    fixture.Service.SetFolderBakeListener({});
}
