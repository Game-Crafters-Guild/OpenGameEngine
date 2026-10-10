// First-detect ordering for package-provided VCS providers — the contract
// that made Git-as-a-package safe:
//   * EditorVersionControlService defers the FIRST workspace detection until
//     NotifyProvidersReady — a project root arriving earlier is only
//     recorded, and PumpDeferredWork keeps the gate closed too, so a
//     later-ordered provider (svn, 10) can NEVER transiently claim a mixed
//     workspace whose earlier-ordered provider (git, 0) is still loading;
//   * NotifyProvidersReady runs the deferred detection itself, and the
//     winner is decided by DetectionOrder — never by registration order;
//   * after the one-shot gate opens, InitializeForProject detects
//     immediately (project switches keep today's behavior).
//
// Links EditorSDK.dll (the shared registry) and compiles the editor's
// EditorVersionControlService.cpp directly — the gate lives in the service.

#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VCSIntegration/IVCSIntegration.h"
#include "VersionControl/EditorVersionControlService.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace ed = GameEngine::Editor;

namespace
{

class GateFakeIntegration final : public GameEngine::IVCSIntegration
{
public:
    bool Initialize(const std::filesystem::path&, const std::filesystem::path&) override
    {
        return true;
    }
    void Shutdown() override { Repository = false; }
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
    int InitializeCount = 0;
};

ed::EditorVcsProviderDescriptor MakeGateDescriptor(const std::string& typeId, int detectionOrder,
                                                   GateFakeIntegration& integration,
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
        integration.Repository = true;
        return true;
    };
    return descriptor;
}

std::filesystem::path MakeMixedWorkspace(const std::string& name,
                                         const std::vector<std::string>& markers)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "ge_vcs_gate_tests" / name;
    std::filesystem::remove_all(root);
    for (const std::string& marker : markers)
        std::filesystem::create_directories(root / marker);
    return root;
}

} // namespace

// The Git-as-a-package hazard, end to end: svn-like registers FIRST, the
// project root arrives, frames pump — and the mixed workspace still gets NO
// claim until NotifyProvidersReady, at which point the git-like provider
// (registered LAST, ordered FIRST) wins the first and only detection.
TEST(EditorVcsFirstDetectGate, MixedWorkspaceNeverSeesTransientLaterOrderedClaim)
{
    const std::filesystem::path root =
        MakeMixedWorkspace("mixed", {".gateGit", ".gateSvn"});

    static GateFakeIntegration svnLike;
    static GateFakeIntegration gitLike;
    svnLike = GateFakeIntegration{};
    gitLike = GateFakeIntegration{};

    GameEngine::EditorVersionControlService service;

    // The later-ordered provider is already there when the project opens —
    // exactly the window the old immediate-detect turned into a wrong claim.
    ed::EditorVcsProviderRegistry::Get().RegisterProvider(
        MakeGateDescriptor("gateTest.svn", 10, svnLike, ".gateSvn"));
    service.InitializeForProject(root);
    EXPECT_EQ(svnLike.InitializeCount, 0)
        << "gated first detection must not claim the workspace";

    // Frames pass; registrations armed the reinit flag — the pump must keep
    // the gate closed rather than detecting with an incomplete provider set.
    service.PumpDeferredWork();
    service.PumpDeferredWork();
    EXPECT_EQ(svnLike.InitializeCount, 0)
        << "PumpDeferredWork must not run detection before NotifyProvidersReady";

    // The default provider's module finishes loading (registration order is
    // the reverse of detection order here — the exact race).
    ed::EditorVcsProviderRegistry::Get().RegisterProvider(
        MakeGateDescriptor("gateTest.git", 0, gitLike, ".gateGit"));

    service.NotifyProvidersReady();
    EXPECT_EQ(gitLike.InitializeCount, 1) << "gate-open must run the deferred detection";
    EXPECT_EQ(svnLike.InitializeCount, 0)
        << "the later-ordered provider must never have been initialized";
    EXPECT_EQ(ed::EditorVcsProviderRegistry::Get().ActiveTypeId(), "gateTest.git");

    // The gate-open reinit consumed the armed flag: the next pump must not
    // disconnect-reconnect the provider it just initialized.
    service.PumpDeferredWork();
    EXPECT_EQ(gitLike.InitializeCount, 1);
}

// After the one-shot gate opens, project switches detect immediately —
// today's behavior, unchanged.
TEST(EditorVcsFirstDetectGate, InitializeAfterReadyDetectsImmediately)
{
    const std::filesystem::path root = MakeMixedWorkspace("postready", {".gatePost"});

    static GateFakeIntegration integration;
    integration = GateFakeIntegration{};

    GameEngine::EditorVersionControlService service;
    ed::EditorVcsProviderRegistry::Get().RegisterProvider(
        MakeGateDescriptor("gateTest.post", 0, integration, ".gatePost"));

    service.NotifyProvidersReady(); // no root yet — opens the gate only
    EXPECT_EQ(integration.InitializeCount, 0);

    service.InitializeForProject(root);
    EXPECT_EQ(integration.InitializeCount, 1) << "post-ready init must detect synchronously";
}
