// The inline reload path (AssetManager::CheckForReloads -> Asset::Reload, used
// by every type that does not decode off-thread). An empty read is a save in
// progress, so it must reach consumers as nothing at all; a rejected decode is
// a real failure and must still reach them as AssetLoadFailed.

#include <gtest/gtest.h>

#include "AssetCore/AssetEvents.h"
#include "AssetCore/AssetReloadInvalidator.h"
#include "Assets/AssetManager.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr const char* kTestExtension = ".inlinereload";

// The content that makes a decode refuse, so a test can stage a real failure
// against the same fixture that stages a deferral.
constexpr const char* kRejectedContent = "REJECT";

// Payload is the file's text, decoded in place on the main thread: the shape
// of every asset type that does not support async reload.
class InlineReloadTestAsset final : public Asset
{
  public:
    InlineReloadTestAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::XML, path)
    {
    }

    bool Load() override
    {
        std::ifstream in(GetPath(), std::ios::binary);
        if (!in.is_open())
            return false;
        return Apply(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
    }

    bool LoadFromData(const Vector<uint8>& data) override
    {
        return Apply(std::string(data.begin(), data.end()));
    }

    void Unload() override
    {
        m_Content.clear();
        SetState(AssetState::Unloaded);
    }

    std::string Content() const { return m_Content; }

  private:
    bool Apply(std::string text)
    {
        if (text == kRejectedContent)
        {
            SetState(AssetState::Failed);
            return false;
        }
        m_Content = std::move(text);
        SetState(AssetState::Loaded);
        return true;
    }

    std::string m_Content;
};

void WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << path.string();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

template <typename Predicate>
bool WaitUntil(Predicate&& predicate, int timeoutMs = 10000, int pollMs = 20)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
    }
    return predicate();
}

// The mtime recorded at Load() must be strictly older than the next write's
// for the change to be noticed at all.
void SleepPastMtimeGranularity()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
}

// One manager + pool + registered inline-reload type per test, in its own dir.
class InlineReloadFixture
{
  public:
    explicit InlineReloadFixture(const char* tag)
    {
        Root = TestUtils::MakeUniqueTempDirectory(tag);
        std::error_code ec;
        std::filesystem::remove_all(Root, ec);
        std::filesystem::create_directories(Root / "Assets", ec);

        Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        Manager = std::make_unique<AssetManager>();
        Initialized = Manager->Initialize(Root / "Assets", Pool.get(), Root / "AssetDatabase.assetdb",
                                          Root / ".Cache" / "AssetDatabase");
        if (!Initialized)
            return;
        Manager->SetHotReloadEnabled(true);

        AssetTypeRegistration registration(
            AssetType::XML, {kTestExtension},
            [](const AssetMetadata& metadata) -> SharedPtr<Asset>
            {
                return std::static_pointer_cast<Asset>(
                    std::make_shared<InlineReloadTestAsset>(metadata.Guid, metadata.Path));
            },
            "InlineReloadTest", 100);
        Registered = Manager->GetAssetTypeRegistry().RegisterAssetType(registration);
    }

    ~InlineReloadFixture()
    {
        if (Manager)
            Manager->Shutdown();
        Manager.reset();
        Pool.reset();
        std::error_code ec;
        std::filesystem::remove_all(Root, ec);
    }

    std::shared_ptr<InlineReloadTestAsset> AddLoadedAsset(const std::filesystem::path& relPath)
    {
        const GUID guid = Manager->ResolveAssetGuid(relPath);
        if (guid.IsNull())
            return nullptr;
        auto asset = std::make_shared<InlineReloadTestAsset>(guid, Root / "Assets" / relPath);
        if (!asset->Load())
            return nullptr;
        Manager->RegisterLoadedAsset(guid, asset);
        return asset;
    }

    std::filesystem::path Root;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> Pool;
    std::unique_ptr<AssetManager> Manager;
    bool Initialized = false;
    bool Registered = false;
};

} // namespace

// A truncate-then-write save is seen as an empty file first. Reloading from it
// would replace a live payload with nothing, so the reload defers — and a
// deferral is not a failure, so no consumer may be told the asset failed.
TEST(InlineReloadDeferral, EmptyReadKeepsThePayloadAndRaisesNoLoadFailure)
{
    InlineReloadFixture fx("ge_inline_reload_deferral");
    ASSERT_TRUE(fx.Initialized);
    ASSERT_TRUE(fx.Registered);

    const std::filesystem::path relPath = std::string("deferral") + kTestExtension;
    WriteTextFile(fx.Root / "Assets" / relPath, "v1");

    auto asset = fx.AddLoadedAsset(relPath);
    ASSERT_NE(asset, nullptr);
    ASSERT_EQ(asset->Content(), "v1");
    const GUID guid = asset->GetGUID();

    std::atomic<int> loadFailedEvents{0};
    const uint32 callbackHandle = fx.Manager->GetEventDispatcher().AddCallback(
        [&loadFailedEvents, guid](const AssetEvent& event)
        {
            if (event.EventType == AssetEventType::AssetLoadFailed && event.AssetGuid == guid)
                loadFailedEvents.fetch_add(1);
        });

    SleepPastMtimeGranularity();
    WriteTextFile(fx.Root / "Assets" / relPath, "");

    // Reload() stamps the timestamp it read even when it defers, so the change
    // being consumed is what NeedsReload() going quiet proves. Without this the
    // "no event" assertion below could pass on a reload that never ran.
    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return !asset->NeedsReload();
        }))
        << "the empty write was never offered to the asset, so this test proves nothing";

    EXPECT_EQ(asset->Content(), "v1") << "an empty read must not replace the live payload";
    EXPECT_TRUE(asset->IsLoaded());
    EXPECT_EQ(loadFailedEvents.load(), 0) << "a deferred reload is not a failure";

    // The writing half of the save is a change of its own and still lands.
    SleepPastMtimeGranularity();
    WriteTextFile(fx.Root / "Assets" / relPath, "v2");
    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return asset->Content() == "v2";
        }))
        << "the completed save never reloaded";
    EXPECT_EQ(loadFailedEvents.load(), 0);

    fx.Manager->GetEventDispatcher().RemoveCallback(callbackHandle);
}

// The control for the test above: bytes the type refuses really are a failure,
// and that one still reaches consumers.
TEST(InlineReloadDeferral, RejectedBytesStillRaiseLoadFailed)
{
    InlineReloadFixture fx("ge_inline_reload_reject");
    ASSERT_TRUE(fx.Initialized);
    ASSERT_TRUE(fx.Registered);

    const std::filesystem::path relPath = std::string("reject") + kTestExtension;
    WriteTextFile(fx.Root / "Assets" / relPath, "v1");

    auto asset = fx.AddLoadedAsset(relPath);
    ASSERT_NE(asset, nullptr);
    const GUID guid = asset->GetGUID();

    std::atomic<int> loadFailedEvents{0};
    const uint32 callbackHandle = fx.Manager->GetEventDispatcher().AddCallback(
        [&loadFailedEvents, guid](const AssetEvent& event)
        {
            if (event.EventType == AssetEventType::AssetLoadFailed && event.AssetGuid == guid)
                loadFailedEvents.fetch_add(1);
        });

    SleepPastMtimeGranularity();
    WriteTextFile(fx.Root / "Assets" / relPath, kRejectedContent);

    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return loadFailedEvents.load() >= 1;
        }))
        << "a rejected decode must still be reported as a failed load";

    fx.Manager->GetEventDispatcher().RemoveCallback(callbackHandle);
}

// A consumer may hold an asset under a GUID the project redirects to it (a scene authored before
// the file's GUID was re-minted): GetAsset resolves the redirect, and the reload event names the
// asset by the GUID it resolved to. An invalidator must reach the redirect source as well, or a
// cache filed under it serves its first copy for the rest of the session.
TEST(InlineReloadDeferral, AReloadInvalidatesTheGuidsThatRedirectToTheAsset)
{
    InlineReloadFixture fx("ge_inline_reload_redirect");
    ASSERT_TRUE(fx.Initialized);
    ASSERT_TRUE(fx.Registered);

    const std::filesystem::path relPath = std::string("redirected") + kTestExtension;
    WriteTextFile(fx.Root / "Assets" / relPath, "v1");
    auto asset = fx.AddLoadedAsset(relPath);
    ASSERT_NE(asset, nullptr);
    const GUID guid = asset->GetGUID();

    const GUID redirectSource("5eed0000-1111-4222-8333-444444444444");
    ASSERT_TRUE(fx.Manager->GetRegistry().AddRedirect(redirectSource, guid));
    ASSERT_EQ(fx.Manager->GetAsset(redirectSource), asset)
        << "the redirect does not resolve, so this test would prove nothing about it";

    std::vector<GUID> invalidated;
    AssetReloadInvalidator invalidator(
        fx.Manager->GetEventDispatcher(), AssetType::XML,
        [&invalidated](const GUID& g) { invalidated.push_back(g); },
        AssetReloadInvalidator::EventSet::ReloadedOnly);

    SleepPastMtimeGranularity();
    WriteTextFile(fx.Root / "Assets" / relPath, "v2");
    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return asset->Content() == "v2" && !invalidated.empty();
        }))
        << "the edit never reloaded";

    EXPECT_NE(std::find(invalidated.begin(), invalidated.end(), guid), invalidated.end());
    EXPECT_NE(std::find(invalidated.begin(), invalidated.end(), redirectSource), invalidated.end())
        << "a cache holding the asset under its redirect source is never told it reloaded";
}

// The file-change half of the same class. A cache that decodes straight from disk (a heightmap, a
// terrain texture) never makes the asset resident, so no AssetReloaded ever names it; the
// watcher's AssetModified is its only signal. That event must reach a cache holding the file
// under a redirect source as well.
TEST(InlineReloadDeferral, AFileChangeInvalidatesTheGuidsThatRedirectToANonResidentFile)
{
    InlineReloadFixture fx("ge_inline_reload_content_redirect");
    ASSERT_TRUE(fx.Initialized);
    ASSERT_TRUE(fx.Registered);

    const std::filesystem::path relPath = std::string("decoded") + kTestExtension;
    WriteTextFile(fx.Root / "Assets" / relPath, "v1");
    const GUID guid = fx.Manager->ResolveAssetGuid(relPath);
    ASSERT_FALSE(guid.IsNull());
    ASSERT_EQ(fx.Manager->GetAsset(guid), nullptr) << "the file must stay non-resident";

    const GUID redirectSource("5eed0000-1111-4222-8333-555555555555");
    ASSERT_TRUE(fx.Manager->GetRegistry().AddRedirect(redirectSource, guid));

    std::mutex mutex;
    std::vector<GUID> invalidated;
    AssetReloadInvalidator invalidator(
        fx.Manager->GetEventDispatcher(), AssetType::XML,
        [&mutex, &invalidated](const GUID& g)
        {
            std::lock_guard<std::mutex> lock(mutex);
            invalidated.push_back(g);
        },
        AssetReloadInvalidator::EventSet::ContentEvents);

    // A save reported by its writer, as editor tools save; the writer's report drives the same
    // file-change dispatch the watcher does.
    SleepPastMtimeGranularity();
    {
        const std::filesystem::path path = fx.Root / "Assets" / relPath;
        auto write = fx.Manager->ExpectWrite(path);
        WriteTextFile(path, "v2");
        write.Report(path);
    }
    const auto contains = [&mutex, &invalidated](const GUID& g)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return std::find(invalidated.begin(), invalidated.end(), g) != invalidated.end();
    };
    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return contains(guid);
        }))
        << "the save was never reported as a file change";

    EXPECT_TRUE(contains(redirectSource))
        << "a cache holding the file under its redirect source is never told it changed";
}
