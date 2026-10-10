#include <gtest/gtest.h>

#include "Logger/Logger.h"
#include "Logger/RingBufferSink.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace
{

using namespace Logger;
using namespace std::chrono_literals;

// A sink that parks inside Write() until it is released. Dispatched from
// Log::Critical, it reproduces the shape a crash handler has to survive: the
// CALLING thread holds Log's sink mutex for as long as the sink stays inside
// Write, so the drain thread cannot reach the sinks and cannot acknowledge a
// flush. A thread that faulted inside a sink leaves the process in exactly this
// state, minus the ability to ever release.
class BlockingSink : public LogSink
{
  public:
    void Write(const LogMessage&) override
    {
        if (m_OnWrite)
            m_OnWrite();
        m_InsideWrite.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Gate.wait(lock, [this] { return m_Released; });
    }

    void Flush() override {}
    bool ShouldLog(LogLevel) const override { return true; }
    String GetName() const override { return "BlockingSink"; }

    bool IsInsideWrite() const { return m_InsideWrite.load(std::memory_order_acquire); }

    void Release()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Released = true;
        }
        m_Gate.notify_all();
    }

    // Runs at the top of Write, i.e. on the thread that already holds Log's sink
    // mutex. Used to exercise a crash flush issued from that thread. Must be set
    // before the sink is handed to Log.
    void SetOnWrite(std::function<void()> callback) { m_OnWrite = std::move(callback); }

  private:
    std::mutex m_Mutex;
    std::condition_variable m_Gate;
    bool m_Released = false;
    std::atomic<bool> m_InsideWrite{false};
    std::function<void()> m_OnWrite;
};

// Releases its sink on destruction, so no assertion failure or exception can
// leave a worker parked in Write (which would hang the whole suite).
class SinkReleaser
{
  public:
    explicit SinkReleaser(BlockingSink& sink) : m_Sink(sink) {}
    ~SinkReleaser() { m_Sink.Release(); }

  private:
    BlockingSink& m_Sink;
};

class CrashFlushTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Log::Config cfg;
        cfg.GlobalMinLevel = LogLevel::Trace;
        cfg.EnableSourceLocation = false;
        Log::Initialize(cfg);
        Log::ClearSinks();
    }

    void TearDown() override { Log::Shutdown(); }

    // Adds a BlockingSink and parks a worker inside it via Log::Critical, which
    // dispatches synchronously while holding the sink mutex. Returns once the
    // sink reports it is inside Write.
    BlockingSink* WedgeSinkMutex(std::thread& worker)
    {
        auto owned = std::make_unique<BlockingSink>();
        BlockingSink* sink = owned.get();
        Log::AddSink(std::move(owned));

        worker = std::thread([] { Log::Critical("wedge the sink mutex"); });

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!sink->IsInsideWrite() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        return sink;
    }
};

// RED ARM. Log::Flush() is an unbounded barrier: while the sink mutex is held it
// does not return at all. Waiting is its contract, which is precisely why a
// death path must not call it. Performed on a worker so the wait is observed
// rather than suffered.
TEST_F(CrashFlushTest, UnboundedFlushDoesNotReturnWhileSinkMutexIsHeld)
{
    std::thread wedge;
    BlockingSink* sink = WedgeSinkMutex(wedge);
    ASSERT_TRUE(sink->IsInsideWrite());

    std::future<void> flushed = std::async(std::launch::async, [] { Log::Flush(); });
    const std::future_status parked = flushed.wait_for(1s);

    sink->Release();
    flushed.wait();
    wedge.join();

    EXPECT_EQ(parked, std::future_status::timeout);
}

// GREEN ARM, same condition: the bounded flush returns, and reports that the
// flush did not complete.
TEST_F(CrashFlushTest, FlushForCrashReturnsWhileSinkMutexIsHeld)
{
    std::thread wedge;
    BlockingSink* sink = WedgeSinkMutex(wedge);
    ASSERT_TRUE(sink->IsInsideWrite());
    SinkReleaser releaser(*sink);

    constexpr auto kTimeout = 200ms;
    const auto start = std::chrono::steady_clock::now();
    const bool acknowledged = Log::FlushForCrash(kTimeout);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(acknowledged);
    EXPECT_GE(elapsed, kTimeout);
    EXPECT_LT(elapsed, 5s);

    sink->Release();
    wedge.join();
}

// The crashing thread is often the one holding the sink mutex. Log's mutexes are
// non-recursive, so any path that blocks on them from that thread self-deadlocks.
TEST_F(CrashFlushTest, FlushForCrashReturnsOnTheThreadHoldingTheSinkMutex)
{
    auto owned = std::make_unique<BlockingSink>();
    BlockingSink* sink = owned.get();

    std::atomic<bool> callbackRan{false};
    std::atomic<bool> acknowledged{true};
    sink->SetOnWrite([&]
    {
        acknowledged.store(Log::FlushForCrash(100ms));
        callbackRan.store(true);
    });
    Log::AddSink(std::move(owned));

    // Critical dispatches inline under the sink mutex, so the callback runs on a
    // thread that already owns it.
    std::thread wedge([] { Log::Critical("crash flush from inside a sink"); });

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!sink->IsInsideWrite() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    sink->Release();
    wedge.join();

    EXPECT_TRUE(callbackRan.load());
    EXPECT_FALSE(acknowledged.load());
}

// Nothing blocked: the bounded flush must still be a real barrier.
TEST_F(CrashFlushTest, FlushForCrashDeliversWhenNothingIsBlocked)
{
    auto owned = std::make_unique<RingBufferSink>(1000);
    RingBufferSink* spy = owned.get();
    Log::AddSink(std::move(owned));

    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i)
        Log::Info("crash-flush delivery {}", i);

    EXPECT_TRUE(Log::FlushForCrash(5s));
    EXPECT_EQ(spy->GetCount(), static_cast<size_t>(kCount));
}

// Called FROM the drain thread — i.e. a fault inside a sink — nothing is or can be
// flushed, so it must report failure. Reporting success there suppresses the caller's
// tail-missing warning in precisely the case where the tail IS lost.
TEST_F(CrashFlushTest, FlushForCrashReportsFailureWhenIssuedFromTheDrainThread)
{
    std::atomic<bool> ran{false};
    std::atomic<bool> acknowledged{true};

    auto owned = std::make_unique<BlockingSink>();
    BlockingSink* sink = owned.get();
    // Runs inside Write on whichever thread dispatched it. Log::Info is delivered by
    // the drain thread, so this observes the drain-thread branch specifically.
    sink->SetOnWrite([&] {
        acknowledged.store(Log::FlushForCrash(100ms));
        ran.store(true);
    });
    SinkReleaser releaser(*sink);
    Log::AddSink(std::move(owned));

    Log::Info("crash flush from the drain thread");

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!ran.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);

    ASSERT_TRUE(ran.load()) << "the sink never ran; the branch under test was not reached";
    EXPECT_FALSE(acknowledged.load());
}

// With no drain thread the flush is synchronous, and must still report success.
TEST_F(CrashFlushTest, FlushForCrashSucceedsAfterShutdown)
{
    auto owned = std::make_unique<RingBufferSink>(1000);
    Log::AddSink(std::move(owned));
    Log::Info("before shutdown");

    Log::Shutdown();
    EXPECT_TRUE(Log::FlushForCrash(1s));

    Log::Config cfg;
    cfg.GlobalMinLevel = LogLevel::Trace;
    Log::Initialize(cfg);
}

} // namespace
