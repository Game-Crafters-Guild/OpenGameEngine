// The ambient cancel flag a decode publishes for its own call tree. Long-running
// decode work (the texture import cook's BCn encode) reads it through
// CurrentAssetDecodeCancellation() rather than through a parameter on the
// generic ProcessAssetData seam, so the scoping contract below is what keeps a
// cancel reaching that work — and what keeps one decode's flag from leaking into
// the next decode to run on the same pooled worker thread.

#include <gtest/gtest.h>

#include "Assets/AssetDecodeCancellation.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetTasks.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/ModelAsset.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{

TEST(AssetDecodeCancellation, NoScopeMeansNotCancellable)
{
    EXPECT_EQ(CurrentAssetDecodeCancellation(), nullptr);
    EXPECT_FALSE(IsAssetDecodeCancelled());
}

TEST(AssetDecodeCancellation, ScopePublishesFlagAndRestoresOnExit)
{
    auto flag = std::make_shared<std::atomic<bool>>(false);
    {
        ScopedAssetDecodeCancellation scope(flag);
        EXPECT_EQ(CurrentAssetDecodeCancellation(), flag);
        EXPECT_FALSE(IsAssetDecodeCancelled());

        flag->store(true, std::memory_order_release);
        EXPECT_TRUE(IsAssetDecodeCancelled());
    }
    // A pooled worker runs decode after decode; a leaked flag would cancel the
    // next one.
    EXPECT_EQ(CurrentAssetDecodeCancellation(), nullptr);
    EXPECT_FALSE(IsAssetDecodeCancelled());
}

TEST(AssetDecodeCancellation, NestedScopesRestoreTheOuterFlag)
{
    auto outer = std::make_shared<std::atomic<bool>>(false);
    auto inner = std::make_shared<std::atomic<bool>>(true);
    {
        ScopedAssetDecodeCancellation outerScope(outer);
        {
            ScopedAssetDecodeCancellation innerScope(inner);
            EXPECT_EQ(CurrentAssetDecodeCancellation(), inner);
            EXPECT_TRUE(IsAssetDecodeCancelled());
        }
        EXPECT_EQ(CurrentAssetDecodeCancellation(), outer);
        EXPECT_FALSE(IsAssetDecodeCancelled());
    }
    EXPECT_EQ(CurrentAssetDecodeCancellation(), nullptr);
}

TEST(AssetDecodeCancellation, ANullFlagIsAValidNotCancellableScope)
{
    auto flag = std::make_shared<std::atomic<bool>>(true);
    ScopedAssetDecodeCancellation scope(flag);
    {
        // Synchronous loads and tooling decode with no flag at all.
        ScopedAssetDecodeCancellation nullScope({});
        EXPECT_EQ(CurrentAssetDecodeCancellation(), nullptr);
        EXPECT_FALSE(IsAssetDecodeCancelled());
    }
    EXPECT_TRUE(IsAssetDecodeCancelled());
}

TEST(AssetDecodeCancellation, FlagIsPerThread)
{
    auto flag = std::make_shared<std::atomic<bool>>(true);
    ScopedAssetDecodeCancellation scope(flag);
    ASSERT_TRUE(IsAssetDecodeCancelled());

    // Decodes run concurrently on pool workers; one cancelling must not read as
    // every worker cancelling.
    bool otherThreadSawFlag = true;
    std::thread other([&otherThreadSawFlag]
                      { otherThreadSawFlag = CurrentAssetDecodeCancellation() != nullptr; });
    other.join();
    EXPECT_FALSE(otherThreadSawFlag);
    EXPECT_TRUE(IsAssetDecodeCancelled());
}

// ── The publish -> cook seam ─────────────────────────────────────────────────
//
// The scope above is only worth anything if a real decode's flag reaches the
// work that polls it. That path crosses three files and a thread-local —
// DecodeAssetPayload publishes, ProcessAssetData dispatches by type, and
// TextureAsset::TryLoadViaCook reads it back through
// CurrentAssetDecodeCancellation() to hand to CookTexture — so it is exercised
// here rather than argued from the call sites.
//
// The discriminator is the cook ARTIFACT, not the decode's return value. A null
// return also happens when the cook never ran at all (no derived-cache root,
// cook disabled, wrong asset type), so asserting only on null would pass on a
// fixture that never reaches CookTexture. The cancelled run must leave the
// derived cache empty; the uncancelled run right after it — same manager, same
// asset, still-cold cache — must fill it.

// Minimal uncompressed 32-bit TGA (top-left origin), large enough for a real
// mip chain. The cook resolves to Uncompressed here (no GPU, so no BC support
// reported) but still runs, because mips are on by default — and the cancel is
// polled per level regardless of output format.
void WriteTestTga(const std::filesystem::path& path, uint32_t width, uint32_t height)
{
    std::vector<unsigned char> tga(18, 0);
    tga[2] = 2; // uncompressed true-color
    tga[12] = static_cast<unsigned char>(width & 0xFF);
    tga[13] = static_cast<unsigned char>((width >> 8) & 0xFF);
    tga[14] = static_cast<unsigned char>(height & 0xFF);
    tga[15] = static_cast<unsigned char>((height >> 8) & 0xFF);
    tga[16] = 32;   // bits per pixel
    tga[17] = 0x20; // top-left origin
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            tga.push_back(static_cast<unsigned char>(x));     // B
            tga.push_back(static_cast<unsigned char>(y));     // G
            tga.push_back(static_cast<unsigned char>(x ^ y)); // R
            tga.push_back(255);                               // A
        }
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(tga.data()), static_cast<std::streamsize>(tga.size()));
}

class AssetDecodeCancellationSeamTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = TestUtils::MakeUniqueTempDirectory("asset_decode_cancel_seam");
        std::filesystem::create_directories(m_Root);
        m_Source = m_Root / "cancel_seam.tga";
        // Written before Initialize so the startup scan claims its identity.
        WriteTestTga(m_Source, 128, 128);

        m_JobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        m_Manager = std::make_unique<AssetManager>();
        // The db file and cache root are the whole reason this fixture differs
        // from the other AssetManager fixtures: without them the registry has no
        // derived-cache root and TryLoadViaCook returns before it ever cooks.
        ASSERT_TRUE(m_Manager->Initialize(m_Root, m_JobSystem.get(),
                                          m_Root / "AssetDatabase.assetdb",
                                          m_Root / ".Cache" / "AssetDatabase"));
        m_Manager->WaitForStartupScan();
    }

    void TearDown() override
    {
        if (m_Manager)
        {
            m_Manager->Shutdown();
            m_Manager.reset();
        }
        if (m_JobSystem)
        {
            m_JobSystem->Shutdown();
            m_JobSystem.reset();
        }
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    size_t CookedArtifactCount() const
    {
        size_t count = 0;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(m_Root, ec), end; it != end;
             it.increment(ec))
        {
            if (ec)
                break;
            if (it->is_regular_file(ec) && it->path().extension() == ".ktx2")
                ++count;
        }
        return count;
    }

    static std::vector<uint8> ReadAllBytes(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
            return {};
        const auto size = static_cast<size_t>(in.tellg());
        in.seekg(0);
        std::vector<uint8> bytes(size);
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        return bytes;
    }

    std::filesystem::path m_Root;
    std::filesystem::path m_Source;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_JobSystem;
    std::unique_ptr<AssetManager> m_Manager;
};

TEST_F(AssetDecodeCancellationSeamTest, PublishedFlagReachesTheTextureCook)
{
    AssetMetadata metadata;
    ASSERT_TRUE(m_Manager->GetRegistry().TryGetAssetMetadata(m_Source, metadata));
    ASSERT_EQ(metadata.Type, AssetType::Texture);

    const std::vector<uint8> bytes = ReadAllBytes(m_Source);
    ASSERT_FALSE(bytes.empty());
    ASSERT_EQ(CookedArtifactCount(), 0u) << "fixture started with a warm cache";

    // Cancelled: DecodeAssetPayload publishes the flag for its own call tree, so
    // the cook sees it through the thread-local and abandons before producing
    // bytes — and a cook that produces no bytes cannot write a cache entry.
    auto cancelled = std::make_shared<std::atomic<bool>>(true);
    const SharedPtr<Asset> abandoned =
        DecodeAssetPayload(*m_Manager, metadata.Guid, metadata, bytes, cancelled);
    EXPECT_EQ(abandoned, nullptr);
    EXPECT_EQ(CookedArtifactCount(), 0u) << "a cancelled decode wrote a derived-cache entry";

    // Positive control, and the reason the assertion above means anything: the
    // same asset through the same seam with the flag clear has to cook.
    auto live = std::make_shared<std::atomic<bool>>(false);
    const SharedPtr<Asset> loaded =
        DecodeAssetPayload(*m_Manager, metadata.Guid, metadata, bytes, live);
    EXPECT_NE(loaded, nullptr);
    EXPECT_GE(CookedArtifactCount(), 1u)
        << "the uncancelled control never cooked, so the cancelled case above proves nothing "
           "about the cook — check the derived-cache root and the asset type";
}


// A grid plane as Wavefront OBJ: enough tessellation that meshopt can reduce it,
// written as a source file so the load goes through the real decode path rather
// than a SetMeshesForTest hook.
void WriteGridObj(const std::filesystem::path& path, uint32 n)
{
    std::ostringstream obj;
    obj << "# generated grid fixture\n";
    for (uint32 z = 0; z < n; ++z)
        for (uint32 x = 0; x < n; ++x)
            obj << "v " << x << " 0 " << z << "\n";
    obj << "vn 0 1 0\n";
    for (uint32 z = 0; z + 1 < n; ++z)
    {
        for (uint32 x = 0; x + 1 < n; ++x)
        {
            const uint32 i0 = z * n + x + 1; // OBJ indices are 1-based
            const uint32 i1 = z * n + x + 2;
            const uint32 i2 = (z + 1) * n + x + 1;
            const uint32 i3 = (z + 1) * n + x + 2;
            obj << "f " << i0 << "//1 " << i2 << "//1 " << i1 << "//1\n";
            obj << "f " << i1 << "//1 " << i2 << "//1 " << i3 << "//1\n";
        }
    }
    const std::string text = obj.str();
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// The mesh-LOD half of the same contract the seam test covers for textures: a
// cancelled generation produces a SHORT chain, so the thing that must not happen
// is it being written to the editor LOD cache under the full config key. If it
// were, every later load would read it back as a Hit and the model would keep
// its truncated chain forever, with nothing left to indicate a cancel occurred.
class LodCacheCancellationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = TestUtils::MakeUniqueTempDirectory("lod_cache_cancel");
        std::filesystem::create_directories(m_Root);
        m_Source = m_Root / "lod_cancel_grid.obj";
        WriteGridObj(m_Source, 48);

        m_JobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        m_Manager = std::make_unique<AssetManager>();
        ASSERT_TRUE(m_Manager->Initialize(m_Root, m_JobSystem.get(),
                                          m_Root / "AssetDatabase.assetdb",
                                          m_Root / ".Cache" / "AssetDatabase"));
        m_Manager->WaitForStartupScan();

        // LOD generation is opt-in and off by default, so without arming it the
        // load never reaches LoadOrGenerateLODs and this whole fixture would pass
        // while testing nothing.
        m_PreviousLodSettings = GetLODImportSettings();
        LODImportSettings armed;
        armed.AutoGenerateOnImport = true;
        SetLODImportSettings(armed);
    }

    void TearDown() override
    {
        SetLODImportSettings(m_PreviousLodSettings);
        if (m_Manager)
        {
            m_Manager->Shutdown();
            m_Manager.reset();
        }
        if (m_JobSystem)
        {
            m_JobSystem->Shutdown();
            m_JobSystem.reset();
        }
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    size_t LodCacheEntryCount() const
    {
        size_t count = 0;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(m_Root, ec), end; it != end;
             it.increment(ec))
        {
            if (ec)
                break;
            if (it->is_regular_file(ec) && it->path().extension() == ".gelod")
                ++count;
        }
        return count;
    }

    static std::vector<uint8> ReadAllBytes(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
            return {};
        const auto size = static_cast<size_t>(in.tellg());
        in.seekg(0);
        std::vector<uint8> bytes(size);
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        return bytes;
    }

    static bool AnyMeshHasExtraLods(const ModelAsset& model)
    {
        for (uint32 i = 0; i < model.GetMeshCount(); ++i)
            if (!model.GetMesh(i).ExtraLODs.empty())
                return true;
        return false;
    }

    std::filesystem::path m_Root;
    std::filesystem::path m_Source;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_JobSystem;
    std::unique_ptr<AssetManager> m_Manager;
    LODImportSettings m_PreviousLodSettings;
};

TEST_F(LodCacheCancellationTest, CancelledGenerationLeavesNoLodCacheEntry)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";

    AssetMetadata metadata;
    ASSERT_TRUE(m_Manager->GetRegistry().TryGetAssetMetadata(m_Source, metadata));
    ASSERT_EQ(metadata.Type, AssetType::Model);

    const std::vector<uint8> bytes = ReadAllBytes(m_Source);
    ASSERT_FALSE(bytes.empty());
    ASSERT_EQ(LodCacheEntryCount(), 0u) << "fixture started with a warm LOD cache";

    // Cancelled: the generator abandons between levels, so the chain it hands
    // back is short. Nothing may persist it.
    auto cancelled = std::make_shared<std::atomic<bool>>(true);
    DecodeAssetPayload(*m_Manager, metadata.Guid, metadata, bytes, cancelled);
    EXPECT_EQ(LodCacheEntryCount(), 0u)
        << "a cancelled generation wrote a LOD cache entry; every later load would take that "
           "truncated chain as a hit";

    // Positive control, and the reason the assertion above means anything: the
    // same model through the same seam with the flag clear has to cook LODs.
    auto live = std::make_shared<std::atomic<bool>>(false);
    const SharedPtr<Asset> loaded =
        DecodeAssetPayload(*m_Manager, metadata.Guid, metadata, bytes, live);
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(LodCacheEntryCount(), 1u)
        << "the uncancelled control never cooked LODs, so the cancelled case above proves "
           "nothing — check the derived-cache root and that LOD generation is armed";

    // The load after a cancel is a MISS that regenerates, not a hit on a
    // truncated chain: the model that comes back actually carries levels.
    const auto model = std::static_pointer_cast<ModelAsset>(loaded);
    ASSERT_GT(model->GetMeshCount(), 0u);
    EXPECT_TRUE(AnyMeshHasExtraLods(*model))
        << "the load following a cancel came back with no LOD levels — a truncated cache hit";
}


} // namespace
