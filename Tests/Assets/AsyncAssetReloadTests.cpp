// Async hot-reload pipeline (CheckForReloads -> StartAsyncReload ->
// DrainCompletedReloads): changed files are read+decoded on workers and the
// staged payload is adopted in place on the main thread. These tests lock the
// three contract points: in-place swap with pointer stability, supersede
// coalescing (drop/restart, no duplicate queueing), and failure keeping the
// previous payload.

#include <gtest/gtest.h>

#include "AssetCore/AssetEvents.h"
#include "AssetCore/AssetReloadInvalidator.h"
#include "Assets/AssetManager.h"
#include "Assets/AudioAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/TextureAsset.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{

void WriteBinaryFile(const std::filesystem::path& p, const void* data, size_t size)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << p.string();
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

void WriteTextFile(const std::filesystem::path& p, const std::string& text)
{
    WriteBinaryFile(p, text.data(), text.size());
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

// The mtime baseline recorded at Load() must be strictly older than the next
// write's mtime for NeedsReload()/the supersede gate to notice the change.
void SleepPastMtimeGranularity()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
}

// Test asset for the async reload pipeline. Decodes file bytes into a content
// string; content "FAIL" refuses to decode. A process-wide gate lets tests
// hold worker-side decodes open to stage supersede races deterministically.
class AsyncReloadTestAsset final : public Asset
{
  public:
    static inline std::atomic<int> s_DecodeStarted{0};
    static inline std::atomic<bool> s_BlockDecodes{false};

    static void ResetStatics()
    {
        s_DecodeStarted.store(0);
        s_BlockDecodes.store(false);
    }

    AsyncReloadTestAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::XML, path)
    {
    }

    bool Load() override
    {
        std::ifstream in(GetPath(), std::ios::binary);
        if (!in.is_open())
            return false;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return ApplyContent(std::move(text));
    }

    bool LoadFromData(const Vector<uint8>& data) override
    {
        s_DecodeStarted.fetch_add(1);
        while (s_BlockDecodes.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return ApplyContent(std::string(data.begin(), data.end()));
    }

    void Unload() override
    {
        m_Content.clear();
        SetState(AssetState::Unloaded);
    }

    bool SupportsAsyncReload() const override { return true; }

    std::string Content() const { return m_Content; }
    int AdoptCount() const { return m_AdoptCount; }

  protected:
    bool AdoptReloadedPayload(Asset& staged) override
    {
        auto* other = dynamic_cast<AsyncReloadTestAsset*>(&staged);
        if (!other || !other->IsLoaded())
            return false;
        std::swap(m_Content, other->m_Content);
        ++m_AdoptCount;
        return true;
    }

  private:
    bool ApplyContent(std::string text)
    {
        if (text == "FAIL")
        {
            SetState(AssetState::Failed);
            return false;
        }
        m_Content = std::move(text);
        SetState(AssetState::Loaded);
        return true;
    }

    std::string m_Content;
    int m_AdoptCount = 0;
};

constexpr const char* kTestExtension = ".artest";

// One manager + pool + registered test type per test, rooted in a unique temp dir.
class AsyncReloadFixture
{
  public:
    explicit AsyncReloadFixture(const char* tag)
    {
        AsyncReloadTestAsset::ResetStatics();
        Root = TestUtils::MakeUniqueTempDirectory(tag);
        std::error_code ec;
        std::filesystem::remove_all(Root, ec);
        std::filesystem::create_directories(Root / "Assets", ec);

        Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(4);
        Manager = std::make_unique<AssetManager>();
        Initialized = Manager->Initialize(Root / "Assets", Pool.get(),
                                          Root / "AssetDatabase.assetdb",
                                          Root / ".Cache" / "AssetDatabase");
        if (!Initialized)
            return;
        Manager->SetHotReloadEnabled(true);

        AssetTypeRegistration registration(
            AssetType::XML, {kTestExtension},
            [](const AssetMetadata& metadata) -> SharedPtr<Asset>
            {
                return std::static_pointer_cast<Asset>(
                    std::make_shared<AsyncReloadTestAsset>(metadata.Guid, metadata.Path));
            },
            "AsyncReloadTest", 100);
        Registered = Manager->GetAssetTypeRegistry().RegisterAssetType(registration);
    }

    ~AsyncReloadFixture()
    {
        AsyncReloadTestAsset::s_BlockDecodes.store(false); // never leave a decode parked
        if (Manager)
            Manager->Shutdown();
        Manager.reset();
        Pool.reset();
        std::error_code ec;
        std::filesystem::remove_all(Root, ec);
    }

    // Registers a live instance the way the editor holds one: loaded from
    // disk, then registered under its resolved GUID.
    std::shared_ptr<AsyncReloadTestAsset> AddLoadedAsset(const std::filesystem::path& relPath)
    {
        const GUID guid = Manager->ResolveAssetGuid(relPath);
        if (guid.IsNull())
            return nullptr;
        auto asset = std::make_shared<AsyncReloadTestAsset>(guid, Root / "Assets" / relPath);
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

// 24bpp bottom-up BMP filled with one color (stb_image decodes it).
std::vector<uint8_t> MakeBmp(uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b)
{
    const uint32_t rowBytes = ((width * 3u + 3u) / 4u) * 4u;
    const uint32_t pixelBytes = rowBytes * height;
    const uint32_t fileSize = 14u + 40u + pixelBytes;

    std::vector<uint8_t> bmp(fileSize, 0);
    auto put16 = [&bmp](size_t off, uint16_t v)
    {
        bmp[off] = static_cast<uint8_t>(v & 0xFF);
        bmp[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    auto put32 = [&bmp](size_t off, uint32_t v)
    {
        bmp[off] = static_cast<uint8_t>(v & 0xFF);
        bmp[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        bmp[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        bmp[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };

    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, fileSize);
    put32(10, 14u + 40u); // pixel data offset
    put32(14, 40u);       // BITMAPINFOHEADER size
    put32(18, width);
    put32(22, height);
    put16(26, 1);  // planes
    put16(28, 24); // bpp
    put32(34, pixelBytes);

    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t off = 54u + y * rowBytes + x * 3u;
            bmp[off] = b;
            bmp[off + 1] = g;
            bmp[off + 2] = r;
        }
    }
    return bmp;
}

} // namespace

TEST(AsyncAssetReload, SwapsPayloadInPlace_PointerStable_FiresReloadedEvent)
{
    AsyncReloadFixture fx("ge_async_reload_swap");
    ASSERT_TRUE(fx.Initialized);
    ASSERT_TRUE(fx.Registered);

    const std::filesystem::path relPath = std::filesystem::path("Textures") / (std::string("swap") + kTestExtension);
    WriteTextFile(fx.Root / "Assets" / relPath, "v1");

    auto asset = fx.AddLoadedAsset(relPath);
    ASSERT_NE(asset, nullptr);
    ASSERT_EQ(asset->Content(), "v1");
    Asset* const originalPointer = asset.get();
    const GUID guid = asset->GetGUID();

    std::atomic<int> reloadedEvents{0};
    const uint32 callbackHandle = fx.Manager->GetEventDispatcher().AddCallback(
        [&reloadedEvents, guid](const AssetEvent& event)
        {
            if (event.EventType == AssetEventType::AssetReloaded && event.AssetGuid == guid)
                reloadedEvents.fetch_add(1);
        });

    SleepPastMtimeGranularity();
    WriteTextFile(fx.Root / "Assets" / relPath, "v2");

    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return asset->Content() == "v2";
        }))
        << "Async reload never swapped the new payload in";

    EXPECT_EQ(fx.Manager->GetAsset(guid).get(), originalPointer) << "Asset identity must survive reload";
    EXPECT_TRUE(asset->IsLoaded());
    EXPECT_EQ(asset->AdoptCount(), 1);
    EXPECT_GE(reloadedEvents.load(), 1) << "AssetReloaded must fire on the main thread after the swap";

    fx.Manager->GetEventDispatcher().RemoveCallback(callbackHandle);
}

TEST(AsyncAssetReload, SecondChangeSupersedes_DropRestart_NoDuplicateAdopt)
{
    AsyncReloadFixture fx("ge_async_reload_supersede");
    ASSERT_TRUE(fx.Initialized);
    ASSERT_TRUE(fx.Registered);

    const std::filesystem::path relPath =
        std::filesystem::path("Textures") / (std::string("supersede") + kTestExtension);
    WriteTextFile(fx.Root / "Assets" / relPath, "v1");

    auto asset = fx.AddLoadedAsset(relPath);
    ASSERT_NE(asset, nullptr);
    Asset* const originalPointer = asset.get();

    // Hold worker decodes open so the second change lands while the first
    // reload is provably in flight.
    AsyncReloadTestAsset::s_BlockDecodes.store(true);

    SleepPastMtimeGranularity();
    WriteTextFile(fx.Root / "Assets" / relPath, "v2");
    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return AsyncReloadTestAsset::s_DecodeStarted.load() >= 1;
        }))
        << "First async reload decode never started";

    // While the decode is parked, the timestamp poll keeps re-noticing the
    // same mtime; none of those marks may restart the in-flight reload.
    for (int i = 0; i < 10; ++i)
    {
        fx.Manager->Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    EXPECT_EQ(AsyncReloadTestAsset::s_DecodeStarted.load(), 1)
        << "Re-noticing an unchanged mtime must not drop/restart the in-flight reload";

    SleepPastMtimeGranularity();
    WriteTextFile(fx.Root / "Assets" / relPath, "v3");
    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return AsyncReloadTestAsset::s_DecodeStarted.load() >= 2;
        }))
        << "Superseding change never restarted the pipeline";

    AsyncReloadTestAsset::s_BlockDecodes.store(false);

    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return asset->Content() == "v3";
        }))
        << "Superseding change never landed";

    // Drop/restart: the superseded v2 decode must never adopt. Pump a little
    // longer to catch a straggler adopt before asserting.
    for (int i = 0; i < 10; ++i)
    {
        fx.Manager->Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    EXPECT_EQ(asset->Content(), "v3");
    EXPECT_EQ(asset->AdoptCount(), 1) << "Exactly one adopt: superseded generation must be dropped";
    EXPECT_EQ(AsyncReloadTestAsset::s_DecodeStarted.load(), 2) << "No duplicate pipelines may queue up";
    EXPECT_EQ(fx.Manager->GetAsset(asset->GetGUID()).get(), originalPointer);
}

TEST(AsyncAssetReload, ExplicitRequestRestartsWhenMtimeUnchanged)
{
    AsyncReloadFixture fx("ge_async_reload_explicit");
    ASSERT_TRUE(fx.Initialized);
    ASSERT_TRUE(fx.Registered);

    const std::filesystem::path relPath =
        std::filesystem::path("Textures") / (std::string("explicit") + kTestExtension);
    WriteTextFile(fx.Root / "Assets" / relPath, "v1");

    auto asset = fx.AddLoadedAsset(relPath);
    ASSERT_NE(asset, nullptr);

    AsyncReloadTestAsset::s_BlockDecodes.store(true);
    ASSERT_TRUE(fx.Manager->RequestAsyncReload(asset->GetGUID()));
    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return AsyncReloadTestAsset::s_DecodeStarted.load() >= 1;
        }))
        << "First explicit reload decode never started";

    ASSERT_TRUE(fx.Manager->RequestAsyncReload(asset->GetGUID()));
    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return AsyncReloadTestAsset::s_DecodeStarted.load() >= 2;
        }))
        << "Explicit RequestAsyncReload must restart even when file mtime is unchanged";

    AsyncReloadTestAsset::s_BlockDecodes.store(false);
    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return asset->AdoptCount() >= 1;
        }))
        << "Explicit reload never adopted";
    EXPECT_EQ(asset->Content(), "v1");
    EXPECT_EQ(AsyncReloadTestAsset::s_DecodeStarted.load(), 2);
}

// A settings change during the asset's first load (a texture cook usage widened by a second
// material, a cook setting edited in the inspector) asks for a reload while nothing is resident
// yet, and the load in flight may already have read the old settings. The request is kept and
// the reload runs once the load lands.
TEST(AsyncAssetReload, ExplicitRequestDuringTheFirstLoadReloadsOnceTheLoadLands)
{
    AsyncReloadFixture fx("ge_async_reload_during_load");
    ASSERT_TRUE(fx.Initialized);
    ASSERT_TRUE(fx.Registered);

    const std::filesystem::path relPath =
        std::filesystem::path("Textures") / (std::string("first-load") + kTestExtension);
    WriteTextFile(fx.Root / "Assets" / relPath, "v1");
    const GUID guid = fx.Manager->ResolveAssetGuid(relPath);
    ASSERT_FALSE(guid.IsNull());

    AsyncReloadTestAsset::s_BlockDecodes.store(true);
    const AssetLoadHandle load = fx.Manager->LoadAsset(guid, [](Result<SharedPtr<Asset>, AssetError>) {});
    ASSERT_TRUE(WaitUntil([] { return AsyncReloadTestAsset::s_DecodeStarted.load() >= 1; }))
        << "The first load's decode never started";
    ASSERT_FALSE(load.IsComplete());

    EXPECT_TRUE(fx.Manager->RequestAsyncReload(guid));
    AsyncReloadTestAsset::s_BlockDecodes.store(false);
    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            const auto asset = std::dynamic_pointer_cast<AsyncReloadTestAsset>(load.GetResult());
            return asset && asset->AdoptCount() >= 1;
        }))
        << "The request made during the first load never reloaded the asset";
    EXPECT_EQ(AsyncReloadTestAsset::s_DecodeStarted.load(), 2);
}

// The kept request follows the asset when an overlapping derived-identity source re-claims its
// path while the first load is in flight: the load lands under the new GUID, and so must the
// reload.
TEST(AsyncAssetReload, RequestKeptForTheFirstLoadFollowsAGuidRemap)
{
    AsyncReloadFixture fx("ge_async_reload_during_load_remap");
    ASSERT_TRUE(fx.Initialized);
    ASSERT_TRUE(fx.Registered);
    AssetSourceDesc overlapping{};
    overlapping.Alias = "overlapping";
    overlapping.Root = fx.Root / "Assets";
    overlapping.DerivedIdentity = true;
    overlapping.Priority = 50;
    ASSERT_TRUE(fx.Manager->RegisterSource(overlapping));

    const std::filesystem::path relPath =
        std::filesystem::path("Textures") / (std::string("remapped") + kTestExtension);
    const std::filesystem::path file = fx.Root / "Assets" / relPath;
    WriteTextFile(file, "v1");
    const GUID guid = fx.Manager->ResolveAssetGuid(relPath);
    ASSERT_FALSE(guid.IsNull());

    AsyncReloadTestAsset::s_BlockDecodes.store(true);
    const AssetLoadHandle load = fx.Manager->LoadAsset(guid, [](Result<SharedPtr<Asset>, AssetError>) {});
    ASSERT_TRUE(WaitUntil([] { return AsyncReloadTestAsset::s_DecodeStarted.load() >= 1; }))
        << "The first load's decode never started";
    EXPECT_TRUE(fx.Manager->RequestAsyncReload(guid));

    ASSERT_TRUE(fx.Manager->GetRegistry().RegisterAsset(file, "overlapping"));
    const GUID remapped = fx.Manager->GetRegistry().GetAssetGUID(file);
    ASSERT_NE(remapped, guid) << "the overlapping source should re-claim the path under a new GUID";

    AsyncReloadTestAsset::s_BlockDecodes.store(false);
    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            const auto asset = std::dynamic_pointer_cast<AsyncReloadTestAsset>(fx.Manager->GetAsset(remapped));
            return asset && asset->AdoptCount() >= 1;
        }))
        << "The request kept for the first load was dropped by the remap";
    EXPECT_EQ(AsyncReloadTestAsset::s_DecodeStarted.load(), 2);
}

TEST(AsyncAssetReload, FailedDecodeKeepsPreviousPayload_ThenRecovers)
{
    AsyncReloadFixture fx("ge_async_reload_fail");
    ASSERT_TRUE(fx.Initialized);
    ASSERT_TRUE(fx.Registered);

    const std::filesystem::path relPath = std::filesystem::path("Textures") / (std::string("fail") + kTestExtension);
    WriteTextFile(fx.Root / "Assets" / relPath, "v1");

    auto asset = fx.AddLoadedAsset(relPath);
    ASSERT_NE(asset, nullptr);
    const GUID guid = asset->GetGUID();

    std::atomic<int> failedEvents{0};
    const uint32 callbackHandle = fx.Manager->GetEventDispatcher().AddCallback(
        [&failedEvents, guid](const AssetEvent& event)
        {
            if (event.EventType == AssetEventType::AssetLoadFailed && event.AssetGuid == guid)
                failedEvents.fetch_add(1);
        });

    SleepPastMtimeGranularity();
    WriteTextFile(fx.Root / "Assets" / relPath, "FAIL");

    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return failedEvents.load() >= 1;
        }))
        << "Failed async reload never reported AssetLoadFailed";

    EXPECT_EQ(asset->Content(), "v1") << "Failed reload must keep the previous payload";
    EXPECT_TRUE(asset->IsLoaded()) << "The live asset must stay usable after a failed reload";
    EXPECT_EQ(asset->AdoptCount(), 0);

    // The pipeline must not wedge: a subsequent good save still lands.
    SleepPastMtimeGranularity();
    WriteTextFile(fx.Root / "Assets" / relPath, "v2");
    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return asset->Content() == "v2";
        }))
        << "Reload pipeline wedged after a failed decode";

    fx.Manager->GetEventDispatcher().RemoveCallback(callbackHandle);
}

namespace
{

// Minimal OBJ text with `triangles` disjoint triangles (3 vertices each).
std::string MakeObj(uint32_t triangles)
{
    std::string obj;
    for (uint32_t t = 0; t < triangles; ++t)
    {
        const float y = static_cast<float>(t) * 2.0f;
        obj += "v 0 " + std::to_string(y) + " 0\n";
        obj += "v 1 " + std::to_string(y) + " 0\n";
        obj += "v 0 " + std::to_string(y + 1.0f) + " 0\n";
        const uint32_t base = t * 3u + 1u;
        obj += "f " + std::to_string(base) + " " + std::to_string(base + 1u) + " " +
               std::to_string(base + 2u) + "\n";
    }
    return obj;
}

// Minimal 16-bit mono PCM WAV with `frames` zero samples at `sampleRate`.
std::vector<uint8_t> MakeWav(uint32_t sampleRate, uint32_t frames)
{
    const uint32_t dataSize = frames * 2u; // mono, 16-bit
    std::vector<uint8_t> wav(44u + dataSize, 0);
    auto put16 = [&wav](size_t off, uint16_t v)
    {
        wav[off] = static_cast<uint8_t>(v & 0xFF);
        wav[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    auto put32 = [&wav](size_t off, uint32_t v)
    {
        wav[off] = static_cast<uint8_t>(v & 0xFF);
        wav[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        wav[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        wav[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    std::memcpy(wav.data(), "RIFF", 4);
    put32(4, 36u + dataSize);
    std::memcpy(wav.data() + 8, "WAVE", 4);
    std::memcpy(wav.data() + 12, "fmt ", 4);
    put32(16, 16u);            // fmt chunk size
    put16(20, 1u);             // PCM
    put16(22, 1u);             // mono
    put32(24, sampleRate);
    put32(28, sampleRate * 2u); // byte rate
    put16(32, 2u);              // block align
    put16(34, 16u);             // bits per sample
    std::memcpy(wav.data() + 36, "data", 4);
    put32(40, dataSize);
    return wav;
}

} // namespace

TEST(AsyncAssetReload, ModelObj_AdoptsNewGeometryInPlace)
{
    AsyncReloadFixture fx("ge_async_reload_model");
    ASSERT_TRUE(fx.Initialized);

    const std::filesystem::path relPath = std::filesystem::path("Models") / "hot.obj";
    const std::filesystem::path absPath = fx.Root / "Assets" / relPath;
    WriteTextFile(absPath, MakeObj(1));

    const GUID guid = fx.Manager->ResolveAssetGuid(relPath);
    ASSERT_FALSE(guid.IsNull());
    auto model = std::make_shared<ModelAsset>(guid, absPath);
    ASSERT_TRUE(model->Load());
    ASSERT_EQ(model->GetMeshCount(), 1u);
    const uint32 initialVertexCount = model->GetTotalVertexCount();
    ASSERT_GT(initialVertexCount, 0u);
    fx.Manager->RegisterLoadedAsset(guid, model);
    Asset* const originalPointer = model.get();

    SleepPastMtimeGranularity();
    WriteTextFile(absPath, MakeObj(2));

    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return model->GetTotalVertexCount() == initialVertexCount * 2u;
        }))
        << "Model async reload never adopted the new geometry (verts stayed "
        << model->GetTotalVertexCount() << ")";

    EXPECT_TRUE(model->IsLoaded());
    EXPECT_EQ(model->GetMeshCount(), 1u);
    EXPECT_EQ(fx.Manager->GetAsset(guid).get(), originalPointer) << "ModelAsset identity must survive reload";
}

TEST(AsyncAssetReload, AudioWav_AdoptsNewPcmInPlace)
{
    AsyncReloadFixture fx("ge_async_reload_audio");
    ASSERT_TRUE(fx.Initialized);

    const std::filesystem::path relPath = std::filesystem::path("Audio") / "hot.wav";
    const std::filesystem::path absPath = fx.Root / "Assets" / relPath;
    const auto shortWav = MakeWav(8000u, 800u);
    WriteBinaryFile(absPath, shortWav.data(), shortWav.size());

    const GUID guid = fx.Manager->ResolveAssetGuid(relPath);
    ASSERT_FALSE(guid.IsNull());
    auto audio = std::make_shared<AudioAsset>(guid, absPath);
    ASSERT_TRUE(audio->Load());
    ASSERT_EQ(audio->GetPCMFrameCount(), 800u);
    ASSERT_EQ(audio->GetSampleRate(), 8000u);
    fx.Manager->RegisterLoadedAsset(guid, audio);
    Asset* const originalPointer = audio.get();

    SleepPastMtimeGranularity();
    const auto longWav = MakeWav(16000u, 3200u);
    WriteBinaryFile(absPath, longWav.data(), longWav.size());

    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return audio->GetPCMFrameCount() == 3200u;
        }))
        << "Audio async reload never adopted the new PCM (frames stayed "
        << audio->GetPCMFrameCount() << ")";

    EXPECT_EQ(audio->GetSampleRate(), 16000u);
    EXPECT_NE(audio->GetPCMFloatData(), nullptr);
    EXPECT_TRUE(audio->IsLoaded());
    EXPECT_EQ(fx.Manager->GetAsset(guid).get(), originalPointer) << "AudioAsset identity must survive reload";
}

TEST(AsyncAssetReload, TextureBmp_AdoptsNewDimensionsInPlace)
{
    AsyncReloadFixture fx("ge_async_reload_texture");
    ASSERT_TRUE(fx.Initialized);

    const std::filesystem::path relPath = std::filesystem::path("Textures") / "hot.bmp";
    const std::filesystem::path absPath = fx.Root / "Assets" / relPath;
    const auto smallBmp = MakeBmp(2, 2, 255, 0, 0);
    WriteBinaryFile(absPath, smallBmp.data(), smallBmp.size());

    const GUID guid = fx.Manager->ResolveAssetGuid(relPath);
    ASSERT_FALSE(guid.IsNull());
    auto texture = std::make_shared<TextureAsset>(guid, absPath);
    ASSERT_TRUE(texture->Load());
    ASSERT_EQ(texture->GetWidth(), 2u);
    fx.Manager->RegisterLoadedAsset(guid, texture);
    Asset* const originalPointer = texture.get();

    SleepPastMtimeGranularity();
    const auto bigBmp = MakeBmp(4, 4, 0, 0, 255);
    WriteBinaryFile(absPath, bigBmp.data(), bigBmp.size());

    ASSERT_TRUE(WaitUntil(
        [&]
        {
            fx.Manager->Update();
            return texture->GetWidth() == 4u;
        }))
        << "Texture async reload never adopted the new pixels";

    EXPECT_EQ(texture->GetHeight(), 4u);
    EXPECT_NE(texture->GetPixelData(), nullptr);
    EXPECT_TRUE(texture->IsLoaded());
    EXPECT_EQ(fx.Manager->GetAsset(guid).get(), originalPointer) << "TextureAsset identity must survive reload";
}

// The async reload path raises its own AssetReloaded (DrainCompletedReloads). It must carry the
// asset's redirect sources like the inline path, or a texture or model bound through a redirected
// GUID keeps its first upload after an edit.
TEST(AsyncAssetReload, ReloadedEventCarriesTheGuidsThatRedirectToTheAsset)
{
    AsyncReloadFixture fx("ge_async_reload_redirect");
    ASSERT_TRUE(fx.Initialized);
    ASSERT_TRUE(fx.Registered);

    const std::filesystem::path relPath = std::string("redirected") + kTestExtension;
    WriteTextFile(fx.Root / "Assets" / relPath, "v1");
    auto asset = fx.AddLoadedAsset(relPath);
    ASSERT_NE(asset, nullptr);
    const GUID guid = asset->GetGUID();

    const GUID redirectSource("5eed0000-1111-4222-8333-666666666666");
    ASSERT_TRUE(fx.Manager->GetRegistry().AddRedirect(redirectSource, guid));

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
        << "the async reload never landed";

    EXPECT_EQ(asset->AdoptCount(), 1) << "the reload must have taken the async path";
    EXPECT_NE(std::find(invalidated.begin(), invalidated.end(), redirectSource), invalidated.end())
        << "the async reload never reached a cache holding the asset under its redirect source";
}
