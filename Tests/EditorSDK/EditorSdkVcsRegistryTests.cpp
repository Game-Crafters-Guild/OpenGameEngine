// EditorVcsProviderRegistry semantics — the contracts VCS provider modules
// (built-in and package) rely on:
//   * registration validation is loud and rejecting,
//   * same-TypeId registration replaces forward and hands the observer the
//     PREVIOUS integration (module hot-reload disconnect),
//   * detection runs in (DetectionOrder, TypeId) order and matches the legacy
//     hard-wired precedence (git > svn > diversion > lore),
//   * the active provider is the first (in detection order) whose integration
//     reports a repository,
//   * observers replay registrations that happened before attach.
//
// Links EditorSDK.dll — the same registry Editor.exe and module DLLs share.

#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VCSIntegration/IVCSIntegration.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace ed = GameEngine::Editor;

namespace
{

class FakeVcsIntegration final : public GameEngine::IVCSIntegration
{
public:
    bool Initialize(const std::filesystem::path&, const std::filesystem::path&) override
    {
        InitializeCount++;
        return InitializeResult;
    }
    void Shutdown() override { ShutdownCount++; Repository = false; }
    bool IsAvailable() const override { return true; }
    bool IsRepository() const override { return Repository; }
    GameEngine::VCSFileStatus GetFileStatus(const std::filesystem::path&) override
    {
        return GameEngine::VCSFileStatus::Clean;
    }
    bool Add(const std::filesystem::path&) override { return true; }
    bool Remove(const std::filesystem::path&) override { return true; }
    bool Move(const std::filesystem::path&, const std::filesystem::path&) override { return true; }
    bool Commit(const std::string&) override { return true; }
    bool Update() override { return true; }
    bool Revert(const std::filesystem::path&) override { return true; }
    bool IsIgnored(const std::filesystem::path&) override { return false; }
    void RefreshStatus(const std::filesystem::path&) override {}
    std::filesystem::path GetRepositoryRoot() const override { return {}; }
    std::string GetCurrentBranch() const override { return {}; }
    std::vector<GameEngine::VCSLogEntry> GetLog(const std::filesystem::path&, int) override
    {
        return {};
    }
    std::filesystem::path GetExecutable() const override { return {}; }
    void SetStatusChangedCallback(StatusChangedCallback) override {}

    bool Repository = false;
    bool InitializeResult = true;
    int InitializeCount = 0;
    int ShutdownCount = 0;
};

ed::EditorVcsProviderDescriptor MakeDescriptor(const std::string& typeId, int detectionOrder,
                                               FakeVcsIntegration& integration,
                                               std::string markerDir)
{
    ed::EditorVcsProviderDescriptor descriptor;
    descriptor.TypeId = typeId;
    descriptor.DisplayName = typeId;
    descriptor.DetectionOrder = detectionOrder;
    descriptor.Detect = [marker = std::move(markerDir)](const std::filesystem::path& root) {
        std::error_code ec;
        return std::filesystem::is_directory(root / marker, ec);
    };
    descriptor.Integration = [&integration]() -> GameEngine::IVCSIntegration& {
        return integration;
    };
    descriptor.Initialize = [&integration](const std::filesystem::path&, std::function<void()>) {
        integration.InitializeCount++;
        return integration.InitializeResult;
    };
    return descriptor;
}

std::filesystem::path MakeTempWorkspace(const std::string& name,
                                        const std::vector<std::string>& markers)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "ge_vcs_registry_tests" / name;
    std::filesystem::remove_all(root);
    for (const std::string& marker : markers)
        std::filesystem::create_directories(root / marker);
    if (markers.empty())
        std::filesystem::create_directories(root);
    return root;
}

} // namespace

TEST(EditorVcsProviderRegistry, RejectsIncompleteDescriptors)
{
    auto& registry = ed::EditorVcsProviderRegistry::Get();
    const std::size_t before = registry.Snapshot().size();

    static FakeVcsIntegration integration;
    ed::EditorVcsProviderDescriptor missingDetect =
        MakeDescriptor("vcsTest.reject", 90, integration, ".reject");
    missingDetect.Detect = nullptr;
    registry.RegisterProvider(missingDetect);

    ed::EditorVcsProviderDescriptor missingTypeId =
        MakeDescriptor("", 90, integration, ".reject");
    registry.RegisterProvider(missingTypeId);

    EXPECT_EQ(registry.Snapshot().size(), before) << "invalid descriptors must be rejected";

    ed::EditorVcsProviderDescriptor out;
    EXPECT_FALSE(registry.TryGet("vcsTest.reject", out));
}

TEST(EditorVcsProviderRegistry, DetectionOrderMatchesLegacyPrecedence)
{
    // The legacy DetectVCS checked markers at the project root with effective
    // precedence git > svn > diversion > lore. Fakes with orders 0/10/20/30
    // must reproduce those results for every combination that mattered.
    auto& registry = ed::EditorVcsProviderRegistry::Get();

    static FakeVcsIntegration git, svn, diversion, lore;
    registry.RegisterProvider(MakeDescriptor("vcsTest.det.git", 0, git, ".fakegit"));
    registry.RegisterProvider(MakeDescriptor("vcsTest.det.svn", 10, svn, ".fakesvn"));
    registry.RegisterProvider(MakeDescriptor("vcsTest.det.diversion", 20, diversion, ".fakediversion"));
    registry.RegisterProvider(MakeDescriptor("vcsTest.det.lore", 30, lore, ".fakelore"));

    EXPECT_EQ(registry.DetectWorkspace(MakeTempWorkspace("gitOnly", {".fakegit"})),
              "vcsTest.det.git");
    EXPECT_EQ(registry.DetectWorkspace(MakeTempWorkspace("svnOnly", {".fakesvn"})),
              "vcsTest.det.svn");
    EXPECT_EQ(registry.DetectWorkspace(MakeTempWorkspace("dvOnly", {".fakediversion"})),
              "vcsTest.det.diversion");
    EXPECT_EQ(registry.DetectWorkspace(MakeTempWorkspace("loreOnly", {".fakelore"})),
              "vcsTest.det.lore");

    // Git wins every mixed workspace (the legacy tie-break).
    EXPECT_EQ(registry.DetectWorkspace(
                  MakeTempWorkspace("gitAndSvn", {".fakegit", ".fakesvn"})),
              "vcsTest.det.git");
    EXPECT_EQ(registry.DetectWorkspace(
                  MakeTempWorkspace("gitAndLore", {".fakelore", ".fakegit"})),
              "vcsTest.det.git");
    // Non-git combinations resolve by fixed order, not registration order.
    EXPECT_EQ(registry.DetectWorkspace(
                  MakeTempWorkspace("svnAndLore", {".fakelore", ".fakesvn"})),
              "vcsTest.det.svn");
    EXPECT_EQ(registry.DetectWorkspace(
                  MakeTempWorkspace("dvAndLore", {".fakelore", ".fakediversion"})),
              "vcsTest.det.diversion");

    EXPECT_EQ(registry.DetectWorkspace(MakeTempWorkspace("none", {})), "");
    EXPECT_EQ(registry.DetectWorkspace({}), "") << "empty root never detects";
}

TEST(EditorVcsProviderRegistry, ActiveProviderIsFirstRepositoryInDetectionOrder)
{
    auto& registry = ed::EditorVcsProviderRegistry::Get();

    static FakeVcsIntegration first, second;
    registry.RegisterProvider(MakeDescriptor("vcsTest.act.a", 40, first, ".fakeActA"));
    registry.RegisterProvider(MakeDescriptor("vcsTest.act.b", 41, second, ".fakeActB"));

    first.Repository = false;
    second.Repository = true;
    EXPECT_EQ(registry.ActiveIntegration(), &second);
    EXPECT_EQ(registry.ActiveTypeId(), "vcsTest.act.b");

    first.Repository = true;
    EXPECT_EQ(registry.ActiveIntegration(), &first)
        << "detection order decides when several integrations report a repository";

    ed::EditorVcsProviderDescriptor active;
    ASSERT_TRUE(registry.TryGetActiveProvider(active));
    EXPECT_EQ(active.TypeId, "vcsTest.act.a");

    first.Repository = false;
    second.Repository = false;
    EXPECT_EQ(registry.ActiveIntegration(), nullptr);
    EXPECT_EQ(registry.ActiveTypeId(), "");
}

TEST(EditorVcsProviderRegistry, ReplaceForwardHandsObserverThePreviousIntegration)
{
    auto& registry = ed::EditorVcsProviderRegistry::Get();

    static FakeVcsIntegration original, reloaded;

    registry.RegisterProvider(MakeDescriptor("vcsTest.reload", 50, original, ".fakeReload"));
    const std::size_t countAfterFirst = registry.Snapshot().size();

    GameEngine::IVCSIntegration* observedReplaced = nullptr;
    std::string observedTypeId;
    registry.SetRegistrationObserver(
        [&](const ed::EditorVcsProviderDescriptor& descriptor,
            GameEngine::IVCSIntegration* replaced) {
            observedTypeId = descriptor.TypeId;
            if (replaced)
                observedReplaced = replaced;
        });

    registry.RegisterProvider(MakeDescriptor("vcsTest.reload", 50, reloaded, ".fakeReload"));

    EXPECT_EQ(registry.Snapshot().size(), countAfterFirst)
        << "same-TypeId re-registration must not grow the registry";
    EXPECT_EQ(observedTypeId, "vcsTest.reload");
    EXPECT_EQ(observedReplaced, &original)
        << "the observer must receive the replaced integration to disconnect it";

    ed::EditorVcsProviderDescriptor out;
    ASSERT_TRUE(registry.TryGet("vcsTest.reload", out));
    EXPECT_EQ(&out.Integration(), &reloaded) << "the new descriptor must win";

    registry.SetRegistrationObserver({});
}

TEST(EditorVcsProviderRegistry, ObserverReplaysEarlierRegistrations)
{
    auto& registry = ed::EditorVcsProviderRegistry::Get();

    static FakeVcsIntegration integration;
    registry.RegisterProvider(MakeDescriptor("vcsTest.replay", 60, integration, ".fakeReplay"));

    bool sawReplay = false;
    registry.SetRegistrationObserver(
        [&](const ed::EditorVcsProviderDescriptor& descriptor,
            GameEngine::IVCSIntegration* replaced) {
            if (descriptor.TypeId == "vcsTest.replay")
            {
                sawReplay = true;
                EXPECT_EQ(replaced, nullptr) << "replay is not a replacement";
            }
        });
    EXPECT_TRUE(sawReplay) << "attach order must not decide whether a provider is seen";

    registry.SetRegistrationObserver({});
}

TEST(EditorVcsProviderRegistry, BadgeSettingsChangedDispatchesThroughHandler)
{
    auto& registry = ed::EditorVcsProviderRegistry::Get();

    int fired = 0;
    registry.SetBadgeSettingsChangedHandler([&]() { fired++; });
    registry.NotifyBadgeSettingsChanged();
    EXPECT_EQ(fired, 1);

    registry.SetBadgeSettingsChangedHandler({});
    registry.NotifyBadgeSettingsChanged(); // loud no-op, must not crash
    EXPECT_EQ(fired, 1);
}
