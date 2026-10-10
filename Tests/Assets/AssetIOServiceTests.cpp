#include <gtest/gtest.h>
#include "Assets/AssetDecodeGate.h"
#include "Assets/AssetIOService.h"
#include "Assets/TextureCookWorkers.h"
#include "JobSystem/TaskHandle.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "TestTempDir.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr auto kResultTimeout = std::chrono::seconds(10);

AssetMetadata MakeMetadata(const std::filesystem::path& path, size_t fileSize,
                           AssetType type = AssetType::Unknown)
{
    AssetMetadata metadata;
    metadata.Guid = GUID::Generate();
    metadata.Path = path;
    metadata.Name = path.stem().string();
    metadata.Extension = path.extension().string();
    metadata.Type = type;
    metadata.FileSize = fileSize;
    return metadata;
}

void WriteFile(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream file(path, std::ios::binary);
    file << content;
}

} // namespace

class AssetIOServiceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        testDir = TestUtils::MakeUniqueTempDirectory("asset_io_service_test");
        std::filesystem::create_directories(testDir);

        jobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        service = std::make_unique<AssetIOService>();
        // Single reader thread: deterministic claim/queue split for the
        // priority and shutdown tests.
        service->Start(*jobSystem, 1);
    }

    void TearDown() override
    {
        service->Stop();
        service.reset();
        jobSystem->Shutdown();
        jobSystem.reset();
        std::filesystem::remove_all(testDir);
    }

    // A read request whose decode continuation stashes the bytes and whose
    // failure path stores the error. Exactly one of the two futures resolves.
    struct CapturedRead
    {
        std::shared_ptr<std::promise<Vector<uint8>>> BytesPromise = std::make_shared<std::promise<Vector<uint8>>>();
        std::shared_ptr<std::promise<String>> FailurePromise = std::make_shared<std::promise<String>>();
        std::future<Vector<uint8>> Bytes = BytesPromise->get_future();
        std::future<String> Failure = FailurePromise->get_future();
    };

    AssetIOService::ReadRequest MakeCapturedRequest(const AssetMetadata& metadata, CapturedRead& capture,
                                                    AssetLoadPriority priority = AssetLoadPriority::Normal)
    {
        AssetIOService::ReadRequest request;
        request.AssetGuid = metadata.Guid;
        request.Metadata = metadata;
        request.Priority = priority;
        request.ProcessData = [bytesPromise = capture.BytesPromise](Vector<uint8> data) -> SharedPtr<Asset>
        {
            bytesPromise->set_value(std::move(data));
            return SharedPtr<Asset>{};
        };
        request.OnFailure = [failurePromise = capture.FailurePromise](const String& error)
        {
            failurePromise->set_value(error);
        };
        return request;
    }

    // Poll until the reader has claimed enough queued reads to leave `target`
    // behind it, then report the depth actually observed. The deadline only
    // bounds a hang: callers assert on the returned depth, never on elapsed
    // time, so machine load can delay the claim without failing the test.
    size_t AwaitQueueDepth(size_t target)
    {
        const auto deadline = std::chrono::steady_clock::now() + kResultTimeout;
        size_t queued = service->GetQueuedReadCount();
        while (queued != target && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            queued = service->GetQueuedReadCount();
        }
        return queued;
    }

    std::filesystem::path testDir;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> jobSystem;
    std::unique_ptr<AssetIOService> service;
};

TEST_F(AssetIOServiceTest, ReadsFileBytesIntoDecodeJob)
{
    const std::string content = "This is a test asset file for AssetIOService.";
    const auto path = testDir / "test_asset.txt";
    WriteFile(path, content);

    CapturedRead capture;
    service->SubmitRead(MakeCapturedRequest(MakeMetadata(path, content.size()), capture));

    ASSERT_EQ(capture.Bytes.wait_for(kResultTimeout), std::future_status::ready);
    const Vector<uint8> data = capture.Bytes.get();
    ASSERT_EQ(data.size(), content.size());
    EXPECT_EQ(std::string(data.begin(), data.end()), content);
}

TEST_F(AssetIOServiceTest, ReadsOnlyTheRequestedByteRange)
{
    const std::string content = "headerPAGE-BYTEStrailer";
    const auto path = testDir / "store.gepage";
    WriteFile(path, content);

    CapturedRead capture;
    AssetIOService::ReadRequest request = MakeCapturedRequest(MakeMetadata(path, content.size()), capture);
    request.RangeOffset = 6;
    request.RangeBytes = 10;
    service->SubmitRead(std::move(request));

    ASSERT_EQ(capture.Bytes.wait_for(kResultTimeout), std::future_status::ready);
    const Vector<uint8> data = capture.Bytes.get();
    EXPECT_EQ(std::string(data.begin(), data.end()), "PAGE-BYTES");
}

TEST_F(AssetIOServiceTest, RangePastTheEndOfTheFileFails)
{
    const std::string content = "short";
    const auto path = testDir / "short.gepage";
    WriteFile(path, content);

    CapturedRead capture;
    AssetIOService::ReadRequest request = MakeCapturedRequest(MakeMetadata(path, content.size()), capture);
    request.RangeOffset = 2;
    request.RangeBytes = 10;
    service->SubmitRead(std::move(request));

    ASSERT_EQ(capture.Failure.wait_for(kResultTimeout), std::future_status::ready);
    EXPECT_NE(capture.Failure.get().find("Short read"), std::string::npos);

    // A range no file could hold is refused before anything is allocated for it.
    CapturedRead huge;
    AssetIOService::ReadRequest hugeRequest = MakeCapturedRequest(MakeMetadata(path, content.size()), huge);
    hugeRequest.RangeOffset = 1;
    hugeRequest.RangeBytes = ~0ull - 1u;
    service->SubmitRead(std::move(hugeRequest));
    ASSERT_EQ(huge.Failure.wait_for(kResultTimeout), std::future_status::ready);
    EXPECT_NE(huge.Failure.get().find("past the end"), std::string::npos);
}

TEST_F(AssetIOServiceTest, AcceptsStaleMetadataFileSize)
{
    const std::string content = "stale-size tolerated because the on-disk size confirms the read";
    const auto path = testDir / "stale_size.bin";
    WriteFile(path, content);

    // Advisory size lies; the re-stat confirms the read and the request succeeds.
    CapturedRead capture;
    service->SubmitRead(MakeCapturedRequest(MakeMetadata(path, /*fileSize=*/1), capture));

    ASSERT_EQ(capture.Bytes.wait_for(kResultTimeout), std::future_status::ready);
    EXPECT_EQ(capture.Bytes.get().size(), content.size());
}

TEST_F(AssetIOServiceTest, RetryWindowToleratesLateFileAppearance)
{
    // Hot-reload rename/write tolerance: the file does not exist when the read
    // is claimed, and appears while the retry loop (5 x 10ms) is running.
    const std::string content = "appeared mid-retry";
    const auto path = testDir / "late_file.txt";

    CapturedRead capture;
    service->SubmitRead(MakeCapturedRequest(MakeMetadata(path, content.size()), capture));

    // Keep the write early in the ~40ms retry window (attempts at ~0/10/20/
    // 30/40ms): a 20ms delay left only the last two attempts as successes and
    // flaked under CPU contention (observed while a -j12 build ran).
    //
    // Write-temp-then-rename, the hot-reload pattern the retry window is
    // designed for: a plain create-then-write exposes an opened-but-empty
    // window, which the retry loop treats as a hard failure ("Asset data is
    // empty") in both the old and new read paths — reproduced under load.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto tmpPath = testDir / "late_file.txt.tmp";
    WriteFile(tmpPath, content);
    std::filesystem::rename(tmpPath, path);

    if (capture.Bytes.wait_for(kResultTimeout) != std::future_status::ready)
    {
        // Distinguish the two ways this can fail: the read resolved through
        // OnFailure (retry window missed / mid-write observation), or neither
        // callback fired at all (decode job never ran — a pool-side bug).
        if (capture.Failure.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            FAIL() << "read resolved through OnFailure: " << capture.Failure.get();
        FAIL() << "neither ProcessData nor OnFailure resolved within timeout";
    }
    EXPECT_EQ(capture.Bytes.get().size(), content.size());
}

TEST_F(AssetIOServiceTest, ReadFailureResolvesOnFailure)
{
    const auto path = testDir / "never_exists.txt";

    CapturedRead capture;
    service->SubmitRead(MakeCapturedRequest(MakeMetadata(path, 16), capture));

    ASSERT_EQ(capture.Failure.wait_for(kResultTimeout), std::future_status::ready);
    const String error = capture.Failure.get();
    EXPECT_NE(error.find("Failed to read file"), String::npos) << "actual error: " << error;
    EXPECT_EQ(capture.Bytes.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
}

TEST_F(AssetIOServiceTest, HigherPriorityReadsClaimedFirstUnderSaturatedQueue)
{
    // Occupy the single reader with a missing-file read (5 x 10ms retry loop),
    // then queue one request per priority level while it is busy. The reader
    // must claim them highest-priority-first, FIFO being irrelevant here.
    CapturedRead gateCapture;
    service->SubmitRead(MakeCapturedRequest(MakeMetadata(testDir / "gate_missing.txt", 4), gateCapture));
    ASSERT_EQ(AwaitQueueDepth(0), 0u) << "the reader never claimed the gate read";

    const std::string content = "priority probe";
    std::mutex orderMutex;
    std::vector<AssetLoadPriority> claimOrder;
    std::atomic<int> submitted{0};
    std::atomic<int> unexpectedFailures{0};

    const AssetLoadPriority priorities[] = {AssetLoadPriority::Low, AssetLoadPriority::Normal,
                                            AssetLoadPriority::High, AssetLoadPriority::Critical};
    for (AssetLoadPriority priority : priorities)
    {
        const auto path = testDir / ("prio_" + std::to_string(static_cast<int>(priority)) + ".txt");
        WriteFile(path, content);

        AssetIOService::ReadRequest request;
        request.AssetGuid = GUID::Generate();
        request.Metadata = MakeMetadata(path, content.size());
        request.Priority = priority;
        request.ProcessData = [](Vector<uint8>) -> SharedPtr<Asset> { return SharedPtr<Asset>{}; };
        request.OnSubmitted = [&orderMutex, &claimOrder, &submitted, priority](const JobSystem::TaskHandle&)
        {
            std::lock_guard<std::mutex> lock(orderMutex);
            claimOrder.push_back(priority);
            submitted.fetch_add(1);
        };
        request.OnFailure = [&unexpectedFailures](const String&) { unexpectedFailures.fetch_add(1); };
        service->SubmitRead(std::move(request));
    }

    const auto deadline = std::chrono::steady_clock::now() + kResultTimeout;
    while (submitted.load() < 4 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_EQ(unexpectedFailures.load(), 0);
    std::lock_guard<std::mutex> lock(orderMutex);
    ASSERT_EQ(claimOrder.size(), 4u);
    EXPECT_EQ(claimOrder[0], AssetLoadPriority::Critical);
    EXPECT_EQ(claimOrder[1], AssetLoadPriority::High);
    EXPECT_EQ(claimOrder[2], AssetLoadPriority::Normal);
    EXPECT_EQ(claimOrder[3], AssetLoadPriority::Low);
}

TEST_F(AssetIOServiceTest, StopFailsQueuedReadsAndJoinsThreads)
{
    // Occupy the reader, queue several reads behind it, then Stop(). The
    // claimed read runs to completion; the queued reads are abandoned with
    // their OnFailure resolved (no stranded waiter), and Stop() returns only
    // after the reader thread is joined.
    CapturedRead gateCapture;
    service->SubmitRead(MakeCapturedRequest(MakeMetadata(testDir / "gate_missing.txt", 4), gateCapture));

    const std::string content = "queued behind gate";
    std::atomic<int> failures{0};
    std::atomic<int> submissions{0};
    constexpr int kQueuedReads = 5;
    for (int i = 0; i < kQueuedReads; ++i)
    {
        const auto path = testDir / ("queued_" + std::to_string(i) + ".txt");
        WriteFile(path, content);

        AssetIOService::ReadRequest request;
        request.AssetGuid = GUID::Generate();
        request.Metadata = MakeMetadata(path, content.size());
        request.ProcessData = [](Vector<uint8>) -> SharedPtr<Asset> { return SharedPtr<Asset>{}; };
        request.OnSubmitted = [&submissions](const JobSystem::TaskHandle&) { submissions.fetch_add(1); };
        request.OnFailure = [&failures](const String&) { failures.fetch_add(1); };
        service->SubmitRead(std::move(request));
    }

    ASSERT_EQ(AwaitQueueDepth(static_cast<size_t>(kQueuedReads)), static_cast<size_t>(kQueuedReads))
        << "the reader must be inside the gate read with every other read still queued";
    service->Stop();

    // The gate read itself resolves (open failure) because it was claimed.
    ASSERT_EQ(gateCapture.Failure.wait_for(std::chrono::milliseconds(0)), std::future_status::ready);
    EXPECT_EQ(failures.load() + submissions.load(), kQueuedReads)
        << "every queued read must resolve exactly once through one of the callbacks";
    EXPECT_EQ(submissions.load(), 0) << "reads queued behind the gate must not be claimed after Stop";
}

TEST_F(AssetIOServiceTest, SubmitAfterStopFailsInline)
{
    service->Stop();

    const auto path = testDir / "after_stop.txt";
    WriteFile(path, "x");

    bool failed = false;
    AssetIOService::ReadRequest request;
    request.AssetGuid = GUID::Generate();
    request.Metadata = MakeMetadata(path, 1);
    request.ProcessData = [](Vector<uint8>) -> SharedPtr<Asset> { return SharedPtr<Asset>{}; };
    request.OnFailure = [&failed](const String&) { failed = true; };
    service->SubmitRead(std::move(request));

    EXPECT_TRUE(failed) << "SubmitRead on a stopped service must fail the request inline";
}

TEST_F(AssetIOServiceTest, CancelRequestRemovesQueuedRead)
{
    // Occupy the single reader with a missing-file read (5 x 10ms retry
    // window), queue a real read behind it, then cancel the queued one: its
    // OnFailure resolves inline on this thread and its decode never runs.
    CapturedRead gateCapture;
    service->SubmitRead(MakeCapturedRequest(MakeMetadata(testDir / "gate_missing.txt", 4), gateCapture));
    ASSERT_EQ(AwaitQueueDepth(0), 0u) << "the reader never claimed the gate read";

    const std::string content = "queued then cancelled";
    const auto path = testDir / "cancel_queued.txt";
    WriteFile(path, content);

    const AssetMetadata metadata = MakeMetadata(path, content.size());
    CapturedRead capture;
    auto request = MakeCapturedRequest(metadata, capture);
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    request.CancelRequested = cancelFlag;
    service->SubmitRead(std::move(request));

    EXPECT_TRUE(service->CancelRequest(metadata.Guid, cancelFlag)) << "the read must still be queued behind the gate";

    ASSERT_EQ(capture.Failure.wait_for(std::chrono::milliseconds(0)), std::future_status::ready)
        << "queue removal must resolve OnFailure inline on the cancelling thread";
    EXPECT_NE(capture.Failure.get().find("cancelled"), String::npos);
    EXPECT_EQ(capture.Bytes.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout)
        << "a cancelled queued read must never reach ProcessData";

    // The service stays healthy for later reads.
    const std::string afterContent = "still serving";
    const auto afterPath = testDir / "after_cancel.txt";
    WriteFile(afterPath, afterContent);
    CapturedRead afterCapture;
    service->SubmitRead(MakeCapturedRequest(MakeMetadata(afterPath, afterContent.size()), afterCapture));
    ASSERT_EQ(afterCapture.Bytes.wait_for(kResultTimeout), std::future_status::ready);
    EXPECT_EQ(afterCapture.Bytes.get().size(), afterContent.size());
}

TEST_F(AssetIOServiceTest, CancelRequestOnlyRemovesMatchingGeneration)
{
    // Generation identity: a stale canceller (holding an OLD load's flag for
    // the same GUID) must not withdraw a NEWER load's queued read — the
    // suppression discriminator on the newer load's callbacks is its own
    // (unset) flag, so a friendly-fire removal would both null the waiters
    // and mark the GUID suppressed.
    CapturedRead gateCapture;
    service->SubmitRead(MakeCapturedRequest(MakeMetadata(testDir / "gen_gate_missing.txt", 4), gateCapture));
    ASSERT_EQ(AwaitQueueDepth(0), 0u) << "the reader never claimed the gate read";

    const std::string content = "newer generation survives a stale cancel";
    const auto path = testDir / "gen_survivor.txt";
    WriteFile(path, content);

    const AssetMetadata metadata = MakeMetadata(path, content.size());
    CapturedRead capture;
    auto request = MakeCapturedRequest(metadata, capture);
    auto currentFlag = std::make_shared<std::atomic<bool>>(false);
    request.CancelRequested = currentFlag;
    service->SubmitRead(std::move(request));

    const auto staleFlag = std::make_shared<std::atomic<bool>>(true);
    EXPECT_FALSE(service->CancelRequest(metadata.Guid, staleFlag))
        << "a stale generation's cancel must not touch the newer load's queued read";

    ASSERT_EQ(capture.Bytes.wait_for(kResultTimeout), std::future_status::ready)
        << "the newer load must still run to its decode continuation";
    EXPECT_EQ(capture.Bytes.get().size(), content.size());
}

TEST_F(AssetIOServiceTest, CancelledClaimedReadIsDiscardedBeforeDecodeSubmit)
{
    // A claimed read runs to completion, but a cancel observed before the
    // decode submission discards the result through OnFailure. The missing
    // file keeps the reader inside the ~50ms retry window, so the flag is set
    // and the file appears after the claim: the pre-submit check is what
    // observes the cancel, and the observable contract is OnFailure fires,
    // ProcessData never does.
    const std::string content = "read completes, result discarded";
    const auto path = testDir / "cancel_claimed.txt";

    const AssetMetadata metadata = MakeMetadata(path, content.size());
    CapturedRead capture;
    auto request = MakeCapturedRequest(metadata, capture);
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    request.CancelRequested = cancelFlag;
    service->SubmitRead(std::move(request));

    ASSERT_EQ(AwaitQueueDepth(0), 0u) << "the reader never claimed the read";
    EXPECT_FALSE(service->CancelRequest(metadata.Guid, cancelFlag))
        << "a claimed read must not be withdrawable from the queue";
    cancelFlag->store(true, std::memory_order_release);

    const auto tmpPath = testDir / "cancel_claimed.txt.tmp";
    WriteFile(tmpPath, content);
    std::filesystem::rename(tmpPath, path); // read succeeds inside the retry window

    ASSERT_EQ(capture.Failure.wait_for(kResultTimeout), std::future_status::ready);
    EXPECT_NE(capture.Failure.get().find("cancelled"), String::npos);
    EXPECT_EQ(capture.Bytes.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout)
        << "a cancelled request must never submit its decode continuation";
}

TEST_F(AssetIOServiceTest, EmptyFileGetsOneRetryTickBeforeFailing)
{
    // Opened-mid-write tolerance: a truncate-then-write save can be observed
    // as an opened-but-empty file. The read gives the writer one retry tick
    // (10ms) before failing; content arriving within the tick is returned.
    const std::string content = "written after the empty observation";
    const auto path = testDir / "empty_then_written.txt";
    WriteFile(path, ""); // exists but empty (mid-write observation)

    CapturedRead capture;
    service->SubmitRead(MakeCapturedRequest(MakeMetadata(path, content.size()), capture));

    // Write-temp-then-rename immediately: if the reader already observed the
    // empty file, the retry tick picks the content up; if it has not claimed
    // yet, the first read simply succeeds. Atomic rename avoids a partial-
    // content observation (which would trip the size-mismatch check instead).
    const auto tmpPath = testDir / "empty_then_written.txt.tmp";
    WriteFile(tmpPath, content);
    std::filesystem::rename(tmpPath, path);

    if (capture.Bytes.wait_for(kResultTimeout) != std::future_status::ready)
    {
        if (capture.Failure.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            FAIL() << "read resolved through OnFailure: " << capture.Failure.get();
        FAIL() << "neither ProcessData nor OnFailure resolved within timeout";
    }
    EXPECT_EQ(capture.Bytes.get().size(), content.size());

    // A file that stays empty past the tick still hard-fails.
    const auto emptyPath = testDir / "stays_empty.txt";
    WriteFile(emptyPath, "");
    CapturedRead emptyCapture;
    service->SubmitRead(MakeCapturedRequest(MakeMetadata(emptyPath, 8), emptyCapture));
    ASSERT_EQ(emptyCapture.Failure.wait_for(kResultTimeout), std::future_status::ready);
    EXPECT_NE(emptyCapture.Failure.get().find("empty"), String::npos);
}

namespace
{
// Texture decodes that block until released, standing in for decodes that run a
// cook for minutes while they hold the decode gate.
struct HeldCooks
{
    std::promise<void> Release;
    std::shared_future<void> Released = Release.get_future().share();
    std::atomic<int> Running{0};
    std::atomic<int> Peak{0};
    std::atomic<int> Finished{0};
};

// A decode that only reports that it ran.
struct DecodeSignal
{
    std::shared_ptr<std::promise<void>> Decoded = std::make_shared<std::promise<void>>();
    std::future<void> Done = Decoded->get_future();
};

SharedPtr<Asset> SignalDecoded(std::promise<void>& decoded)
{
    decoded.set_value();
    return SharedPtr<Asset>{};
}

SharedPtr<Asset> HoldLikeACook(HeldCooks& cooks)
{
    const int running = cooks.Running.fetch_add(1) + 1;
    int peak = cooks.Peak.load();
    while (running > peak && !cooks.Peak.compare_exchange_weak(peak, running))
    {
    }
    cooks.Released.wait();
    cooks.Running.fetch_sub(1);
    cooks.Finished.fetch_add(1);
    return SharedPtr<Asset>{};
}

bool AwaitCount(const std::atomic<int>& count, int target)
{
    const auto deadline = std::chrono::steady_clock::now() + kResultTimeout;
    while (count.load() < target && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return count.load() >= target;
}
} // namespace

// A decode the frame waits on (a material during scene resolve) must not queue
// behind texture decodes that hold the decode gate for a whole cook. More texture
// decodes are queued than the gate admits, and they hold until released: the
// material read must still be claimed, decoded and completed while they hold.
TEST(AssetIOServiceDecodeGate, ShortDecodeIsNotQueuedBehindTextureCooks)
{
    const auto testDir = TestUtils::MakeUniqueTempDirectory("asset_io_gate_test");
    std::filesystem::create_directories(testDir);
    {
        // Six workers: the gate admits four decodes at once, two of them textures.
        constexpr size_t kWorkers = 6;
        JobSystem::WorkStealingThreadPool pool(kWorkers);
        AssetIOService service;
        service.Start(pool, 2);

        HeldCooks cooks;
        constexpr int kTextureDecodes = 6;
        const std::string content = "texture source bytes";
        for (int i = 0; i < kTextureDecodes; ++i)
        {
            const auto path = testDir / ("cook_" + std::to_string(i) + ".png");
            WriteFile(path, content);
            AssetIOService::ReadRequest request;
            request.AssetGuid = GUID::Generate();
            request.Metadata = MakeMetadata(path, content.size(), AssetType::Texture);
            request.ProcessData = [&cooks](Vector<uint8>) { return HoldLikeACook(cooks); };
            service.SubmitRead(std::move(request));
        }
        ASSERT_TRUE(AwaitCount(cooks.Running, 2)) << "the texture decodes never started";

        const std::string materialContent = "material document";
        const auto materialPath = testDir / "frame_waits_on.material";
        WriteFile(materialPath, materialContent);
        DecodeSignal material;
        AssetIOService::ReadRequest request;
        request.AssetGuid = GUID::Generate();
        request.Metadata = MakeMetadata(materialPath, materialContent.size(), AssetType::Material);
        request.ProcessData = [decoded = material.Decoded](Vector<uint8>) { return SignalDecoded(*decoded); };
        service.SubmitRead(std::move(request));

        const bool materialDecoded = material.Done.wait_for(kResultTimeout) == std::future_status::ready;
        const int cooksHeldMeanwhile = cooks.Running.load();
        cooks.Release.set_value();

        EXPECT_TRUE(materialDecoded) << "the material decode queued behind " << cooksHeldMeanwhile
                                     << " texture decodes holding the gate";
        EXPECT_GT(cooksHeldMeanwhile, 0) << "the texture decodes must still hold while the material decodes";
        EXPECT_TRUE(AwaitCount(cooks.Finished, kTextureDecodes)) << "the held texture decodes never all ran";
        EXPECT_LE(cooks.Peak.load(), static_cast<int>(service.GetDecodeGate()->MaxTextureWorkers()))
            << "texture decodes took more of the gate than its texture share";

        service.Stop();
        pool.Shutdown();
    }
    std::filesystem::remove_all(testDir);
}

namespace
{
// Pool threads busy with one kind of asset work right now, and the most at once.
struct Occupancy
{
    void Enter()
    {
        const int now = Running.fetch_add(1) + 1;
        int peak = Peak.load();
        while (now > peak && !Peak.compare_exchange_weak(peak, now))
        {
        }
    }
    void Leave() { Running.fetch_sub(1); }

    std::atomic<int> Running{0};
    std::atomic<int> Peak{0};
};

// Texture decodes that run a large cook through the gate's TextureCookWorkers,
// every band holding until released, so the cooking threads and their helpers
// keep the texture share full. A decode counts as asset and texture work from
// start to end; a band counts only when a helper encodes it, since the cooking
// thread is its decode.
struct LargeCooks
{
    explicit LargeCooks(TextureCookWorkers& workers) : Workers(workers) {}

    TextureCookWorkers& Workers;
    Occupancy AssetWork;
    Occupancy TextureWork;
    std::promise<void> Release;
    std::shared_future<void> Released = Release.get_future().share();
    std::atomic<int> HelperBands{0};
    std::atomic<int> Failed{0};
    std::atomic<int> Finished{0};
};

bool EncodeHeldBand(LargeCooks& cooks, std::thread::id cookingThread)
{
    const bool onHelper = std::this_thread::get_id() != cookingThread;
    if (onHelper)
    {
        cooks.AssetWork.Enter();
        cooks.TextureWork.Enter();
        cooks.HelperBands.fetch_add(1);
    }
    cooks.Released.wait();
    if (onHelper)
    {
        cooks.TextureWork.Leave();
        cooks.AssetWork.Leave();
    }
    return true;
}

SharedPtr<Asset> CookLikeATextureDecode(LargeCooks& cooks)
{
    constexpr uint32 kBands = 32;
    cooks.AssetWork.Enter();
    cooks.TextureWork.Enter();
    const std::thread::id cookingThread = std::this_thread::get_id();
    if (!cooks.Workers.RunBands(kBands, [&cooks, cookingThread](uint32) { return EncodeHeldBand(cooks, cookingThread); },
                                {}))
        cooks.Failed.fetch_add(1);
    cooks.TextureWork.Leave();
    cooks.AssetWork.Leave();
    cooks.Finished.fetch_add(1);
    return SharedPtr<Asset>{};
}

SharedPtr<Asset> ShortDecode(Occupancy& assetWork, std::atomic<int>& decoded)
{
    assetWork.Enter();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assetWork.Leave();
    decoded.fetch_add(1);
    return SharedPtr<Asset>{};
}

void SubmitDecode(AssetIOService& service, const std::filesystem::path& path, AssetType type,
                  Function<SharedPtr<Asset>(Vector<uint8>)> decode)
{
    const std::string content = "asset bytes";
    WriteFile(path, content);
    AssetIOService::ReadRequest request;
    request.AssetGuid = GUID::Generate();
    request.Metadata = MakeMetadata(path, content.size(), type);
    request.ProcessData = std::move(decode);
    service.SubmitRead(std::move(request));
}
} // namespace

// Large cooks spread their bands over helpers while short decodes keep arriving.
// The helpers take their slots from the decode gate, as texture decodes do, so
// while the cooks hold the whole texture share: the pool workers busy with asset
// work never pass the gate (two stay free for the frame), texture work never
// passes its share, and every short decode still runs, in the slots no cook holds.
TEST(AssetIOServiceDecodeGate, CookHelpersShareTheGateWithDecodes)
{
    const auto testDir = TestUtils::MakeUniqueTempDirectory("asset_io_gate_helpers_test");
    std::filesystem::create_directories(testDir);
    {
        // Eight workers: a gate of six slots, four of them for texture work.
        constexpr size_t kWorkers = 8;
        JobSystem::WorkStealingThreadPool pool(kWorkers);
        AssetIOService service;
        service.Start(pool, 2);
        const std::shared_ptr<AssetDecodeGate> gate = service.GetDecodeGate();
        ASSERT_NE(gate, nullptr);
        TextureCookWorkers workers(pool, gate);
        const int gateSize = static_cast<int>(gate->MaxWorkers());
        const int textureShare = static_cast<int>(gate->MaxTextureWorkers());

        LargeCooks cooks(workers);
        constexpr int kCooks = 3;
        for (int i = 0; i < kCooks; ++i)
            SubmitDecode(service, testDir / ("albedo_" + std::to_string(i) + ".png"), AssetType::Texture,
                         [&cooks](Vector<uint8>) { return CookLikeATextureDecode(cooks); });
        // Every check below runs after the release: a held band left blocked would
        // hang the pool's shutdown instead of failing the test.
        const bool shareFilled = AwaitCount(cooks.TextureWork.Running, textureShare);

        constexpr int kShortDecodes = 24;
        std::atomic<int> decoded{0};
        for (int i = 0; i < kShortDecodes; ++i)
            SubmitDecode(service, testDir / ("short_" + std::to_string(i) + ".material"), AssetType::Material,
                         [&cooks, &decoded](Vector<uint8>) { return ShortDecode(cooks.AssetWork, decoded); });
        const bool shortDecodesRan = AwaitCount(decoded, kShortDecodes);
        const int textureWorkMeanwhile = cooks.TextureWork.Running.load();
        cooks.Release.set_value();

        EXPECT_TRUE(shareFilled) << "the cooks never filled the texture share";
        EXPECT_TRUE(shortDecodesRan) << decoded.load() << " of " << kShortDecodes
                                     << " short decodes ran while the cooks held the texture share";
        EXPECT_EQ(textureWorkMeanwhile, textureShare) << "the cooks must still hold while the short decodes run";
        EXPECT_GT(cooks.HelperBands.load(), 0) << "no cook spread a band to a helper";
        EXPECT_TRUE(AwaitCount(cooks.Finished, kCooks)) << "the cooks never all finished";
        EXPECT_EQ(cooks.Failed.load(), 0);
        EXPECT_LE(cooks.TextureWork.Peak.load(), textureShare) << "texture work passed the gate's texture share";
        EXPECT_LE(cooks.AssetWork.Peak.load(), gateSize)
            << "asset work took " << cooks.AssetWork.Peak.load() << " of " << kWorkers
            << " pool workers: fewer than two were left to the frame";

        service.Stop();
        pool.Shutdown();
    }
    std::filesystem::remove_all(testDir);
}
