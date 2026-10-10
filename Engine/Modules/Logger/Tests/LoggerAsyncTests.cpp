#include <gtest/gtest.h>

#include "Logger/Logger.h"
#include "Logger/CallbackSink.h"
#include "Logger/FileSink.h"
#include "Logger/RingBufferSink.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace Logger;

// Fixture: initializes Logger with a RingBufferSink, shuts down in TearDown.
class LoggerAsyncTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Log::Config cfg;
        cfg.GlobalMinLevel = LogLevel::Trace;
        cfg.EnableSourceLocation = false;
        Log::Initialize(cfg);

        Log::ClearSinks();

        auto sink = std::make_unique<RingBufferSink>(50000);
        m_Spy = sink.get();
        Log::AddSink(std::move(sink));
    }

    void TearDown() override
    {
        Log::Shutdown();
        m_Spy = nullptr;
    }

    RingBufferSink* m_Spy = nullptr;
};

// ==========================================================================
// A) CORRECTNESS
// ==========================================================================

TEST_F(LoggerAsyncTest, OwnedSourceIsCopiedBeforeItsOwnerDies)
{
    auto captured = std::make_shared<std::vector<LogMessage>>();
    auto sink = std::make_unique<CallbackSink>();
    sink->RegisterCallback([captured](const LogMessage& message) { captured->push_back(message); });
    Log::AddSink(std::move(sink));
    {
        OwnedSourceLocation source("owned.cpp", "ScopedFunction");
        Log::LogWithSource(LogLevel::Info, source, 42, "owned {}", 7);
        Log::LogWithSource(LogLevel::Info, source, 43, std::string_view("plain"));
        Log::LogWithSource(LogLevel::Critical, source, 44, std::string_view("critical"));
    }
    Log::Flush();
    const auto& messages = *captured;
    ASSERT_EQ(messages.size(), 3u);
    for (const auto& message : messages)
    {
        EXPECT_EQ(message.SourceFile, "owned.cpp");
        EXPECT_EQ(message.Function, "ScopedFunction");
    }
}

#if LOGGER_ENABLE_SOURCE_LOCATION && defined(_DEBUG)
TEST_F(LoggerAsyncTest, SourceMacrosKeepCallerFunctionAndLine)
{
    auto captured = std::make_shared<std::vector<LogMessage>>();
    auto sink = std::make_unique<CallbackSink>();
    sink->RegisterCallback([captured](const LogMessage& message) { captured->push_back(message); });
    Log::AddSink(std::move(sink));
    const int expectedLine = __LINE__ + 1;
    LOG_INFO("macro {}", 3);
    Log::Flush();
    const auto& messages = *captured;
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].SourceFile, __FILE__);
    EXPECT_EQ(messages[0].Function, __FUNCTION__);
    EXPECT_EQ(messages[0].SourceLine, expectedLine);
}

TEST_F(LoggerAsyncTest, FilteredSourceMacroDoesNotEvaluateArguments)
{
    Log::SetLogLevel(LogLevel::Critical);
    int evaluations = 0;
    LOG_INFO("filtered {}", ++evaluations);
    EXPECT_EQ(evaluations, 0);
}
#endif

TEST_F(LoggerAsyncTest, BasicMessageDelivery)
{
    constexpr int kCount = 50;
    for (int i = 0; i < kCount; ++i)
    {
        Log::Info("message {}", i);
    }
    Log::Flush();
    EXPECT_EQ(m_Spy->GetCount(), static_cast<size_t>(kCount));
}

TEST_F(LoggerAsyncTest, MessageOrdering_SingleThread)
{
    Log::Info("alpha");
    Log::Info("bravo");
    Log::Info("charlie");
    Log::Flush();

    auto msgs = m_Spy->GetMessages();
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_NE(msgs[0].Message.find("alpha"), std::string::npos);
    EXPECT_NE(msgs[1].Message.find("bravo"), std::string::npos);
    EXPECT_NE(msgs[2].Message.find("charlie"), std::string::npos);
}

TEST_F(LoggerAsyncTest, FlushBlocksUntilDelivered)
{
    for (int i = 0; i < 200; ++i)
    {
        Log::Info("msg {}", i);
    }
    Log::Flush();
    EXPECT_EQ(m_Spy->GetCount(), 200u);
}

TEST_F(LoggerAsyncTest, ShutdownDrainsRemaining)
{
    // Use an atomic counter via CallbackSink to survive Shutdown (which
    // destroys sinks, making m_Spy a dangling pointer).
    std::atomic<int> messageCount{0};
    auto callbackSink = std::make_unique<CallbackSink>();
    callbackSink->RegisterCallback([&messageCount](const LogMessage&)
    {
        messageCount.fetch_add(1, std::memory_order_relaxed);
    });
    Log::AddSink(std::move(callbackSink));

    for (int i = 0; i < 100; ++i)
    {
        Log::Info("drain-check {}", i);
    }
    // No Flush -- Shutdown must drain.
    Log::Shutdown();

    // Counter lives on the stack, safe to access after Shutdown.
    EXPECT_EQ(messageCount.load(), 100);

    // Re-initialize so TearDown doesn't double-shutdown.
    Log::Config cfg;
    cfg.GlobalMinLevel = LogLevel::Trace;
    Log::Initialize(cfg);
}

#if LOGGER_ENABLE_FILE_LOGGING
// A burst's trailing lines must become visible WITHOUT an explicit Flush():
// FileSink defaults to autoFlush=false, so before the drain thread's bounded
// idle flush they could sit in the stream buffer until unrelated later traffic
// happened to fill it (observed as build-completion lines arriving minutes
// late, masquerading as hangs).
TEST_F(LoggerAsyncTest, BurstTrailingLinesFlushWithoutExplicitFlush)
{
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "ge_logger_burst_flush.txt";
    std::error_code ec;
    fs::remove(path, ec);

    FileSink::Config cfg;
    cfg.filename = path.string().c_str();
    cfg.append = false;
    ASSERT_FALSE(cfg.autoFlush) << "test assumes the buffered default";
    Log::AddSink(std::make_unique<FileSink>(cfg));

    // Small burst (well under the stream buffer size) ending in a sentinel, so
    // nothing self-flushes by filling the buffer.
    for (int i = 0; i < 5; ++i)
    {
        Log::Info("burst line {}", i);
    }
    Log::Info("BURST-SENTINEL-DONE");

    // Deliberately NO Log::Flush() — the drain's bounded idle flush must
    // surface the sentinel on its own. Generous ceiling for CI scheduling; the
    // configured bound is kMaxUnflushedSinkLatency (200ms).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool visible = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::ifstream in(path);
        const std::string content((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        if (content.find("BURST-SENTINEL-DONE") != std::string::npos)
        {
            visible = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    EXPECT_TRUE(visible) << "unflushed sink output was not surfaced within the bounded latency";

    // Release the file handle before removing the temp file.
    Log::ClearSinks();
    fs::remove(path, ec);
}

// Log::Flush() must be a barrier all the way to the file, not just to the sink:
// the crash handlers call it and then abort, so there is no bounded idle flush
// left to run and no sink destructor left to close the stream.
TEST_F(LoggerAsyncTest, ExplicitFlushReachesTheFileImmediately)
{
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "ge_logger_explicit_flush.txt";
    std::error_code ec;
    fs::remove(path, ec);

    FileSink::Config cfg;
    cfg.filename = path.string().c_str();
    cfg.append = false;
    ASSERT_FALSE(cfg.autoFlush) << "test assumes the buffered default";
    Log::AddSink(std::make_unique<FileSink>(cfg));

    for (int i = 0; i < 5; ++i)
    {
        Log::Info("pre-flush line {}", i);
    }
    Log::Info("EXPLICIT-FLUSH-SENTINEL");

    Log::Flush();

    // Read straight back: no polling and no sleep, or the bounded idle flush
    // would satisfy the assertion on its own and the barrier would go untested.
    std::ifstream in(path);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("EXPLICIT-FLUSH-SENTINEL"), std::string::npos)
        << "Flush() returned with the sink's tail still buffered";
    EXPECT_NE(content.find("pre-flush line 0"), std::string::npos);

    Log::ClearSinks();
    fs::remove(path, ec);
}
#endif // LOGGER_ENABLE_FILE_LOGGING

TEST(LoggerAsyncPreInitTest, LogBeforeInitialize_NoCrash)
{
    // Logger is not initialized. Should not crash; output goes to stdout.
    Log::Info("pre-init message");
    Log::Warning("another pre-init message");
}

TEST_F(LoggerAsyncTest, ReinitAfterShutdown)
{
    Log::Info("before shutdown");
    Log::Shutdown();

    Log::Config cfg;
    cfg.GlobalMinLevel = LogLevel::Trace;
    Log::Initialize(cfg);

    Log::ClearSinks();
    auto sink = std::make_unique<RingBufferSink>(10000);
    auto* spy2 = sink.get();
    Log::AddSink(std::move(sink));

    Log::Info("after re-init");
    Log::Flush();

    EXPECT_GE(spy2->GetCount(), 1u);
    auto msgs = spy2->GetMessages(LogLevel::Trace, 0, "after re-init");
    EXPECT_EQ(msgs.size(), 1u);
}

TEST_F(LoggerAsyncTest, CriticalBypassesQueue)
{
    // Critical is dispatched synchronously -- visible without Flush.
    Log::Critical("immediate");

    EXPECT_GE(m_Spy->GetCount(), 1u);
    auto msgs = m_Spy->GetMessages(LogLevel::Critical, 0, "immediate");
    EXPECT_EQ(msgs.size(), 1u);
}

TEST_F(LoggerAsyncTest, LevelFiltering)
{
    Log::SetLogLevel(LogLevel::Warning);

    Log::Trace("filtered");
    Log::Debug("filtered");
    Log::Info("filtered");
    Log::Warning("visible-warning");
    Log::Error("visible-error");
    Log::Critical("visible-critical");
    Log::Flush();

    EXPECT_EQ(m_Spy->GetCount(), 3u);
    auto msgs = m_Spy->GetMessages();
    for (const auto& m : msgs)
    {
        EXPECT_GE(m.Level, LogLevel::Warning);
    }
}

TEST_F(LoggerAsyncTest, FormatStringWithMultipleArgs)
{
    Log::Info("values: {} {} {} {} {}", 1, 2.5, "hello", 'X', true);
    Log::Flush();

    ASSERT_EQ(m_Spy->GetCount(), 1u);
    auto msgs = m_Spy->GetMessages();
    EXPECT_NE(msgs[0].Message.find("values:"), std::string::npos);
    EXPECT_NE(msgs[0].Message.find("hello"), std::string::npos);
}

TEST_F(LoggerAsyncTest, AllLogLevelsDelivered)
{
    Log::Trace("t");
    Log::Debug("d");
    Log::Info("i");
    Log::Warning("w");
    Log::Error("e");
    Log::Critical("c");
    Log::Flush();

    EXPECT_EQ(m_Spy->GetCount(), 6u);
}

TEST_F(LoggerAsyncTest, EmptyMessage_NoCrash)
{
    Log::Info("");
    Log::Flush();
    EXPECT_EQ(m_Spy->GetCount(), 1u);
}

TEST_F(LoggerAsyncTest, FlushOnEmptyQueue_NoCrash)
{
    Log::Flush();
    EXPECT_EQ(m_Spy->GetCount(), 0u);
}

// ==========================================================================
// B) CONCURRENCY / CONTENTION
// ==========================================================================

TEST_F(LoggerAsyncTest, MultiThreadStress)
{
    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([t]()
        {
            for (int i = 0; i < kPerThread; ++i)
            {
                Log::Info("thread {} msg {}", t, i);
            }
        });
    }
    for (auto& th : threads) th.join();

    Log::Flush();
    EXPECT_EQ(m_Spy->GetCount(), static_cast<size_t>(kThreads * kPerThread));
}

TEST_F(LoggerAsyncTest, ConcurrentFlush_NoDeadlock)
{
    constexpr int kFlushThreads = 4;
    constexpr int kLogCount = 500;

    std::thread producer([]()
    {
        for (int i = 0; i < kLogCount; ++i)
        {
            Log::Info("concurrent-flush msg {}", i);
        }
    });

    std::vector<std::thread> flushers;
    flushers.reserve(kFlushThreads);
    for (int t = 0; t < kFlushThreads; ++t)
    {
        flushers.emplace_back([]()
        {
            for (int i = 0; i < 10; ++i)
            {
                Log::Flush();
            }
        });
    }

    producer.join();
    for (auto& th : flushers) th.join();

    Log::Flush();
    EXPECT_EQ(m_Spy->GetCount(), static_cast<size_t>(kLogCount));
}

TEST_F(LoggerAsyncTest, LogDuringShutdown_NoCrash)
{
    std::atomic<bool> go{false};

    std::thread logger([&go]()
    {
        while (!go.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 500; ++i)
        {
            Log::Info("during-shutdown {}", i);
        }
    });

    go.store(true, std::memory_order_release);
    Log::Shutdown();
    logger.join();

    // Re-initialize so TearDown works.
    Log::Config cfg;
    cfg.GlobalMinLevel = LogLevel::Trace;
    Log::Initialize(cfg);
}

TEST_F(LoggerAsyncTest, FlushDuringShutdown_NoHang)
{
    std::atomic<bool> go{false};

    std::thread flusher([&go]()
    {
        while (!go.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 20; ++i)
        {
            Log::Flush();
        }
    });

    for (int i = 0; i < 100; ++i)
    {
        Log::Info("pre-shutdown {}", i);
    }

    go.store(true, std::memory_order_release);
    Log::Shutdown();
    flusher.join();

    Log::Config cfg;
    cfg.GlobalMinLevel = LogLevel::Trace;
    Log::Initialize(cfg);
}

// ==========================================================================
// C) RE-ENTRANCY
// ==========================================================================

TEST_F(LoggerAsyncTest, CallbackSinkThatLogs_NoDeadlock)
{
    auto callbackSink = std::make_unique<CallbackSink>();
    // Heap-owned, captured by value: the sink outlives the test body (TearDown's
    // Shutdown still dispatches to it during the final drain), so the callback
    // must not reference this frame's stack.
    auto reentrantCount = std::make_shared<std::atomic<int>>(0);

    callbackSink->RegisterCallback([reentrantCount](const LogMessage&)
    {
        reentrantCount->fetch_add(1, std::memory_order_relaxed);
        Log::Info("re-entrant message");
    });
    Log::AddSink(std::move(callbackSink));

    Log::Info("trigger");
    Log::Flush();

    EXPECT_GE(reentrantCount->load(), 1);

    // Flush again to drain re-entrant messages.
    Log::Flush();
    EXPECT_GE(m_Spy->GetCount(), 2u);
}

TEST_F(LoggerAsyncTest, CallbackSinkThatFlushes_NoDeadlock)
{
    auto callbackSink = std::make_unique<CallbackSink>();
    // Heap-owned, captured by value: same lifetime hazard as
    // CallbackSinkThatLogs_NoDeadlock above.
    auto flushCallCount = std::make_shared<std::atomic<int>>(0);

    callbackSink->RegisterCallback([flushCallCount](const LogMessage&)
    {
        flushCallCount->fetch_add(1, std::memory_order_relaxed);
        // Re-entrancy guard should short-circuit this without deadlock.
        Log::Flush();
    });
    Log::AddSink(std::move(callbackSink));

    Log::Info("trigger flush re-entrancy");
    Log::Flush();

    EXPECT_GE(flushCallCount->load(), 1);
    EXPECT_GE(m_Spy->GetCount(), 1u);
}

// ==========================================================================
// D) PERFORMANCE REGRESSION
// ==========================================================================

TEST_F(LoggerAsyncTest, ThroughputRegression)
{
    constexpr int kCount = 10000;

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kCount; ++i)
    {
        Log::Info("perf arg1={} arg2={} arg3={} arg4={} arg5={}", i, i * 2, "str", 3.14, true);
    }
    auto enqueueEnd = std::chrono::steady_clock::now();

    Log::Flush();
    auto flushEnd = std::chrono::steady_clock::now();

    auto enqueueUs = std::chrono::duration_cast<std::chrono::microseconds>(enqueueEnd - start).count();
    auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(flushEnd - start).count();

    std::printf("[PERF] Enqueue %d formatted msgs: %lld us (%.2f us/msg), total with Flush: %lld us\n",
                kCount, static_cast<long long>(enqueueUs),
                static_cast<double>(enqueueUs) / kCount,
                static_cast<long long>(totalUs));

    EXPECT_EQ(m_Spy->GetCount(), static_cast<size_t>(kCount));
}

TEST_F(LoggerAsyncTest, MultiThreadThroughputRegression)
{
    constexpr int kThreads = 4;
    constexpr int kPerThread = 5000;

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([t]()
        {
            for (int i = 0; i < kPerThread; ++i)
            {
                Log::Info("mt-perf t={} i={}", t, i);
            }
        });
    }
    for (auto& th : threads) th.join();

    auto enqueueEnd = std::chrono::steady_clock::now();
    Log::Flush();
    auto flushEnd = std::chrono::steady_clock::now();

    auto enqueueUs = std::chrono::duration_cast<std::chrono::microseconds>(enqueueEnd - start).count();
    auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(flushEnd - start).count();

    std::printf("[PERF] %d threads x %d msgs: enqueue %lld us, total %lld us\n",
                kThreads, kPerThread,
                static_cast<long long>(enqueueUs),
                static_cast<long long>(totalUs));

    EXPECT_EQ(m_Spy->GetCount(), static_cast<size_t>(kThreads * kPerThread));
}

} // anonymous namespace
