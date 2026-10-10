// EditorSDK phase 1 end-to-end: generate → compile → load a REAL native
// Editor-kind package module against the EditorSDK interface, and assert its
// registrations land in the process-wide editor registries.
//
// Mirrors UserModuleBuildLoadTest (C10): a fixture module source is written to
// a scratch dir, the standalone user project is generated and built with a
// real cmake + MSVC invocation, and the DLL is loaded through the standard
// NativeScriptManager pipeline (ABI version + toolchain fingerprint gates
// included). The module links BOTH import libs — Engine and EditorSDK — the
// exact shape the editor gives Editor-kind package modules; the interface
// paths arrive via the CMake-generated `editorsdk_config.txt` next to the exe.
//
// The test calls EditorPluginRegistry::RegisterInspectors() BEFORE loading,
// reproducing real ordering (editor startup pass runs before packages load at
// project open) — the fixture's inspector must arrive via the late-plugin
// replay, not by luck of ordering.

#include "Core/Application.h" // PathUtils::GetExecutableDirectory
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Registries/BuildExporterRegistry.h"
#include "Editor/Registries/EditorMenuRegistry.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "InspectorRegistry.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/ScrollView.h"
#include "NativeScripting/NativeBuildConfig.h"
#include "NativeScripting/NativeScriptManager.h"
#include "Engine/Build/BuildPipeline.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ns = GameEngine::NativeScripting;
namespace ed = GameEngine::Editor;
namespace fs = std::filesystem;

namespace
{

std::vector<std::string> SplitList(const std::string& v, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(v);
    while (std::getline(in, cur, sep))
        if (!cur.empty())
            out.push_back(cur);
    return out;
}

std::map<std::string, std::string> ReadConfigFile(const fs::path& path, std::string& err)
{
    std::map<std::string, std::string> kv;
    std::ifstream in(path);
    if (!in)
    {
        err = "cannot open config file " + path.string();
        return kv;
    }
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

// The fixture editor module: an IEditorPlugin whose inspector registration
// must arrive via the late-registration replay, plus a traits registration at
// static init — everything an Editor-kind package module does, minus the UI.
constexpr const char* kFixtureModuleSource = R"cpp(
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Registries/BuildExporterRegistry.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "Engine/Build/BuildPipeline.h"
#include "InspectorRegistry.h"

namespace
{

constexpr GameEngine::ECS::ComponentTypeId kFixtureTypeId = 0xEDF1;

class FixtureEditorPlugin final : public GameEngine::Editor::IEditorPlugin
{
public:
    const GameEngine::Plugins::PluginDescriptor& GetDescriptor() const override
    {
        static const GameEngine::Plugins::PluginDescriptor descriptor{
            "editorSdkFixture", "EditorSDK Fixture", "1.0.0", true};
        return descriptor;
    }

    void RegisterInspectors() override
    {
        GameEngine::InspectorRegistry::Get().RegisterComponentInspectorByTypeId(
            kFixtureTypeId, [](const GameEngine::InspectorContext&) {});
    }
};

struct FixtureRegistrar
{
    FixtureRegistrar()
    {
        GameEngine::Editor::EditorComponentTraits traits;
        traits.DisplayName = "EditorSDK Fixture";
        traits.HierarchyRowClass = "hierarchy-entity-editor-sdk-fixture";
        GameEngine::Editor::EditorComponentTraitsRegistry::Get().Register(kFixtureTypeId,
                                                                          std::move(traits));

        static FixtureEditorPlugin plugin;
        GameEngine::Editor::EditorPluginRegistry::Get().RegisterPlugin(plugin);

        GameEngine::Editor::BuildExporterDescriptor exporter;
        exporter.ExporterId = "editorSdkFixture.exporter";
        exporter.DisplayName = "EditorSDK fixture export";
        exporter.HandlesPlatform = [](std::string_view name) { return name == "editorSdkFixture.platform"; };
        exporter.Run = [](const GameEngine::BuildSettings& settings, GameEngine::BuildPipeline& pipeline,
                          const GameEngine::Editor::BuildExporterDescriptor::ProgressCallback& progress) {
            GameEngine::BuildProgress report;
            report.cancelled = pipeline.IsCancelled();
            report.currentStage = report.cancelled ? GameEngine::BuildProgress::Stage::Failed
                                                   : GameEngine::BuildProgress::Stage::Complete;
            report.progress = 1.0f;
            report.statusMessage = settings.platformName;
            progress(report);
            return !report.cancelled;
        };
        GameEngine::Editor::BuildExporterRegistry::Get().Register(std::move(exporter));
    }
};

FixtureRegistrar s_Registrar;

} // namespace
)cpp";

std::map<std::string, std::string> LoadConfigOrFail()
{
    const fs::path exeDir = GameEngine::PathUtils::GetExecutableDirectory();
    std::string cfgErr;
    auto kv = ReadConfigFile(exeDir / "editorsdk_config.txt", cfgErr);
    EXPECT_TRUE(cfgErr.empty()) << cfgErr;
    for (const char* required :
         {"cmake", "importlib", "editorimportlib", "config", "sdkentry", "workdir", "includes",
          "editorincludes"})
        EXPECT_TRUE(kv.count(required)) << "missing config key: " << required;
    return kv;
}

// The Editor-kind module build shape — exactly what the editor's package
// wiring produces: engine interface + EditorSDK import lib + editor headers.
ns::NativeBuildConfig MakeEditorModuleConfig(const std::map<std::string, std::string>& kv,
                                             const fs::path& root,
                                             const fs::path& sourceDir,
                                             const std::string& moduleName)
{
    ns::NativeBuildConfig config;
    config.SourceDir = sourceDir;
    config.BuildDir = root / "build";
    config.ActiveDir = root / "active";
    config.CMakeExe = kv.at("cmake");
    config.Config = kv.at("config");
    config.EngineImportLib = kv.at("importlib");
    config.SdkEntrySource = kv.at("sdkentry");
    config.ModuleName = moduleName;
    for (const auto& d : SplitList(kv.at("includes"), ';'))
        config.IncludeDirs.emplace_back(d);
    if (kv.count("sdkinc"))
        config.IncludeDirs.emplace_back(kv.at("sdkinc"));
    for (const auto& d : SplitList(kv.count("defs") ? kv.at("defs") : "", ';'))
        config.CompileDefinitions.push_back(d);
    for (const auto& d : SplitList(kv.count("globaldefs") ? kv.at("globaldefs") : "", ';'))
        config.CompileDefinitions.push_back(d);
    config.EditorImportLib = kv.at("editorimportlib");
    for (const auto& d : SplitList(kv.at("editorincludes"), ';'))
        config.EditorIncludeDirs.emplace_back(d);
    return config;
}

void RunAndExpectSuccess(ns::NativeScriptManager& manager, const ns::NativeBuildConfig& config,
                         ns::NativeBuildResult* outResult = nullptr)
{
    const ns::NativeBuildResult result = manager.BuildAndLoad(config);
    if (outResult)
        *outResult = result;
    ASSERT_TRUE(result.Success()) << "BuildAndLoad failed: " << result.Error
                                  << "\n--- build output tail ---\n"
                                  << (result.Output.size() > 4000
                                          ? result.Output.substr(result.Output.size() - 4000)
                                          : result.Output);
}

bool PluginRegisteredWithId(std::string_view id)
{
    const std::vector<ed::IEditorPlugin*> plugins = ed::EditorPluginRegistry::Get().GetPlugins();
    return std::any_of(plugins.begin(), plugins.end(), [id](ed::IEditorPlugin* p) {
        return p && std::string_view(p->GetDescriptor().Id) == id;
    });
}

} // namespace

// The ai-assistant package's Editor-kind module builds from source against the
// Engine + EditorSDK interfaces and registers its panel, docked beside the
// Scene View the first time it opens.
TEST(EditorModuleBuildLoad, AiAssistantEditorModuleBuildsAndRegistersPanel)
{
    const auto kv = LoadConfigOrFail();
    const fs::path exeDir = GameEngine::PathUtils::GetExecutableDirectory();
    const fs::path sourceDir = exeDir / "TestData" / "Packages" / "ai-assistant" / "Editor";
    ASSERT_TRUE(fs::is_directory(sourceDir)) << sourceDir.string();

    const fs::path root = fs::path(kv.at("workdir")) / "ai-assistant";
    std::error_code ec;
    fs::remove_all(root, ec);

    const ns::NativeBuildConfig config =
        MakeEditorModuleConfig(kv, root, sourceDir, "AiAssistant.Editor");

    ns::NativeScriptManager manager;
    ASSERT_TRUE(manager.Initialize());
    RunAndExpectSuccess(manager, config);

    ed::EditorPanelDescriptor panel;
    ASSERT_TRUE(ed::EditorPanelRegistry::Get().TryGetPanel("AIAssistant", panel))
        << "the package module must register the AI Assistant panel";
    EXPECT_EQ(panel.Title, "AI Assistant");
    EXPECT_EQ(panel.AssetSourceAlias, "ai-assistant");
    EXPECT_EQ(panel.DefaultDockAnchorPanelId, "SceneView");
    ASSERT_TRUE(panel.Factory);
    {
        const std::unique_ptr<GameEngine::UIElement> element = panel.Factory();
        auto* dockPanel = dynamic_cast<GameEngine::DockPanel*>(element.get());
        ASSERT_NE(dockPanel, nullptr) << "package panels must be DockPanels";
        EXPECT_EQ(dockPanel->GetTitle(), "AI Assistant");
    }

    manager.Shutdown();
}

TEST(EditorModuleBuildLoad, EditorKindModuleBuildsAgainstEditorSdkAndRegisters)
{
    const auto kv = LoadConfigOrFail();
    const fs::path root = fs::path(kv.at("workdir")) / "fixture";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path sourceDir = root / "EditorModule";
    fs::create_directories(sourceDir, ec);
    ASSERT_FALSE(ec) << ec.message();
    {
        std::ofstream src(sourceDir / "FixtureEditorModule.cpp", std::ios::binary);
        ASSERT_TRUE(src.is_open());
        src << kFixtureModuleSource;
    }

    const ns::NativeBuildConfig config = MakeEditorModuleConfig(kv, root, sourceDir, "FixturePack.Editor");

    // Real ordering: the editor's startup inspector pass has already run when
    // package modules load at project open.
    ed::EditorPluginRegistry::Get().RegisterInspectors();
    ASSERT_EQ(GameEngine::InspectorRegistry::Get().TryGetComponentInspector(0xEDF1), nullptr)
        << "fixture inspector must not exist before the module loads";

    ns::NativeScriptManager manager;
    ASSERT_TRUE(manager.Initialize());
    ns::NativeBuildResult buildResult;
    RunAndExpectSuccess(manager, config, &buildResult);

    EXPECT_EQ(buildResult.ModuleId, config.ModuleName);
    EXPECT_TRUE(buildResult.RegistrationsChanged)
        << "first package load must report newly registered editor extensions";

    // The plugin registered into the process-wide registry (via EditorSDK.dll).
    EXPECT_TRUE(PluginRegisteredWithId("editorSdkFixture"))
        << "fixture plugin must be registered in the shared registry";

    // Traits registered at module static init.
    ed::EditorComponentTraits traits;
    ASSERT_TRUE(ed::EditorComponentTraitsRegistry::Get().TryGet(0xEDF1, traits));
    EXPECT_EQ(traits.DisplayName, "EditorSDK Fixture");
    EXPECT_EQ(traits.HierarchyRowClass, "hierarchy-entity-editor-sdk-fixture");

    // The inspector arrived through the late-plugin replay (the pass ran
    // before the module existed).
    EXPECT_NE(GameEngine::InspectorRegistry::Get().TryGetComponentInspector(0xEDF1), nullptr)
        << "late-registered plugin must have its inspector pass replayed";
    EXPECT_TRUE(GameEngine::InspectorRegistry::Get().IsComponentInspectorOwnedBy(
        0xEDF1, config.ModuleName))
        << "late package inspector must retain module ownership for targeted panel refresh";

    // The DLL and headless host must see one SDK-owned exporter registry. A
    // private copy compiled into either side would hide this registration.
    ed::BuildExporterDescriptor exporter;
    ASSERT_TRUE(ed::BuildExporterRegistry::Get().TryFindForPlatform("editorSdkFixture.platform", exporter));
    EXPECT_EQ(exporter.ExporterId, "editorSdkFixture.exporter");
    EXPECT_EQ(exporter.DisplayName, "EditorSDK fixture export");
    GameEngine::BuildSettings settings;
    settings.platformName = "editorSdkFixture.platform";
    GameEngine::BuildPipeline pipeline(nullptr);
    GameEngine::BuildProgress report;
    int reports = 0;
    const auto progress = [&](const GameEngine::BuildProgress& value) { report = value; ++reports; };
    EXPECT_TRUE(exporter.Run(settings, pipeline, progress));
    EXPECT_EQ(reports, 1);
    EXPECT_EQ(report.currentStage, GameEngine::BuildProgress::Stage::Complete);
    EXPECT_EQ(report.statusMessage, settings.platformName);
    EXPECT_FALSE(report.cancelled);
    pipeline.RequestCancel();
    EXPECT_FALSE(exporter.Run(settings, pipeline, progress));
    EXPECT_EQ(reports, 2);
    EXPECT_EQ(report.currentStage, GameEngine::BuildProgress::Stage::Failed);
    EXPECT_TRUE(report.cancelled);

    manager.Shutdown();
}

// The real customer: the eztree package's Editor-kind module — the migrated
// EZTreeEditorPlugin — compiles and links against ONLY the Engine + EditorSDK
// interfaces, loads through the standard pipeline, and registers the Tree
// Generator plugin, its component traits, and its pick provider into the
// shared registries. This is the phase-1 acceptance proof for the EZTreeEditor
// migration (the live-editor visual leg runs separately).
TEST(EditorModuleBuildLoad, EztreeEditorModuleBuildsAndRegistersRealPlugin)
{
    const auto kv = LoadConfigOrFail();
    const fs::path exeDir = GameEngine::PathUtils::GetExecutableDirectory();
    const fs::path pkgRoot = exeDir / "TestData" / "Packages" / "eztree";
    ASSERT_TRUE(fs::is_directory(pkgRoot / "Editor")) << (pkgRoot / "Editor").string();
    ASSERT_TRUE(fs::is_directory(pkgRoot / "Native"));

    const fs::path root = fs::path(kv.at("workdir")) / "eztree";
    std::error_code ec;
    fs::remove_all(root, ec);

    ns::NativeBuildConfig config = MakeEditorModuleConfig(kv, root, pkgRoot / "Editor", "Eztree.Editor");
    // The sibling Runtime module's headers — the editor wiring adds the peer
    // root for editor halves that extend their package's runtime half.
    config.IncludeDirs.push_back(pkgRoot / "Native");

    ASSERT_FALSE(PluginRegisteredWithId("ezTree")) << "eztree plugin must arrive via the module load";

    ns::NativeScriptManager manager;
    ASSERT_TRUE(manager.Initialize());
    RunAndExpectSuccess(manager, config);

    EXPECT_TRUE(PluginRegisteredWithId("ezTree"))
        << "the migrated Tree Generator plugin must register from the package module";

    // Traits registered at module load: find by display name (the test does
    // not compile against the package's component headers).
    const auto traitsSnapshot = ed::EditorComponentTraitsRegistry::Get().Snapshot();
    const auto treeTraits = std::find_if(traitsSnapshot.begin(), traitsSnapshot.end(),
                                         [](const auto& entry) {
                                             return entry.second.DisplayName == "Tree Generator";
                                         });
    ASSERT_NE(treeTraits, traitsSnapshot.end());
    EXPECT_EQ(treeTraits->second.HierarchyRowClass, "hierarchy-entity-tree-generator");
    EXPECT_EQ(treeTraits->second.InspectorIconClass, "inspector-section-icon-tree-generator");
    EXPECT_TRUE(treeTraits->second.IsPickInstanceRoot);
    EXPECT_TRUE(treeTraits->second.GizmoPivotAtEntityOrigin);

    // Phase 2: the module registered its Tree Stats panel descriptor with the
    // package-mount-scoped UI asset paths, and its factory produces a real
    // DockPanel headless (no UIManager needed until first show).
    ed::EditorPanelDescriptor statsPanel;
    ASSERT_TRUE(ed::EditorPanelRegistry::Get().TryGetPanel("EZTreeStats", statsPanel))
        << "the package module must register its panel descriptor";
    EXPECT_EQ(statsPanel.Title, "Tree Stats");
    EXPECT_EQ(statsPanel.TabIconClass, "eztree-stats-tab-icon");
    EXPECT_EQ(statsPanel.AssetSourceAlias, "eztree");
    EXPECT_EQ(statsPanel.LayoutAssetPath, "Editor/UI/panels/EZTreeStatsPanel.uxml");
    EXPECT_EQ(statsPanel.StyleAssetPath, "Editor/UI/panels/EZTreeStatsPanel.css");
    ASSERT_TRUE(statsPanel.Factory);
    {
        const std::unique_ptr<GameEngine::UIElement> panel = statsPanel.Factory();
        auto* dockPanel = dynamic_cast<GameEngine::DockPanel*>(panel.get());
        ASSERT_NE(dockPanel, nullptr) << "package panels must be DockPanels";
        EXPECT_EQ(dockPanel->GetTitle(), "Tree Stats");
    }

    // ...and its editor-chrome stylesheet contribution (hierarchy/inspector/
    // search icon classes moved out of the editor theme into the package).
    const auto sheets = ed::EditorPanelRegistry::Get().StyleSheetSnapshot();
    EXPECT_TRUE(std::any_of(sheets.begin(), sheets.end(),
                            [](const ed::EditorStyleSheetContribution& c) {
                                return c.AssetSourceAlias == "eztree" &&
                                       c.StyleAssetPath == "Editor/UI/EZTreeEditorChrome.css";
                            }))
        << "the package module must contribute its editor-chrome stylesheet";

    manager.Shutdown();
}

// The packaged Unity importer: the unity-import package's Editor-kind module
// — the migrated UnityImportModal — compiles and links against ONLY the
// Engine + EditorSDK interfaces (the hosted-converter seam is an EditorSDK
// export precisely so this build shape never touches the NETHOST-layout-
// sensitive scripting headers), loads through the standard pipeline, and
// registers its Tools-menu item and UI-root overlay. Headless: the overlay
// factory builds the modal UI in code (no UIManager needed until first
// show); the converter DLL itself is exercised by the live-editor leg.
TEST(EditorModuleBuildLoad, UnityImportEditorModuleBuildsAndRegistersMenuAndOverlay)
{
    const auto kv = LoadConfigOrFail();
    const fs::path exeDir = GameEngine::PathUtils::GetExecutableDirectory();
    const fs::path pkgRoot = exeDir / "TestData" / "Packages" / "unity-import";
    ASSERT_TRUE(fs::is_directory(pkgRoot / "Editor")) << (pkgRoot / "Editor").string();

    const fs::path root = fs::path(kv.at("workdir")) / "unity-import";
    std::error_code ec;
    fs::remove_all(root, ec);

    const ns::NativeBuildConfig config =
        MakeEditorModuleConfig(kv, root, pkgRoot / "Editor", "UnityImport.Editor");

    ns::NativeScriptManager manager;
    ASSERT_TRUE(manager.Initialize());
    RunAndExpectSuccess(manager, config);

    // The Tools-menu item registered with a native-range command id.
    const auto menuItems = ed::EditorMenuRegistry::Get().Snapshot();
    const auto menuIt = std::find_if(menuItems.begin(), menuItems.end(),
                                     [](const ed::EditorMenuItemSnapshot& i) {
                                         return i.Path == "Tools/Import Unity Package...";
                                     });
    ASSERT_NE(menuIt, menuItems.end())
        << "the package module must register its Tools-menu item";
    EXPECT_GE(menuIt->CommandId, 0x6000u);
    EXPECT_LE(menuIt->CommandId, 0x6FFFu);

    // The modal overlay registered and its factory produces the element
    // headless (the editor's consumer attaches it to the UI root).
    const auto overlays = ed::EditorPanelRegistry::Get().OverlaySnapshot();
    const auto overlayIt = std::find_if(overlays.begin(), overlays.end(),
                                        [](const ed::EditorOverlayDescriptor& d) {
                                            return d.OverlayId == "unityImport.modal";
                                        });
    ASSERT_NE(overlayIt, overlays.end())
        << "the package module must register its modal overlay";
    ASSERT_TRUE(overlayIt->Factory);
    {
        const std::unique_ptr<GameEngine::UIElement> modal = overlayIt->Factory();
        EXPECT_NE(modal, nullptr) << "the overlay factory must build the modal headless";
    }

    // The factory-produced modal was destroyed above and the module must have
    // untracked it: dispatching the menu command now takes the loud no-modal
    // path (error log + return) instead of touching the dead element.
    EXPECT_TRUE(ed::EditorMenuRegistry::Get().TryInvoke(menuIt->CommandId));

    manager.Shutdown();
}

// The extracted Git provider — the DEFAULT provider, DetectionOrder 0: the
// git-vcs package's Editor-kind module compiles against ONLY the Engine +
// EditorSDK interfaces, loads through the standard pipeline, and registers
// its provider descriptor with the legacy precedence slot, the Push menu
// extra, the provider-owned default commit message, and its chrome
// stylesheet. Headless: Detect must miss without a `.git` marker and hit for
// BOTH a `.git` directory and a `.git` file (worktree/submodule); no git.exe
// is required.
TEST(EditorModuleBuildLoad, GitVcsEditorModuleBuildsAndRegistersProvider)
{
    const auto kv = LoadConfigOrFail();
    const fs::path exeDir = GameEngine::PathUtils::GetExecutableDirectory();
    const fs::path pkgRoot = exeDir / "TestData" / "Packages" / "git-vcs";
    ASSERT_TRUE(fs::is_directory(pkgRoot / "Editor")) << (pkgRoot / "Editor").string();

    const fs::path root = fs::path(kv.at("workdir")) / "gitvcs";
    std::error_code ec;
    fs::remove_all(root, ec);

    const ns::NativeBuildConfig config =
        MakeEditorModuleConfig(kv, root, pkgRoot / "Editor", "GitVcs.Editor");

    ed::EditorVcsProviderDescriptor provider;
    ASSERT_FALSE(ed::EditorVcsProviderRegistry::Get().TryGet("git", provider))
        << "the git provider must arrive via the module load — no built-in registration remains";

    ns::NativeScriptManager manager;
    ASSERT_TRUE(manager.Initialize());
    RunAndExpectSuccess(manager, config);

    ASSERT_TRUE(ed::EditorVcsProviderRegistry::Get().TryGet("git", provider));
    EXPECT_EQ(provider.DisplayName, "Git");
    EXPECT_EQ(provider.SettingsRowClass, "git-row");
    EXPECT_EQ(provider.StatusColumnTitle, "Git Status");
    EXPECT_EQ(provider.DetectionOrder, 0) << "git must keep the first-detect slot";
    EXPECT_FALSE(provider.ServerBacked);
    EXPECT_EQ(provider.UpdateActionLabel, "Pull");
    EXPECT_TRUE(static_cast<bool>(provider.GetBaseContent)) << "git show HEAD backs the diff view";
    EXPECT_TRUE(static_cast<bool>(provider.DefaultCommitMessage))
        << "the commit dialog's default message is provider-owned";

    // Detection: miss without .git; hit for a directory AND a plain file
    // (worktree/submodule marker).
    const fs::path workspace = root / "workspace";
    fs::create_directories(workspace, ec);
    ASSERT_TRUE(provider.Detect);
    EXPECT_FALSE(provider.Detect(workspace));
    fs::create_directories(workspace / ".git", ec);
    EXPECT_TRUE(provider.Detect(workspace));
    const fs::path fileMarkerWorkspace = root / "worktree";
    fs::create_directories(fileMarkerWorkspace, ec);
    {
        std::ofstream marker(fileMarkerWorkspace / ".git");
        marker << "gitdir: elsewhere\n";
    }
    EXPECT_TRUE(provider.Detect(fileMarkerWorkspace)) << "a .git FILE is a valid marker";

    // The Push context-menu extra rides the descriptor (directory menus only).
    ASSERT_TRUE(provider.CollectMenuItems);
    {
        std::vector<ed::VcsMenuItem> items;
        provider.CollectMenuItems(/*isDirectory=*/true, items);
        ASSERT_EQ(items.size(), 1u);
        EXPECT_EQ(items[0].Label, "Push");
        items.clear();
        provider.CollectMenuItems(/*isDirectory=*/false, items);
        EXPECT_TRUE(items.empty());
    }

    ASSERT_TRUE(provider.BadgeUiSettings);
    EXPECT_TRUE(provider.BadgeUiSettings().ShowStatusIcons);

    ASSERT_TRUE(provider.BuildSettingsContent);
    {
        GameEngine::ScrollView contentBody;
        provider.BuildSettingsContent(contentBody);
        EXPECT_FALSE(contentBody.GetChildren().empty())
            << "the Git settings tab must produce content";
    }

    const auto sheets = ed::EditorPanelRegistry::Get().StyleSheetSnapshot();
    EXPECT_TRUE(std::any_of(sheets.begin(), sheets.end(),
                            [](const ed::EditorStyleSheetContribution& c) {
                                return c.AssetSourceAlias == "git-vcs" &&
                                       c.StyleAssetPath == "Editor/UI/GitVcsChrome.css";
                            }))
        << "the package module must contribute its chrome stylesheet";

    manager.Shutdown();
}

// The extracted SVN provider: the svn-vcs package's Editor-kind module
// compiles against ONLY the Engine + EditorSDK interfaces, loads through the
// standard pipeline, and registers its provider descriptor with the legacy
// detection order (10 — between packaged Git at 0 and Diversion at 20) plus
// its chrome stylesheet. Headless: Detect must miss without a `.svn` marker
// and hit with one; no svn.exe is required.
TEST(EditorModuleBuildLoad, SvnVcsEditorModuleBuildsAndRegistersProvider)
{
    const auto kv = LoadConfigOrFail();
    const fs::path exeDir = GameEngine::PathUtils::GetExecutableDirectory();
    const fs::path pkgRoot = exeDir / "TestData" / "Packages" / "svn-vcs";
    ASSERT_TRUE(fs::is_directory(pkgRoot / "Editor")) << (pkgRoot / "Editor").string();

    const fs::path root = fs::path(kv.at("workdir")) / "svnvcs";
    std::error_code ec;
    fs::remove_all(root, ec);

    const ns::NativeBuildConfig config =
        MakeEditorModuleConfig(kv, root, pkgRoot / "Editor", "SvnVcs.Editor");

    ed::EditorVcsProviderDescriptor provider;
    ASSERT_FALSE(ed::EditorVcsProviderRegistry::Get().TryGet("svn", provider))
        << "the svn provider must arrive via the module load";

    ns::NativeScriptManager manager;
    ASSERT_TRUE(manager.Initialize());
    RunAndExpectSuccess(manager, config);

    // The provider descriptor registered into the process-wide registry with
    // the legacy precedence slot intact (mixed workspaces: git > svn > ...).
    ASSERT_TRUE(ed::EditorVcsProviderRegistry::Get().TryGet("svn", provider));
    EXPECT_EQ(provider.DisplayName, "SVN");
    EXPECT_EQ(provider.SettingsRowClass, "svn-row");
    EXPECT_EQ(provider.StatusColumnTitle, "SVN Status");
    EXPECT_EQ(provider.DetectionOrder, 10);
    EXPECT_FALSE(provider.ServerBacked);
    EXPECT_EQ(provider.UpdateActionLabel, "Update");
    EXPECT_TRUE(static_cast<bool>(provider.GetBaseContent))
        << "svn cat -r BASE backs the diff view";

    // Detection: a cheap marker probe — miss without .svn, hit with it.
    const fs::path workspace = root / "workspace";
    fs::create_directories(workspace, ec);
    ASSERT_TRUE(provider.Detect);
    EXPECT_FALSE(provider.Detect(workspace)) << "detect-miss on a non-SVN dir is the honest result";
    fs::create_directories(workspace / ".svn", ec);
    EXPECT_TRUE(provider.Detect(workspace));

    // Badge prefs load from the provider's settings (defaults headless).
    ASSERT_TRUE(provider.BadgeUiSettings);
    const ed::VcsBadgeUiSettings badge = provider.BadgeUiSettings();
    EXPECT_TRUE(badge.ShowStatusIcons);

    // The settings tab builds headless into a plain ScrollView.
    ASSERT_TRUE(provider.BuildSettingsContent);
    {
        GameEngine::ScrollView contentBody;
        provider.BuildSettingsContent(contentBody);
        EXPECT_FALSE(contentBody.GetChildren().empty())
            << "the SVN settings tab must produce content";
    }

    // ...and the chrome stylesheet contribution (settings-tree row icon).
    const auto sheets = ed::EditorPanelRegistry::Get().StyleSheetSnapshot();
    EXPECT_TRUE(std::any_of(sheets.begin(), sheets.end(),
                            [](const ed::EditorStyleSheetContribution& c) {
                                return c.AssetSourceAlias == "svn-vcs" &&
                                       c.StyleAssetPath == "Editor/UI/SvnVcsChrome.css";
                            }))
        << "the package module must contribute its chrome stylesheet";

    manager.Shutdown();
}

// The extracted Diversion provider: the diversion-vcs package's Editor-kind
// module compiles against ONLY the Engine + EditorSDK interfaces, loads
// through the standard pipeline, and registers its provider descriptor with
// the legacy detection order (20 — after packaged Git at 0 and SVN
// at 10) plus its chrome stylesheet. Headless: Detect must miss without a
// `.diversion` marker and hit with one; no dv CLI is required.
TEST(EditorModuleBuildLoad, DiversionVcsEditorModuleBuildsAndRegistersProvider)
{
    const auto kv = LoadConfigOrFail();
    const fs::path exeDir = GameEngine::PathUtils::GetExecutableDirectory();
    const fs::path pkgRoot = exeDir / "TestData" / "Packages" / "diversion-vcs";
    ASSERT_TRUE(fs::is_directory(pkgRoot / "Editor")) << (pkgRoot / "Editor").string();

    const fs::path root = fs::path(kv.at("workdir")) / "diversionvcs";
    std::error_code ec;
    fs::remove_all(root, ec);

    const ns::NativeBuildConfig config =
        MakeEditorModuleConfig(kv, root, pkgRoot / "Editor", "DiversionVcs.Editor");

    ed::EditorVcsProviderDescriptor provider;
    ASSERT_FALSE(ed::EditorVcsProviderRegistry::Get().TryGet("diversion", provider))
        << "the diversion provider must arrive via the module load";

    ns::NativeScriptManager manager;
    ASSERT_TRUE(manager.Initialize());
    RunAndExpectSuccess(manager, config);

    // The provider descriptor registered into the process-wide registry with
    // the legacy precedence slot intact (git > svn > diversion > lore).
    ASSERT_TRUE(ed::EditorVcsProviderRegistry::Get().TryGet("diversion", provider));
    EXPECT_EQ(provider.DisplayName, "Diversion");
    EXPECT_EQ(provider.SettingsRowClass, "diversion-row");
    EXPECT_EQ(provider.StatusColumnTitle, "DV Status");
    EXPECT_EQ(provider.DetectionOrder, 20);
    EXPECT_TRUE(provider.ServerBacked);
    EXPECT_EQ(provider.UpdateActionLabel, "Sync");
    EXPECT_TRUE(static_cast<bool>(provider.GetBaseContent))
        << "dv cat backs the diff view";

    // Detection: a cheap marker probe — miss without .diversion, hit with it.
    const fs::path workspace = root / "workspace";
    fs::create_directories(workspace, ec);
    ASSERT_TRUE(provider.Detect);
    EXPECT_FALSE(provider.Detect(workspace))
        << "detect-miss on a non-Diversion dir is the honest result";
    fs::create_directories(workspace / ".diversion", ec);
    EXPECT_TRUE(provider.Detect(workspace));

    // Badge prefs load from the provider's settings (defaults headless).
    ASSERT_TRUE(provider.BadgeUiSettings);
    const ed::VcsBadgeUiSettings badge = provider.BadgeUiSettings();
    EXPECT_TRUE(badge.ShowStatusIcons);

    // The settings tab builds headless into a plain ScrollView.
    ASSERT_TRUE(provider.BuildSettingsContent);
    {
        GameEngine::ScrollView contentBody;
        provider.BuildSettingsContent(contentBody);
        EXPECT_FALSE(contentBody.GetChildren().empty())
            << "the Diversion settings tab must produce content";
    }

    // ...and the chrome stylesheet contribution (settings-tree row icon).
    const auto sheets = ed::EditorPanelRegistry::Get().StyleSheetSnapshot();
    EXPECT_TRUE(std::any_of(sheets.begin(), sheets.end(),
                            [](const ed::EditorStyleSheetContribution& c) {
                                return c.AssetSourceAlias == "diversion-vcs" &&
                                       c.StyleAssetPath == "Editor/UI/DiversionVcsChrome.css";
                            }))
        << "the package module must contribute its chrome stylesheet";

    manager.Shutdown();
}

// The phase-3 customer: the lore-vcs package's Editor-kind module — the
// extracted Lore VCS provider — compiles against ONLY the Engine + EditorSDK
// interfaces, loads through the standard pipeline, and registers its provider
// descriptor (detection, settings tab, badge prefs) plus its chrome
// stylesheet into the shared registries. Headless proof for the VCS seam; a
// real Lore workspace is NOT required — Detect must simply miss on non-Lore
// dirs and hit on a `.lore` (or legacy `.urc`) marker.
TEST(EditorModuleBuildLoad, LoreVcsEditorModuleBuildsAndRegistersProvider)
{
    const auto kv = LoadConfigOrFail();
    const fs::path exeDir = GameEngine::PathUtils::GetExecutableDirectory();
    const fs::path pkgRoot = exeDir / "TestData" / "Packages" / "lore-vcs";
    ASSERT_TRUE(fs::is_directory(pkgRoot / "Editor")) << (pkgRoot / "Editor").string();

    const fs::path root = fs::path(kv.at("workdir")) / "lorevcs";
    std::error_code ec;
    fs::remove_all(root, ec);

    const ns::NativeBuildConfig config =
        MakeEditorModuleConfig(kv, root, pkgRoot / "Editor", "LoreVcs.Editor");

    ed::EditorVcsProviderDescriptor provider;
    ASSERT_FALSE(ed::EditorVcsProviderRegistry::Get().TryGet("lore", provider))
        << "the lore provider must arrive via the module load";

    ns::NativeScriptManager manager;
    ASSERT_TRUE(manager.Initialize());
    RunAndExpectSuccess(manager, config);

    // The provider descriptor registered into the process-wide registry.
    ASSERT_TRUE(ed::EditorVcsProviderRegistry::Get().TryGet("lore", provider));
    EXPECT_EQ(provider.DisplayName, "Lore");
    EXPECT_EQ(provider.SettingsRowClass, "lore-row");
    EXPECT_EQ(provider.StatusColumnTitle, "Lore Status");
    EXPECT_EQ(provider.DetectionOrder, 30);
    EXPECT_TRUE(provider.ServerBacked);
    EXPECT_EQ(provider.UpdateActionLabel, "Sync");
    EXPECT_TRUE(static_cast<bool>(provider.GetBaseContent))
        << "lore file write backs the diff view";

    // Detection: a cheap marker probe — miss without a marker, hit on `.lore`
    // and on the pre-rename `.urc` the CLI still opens.
    const fs::path workspace = root / "workspace";
    fs::create_directories(workspace, ec);
    ASSERT_TRUE(provider.Detect);
    EXPECT_FALSE(provider.Detect(workspace)) << "detect-miss on a non-Lore dir is the honest result";
    fs::create_directories(workspace / ".lore", ec);
    EXPECT_TRUE(provider.Detect(workspace));
    const fs::path legacyWorkspace = root / "legacy-workspace";
    fs::create_directories(legacyWorkspace / ".urc", ec);
    EXPECT_TRUE(provider.Detect(legacyWorkspace));

    // Badge prefs load from the provider's settings (defaults headless).
    ASSERT_TRUE(provider.BadgeUiSettings);
    const ed::VcsBadgeUiSettings badge = provider.BadgeUiSettings();
    EXPECT_TRUE(badge.ShowStatusIcons);

    // The settings tab builds headless into a plain ScrollView.
    ASSERT_TRUE(provider.BuildSettingsContent);
    {
        GameEngine::ScrollView contentBody;
        provider.BuildSettingsContent(contentBody);
        EXPECT_FALSE(contentBody.GetChildren().empty())
            << "the Lore settings tab must produce content";
    }

    // ...and the chrome stylesheet contribution (settings-tree row icon).
    const auto sheets = ed::EditorPanelRegistry::Get().StyleSheetSnapshot();
    EXPECT_TRUE(std::any_of(sheets.begin(), sheets.end(),
                            [](const ed::EditorStyleSheetContribution& c) {
                                return c.AssetSourceAlias == "lore-vcs" &&
                                       c.StyleAssetPath == "Editor/UI/LoreVcsChrome.css";
                            }))
        << "the package module must contribute its chrome stylesheet";

    manager.Shutdown();
}
