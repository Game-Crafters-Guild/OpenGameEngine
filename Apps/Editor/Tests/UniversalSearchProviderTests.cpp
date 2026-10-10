// The editor's global search palette must answer queries and build its project
// code index without stalling the UI thread. UniversalSearchProvider runs both
// as JobSystem jobs, and its destructor waits for any job still running.

#include "UI/UniversalSearchProvider.h"

#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <algorithm>
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
#include <utility>
#include <vector>

using GameEngine::SearchResultItem;
using GameEngine::UniversalSearchEntry;
using GameEngine::UniversalSearchKind;
using GameEngine::UniversalSearchProvider;

namespace
{

constexpr auto kSearchTimeout = std::chrono::seconds(10);
constexpr auto kCodeIndexPollInterval = std::chrono::milliseconds(20);
// Time the destroying thread gets to reach its wait while the drain job is
// parked. A shorter head start can only hide a teardown-order defect; it
// cannot fail a correct destructor.
constexpr auto kDestroyerHeadStart = std::chrono::milliseconds(100);

// The provider's destructor waits for its jobs, so the pool must outlive it:
// member order here is the lifetime contract.
struct ProviderHarness
{
    JobSystem::WorkStealingThreadPool Pool{2};
    UniversalSearchProvider Provider{Pool};
};

// Collects the labels a search delivers; the sink runs on a pool thread.
struct SearchOutcome
{
    std::mutex Mutex;
    std::condition_variable Cv;
    std::vector<std::string> Labels;
    bool Complete = false;
};

std::vector<std::string> RunSearch(UniversalSearchProvider& provider, const std::string& query)
{
    auto outcome = std::make_shared<SearchOutcome>();
    provider.CancelSearch();
    provider.BeginSearch(query, [outcome](std::vector<SearchResultItem> batch, bool isComplete)
    {
        std::lock_guard lock(outcome->Mutex);
        for (const SearchResultItem& item : batch)
            outcome->Labels.push_back(item.Label);
        outcome->Complete = outcome->Complete || isComplete;
        outcome->Cv.notify_all();
    });
    std::unique_lock lock(outcome->Mutex);
    EXPECT_TRUE(outcome->Cv.wait_for(lock, kSearchTimeout, [&]() { return outcome->Complete; }))
        << "search for '" << query << "' never completed";
    return outcome->Labels;
}

bool Contains(const std::vector<std::string>& labels, const std::string& label)
{
    return std::find(labels.begin(), labels.end(), label) != labels.end();
}

std::filesystem::path MakeTempRoot(const char* label)
{
    static std::atomic<uint32_t> counter{0};
    const auto threadHash = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("ge-universal-search-" + std::string(label) + "-" + std::to_string(threadHash) + "-" +
         std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

UniversalSearchEntry MakeCommand(const std::string& label)
{
    UniversalSearchEntry entry;
    entry.Kind = UniversalSearchKind::Command;
    entry.Label = label;
    entry.Provider = "Test";
    return entry;
}

// Parks the lexical drain job the first time it copies a gated action off the
// test thread, until Release(). Copies on the test thread pass through.
struct DrainJobGate
{
    std::thread::id TestThread = std::this_thread::get_id();
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Parked = false;
    bool Released = false;

    void ParkOnceOffTestThread()
    {
        if (std::this_thread::get_id() == TestThread)
            return;
        std::unique_lock lock(Mutex);
        if (Parked)
            return;
        Parked = true;
        Cv.notify_all();
        Cv.wait(lock, [this]() { return Released; });
    }

    bool WaitUntilParked()
    {
        std::unique_lock lock(Mutex);
        return Cv.wait_for(lock, kSearchTimeout, [this]() { return Parked; });
    }

    void Release()
    {
        std::lock_guard lock(Mutex);
        Released = true;
        Cv.notify_all();
    }
};

// An entry action whose copy runs the gate. The drain job copies each matching
// entry's action into its result after the query-cache lookup and before the
// cache store, so the gate parks it between two reads of the provider's
// lexical state.
struct GatedAction
{
    explicit GatedAction(std::shared_ptr<DrainJobGate> gate)
        : Gate(std::move(gate))
    {
    }

    GatedAction(const GatedAction& other)
        : Gate(other.Gate)
    {
        Gate->ParkOnceOffTestThread();
    }

    void operator()() const {}

    std::shared_ptr<DrainJobGate> Gate;
};

} // namespace

TEST(UniversalSearchProvider, LexicalSearchDeliversMatches)
{
    ProviderHarness harness;
    harness.Provider.AddEntry(MakeCommand("Frobnicate Widgets"));
    harness.Provider.AddEntry(MakeCommand("Open Settings"));

    const std::vector<std::string> labels = RunSearch(harness.Provider, "frobnicate");

    EXPECT_TRUE(Contains(labels, "Frobnicate Widgets"));
    EXPECT_FALSE(Contains(labels, "Open Settings"));
}

TEST(UniversalSearchProvider, CodeIndexJobPublishesProjectFiles)
{
    const std::filesystem::path root = MakeTempRoot("code-index");
    {
        std::ofstream source(root / "WidgetFactory.cpp");
        source << "int MakeWidget() { return 1; }\n";
    }

    {
        ProviderHarness harness;
        harness.Provider.StartCodeIndex(root);

        bool found = false;
        const auto deadline = std::chrono::steady_clock::now() + kSearchTimeout;
        while (!found && std::chrono::steady_clock::now() < deadline)
        {
            harness.Provider.RefreshRuntimeEntries();
            found = Contains(RunSearch(harness.Provider, "file:WidgetFactory"), "WidgetFactory.cpp");
            if (!found)
                std::this_thread::sleep_for(kCodeIndexPollInterval);
        }
        EXPECT_TRUE(found) << "the code index job never published WidgetFactory.cpp";
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(UniversalSearchProvider, DestructionWaitsForQueuedSearches)
{
    std::atomic<int> deliveries{0};
    {
        ProviderHarness harness;
        for (int i = 0; i < 64; ++i)
            harness.Provider.AddEntry(MakeCommand("Command " + std::to_string(i)));
        for (int i = 0; i < 32; ++i)
        {
            harness.Provider.CancelSearch();
            harness.Provider.BeginSearch("command", [&deliveries](std::vector<SearchResultItem>, bool)
            {
                deliveries.fetch_add(1, std::memory_order_relaxed);
            });
        }
    }
    // Reaching here without a crash is the contract: the drain job finished
    // before the provider's state was destroyed. Latest-wins coalescing may
    // drop superseded requests, so only an upper bound is fixed.
    EXPECT_LE(deliveries.load(), 32);
}

TEST(UniversalSearchProvider, DestructionWaitsForTheRunningSearchBeforeTeardown)
{
    auto gate = std::make_shared<DrainJobGate>();
    JobSystem::WorkStealingThreadPool pool{2};
    auto provider = std::make_unique<UniversalSearchProvider>(pool);
    UniversalSearchEntry entry = MakeCommand("Gated Command");
    entry.Execute = GatedAction(gate);
    provider->AddEntry(std::move(entry));
    provider->BeginSearch("gated", [](std::vector<SearchResultItem>, bool) {});
    EXPECT_TRUE(gate->WaitUntilParked()) << "the drain job never copied the gated action";

    std::atomic<bool> destroyed{false};
    std::thread destroyer([&provider, &destroyed]()
    {
        provider.reset();
        destroyed.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(kDestroyerHeadStart);
    EXPECT_FALSE(destroyed.load(std::memory_order_acquire))
        << "the destructor returned while the drain job was still running";

    // The parked job resumes into the rest of its search; it must find the
    // provider's lexical state alive.
    gate->Release();
    destroyer.join();
    EXPECT_TRUE(destroyed.load(std::memory_order_acquire));
}
