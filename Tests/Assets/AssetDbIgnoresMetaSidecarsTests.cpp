#include <gtest/gtest.h>

#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;

namespace
{
static void WriteTextFile(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << p.string();
    out << text;
}
} // namespace

TEST(AssetDatabase, IgnoresMetaSidecarsAndKeepsGuidFromAuthoritativeDbDuringAsyncScan)
{
    namespace fs = std::filesystem;

    const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_assetdb_ignore_meta");
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);

    const fs::path assetPath = tmpRoot / "foo.txt";
    WriteTextFile(assetPath, "hello");

    // Create a .meta sidecar with a different GUID; the new system must ignore it.
    const GUID metaGuid("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
    WriteTextFile(fs::path(assetPath.string() + ".meta"), "guid: " + metaGuid.ToString() + "\n");

    // Authoritative DB record (should win).
    const GUID dbGuid("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
    {
        const fs::path dbPath = tmpRoot / "AssetDatabase.assetdb";
        std::ofstream out(dbPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open()) << dbPath.string();
        out << "{\"guid\":\"" << dbGuid.ToString() << "\","
            << "\"path\":\"foo.txt\","
            << "\"type\":\"Unknown\","
            << "\"missing\":false,"
            << "\"kv\":{}}\n";
    }

    JobSystem::WorkStealingThreadPool pool(2);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(tmpRoot, &pool));

    // GUID should come from AssetDatabase.assetdb, not the sidecar.
    EXPECT_EQ(reg.GetAssetGUID(assetPath), dbGuid);

    // Drive an async scan; this must NOT replace the path->GUID mapping with a newly generated GUID.
    auto fut = reg.ScanDirectoryAsync(tmpRoot, true);
    (void)fut.get();

    EXPECT_EQ(reg.GetAssetGUID(assetPath), dbGuid);

    reg.Shutdown();
    fs::remove_all(tmpRoot, ec);
}


