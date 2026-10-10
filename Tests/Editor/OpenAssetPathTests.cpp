#include <gtest/gtest.h>

#include "DebugServer/OpenAssetPath.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"

#include "TestTempDir.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
using namespace GameEngine;
namespace fs = std::filesystem;

// The asset-addressed debug commands take project-relative paths from scripts
// and IPC clients. The engine parks the process CWD at the project root, so a
// CWD-relative probe finds "Assets/<file>" and returns it verbatim; the
// registry then prefixes the asset root a second time. These tests pin the
// resolution to the asset mounts by parking the CWD on a decoy tree that a
// CWD-relative probe WOULD find.

class ScopedCurrentPath
{
  public:
    explicit ScopedCurrentPath(const fs::path& newCurrentPath)
    {
        std::error_code ec;
        m_PreviousPath = fs::current_path(ec);
        if (ec)
            return;
        fs::current_path(newCurrentPath, ec);
        m_Active = !ec;
    }

    ~ScopedCurrentPath()
    {
        if (!m_Active)
            return;
        std::error_code ec;
        fs::current_path(m_PreviousPath, ec);
    }

    bool IsActive() const { return m_Active; }

  private:
    fs::path m_PreviousPath;
    bool m_Active{false};
};

class OpenAssetPathTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    void SetUp() override
    {
        m_AssetRoot = EngineCore::GetInstance().GetAssetManager().GetAssetRoot();
        ASSERT_FALSE(m_AssetRoot.empty());
        ASSERT_TRUE(m_AssetRoot.is_absolute());

        // Every test process of this build shares the asset root: a subfolder of this process
        // keeps a concurrent run of these tests from deleting the files this one resolves.
        m_Subdir = "OpenAssetPathTests_" + std::to_string(TestUtils::GetProcessIdForTests());
        m_AssetDir = m_AssetRoot / m_Subdir;
        m_AwayDir = TestUtils::MakeUniqueTempDirectory("GameEngine_OpenAssetPathTests_away");
        std::error_code ec;
        fs::remove_all(m_AssetDir, ec);
        fs::create_directories(m_AssetDir, ec);
        fs::create_directories(m_AwayDir / m_Subdir, ec);

        // The real asset under the asset root, and a decoy at the same relative
        // spelling under the parked CWD.
        Touch(m_AssetDir / kFile);
        Touch(m_AwayDir / m_Subdir / kFile);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_AssetDir, ec);
        fs::remove_all(m_AwayDir, ec);
    }

    static void Touch(const fs::path& absPath)
    {
        std::ofstream out(absPath);
        out << "{}";
        ASSERT_TRUE(fs::exists(absPath));
    }

    static std::string Canon(const fs::path& p)
    {
        std::error_code ec;
        fs::path c = fs::weakly_canonical(p, ec);
        if (ec || c.empty())
            c = p.lexically_normal();
        std::string s = c.generic_string();
#if defined(_WIN32)
        for (auto& ch : s)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
#endif
        return s;
    }

    static const AssetManager& Assets() { return EngineCore::GetInstance().GetAssetManager(); }

    static constexpr const char* kFile = "asset.gltf";

    fs::path m_Subdir; // relative spelling of the asset folder under the asset root and the CWD
    fs::path m_AssetRoot;
    fs::path m_AssetDir;
    fs::path m_AwayDir; // CWD parked here: holds a decoy the mounts must beat
};

TEST_F(OpenAssetPathTests, RelativePathResolvesUnderAssetRootNotCwd)
{
    ScopedCurrentPath cwdGuard(m_AwayDir);
    ASSERT_TRUE(cwdGuard.IsActive());

    const fs::path resolved = Editor::ResolveOpenAssetPath(Assets(), m_Subdir / kFile);

    EXPECT_TRUE(resolved.is_absolute());
    EXPECT_EQ(Canon(resolved), Canon(m_AssetDir / kFile));
}

TEST_F(OpenAssetPathTests, AssetsPrefixedRelativePathIsNotDoubled)
{
    ScopedCurrentPath cwdGuard(m_AwayDir);
    ASSERT_TRUE(cwdGuard.IsActive());

    const fs::path resolved = Editor::ResolveOpenAssetPath(Assets(), fs::path("Assets") / m_Subdir / kFile);

    EXPECT_TRUE(resolved.is_absolute());
    EXPECT_EQ(Canon(resolved), Canon(m_AssetDir / kFile));
}

TEST_F(OpenAssetPathTests, AbsolutePathPassesThroughUnchanged)
{
    ScopedCurrentPath cwdGuard(m_AwayDir);
    ASSERT_TRUE(cwdGuard.IsActive());

    const fs::path absPath = m_AwayDir / m_Subdir / kFile;
    const fs::path resolved = Editor::ResolveOpenAssetPath(Assets(), absPath);

    EXPECT_EQ(Canon(resolved), Canon(absPath));
}

TEST_F(OpenAssetPathTests, EmptyPathStaysEmpty)
{
    EXPECT_TRUE(Editor::ResolveOpenAssetPath(Assets(), fs::path{}).empty());
}

} // namespace
