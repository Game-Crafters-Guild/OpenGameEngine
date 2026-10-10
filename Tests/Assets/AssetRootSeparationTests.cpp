#include <gtest/gtest.h>

#include "AssetCore/AssetIgnoreRules.h"
#include "AssetCore/PathNormalization.h"
#include "AssetDatabase/AssetDbCache_Sqlite.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetDatabase/AssetSourceSnapshot.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "Core/EngineLoggerBridge.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/LogSink.h"
#include "Logger/Logger.h"
#include "Scripting/ScriptManager.h"
#include "TestTempDir.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

static std::string ReadFileBytes(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open())
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string NormalizePathForCompare(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::path canon = std::filesystem::weakly_canonical(p, ec);
    if (ec)
        canon = std::filesystem::absolute(p, ec).lexically_normal();
    std::string lower = canon.generic_string();
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return lower;
}

class ScopedCurrentPath
{
  public:
    explicit ScopedCurrentPath(const std::filesystem::path& newCurrentPath)
    {
        std::error_code ec;
        m_PreviousPath = std::filesystem::current_path(ec);
        if (ec)
            return;

        std::filesystem::current_path(newCurrentPath, ec);
        m_Active = !ec;
    }

    ~ScopedCurrentPath()
    {
        if (!m_Active)
            return;

        std::error_code ec;
        std::filesystem::current_path(m_PreviousPath, ec);
    }

    bool IsActive() const { return m_Active; }

  private:
    std::filesystem::path m_PreviousPath;
    bool m_Active{false};
};

template <typename Predicate>
static bool WaitUntil(Predicate&& predicate, int timeoutMs = 8000, int pollMs = 50)
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

class TestReloadableAsset final : public Asset
{
  public:
    TestReloadableAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::UIStyle, path)
    {
    }

    bool Load() override
    {
        m_LoadCount.fetch_add(1, std::memory_order_relaxed);
        SetState(AssetState::Loaded);
        return true;
    }

    bool LoadFromData(const Vector<uint8>& /*data*/) override
    {
        m_LoadCount.fetch_add(1, std::memory_order_relaxed);
        SetState(AssetState::Loaded);
        return true;
    }

    void Unload() override
    {
        m_UnloadCount.fetch_add(1, std::memory_order_relaxed);
        SetState(AssetState::Unloaded);
    }

    int LoadCount() const { return m_LoadCount.load(std::memory_order_relaxed); }

  private:
    std::atomic<int> m_LoadCount{0};
    std::atomic<int> m_UnloadCount{0};
};

// Captures formatted log lines so tests can assert on one-shot summary
// messages (e.g. the E1 absolute-path purge summary).
class CapturingLogSink final : public Logger::LogSink
{
  public:
    explicit CapturingLogSink(std::vector<std::string>* out) : m_Out(out) {}

    void Write(const Logger::LogMessage& message) override
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Out->push_back(message.Message);
    }

    void Flush() override {}
    bool ShouldLog(Logger::LogLevel) const override { return true; }
    Logger::String GetName() const override { return "CapturingLogSink"; }

  private:
    std::vector<std::string>* m_Out;
    std::mutex m_Mutex;
};
} // namespace

TEST(AssetRootSeparation, EditorMountIsNonPersistentWhenRootsDiffer_NoProjectDbPollution_DeterministicGuid_EditorLocalKv)
{
    namespace fs = std::filesystem;

    const fs::path projectRoot = TestUtils::MakeUniqueTempDirectory("ge_project_root_sep");
    const fs::path editorRoot = TestUtils::MakeUniqueTempDirectory("ge_editor_root_sep");

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
    fs::remove_all(editorRoot, ec);
    fs::create_directories(projectRoot / "Assets", ec);
    fs::create_directories(editorRoot / "Assets", ec);

    // Editor asset (non-persistent mount)
    WriteTextFile(editorRoot / "Assets" / "UI" / "theme.css", "DUMMY");

    const fs::path projectAssetsRoot = projectRoot / "Assets";
    const fs::path editorAssetsRoot = editorRoot / "Assets";

    const fs::path projectDb = projectRoot / "AssetDatabase.assetdb";
    const fs::path projectCache = projectRoot / ".Cache" / "AssetDatabase";

    // For testing editor-local DB behavior we use a temp location (not the real user cache dir).
    const fs::path editorLocalRoot = projectRoot / ".EditorLocal";
    const fs::path editorDb = editorLocalRoot / "EditorAssetDatabase.assetdb";
    const fs::path editorCache = editorLocalRoot / ".Cache" / "AssetDatabase";

    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(projectAssetsRoot, nullptr, projectDb, projectCache));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = editorAssetsRoot;
    editorSource.AuthoritativeDbFile = editorDb;
    editorSource.CacheRoot = editorCache;
    ASSERT_TRUE(reg.RegisterSource(editorSource));

    const fs::path editorAsset = editorAssetsRoot / "UI" / "theme.css";
    ASSERT_TRUE(reg.RegisterAsset(editorAsset));
    const GUID editorGuid = reg.GetAssetGUID(editorAsset);
    ASSERT_FALSE(editorGuid.IsNull());

    // Editor-local KV should work and not go into the project DB.
    EXPECT_TRUE(reg.SetMetaValue(editorAsset, "testKey", "testValue"));

    // Persist project DB and then shutdown (shutdown flushes editor-local DB too).
    ASSERT_TRUE(reg.SaveToFile({}));
    reg.Shutdown();

    // Project DB should not contain editor-mounted assets.
    {
        AssetDatabase::AssetStore_TextJsonl store;
        std::string err;
        ASSERT_TRUE(store.LoadFromFile(projectDb, &err)) << err;

        AssetDatabase::AssetRecord rec{};
        EXPECT_FALSE(store.TryGetAsset(editorGuid, rec));
        EXPECT_FALSE(store.LookupGuidByPath("UI/theme.css").has_value());
    }

    // Editor-local DB should contain the editor asset + kv.
    {
        AssetDatabase::AssetStore_TextJsonl store;
        std::string err;
        ASSERT_TRUE(store.LoadFromFile(editorDb, &err)) << err;

        AssetDatabase::AssetRecord rec{};
        ASSERT_TRUE(store.TryGetAsset(editorGuid, rec));
        EXPECT_EQ(rec.path, "ui/theme.css");
        EXPECT_EQ(store.LookupGuidByPath("ui/theme.css").value_or(GUID::Null()), editorGuid);
        auto it = rec.kv.find("testKey");
        ASSERT_TRUE(it != rec.kv.end());
        EXPECT_EQ(it->second, "testValue");
    }

    // GUID should remain stable across registry restart for the same source alias/path.
    AssetRegistry reg2;
    ASSERT_TRUE(reg2.Initialize(projectAssetsRoot, nullptr, projectDb, projectCache));
    ASSERT_TRUE(reg2.RegisterSource(editorSource));
    ASSERT_TRUE(reg2.RegisterAsset(editorAsset));
    EXPECT_EQ(reg2.GetAssetGUID(editorAsset), editorGuid);
    reg2.Shutdown();

    fs::remove_all(projectRoot, ec);
    fs::remove_all(editorRoot, ec);
}

TEST(AssetRootSeparation, WhenRootsAreSame_EditorMountRegistrationDoesNotCreateEditorScopedKeysInProjectDb)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_root_same");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Assets" / "UI", ec);

    WriteTextFile(root / "Assets" / "UI" / "theme.css", "DUMMY");

    const fs::path assetsRoot = root / "Assets";
    const fs::path dbPath = root / "AssetDatabase.assetdb";
    const fs::path cacheRoot = root / ".Cache" / "AssetDatabase";

    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(assetsRoot, nullptr, dbPath, cacheRoot));

    // Even if an editor mount registration exists, assets under the primary asset root are persistent.
    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = assetsRoot;
    ASSERT_TRUE(reg.RegisterSource(editorSource));

    ASSERT_TRUE(reg.RegisterAsset(assetsRoot / "UI" / "theme.css"));
    const GUID themeGuid = reg.GetAssetGUID(assetsRoot / "UI" / "theme.css");
    ASSERT_FALSE(themeGuid.IsNull());
    ASSERT_TRUE(reg.SaveToFile({}));
    reg.Shutdown();

    AssetDatabase::AssetStore_TextJsonl store;
    std::string err;
    ASSERT_TRUE(store.LoadFromFile(dbPath, &err)) << err;
    EXPECT_FALSE(store.LookupGuidByPath("editor/ui/theme.css").has_value());

    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetRegistry_DerivedSourceStorePathsRemainCanonicalRelativeForFilesystemHydration)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_derived_store_canonical_paths");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Assets" / "UI", ec);
    WriteTextFile(root / "Assets" / "UI" / "layout.uxml", "<ui/>");

    const fs::path sharedRoot = root / "Assets";
    const fs::path projectDb = root / "AssetDatabase.assetdb";
    const fs::path projectCache = root / ".Cache" / "AssetDatabase";
    const fs::path editorDb = root / ".EditorLocal" / "EditorAssetDatabase.assetdb";
    const fs::path editorCache = root / ".EditorLocal" / ".Cache" / "AssetDatabase";
    const fs::path layoutAbs = sharedRoot / "UI" / "layout.uxml";

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = sharedRoot; // same physical root as project
    editorSource.AuthoritativeDbFile = editorDb;
    editorSource.CacheRoot = editorCache;

    GUID guid{};
    {
        AssetRegistry reg;
        ASSERT_TRUE(reg.Initialize(sharedRoot, nullptr, projectDb, projectCache));
        ASSERT_TRUE(reg.RegisterSource(editorSource));
        ASSERT_TRUE(reg.RegisterAsset(layoutAbs, "editor"));
        guid = reg.GetAssetGUID(layoutAbs);
        ASSERT_FALSE(guid.IsNull());
        reg.Shutdown();
    }

    {
        AssetRegistry reg;
        ASSERT_TRUE(reg.Initialize(sharedRoot, nullptr, projectDb, projectCache));
        ASSERT_TRUE(reg.RegisterSource(editorSource));

        AssetMetadata md{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(guid, md));
        EXPECT_EQ(NormalizePathForCompare(md.Path), NormalizePathForCompare(layoutAbs));
        EXPECT_TRUE(fs::exists(md.Path));
        EXPECT_FALSE(fs::exists(sharedRoot / "editor" / "UI" / "layout.uxml"))
            << "Source alias must not be interpreted as a real filesystem segment";

        reg.Shutdown();
    }

    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetManager_AllowsEditorAliasWhenEditorAndProjectRootsMatch)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_asset_manager_same_root_alias");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Assets" / "UI", ec);

    WriteTextFile(root / "Assets" / "UI" / "layout.uxml", "<ui/>");

    const fs::path assetsRoot = root / "Assets";
    const fs::path dbPath = root / "AssetDatabase.assetdb";
    const fs::path cacheRoot = root / ".Cache" / "AssetDatabase";

    AssetManager am;
    ASSERT_TRUE(am.Initialize(assetsRoot, nullptr, dbPath, cacheRoot));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = assetsRoot;

    ASSERT_TRUE(am.RegisterSource(editorSource));
    EXPECT_EQ(NormalizePathForCompare(am.GetSourceRoot("editor")), NormalizePathForCompare(am.GetAssetRoot()));

    const fs::path layoutRel = fs::path("UI") / "layout.uxml";
    const fs::path explicitResolved = am.ResolveAssetPath(layoutRel, "editor");
    EXPECT_EQ(NormalizePathForCompare(explicitResolved), NormalizePathForCompare(am.GetAssetRoot() / layoutRel));

    const GUID layoutGuid = am.ResolveAssetGuid(layoutRel, "editor");
    EXPECT_FALSE(layoutGuid.IsNull());

    AssetMetadata metadata{};
    EXPECT_TRUE(am.GetRegistry().TryGetAssetMetadata(layoutGuid, metadata));
    EXPECT_EQ(NormalizePathForCompare(metadata.Path), NormalizePathForCompare(explicitResolved));

    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetManager_RejectsDuplicateAliasAndRootAndReservedAlias)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_asset_manager_source_dupes");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ProjectAssets", ec);
    fs::create_directories(root / "EditorAssets", ec);
    fs::create_directories(root / "PackageAssets", ec);

    AssetManager am;
    ASSERT_TRUE(am.Initialize(root / "ProjectAssets", nullptr, root / "AssetDatabase.assetdb", root / ".Cache" / "AssetDatabase"));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = root / "EditorAssets";
    ASSERT_TRUE(am.RegisterSource(editorSource));

    AssetSourceDesc dupAlias{};
    dupAlias.Alias = "EDITOR"; // normalized collision
    dupAlias.Root = root / "PackageAssets";
    EXPECT_FALSE(am.RegisterSource(dupAlias));

    // In the unified model, duplicate roots are allowed (priority resolves conflicts).
    AssetSourceDesc dupRoot{};
    dupRoot.Alias = "package";
    dupRoot.Root = root / "EditorAssets";
    EXPECT_TRUE(am.RegisterSource(dupRoot)); // Same root, different alias -- now allowed.

    // "project" is no longer reserved but is already registered (from Initialize).
    AssetSourceDesc reserved{};
    reserved.Alias = "project";
    reserved.Root = root / "PackageAssets";
    EXPECT_FALSE(am.RegisterSource(reserved)); // Duplicate alias -- rejected.

    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetManager_SourceAwareResolution_ImplicitExplicitAndPrefixed)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_asset_manager_source_resolution");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ProjectAssets" / "UI", ec);
    fs::create_directories(root / "EditorAssets" / "UI", ec);

    WriteTextFile(root / "ProjectAssets" / "UI" / "theme.css", "project");
    WriteTextFile(root / "EditorAssets" / "UI" / "theme.css", "editor");

    AssetManager am;
    ASSERT_TRUE(am.Initialize(root / "ProjectAssets", nullptr, root / "AssetDatabase.assetdb", root / ".Cache" / "AssetDatabase"));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "Editor";
    editorSource.Root = root / "EditorAssets";
    ASSERT_TRUE(am.RegisterSource(editorSource));

    const fs::path rel = fs::path("UI") / "theme.css";
    const fs::path projectPath = (root / "ProjectAssets" / rel).lexically_normal();
    const fs::path editorPath = (root / "EditorAssets" / rel).lexically_normal();

    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPath(rel)), NormalizePathForCompare(projectPath));
    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPath(rel, "editor")), NormalizePathForCompare(editorPath));
    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPath("EDITOR:UI/theme.css")), NormalizePathForCompare(editorPath));
    // A slash after the alias is the canonical package URL spelling used by
    // render graphs. On Windows this slash is a root-directory without a
    // root-name; it must not discard the source root and resolve at C:/UI.
    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPath("EDITOR:/UI/theme.css")),
              NormalizePathForCompare(editorPath));
    // The root-strip composes with the Assets/-prefix strip: a rooted alias URL
    // that also spells the Assets folder still lands on the same file.
    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPath("editor:/Assets/UI/theme.css")),
              NormalizePathForCompare(editorPath));
#if defined(_WIN32)
    // Rooted paths without a root-name are not absolute on Windows, so they
    // reach the source join in the explicit-alias overload and the implicit
    // route; both must treat the root portion as source-relative rather than
    // resolving at the drive root. (On POSIX the same spelling IS a genuine
    // absolute path and takes the absolute-passthrough early-return instead —
    // rooted alias references there use the `alias:/path` or CSS `url()` forms,
    // which strip the root before resolution.)
    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPath("/UI/theme.css", "editor")),
              NormalizePathForCompare(editorPath));
    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPath("/UI/theme.css")),
              NormalizePathForCompare(projectPath));
#endif
    // Genuinely absolute paths still pass through both overloads untouched.
    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPath(editorPath)),
              NormalizePathForCompare(editorPath));
    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPath(editorPath, "project")),
              NormalizePathForCompare(editorPath));
    EXPECT_TRUE(am.ResolveAssetPath(rel, "unknown").empty());

    const auto sources = am.GetRegisteredSources();
    // Now includes project + editor (unified model).
    ASSERT_GE(sources.size(), 2u);
    bool foundEditor = false;
    for (const auto& s : sources)
    {
        if (s.Alias == "editor") foundEditor = true;
    }
    EXPECT_TRUE(foundEditor);
    EXPECT_EQ(am.GetSourceRoot("EDITOR").lexically_normal(), (root / "EditorAssets").lexically_normal());

    const GUID projectGuid = am.ResolveAssetGuid(rel);
    const GUID editorGuid = am.ResolveAssetGuid("editor:UI/theme.css");
    EXPECT_FALSE(projectGuid.IsNull());
    EXPECT_FALSE(editorGuid.IsNull());
    EXPECT_NE(projectGuid, editorGuid);
    // The rooted spelling identifies the same asset, not a drive-root phantom.
    EXPECT_EQ(am.ResolveAssetGuid("editor:/UI/theme.css"), editorGuid);

    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetManager_NormalizesAliasAndRejectsInvalidAliasCharacters)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_asset_manager_alias_validation");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ProjectAssets", ec);
    fs::create_directories(root / "EditorAssets", ec);
    fs::create_directories(root / "OtherAssets", ec);

    AssetManager am;
    ASSERT_TRUE(am.Initialize(root / "ProjectAssets", nullptr, root / "AssetDatabase.assetdb", root / ".Cache" / "AssetDatabase"));

    AssetSourceDesc normalized{};
    normalized.Alias = "  Editor  ";
    normalized.Root = root / "EditorAssets";
    ASSERT_TRUE(am.RegisterSource(normalized));
    EXPECT_EQ(am.GetSourceRoot("editor").lexically_normal(), (root / "EditorAssets").lexically_normal());
    EXPECT_TRUE(am.BeginUnregisterSource("  EDITOR  "));

    AssetSourceDesc invalidColon{};
    invalidColon.Alias = "editor:bad";
    invalidColon.Root = root / "EditorAssets";
    EXPECT_FALSE(am.RegisterSource(invalidColon));

    AssetSourceDesc invalidSpace{};
    invalidSpace.Alias = "editor assets";
    invalidSpace.Root = root / "OtherAssets";
    EXPECT_FALSE(am.RegisterSource(invalidSpace));

    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetManager_RelativeRootsWorkWhenCwdIsEditorFolder_AndTeardownIsClean)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_asset_manager_editor_cwd");
    std::error_code ec;
    fs::remove_all(root, ec);

    const fs::path editorRuntime = root / "EditorRuntime";
    fs::create_directories(editorRuntime / "Assets" / "UI", ec);
    WriteTextFile(editorRuntime / "Assets" / "UI" / "layout.uxml", "<ui/>");
    WriteTextFile(editorRuntime / "Assets" / "UI" / "theme.css", "body{}");

    {
        ScopedCurrentPath cwdGuard(editorRuntime);
        ASSERT_TRUE(cwdGuard.IsActive());

        AssetManager am;
        ASSERT_TRUE(am.Initialize("Assets", nullptr, "AssetDatabase.assetdb", ".Cache/AssetDatabase"));

        AssetSourceDesc editorSource{};
        editorSource.Alias = "editor";
        editorSource.Root = "Assets"; // relative root resolved against CWD (= editor runtime)
        ASSERT_TRUE(am.RegisterSource(editorSource));

        // A relative root resolves against the cwd as the OS reports it, which
        // is not always the string handed to chdir: macOS resolves the symlinked
        // $TMPDIR (/var -> /private/var) in current_path(). Build the
        // expectation from the same report the registry resolves against.
        const fs::path expectedAssetsRoot = (fs::current_path() / "Assets").lexically_normal();
        EXPECT_EQ(NormalizePathForCompare(am.GetAssetRoot()), NormalizePathForCompare(expectedAssetsRoot));
        EXPECT_EQ(NormalizePathForCompare(am.GetSourceRoot("EDITOR")), NormalizePathForCompare(expectedAssetsRoot));

        const fs::path layoutRel = fs::path("UI") / "layout.uxml";
        const fs::path explicitResolved = am.ResolveAssetPath(layoutRel, "editor");
        EXPECT_EQ(NormalizePathForCompare(explicitResolved), NormalizePathForCompare(expectedAssetsRoot / layoutRel));
        EXPECT_FALSE(am.ResolveAssetGuid("editor:UI/layout.uxml").IsNull());

        // Ensure explicit source teardown works even when source maps to the same root as project assets.
        EXPECT_TRUE(am.BeginUnregisterSource("EDITOR"));
        EXPECT_TRUE(am.ResolveAssetPath(layoutRel, "editor").empty());

        // Re-register and validate again before shutdown.
        ASSERT_TRUE(am.RegisterSource(editorSource));
        EXPECT_FALSE(am.ResolveAssetGuid(layoutRel, "editor").IsNull());
        am.Shutdown();

        // Post-shutdown operations should fail cleanly.
        EXPECT_FALSE(am.BeginUnregisterSource("editor"));

        // Re-initialize after teardown to validate clean state.
        ASSERT_TRUE(am.Initialize("Assets", nullptr, "AssetDatabase_second.assetdb", ".Cache/AssetDatabase_second"));
        ASSERT_TRUE(am.RegisterSource(editorSource));
        EXPECT_FALSE(am.ResolveAssetGuid("editor:UI/theme.css").IsNull());
        am.Shutdown();
    }

    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetRegistry_SourceApis_NormalizeEnumerateAndUnregister)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_asset_registry_source_api");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ProjectAssets", ec);
    fs::create_directories(root / "EditorAssets", ec);
    fs::create_directories(root / "PackageAssets", ec);

    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(root / "ProjectAssets", nullptr, root / "AssetDatabase.assetdb", root / ".Cache" / "AssetDatabase"));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "  Editor  ";
    editorSource.Root = root / "EditorAssets";
    ASSERT_TRUE(reg.RegisterSource(editorSource));

    auto sources = reg.GetRegisteredSources();
    ASSERT_GE(sources.size(), 2u);
    bool foundEditor = false;
    for (const auto& source : sources)
    {
        if (source.Alias == "editor")
        {
            foundEditor = true;
            EXPECT_EQ(NormalizePathForCompare(source.Root), NormalizePathForCompare(root / "EditorAssets"));
        }
    }
    EXPECT_TRUE(foundEditor);

    AssetSourceDesc dupAlias{};
    dupAlias.Alias = "EDITOR";
    dupAlias.Root = root / "PackageAssets";
    EXPECT_FALSE(reg.RegisterSource(dupAlias));

    // Duplicate roots are now allowed in the unified model.
    AssetSourceDesc dupRoot{};
    dupRoot.Alias = "package";
    dupRoot.Root = root / "EditorAssets";
    EXPECT_TRUE(reg.RegisterSource(dupRoot));

    // "project" is no longer reserved but is already registered (from Initialize).
    AssetSourceDesc reserved{};
    reserved.Alias = "project";
    reserved.Root = root / "PackageAssets";
    EXPECT_FALSE(reg.RegisterSource(reserved)); // Duplicate alias.

    AssetSourceDesc invalid{};
    invalid.Alias = "invalid alias";
    invalid.Root = root / "PackageAssets";
    EXPECT_FALSE(reg.RegisterSource(invalid));

    EXPECT_FALSE(reg.UnregisterSource("unknown"));
    EXPECT_TRUE(reg.UnregisterSource("EDITOR"));
    // After unregistering editor, package + project sources remain.
    EXPECT_FALSE(reg.GetRegisteredSources().empty());

    EXPECT_TRUE(reg.UnregisterSource("package"));
    EXPECT_EQ(reg.GetRegisteredSources().size(), 1u); // project

    // Ensure unregister truly clears the alias for reuse.
    ASSERT_TRUE(reg.RegisterSource(editorSource));
    EXPECT_EQ(reg.GetRegisteredSources().size(), 2u); // project + editor

    reg.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, TextureCookPolicySurvivesSourceProjectionAndRebind)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_texture_source_policy");
    const GUID bakedGuid = GUID::Generate();
    const std::string manifest = "{\"guid\":\"" + bakedGuid.ToString() +
        "\",\"path\":\"tile.png\",\"type\":\"Texture\"}\n";
    WriteTextFile(root / "ProjectAssets" / "tile.png", "source");
    for (const char* directory : {"PackageAssets", "MovedAssets"})
    {
        WriteTextFile(root / directory / "tile.png", "source");
        WriteTextFile(root / directory / ".assetmanifest", manifest);
    }

    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(root / "ProjectAssets", nullptr,
        root / "AssetDatabase.assetdb", root / ".Cache" / "AssetDatabase"));
    ASSERT_TRUE(reg.RegisterAsset(root / "ProjectAssets" / "tile.png"));
    const DerivedArtifactPolicy authoredPolicy =
        reg.GetDerivedArtifactPolicy(reg.GetAssetGUID(root / "ProjectAssets" / "tile.png"));
    EXPECT_TRUE(authoredPolicy.InfersTextureImportSettings);
    EXPECT_TRUE(authoredPolicy.CooksOnMiss);
    const DerivedArtifactPolicy unknownPolicy = reg.GetDerivedArtifactPolicy(GUID{});
    EXPECT_FALSE(unknownPolicy.InfersTextureImportSettings);
    EXPECT_FALSE(unknownPolicy.CooksOnMiss);

    ASSERT_TRUE(reg.RegisterSource(MakePackageMount("staged", root / "PackageAssets")));
    EXPECT_FALSE(reg.GetDerivedArtifactPolicy(bakedGuid).CooksOnMiss);
    const auto sources = reg.GetRegisteredSources();
    const auto staged = std::find_if(sources.begin(), sources.end(),
        [](const AssetSourceDesc& desc) { return desc.Alias == "staged"; });
    ASSERT_NE(staged, sources.end());
    EXPECT_FALSE(staged->InfersTextureImportSettings);
    EXPECT_FALSE(staged->CooksDerivedArtifactsOnMiss);

    // Rebind changes the location, preserving the original source's policy
    // even when the replacement descriptor has development defaults.
    AssetSourceDesc replacement;
    replacement.Root = root / "MovedAssets";
    replacement.DerivedIdentity = false;
    replacement.AuthoritativeDbFile = replacement.Root / ".assetmanifest";
    ASSERT_TRUE(reg.RebindSource("staged", replacement));
    const DerivedArtifactPolicy reboundPolicy = reg.GetDerivedArtifactPolicy(bakedGuid);
    EXPECT_FALSE(reboundPolicy.InfersTextureImportSettings);
    EXPECT_FALSE(reboundPolicy.CooksOnMiss);
    AssetMetadata rebound;
    ASSERT_TRUE(reg.TryGetAssetMetadata(bakedGuid, rebound));
    EXPECT_EQ(NormalizePathForCompare(rebound.Path),
        NormalizePathForCompare(replacement.Root / "tile.png"));
    ASSERT_TRUE(reg.UnregisterSource("staged"));
    EXPECT_FALSE(reg.GetDerivedArtifactPolicy(bakedGuid).CooksOnMiss);
    reg.Shutdown();
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, TextureCookPolicyUsesExplicitOwnerAcrossOverlappingRoots)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_texture_owner_policy");
    const fs::path asset = root / "SharedAssets" / "tile.png";
    WriteTextFile(asset, "source");
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(root / "ProjectAssets", nullptr,
        root / "AssetDatabase.assetdb", root / ".Cache" / "AssetDatabase"));

    AssetSourceDesc owner;
    owner.Alias = "authoring";
    owner.Root = asset.parent_path();
    owner.Priority = 10;
    owner.AuthoritativeDbFile = root / "authoring.assetdb";
    owner.CacheRoot = root / "authoring-cache" / "AssetDatabase";
    ASSERT_TRUE(reg.RegisterSource(owner));
    AssetSourceDesc overlapping = owner;
    overlapping.Alias = "baked";
    overlapping.Priority = 20;
    overlapping.InfersTextureImportSettings = false;
    overlapping.CooksDerivedArtifactsOnMiss = false;
    overlapping.AuthoritativeDbFile = root / "baked.assetdb";
    overlapping.CacheRoot = root / "baked-cache" / "AssetDatabase";
    ASSERT_TRUE(reg.RegisterSource(overlapping));

    ASSERT_TRUE(reg.RegisterAsset(asset, "authoring"));
    const GUID authored = reg.GetAssetGUID(asset);
    const DerivedArtifactPolicy ownedByAuthoring = reg.GetDerivedArtifactPolicy(authored);
    EXPECT_TRUE(ownedByAuthoring.InfersTextureImportSettings);
    EXPECT_TRUE(ownedByAuthoring.CooksOnMiss);
    EXPECT_EQ(reg.TryGetCacheRoot(authored), root / "authoring-cache");
    EXPECT_EQ(reg.TryGetCacheRoot(asset), root / "authoring-cache");
    ASSERT_TRUE(reg.RegisterAsset(asset, "baked"));
    const GUID baked = reg.GetAssetGUID(asset);
    EXPECT_NE(authored, baked);
    EXPECT_FALSE(reg.GetDerivedArtifactPolicy(authored).CooksOnMiss);
    EXPECT_FALSE(reg.GetDerivedArtifactPolicy(baked).CooksOnMiss);
    EXPECT_FALSE(reg.GetDerivedArtifactPolicy(baked).InfersTextureImportSettings);
    EXPECT_EQ(reg.TryGetCacheRoot(baked), root / "baked-cache");
    EXPECT_EQ(reg.TryGetCacheRoot(asset), root / "baked-cache");
    reg.Shutdown();
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetManager_LoadAssetPathApis_ResolveExpectedGuidsAndErrorPaths)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_asset_manager_load_path_api");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ProjectAssets" / "UI", ec);
    fs::create_directories(root / "EditorAssets" / "UI", ec);

    WriteTextFile(root / "EditorAssets" / "UI" / "layout.uxml", "<ui/>");

    AssetManager am;
    ASSERT_TRUE(am.Initialize(root / "ProjectAssets", nullptr, root / "AssetDatabase.assetdb", root / ".Cache" / "AssetDatabase"));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = root / "EditorAssets";
    ASSERT_TRUE(am.RegisterSource(editorSource));

    const fs::path layoutRel = fs::path("UI") / "layout.uxml";
    const GUID explicitGuid = am.ResolveAssetGuid(layoutRel, "editor");
    ASSERT_FALSE(explicitGuid.IsNull());

    AssetLoadHandle explicitHandle = am.LoadAsset(layoutRel, "editor", {}, AssetLoadPriority::Normal);
    EXPECT_EQ(explicitHandle.AssetGuid, explicitGuid);
    EXPECT_TRUE(explicitHandle.Future.has_value());

    AssetLoadHandle prefixedHandle = am.LoadAsset(fs::path("editor:UI/layout.uxml"), {}, AssetLoadPriority::Normal);
    EXPECT_EQ(prefixedHandle.AssetGuid, explicitGuid);
    EXPECT_TRUE(prefixedHandle.Future.has_value());

    bool unknownAliasCallbackCalled = false;
    AssetError unknownAliasErrorCode = AssetError::ImportFailed;
    AssetLoadHandle unknownAliasHandle = am.LoadAsset(
        layoutRel,
        "missing",
        [&](Result<SharedPtr<Asset>, AssetError> r)
        {
            unknownAliasCallbackCalled = true;
            EXPECT_FALSE(r.IsOk());
            if (!r.IsOk())
                unknownAliasErrorCode = r.Error();
        },
        AssetLoadPriority::Normal);
    EXPECT_TRUE(unknownAliasHandle.AssetGuid.IsNull());
    EXPECT_TRUE(unknownAliasCallbackCalled);
    EXPECT_EQ(unknownAliasErrorCode, AssetError::Missing);

    bool missingPathCallbackCalled = false;
    AssetError missingPathErrorCode = AssetError::ImportFailed;
    AssetLoadHandle missingPathHandle = am.LoadAsset(
        fs::path("UI") / "missing.uxml",
        "editor",
        [&](Result<SharedPtr<Asset>, AssetError> r)
        {
            missingPathCallbackCalled = true;
            EXPECT_FALSE(r.IsOk());
            if (!r.IsOk())
                missingPathErrorCode = r.Error();
        },
        AssetLoadPriority::Normal);
    EXPECT_TRUE(missingPathHandle.AssetGuid.IsNull());
    EXPECT_TRUE(missingPathCallbackCalled);
    EXPECT_EQ(missingPathErrorCode, AssetError::Missing);

    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetManager_SourceAliasInference_PrefersImporterNamespace)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_asset_manager_alias_inference");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ProjectAssets" / "UI", ec);
    fs::create_directories(root / "EditorAssets" / "UI", ec);
    WriteTextFile(root / "ProjectAssets" / "UI" / "theme.css", "project");
    WriteTextFile(root / "EditorAssets" / "UI" / "theme.css", "editor");

    AssetManager am;
    ASSERT_TRUE(am.Initialize(root / "ProjectAssets", nullptr, root / "AssetDatabase.assetdb", root / ".Cache" / "AssetDatabase"));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = root / "EditorAssets";
    ASSERT_TRUE(am.RegisterSource(editorSource));

    EXPECT_EQ(am.GetSourceAliasForPath(root / "EditorAssets" / "UI" / "theme.css"), "editor");
    EXPECT_EQ(am.GetSourceAliasForPath(root / "ProjectAssets" / "UI" / "theme.css"), "project");
    EXPECT_TRUE(am.GetSourceAliasForPath(root / "UnknownAssets" / "UI" / "theme.css").empty());

    // Same-root alias mapping: in the unified priority model, the highest-priority source
    // that matches the path wins. Project (priority 100) wins over editor (priority 0).
    EXPECT_TRUE(am.BeginUnregisterSource("editor"));
    editorSource.Root = root / "ProjectAssets";
    ASSERT_TRUE(am.RegisterSource(editorSource));
    // Project has higher priority, so it wins for same-root paths.
    EXPECT_EQ(am.GetSourceAliasForPath(root / "ProjectAssets" / "UI" / "theme.css"), "project");

    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetManager_ResolveAssetPathFromReference_PrefersReferrerSourceAlias)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_asset_reference_source_priority");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ProjectAssets" / "UI" / "theme", ec);
    fs::create_directories(root / "EditorAssets" / "UI" / "theme", ec);

    // Same relative path exists in both sources.
    WriteTextFile(root / "ProjectAssets" / "UI" / "theme" / "tokens.css", "project\n");
    WriteTextFile(root / "EditorAssets" / "UI" / "theme" / "tokens.css", "editor\n");
    WriteTextFile(root / "EditorAssets" / "UI" / "theme.css", "@import \"UI/theme/tokens.css\";\n");

    AssetManager am;
    ASSERT_TRUE(am.Initialize(root / "ProjectAssets", nullptr, root / "AssetDatabase.assetdb", root / ".Cache" / "AssetDatabase"));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = root / "EditorAssets";
    ASSERT_TRUE(am.RegisterSource(editorSource));

    const fs::path tokensRel = fs::path("UI") / "theme" / "tokens.css";
    const fs::path importerAbs = root / "EditorAssets" / "UI" / "theme.css";
    const fs::path resolvedFromRef = am.ResolveAssetPathFromReference(tokensRel, importerAbs);
    const fs::path resolvedImplicit = am.ResolveAssetPath(tokensRel);
    const fs::path expectedEditor = (root / "EditorAssets" / tokensRel).lexically_normal();
    const fs::path expectedProject = (root / "ProjectAssets" / tokensRel).lexically_normal();

    EXPECT_EQ(NormalizePathForCompare(resolvedFromRef), NormalizePathForCompare(expectedEditor));
    EXPECT_EQ(NormalizePathForCompare(resolvedImplicit), NormalizePathForCompare(expectedProject));

    am.Shutdown();
    fs::remove_all(root, ec);
}

// The three-way contract of the prefer-a-mount resolve: the preferred source
// wins only when it actually has the asset, and both other outcomes — source
// registered but lacking it, source not registered at all — fall through to
// implicit priority instead of failing.
TEST(AssetRootSeparation, AssetManager_ResolveAssetPathPreferringSource_PrefersMountThenFallsThrough)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_prefer_source_contract");
    std::error_code ec;
    fs::create_directories(root / "ProjectAssets" / "Shaders", ec);
    fs::create_directories(root / "EditorAssets" / "Shaders", ec);

    WriteTextFile(root / "ProjectAssets" / "Shaders" / "blit.glsl", "project\n");
    WriteTextFile(root / "EditorAssets" / "Shaders" / "blit.glsl", "editor\n");
    WriteTextFile(root / "ProjectAssets" / "Shaders" / "project_only.glsl", "project\n");

    AssetManager am;
    ASSERT_TRUE(am.Initialize(root / "ProjectAssets", nullptr, root / "AssetDatabase.assetdb",
                              root / ".Cache" / "AssetDatabase"));

    const fs::path blitRel = fs::path("Shaders") / "blit.glsl";
    const fs::path projectOnlyRel = fs::path("Shaders") / "project_only.glsl";

    // Preferred source not registered: implicit priority, not a failure.
    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPathPreferringSource(blitRel, kAssetSourceAliasEditor,
                                                                          AssetPathKind::AnyEntry)),
              NormalizePathForCompare((root / "ProjectAssets" / blitRel).lexically_normal()));

    AssetSourceDesc editorSource{};
    editorSource.Alias = std::string(kAssetSourceAliasEditor);
    editorSource.Root = root / "EditorAssets";
    ASSERT_TRUE(am.RegisterSource(editorSource));

    // Registered and has it: the preferred mount beats the project copy.
    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPathPreferringSource(blitRel, kAssetSourceAliasEditor,
                                                                          AssetPathKind::AnyEntry)),
              NormalizePathForCompare((root / "EditorAssets" / blitRel).lexically_normal()));

    // Registered but lacks it: falls through instead of returning the
    // non-existent editor-mount candidate.
    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPathPreferringSource(projectOnlyRel, kAssetSourceAliasEditor,
                                                                          AssetPathKind::AnyEntry)),
              NormalizePathForCompare((root / "ProjectAssets" / projectOnlyRel).lexically_normal()));

    am.Shutdown();
    fs::remove_all(root, ec);
}

// AnyEntry vs Directory is load-bearing, not decoration: an adapter-shader root
// gets walked into, so a same-named FILE in the preferred mount must fall
// through. The AnyEntry spelling deliberately accepts it — its callers resolve
// files.
TEST(AssetRootSeparation, AssetManager_ResolveAssetPathPreferringSource_DirectoryKindRejectsSameNamedFile)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_prefer_source_dirkind");
    std::error_code ec;
    fs::create_directories(root / "ProjectAssets" / "Shaders", ec);
    fs::create_directories(root / "EditorAssets", ec);
    WriteTextFile(root / "ProjectAssets" / "Shaders" / "adapter.glsl", "project\n");
    // A FILE exactly where the editor mount would carry its Shaders/ tree.
    WriteTextFile(root / "EditorAssets" / "Shaders", "not a directory\n");

    AssetManager am;
    ASSERT_TRUE(am.Initialize(root / "ProjectAssets", nullptr, root / "AssetDatabase.assetdb",
                              root / ".Cache" / "AssetDatabase"));
    AssetSourceDesc editorSource{};
    editorSource.Alias = std::string(kAssetSourceAliasEditor);
    editorSource.Root = root / "EditorAssets";
    ASSERT_TRUE(am.RegisterSource(editorSource));

    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPathPreferringSource("Shaders", kAssetSourceAliasEditor,
                                                                          AssetPathKind::Directory)),
              NormalizePathForCompare((root / "ProjectAssets" / "Shaders").lexically_normal()));
    EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPathPreferringSource("Shaders", kAssetSourceAliasEditor,
                                                                          AssetPathKind::AnyEntry)),
              NormalizePathForCompare((root / "EditorAssets" / "Shaders").lexically_normal()));

    am.Shutdown();
    fs::remove_all(root, ec);
}

// An absent preferred mount is a deployment shape, not an authoring mistake: a
// shipped game registers no 'editor' source and the engine-shader resolver asks
// for it on a per-frame path. The explicit overload must keep warning, because
// there the alias was authored.
TEST(AssetRootSeparation, AssetManager_ResolveAssetPathPreferringSource_AbsentSourceSilentExplicitStillWarns)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_prefer_source_silent");
    std::error_code ec;
    fs::create_directories(root / "ProjectAssets" / "Shaders", ec);
    WriteTextFile(root / "ProjectAssets" / "Shaders" / "blit.glsl", "project\n");

    AssetManager am;
    ASSERT_TRUE(am.Initialize(root / "ProjectAssets", nullptr, root / "AssetDatabase.assetdb",
                              root / ".Cache" / "AssetDatabase"));
    // The packaged shape: no 'editor' source at all.
    ASSERT_TRUE(am.GetSourceRoot(kAssetSourceAliasEditor).empty());

    std::vector<std::string> logLines;
    // Engine is a SHARED library: AssetManager logs through Engine.dll's Logger
    // state, not this exe's copy. Adopt it, or the capture reads zero for the
    // wrong reason.
    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    Logger::Log::Initialize({});
    Logger::Log::AddSink(std::make_unique<CapturingLogSink>(&logLines));

    const auto unknownAliasLines = [&logLines]()
    {
        return std::count_if(logLines.begin(), logLines.end(), [](const std::string& l)
                             { return l.find("unknown asset source alias 'editor'") != std::string::npos; });
    };

    const fs::path blitRel = fs::path("Shaders") / "blit.glsl";
    for (int i = 0; i < 4; ++i) // Repeat: this sits on a per-frame path.
    {
        EXPECT_EQ(NormalizePathForCompare(am.ResolveAssetPathPreferringSource(blitRel, kAssetSourceAliasEditor,
                                                                              AssetPathKind::AnyEntry)),
                  NormalizePathForCompare((root / "ProjectAssets" / blitRel).lexically_normal()));
    }
    Logger::Log::Flush();
    EXPECT_EQ(unknownAliasLines(), 0) << "an absent preferred mount must not be reported as an error";

    // Same alias, same manager, authored spelling: this one must report. The
    // warning is once-per-alias, so seeing it here also proves the loop above
    // did not quietly consume the one-shot.
    EXPECT_TRUE(am.ResolveAssetPath(blitRel, kAssetSourceAliasEditor).empty());
    Logger::Log::Flush();
    EXPECT_EQ(unknownAliasLines(), 1) << "an authored unknown alias must still be reported";

    Logger::Log::ClearSinks();
    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetManager_ResolveAssetGuidFromReference_PreservesImporterOwnershipAcrossProjectRebind)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_asset_reference_guid_rebind");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "SharedAssets" / "UI" / "theme", ec);
    fs::create_directories(root / "NewProjectAssets", ec);

    WriteTextFile(root / "SharedAssets" / "UI" / "theme.css", "@import \"UI/theme/tokens.css\";\n");
    WriteTextFile(root / "SharedAssets" / "UI" / "theme" / "tokens.css", ":root { --ui_color_text: #E5E5E5; }\n");

    const fs::path sharedRoot = root / "SharedAssets";
    AssetManager am;
    ASSERT_TRUE(am.Initialize(sharedRoot, nullptr, root / "AssetDatabase.assetdb", root / ".Cache" / "AssetDatabase"));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = sharedRoot; // intentional overlap with project source
    ASSERT_TRUE(am.RegisterSource(editorSource));

    const fs::path importerRel = fs::path("UI") / "theme.css";
    const fs::path importerAbs = (sharedRoot / importerRel).lexically_normal();
    const fs::path importedRel = fs::path("UI") / "theme" / "tokens.css";

    // Baseline project identity for the imported leaf under overlap.
    const GUID projectLeafGuid = am.ResolveAssetGuid(importedRel, "project");
    ASSERT_FALSE(projectLeafGuid.IsNull());

    // Explicitly claim importer as editor source and then resolve imported dependency
    // through referrer context. This must produce editor-stable identity.
    const GUID importerGuid = am.ResolveAssetGuid(importerRel, "editor");
    ASSERT_FALSE(importerGuid.IsNull());
    EXPECT_EQ(am.GetRegistry().GetAssetSourceOwnerAlias(importerAbs), "editor");

    const GUID importedGuidBefore = am.ResolveAssetGuidFromReference(importedRel, importerAbs);
    ASSERT_FALSE(importedGuidBefore.IsNull());
    EXPECT_NE(importedGuidBefore, projectLeafGuid)
        << "Imported leaf should be remapped into importer source namespace";

    AssetManager::SourceRebindDesc rebindDesc;
    rebindDesc.NewRoot = root / "NewProjectAssets";
    rebindDesc.AuthoritativeDbFile = root / "NewProjectAssets" / "AssetDatabase.assetdb";
    rebindDesc.CacheRoot = root / "NewProjectAssets" / ".Cache" / "AssetDatabase";
    ASSERT_TRUE(am.BeginRebindSource("project", rebindDesc));

    // Importer + imported dependency must survive project rebind with stable GUIDs.
    EXPECT_TRUE(am.GetRegistry().IsAssetRegistered(importerGuid));
    EXPECT_TRUE(am.GetRegistry().IsAssetRegistered(importedGuidBefore));

    const GUID importedGuidAfter = am.ResolveAssetGuidFromReference(importedRel, importerAbs);
    EXPECT_EQ(importedGuidBefore, importedGuidAfter)
        << "Imported dependency GUID must remain stable across project rebind";

    am.Shutdown();
    fs::remove_all(root, ec);
}

// ==========================================================================
// Mount-Preserving Rebind Tests
// ==========================================================================

TEST(AssetRootSeparation, AssetManager_RebindProjectSource_PreservesEditorSource)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_rebind_preserves_editor");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ProjectA" / "Assets" / "Textures", ec);
    fs::create_directories(root / "ProjectB" / "Assets" / "Textures", ec);
    fs::create_directories(root / "EditorAssets" / "UI", ec);
    WriteTextFile(root / "ProjectA" / "Assets" / "Textures" / "hero.png", "project-a");
    WriteTextFile(root / "ProjectB" / "Assets" / "Textures" / "hero.png", "project-b");
    WriteTextFile(root / "EditorAssets" / "UI" / "theme.css", "editor-theme");

    AssetManager am;
    ASSERT_TRUE(am.Initialize(root / "ProjectA" / "Assets", nullptr,
                              root / "ProjectA" / "AssetDatabase.assetdb",
                              root / "ProjectA" / ".Cache" / "AssetDatabase"));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = root / "EditorAssets";
    ASSERT_TRUE(am.RegisterSource(editorSource));

    // Resolve editor GUID before rebind.
    const GUID editorGuidBefore = am.ResolveAssetGuid(fs::path("UI") / "theme.css", "editor");
    ASSERT_FALSE(editorGuidBefore.IsNull());

    // Resolve a project asset GUID.
    const GUID projectGuidBefore = am.ResolveAssetGuid(fs::path("Textures") / "hero.png", "project");
    ASSERT_FALSE(projectGuidBefore.IsNull());

    // Rebind project to ProjectB.
    AssetManager::SourceRebindDesc rebindDesc;
    rebindDesc.NewRoot = root / "ProjectB" / "Assets";
    rebindDesc.AuthoritativeDbFile = root / "ProjectB" / "AssetDatabase.assetdb";
    rebindDesc.CacheRoot = root / "ProjectB" / ".Cache" / "AssetDatabase";
    ASSERT_TRUE(am.BeginRebindSource("project", rebindDesc));

    // Editor GUID must be unchanged after rebind.
    const GUID editorGuidAfter = am.ResolveAssetGuid(fs::path("UI") / "theme.css", "editor");
    EXPECT_EQ(editorGuidBefore, editorGuidAfter) << "Editor GUID must be stable across project rebind";

    // Editor source must still be registered.
    const auto sources = am.GetRegisteredSources();
    bool foundEditor = false;
    for (const auto& s : sources)
    {
        if (s.Alias == "editor")
            foundEditor = true;
    }
    EXPECT_TRUE(foundEditor) << "Editor source must survive project rebind";

    // New asset root should point to ProjectB.
    const std::string newRoot = NormalizePathForCompare(am.GetAssetRoot());
    const std::string expectedRoot = NormalizePathForCompare(root / "ProjectB" / "Assets");
    EXPECT_EQ(newRoot, expectedRoot);

    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetManager_RebindProjectSource_EjectsProjectAssets)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_rebind_ejects_project");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ProjectA" / "Assets" / "Textures", ec);
    fs::create_directories(root / "ProjectB" / "Assets", ec);
    WriteTextFile(root / "ProjectA" / "Assets" / "Textures" / "hero.png", "project-a");

    AssetManager am;
    ASSERT_TRUE(am.Initialize(root / "ProjectA" / "Assets", nullptr,
                              root / "ProjectA" / "AssetDatabase.assetdb",
                              root / "ProjectA" / ".Cache" / "AssetDatabase"));

    // Resolve a project asset GUID.
    const GUID projectGuid = am.ResolveAssetGuid(fs::path("Textures") / "hero.png", "project");
    ASSERT_FALSE(projectGuid.IsNull());

    // The asset should be registered.
    EXPECT_TRUE(am.GetRegistry().IsAssetRegistered(projectGuid));

    // Rebind project to ProjectB (which doesn't have hero.png).
    AssetManager::SourceRebindDesc rebindDesc;
    rebindDesc.NewRoot = root / "ProjectB" / "Assets";
    rebindDesc.AuthoritativeDbFile = root / "ProjectB" / "AssetDatabase.assetdb";
    rebindDesc.CacheRoot = root / "ProjectB" / ".Cache" / "AssetDatabase";
    ASSERT_TRUE(am.BeginRebindSource("project", rebindDesc));

    // The old project GUID should no longer be registered (ejected).
    EXPECT_FALSE(am.GetRegistry().IsAssetRegistered(projectGuid))
        << "Project assets should be ejected after rebind";

    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetManager_TwoStepInit_Works)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_two_step_init");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Assets" / "UI", ec);
    WriteTextFile(root / "Assets" / "UI" / "test.css", "body {}");

    AssetManager am;
    // Step 1: infrastructure only.
    ASSERT_TRUE(am.Initialize(static_cast<JobSystem::WorkStealingThreadPool*>(nullptr)));

    // Step 2: register project source.
    AssetSourceDesc projectDesc{};
    projectDesc.Alias = "project";
    projectDesc.Root = root / "Assets";
    projectDesc.DerivedIdentity = false;
    projectDesc.AuthoritativeDbFile = root / "AssetDatabase.assetdb";
    projectDesc.CacheRoot = root / ".Cache" / "AssetDatabase";
    projectDesc.Priority = 100;
    ASSERT_TRUE(am.RegisterSource(projectDesc));

    // Verify project root is set.
    EXPECT_EQ(NormalizePathForCompare(am.GetAssetRoot()), NormalizePathForCompare(root / "Assets"));

    // Resolve an asset.
    const GUID guid = am.ResolveAssetGuid(fs::path("UI") / "test.css", "project");
    EXPECT_FALSE(guid.IsNull());

    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetManager_UnregisterSource_EjectsLoadedAssets)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_unregister_ejects");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ProjectAssets" / "UI", ec);
    fs::create_directories(root / "EditorAssets" / "UI", ec);
    WriteTextFile(root / "ProjectAssets" / "UI" / "test.css", "project");
    WriteTextFile(root / "EditorAssets" / "UI" / "test.css", "editor");

    AssetManager am;
    ASSERT_TRUE(am.Initialize(root / "ProjectAssets", nullptr,
                              root / "AssetDatabase.assetdb",
                              root / ".Cache" / "AssetDatabase"));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = root / "EditorAssets";
    ASSERT_TRUE(am.RegisterSource(editorSource));

    // Resolve editor asset.
    const GUID editorGuid = am.ResolveAssetGuid(fs::path("UI") / "test.css", "editor");
    ASSERT_FALSE(editorGuid.IsNull());
    EXPECT_TRUE(am.GetRegistry().IsAssetRegistered(editorGuid));

    // Unregister editor source.
    ASSERT_TRUE(am.BeginUnregisterSource("editor"));

    // Editor asset should be ejected from registry.
    EXPECT_FALSE(am.GetRegistry().IsAssetRegistered(editorGuid))
        << "Assets should be ejected when their source is unregistered";

    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetRegistry_PriorityBasedResolution)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_priority_resolution");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "HighPriority" / "UI", ec);
    fs::create_directories(root / "LowPriority" / "UI", ec);
    WriteTextFile(root / "HighPriority" / "UI" / "theme.css", "high");
    WriteTextFile(root / "LowPriority" / "UI" / "theme.css", "low");

    AssetManager am;
    ASSERT_TRUE(am.Initialize(static_cast<JobSystem::WorkStealingThreadPool*>(nullptr)));

    // Register low-priority source first.
    AssetSourceDesc lowSource{};
    lowSource.Alias = "low";
    lowSource.Root = root / "LowPriority";
    lowSource.Priority = 10;
    ASSERT_TRUE(am.RegisterSource(lowSource));

    // Register high-priority source second.
    AssetSourceDesc highSource{};
    highSource.Alias = "high";
    highSource.Root = root / "HighPriority";
    highSource.Priority = 50;
    ASSERT_TRUE(am.RegisterSource(highSource));

    // Implicit resolution should prefer high-priority source.
    const fs::path resolved = am.ResolveAssetPath(fs::path("UI") / "theme.css");
    const std::string expected = NormalizePathForCompare(root / "HighPriority" / "UI" / "theme.css");
    EXPECT_EQ(NormalizePathForCompare(resolved), expected)
        << "Higher-priority source should win in implicit resolution";

    am.Shutdown();
    fs::remove_all(root, ec);
}

// A path reference in a dependency resolves the way the runtime loads it: the
// project root, then the referrer's own folder, then the other mounts. The two
// cases below pin both ends of that order.
namespace
{
bool ContainsGuid(const Vector<GUID>& guids, const GUID& guid)
{
    return std::find(guids.begin(), guids.end(), guid) != guids.end();
}

// A project asset root plus an editor mount, both scanned.
void MountProjectAndEditor(AssetManager& am, const std::filesystem::path& root)
{
    ASSERT_TRUE(am.Initialize(root / "ProjectAssets", nullptr, root / "AssetDatabase.assetdb",
                              root / ".Cache" / "AssetDatabase"));
    AssetSourceDesc editorSource{};
    editorSource.Alias = std::string(kAssetSourceAliasEditor);
    editorSource.Root = root / "EditorAssets";
    editorSource.RegisterFileWatcher = false;
    ASSERT_TRUE(am.RegisterSource(editorSource));
    am.WaitForStartupScan(std::string(kAssetSourceAliasEditor));
    am.WaitForStartupScan();
}
} // namespace

TEST(AssetRootSeparation, AssetRegistry_GltfImageUriEdgeIsItsSiblingWhenAnotherMountHasTheName)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_gltf_sibling_edge");
    std::error_code ec;
    fs::remove_all(root, ec);
    // A glTF image URI is relative to the .gltf; the editor mount's root holds an
    // unrelated file of the same name.
    WriteTextFile(root / "ProjectAssets" / "Models" / "Crate" / "crate.gltf",
                  R"({ "asset": { "version": "2.0" }, "images": [ { "uri": "crate.png" } ] })");
    WriteTextFile(root / "ProjectAssets" / "Models" / "Crate" / "crate.png", "SIBLING");
    WriteTextFile(root / "EditorAssets" / "crate.png", "EDITOR");

    AssetManager am;
    MountProjectAndEditor(am, root);
    AssetRegistry& reg = am.GetRegistry();
    const GUID model = reg.GetAssetGUID(root / "ProjectAssets" / "Models" / "Crate" / "crate.gltf");
    const GUID sibling = reg.GetAssetGUID(root / "ProjectAssets" / "Models" / "Crate" / "crate.png");
    const GUID editorFile = reg.GetAssetGUID(root / "EditorAssets" / "crate.png");
    ASSERT_FALSE(model.IsNull());
    ASSERT_FALSE(sibling.IsNull());
    ASSERT_FALSE(editorFile.IsNull());

    const Vector<GUID> deps = reg.RefreshDependencies(model);
    EXPECT_TRUE(ContainsGuid(deps, sibling)) << "the glTF's own image is not its dependency";
    EXPECT_FALSE(ContainsGuid(deps, editorFile)) << "the dependency went to another mount's file of the same name";

    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetRegistry_ProjectSceneNamingAnEditorMaterialByPathDependsOnIt)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_scene_editor_material_edge");
    std::error_code ec;
    fs::remove_all(root, ec);
    WriteTextFile(root / "EditorAssets" / "Materials" / "Ed.material", R"({ "materialName": "Ed" })");
    WriteTextFile(root / "ProjectAssets" / "Scenes" / "main.scene",
                  "[scene name=\"main\" version=1]\n\n[entity id=\"e\"]\nMeshRenderer.meshPrimitive = Plane\n"
                  "MeshRenderer.material = [path=\"Materials/Ed.material\"]\n");

    AssetManager am;
    MountProjectAndEditor(am, root);
    AssetRegistry& reg = am.GetRegistry();
    const GUID scene = reg.GetAssetGUID(root / "ProjectAssets" / "Scenes" / "main.scene");
    const GUID material = reg.GetAssetGUID(root / "EditorAssets" / "Materials" / "Ed.material");
    ASSERT_FALSE(scene.IsNull());
    ASSERT_FALSE(material.IsNull());

    EXPECT_TRUE(ContainsGuid(reg.RefreshDependencies(scene), material))
        << "the scene does not depend on the editor-mount material it names by path";

    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, AssetManager_ProjectRebind_PreservesEditorHotReloadContinuity)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_rebind_editor_hotreload");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ProjectA" / "Assets" / "Textures", ec);
    fs::create_directories(root / "ProjectB" / "Assets", ec);
    fs::create_directories(root / "EditorAssets" / "UI", ec);
    WriteTextFile(root / "ProjectA" / "Assets" / "Textures" / "hero.png", "project-a");
    WriteTextFile(root / "EditorAssets" / "UI" / "theme.css", "v1");

    AssetManager am;
    ASSERT_TRUE(am.Initialize(root / "ProjectA" / "Assets", nullptr,
                              root / "ProjectA" / "AssetDatabase.assetdb",
                              root / "ProjectA" / ".Cache" / "AssetDatabase"));
    am.SetHotReloadEnabled(true);

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = root / "EditorAssets";
    ASSERT_TRUE(am.RegisterSource(editorSource));

    const fs::path editorRel = fs::path("UI") / "theme.css";
    const fs::path editorAbs = root / "EditorAssets" / editorRel;
    const GUID editorGuid = am.ResolveAssetGuid(editorRel, "editor");
    ASSERT_FALSE(editorGuid.IsNull());

    auto testAsset = std::make_shared<TestReloadableAsset>(editorGuid, editorAbs);
    ASSERT_TRUE(testAsset->Load()); // establish baseline timestamp for NeedsReload()
    am.RegisterLoadedAsset(editorGuid, testAsset);
    ASSERT_TRUE(am.IsAssetLoaded(editorGuid));
    ASSERT_EQ(testAsset->LoadCount(), 1);

    // Pre-rebind edit should still hot-reload editor-mounted assets. The sleep
    // is filesystem-timestamp granularity, not a readiness wait: the next
    // write's mtime must be strictly newer than the baseline Load() recorded,
    // or NeedsReload() sees no change. Sleeping longer is always safe.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    WriteTextFile(editorAbs, "v2");
    ASSERT_TRUE(WaitUntil([&]() {
        am.Update();
        return testAsset->LoadCount() >= 2;
    })) << "Expected editor asset to hot-reload before project rebind";

    AssetManager::SourceRebindDesc rebindDesc;
    rebindDesc.NewRoot = root / "ProjectB" / "Assets";
    rebindDesc.AuthoritativeDbFile = root / "ProjectB" / "AssetDatabase.assetdb";
    rebindDesc.CacheRoot = root / "ProjectB" / ".Cache" / "AssetDatabase";
    ASSERT_TRUE(am.BeginRebindSource("project", rebindDesc));

    // Editor-loaded asset must survive project source rebind.
    EXPECT_TRUE(am.IsAssetLoaded(editorGuid));

    // Post-rebind edit should continue to hot-reload editor-mounted assets
    // (same mtime-granularity sleep as above).
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    WriteTextFile(editorAbs, "v3");
    ASSERT_TRUE(WaitUntil([&]() {
        am.Update();
        return testAsset->LoadCount() >= 3;
    })) << "Expected editor asset to keep hot-reloading after project rebind";

    am.Shutdown();
    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, ScriptManager_RebindProjectScripts_UpdatesProjectPaths)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_script_rebind_watcher");
    const fs::path projectA = root / "ProjectA";
    const fs::path projectB = root / "ProjectB";
    const fs::path scriptsA = projectA / "Assets";
    const fs::path scriptsB = projectB / "Assets";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(scriptsA, ec);
    fs::create_directories(scriptsB, ec);

    JobSystem::WorkStealingThreadPool pool(2);
    ScriptManager manager;

    ScriptsConfig configA{};
    configA.workspaceRoot = projectA;
    configA.scriptsRoot = scriptsA;
    configA.assembliesRoot = projectA / "ScriptAssemblies";
    configA.generatedProjectRoot = configA.assembliesRoot;
    configA.disableClr = true;
    configA.enableHotReload = false;
    configA.enableAsyncHotReload = false;
    configA.enableAutoProjectGeneration = false;
    ASSERT_TRUE(manager.Initialize(configA, pool));
    EXPECT_EQ(NormalizePathForCompare(manager.GetScriptsDirectory()), NormalizePathForCompare(scriptsA));
    EXPECT_EQ(NormalizePathForCompare(manager.GetAssembliesDirectory()), NormalizePathForCompare(projectA / "ScriptAssemblies"));

    ScriptsConfig configB = configA;
    configB.workspaceRoot = projectB;
    configB.scriptsRoot = scriptsB;
    configB.assembliesRoot = projectB / "ScriptAssemblies";
    configB.generatedProjectRoot = configB.assembliesRoot;
    ASSERT_TRUE(manager.RebindProjectScripts(configB));
    EXPECT_EQ(NormalizePathForCompare(manager.GetScriptsDirectory()), NormalizePathForCompare(scriptsB));
    EXPECT_EQ(NormalizePathForCompare(manager.GetAssembliesDirectory()), NormalizePathForCompare(projectB / "ScriptAssemblies"));
    EXPECT_TRUE(fs::exists(scriptsB));
    EXPECT_TRUE(fs::exists(projectB / "ScriptAssemblies"));

    manager.Shutdown();
    fs::remove_all(root, ec);
}

// ------------------------------------------------------------------
// Same-root overlap: editor GUID must survive project rebind
// ------------------------------------------------------------------

TEST(AssetRootSeparation, AssetManager_SameRootOverlap_EditorGuidSurvivesProjectRebind)
{
    namespace fs = std::filesystem;

    // Scenario: project and editor share the exact same Assets root at startup.
    // The editor explicitly resolves its theme via the "editor" alias, which
    // assigns a derived-identity GUID and records editor ownership.
    // A separate project-only asset (not claimed by editor) should still be
    // ejected on project rebind, while the editor-claimed asset survives.

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_same_root_overlap");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "SharedAssets" / "UI" / "theme", ec);
    fs::create_directories(root / "SharedAssets" / "Textures", ec);
    fs::create_directories(root / "NewProjectAssets", ec);
    WriteTextFile(root / "SharedAssets" / "UI" / "theme" / "tokens.css", "body{}");
    WriteTextFile(root / "SharedAssets" / "Textures" / "hero.png", "PNG");

    const fs::path sharedRoot = root / "SharedAssets";
    const fs::path projectDb = root / "AssetDatabase.assetdb";
    const fs::path projectCache = root / ".Cache" / "AssetDatabase";

    AssetManager am;
    ASSERT_TRUE(am.Initialize(sharedRoot, nullptr, projectDb, projectCache));

    // Register editor source on the same root.
    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = sharedRoot; // same as project
    ASSERT_TRUE(am.RegisterSource(editorSource));

    // Editor explicitly resolves its theme -- this claims ownership and remaps
    // the in-memory GUID from the project's stored identity to a derived GUID.
    const fs::path editorRel = fs::path("UI") / "theme" / "tokens.css";
    const GUID editorGuid = am.ResolveAssetGuid(editorRel, "editor");
    ASSERT_FALSE(editorGuid.IsNull());

    // A project-only asset that the editor never explicitly claims.
    const fs::path projectOnlyRel = fs::path("Textures") / "hero.png";
    const GUID projectOnlyGuid = am.ResolveAssetGuid(projectOnlyRel);
    ASSERT_FALSE(projectOnlyGuid.IsNull());

    // Rebind project to a completely different root.
    AssetManager::SourceRebindDesc rebindDesc;
    rebindDesc.NewRoot = root / "NewProjectAssets";
    rebindDesc.AuthoritativeDbFile = root / "NewProjectAssets" / "AssetDatabase.assetdb";
    rebindDesc.CacheRoot = root / "NewProjectAssets" / ".Cache" / "AssetDatabase";
    ASSERT_TRUE(am.BeginRebindSource("project", rebindDesc));

    // Editor-claimed asset must survive project rebind.
    EXPECT_TRUE(am.GetRegistry().IsAssetRegistered(editorGuid))
        << "Editor-owned asset must survive project rebind";

    // Project-only asset (no explicit editor ownership) should be ejected.
    EXPECT_FALSE(am.GetRegistry().IsAssetRegistered(projectOnlyGuid))
        << "Project-only asset should be ejected after rebind";

    // Re-resolving via editor alias should give the same stable GUID.
    const GUID editorGuidAfter = am.ResolveAssetGuid(editorRel, "editor");
    EXPECT_EQ(editorGuid, editorGuidAfter)
        << "Editor GUID must be stable across project rebind";

    am.Shutdown();
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Phase 6 — Package mount: registers an immutable, manifest-driven source.
// The preset (MakePackageMount) wires the lifecycle flags so the mount
// skips the auto-scan, file watcher, and write-back paths; the manifest
// is loaded as the source's authoritative store via the existing JSONL
// loader (header line { "format": "assetmanifest", "version": 2 }).
// ---------------------------------------------------------------------------
TEST(AssetRootSeparation, PackageMount_LoadsManifestAndRejectsMutations)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_package_mount");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const fs::path manifestPath = root / ".assetmanifest";

    // Package mounts enumerate from a manifest but the assets themselves
    // are still on disk — the manifest just gives identity + ordering
    // without needing a fs walk. Create the asset file alongside the
    // manifest so reconcile's existence check finds it.
    const fs::path assetOnDisk = root / "models" / "cube.fbx";
    fs::create_directories(assetOnDisk.parent_path(), ec);
    WriteTextFile(assetOnDisk, "DUMMY");

    // Hand-write a manifest with one asset record. Format mirrors the
    // assetdb v2 JSONL: header line + per-asset record line.
    const GUID expectedGuid("11111111-2222-3333-4444-555555555555");
    {
        std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open()) << manifestPath.string();
        out << R"({"format":"assetmanifest","version":2})" << "\n";
        out << R"({"guid":"11111111-2222-3333-4444-555555555555",)"
            << R"("path":"models/cube.fbx",)"
            << R"("type":"Model"})" << "\n";
    }

    JobSystem::WorkStealingThreadPool pool(2);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(&pool));

    AssetSourceDesc pkg = MakePackageMount("pkg-test", root, /*priority*/ 25);
    ASSERT_TRUE(reg.RegisterSource(pkg));

    // Manifest record is loaded — the GUID is queryable, type is preserved.
    AssetMetadata md{};
    ASSERT_TRUE(reg.TryGetAssetMetadata(expectedGuid, md));
    EXPECT_EQ(md.Type, AssetType::Model);

    // Mutations land on rejection paths because IsImmutable=true. Try a
    // RegisterAsset on a path that's NOT in the manifest (so the
    // idempotent "already registered" fast-path can't short-circuit) —
    // the immutable check should reject.
    const fs::path newAsset = root / "models" / "sphere.fbx";
    WriteTextFile(newAsset, "DUMMY");
    EXPECT_FALSE(reg.RegisterAsset(newAsset))
        << "RegisterAsset of a new asset under an immutable Package mount should be rejected";
    EXPECT_TRUE(reg.GetAssetGUID(newAsset).IsNull())
        << "Rejected registration must not have inserted into m_PathToGuid";

    // Unregister-by-path of an existing manifest asset is also rejected.
    const fs::path inPackage = root / "models" / "cube.fbx";
    EXPECT_FALSE(reg.TryUnregisterAssetByPath(inPackage))
        << "TryUnregisterAssetByPath under an immutable Package mount should be rejected";
    EXPECT_TRUE(reg.IsAssetRegistered(expectedGuid))
        << "Rejected unregister must leave the manifest record intact";

    reg.Shutdown();
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// kv rows are store state, so the mount writability contract covers
// SetMetaValue too. A manifest-listed package asset is already registered, so
// SetMetaValue's RegisterAsset precondition passes and the immutable check has
// to live on the store-write branch itself. A writable non-project mount in
// the same registry must keep accepting kv.
// ---------------------------------------------------------------------------
TEST(AssetRootSeparation, SetMetaValue_RejectsImmutablePackageMount_AcceptsWritableMount)
{
    namespace fs = std::filesystem;

    const fs::path projectRoot = TestUtils::MakeUniqueTempDirectory("ge_setmeta_project");
    const fs::path packageRoot = TestUtils::MakeUniqueTempDirectory("ge_setmeta_package");
    const fs::path editorRoot = TestUtils::MakeUniqueTempDirectory("ge_setmeta_editor");

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
    fs::remove_all(packageRoot, ec);
    fs::remove_all(editorRoot, ec);
    fs::create_directories(projectRoot / "Assets", ec);
    fs::create_directories(editorRoot / "Assets", ec);

    const fs::path packageAsset = packageRoot / "models" / "cube.fbx";
    WriteTextFile(packageAsset, "DUMMY");

    const GUID packageGuid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    {
        std::ofstream out(packageRoot / ".assetmanifest", std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << R"({"format":"assetmanifest","version":2})" << "\n";
        out << R"({"guid":"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",)"
            << R"("path":"models/cube.fbx",)"
            << R"("type":"Model"})" << "\n";
    }

    const fs::path projectAssetsRoot = projectRoot / "Assets";
    const fs::path editorAssetsRoot = editorRoot / "Assets";
    const fs::path editorAsset = editorAssetsRoot / "UI" / "theme.css";
    WriteTextFile(editorAsset, "DUMMY");

    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(projectAssetsRoot,
                               nullptr,
                               projectRoot / "AssetDatabase.assetdb",
                               projectRoot / ".Cache" / "AssetDatabase"));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = editorAssetsRoot;
    editorSource.AuthoritativeDbFile = projectRoot / ".EditorLocal" / "EditorAssetDatabase.assetdb";
    editorSource.CacheRoot = projectRoot / ".EditorLocal" / ".Cache" / "AssetDatabase";
    ASSERT_TRUE(reg.RegisterSource(editorSource));
    ASSERT_TRUE(reg.RegisterSource(MakePackageMount("pkg-setmeta", packageRoot, /*priority*/ 25)));

    // Instrument check: the package asset resolves to its manifest identity,
    // so a rejection below is the writability gate firing, not an unrelated
    // early-out on an unknown path.
    ASSERT_EQ(reg.GetAssetGUID(packageAsset), packageGuid);

    std::string readBack;
    EXPECT_FALSE(reg.SetMetaValue(packageAsset, "testKey", "testValue"))
        << "SetMetaValue under an immutable Package mount should be rejected";
    EXPECT_FALSE(reg.TryGetMetaValue(packageAsset, "testKey", readBack))
        << "Rejected kv write must not have landed in the package store";

    // Positive control in the same registry: a writable non-project mount
    // still routes kv to its own store.
    ASSERT_TRUE(reg.RegisterAsset(editorAsset));
    EXPECT_TRUE(reg.SetMetaValue(editorAsset, "testKey", "testValue"));
    ASSERT_TRUE(reg.TryGetMetaValue(editorAsset, "testKey", readBack));
    EXPECT_EQ(readBack, "testValue");

    reg.Shutdown();
    fs::remove_all(projectRoot, ec);
    fs::remove_all(packageRoot, ec);
    fs::remove_all(editorRoot, ec);
}

// ---------------------------------------------------------------------------
// E1 — the scan fan-in (RegisterAssetMetadata) must never leak absolute-path
// records into the project store. Paths outside the project root route to
// the owning source's store, or stay in-memory when no writable store owns
// them.
// ---------------------------------------------------------------------------

TEST(AssetRootSeparation, RegisterAssetMetadata_RoutesNonProjectPathToOwningSourceStore)
{
    namespace fs = std::filesystem;

    const fs::path projectRoot = TestUtils::MakeUniqueTempDirectory("ge_e1_route_project");
    const fs::path editorRoot = TestUtils::MakeUniqueTempDirectory("ge_e1_route_editor");
    const fs::path orphanRoot = TestUtils::MakeUniqueTempDirectory("ge_e1_route_orphan");
    std::error_code ec;
    fs::remove_all(projectRoot, ec);
    fs::remove_all(editorRoot, ec);
    fs::remove_all(orphanRoot, ec);
    fs::create_directories(projectRoot / "Assets", ec);
    fs::create_directories(editorRoot / "Assets" / "UI", ec);
    fs::create_directories(orphanRoot, ec);

    const fs::path projectAssetsRoot = projectRoot / "Assets";
    const fs::path editorAssetsRoot = editorRoot / "Assets";
    const fs::path projectDb = projectRoot / "AssetDatabase.assetdb";
    const fs::path projectCache = projectRoot / ".Cache" / "AssetDatabase";
    const fs::path editorDb = projectRoot / ".EditorLocal" / "EditorAssetDatabase.assetdb";
    const fs::path editorCache = projectRoot / ".EditorLocal" / ".Cache" / "AssetDatabase";

    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(projectAssetsRoot, nullptr, projectDb, projectCache));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = editorAssetsRoot;
    editorSource.AuthoritativeDbFile = editorDb;
    editorSource.CacheRoot = editorCache;
    ASSERT_TRUE(reg.RegisterSource(editorSource));

    // Created after RegisterSource so the mount's synchronous startup scan
    // never saw it: RegisterAssetMetadata below is the record's first contact
    // with any store — the exact shape of the scan-completion fan-in.
    const fs::path editorAsset = editorAssetsRoot / "UI" / "panel.css";
    WriteTextFile(editorAsset, "DUMMY");

    AssetMetadata md{};
    md.Guid = reg.GetOrCreateAssetGUID(editorAsset);
    ASSERT_FALSE(md.Guid.IsNull());
    md.Path = editorAsset;
    md.Type = AssetType::UIStyle;
    ASSERT_TRUE(reg.RegisterAssetMetadata(md));

    // A path owned by NO registered source must register in-memory only.
    const fs::path orphanAsset = orphanRoot / "stray.png";
    WriteTextFile(orphanAsset, "DUMMY");
    AssetMetadata orphanMd{};
    orphanMd.Guid = GUID::Generate();
    orphanMd.Path = orphanAsset;
    orphanMd.Type = AssetType::Texture;
    ASSERT_TRUE(reg.RegisterAssetMetadata(orphanMd));
    EXPECT_TRUE(reg.IsAssetRegistered(orphanMd.Guid));

    ASSERT_TRUE(reg.SaveToFile({}));
    reg.Shutdown();

    // The editor-mount record lands in the editor store, canonical-relative
    // to the editor root.
    {
        AssetDatabase::AssetStore_TextJsonl store;
        std::string err;
        ASSERT_TRUE(store.LoadFromFile(editorDb, &err)) << err;
        AssetDatabase::AssetRecord rec{};
        ASSERT_TRUE(store.TryGetAsset(md.Guid, rec));
        EXPECT_EQ(rec.path, "ui/panel.css");
    }

    // The project store contains neither record — and no absolute-path
    // record at all.
    {
        AssetDatabase::AssetStore_TextJsonl store;
        std::string err;
        ASSERT_TRUE(store.LoadFromFile(projectDb, &err)) << err;
        AssetDatabase::AssetRecord rec{};
        EXPECT_FALSE(store.TryGetAsset(md.Guid, rec));
        EXPECT_FALSE(store.TryGetAsset(orphanMd.Guid, rec));
        for (const auto& r : store.EnumerateAssets())
        {
            EXPECT_FALSE(AssetPaths::IsAbsoluteStorePath(r.path))
                << "project store leaked absolute path: " << r.path;
        }
        EXPECT_TRUE(store.GetLoadQuarantinedRecords().empty());
    }

    fs::remove_all(projectRoot, ec);
    fs::remove_all(editorRoot, ec);
    fs::remove_all(orphanRoot, ec);
}

TEST(AssetRootSeparation, StoreLoadQuarantinesLeakedAbsolutePathRecords)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_e1_quarantine");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Assets" / "UI", ec);
    WriteTextFile(root / "Assets" / "UI" / "real.css", "body{}");
    WriteTextFile(root / "Assets" / "UI" / "legacy.css", "body{}");

    const fs::path assetsRoot = root / "Assets";
    const fs::path projectDb = root / "AssetDatabase.assetdb";
    const fs::path cacheRoot = root / ".Cache" / "AssetDatabase";

    const GUID keepGuid("11111111-1111-1111-1111-111111111111");
    const GUID winLeakGuid("22222222-2222-2222-2222-222222222222");
    const GUID posixLeakGuid("33333333-3333-3333-3333-333333333333");
    const GUID underRootGuid("44444444-4444-4444-4444-444444444444");

    // The leak persisted NormalizePathForMap output: casefolded on
    // Windows/macOS, exact-case on Linux. Reproduce that shape for the
    // under-root record so the repair path sees the same string the real
    // corruption carries.
#if defined(__linux__)
    const std::string underRootAbs = (assetsRoot / "UI" / "legacy.css").lexically_normal().generic_string();
#else
    const std::string underRootAbs = AssetPaths::NormalizeForRegistryKey(assetsRoot / "UI" / "legacy.css");
#endif

    {
        std::ofstream out(projectDb, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << R"({"format":"assetdb","version":2})" << "\n";
        out << R"({"guid":"11111111-1111-1111-1111-111111111111","path":"UI/real.css","type":"UIStyle"})" << "\n";
        out << R"({"guid":"22222222-2222-2222-2222-222222222222","path":"c:/dev/other-machine/build/tex.png","type":"Texture"})" << "\n";
        out << R"({"guid":"33333333-3333-3333-3333-333333333333","path":"/users/macstudio/library/leak.glsl","type":"Shader"})" << "\n";
        out << R"({"guid":"44444444-4444-4444-4444-444444444444","path":")" << underRootAbs
            << R"(","type":"UIStyle","kv":{"userKey":"userValue"}})" << "\n";
    }

    // Seed the SQLite cache with the rows the leak used to accumulate for a
    // leaked record: asset row (with fingerprint) + kv mirror. All must be
    // gone after the recovery pass so nothing re-fingerprints next session.
    {
        AssetDatabase::AssetDbCache_Sqlite cache;
        std::string err;
        fs::create_directories(cacheRoot, ec);
        ASSERT_TRUE(cache.Open(cacheRoot / "AssetDbCache.sqlite", &err)) << err;
        ASSERT_TRUE(cache.EnsureSchema(&err)) << err;
        AssetDatabase::AssetRecord leak{};
        leak.guid = winLeakGuid;
        leak.path = "c:/dev/other-machine/build/tex.png";
        leak.type = AssetType::Texture;
        ASSERT_TRUE(cache.UpsertAsset(leak, &err)) << err;
        ASSERT_TRUE(cache.UpdateFileFingerprint(winLeakGuid, 123, 456, "deadbeef", "fid", &err)) << err;
        ASSERT_TRUE(cache.SetKeyValue(winLeakGuid, "k", "v", &err)) << err;
        cache.Close();
    }

    std::vector<std::string> logLines;
    // Engine is a SHARED library: the registry logs through Engine.dll's
    // Logger state, not this exe's copy. Adopt the engine's state so
    // Initialize/AddSink/Flush below act on the state the recovery writes to.
    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    Logger::Log::Initialize({});
    Logger::Log::AddSink(std::make_unique<CapturingLogSink>(&logLines));

    {
        AssetRegistry reg;
        ASSERT_TRUE(reg.Initialize(assetsRoot, nullptr, projectDb, cacheRoot));

        // Quarantined records must not hydrate the registry; healthy and
        // repaired ones must.
        EXPECT_TRUE(reg.IsAssetRegistered(keepGuid));
        EXPECT_FALSE(reg.IsAssetRegistered(winLeakGuid));
        EXPECT_FALSE(reg.IsAssetRegistered(posixLeakGuid));
        EXPECT_TRUE(reg.IsAssetRegistered(underRootGuid))
            << "under-root absolute record must be repaired, not dropped";

        ASSERT_TRUE(reg.SaveToFile({}));
        reg.Shutdown();
    }

    Logger::Log::Flush();
    const bool sawSummary = std::any_of(
        logLines.begin(), logLines.end(), [](const std::string& l)
        { return l.find("quarantined 2 leaked absolute-path records from 'project'") != std::string::npos &&
                 l.find("1 repaired to canonical-relative") != std::string::npos; });
    EXPECT_TRUE(sawSummary) << "expected the quarantine summary log line";
    Logger::Log::ClearSinks();

    // The dropped records are journal-silent: no tombstone lines appear in
    // the file for any leaked GUID (that append-per-session churn is the
    // regression this guards against).
    {
        const std::string fileText = [&projectDb]() {
            std::ifstream in(projectDb, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }();
        EXPECT_EQ(fileText.find("\"deleted\":true"), std::string::npos)
            << "quarantined records must never be tombstoned into the journal";
    }

    // On disk after reload: leaked records quarantined again (still present
    // as historical lines, never live), healthy record intact, under-root
    // record live at its repaired canonical-relative path with kv preserved.
    {
        AssetDatabase::AssetStore_TextJsonl store;
        std::string err;
        ASSERT_TRUE(store.LoadFromFile(projectDb, &err)) << err;
        EXPECT_EQ(store.GetLoadQuarantinedRecords().size(), 2u);

        AssetDatabase::AssetRecord rec{};
        EXPECT_TRUE(store.TryGetAsset(keepGuid, rec));
        EXPECT_FALSE(store.TryGetAsset(winLeakGuid, rec));
        EXPECT_FALSE(store.TryGetAsset(posixLeakGuid, rec));
        ASSERT_TRUE(store.TryGetAsset(underRootGuid, rec))
            << "repaired record must survive the save/load round-trip";
        EXPECT_FALSE(AssetPaths::IsAbsoluteStorePath(rec.path));
        auto it = rec.kv.find("userKey");
        ASSERT_TRUE(it != rec.kv.end()) << "repair must preserve user-authored kv";
        EXPECT_EQ(it->second, "userValue");
    }

    // SQLite rows for the leaked GUID are gone: asset row, fingerprint, kv.
    {
        AssetDatabase::AssetDbCache_Sqlite cache;
        std::string err;
        ASSERT_TRUE(cache.Open(cacheRoot / "AssetDbCache.sqlite", &err)) << err;
        AssetDatabase::AssetRecord rec{};
        EXPECT_FALSE(cache.TryGetAsset(winLeakGuid, rec));
        AssetDatabase::IAssetDbCache::FileFingerprint fp{};
        EXPECT_FALSE(cache.TryGetFileFingerprint(winLeakGuid, fp));
        std::string v;
        EXPECT_FALSE(cache.TryGetKeyValue(winLeakGuid, "k", v));
        cache.Close();
    }

    // Idempotence: a second session loads the store cleanly, with the
    // repaired record still resolving.
    {
        AssetRegistry reg;
        ASSERT_TRUE(reg.Initialize(assetsRoot, nullptr, projectDb, cacheRoot));
        EXPECT_TRUE(reg.IsAssetRegistered(keepGuid));
        EXPECT_TRUE(reg.IsAssetRegistered(underRootGuid));
        reg.Shutdown();
    }

    fs::remove_all(root, ec);
}

// The acceptance shape for the polluted-journal defect: a project whose
// tracked .assetdb carries historical absolute-path garbage must survive a
// no-change session byte-identical — no tombstones, no re-appends, no
// compaction. This is what keeps the file clean under VCS across machines.
TEST(AssetRootSeparation, PollutedProjectJournalStaysByteIdenticalAcrossNoChangeSession)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_e1_stablefile");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Assets" / "UI", ec);
    WriteTextFile(root / "Assets" / "UI" / "real.css", "body{}");

    const fs::path assetsRoot = root / "Assets";
    const fs::path projectDb = root / "AssetDatabase.assetdb";
    const fs::path cacheRoot = root / ".Cache" / "AssetDatabase";

    const GUID keepGuid("11111111-1111-1111-1111-111111111111");

    {
        std::ofstream out(projectDb, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << R"({"format":"assetdb","version":2})" << "\n";
        out << R"({"guid":"11111111-1111-1111-1111-111111111111","path":"UI/real.css","type":"UIStyle"})" << "\n";
        out << R"({"guid":"22222222-2222-2222-2222-222222222222","path":"/users/macstudio/library/application support/gameengine/editor/editorassets/icons/copy.png","type":"Texture"})" << "\n";
        out << R"({"guid":"33333333-3333-3333-3333-333333333333","path":"/users/macstudio/library/application support/gameengine/editor/editorassets/shaders/brdf_lut.comp.spv","type":"Unknown"})" << "\n";
        out << R"({"guid":"44444444-4444-4444-4444-444444444444","path":"c:/dev/other-machine/build/bin/debugfast/apps/editor/assets/ui/theme.css","type":"UIStyle"})" << "\n";
    }

    const std::string bytesBefore = ReadFileBytes(projectDb);
    ASSERT_FALSE(bytesBefore.empty());

    {
        AssetRegistry reg;
        ASSERT_TRUE(reg.Initialize(assetsRoot, nullptr, projectDb, cacheRoot));
        EXPECT_TRUE(reg.IsAssetRegistered(keepGuid));
        reg.Shutdown();
    }

    const std::string bytesAfter = ReadFileBytes(projectDb);
    EXPECT_EQ(bytesBefore, bytesAfter)
        << "a no-change session over a polluted journal must not rewrite it";

    // And the session after behaves identically (stable fixed point).
    {
        AssetRegistry reg;
        ASSERT_TRUE(reg.Initialize(assetsRoot, nullptr, projectDb, cacheRoot));
        EXPECT_TRUE(reg.IsAssetRegistered(keepGuid));
        reg.Shutdown();
    }
    EXPECT_EQ(bytesBefore, ReadFileBytes(projectDb));

    fs::remove_all(root, ec);
}

// Writer-side contract gate: an absolute path must never reach the journal,
// whatever the caller. This is the store-level backstop for the class of
// leak that produced cross-machine absolute rows in tracked project DBs.
TEST(AssetRootSeparation, JsonlStoreRejectsAbsolutePathUpserts)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_store_absreject");
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path dbFile = root / "store.assetdb";

    AssetDatabase::AssetStore_TextJsonl store;

    AssetDatabase::AssetRecord winAbs{};
    winAbs.guid = GUID("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
    winAbs.path = "C:/dev/other-machine/build/tex.png";
    winAbs.type = AssetType::Texture;
    std::string err;
    EXPECT_FALSE(store.UpsertAsset(winAbs, &err));
    EXPECT_FALSE(err.empty());

    AssetDatabase::AssetRecord posixAbs{};
    posixAbs.guid = GUID("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
    posixAbs.path = "/users/macstudio/library/leak.glsl";
    posixAbs.type = AssetType::Shader;
    EXPECT_FALSE(store.UpsertAsset(posixAbs, nullptr));

    AssetDatabase::AssetRecord rel{};
    rel.guid = GUID("cccccccc-cccc-cccc-cccc-cccccccccccc");
    rel.path = "Textures/ok.png";
    rel.type = AssetType::Texture;
    EXPECT_TRUE(store.UpsertAsset(rel, nullptr));

    EXPECT_EQ(store.CountAssets(), 1u);
    EXPECT_FALSE(store.LookupGuidByPath(winAbs.path).has_value());

    ASSERT_TRUE(store.SaveToFile(dbFile, &err)) << err;
    const std::string fileText = ReadFileBytes(dbFile);
    EXPECT_NE(fileText.find("Textures/ok.png"), std::string::npos);
    EXPECT_EQ(fileText.find("other-machine"), std::string::npos)
        << "absolute-path record reached the journal";
    EXPECT_EQ(fileText.find("macstudio"), std::string::npos)
        << "absolute-path record reached the journal";

    fs::remove_all(root, ec);
}

// Build/tooling files inside the asset root (a user-authored .csproj;
// CMakeLists.txt rides along in converted projects) are not assets and must
// never journal.
TEST(AssetRootSeparation, GeneratedBuildFilesNeverJournal)
{
    namespace fs = std::filesystem;

    // Rule-level: the defaults classify build/tooling files as ignored.
    {
        const AssetIgnoreRules rules = AssetIgnoreRules::CreateDefault();
        EXPECT_TRUE(rules.ShouldIgnoreCanonicalRelativePath("gameengine.scripts.csproj"));
        EXPECT_TRUE(rules.ShouldIgnoreCanonicalRelativePath("CMakeLists.txt"));
        EXPECT_TRUE(rules.ShouldIgnoreCanonicalRelativePath("Sub/Dir/Game.sln"));
        EXPECT_TRUE(rules.ShouldIgnoreCanonicalRelativePath("Game.slnx"));
        EXPECT_TRUE(rules.ShouldIgnoreCanonicalRelativePath("Native/Engine.vcxproj"));
        EXPECT_FALSE(rules.ShouldIgnoreCanonicalRelativePath("UI/style.css"));
        EXPECT_FALSE(rules.ShouldIgnoreCanonicalRelativePath("Textures/icon.png"));
    }

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_buildfile_ignore");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Assets", ec);
    WriteTextFile(root / "Assets" / "GameEngine.Scripts.csproj", "<Project/>");
    WriteTextFile(root / "Assets" / "CMakeLists.txt", "add_subdirectory(x)");
    WriteTextFile(root / "Assets" / "icon.png", "not-really-a-png");

    const fs::path assetsRoot = root / "Assets";
    const fs::path projectDb = root / "AssetDatabase.assetdb";
    const fs::path cacheRoot = root / ".Cache" / "AssetDatabase";

    {
        AssetRegistry reg;
        ASSERT_TRUE(reg.Initialize(assetsRoot, nullptr, projectDb, cacheRoot));

        EXPECT_FALSE(reg.RegisterAsset(assetsRoot / "GameEngine.Scripts.csproj"))
            << "generated csproj must be rejected by the ignore rules";
        EXPECT_FALSE(reg.RegisterAsset(assetsRoot / "CMakeLists.txt"))
            << "build definition must be rejected by the ignore rules";
        EXPECT_TRUE(reg.RegisterAsset(assetsRoot / "icon.png"));

        ASSERT_TRUE(reg.SaveToFile({}));
        reg.Shutdown();
    }

    std::string fileText = ReadFileBytes(projectDb);
    std::transform(fileText.begin(), fileText.end(), fileText.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    EXPECT_NE(fileText.find("icon.png"), std::string::npos);
    EXPECT_EQ(fileText.find("csproj"), std::string::npos)
        << "generated csproj journaled into the project DB";
    EXPECT_EQ(fileText.find("cmakelists"), std::string::npos)
        << "CMakeLists journaled into the project DB";

    fs::remove_all(root, ec);
}

// RegisterAsset on a missing file is re-probed every frame by hot callers
// (renderer texture resolution, asset retry loops). The ERROR must be
// episodic: once per path while it stays missing — never a per-frame storm —
// and cleared when the file appears so a later disappearance logs afresh.
TEST(AssetRootSeparation, RegisterAsset_MissingFileErrorIsEpisodic)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_missing_episodic");
    std::error_code ec;
    const fs::path assetsRoot = root / "Assets";
    fs::create_directories(assetsRoot, ec);
    const fs::path projectDb = root / "AssetDatabase.assetdb";
    const fs::path cacheRoot = root / ".Cache" / "AssetDatabase";

    std::vector<std::string> logLines;
    // Engine is a SHARED library: the registry logs through Engine.dll's
    // Logger state, not this exe's copy (same adoption as the purge test).
    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    Logger::Log::Initialize({});
    Logger::Log::AddSink(std::make_unique<CapturingLogSink>(&logLines));

    const auto countLines = [&logLines](std::string_view needle)
    {
        return std::count_if(logLines.begin(), logLines.end(),
                             [needle](const std::string& l)
                             { return l.find(needle) != std::string::npos; });
    };

    {
        AssetRegistry reg;
        ASSERT_TRUE(reg.Initialize(assetsRoot, nullptr, projectDb, cacheRoot));

        const fs::path ghost = assetsRoot / "Textures" / "ghost.png";
        EXPECT_FALSE(reg.RegisterAsset(ghost));
        EXPECT_FALSE(reg.RegisterAsset(ghost));
        EXPECT_FALSE(reg.RegisterAsset(ghost));
        Logger::Log::Flush();
        EXPECT_EQ(countLines("Asset file does not exist"), 1)
            << "missing-file ERROR must fire once per episode, not per probe";

        // Episodes are per path: a second missing path logs its own ERROR.
        EXPECT_FALSE(reg.RegisterAsset(assetsRoot / "Textures" / "ghost2.png"));
        Logger::Log::Flush();
        EXPECT_EQ(countLines("Asset file does not exist"), 2);

        // Recovery: the file appears, registration succeeds, and the episode
        // closes with the transition log (which is what re-arms the ERROR
        // for a later disappearance of the same path).
        WriteTextFile(ghost, "not-really-a-png");
        EXPECT_TRUE(reg.RegisterAsset(ghost));
        Logger::Log::Flush();
        EXPECT_EQ(countLines("Asset file appeared"), 1)
            << "recovery must close the missing-file episode";

        reg.Shutdown();
    }

    Logger::Log::ClearSinks();
    fs::remove_all(root, ec);
}

// ----------------------------------------------------------------------------
// Root/key separation: a source root is a path files are OPENED from, so it
// keeps its on-disk spelling; only its key is folded. Full Unicode case folding
// is 1:N, so for a root like "Straße1307" the folded form is a DIFFERENT string
// of a different length — mounting on it created a phantom directory beside the
// real one and every reader followed the phantom.
// ----------------------------------------------------------------------------

// A store row keeps the spelling it was written with, and that differs by
// platform (NormalizePathForMap folds on Windows/macOS, preserves case on
// Linux), so journal assertions fold the text and look for the identity key.
static std::string FoldedJournalText(const std::filesystem::path& journalFile)
{
    return AssetPaths::FoldStorePathKey(ReadFileBytes(journalFile));
}

// U+00DF LATIN SMALL LETTER SHARP S folds to "ss": the fold changes length, so
// (unlike a case-only difference) the case-insensitive Windows filesystem does
// not paper over it.
static std::filesystem::path SharpSDirectoryName()
{
    const std::string utf8 = "Stra\xC3\x9F" "e1307";
    const auto* u8data = reinterpret_cast<const char8_t*>(utf8.data());
    return std::filesystem::path(u8data, u8data + utf8.size());
}

// The entries of `dir`, each spelled as the filesystem lists it. This is the
// instrument for "no phantom root was created": fs::exists(folded) is not,
// because APFS matches names under full case folding — "strasse1307" opens
// "Straße1307" — so the folded path exists there whether or not a second
// directory was made. A listing names every directory that actually exists.
static std::vector<std::filesystem::path> DirectoryEntryNames(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        names.push_back(entry.path().filename());
    std::sort(names.begin(), names.end());
    return names;
}

TEST(AssetRootSeparation, RegisterSource_RootWhoseFoldChangesLengthMountsOnDiskAndCreatesNoPhantom)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_fold_length_root");
    std::error_code ec;
    fs::remove_all(root, ec);

    const fs::path projectDir = root / SharpSDirectoryName();
    const fs::path assetsRoot = projectDir / "Assets";
    fs::create_directories(assetsRoot / "Scenes", ec);
    WriteTextFile(assetsRoot / "Scenes" / "Level.scene", "{}");

    // The only directory under `root` is the real one; the fold's spelling
    // ("strasse1307") must never be listed beside it.
    const std::vector<fs::path> onlyTheRealRoot{SharpSDirectoryName()};
    ASSERT_NE(AssetPaths::NormalizeForRegistryKey(assetsRoot),
              assetsRoot.lexically_normal().generic_string())
        << "fixture is not exercising a fold that changes the string";

    const fs::path projectDb = projectDir / "AssetDatabase.assetdb";
    const fs::path cacheRoot = projectDir / ".Cache" / "AssetDatabase";

    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(assetsRoot, nullptr, projectDb, cacheRoot));

    // The mount opens the directory that exists.
    EXPECT_EQ(reg.GetAssetRoot(), assetsRoot.lexically_normal());
    EXPECT_EQ(reg.GetSourceRoot("project"), assetsRoot.lexically_normal());
    EXPECT_EQ(DirectoryEntryNames(root), onlyTheRealRoot)
        << "mounting created a folded phantom root beside the real one";

    // A second, non-project mount takes the same rule.
    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = projectDir / "EditorAssets";
    ASSERT_TRUE(reg.RegisterSource(editorSource));
    EXPECT_EQ(reg.GetSourceRoot("editor"),
              (projectDir / "EditorAssets").lexically_normal());
    EXPECT_TRUE(fs::exists(projectDir / "EditorAssets"));
    EXPECT_EQ(DirectoryEntryNames(root), onlyTheRealRoot);

    // Registration still routes to the project store. This is the regression
    // that an unfolded root would otherwise cause: every canonical-relative
    // computation pairs the root with a FOLDED asset path, fs::path compares
    // case-sensitively, and a lexical pairing would answer "outside the root"
    // for every asset — silently dropping the project store's rows.
    const fs::path scene = assetsRoot / "Scenes" / "Level.scene";
    AssetMetadata sceneMd{};
    sceneMd.Path = scene;
    sceneMd.Guid = GUID("55555555-5555-5555-5555-555555555555");
    sceneMd.Type = AssetType::Scene;
    ASSERT_TRUE(reg.RegisterAssetMetadata(sceneMd));
    EXPECT_TRUE(reg.IsAssetRegistered(sceneMd.Guid));
    reg.Shutdown();

    const std::string journal = FoldedJournalText(projectDb);
    EXPECT_NE(journal.find("\"path\":\"scenes/level.scene\""), std::string::npos)
        << "the project store stopped receiving rows for assets under this root:\n" << journal;

    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, RegisterSource_CaseOnlyVariantRootResolvesToOneSource)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_case_variant_root");
    std::error_code ec;
    fs::remove_all(root, ec);

    // On disk: "Assets". Mounted as: "ASSETS" — the same directory on a
    // case-insensitive filesystem, a different string to fs::path.
    const fs::path onDiskRoot = root / "Assets";
    fs::create_directories(onDiskRoot / "Textures", ec);
    WriteTextFile(onDiskRoot / "Textures" / "Wall.png", "png");
    const fs::path variantRoot = root / "ASSETS";

    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(variantRoot, nullptr,
                               root / "AssetDatabase.assetdb", root / ".Cache" / "AssetDatabase"));

    const fs::path onDiskAsset = onDiskRoot / "Textures" / "Wall.png";
    const fs::path variantAsset = variantRoot / "textures" / "wall.png";

    ASSERT_TRUE(reg.RegisterAsset(onDiskAsset));
    const GUID viaOnDisk = reg.GetAssetGUID(onDiskAsset);
    ASSERT_FALSE(viaOnDisk.IsNull());
    EXPECT_EQ(reg.GetAssetGUID(variantAsset), viaOnDisk)
        << "a case-only spelling of the same file must resolve to one asset";

    // Registering through the other spelling must not fork a second source or
    // a second identity.
    ASSERT_TRUE(reg.RegisterAsset(variantAsset));
    EXPECT_EQ(reg.GetAssetGUID(onDiskAsset), viaOnDisk);
    EXPECT_EQ(reg.GetRegisteredSources().size(), 1u);
    EXPECT_EQ(reg.GetRegisteredSources().front().Alias, "project");

    reg.Shutdown();

    // The journal is the assertion that matters: a root and an asset path that
    // disagree only in case must still produce a canonical-relative row. Pairing
    // them lexically yields an empty relative, which flips `persistent` false in
    // RegisterAssetMetadata and drops the project store's writes silently — the
    // failure mode is a store that stops recording, not one that records wrong.
    const std::string journal = FoldedJournalText(root / "AssetDatabase.assetdb");
    EXPECT_NE(journal.find("\"path\":\"textures/wall.png\""), std::string::npos)
        << "an ordinary mixed-case root stopped persisting rows: " << journal;

    fs::remove_all(root, ec);
}

TEST(AssetRootSeparation, CanonicalRelativePathContainmentIsSpellingBlindButTheTailIsNot)
{
    namespace fs = std::filesystem;

    // The root reaches this API in the caller's spelling and registry paths
    // reach it folded, so CONTAINMENT must not depend on either side's
    // spelling. The TAIL must, because it is what store rows record and what
    // gets joined back onto a root to reopen the file — on a case-sensitive
    // filesystem only the on-disk spelling reopens it.
    const fs::path root = fs::current_path() / "Proj" / "Assets";
    const fs::path asset = root / "Models" / "Knight.fbx";

    const auto foldedPath = [](const fs::path& p) {
        const std::string key = AssetPaths::NormalizeForRegistryKey(p);
        const auto* u8data = reinterpret_cast<const char8_t*>(key.data());
        return fs::path(u8data, u8data + key.size());
    };

    std::string out;

    // Containment holds for all four spelling pairings.
    ASSERT_TRUE(AssetRegistry::TryComputeCanonicalRelativePath(root, asset, out));
    EXPECT_EQ(out, "Models/Knight.fbx");

    ASSERT_TRUE(AssetRegistry::TryComputeCanonicalRelativePath(foldedPath(root), asset, out))
        << "folded root paired with an on-disk asset path";
    EXPECT_EQ(out, "Models/Knight.fbx") << "the tail follows assetPath, not the root";

    ASSERT_TRUE(AssetRegistry::TryComputeCanonicalRelativePath(root, foldedPath(asset), out))
        << "on-disk root paired with a folded asset path";
    EXPECT_EQ(out, "models/knight.fbx") << "the tail follows assetPath, not the root";

    ASSERT_TRUE(AssetRegistry::TryComputeCanonicalRelativePath(foldedPath(root), foldedPath(asset), out));
    EXPECT_EQ(out, "models/knight.fbx");

    // A trailing separator on the root is a spelling too.
    ASSERT_TRUE(AssetRegistry::TryComputeCanonicalRelativePath(root / "", asset, out));
    EXPECT_EQ(out, "Models/Knight.fbx");

    // Whole elements are compared, so a sibling with a common prefix is out.
    EXPECT_FALSE(AssetRegistry::TryComputeCanonicalRelativePath(
        root.parent_path() / "Asset", asset, out));

    // The root itself, a sibling subtree, and empties are refused.
    EXPECT_FALSE(AssetRegistry::TryComputeCanonicalRelativePath(root, root, out));
    EXPECT_FALSE(AssetRegistry::TryComputeCanonicalRelativePath(root, root.parent_path() / "Other" / "x.fbx", out));
    EXPECT_FALSE(AssetRegistry::TryComputeCanonicalRelativePath(root, fs::path(), out));
    EXPECT_FALSE(AssetRegistry::TryComputeCanonicalRelativePath(fs::path(), asset, out));
}

// The output is what store rows and md.Path joins are built from, so it must
// stay byte-for-byte what a plain lexically_relative produces. Folding it would
// be invisible on Windows (every hot-path caller already passes folded paths)
// and would rewrite every row on a case-sensitive filesystem, where
// NormalizePathForMap preserves case and the rows are joined back onto a root
// to open files.
TEST(AssetRootSeparation, CanonicalRelativePathMatchesLexicallyRelativeForSameSpellingInputs)
{
    namespace fs = std::filesystem;

    const fs::path root = fs::current_path() / "Proj" / "Assets";
    const fs::path cases[] = {
        root / "a.png",
        root / "UI" / "Panels" / "MixedCase.CSS",
        root / "Models" / "sub dir" / "Knight.fbx",
    };

    for (const fs::path& asset : cases)
    {
        std::string out;
        ASSERT_TRUE(AssetRegistry::TryComputeCanonicalRelativePath(root, asset, out)) << asset.string();
        EXPECT_EQ(out, asset.lexically_relative(root).generic_string()) << asset.string();
    }
}

// The two halves together: a root whose fold changes length, paired with the
// folded asset path the registry actually hands around.
TEST(AssetRootSeparation, CanonicalRelativePathSurvivesALengthChangingFoldInTheRoot)
{
    namespace fs = std::filesystem;

    const fs::path root = fs::current_path() / SharpSDirectoryName() / "Assets";
    const fs::path asset = root / "Scenes" / "Level.scene";

    const std::string foldedKey = AssetPaths::NormalizeForRegistryKey(asset);
    const auto* u8data = reinterpret_cast<const char8_t*>(foldedKey.data());
    const fs::path foldedAsset(u8data, u8data + foldedKey.size());

    std::string out;
    ASSERT_TRUE(AssetRegistry::TryComputeCanonicalRelativePath(root, asset, out));
    EXPECT_EQ(out, "Scenes/Level.scene");

    ASSERT_TRUE(AssetRegistry::TryComputeCanonicalRelativePath(root, foldedAsset, out))
        << "a length-changing fold in the root must not defeat containment";
    EXPECT_EQ(out, "scenes/level.scene");
}

// The warm-start fingerprint map is keyed by the FOLDED canonical relative
// path, so every lookup must fold too. A lookup that passes the verbatim
// canonical relative misses the entry and silently re-hashes the file at every
// warm start.
//
// This bites on every platform, not only case-sensitive ones: the reconcile's
// new-files loop derives its canonical relative from a DISK enumeration, so the
// tail carries the on-disk spelling even where registry paths are folded.
// Verified by removing the fold at AssetStoreReconciler.cpp's lookup, which
// turns this test red on Windows.
TEST(AssetRootSeparation, WarmStartReusesTheSnapshotHashForAMixedCasePath)
{
    namespace fs = std::filesystem;

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_mixedcase_snapshot_reuse");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "UI" / "Panels", ec);

    // The shape that reaches the snapshot-hash-reuse path (mirrors
    // AssetDbHardening.ReconcileReusesSnapshotHashWhenStatMatches): one file
    // deleted between sessions, so its missing record gates the new-files loop
    // in StartupReconcileAssetDatabase, and one file whose store row is removed
    // so that loop sees it as new at a path the snapshot already covers.
    const fs::path mixedCase = root / "UI" / "Panels" / "ThemeDark.CSS";
    const fs::path gate = root / "UI" / "Panels" / "gate.txt";
    WriteTextFile(mixedCase, "body{}");
    WriteTextFile(gate, "gate");

    {
        JobSystem::WorkStealingThreadPool pool(2);
        AssetRegistry reg;
        ASSERT_TRUE(reg.Initialize(root, &pool));
        ASSERT_TRUE(reg.RegisterAsset(mixedCase));
        ASSERT_TRUE(reg.RegisterAsset(gate));
        ASSERT_TRUE(reg.SaveToFile({}));
        {
            auto scan = reg.ScanDirectoryAsync(root, true);
            (void)scan.get();
        }
        reg.Shutdown();
    }

    const fs::path dbFile = root / "AssetDatabase.assetdb";
    ASSERT_TRUE(fs::exists(root / ".Cache" / "AssetDatabase" / "watcher.snapshot.bin", ec))
        << "session 1 should have written a snapshot";

    // The snapshot must actually carry a fingerprint for the mixed-case file,
    // or this test proves nothing about the lookup.
    {
        fs::path mountRoot;
        uint64_t ignoreSig = 0;
        std::vector<AssetDatabase::AssetSourceSnapshotRecord> snapRecs;
        std::vector<AssetDatabase::AssetSourceSnapshotDirectory> snapDirs;
        std::string err;
        ASSERT_TRUE(AssetDatabase::AssetSourceSnapshot::Load(
            root / ".Cache" / "AssetDatabase" / "watcher.snapshot.bin",
            mountRoot, ignoreSig, snapRecs, snapDirs, &err)) << err;
        const bool covered = std::any_of(snapRecs.begin(), snapRecs.end(),
            [](const AssetDatabase::AssetSourceSnapshotRecord& r) {
                return AssetPaths::FoldStorePathKey(r.CanonicalPath) == "ui/panels/themedark.css" &&
                       !r.Hash.empty();
            });
        ASSERT_TRUE(covered) << "snapshot has no hashed entry for the mixed-case file";
    }

    fs::remove(gate, ec);
    {
        AssetDatabase::AssetStore_TextJsonl store;
        ASSERT_TRUE(store.LoadFromFile(dbFile, nullptr));
        for (const auto& r : store.EnumerateAssets())
        {
            if (AssetPaths::FoldStorePathKey(r.path) == "ui/panels/themedark.css")
            {
                ASSERT_TRUE(store.RemoveAsset(r.guid, nullptr));
                break;
            }
        }
        ASSERT_TRUE(store.SaveToFile(dbFile, nullptr));
    }

    {
        JobSystem::WorkStealingThreadPool pool(2);
        AssetRegistry reg;
        ASSERT_TRUE(reg.Initialize(root, &pool));
        {
            auto scan = reg.ScanDirectoryAsync(root, true);
            (void)scan.get();
        }
        EXPECT_GT(reg.GetSnapshotRecordCount("project"), 0u);
        EXPECT_GE(reg.GetLastReconcileHashesReused("project"), 1u)
            << "a mixed-case path missed the folded snapshot map and re-hashed";
        reg.Shutdown();
    }

    fs::remove_all(root, ec);
}
