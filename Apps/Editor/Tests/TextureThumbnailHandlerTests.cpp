// A texture asset must not serve its own full-resolution source as its
// thumbnail. TextureThumbnailHandler decodes the source once, writes a
// downscaled PNG into the project's thumbnail cache, and hands every later
// request that file instead — with the decode kept off the requesting thread,
// because the requester is the editor's UI thread.

#include "Thumbnails/TextureThumbnailHandler.h"

#include "JobSystem/WorkStealingThreadPool.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

using GameEngine::TextureThumbnailHandler;

namespace
{

// The handler decodes on JobSystem background jobs. Each test owns a small pool
// that outlives its handler — the handler's destructor waits for in-flight
// decodes, so member order here is the lifetime contract.
struct HandlerHarness
{
    JobSystem::WorkStealingThreadPool Pool{2};
    TextureThumbnailHandler Handler{Pool};
};

// One temp tree per test process AND per test, so parallel ctest jobs and
// repeated runs never share a cache root.
std::filesystem::path MakeTempRoot(const char* label)
{
    static std::atomic<uint32_t> counter{0};
    const auto pid = static_cast<uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("ge-texthumb-" + std::string(label) + "-" + std::to_string(pid) + "-" +
         std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

// A source image with per-pixel variation, so a downscale that silently
// produced a constant image would still be visible in the decoded result.
bool WriteTestPng(const std::filesystem::path& path, int width, int height)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            pixels[i + 0] = static_cast<uint8_t>(x % 256);
            pixels[i + 1] = static_cast<uint8_t>(y % 256);
            pixels[i + 2] = static_cast<uint8_t>((x + y) % 256);
            pixels[i + 3] = 255;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return stbi_write_png(path.string().c_str(), width, height, 4, pixels.data(), width * 4) != 0;
}

void WriteBytes(const std::filesystem::path& path, const std::string& content)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << content;
}

// Collects the handler's asynchronous answers. The handler invokes callbacks
// from a worker thread, so every field is guarded.
struct ReadyRecorder
{
    std::mutex Mutex;
    std::condition_variable Cv;
    std::vector<std::string> Results;
    std::vector<std::thread::id> Threads;

    std::function<void(const std::string&)> Callback()
    {
        return [this](const std::string& rel)
        {
            {
                std::lock_guard<std::mutex> lock(Mutex);
                Results.push_back(rel);
                Threads.push_back(std::this_thread::get_id());
            }
            Cv.notify_all();
        };
    }

    // Returns false on timeout rather than hanging a CI run.
    bool WaitForCount(size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        std::unique_lock<std::mutex> lock(Mutex);
        return Cv.wait_for(lock, timeout, [&] { return Results.size() >= count; });
    }

    // For "the handler declined" assertions. Reading Results straight after the
    // call proves nothing — it races a worker that may not have started yet —
    // so wait out a window many times longer than any decode in this suite and
    // require that nothing arrives.
    bool StaysSilent(std::chrono::milliseconds window = std::chrono::milliseconds(750))
    {
        std::unique_lock<std::mutex> lock(Mutex);
        Cv.wait_for(lock, window, [&] { return !Results.empty(); });
        return Results.empty();
    }

    size_t Count()
    {
        std::lock_guard<std::mutex> lock(Mutex);
        return Results.size();
    }
};

struct ImageSize
{
    int Width = 0;
    int Height = 0;
    bool Valid = false;
};

ImageSize ReadPngSize(const std::filesystem::path& path)
{
    ImageSize size;
    int channels = 0;
    size.Valid = stbi_info(path.string().c_str(), &size.Width, &size.Height, &channels) != 0;
    return size;
}

// Rewrites a PNG's IHDR dimensions in place. A real image of these dimensions
// would cost hundreds of megabytes to author here; the pixel budget is decided
// from the header alone, so the header is the honest thing to test against.
// (IHDR data starts 16 bytes in: 8 signature + 4 length + 4 type.)
bool ForgePngDimensions(const std::filesystem::path& path, uint32_t width, uint32_t height)
{
    std::vector<uint8_t> bytes;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (bytes.size() < 24)
        return false;
    const auto put = [&bytes](size_t at, uint32_t v)
    {
        bytes[at + 0] = static_cast<uint8_t>((v >> 24) & 0xFF);
        bytes[at + 1] = static_cast<uint8_t>((v >> 16) & 0xFF);
        bytes[at + 2] = static_cast<uint8_t>((v >> 8) & 0xFF);
        bytes[at + 3] = static_cast<uint8_t>(v & 0xFF);
    };
    put(16, width);
    put(20, height);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

// Pushes a file's timestamp forward so a cache written a moment ago is provably
// older than its source, without sleeping out the filesystem's granularity.
void AgeSourcePastCache(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(path, now + std::chrono::seconds(5), ec);
}

} // namespace

// A cold request produces the cache file the service will later hand the UI,
// named by the documented convention, at the documented size.
TEST(TextureThumbnailHandler, ColdRequestWritesDownscaledCachePng)
{
    const auto root = MakeTempRoot("cold");
    const auto source = root / "Assets" / "brick.png";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    ASSERT_TRUE(WriteTestPng(source, 1024, 512));

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    ReadyRecorder recorder;
    const std::string immediate = handler.GetOrRequest(source, 128, recorder.Callback(), false);

    // The request path never decodes: it has nothing to return yet.
    EXPECT_TRUE(immediate.empty());

    ASSERT_TRUE(recorder.WaitForCount(1));

    const auto expected = cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source);
    EXPECT_EQ(recorder.Results[0], expected.generic_string());
    ASSERT_TRUE(std::filesystem::exists(expected));

    const ImageSize size = ReadPngSize(expected);
    ASSERT_TRUE(size.Valid);
    EXPECT_EQ(size.Width, TextureThumbnailHandler::kThumbLongEdge);
    EXPECT_EQ(size.Height, TextureThumbnailHandler::kThumbLongEdge / 2);
}

// The decode must not run on the caller's thread — the caller is the UI thread
// on every panel request.
TEST(TextureThumbnailHandler, DecodeRunsOffTheRequestingThread)
{
    const auto root = MakeTempRoot("thread");
    const auto source = root / "Assets" / "wide.png";
    ASSERT_TRUE(WriteTestPng(source, 800, 600));

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(root / ".Editor" / "Thumbnails");

    ReadyRecorder recorder;
    handler.GetOrRequest(source, 128, recorder.Callback(), false);
    ASSERT_TRUE(recorder.WaitForCount(1));

    std::lock_guard<std::mutex> lock(recorder.Mutex);
    EXPECT_NE(recorder.Threads[0], std::this_thread::get_id());
}

// A warm cache is the common case once a project has been browsed: it must
// answer from the calling thread with no worker round-trip.
TEST(TextureThumbnailHandler, WarmCacheAnswersSynchronously)
{
    const auto root = MakeTempRoot("warm");
    const auto source = root / "Assets" / "warm.png";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    ASSERT_TRUE(WriteTestPng(source, 600, 600));

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    ReadyRecorder cold;
    handler.GetOrRequest(source, 128, cold.Callback(), false);
    ASSERT_TRUE(cold.WaitForCount(1));

    ReadyRecorder warm;
    const std::string immediate = handler.GetOrRequest(source, 128, warm.Callback(), false);

    const auto expected = cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source);
    EXPECT_EQ(immediate, expected.generic_string());

    std::lock_guard<std::mutex> lock(warm.Mutex);
    ASSERT_EQ(warm.Results.size(), 1u);
    EXPECT_EQ(warm.Results[0], expected.generic_string());
    EXPECT_EQ(warm.Threads[0], std::this_thread::get_id());
}

// A source already no larger than a thumbnail is declined outright. Copying it
// at its own resolution would spend a decode and a second file on disk to
// produce nothing smaller; the service's fallback serves the source for free.
TEST(TextureThumbnailHandler, SubThumbnailSizeSourceIsDeclinedNotReEncoded)
{
    const auto root = MakeTempRoot("small");
    const auto source = root / "Assets" / "icon.png";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    ASSERT_TRUE(WriteTestPng(source, 64, 32));

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    const uint64_t decodesBefore = TextureThumbnailHandler::GetDecodeAttemptCount();

    // The requester is answered with empty, which the service turns into the
    // raw source path — but no decode ran and no cache file was written.
    ReadyRecorder recorder;
    const std::string immediate = handler.GetOrRequest(source, 128, recorder.Callback(), false);
    EXPECT_TRUE(immediate.empty());
    ASSERT_TRUE(recorder.WaitForCount(1));
    EXPECT_TRUE(recorder.Results[0].empty());

    EXPECT_FALSE(std::filesystem::exists(
        cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source)));
    EXPECT_EQ(TextureThumbnailHandler::GetDecodeAttemptCount(), decodesBefore);

    // And the verdict sticks, so browsing back does not re-read the file.
    EXPECT_TRUE(handler.AllowsRawSourceFallback(source));
    ReadyRecorder repeat;
    handler.GetOrRequest(source, 128, repeat.Callback(), false);
    EXPECT_TRUE(repeat.StaysSilent());
}

// A source at exactly the thumbnail size is on the declining side of the same
// boundary — there is still nothing to gain from re-encoding it.
TEST(TextureThumbnailHandler, SourceAtExactlyThumbnailSizeIsDeclined)
{
    const auto root = MakeTempRoot("exact");
    const auto source = root / "Assets" / "exact.png";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    ASSERT_TRUE(WriteTestPng(source, TextureThumbnailHandler::kThumbLongEdge,
                             TextureThumbnailHandler::kThumbLongEdge));

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    ReadyRecorder recorder;
    handler.GetOrRequest(source, 128, recorder.Callback(), false);
    ASSERT_TRUE(recorder.WaitForCount(1));
    EXPECT_TRUE(recorder.Results[0].empty());
    EXPECT_FALSE(std::filesystem::exists(
        cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source)));
}

// One pixel past it, the thumbnail is worth making.
TEST(TextureThumbnailHandler, SourceJustOverThumbnailSizeIsDownscaled)
{
    const auto root = MakeTempRoot("justover");
    const auto source = root / "Assets" / "justover.png";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    ASSERT_TRUE(WriteTestPng(source, TextureThumbnailHandler::kThumbLongEdge + 1,
                             TextureThumbnailHandler::kThumbLongEdge + 1));

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    ReadyRecorder recorder;
    handler.GetOrRequest(source, 128, recorder.Callback(), false);
    ASSERT_TRUE(recorder.WaitForCount(1));

    const ImageSize size =
        ReadPngSize(cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source));
    ASSERT_TRUE(size.Valid);
    EXPECT_EQ(size.Width, TextureThumbnailHandler::kThumbLongEdge);
    EXPECT_EQ(size.Height, TextureThumbnailHandler::kThumbLongEdge);
}

// Extensions the CPU decoder cannot read are declined outright, so
// ThumbnailService keeps its raw-source fallback for them.
TEST(TextureThumbnailHandler, UndecodableExtensionsAreDeclined)
{
    EXPECT_TRUE(TextureThumbnailHandler::IsDecodableSourceExtension("a.png"));
    EXPECT_TRUE(TextureThumbnailHandler::IsDecodableSourceExtension("a.PNG"));
    EXPECT_TRUE(TextureThumbnailHandler::IsDecodableSourceExtension("a.jpg"));
    EXPECT_TRUE(TextureThumbnailHandler::IsDecodableSourceExtension("a.jpeg"));
    EXPECT_TRUE(TextureThumbnailHandler::IsDecodableSourceExtension("a.bmp"));
    EXPECT_TRUE(TextureThumbnailHandler::IsDecodableSourceExtension("a.tga"));
    EXPECT_TRUE(TextureThumbnailHandler::IsDecodableSourceExtension("a.hdr"));

    EXPECT_FALSE(TextureThumbnailHandler::IsDecodableSourceExtension("a.ktx2"));
    EXPECT_FALSE(TextureThumbnailHandler::IsDecodableSourceExtension("a.ktx"));
    EXPECT_FALSE(TextureThumbnailHandler::IsDecodableSourceExtension("a.dds"));
    EXPECT_FALSE(TextureThumbnailHandler::IsDecodableSourceExtension("a.svg"));
    EXPECT_FALSE(TextureThumbnailHandler::IsDecodableSourceExtension("a.exr"));
    EXPECT_FALSE(TextureThumbnailHandler::IsDecodableSourceExtension("a.fbx"));
    EXPECT_FALSE(TextureThumbnailHandler::IsDecodableSourceExtension("noext"));

    // stb can decode these, but nothing routes them here: they are absent from
    // the asset-type extension map, so claiming them would be dead surface.
    EXPECT_FALSE(TextureThumbnailHandler::IsDecodableSourceExtension("a.psd"));
    EXPECT_FALSE(TextureThumbnailHandler::IsDecodableSourceExtension("a.gif"));
}

TEST(TextureThumbnailHandler, UnsupportedFormatWritesNothingAndReturnsEmpty)
{
    const auto root = MakeTempRoot("unsupported");
    const auto source = root / "Assets" / "compressed.ktx2";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    WriteBytes(source, "not really a ktx2 container");

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);
    EXPECT_FALSE(handler.AllowsRawSourceFallback(source));

    ReadyRecorder recorder;
    const std::string immediate = handler.GetOrRequest(source, 128, recorder.Callback(), false);

    EXPECT_TRUE(immediate.empty());
    EXPECT_TRUE(recorder.StaysSilent());
    EXPECT_FALSE(std::filesystem::exists(
        cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source)));
}

// A source with a decodable extension that does not decode reports failure
// once and is not retried, so a corrupt file cannot re-decode per navigation.
TEST(TextureThumbnailHandler, UndecodableContentFailsOnceAndIsNotRetried)
{
    const auto root = MakeTempRoot("corrupt");
    const auto source = root / "Assets" / "corrupt.png";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    WriteBytes(source, "this is not a PNG at all");

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    ReadyRecorder first;
    handler.GetOrRequest(source, 128, first.Callback(), false);
    ASSERT_TRUE(first.WaitForCount(1));
    EXPECT_TRUE(first.Results[0].empty());
    EXPECT_FALSE(std::filesystem::exists(
        cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source)));

    ReadyRecorder second;
    const std::string immediate = handler.GetOrRequest(source, 128, second.Callback(), false);
    EXPECT_TRUE(immediate.empty());
    // A retry would re-decode and answer (with empty) a second time.
    EXPECT_TRUE(second.StaysSilent());
}

// With no project open there is no cache root; the handler must decline rather
// than write a thumbnail into a directory the project does not own.
TEST(TextureThumbnailHandler, NoCacheRootDeclinesWithoutWriting)
{
    const auto root = MakeTempRoot("nocacheroot");
    const auto source = root / "Assets" / "orphan.png";
    ASSERT_TRUE(WriteTestPng(source, 256, 256));

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;

    ReadyRecorder recorder;
    const std::string immediate = handler.GetOrRequest(source, 128, recorder.Callback(), false);

    EXPECT_TRUE(immediate.empty());
    EXPECT_TRUE(recorder.StaysSilent());

    // Nothing beside the source either.
    EXPECT_FALSE(std::filesystem::exists(
        source.parent_path() / TextureThumbnailHandler::MakeCacheFileName(source)));
}

// Several cells can ask for the same texture in one rebuild. Each requester
// must still be answered, from a single decode.
TEST(TextureThumbnailHandler, ConcurrentRequestsForOneSourceAllGetAnswered)
{
    const auto root = MakeTempRoot("coalesce");
    const auto source = root / "Assets" / "shared.png";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    ASSERT_TRUE(WriteTestPng(source, 900, 900));

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    const uint64_t decodesBefore = TextureThumbnailHandler::GetDecodeAttemptCount();

    ReadyRecorder recorder;
    constexpr size_t kRequests = 8;
    for (size_t i = 0; i < kRequests; ++i)
        handler.GetOrRequest(source, 128, recorder.Callback(), false);

    ASSERT_TRUE(recorder.WaitForCount(kRequests));

    // Every requester answered, from exactly one decode.
    EXPECT_EQ(TextureThumbnailHandler::GetDecodeAttemptCount(), decodesBefore + 1);

    const auto expected = cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source);
    std::lock_guard<std::mutex> lock(recorder.Mutex);
    EXPECT_EQ(recorder.Results.size(), kRequests);
    for (const auto& result : recorder.Results)
        EXPECT_EQ(result, expected.generic_string());
}

// The cache key is the source path and nothing else: two different textures
// never collide, and the same texture requested at two sizes shares one file.
TEST(TextureThumbnailHandler, CacheNameKeysOnPathOnly)
{
    const std::string a = TextureThumbnailHandler::MakeCacheFileName("C:/p/Assets/a.png");
    const std::string b = TextureThumbnailHandler::MakeCacheFileName("C:/p/Assets/b.png");
    EXPECT_NE(a, b);
    EXPECT_EQ(a.rfind("texthumb_", 0), 0u);
    EXPECT_NE(a.find(".png"), std::string::npos);

    // Separator form must not fork the key: panels and the preload thread hand
    // the same asset through in different path forms. Only Windows treats the
    // backslash as a separator; on POSIX it is an ordinary character, so the
    // generic_string() the key hashes cannot fold the two forms there.
#if defined(_WIN32)
    EXPECT_EQ(TextureThumbnailHandler::MakeCacheFileName("C:/p/Assets/a.png"),
              TextureThumbnailHandler::MakeCacheFileName("C:\\p\\Assets\\a.png"));
#endif
}

// Tearing the handler down while a decode is queued must not hang, and must not
// call back into state the caller has already destroyed.
TEST(TextureThumbnailHandler, DestructionDuringWorkDropsPendingCallbacks)
{
    const auto root = MakeTempRoot("teardown");
    const auto cacheRoot = root / ".Editor" / "Thumbnails";

    std::atomic<int> callbackCount{0};
    {
        HandlerHarness harness;
        TextureThumbnailHandler& handler = harness.Handler;
        handler.SetCacheRoot(cacheRoot);

        for (int i = 0; i < 16; ++i)
        {
            const auto source = root / "Assets" / ("bulk" + std::to_string(i) + ".png");
            ASSERT_TRUE(WriteTestPng(source, 1200, 1200));
            handler.GetOrRequest(source, 128, [&callbackCount](const std::string&)
                                 { callbackCount.fetch_add(1); }, false);
        }
    }

    // The destructor waited for its in-flight decode jobs; no callback may
    // arrive after this point.
    const int settled = callbackCount.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(callbackCount.load(), settled);
}

// The preview panes size their request to the viewport — up to 4096 — and are
// the surface people inspect textures on. Answering them from a 512-pixel cache
// would be a silent downgrade, so a request for more pixels than the cache
// holds is declined and the service serves the source instead. This holds even
// once a cache file exists.
TEST(TextureThumbnailHandler, RequestsLargerThanTheCacheAreDeclined)
{
    const auto root = MakeTempRoot("bigrequest");
    const auto source = root / "Assets" / "hero.png";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    ASSERT_TRUE(WriteTestPng(source, 1024, 1024));

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    // Warm the cache through a grid-sized request.
    ReadyRecorder cell;
    handler.GetOrRequest(source, 128, cell.Callback(), false);
    ASSERT_TRUE(cell.WaitForCount(1));
    const auto cached = cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source);
    ASSERT_TRUE(std::filesystem::exists(cached));

    // The preview pane's default and its viewport-sized maximum both decline.
    for (int desired : {TextureThumbnailHandler::kThumbLongEdge + 1, 1024, 4096})
    {
        ReadyRecorder preview;
        const std::string immediate =
            handler.GetOrRequest(source, desired, preview.Callback(), false);
        EXPECT_TRUE(immediate.empty()) << "desiredSize=" << desired;
        EXPECT_TRUE(preview.StaysSilent(std::chrono::milliseconds(250)))
            << "desiredSize=" << desired;
    }

    // A request the cache can satisfy still gets it.
    const std::string small = handler.GetOrRequest(source, 256, nullptr, false);
    EXPECT_EQ(small, cached.generic_string());
}

// An image shown inline (a capture in the AI Assistant) gets its own copy at
// kInlineImageLongEdge, beside the tile image, which stays at kThumbLongEdge;
// a source no larger than the inline copy is answered with itself. The preview
// panes' requests are untouched (RequestsLargerThanTheCacheAreDeclined).
TEST(TextureThumbnailHandler, AnInlineImageIsCachedAtItsOwnSizeBesideTheTile)
{
    const auto root = MakeTempRoot("inline");
    const auto source = root / "Assets" / "capture.png";
    const auto small = root / "Assets" / "small.png";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    ASSERT_TRUE(WriteTestPng(source, 2048, 1024));
    ASSERT_TRUE(WriteTestPng(small, 800, 600));

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    ReadyRecorder inlineImage;
    EXPECT_TRUE(handler.GetOrRequestInlineImage(source, inlineImage.Callback()).empty());
    ASSERT_TRUE(inlineImage.WaitForCount(1));
    const ImageSize inlineSize = ReadPngSize(inlineImage.Results[0]);
    ASSERT_TRUE(inlineSize.Valid) << inlineImage.Results[0];
    EXPECT_EQ(inlineSize.Width, TextureThumbnailHandler::kInlineImageLongEdge);
    EXPECT_EQ(inlineSize.Height, TextureThumbnailHandler::kInlineImageLongEdge / 2);

    ReadyRecorder tile;
    handler.GetOrRequest(source, 128, tile.Callback(), false);
    ASSERT_TRUE(tile.WaitForCount(1));
    const auto tilePath = cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source);
    EXPECT_EQ(tile.Results[0], tilePath.generic_string());
    EXPECT_EQ(ReadPngSize(tilePath).Width, TextureThumbnailHandler::kThumbLongEdge) << "the tile image is unchanged";
    EXPECT_NE(tile.Results[0], inlineImage.Results[0]);

    ReadyRecorder smallImage;
    handler.GetOrRequestInlineImage(small, smallImage.Callback());
    ASSERT_TRUE(smallImage.WaitForCount(1));
    EXPECT_EQ(smallImage.Results[0], small.generic_string()) << "a source no larger than the copy is shown as it is";
}

// A cache entry older than its source is stale. The editor's other thumbnail
// caches key on the path alone and never notice; textures are edited in place
// constantly, so this one checks the timestamp it already has to hand.
TEST(TextureThumbnailHandler, EditedSourceInvalidatesTheCachedThumbnail)
{
    const auto root = MakeTempRoot("stale");
    const auto source = root / "Assets" / "edited.png";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    ASSERT_TRUE(WriteTestPng(source, 1024, 512));

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    ReadyRecorder first;
    handler.GetOrRequest(source, 128, first.Callback(), false);
    ASSERT_TRUE(first.WaitForCount(1));

    const auto cached = cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source);
    ASSERT_TRUE(std::filesystem::exists(cached));

    // Re-author the source at a different shape, then stamp it newer.
    ASSERT_TRUE(WriteTestPng(source, 512, 1024));
    AgeSourcePastCache(source);

    const uint64_t decodesBefore = TextureThumbnailHandler::GetDecodeAttemptCount();

    ReadyRecorder second;
    const std::string immediate = handler.GetOrRequest(source, 128, second.Callback(), false);
    EXPECT_TRUE(immediate.empty()) << "a stale cache must not be served";
    ASSERT_TRUE(second.WaitForCount(1));
    EXPECT_EQ(TextureThumbnailHandler::GetDecodeAttemptCount(), decodesBefore + 1);

    const ImageSize size = ReadPngSize(cached);
    ASSERT_TRUE(size.Valid);
    EXPECT_EQ(size.Width, TextureThumbnailHandler::kThumbLongEdge / 2);
    EXPECT_EQ(size.Height, TextureThumbnailHandler::kThumbLongEdge);
}

// A source past the decode budget is refused from its header, without ever
// allocating the image. The header is forged rather than authored: the point is
// that the declared size decides, before any pixels are touched.
TEST(TextureThumbnailHandler, OversizedSourceIsRefusedWithoutDecoding)
{
    const auto root = MakeTempRoot("oversize");
    const auto source = root / "Assets" / "huge.png";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    ASSERT_TRUE(WriteTestPng(source, 8, 8));
    ASSERT_TRUE(ForgePngDimensions(source, 8192, 8192)); // 67 Mpx, past the budget

    int w = 0, h = 0, c = 0;
    ASSERT_TRUE(stbi_info(source.string().c_str(), &w, &h, &c));
    ASSERT_EQ(static_cast<uint64_t>(w) * h, 8192ull * 8192ull);
    ASSERT_GT(static_cast<uint64_t>(w) * h, TextureThumbnailHandler::kMaxSourcePixels);

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    const uint64_t decodesBefore = TextureThumbnailHandler::GetDecodeAttemptCount();

    ReadyRecorder recorder;
    handler.GetOrRequest(source, 128, recorder.Callback(), false);
    ASSERT_TRUE(recorder.WaitForCount(1));
    EXPECT_TRUE(recorder.Results[0].empty());

    // The whole point: the header was read, the image never was.
    EXPECT_FALSE(handler.AllowsRawSourceFallback(source));
    EXPECT_EQ(TextureThumbnailHandler::GetDecodeAttemptCount(), decodesBefore);
    EXPECT_FALSE(std::filesystem::exists(
        cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source)));

    ReadyRecorder repeat;
    handler.GetOrRequest(source, 128, repeat.Callback(), false);
    EXPECT_TRUE(repeat.StaysSilent());
}

// A decline is a verdict on one revision of a file, not on the path. Editing
// the source must reopen it — otherwise fixing a corrupt texture would need an
// editor restart.
TEST(TextureThumbnailHandler, EditingADeclinedSourceReopensTheDecision)
{
    const auto root = MakeTempRoot("undecline");
    const auto source = root / "Assets" / "wasbroken.png";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    WriteBytes(source, "this is not a PNG at all");

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    ReadyRecorder broken;
    handler.GetOrRequest(source, 128, broken.Callback(), false);
    ASSERT_TRUE(broken.WaitForCount(1));
    EXPECT_TRUE(broken.Results[0].empty());

    ReadyRecorder stillBroken;
    handler.GetOrRequest(source, 128, stillBroken.Callback(), false);
    EXPECT_TRUE(stillBroken.StaysSilent()) << "the decline must hold while the file does";

    // The user fixes the file.
    ASSERT_TRUE(WriteTestPng(source, 1024, 1024));
    AgeSourcePastCache(source);

    ReadyRecorder fixed;
    handler.GetOrRequest(source, 128, fixed.Callback(), false);
    ASSERT_TRUE(fixed.WaitForCount(1));
    EXPECT_EQ(fixed.Results[0],
              (cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source)).generic_string());
}

// Paths do not survive a project switch, so neither may the verdicts recorded
// against them.
TEST(TextureThumbnailHandler, ProjectSwitchClearsDeclinedSources)
{
    const auto root = MakeTempRoot("switch");
    const auto source = root / "Assets" / "corrupt.png";
    const auto cacheRoot = root / ".Editor" / "Thumbnails";
    WriteBytes(source, "still not a PNG");

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    ReadyRecorder first;
    handler.GetOrRequest(source, 128, first.Callback(), false);
    ASSERT_TRUE(first.WaitForCount(1));

    ReadyRecorder suppressed;
    handler.GetOrRequest(source, 128, suppressed.Callback(), false);
    ASSERT_TRUE(suppressed.StaysSilent());

    handler.ResetForProjectSwitch();

    // The verdict is gone: the same request is evaluated again and answers.
    ReadyRecorder afterReset;
    handler.GetOrRequest(source, 128, afterReset.Callback(), false);
    EXPECT_TRUE(afterReset.WaitForCount(1)) << "ResetForProjectSwitch must clear declines";
}

// Two thumbnails generated at once must not race for the same temp name, and
// nor must two editors sharing a project cache. std::thread::id is only
// system-unique on some standard libraries, so the name carries the pid too.
TEST(TextureThumbnailHandler, ConcurrentGenerationProducesIntactCacheFiles)
{
    const auto root = MakeTempRoot("tempname");
    const auto cacheRoot = root / ".Editor" / "Thumbnails";

    HandlerHarness harness;
    TextureThumbnailHandler& handler = harness.Handler;
    handler.SetCacheRoot(cacheRoot);

    ReadyRecorder recorder;
    constexpr size_t kSources = 12;
    std::vector<std::filesystem::path> sources;
    for (size_t i = 0; i < kSources; ++i)
    {
        const auto source = root / "Assets" / ("tex" + std::to_string(i) + ".png");
        ASSERT_TRUE(WriteTestPng(source, 1024, 768));
        sources.push_back(source);
        handler.GetOrRequest(source, 128, recorder.Callback(), false);
    }

    ASSERT_TRUE(recorder.WaitForCount(kSources, std::chrono::seconds(30)));

    for (const auto& source : sources)
    {
        const ImageSize size =
            ReadPngSize(cacheRoot / TextureThumbnailHandler::MakeCacheFileName(source));
        ASSERT_TRUE(size.Valid) << source.string();
        EXPECT_EQ(size.Width, TextureThumbnailHandler::kThumbLongEdge);
    }

    // No temp file survived its rename.
    for (const auto& entry : std::filesystem::directory_iterator(cacheRoot))
        EXPECT_NE(entry.path().extension(), ".tmp") << entry.path().string();
}


TEST(TextureThumbnailHandler, ThrowingCallbacksDoNotStrandSlotsOrBlockShutdown)
{
    const auto root = MakeTempRoot("callback-exception");
    ReadyRecorder final;
    HandlerHarness harness;
    harness.Handler.SetCacheRoot(root / ".Editor/Thumbnails");
    for (int i = 0; i < 2; ++i)
    {
        const auto source = root / "Assets" / ("throws" + std::to_string(i) + ".png");
        ASSERT_TRUE(WriteTestPng(source, 8, 8));
        harness.Handler.GetOrRequest(source, 128, [](const std::string&) {
            throw std::runtime_error("thumbnail callback failure");
        }, false);
    }
    const auto source = root / "Assets/final.png";
    ASSERT_TRUE(WriteTestPng(source, 8, 8));
    harness.Handler.GetOrRequest(source, 128, final.Callback(), false);
    EXPECT_TRUE(final.WaitForCount(1, std::chrono::seconds(3)));
    // Destruction also waits for every claimed slot, including failed callbacks.
}
