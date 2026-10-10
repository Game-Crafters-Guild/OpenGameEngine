#include "UI/Controls/Button.h"
#include "UI/UIManager.h"
#include "UI/UIHotReload.h"
#include "UI/ResolvedStyle.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/StyleProperties.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/Controls/AccordionItem.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Mount.h"
#include "UI/Controls/TextField.h"
#include "Rendering/Core/Device.h"
#include "UIRgTestHarness.h"
#include "Assets/AssetManager.h"
#include "AssetCore/AssetEvents.h"

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <cstdlib>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

static std::filesystem::path MakeTempCss(const char* name, const char* contents)
{
    auto dir = std::filesystem::temp_directory_path() / "ui_hot_reload_tests";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    std::ofstream f(path);
    f << contents;
    return path;
}

static std::filesystem::path MakeTempUxml(const char* name, const char* contents)
{
    auto dir = std::filesystem::temp_directory_path() / "ui_hot_reload_tests";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    std::ofstream f(path);
    f << contents;
    return path;
}

struct TemporaryStyleDirectory
{
    const std::filesystem::path Path = std::filesystem::temp_directory_path() /
                                       ("ui-style-test-" + GUID::Generate().ToString());
    TemporaryStyleDirectory() { std::filesystem::create_directories(Path); }
    ~TemporaryStyleDirectory()
    {
        std::error_code ec;
        std::filesystem::remove_all(Path, ec);
    }
};

static void SaveStyle(AssetManager& assets, const std::filesystem::path& path, const char* contents)
{
    assets.SetHotReloadEnabled(true);
    auto save = assets.ExpectWrite(path);
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << contents;
    }
    save.Report(path);
    assets.Update();
}

} // namespace

// Basic smoke test: a UIStyle that imports another style should not crash ApplyStyleReload
// and should visit both importer and imported GUIDs.
TEST(UIHotReloadTests, ImportGraphTraversal_NoCrash)
{
	    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }

    UIRegistration::RegisterBuiltInControls();

    // Build minimal UI tree.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");

	UIManager ui(dev.get());
	ui.SetRoot(std::move(root));

	// Set up a small AssetManager rooted at a temp dir so UIStyleAsset can resolve imports.
	auto assetRoot = std::filesystem::temp_directory_path() / "ui_hot_reload_assets";
	std::filesystem::create_directories(assetRoot);

	JobSystem::WorkStealingThreadPool pool(2);
	AssetManager assets;
	ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

	// Create two styles: A imports B.
	auto pathB = MakeTempCss("style_b.css", "#root { color: red; }\n");
	auto pathA = MakeTempCss("style_a.css", "@import \"style_b.css\";\n#root { background-color: blue; }\n");

	// Register style assets so the asset pipeline knows about them and UIStyleAsset
	// can discover imported GUIDs during load/reload.
	auto& reg = assets.GetRegistry();
	ASSERT_TRUE(reg.RegisterAsset(pathB));
	ASSERT_TRUE(reg.RegisterAsset(pathA));

	const GUID guidB = reg.GetAssetGUID(pathB);
	const GUID guidA = reg.GetAssetGUID(pathA);
	ASSERT_FALSE(guidA.IsNull());
	ASSERT_FALSE(guidB.IsNull());

	// Load UIStyleAsset instances via the unified async API and block for completion so
	// GetImportedStyleGuids is populated.
	auto futureB = assets.LoadAssetAsync(guidB, AssetLoadPriority::Normal);
	auto futureA = assets.LoadAssetAsync(guidA, AssetLoadPriority::Normal);
	auto assetB = futureB.get();
	auto assetA = futureA.get();
	ASSERT_TRUE(assetA);
	ASSERT_TRUE(assetB);

	auto* styleB = dynamic_cast<UIStyleAsset*>(assetB.get());
	auto* styleA = dynamic_cast<UIStyleAsset*>(assetA.get());
	ASSERT_NE(styleA, nullptr);
	ASSERT_NE(styleB, nullptr);
	ASSERT_FALSE(styleA->GetImportedStyleGuids().empty());

	const GUID importedGuid = styleA->GetImportedStyleGuids().front();
	ASSERT_FALSE(importedGuid.IsNull());

	// Attach A globally so UIHotReload will track it and its imports.
	ui.AddStylesheet(styleA->GetStylesheetHandle());
	auto hotReload = std::make_unique<UIHotReload>(ui, assets);

	hotReload->RegisterStyleBinding(guidA, /*target=*/nullptr, UIHotReload::StyleBindMode::Global);

	// Exercise ApplyStyleReload on the imported GUID; this used to crash when mutating
	// the import graph while iterating.
	ASSERT_NO_THROW(hotReload->ApplyStyleNow(importedGuid));
}

TEST(UIHotReloadTests, AssetSaveReloadsStyleAndAppliesOnNextUpdate)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    // Set up a small AssetManager rooted at a temp dir so UIStyleAsset can resolve.
    auto assetRoot = std::filesystem::temp_directory_path() / "ui_hot_reload_assets_modified";
    std::filesystem::create_directories(assetRoot);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    // Create a style that controls root width.
    auto pathA = MakeTempCss("style_modified.css", "#root { width: 100px; height: 20px; }\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(pathA));
    const GUID guidA = reg.GetAssetGUID(pathA);
    ASSERT_FALSE(guidA.IsNull());

    auto futureA = assets.LoadAssetAsync(guidA, AssetLoadPriority::Normal);
    auto assetA = futureA.get();
    ASSERT_TRUE(assetA);
    auto* styleA = dynamic_cast<UIStyleAsset*>(assetA.get());
    ASSERT_NE(styleA, nullptr);

    // Build minimal UI tree and attach style via UIManager's asset-driven path (installs UIHotReload).
    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    UIManager ui(dev.get(), &assets);
    ui.SetRoot(std::move(root));
    ASSERT_TRUE(ui.AttachStyleFromAsset(*styleA));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* liveRoot = ui.GetRootElement();
    ASSERT_NE(liveRoot, nullptr);
    EXPECT_NEAR(liveRoot->GetLayoutWidth(), 100.0f, 1.0f);

    SaveStyle(assets, pathA, "#root { width: 200px; height: 20px; }\n");

    // Next UI update adopts the snapshot already committed by AssetManager.
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);
    EXPECT_NEAR(liveRoot->GetLayoutWidth(), 200.0f, 1.0f);
}

class SharedStylesheetReloadTests : public testing::TestWithParam<bool> {};

TEST_P(SharedStylesheetReloadTests, ManagersObserveOneCommitAndKeepCachedRulesAlive)
{
    auto dev = MakeHeadlessDevice();
    ASSERT_NE(dev, nullptr) << "A headless device is required to exercise the real cascade";
    TemporaryStyleDirectory directory;
    const auto& assetRoot = directory.Path;
    const auto path = assetRoot / "shared.css";
    const auto write = [&](const char* color)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "#root { width: 300px; height: 80px; } "
                "#sample { width: 100px; height: 40px; } "
                "#sample:hover { background-color: " << color << "; }";
    };
    write("red");
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));
    assets.WaitForStartupScan();
    ASSERT_TRUE(assets.GetRegistry().RegisterAsset(path));
    const GUID guid = assets.GetRegistry().GetAssetGUID(path);
    ASSERT_FALSE(guid.IsNull());
    auto loaded = assets.LoadAssetAsync(guid, AssetLoadPriority::Normal).get();
    auto* style = dynamic_cast<UIStyleAsset*>(loaded.get());
    ASSERT_NE(style, nullptr);
    assets.SetHotReloadEnabled(true);

    UIManager first(dev.get(), &assets), second(dev.get(), &assets);
    for (UIManager* ui : {&first, &second})
    {
        std::unique_ptr<UIElement> root;
        ASSERT_TRUE(UIParsing::XMLParser::ParseLayoutFromString(
            "<uielement id='root'><uielement id='sample'/></uielement>", root));
        ui->SetRoot(std::move(root));
        ui->OnMouseMove(-100.0f, -100.0f);
    }
    ASSERT_TRUE(first.AttachStyleFromAsset(*style));
    if (GetParam()) ASSERT_TRUE(second.AttachStyleFromAsset(*style));
    first.Update(0.0f, true);
    second.Update(0.0f, true);
    auto* sample = first.GetRootElement()->FindById("sample");
    ASSERT_NE(sample, nullptr);
    first.OnMouseMove(10.0f, 10.0f);
    first.Update(0.0f, true);
    ASSERT_EQ(sample->GetResolvedStyle().Visual.BackgroundColor, 0xFFFF0000u);
    const auto previous = first.GetStylesheets();

    auto save = assets.ExpectWrite(path);
    write("blue");
    save.Report(path);
    assets.Update();
    const auto committed = style->GetCascadeHandles(assets);
    ASSERT_NE(committed, previous);
    EXPECT_EQ(first.GetStylesheets(), previous);
    // Native pointer handoff can clear hover before either manager pumps the
    // reload event. The old rule snapshot must still support that immediate path.
    first.ClearHover();
    EXPECT_NE(sample->GetResolvedStyle().Visual.BackgroundColor, 0xFFFF0000u);
    first.OnMouseMove(-100.0f, -100.0f);
    first.Update(0.0f, true);
    second.Update(0.0f, true);
    EXPECT_EQ(style->GetCascadeHandles(assets), committed)
        << "UI observers must not commit another reload of the shared asset";

    ASSERT_FALSE(sample->GetCachedMatchedRules().empty());
    // Compare addresses without dereferencing cached pointers. The unfixed
    // code must fail deterministically before the next hover would use them.
    for (const auto& cached : sample->GetCachedMatchedRules())
    {
        bool owned = false;
        for (const auto& sheet : first.GetStylesheets())
            if (sheet)
                for (const auto& rule : sheet->Rules)
                    owned |= cached.rule == &rule;
        ASSERT_TRUE(owned) << "Another manager invalidated a cached rule's backing storage";
    }
    first.OnMouseMove(10.0f, 10.0f);
    first.Update(0.0f, true);
    EXPECT_EQ(sample->GetResolvedStyle().Visual.BackgroundColor, 0xFF0000FFu);
    first.ClearHover();
    EXPECT_NE(sample->GetResolvedStyle().Visual.BackgroundColor, 0xFF0000FFu);
}

INSTANTIATE_TEST_SUITE_P(SecondManagerBinding, SharedStylesheetReloadTests, testing::Bool());

class StylesheetCascadeReloadTests : public testing::TestWithParam<bool> {};

TEST_P(StylesheetCascadeReloadTests, SnapshotAdoptionPreservesImportOrderAndDetachedTargets)
{
    TemporaryStyleDirectory directory;
    const auto leafPath = directory.Path / "leaf.css";
    const auto themePath = directory.Path / "theme.css";
    std::ofstream(leafPath) << ".sample { width: 40px; }";
    std::ofstream(themePath) << ".sample { width: 60px; } @import \"leaf.css\"; .sample { height: 30px; }";
    auto dev = MakeHeadlessDevice();
    ASSERT_NE(dev, nullptr);
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(directory.Path, &pool));
    assets.WaitForStartupScan();
    const GUID leafGuid = assets.GetRegistry().GetAssetGUID(leafPath);
    const GUID themeGuid = assets.GetRegistry().GetAssetGUID(themePath);
    ASSERT_FALSE(leafGuid.IsNull());
    ASSERT_FALSE(themeGuid.IsNull());
    const auto leaf = assets.LoadAssetAsync(leafGuid, AssetLoadPriority::Normal).get();
    ASSERT_TRUE(leaf);
    const auto theme = assets.LoadAssetAsync(themeGuid, AssetLoadPriority::Normal).get();
    auto* style = dynamic_cast<UIStyleAsset*>(theme.get());
    ASSERT_NE(style, nullptr);

    UIManager ui(dev.get(), &assets);
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(UIParsing::XMLParser::ParseLayoutFromString(
        "<uielement id='root'><uielement id='sample' class='sample'/><uielement id='direct' class='sample'/></uielement>", root));
    ui.SetRoot(std::move(root));
    auto* sample = ui.GetRootElement()->FindById("sample");
    ASSERT_NE(sample, nullptr);
    // The manager's own sheets (the module default) lead its global list.
    const size_t leadingSheets = GetParam() ? 0 : ui.GetStylesheets().size();
    if (GetParam()) ASSERT_TRUE(ui.AttachStyleToSubtreeFromAsset(sample, *style));
    else ASSERT_TRUE(ui.AttachStyleFromAsset(*style));
    auto* direct = ui.GetRootElement()->FindById("direct");
    ASSERT_NE(direct, nullptr);
    ASSERT_TRUE(ui.AttachStyleToSubtreeFromAsset(direct, *static_cast<UIStyleAsset*>(leaf.get())));
    auto overrideSheet = std::make_shared<Stylesheet>();
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(".sample { height: 99px; }", *overrideSheet));
    if (GetParam()) sample->AddStylesheet(overrideSheet);
    else ui.AddStylesheet(overrideSheet);
    const auto expect = [&](float width)
    {
        ui.Update(0.0f, false);
        EXPECT_FLOAT_EQ(sample->GetResolvedStyle().Layout.Width.Value, width);
        EXPECT_FLOAT_EQ(sample->GetResolvedStyle().Layout.Height.Value, 99.0f);
    };
    expect(40.0f);
    SaveStyle(assets, leafPath, ".sample { width: 80px; }");
    expect(80.0f);
    EXPECT_FLOAT_EQ(direct->GetResolvedStyle().Layout.Width.Value, 80.0f);
    SaveStyle(assets, themePath, "@import \"leaf.css\"; .sample { width: 60px; height: 30px; }");
    expect(60.0f);

    // Clearing/restoring a root or imported leaf must retain its place before
    // the unrelated override, even when the clear contains no CSS segment.
    SaveStyle(assets, themePath, " \n ");
    ui.Update(0.0f, false);
    const auto& attached = GetParam() ? sample->GetStylesheets() : ui.GetStylesheets();
    ASSERT_EQ(attached.size(), leadingSheets + 2u);
    EXPECT_TRUE(attached[leadingSheets]->Rules.empty());
    EXPECT_EQ(attached.back(), overrideSheet);
    SaveStyle(assets, themePath, ".sample { width: 70px; height: 20px; }");
    expect(70.0f);
    SaveStyle(assets, themePath, ".sample { width: 70px; } @import \"leaf.css\";");
    expect(80.0f);
    SaveStyle(assets, leafPath, " \n ");
    expect(70.0f);
    SaveStyle(assets, leafPath, ".sample { width: 90px; }");
    expect(90.0f);

    auto detached = ui.GetRootElement()->TakeChild(sample);
    ASSERT_NE(detached, nullptr);
    SaveStyle(assets, leafPath, ".sample { width: 95px; }");
    ui.Update(0.0f, false);
    ui.GetRootElement()->AddChild(std::move(detached));
    expect(95.0f);
    detached = ui.GetRootElement()->TakeChild(sample);
    SaveStyle(assets, themePath, ".sample { width: 105px; height: 20px; }");
    ui.Update(0.0f, false);
    ui.GetRootElement()->AddChild(std::move(detached));
    expect(105.0f);
    std::ofstream(themePath) << ".sample { width: 110px; height: 20px; }";
    ASSERT_EQ(assets.ReloadAssetNow(themeGuid), ReloadOutcome::Reloaded);
    if (GetParam()) ASSERT_TRUE(ui.AttachStyleToSubtreeFromAsset(sample, *style));
    else ASSERT_TRUE(ui.AttachStyleFromAsset(*style));
    expect(110.0f);
    EXPECT_EQ((GetParam() ? sample->GetStylesheets() : ui.GetStylesheets()).size(), leadingSheets + 2u);
    const std::weak_ptr<const Stylesheet> retired =
        (GetParam() ? sample->GetStylesheets() : ui.GetStylesheets())[leadingSheets];
    ui.GetRootElement()->RemoveChild(sample);
    SaveStyle(assets, themePath, ".sample { width: 115px; }");
    EXPECT_NO_THROW(ui.Update(0.0f, false));
    // The manager's interner separately retains sheets until invalidation.
    // Release those cache keep-alives to isolate the expired binding's ownership.
    ui.NotifyStylesheetContentChanged();
    EXPECT_TRUE(retired.expired()) << "Destroyed bindings must release their retained snapshots";
}

INSTANTIATE_TEST_SUITE_P(BindingScope, StylesheetCascadeReloadTests, testing::Bool());

// One panel lifetime: open a panel that attaches the stylesheet to its own
// subtree, lay it out, then close (destroy) it. No style event is raised.
static bool OpenAndCloseStyledPanel(UIManager& ui, const UIStyleAsset& style)
{
    auto panel = std::make_unique<UIElement>();
    panel->AddClass("panel");
    UIElement* live = panel.get();
    ui.GetRootElement()->AddChild(std::move(panel));
    if (!ui.AttachStyleToSubtreeFromAsset(live, style))
        return false;
    ui.Update(0.0f, false);
    ui.GetRootElement()->RemoveChild(live);
    ui.Update(0.0f, false);
    return true;
}

// A panel that attaches a stylesheet and closes leaves a binding holding its
// snapshots. With no style event to release them, the next registration does,
// so a session that only opens and closes panels holds a bounded number of
// snapshots instead of one more per panel.
TEST(UIHotReloadTests, ClosedPanelsDoNotAccumulateStylesheetSnapshotsWithoutStyleEvents)
{
    TemporaryStyleDirectory directory;
    const auto stylePath = directory.Path / "panel.css";
    std::ofstream(stylePath) << ".panel { width: 40px; }";
    auto dev = MakeHeadlessDevice();
    ASSERT_NE(dev, nullptr);
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(directory.Path, &pool));
    assets.WaitForStartupScan();
    const GUID styleGuid = assets.GetRegistry().GetAssetGUID(stylePath);
    ASSERT_FALSE(styleGuid.IsNull());
    const auto asset = assets.LoadAssetAsync(styleGuid, AssetLoadPriority::Normal).get();
    auto* style = dynamic_cast<UIStyleAsset*>(asset.get());
    ASSERT_NE(style, nullptr);

    UIManager ui(dev.get(), &assets);
    ui.SetRoot(std::make_unique<UIElement>());

    const StylesheetHandle snapshot = style->GetCascadeHandles(assets).front();
    ASSERT_TRUE(OpenAndCloseStyledPanel(ui, *style));
    const long settled = snapshot.use_count();
    for (int cycle = 0; cycle < 100; ++cycle)
        ASSERT_TRUE(OpenAndCloseStyledPanel(ui, *style));
    EXPECT_EQ(snapshot.use_count(), settled)
        << "each closed panel's binding kept its stylesheet snapshot alive";
}

TEST(UIHotReloadTests, SharedGlobalImportsKeepTheirEstablishedSlots)
{
    TemporaryStyleDirectory directory;
    const auto leafPath = directory.Path / "leaf.css";
    const auto nestedPath = directory.Path / "nested.css";
    const auto aPath = directory.Path / "a.css";
    const auto bPath = directory.Path / "b.css";
    std::ofstream(leafPath) << ".sample { width: 40px; }";
    std::ofstream(nestedPath) << ".sample { width: 70px; }";
    std::ofstream(aPath) << ".sample { height: 20px; } @import \"leaf.css\";";
    std::ofstream(bPath) << ".sample { width: 90px; } @import \"leaf.css\";";
    auto dev = MakeHeadlessDevice();
    ASSERT_NE(dev, nullptr);
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(directory.Path, &pool));
    assets.WaitForStartupScan();
    const auto load = [&](const std::filesystem::path& path)
    {
        return assets.LoadAssetAsync(assets.GetRegistry().GetAssetGUID(path), AssetLoadPriority::Normal).get();
    };
    ASSERT_TRUE(load(leafPath));
    ASSERT_TRUE(load(nestedPath));
    const auto a = load(aPath), b = load(bPath);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    UIManager ui(dev.get(), &assets);
    auto root = std::make_unique<UIElement>();
    root->AddClass("sample");
    ui.SetRoot(std::move(root));
    // The manager's own sheets (the module default) lead its global list.
    const size_t leadingSheets = ui.GetStylesheets().size();
    ASSERT_TRUE(ui.AttachStyleFromAsset(*static_cast<UIStyleAsset*>(a.get())));
    auto runtime = std::make_shared<Stylesheet>();
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(".sample { height: 99px; }", *runtime));
    ui.AddStylesheet(runtime);
    ASSERT_TRUE(ui.AttachStyleFromAsset(*static_cast<UIStyleAsset*>(b.get())));
    const auto check = [&]()
    {
        ui.Update(0.0f, false);
        EXPECT_FLOAT_EQ(ui.GetRootElement()->GetResolvedStyle().Layout.Width.Value, 90.0f);
        EXPECT_FLOAT_EQ(ui.GetRootElement()->GetResolvedStyle().Layout.Height.Value, 99.0f);
        std::unordered_set<const Stylesheet*> unique;
        for (const auto& sheet : ui.GetStylesheets()) EXPECT_TRUE(unique.insert(sheet.get()).second);
    };
    check();
    SaveStyle(assets, leafPath, ".sample { width: 60px; }");
    check();
    ASSERT_EQ(ui.GetStylesheets().size(), leadingSheets + 4u);
    EXPECT_EQ(ui.GetStylesheets()[leadingSheets + 2], runtime); // [A, leaf, runtime override, B]
    SaveStyle(assets, leafPath, ".sample { width: 60px; } @import \"nested.css\";");
    check();
    ASSERT_EQ(ui.GetStylesheets().size(), leadingSheets + 5u);
    EXPECT_EQ(ui.GetStylesheets()[leadingSheets + 3], runtime);
    SaveStyle(assets, leafPath, "@import \"nested.css\";");
    check();
    SaveStyle(assets, leafPath, ".sample { width: 65px; }");
    check();
    ASSERT_EQ(ui.GetStylesheets().size(), leadingSheets + 4u);
    EXPECT_EQ(ui.GetStylesheets()[leadingSheets + 2], runtime);

    // Repeated GUID-based attach before Pump must adopt, not append snapshots.
    std::ofstream(bPath) << ".sample { width: 95px; } @import \"leaf.css\";";
    ASSERT_EQ(assets.ReloadAssetNow(b->GetGUID()), ReloadOutcome::Reloaded);
    ui.AttachStyle(b->GetGUID());
    ui.Update(0.0f, false);
    EXPECT_FLOAT_EQ(ui.GetRootElement()->GetResolvedStyle().Layout.Width.Value, 95.0f);
    ASSERT_EQ(ui.GetStylesheets().size(), leadingSheets + 4u);
    EXPECT_EQ(ui.GetStylesheets()[leadingSheets + 2], runtime);
}

TEST(UIHotReloadTests, AssetReloadedEventAppliesStyleOnNextUpdate)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    // Set up a small AssetManager rooted at a temp dir so UIStyleAsset can resolve.
    auto assetRoot = std::filesystem::temp_directory_path() / "ui_hot_reload_assets_reloaded";
    std::filesystem::create_directories(assetRoot);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    auto pathA = MakeTempCss("style_reloaded.css", "#root { width: 110px; height: 20px; }\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(pathA));
    const GUID guidA = reg.GetAssetGUID(pathA);
    ASSERT_FALSE(guidA.IsNull());

    auto futureA = assets.LoadAssetAsync(guidA, AssetLoadPriority::Normal);
    auto assetA = futureA.get();
    ASSERT_TRUE(assetA);
    auto* styleA = dynamic_cast<UIStyleAsset*>(assetA.get());
    ASSERT_NE(styleA, nullptr);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    UIManager ui(dev.get(), &assets);
    ui.SetRoot(std::move(root));
    ASSERT_TRUE(ui.AttachStyleFromAsset(*styleA));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* liveRoot = ui.GetRootElement();
    ASSERT_NE(liveRoot, nullptr);
    EXPECT_NEAR(liveRoot->GetLayoutWidth(), 110.0f, 1.0f);

    // Simulate AssetManager hot-reload: reload asset, then dispatch AssetReloaded.
    {
        std::ofstream f(pathA);
        f << "#root { width: 220px; height: 20px; }\n";
    }
    ASSERT_EQ(assets.ReloadAssetNow(guidA), ReloadOutcome::Reloaded);
    assets.GetEventDispatcher().DispatchEvent(AssetEvent(AssetEventType::AssetReloaded, guidA, AssetType::UIStyle, pathA.string()));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);
    EXPECT_NEAR(liveRoot->GetLayoutWidth(), 220.0f, 1.0f);
}

TEST(UIHotReloadTests, ImportedLeaf_ModifiedUpdatesMountedSubtreeNextUpdate)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    // Set up a small AssetManager rooted at a temp dir so UIStyleAsset can resolve imports.
    auto assetRoot = std::filesystem::temp_directory_path() / "ui_hot_reload_assets_import_leaf";
    std::filesystem::create_directories(assetRoot);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    // Create theme + imported tokens file.
    auto pathTokens = MakeTempCss("tokens_leaf.css", ":root { --ui_color_text: #E5E5E5; }\n");
    auto pathTheme =
        MakeTempCss("theme_leaf.css", "@import \"tokens_leaf.css\";\n#lbl { color: var(--ui_color_text); }\n");

    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(pathTokens));
    ASSERT_TRUE(reg.RegisterAsset(pathTheme));
    const GUID guidTokens = reg.GetAssetGUID(pathTokens);
    const GUID guidTheme = reg.GetAssetGUID(pathTheme);
    ASSERT_FALSE(guidTokens.IsNull());
    ASSERT_FALSE(guidTheme.IsNull());

    auto themeFuture = assets.LoadAssetAsync(guidTheme, AssetLoadPriority::Normal);
    auto themeAsset = themeFuture.get();
    ASSERT_TRUE(themeAsset);
    auto* themeStyle = dynamic_cast<UIStyleAsset*>(themeAsset.get());
    ASSERT_NE(themeStyle, nullptr);

    // Build UI tree: root -> mount(target=panel) where panel contains #lbl.
    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto panel = std::make_unique<UIElement>();
    UIElement* panelRaw = panel.get();
    panelRaw->SetId("panel");
    panelRaw->Overrides()
        .Set(Style::Width, StyleLength::Px(300.0f))
        .Set(Style::Height, StyleLength::Px(200.0f));

    auto lbl = std::make_unique<Label>();
    Label* lblRaw = lbl.get();
    lblRaw->SetId("lbl");
    lblRaw->SetText("Hello");
    panelRaw->AddChild(std::move(lbl));

    auto mount = std::make_unique<Mount>();
    mount->SetId("mount");
    mount->SetTarget(panelRaw);
    root->AddChild(std::move(mount));

    UIManager ui(dev.get(), &assets);
    ui.SetRoot(std::move(root));
    // Mount targets are not owned by the tree; associate them with this UIManager explicitly (mirrors DockspaceElement).
    panelRaw->SetOwnerManager(&ui);

    ASSERT_TRUE(ui.AttachStyleFromAsset(*themeStyle));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    const auto& before = lblRaw->GetResolvedStyle();
    EXPECT_EQ(before.Visual.Color, 0xFFE5E5E5u);

    SaveStyle(assets, pathTokens, ":root { --ui_color_text: #FF0000; }\n");

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    const auto& after = lblRaw->GetResolvedStyle();
    EXPECT_EQ(after.Visual.Color, 0xFFFF0000u);
}

TEST(UIHotReloadTests, ImportedLeaf_ModifiedStillPropagatesAfterProjectRebind)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "ui_hot_reload_assets_import_rebind";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ProjectA" / "Assets" / "UI" / "theme", ec);
    fs::create_directories(root / "ProjectB" / "Assets", ec);

    const fs::path pathTokens = root / "ProjectA" / "Assets" / "UI" / "theme" / "tokens_rebind.css";
    const fs::path pathTheme = root / "ProjectA" / "Assets" / "UI" / "theme_rebind.css";
    {
        std::ofstream f(pathTokens);
        f << ":root { --ui_color_text: #E5E5E5; }\n";
    }
    {
        std::ofstream f(pathTheme);
        f << "@import \"UI/theme/tokens_rebind.css\";\n#lbl { color: var(--ui_color_text); }\n";
    }

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root / "ProjectA" / "Assets",
                                  &pool,
                                  root / "ProjectA" / "AssetDatabase.assetdb",
                                  root / "ProjectA" / ".Cache" / "AssetDatabase"));

    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = root / "ProjectA" / "Assets"; // intentional overlap with project
    ASSERT_TRUE(assets.RegisterSource(editorSource));

    // RegisterSource kicks an ASYNC startup scan per source. With two sources
    // overlapping the same root, the scans race each other (and this thread's
    // explicit resolves/loads) to claim the same files, evicting and remapping
    // GUIDs as claims flip. Converge both scans before resolving identities so
    // ownership is deterministic for the rest of the test.
    assets.WaitForStartupScan("project");
    assets.WaitForStartupScan("editor");

    const fs::path themeRel = fs::path("UI") / "theme_rebind.css";
    const fs::path tokensRel = fs::path("UI") / "theme" / "tokens_rebind.css";
    const GUID themeGuid = assets.ResolveAssetGuid(themeRel, "editor");
    ASSERT_FALSE(themeGuid.IsNull());

    auto themeFuture = assets.LoadAssetAsync(themeGuid, AssetLoadPriority::Normal);
    auto themeAsset = themeFuture.get();
    ASSERT_TRUE(themeAsset);
    auto* themeStyle = dynamic_cast<UIStyleAsset*>(themeAsset.get());
    ASSERT_NE(themeStyle, nullptr);

    const GUID importedGuidBefore = assets.ResolveAssetGuidFromReference(tokensRel, pathTheme);
    ASSERT_FALSE(importedGuidBefore.IsNull());

    auto rootEl = std::make_unique<UIElement>();
    rootEl->SetId("root");
    auto lbl = std::make_unique<Label>();
    Label* lblRaw = lbl.get();
    lblRaw->SetId("lbl");
    lblRaw->SetText("Hello");
    rootEl->AddChild(std::move(lbl));

    UIManager ui(dev.get(), &assets);
    ui.SetRoot(std::move(rootEl));
    ASSERT_TRUE(ui.AttachStyleFromAsset(*themeStyle));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    const auto& before = lblRaw->GetResolvedStyle();
    EXPECT_EQ(before.Visual.Color, 0xFFE5E5E5u);

    AssetManager::SourceRebindDesc rebindDesc;
    rebindDesc.NewRoot = root / "ProjectB" / "Assets";
    rebindDesc.AuthoritativeDbFile = root / "ProjectB" / "AssetDatabase.assetdb";
    rebindDesc.CacheRoot = root / "ProjectB" / ".Cache" / "AssetDatabase";
    bool rebound = false;
    ASSERT_TRUE(assets.BeginRebindSource("project", rebindDesc, [&rebound](bool ok) { rebound = ok; }));
    // Nothing of the outgoing project is in flight here, so the rebind finishes
    // inside the call; Update() is the general path and costs nothing when the
    // continuation has already run.
    assets.Update();
    ASSERT_TRUE(rebound);
    // The rebind kicks an async scan of the new project root; converge it so
    // post-rebind identity checks don't race the re-registration.
    assets.WaitForStartupScan("project");

    const GUID importedGuidAfter = assets.ResolveAssetGuidFromReference(tokensRel, pathTheme);
    EXPECT_EQ(importedGuidBefore, importedGuidAfter);

    SaveStyle(assets, pathTokens, ":root { --ui_color_text: #00FF00; }\n");

    // Rebinding the project source EJECTS loaded assets whose claim landed on
    // "project" (theme/tokens sit under the overlapped root, so attribution
    // depends on which source claimed them). An ejected style takes the async
    // reload path in UIHotReload::Pump — request on one pump, apply on a later
    // pump once the pool-thread load lands — so a single post-dispatch Update is
    // not the contract here. Poll the observable condition (the resolved color)
    // with a deadline; each iteration performs real pump work, no sleeps.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    uint32 afterColor = 0;
    for (;;)
    {
        ui.Update(0.0f, /*interactive=*/false);
        DriveUiRender(ui, rg);
        afterColor = lblRaw->GetResolvedStyle().Visual.Color;
        if (afterColor == 0xFF00FF00u || std::chrono::steady_clock::now() >= deadline)
            break;
    }
    EXPECT_EQ(afterColor, 0xFF00FF00u)
        << "Imported-leaf edit must still propagate after a project rebind";

    fs::remove_all(root, ec);
}

TEST(UIHotReloadTests, AssetModifiedEventReloadsLayoutAndReconcilesOnNextUpdate)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    // Set up a small AssetManager rooted at a temp dir so UILayoutAsset can resolve.
    auto assetRoot = std::filesystem::temp_directory_path() / "ui_hot_reload_assets_layout_modified";
    std::filesystem::create_directories(assetRoot);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    // Create a minimal layout with a Label whose inner text we can observe.
    auto pathLayout = MakeTempUxml("layout_modified.uxml",
                                   "<UIElement id=\"root\">\n"
                                   "  <Label id=\"lbl\">Hello</Label>\n"
                                   "</UIElement>\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(pathLayout));
    const GUID guidLayout = reg.GetAssetGUID(pathLayout);
    ASSERT_FALSE(guidLayout.IsNull());

    auto future = assets.LoadAssetAsync(guidLayout, AssetLoadPriority::Normal);
    auto asset = future.get();
    ASSERT_TRUE(asset);
    auto* layout = dynamic_cast<UILayoutAsset*>(asset.get());
    ASSERT_NE(layout, nullptr);

    UIManager ui(dev.get(), &assets);
    ASSERT_TRUE(ui.LoadLayoutFromAsset(*layout));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* liveRoot = ui.GetRootElement();
    ASSERT_NE(liveRoot, nullptr);
    auto* lbl0 = dynamic_cast<Label*>(liveRoot->FindById("lbl"));
    ASSERT_NE(lbl0, nullptr);
    EXPECT_EQ(lbl0->GetText(), "Hello");

    UIManager second(dev.get(), &assets);
    ASSERT_TRUE(second.LoadLayoutFromAsset(*layout));
    second.Update(0.0f, false);
    int firstReconciles = 0;
    int secondReconciles = 0;
    ui.GetRootElement()->RegisterEventHandler(kEventLayoutReconciled, [&](UIEvent&) { ++firstReconciles; });
    second.GetRootElement()->RegisterEventHandler(kEventLayoutReconciled, [&](UIEvent&) { ++secondReconciles; });
    // One save is one reload, announced once: the manager that reloads it
    // publishes, and the other adopts that instead of reloading again.
    auto reloadEvents = std::make_shared<int>(0);
    const auto reloadCounter = assets.GetEventDispatcher().AddCallback(
        [reloadEvents, guidLayout](const AssetEvent& e)
        {
            if (e.EventType == AssetEventType::AssetReloaded && e.AssetGuid == guidLayout)
                ++*reloadEvents;
        });

    // Modify the UXML file and inject an AssetModified event directly.
    {
        std::ofstream f(pathLayout);
        f << "<UIElement id=\"root\">\n"
             "  <Label id=\"lbl\">World</Label>\n"
             "</UIElement>\n";
    }
    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetModified, guidLayout, AssetType::UILayout, pathLayout.string()));

    // Next update should reload the asset and reconcile the live tree in-place.
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* liveRoot2 = ui.GetRootElement();
    ASSERT_NE(liveRoot2, nullptr);
    auto* lbl1 = dynamic_cast<Label*>(liveRoot2->FindById("lbl"));
    ASSERT_NE(lbl1, nullptr);
    EXPECT_EQ(lbl1->GetText(), "World");
    EXPECT_EQ(firstReconciles, 1);
    second.Update(0.0f, false);
    EXPECT_EQ(secondReconciles, 1);
    lbl1->SetText("Runtime text");
    for (int i = 0; i < 2; ++i)
    {
        ui.Update(0.0f, false);
        second.Update(0.0f, false);
        EXPECT_EQ(firstReconciles, 1);
        EXPECT_EQ(secondReconciles, 1);
        EXPECT_EQ(lbl1->GetText(), "Runtime text");
    }
    EXPECT_EQ(*reloadEvents, 1) << "one layout save must publish exactly one AssetReloaded";
    assets.GetEventDispatcher().RemoveCallback(reloadCounter);
}

// -----------------------------------------------------------------------------
// C1 + E7: AssetReloaded(Texture) evicts m_BgTextureCache so the next paint
// re-resolves the background-image URL to a fresh GPU handle. Without this
// listener, panel-background .png edits never propagate without a restart.
// -----------------------------------------------------------------------------
TEST(UIHotReloadTests, TextureAssetReloadedEvictsBackgroundCache)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }

    UIRegistration::RegisterBuiltInControls();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_hot_reload_assets_texture";
    std::filesystem::create_directories(assetRoot);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    // Use a placeholder GUID via path registration. The file does not need to
    // exist on disk for this test; we only need a stable registry entry whose
    // GUID we can dispatch AssetReloaded for.
    auto pngPath = assetRoot / "panel_bg.png";
    {
        std::ofstream f(pngPath, std::ios::binary);
        f << "fake-png-bytes"; // not a valid PNG; we never load it through the parser.
    }
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(pngPath));
    const GUID guid = reg.GetAssetGUID(pngPath);
    ASSERT_FALSE(guid.IsNull());

    UIManager ui(dev.get(), &assets);
    ui.SetRoot(std::make_unique<UIElement>());

    // Seed the cache with a synthetic shared-owned handle. The eviction path
    // skips DestroyTexture for sharedOwned entries.
    Rendering::TextureHandle dummy{};
    dummy.id = 0xDEADBEEFu;
    ui.SeedBackgroundTextureForTesting(guid, dummy, 64, 64);
    ASSERT_TRUE(ui.HasBackgroundTextureCachedForTesting(guid));

    // Dispatch AssetReloaded(Texture) and pump Update once.
    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetReloaded, guid, AssetType::Texture, pngPath.string()));

    ui.Update(0.0f, /*interactive=*/false);

    EXPECT_FALSE(ui.HasBackgroundTextureCachedForTesting(guid))
        << "AssetReloaded(Texture) should have evicted the cached background texture";
}

// AssetModified for a texture also triggers eviction (mirrors style/layout behavior:
// some hot-reload pipelines emit Modified rather than Reloaded for assets that
// aren't currently held by AssetManager as live Asset objects).
TEST(UIHotReloadTests, TextureAssetModifiedEvictsBackgroundCache)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }

    UIRegistration::RegisterBuiltInControls();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_hot_reload_assets_texture_modified";
    std::filesystem::create_directories(assetRoot);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    auto pngPath = assetRoot / "icon.png";
    {
        std::ofstream f(pngPath, std::ios::binary);
        f << "fake-png-bytes";
    }
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(pngPath));
    const GUID guid = reg.GetAssetGUID(pngPath);
    ASSERT_FALSE(guid.IsNull());

    UIManager ui(dev.get(), &assets);
    ui.SetRoot(std::make_unique<UIElement>());

    Rendering::TextureHandle dummy{};
    dummy.id = 0xCAFEBABEu;
    ui.SeedBackgroundTextureForTesting(guid, dummy);
    ASSERT_TRUE(ui.HasBackgroundTextureCachedForTesting(guid));

    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetModified, guid, AssetType::Texture, pngPath.string()));

    ui.Update(0.0f, /*interactive=*/false);

    EXPECT_FALSE(ui.HasBackgroundTextureCachedForTesting(guid));
}

// -----------------------------------------------------------------------------
// C2: AssetReloaded(Font) evicts the matching FontAtlas alias / instance so
// the next text frame re-resolves the family through the host font resolver.
// Without this, FontAtlas::GetAtlasId is stale after the underlying TTF
// changes and the renderer's slug/atlas-page caches return wrong glyphs.
// -----------------------------------------------------------------------------
TEST(UIHotReloadTests, FontAssetReloadedEvictsFontAtlasMapping)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }

    UIRegistration::RegisterBuiltInControls();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_hot_reload_assets_font";
    std::filesystem::create_directories(assetRoot);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    auto fontPath = assetRoot / "TestFont.ttf";
    {
        std::ofstream f(fontPath, std::ios::binary);
        f << "fake-ttf-bytes";
    }
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(fontPath));
    const GUID fontGuid = reg.GetAssetGUID(fontPath);
    ASSERT_FALSE(fontGuid.IsNull());

    UIManager ui(dev.get(), &assets);
    ui.SetRoot(std::make_unique<UIElement>());

    const std::string atlasKey = "testfont|400|0|0";
    ui.SeedFontGuidForTesting(fontGuid, atlasKey);
    ASSERT_TRUE(ui.HasFontAtlasKeyForTesting(atlasKey));

    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetReloaded, fontGuid, AssetType::Font, fontPath.string()));

    ui.Update(0.0f, /*interactive=*/false);

    EXPECT_FALSE(ui.HasFontAtlasKeyForTesting(atlasKey))
        << "AssetReloaded(Font) should have evicted the matching atlas alias";
}

// -----------------------------------------------------------------------------
// C3: late-loading import edge case. When a leaf UIStyle import fails on
// the initial async load, then loads successfully later, the importer's
// cascade must rebuild so the leaf's content shows up. The mechanism is
// AssetEventType::AssetLoaded -> m_StyleApplyWaitingDeps drain.
// -----------------------------------------------------------------------------
TEST(UIHotReloadTests, LateLoadingImportRebuildsImporterCascade)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_hot_reload_assets_late_import";
    std::error_code ec;
    std::filesystem::remove_all(assetRoot, ec);
    std::filesystem::create_directories(assetRoot);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    // Theme @imports tokens. Create both files up front; we'll simulate the
    // "leaf was missing" case by emitting AssetLoaded for the leaf AFTER the
    // importer is already cascaded. UIHotReload's AssetLoaded handler drives
    // the m_StyleApplyWaitingDeps drain that rebuilds the importer cascade.
    auto pathTokens = MakeTempCss("late_tokens.css", ":root { --c: #112233; }\n");
    auto pathTheme = MakeTempCss("late_theme.css",
                                 "@import \"late_tokens.css\";\n#lbl { color: var(--c); }\n");
    // Move the temps under our asset root so AssetManager registers them.
    namespace fs = std::filesystem;
    const fs::path tokensFinal = assetRoot / "late_tokens.css";
    const fs::path themeFinal = assetRoot / "late_theme.css";
    fs::copy_file(pathTokens, tokensFinal, fs::copy_options::overwrite_existing, ec);
    fs::copy_file(pathTheme, themeFinal, fs::copy_options::overwrite_existing, ec);

    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(tokensFinal));
    ASSERT_TRUE(reg.RegisterAsset(themeFinal));
    const GUID guidTokens = reg.GetAssetGUID(tokensFinal);
    const GUID guidTheme = reg.GetAssetGUID(themeFinal);
    ASSERT_FALSE(guidTokens.IsNull());
    ASSERT_FALSE(guidTheme.IsNull());

    auto themeFuture = assets.LoadAssetAsync(guidTheme, AssetLoadPriority::Normal);
    auto themeAsset = themeFuture.get();
    ASSERT_TRUE(themeAsset);
    auto* themeStyle = dynamic_cast<UIStyleAsset*>(themeAsset.get());
    ASSERT_NE(themeStyle, nullptr);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto lbl = std::make_unique<Label>();
    Label* lblRaw = lbl.get();
    lblRaw->SetId("lbl");
    lblRaw->SetText("Hello");
    root->AddChild(std::move(lbl));

    UIManager ui(dev.get(), &assets);
    ui.SetRoot(std::move(root));
    ASSERT_TRUE(ui.AttachStyleFromAsset(*themeStyle));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    // Tokens load was kicked off via RefreshStyleImportGraphFor(Priority::Low).
    // Wait for the leaf load to land (the asset pipeline runs on a thread pool).
    // If it hasn't loaded by the time we re-render, the importer's cascade
    // would otherwise reflect the missing leaf and the var() would resolve
    // to its default. UIHotReload's AssetLoaded dependency-drain rebuilds it.
    for (int i = 0; i < 50 && !assets.IsAssetLoaded(guidTokens); ++i)
    {
        ui.Update(0.0f, /*interactive=*/false);
        DriveUiRender(ui, rg);
    }

    // After the leaf loads, AssetEventType::AssetLoaded propagates through
    // UIHotReload::Pump() and triggers ApplyStyleReload(theme) so the theme's
    // cascade reflects the leaf's tokens.
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    const auto& resolved = lblRaw->GetResolvedStyle();
    EXPECT_EQ(resolved.Visual.Color, 0xFF112233u)
        << "Late-loading @import should rebuild the importer's cascade";
}

// -----------------------------------------------------------------------------
// C4: runtime-mutated UI state survives layout reload.
// Foldout `expanded` and TextField `value` are written at runtime; XML's
// initial value should not clobber the live state on reload.
// -----------------------------------------------------------------------------
TEST(UIHotReloadTests, FoldoutExpandedSurvivesLayoutReload)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_hot_reload_assets_foldout";
    std::filesystem::create_directories(assetRoot);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    auto pathLayout = MakeTempUxml("foldout_layout.uxml",
                                   "<UIElement id=\"root\">\n"
                                   "  <Foldout id=\"fold\" expanded=\"false\" title=\"Title A\"/>\n"
                                   "</UIElement>\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(pathLayout));
    const GUID guidLayout = reg.GetAssetGUID(pathLayout);
    ASSERT_FALSE(guidLayout.IsNull());

    auto future = assets.LoadAssetAsync(guidLayout, AssetLoadPriority::Normal);
    auto asset = future.get();
    auto* layout = dynamic_cast<UILayoutAsset*>(asset.get());
    ASSERT_NE(layout, nullptr);

    UIManager ui(dev.get(), &assets);
    ASSERT_TRUE(ui.LoadLayoutFromAsset(*layout));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    auto* fold0 = dynamic_cast<Foldout*>(ui.GetRootElement()->FindById("fold"));
    ASSERT_NE(fold0, nullptr);
    EXPECT_FALSE(fold0->IsExpanded()); // initial XML state
    fold0->SetExpanded(true);          // simulate user toggle
    EXPECT_TRUE(fold0->IsExpanded());

    // Modify only the title (expanded= still says "false") and reload.
    {
        std::ofstream f(pathLayout);
        f << "<UIElement id=\"root\">\n"
             "  <Foldout id=\"fold\" expanded=\"false\" title=\"Title B\"/>\n"
             "</UIElement>\n";
    }
    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetModified, guidLayout, AssetType::UILayout, pathLayout.string()));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    auto* fold1 = dynamic_cast<Foldout*>(ui.GetRootElement()->FindById("fold"));
    ASSERT_NE(fold1, nullptr);
    EXPECT_TRUE(fold1->IsExpanded())
        << "Runtime-set expanded=true must survive layout reload "
           "(ShouldPreserveAttributeOnExisting must list \"expanded\")";
}

TEST(UIHotReloadTests, AccordionItemExpandedSurvivesLayoutReload)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_hot_reload_assets_accord";
    std::filesystem::create_directories(assetRoot);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    auto pathLayout = MakeTempUxml("accord_layout.uxml",
                                   "<UIElement id=\"root\">\n"
                                   "  <AccordionItem id=\"item\" expanded=\"false\" title=\"X\"/>\n"
                                   "</UIElement>\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(pathLayout));
    const GUID guidLayout = reg.GetAssetGUID(pathLayout);
    ASSERT_FALSE(guidLayout.IsNull());

    auto future = assets.LoadAssetAsync(guidLayout, AssetLoadPriority::Normal);
    auto asset = future.get();
    auto* layout = dynamic_cast<UILayoutAsset*>(asset.get());
    ASSERT_NE(layout, nullptr);

    UIManager ui(dev.get(), &assets);
    ASSERT_TRUE(ui.LoadLayoutFromAsset(*layout));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    auto* item0 = dynamic_cast<AccordionItem*>(ui.GetRootElement()->FindById("item"));
    ASSERT_NE(item0, nullptr);
    EXPECT_FALSE(item0->IsExpanded());
    item0->SetExpanded(true);
    EXPECT_TRUE(item0->IsExpanded());

    {
        std::ofstream f(pathLayout);
        f << "<UIElement id=\"root\">\n"
             "  <AccordionItem id=\"item\" expanded=\"false\" title=\"Y\"/>\n"
             "</UIElement>\n";
    }
    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetModified, guidLayout, AssetType::UILayout, pathLayout.string()));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    auto* item1 = dynamic_cast<AccordionItem*>(ui.GetRootElement()->FindById("item"));
    ASSERT_NE(item1, nullptr);
    EXPECT_TRUE(item1->IsExpanded());
}

TEST(UIHotReloadTests, TextFieldValueSurvivesLayoutReload)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }
    UiRgHarness rg(dev.get());

    UIRegistration::RegisterBuiltInControls();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_hot_reload_assets_textfield";
    std::filesystem::create_directories(assetRoot);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    auto pathLayout = MakeTempUxml("tf_layout.uxml",
                                   "<UIElement id=\"root\">\n"
                                   "  <TextField id=\"tf\" value=\"initial\"/>\n"
                                   "</UIElement>\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(pathLayout));
    const GUID guidLayout = reg.GetAssetGUID(pathLayout);

    auto future = assets.LoadAssetAsync(guidLayout, AssetLoadPriority::Normal);
    auto asset = future.get();
    auto* layout = dynamic_cast<UILayoutAsset*>(asset.get());
    ASSERT_NE(layout, nullptr);

    UIManager ui(dev.get(), &assets);
    ASSERT_TRUE(ui.LoadLayoutFromAsset(*layout));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    auto* tf0 = dynamic_cast<TextField*>(ui.GetRootElement()->FindById("tf"));
    ASSERT_NE(tf0, nullptr);
    EXPECT_EQ(tf0->GetValue(), "initial");
    tf0->SetValue("user-typed");
    EXPECT_EQ(tf0->GetValue(), "user-typed");

    // Bump the layout but keep the same XML value; reload must not clobber.
    {
        std::ofstream f(pathLayout);
        f << "<UIElement id=\"root\">\n"
             "  <TextField id=\"tf\" value=\"initial\" tabindex=\"3\"/>\n"
             "</UIElement>\n";
    }
    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetModified, guidLayout, AssetType::UILayout, pathLayout.string()));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    auto* tf1 = dynamic_cast<TextField*>(ui.GetRootElement()->FindById("tf"));
    ASSERT_NE(tf1, nullptr);
    EXPECT_EQ(tf1->GetValue(), "user-typed")
        << "TextField runtime value must survive layout reload";
}

// ---------------------------------------------------------------------------
// Reconcile keys on the TAG, not the C++ class.
//
// A family of element types registered from outside C++ shares one native proxy class, so
// RTTI cannot tell <ReconcileFoo/> from <ReconcileBar/> and a reconcile would happily hand a
// leftover Foo to a Bar slot. These arms pin the tag as the identity, in both directions:
// a different tag must replace, and the same tag must be preserved.
// ---------------------------------------------------------------------------

namespace
{
// Two tags, one C++ class — the shape the native proxy has.
class ReconcileProxy : public UIElement
{
};

struct ReconcileProxyTags
{
    ReconcileProxyTags()
    {
        UIRegistration::Register<ReconcileProxy>("ReconcileFoo");
        UIRegistration::Register<ReconcileProxy>("ReconcileBar");
    }
};

void EnsureReconcileProxyTags()
{
    static const ReconcileProxyTags kTags;
    (void)kTags;
}

// A temp root private to these arms, so a parallel run of another suite writing the shared
// ui_hot_reload_tests directory cannot race the file rewrite these tests depend on.
std::filesystem::path MakeTagIdentityUxml(const char* name, const char* contents)
{
    auto dir = std::filesystem::temp_directory_path() / "ui_tag_identity_tests";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    std::ofstream f(path);
    f << contents;
    return path;
}
} // namespace

TEST(UIHotReloadTests, ReconcileReplacesWhenOnlyTheTagDiffers)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();
    EnsureReconcileProxyTags();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_tag_identity_replace_assets";
    std::filesystem::create_directories(assetRoot);
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    auto path = MakeTagIdentityUxml("tag_identity_replace.uxml",
                                    "<UIElement id=\"root\">\n"
                                    "  <ReconcileFoo id=\"slot\"/>\n"
                                    "</UIElement>\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(path));
    const GUID guid = reg.GetAssetGUID(path);
    ASSERT_FALSE(guid.IsNull());

    auto asset = assets.LoadAssetAsync(guid, AssetLoadPriority::Normal).get();
    ASSERT_TRUE(asset);
    auto* layout = dynamic_cast<UILayoutAsset*>(asset.get());
    ASSERT_NE(layout, nullptr);

    UIManager ui(dev.get(), &assets);
    ASSERT_TRUE(ui.LoadLayoutFromAsset(*layout));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* before = ui.GetRootElement()->FindById("slot");
    ASSERT_NE(before, nullptr);
    const uint64_t idBefore = before->GetInstanceId();
    auto& factories = UIRegistration::ElementFactoryRegistry::Instance();
    EXPECT_TRUE(factories.IsSameType(*before, "reconcilefoo"));

    {
        std::ofstream f(path);
        f << "<UIElement id=\"root\">\n"
             "  <ReconcileBar id=\"slot\"/>\n"
             "</UIElement>\n";
    }
    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetModified, guid, AssetType::UILayout, path.string()));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* after = ui.GetRootElement()->FindById("slot");
    ASSERT_NE(after, nullptr);
    // Instance ids are never reused, so a different id is proof the element was rebuilt
    // rather than adopted. Under RTTI identity both tags are ReconcileProxy, the reconciler
    // sees "same type", and the id is unchanged.
    EXPECT_NE(after->GetInstanceId(), idBefore)
        << "a slot whose tag changed must be rebuilt, not adopted from the previous tag";
    EXPECT_TRUE(factories.IsSameType(*after, "reconcilebar"));
    EXPECT_FALSE(factories.IsSameType(*after, "reconcilefoo"));
}

TEST(UIHotReloadTests, ReconcilePreservesAnElementWhoseTagIsUnchanged)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();
    EnsureReconcileProxyTags();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_tag_identity_preserve_assets";
    std::filesystem::create_directories(assetRoot);
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    // An UNREGISTERED tag: the case an orphaned element is in once its factory has gone
    // away. It must reconcile against itself and keep its children, not be rebuilt.
    auto path = MakeTagIdentityUxml("tag_identity_preserve.uxml",
                                    "<UIElement id=\"root\">\n"
                                    "  <NoSuchRegisteredWidget id=\"slot\" data-v=\"1\">\n"
                                    "    <Label id=\"kid\">child</Label>\n"
                                    "  </NoSuchRegisteredWidget>\n"
                                    "</UIElement>\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(path));
    const GUID guid = reg.GetAssetGUID(path);
    ASSERT_FALSE(guid.IsNull());

    auto asset = assets.LoadAssetAsync(guid, AssetLoadPriority::Normal).get();
    ASSERT_TRUE(asset);
    auto* layout = dynamic_cast<UILayoutAsset*>(asset.get());
    ASSERT_NE(layout, nullptr);

    UIManager ui(dev.get(), &assets);
    ASSERT_TRUE(ui.LoadLayoutFromAsset(*layout));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* before = ui.GetRootElement()->FindById("slot");
    ASSERT_NE(before, nullptr);
    const uint64_t idBefore = before->GetInstanceId();
    UIElement* kidBefore = ui.GetRootElement()->FindById("kid");
    ASSERT_NE(kidBefore, nullptr);
    const uint64_t kidIdBefore = kidBefore->GetInstanceId();

    {
        std::ofstream f(path);
        f << "<UIElement id=\"root\">\n"
             "  <NoSuchRegisteredWidget id=\"slot\" data-v=\"2\">\n"
             "    <Label id=\"kid\">child</Label>\n"
             "  </NoSuchRegisteredWidget>\n"
             "</UIElement>\n";
    }
    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetModified, guid, AssetType::UILayout, path.string()));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* after = ui.GetRootElement()->FindById("slot");
    ASSERT_NE(after, nullptr);
    // Before the tag became the identity, an unknown tag matched nothing, so every reload
    // rebuilt the element and threw its subtree away.
    EXPECT_EQ(after->GetInstanceId(), idBefore)
        << "an element whose tag did not change must be preserved across a reload";
    UIElement* kidAfter = ui.GetRootElement()->FindById("kid");
    ASSERT_NE(kidAfter, nullptr);
    EXPECT_EQ(kidAfter->GetInstanceId(), kidIdBefore);
}

// A panel binds a .uxml into its own subtree and holds elements it resolved by id. A
// reload that renames one id and retypes another destroys those elements, so the panel
// must hear kEventLayoutReconciled on its own target, after the reconcile, and a panel
// that re-resolves as WeakRefs and wires once per instance sees null or new elements
// while a preserved button keeps exactly one handler. A second panel bound to a
// different layout hears nothing.
TEST(UIHotReloadTests, ReconcileAnnouncesToTheBoundTargetAfterReplacingElements)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();
    EnsureReconcileProxyTags();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_reconcile_event_assets";
    std::filesystem::create_directories(assetRoot);
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    const auto pathA = MakeTagIdentityUxml("reconcile_event_a.uxml",
                                           "<UIElement id=\"root\">\n"
                                           "  <Button id=\"keep\"/>\n"
                                           "  <Label id=\"gone\">x</Label>\n"
                                           "  <ReconcileFoo id=\"slot\"/>\n"
                                           "</UIElement>\n");
    const auto pathB = MakeTagIdentityUxml("reconcile_event_b.uxml",
                                           "<UIElement id=\"root\">\n"
                                           "  <Label id=\"other\">y</Label>\n"
                                           "</UIElement>\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(pathA));
    ASSERT_TRUE(reg.RegisterAsset(pathB));
    const GUID guidA = reg.GetAssetGUID(pathA);
    const GUID guidB = reg.GetAssetGUID(pathB);
    ASSERT_FALSE(guidA.IsNull());
    ASSERT_FALSE(guidB.IsNull());
    auto assetA = assets.LoadAssetAsync(guidA, AssetLoadPriority::Normal).get();
    auto assetB = assets.LoadAssetAsync(guidB, AssetLoadPriority::Normal).get();
    auto* layoutA = dynamic_cast<UILayoutAsset*>(assetA.get());
    auto* layoutB = dynamic_cast<UILayoutAsset*>(assetB.get());
    ASSERT_NE(layoutA, nullptr);
    ASSERT_NE(layoutB, nullptr);

    UIManager ui(dev.get(), &assets);
    auto rootEl = std::make_unique<UIElement>();
    auto panelOwned = std::make_unique<UIElement>();
    auto otherOwned = std::make_unique<UIElement>();
    UIElement* panel = panelOwned.get();
    UIElement* otherPanel = otherOwned.get();
    rootEl->AddChild(std::move(panelOwned));
    rootEl->AddChild(std::move(otherOwned));
    ui.SetRoot(std::move(rootEl));

    // The panel side: resolve on every announcement, wire once per element instance.
    int reconciles = 0;
    int clicks = 0;
    UIElement::WeakRef<Button> keep;
    UIElement::WeakRef<Button> wiredKeep;
    UIElement::WeakRef<> gone;
    UIElement::WeakRef<> renamed;
    UIElement::WeakRef<> slot;
    panel->RegisterEventHandler(kEventLayoutReconciled,
                                [&](UIEvent& e)
                                {
                                    EXPECT_EQ(e.Target, panel);
                                    ++reconciles;
                                    keep = UIElement::MakeWeakRef(dynamic_cast<Button*>(panel->FindById("keep")));
                                    gone = UIElement::MakeWeakRef(panel->FindById("gone"));
                                    renamed = UIElement::MakeWeakRef(panel->FindById("renamed"));
                                    slot = UIElement::MakeWeakRef(panel->FindById("slot"));
                                    Button* button = keep.Get();
                                    if (!button || button == wiredKeep.Get())
                                        return;
                                    button->RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++clicks; });
                                    wiredKeep = keep;
                                });
    int otherReconciles = 0;
    otherPanel->RegisterEventHandler(kEventLayoutReconciled, [&](UIEvent&) { ++otherReconciles; });

    ASSERT_TRUE(ui.BindLayoutToSubtreeChildrenFromAsset(panel, *layoutA));
    ASSERT_TRUE(ui.BindLayoutToSubtreeChildrenFromAsset(otherPanel, *layoutB));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);
    ASSERT_EQ(reconciles, 1) << "binding reconciles the layout into the target once";
    ASSERT_EQ(otherReconciles, 1);
    ASSERT_NE(keep.Get(), nullptr);
    ASSERT_NE(gone.Get(), nullptr) << "the renamed element must exist before the reload, or its null proves nothing";
    ASSERT_NE(slot.Get(), nullptr);
    const uint64_t keepId = keep.Get()->GetInstanceId();
    const uint64_t slotId = slot.Get()->GetInstanceId();

    {
        std::ofstream f(pathA);
        f << "<UIElement id=\"root\">\n"
             "  <Button id=\"keep\"/>\n"
             "  <Label id=\"renamed\">x</Label>\n"
             "  <ReconcileBar id=\"slot\"/>\n"
             "</UIElement>\n";
    }
    assets.GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetModified, guidA, AssetType::UILayout, pathA.string()));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    EXPECT_EQ(reconciles, 2) << "one reload is one announcement on the bound target";
    EXPECT_EQ(otherReconciles, 1) << "a panel bound to another layout must not hear this reload";
    EXPECT_EQ(gone.Get(), nullptr) << "the resolve ran before the reconcile, or the renamed element survived";
    ASSERT_NE(renamed.Get(), nullptr);
    ASSERT_NE(slot.Get(), nullptr);
    EXPECT_NE(slot.Get()->GetInstanceId(), slotId) << "the retyped slot must resolve to the new element";
    EXPECT_TRUE(UIRegistration::ElementFactoryRegistry::Instance().IsSameType(*slot.Get(), "reconcilebar"));
    ASSERT_NE(keep.Get(), nullptr);
    EXPECT_EQ(keep.Get()->GetInstanceId(), keepId) << "an unchanged button must be preserved, not rebuilt";

    UIEvent click{};
    click.Id = kEventButtonClick;
    click.Target = keep.Get();
    click.CurrentTarget = keep.Get();
    keep.Get()->DispatchEvent(click);
    EXPECT_EQ(clicks, 1) << "a preserved button was wired again and fires once per reconcile";
}

// A save that is not atomic — truncate, then write — puts the file through
// zero length and then through a half-written prefix before it is complete.
// Neither state is an edit the author made, and applying either one costs the
// subtree every rule it is wearing. Mirrors EditorTopToolbar: a stylesheet
// attached to one subtree, reloaded through the central asset save pipeline.
TEST(UIHotReloadTests, MidWriteSaveNeverCostsTheSubtreeItsStyle)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "No device";
    }
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();

    auto assetRoot = std::filesystem::temp_directory_path() / "ui_hot_reload_assets_midwrite";
    std::filesystem::create_directories(assetRoot);

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetRoot, &pool));

    auto path = MakeTempCss("style_midwrite.css", ".panel { width: 100px; height: 20px; }\n");
    auto& reg = assets.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(path));
    const GUID guid = reg.GetAssetGUID(path);
    ASSERT_FALSE(guid.IsNull());

    auto asset = assets.LoadAssetAsync(guid, AssetLoadPriority::Normal).get();
    ASSERT_TRUE(asset);
    auto* style = dynamic_cast<UIStyleAsset*>(asset.get());
    ASSERT_NE(style, nullptr);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    UIManager ui(dev.get(), &assets);
    ui.SetRoot(std::move(root));

    UIElement* liveRoot = ui.GetRootElement();
    ASSERT_NE(liveRoot, nullptr);
    auto panelOwned = std::make_unique<UIElement>();
    panelOwned->SetId("panel");
    panelOwned->AddClass("panel");
    UIElement* panel = panelOwned.get();
    liveRoot->AddChild(std::move(panelOwned));

    ASSERT_TRUE(ui.AttachStyleToSubtreeFromAsset(panel, *style));
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);
    ASSERT_NEAR(panel->GetLayoutWidth(), 100.0f, 1.0f);

    auto saveStep = [&](const char* contents)
    {
        SaveStyle(assets, path, contents);
        ui.Update(0.0f, /*interactive=*/false);
        DriveUiRender(ui, rg);
    };

    // The file has been truncated and the writer has not written yet.
    saveStep("");
    EXPECT_NEAR(panel->GetLayoutWidth(), 100.0f, 1.0f)
        << "a truncated-but-not-yet-written file cost the subtree its live rules";

    // Some of the new bytes have landed; the stylesheet ends inside its block.
    saveStep(".panel { width: 200px;");
    EXPECT_NEAR(panel->GetLayoutWidth(), 100.0f, 1.0f)
        << "a half-written stylesheet cost the subtree its live rules";

    // The save completes.
    saveStep(".panel { width: 200px; height: 20px; }\n");
    EXPECT_NEAR(panel->GetLayoutWidth(), 200.0f, 1.0f)
        << "the completed save did not reach the subtree";
}

TEST(UIHotReloadTests, ControlOwnedStylesResistProjectShadowing)
{
    auto dev = MakeHeadlessDevice();
    if (!dev) GTEST_SKIP() << "No device";
    UiRgHarness rg(dev.get());
    UIRegistration::RegisterBuiltInControls();
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("ui-control-source-" + GUID::Generate().ToString());
    fs::create_directories(root / "project/UI/controls");
    fs::create_directories(root / "editor/UI/controls");
    for (const char* control : {"Button", "Foldout"})
    {
        const auto file = std::string(control) + ".css";
        std::ofstream(root / "project/UI/controls" / file)
            << ".button-text, .foldout-title { display: none; }";
        std::ofstream(root / "editor/UI/controls" / file)
            << ".button-text, .foldout-title { display: block; }";
    }
    std::ofstream(root / "project/UI/controls/Missing.css") << ".probe { display: none; }";
    for (bool mounted : {false, true})
    {
        JobSystem::WorkStealingThreadPool pool(2);
        AssetManager assets;
        const auto state = root / (mounted ? "mounted" : "standalone");
        ASSERT_TRUE(assets.Initialize(root / "project", &pool, state / "assets.assetdb", state / "cache"));
        if (mounted)
        {
            AssetSourceDesc source{};
            source.Alias = "editor";
            source.Root = root / "editor";
            ASSERT_TRUE(assets.RegisterSource(source));
            assets.WaitForStartupScan("editor");
        }
        assets.WaitForStartupScan("project");
        UIManager ui(dev.get(), &assets);
        auto tree = std::make_unique<UIElement>();
        auto button = std::make_unique<Button>();
        button->SetText("Save");
        auto* label = button->GetChildren().front().get();
        tree->AddChild(std::move(button));
        auto foldout = std::make_unique<Foldout>();
        foldout->SetTitle("Diagnostics");
        auto* title = foldout->GetHeader()->GetChildren().back().get();
        tree->AddChild(std::move(foldout));
        auto missing = std::make_unique<UIElement>();
        auto* missingRaw = missing.get();
        missing->RequestSubtreeStyleAssetPath("UI/controls/Missing.css", "editor");
        tree->AddChild(std::move(missing));
        ui.SetRoot(std::move(tree));
        ui.Update(0.0f, false);
        DriveUiRender(ui, rg);
        EXPECT_EQ(label->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None, !mounted);
        EXPECT_EQ(title->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None, !mounted);
        // A broken editor installation must not silently pick up project CSS.
        EXPECT_EQ(missingRaw->GetRequestedSubtreeStyleAssetGuids().empty(), mounted);

        // Controls constructed later (popups and lazy panels) have the same ownership.
        auto late = std::make_unique<Button>();
        late->SetText("Cancel");
        auto* lateLabel = late->GetChildren().front().get();
        ui.GetRootElement()->AddChild(std::move(late));
        ui.Update(0.0f, false);
        DriveUiRender(ui, rg);
        EXPECT_EQ(lateLabel->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None, !mounted);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}
