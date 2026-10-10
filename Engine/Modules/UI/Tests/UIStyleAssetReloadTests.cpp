#include "UI/Assets/UIStyleAsset.h"
#include "UI/Parsers/CSSParser.h"
#include "Assets/AssetManager.h"
#include "../../../../Tests/WatchedFileEvents.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace GameEngine;

namespace
{
class UIStyleAssetReloadTests : public testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = std::filesystem::temp_directory_path() / ("ui-style-reload-" + GUID::Generate().ToString());
        std::filesystem::create_directories(m_Root);
        m_Path = m_Root / "style.css";
        Write(".sample { width: 40px; }");
        m_Asset = std::make_unique<UIStyleAsset>(GUID::Generate(), m_Path);
        ASSERT_TRUE(m_Asset->Load());
        m_Handle = m_Asset->GetStylesheetHandle();
        m_Cascade = m_Asset->GetCascadeHandles(m_Assets);
        ASSERT_EQ(m_Cascade.size(), 1u);
    }

    void TearDown() override
    {
        m_Asset.reset();
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    void Write(const char* text)
    {
        std::ofstream file(m_Path, std::ios::binary | std::ios::trunc);
        file << text;
        file.close();
        ASSERT_TRUE(file);
    }

    float Width(const StylesheetHandle& sheet)
    {
        UIElement element;
        element.AddClass("sample");
        return UIParsing::CSSParser::ComputeStyleFor(element, *sheet, {}).Layout.Width.Value;
    }

    void ExpectLastGood()
    {
        EXPECT_TRUE(m_Asset->IsLoaded());
        EXPECT_FALSE(m_Asset->HasFailed());
        EXPECT_EQ(m_Handle.get(), m_Asset->GetStylesheetHandle().get());
        EXPECT_FLOAT_EQ(Width(m_Handle), 40.0f);
        EXPECT_FLOAT_EQ(Width(m_Cascade.front()), 40.0f);
        const auto cascade = m_Asset->GetCascadeHandles(m_Assets);
        ASSERT_EQ(cascade.size(), 1u);
        EXPECT_EQ(cascade.front().get(), m_Cascade.front().get());
    }

    std::filesystem::path m_Root, m_Path;
    AssetManager m_Assets;
    std::unique_ptr<UIStyleAsset> m_Asset;
    StylesheetHandle m_Handle;
    std::vector<StylesheetHandle> m_Cascade;
};

TEST_F(UIStyleAssetReloadTests, MissingFileKeepsLiveRulesAndRecovers)
{
    std::filesystem::rename(m_Path, m_Root / "unavailable.css");
    EXPECT_EQ(m_Asset->Reload(), ReloadOutcome::Failed);
    ExpectLastGood();

    Write(".sample { width: 80px; }");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    EXPECT_TRUE(m_Asset->IsLoaded());
    EXPECT_NE(m_Handle.get(), m_Asset->GetStylesheetHandle().get());
    EXPECT_FLOAT_EQ(Width(m_Handle), 40.0f);
    EXPECT_FLOAT_EQ(Width(m_Cascade.front()), 40.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetStylesheetHandle()), 80.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetCascadeHandles(m_Assets).front()), 80.0f);
}

TEST_F(UIStyleAssetReloadTests, MissingImportCannotPartiallyReplaceLiveRules)
{
    Write(".sample { width: 80px; } @import \"missing.css\";");
    EXPECT_EQ(m_Asset->Reload(), ReloadOutcome::Failed);
    ExpectLastGood();
}

TEST_F(UIStyleAssetReloadTests, SuccessfulReloadPreservesPublishedSnapshots)
{
    const CSSRule* previousRule = &m_Cascade.front()->Rules.front();
    Write(".sample { width: 80px; } .additional:hover { height: 30px; }");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    const auto current = m_Asset->GetCascadeHandles(m_Assets);
    ASSERT_EQ(current.size(), 1u);
    EXPECT_NE(current.front(), m_Cascade.front());
    EXPECT_NE(m_Asset->GetStylesheetHandle(), m_Handle);
    EXPECT_EQ(&m_Cascade.front()->Rules.front(), previousRule);
    EXPECT_FLOAT_EQ(Width(m_Handle), 40.0f);
    EXPECT_FLOAT_EQ(Width(m_Cascade.front()), 40.0f);
    EXPECT_FLOAT_EQ(Width(current.front()), 80.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetStylesheetHandle()), 80.0f);
}

TEST_F(UIStyleAssetReloadTests, UnloadPreservesPublishedSnapshots)
{
    m_Asset->Unload();
    EXPECT_FALSE(m_Asset->IsLoaded());
    ASSERT_FALSE(m_Handle->Rules.empty());
    ASSERT_FALSE(m_Cascade.front()->Rules.empty());
    EXPECT_FLOAT_EQ(Width(m_Handle), 40.0f);
    EXPECT_FLOAT_EQ(Width(m_Cascade.front()), 40.0f);
    EXPECT_TRUE(m_Asset->GetCascadeHandles(m_Assets).empty());
}

TEST_F(UIStyleAssetReloadTests, LoadFromDataPublishesOnlyCompleteCandidates)
{
    const auto load = [&](std::string_view text)
    {
        return m_Asset->LoadFromData(Vector<uint8>(text.begin(), text.end()));
    };
    EXPECT_FALSE(load(".sample { width: 80px;"));
    ExpectLastGood();
    ASSERT_TRUE(load(".sample { width: 80px; }"));
    EXPECT_FLOAT_EQ(Width(m_Handle), 40.0f);
    EXPECT_FLOAT_EQ(Width(m_Cascade.front()), 40.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetStylesheetHandle()), 80.0f);
    const auto current = m_Asset->GetStylesheetHandle();
    EXPECT_FALSE(load(".sample { width: 90px; } @import \"missing.css\";"));
    EXPECT_EQ(m_Asset->GetStylesheetHandle(), current);
    EXPECT_TRUE(m_Asset->IsLoaded());
}

// An explicit reload is silent in every outcome: its caller owns the event it
// needs (see AssetManager::ReloadAssetNow), so no consumer hears one reload twice.
TEST_F(UIStyleAssetReloadTests, ExplicitReloadPublishesNoEventInAnyOutcome)
{
    auto style = std::make_shared<UIStyleAsset>(GUID::Generate(), m_Path);
    ASSERT_TRUE(style->Load());
    m_Assets.RegisterLoadedAsset(style->GetGUID(), style);
    auto events = std::make_shared<std::vector<AssetEventType>>();
    const auto callback = m_Assets.GetEventDispatcher().AddCallback(
        [events](const AssetEvent& event) { events->push_back(event.EventType); });
    const auto previous = style->GetStylesheetHandle();
    Write(".sample { width: 80px; }");
    EXPECT_EQ(m_Assets.ReloadAssetNow(style->GetGUID()), ReloadOutcome::Reloaded);
    EXPECT_FLOAT_EQ(Width(previous), 40.0f);
    const auto current = style->GetStylesheetHandle();
    EXPECT_FLOAT_EQ(Width(current), 80.0f);
    Write("");
    EXPECT_EQ(m_Assets.ReloadAssetNow(style->GetGUID()), ReloadOutcome::Deferred);
    EXPECT_EQ(style->GetStylesheetHandle(), current);
    Write(".sample { width: 90px;");
    EXPECT_EQ(m_Assets.ReloadAssetNow(style->GetGUID()), ReloadOutcome::Failed);
    EXPECT_EQ(style->GetStylesheetHandle(), current);
    EXPECT_TRUE(events->empty()) << "ReloadAssetNow published " << events->size() << " event(s)";
    m_Assets.GetEventDispatcher().RemoveCallback(callback);
}

class UIStyleAssetIncompleteReloadTests : public UIStyleAssetReloadTests,
                                          public testing::WithParamInterface<const char*> {};

TEST_P(UIStyleAssetIncompleteReloadTests, KeepsRulesUntilTheWriteIsComplete)
{
    Write(GetParam());
    EXPECT_EQ(m_Asset->Reload(), ReloadOutcome::Failed);
    ExpectLastGood();

    Write(".sample { width: 80px; }");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    EXPECT_NE(m_Handle.get(), m_Asset->GetStylesheetHandle().get());
    EXPECT_FLOAT_EQ(Width(m_Handle), 40.0f);
    EXPECT_FLOAT_EQ(Width(m_Cascade.front()), 40.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetStylesheetHandle()), 80.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetCascadeHandles(m_Assets).front()), 80.0f);
}

INSTANTIATE_TEST_SUITE_P(TruncatedCss, UIStyleAssetIncompleteReloadTests, testing::Values(
    ".sample { width: 80px;",
    ".sample { width:",
    ".sample { font-family: \"unfinished",
    "/* unfinished comment",
    ".sample { width: calc(80px",
    "[data-state=\"open\"",
    ".sample { width: 80px; } .unfinished",
    "@import \"unfinished.css\"",
    ".sample { background-image: url(unfinished"));

TEST_F(UIStyleAssetReloadTests, MalformedButCompleteStylesheetStillLoads)
{
    // A stray ']' is malformed, not truncated: the file is all there, so CSS
    // error recovery drops what it cannot read and the rest applies, as in a
    // browser. Rejecting it would cost a whole stylesheet for one typo.
    Write(".sample { width: 80px; ] }");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    EXPECT_FLOAT_EQ(Width(m_Handle), 40.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetStylesheetHandle()), 80.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetCascadeHandles(m_Assets).front()), 80.0f);
}

TEST_F(UIStyleAssetReloadTests, EmptyFileKeepsTheLiveRules)
{
    // Zero bytes is the state every truncate-then-write save passes through
    // before the writer gets to the writing, so an empty read is a save in
    // progress far more often than a file somebody emptied. A stylesheet that
    // really has no rules says so with a complete file — see
    // CompleteCommentOnlyStylesheetClearsRules, which still clears.
    Write("");
    EXPECT_EQ(m_Asset->Reload(), ReloadOutcome::Deferred);
    ExpectLastGood();

    Write(".sample { width: 80px; }");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    EXPECT_FLOAT_EQ(Width(m_Handle), 40.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetStylesheetHandle()), 80.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetCascadeHandles(m_Assets).front()), 80.0f);
}

TEST_F(UIStyleAssetReloadTests, CompleteCommentOnlyStylesheetClearsRules)
{
    Write("/* Intentionally no rules: } ] ) */\n");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    EXPECT_FALSE(m_Handle->Rules.empty());
    EXPECT_FALSE(m_Cascade.front()->Rules.empty());
    EXPECT_TRUE(m_Asset->GetStylesheetHandle()->Rules.empty());
    EXPECT_TRUE(m_Asset->GetCascadeHandles(m_Assets).front()->Rules.empty());
}

// A UTF-8 BOM is an encoding mark, not content. The CSS tokenizer reads U+FEFF
// as an identifier, so without the skip a rule-free file reads as truncated and
// the first selector of a file that has rules picks the mark up.
TEST_F(UIStyleAssetReloadTests, ByteOrderMarkOnlyStylesheetClearsRules)
{
    Write("\xEF\xBB\xBF");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    EXPECT_FALSE(m_Handle->Rules.empty());
    EXPECT_FALSE(m_Cascade.front()->Rules.empty());
    EXPECT_TRUE(m_Asset->GetStylesheetHandle()->Rules.empty());
    EXPECT_TRUE(m_Asset->GetCascadeHandles(m_Assets).front()->Rules.empty());
}

TEST_F(UIStyleAssetReloadTests, ByteOrderMarkBeforeACommentOnlyStylesheetClearsRules)
{
    Write("\xEF\xBB\xBF/* Intentionally no rules */\n");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    EXPECT_FALSE(m_Handle->Rules.empty());
    EXPECT_FALSE(m_Cascade.front()->Rules.empty());
    EXPECT_TRUE(m_Asset->GetStylesheetHandle()->Rules.empty());
    EXPECT_TRUE(m_Asset->GetCascadeHandles(m_Assets).front()->Rules.empty());
}

TEST_F(UIStyleAssetReloadTests, ByteOrderMarkBeforeARuleKeepsTheRule)
{
    Write("\xEF\xBB\xBF.sample { width: 80px; }");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    EXPECT_FLOAT_EQ(Width(m_Handle), 40.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetStylesheetHandle()), 80.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetCascadeHandles(m_Assets).front()), 80.0f);

    // Initial load reaches the same parser entry, so it answers the same way.
    UIStyleAsset fresh(GUID::Generate(), m_Path);
    ASSERT_TRUE(fresh.Load());
    EXPECT_FLOAT_EQ(Width(fresh.GetStylesheetHandle()), 80.0f);
}

TEST_F(UIStyleAssetReloadTests, QuotedAndEscapedDelimitersAreNotIncompleteBlocks)
{
    Write(R"css(.sample { width: 80px; font-family: "} ] ) \" quoted"; }
               .escaped\{name { height: 10px; } /* { [ ( */)css");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    EXPECT_FLOAT_EQ(Width(m_Handle), 40.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetStylesheetHandle()), 80.0f);
    EXPECT_FLOAT_EQ(Width(m_Asset->GetCascadeHandles(m_Assets).front()), 80.0f);
}

TEST_F(UIStyleAssetReloadTests, SuccessfulReloadRetiresSegmentsWithoutMutatingPublishedRules)
{
    std::ofstream(m_Root / "dependency.css") << ".dependency { height: 10px; }";
    Write(".sample { width: 50px; } @import \"dependency.css\"; .other { height: 20px; }");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    const auto cascade = m_Asset->GetCascadeHandles(m_Assets);
    ASSERT_EQ(cascade.size(), 2u);
    ASSERT_FALSE(cascade.back()->Rules.empty());

    Write(".sample { width: 80px; }");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    EXPECT_FLOAT_EQ(Width(cascade.front()), 50.0f);
    EXPECT_FALSE(cascade.back()->Rules.empty());
    const auto current = m_Asset->GetCascadeHandles(m_Assets);
    ASSERT_EQ(current.size(), 1u);
    EXPECT_FLOAT_EQ(Width(current.front()), 80.0f);
}

TEST_F(UIStyleAssetReloadTests, FailedReloadPreservesImportDependenciesAndCascadeOrder)
{
    const auto dependency = m_Root / "dependency.css";
    std::ofstream(dependency) << ".sample { width: 20px; }";
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(m_Root, &pool));
    assets.WaitForStartupScan();
    AssetManager::ScopedThreadAssetManager context(&assets);
    const auto dependencyGuid = assets.GetRegistry().GetAssetGUID(dependency);
    ASSERT_FALSE(dependencyGuid.IsNull());
    ASSERT_TRUE(assets.LoadAssetAsync(dependencyGuid, AssetLoadPriority::Normal).get());

    Write("@import \"dependency.css\"; .sample { width: 50px; }");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    const auto imports = m_Asset->GetImportedStyleGuids();
    ASSERT_EQ(imports.size(), 1u);
    EXPECT_EQ(imports.front(), dependencyGuid);
    const auto cascade = m_Asset->GetCascadeHandles(assets);
    ASSERT_EQ(cascade.size(), 2u);
    EXPECT_FLOAT_EQ(Width(cascade.front()), 20.0f);
    EXPECT_FLOAT_EQ(Width(cascade.back()), 50.0f);

    std::filesystem::remove(dependency);
    EXPECT_EQ(m_Asset->Reload(), ReloadOutcome::Failed);
    EXPECT_EQ(m_Asset->GetImportedStyleGuids(), imports);
    EXPECT_EQ(m_Asset->GetCascadeHandles(assets), cascade);
    EXPECT_FLOAT_EQ(Width(cascade.front()), 20.0f);
    EXPECT_FLOAT_EQ(Width(cascade.back()), 50.0f);
    std::ofstream(dependency) << ".sample { width: 20px; }";

    Write(".sample { width: 80px; } @import \"missing.css\";");
    EXPECT_EQ(m_Asset->Reload(), ReloadOutcome::Failed);
    EXPECT_TRUE(m_Asset->IsLoaded());
    EXPECT_EQ(m_Asset->GetImportedStyleGuids(), imports);
    EXPECT_EQ(m_Asset->GetCascadeHandles(assets), cascade);
    EXPECT_FLOAT_EQ(Width(cascade.front()), 20.0f);
    EXPECT_FLOAT_EQ(Width(cascade.back()), 50.0f);

    Write("@import \"dependency.css\"; .sample { width: 80px; }");
    ASSERT_EQ(m_Asset->Reload(), ReloadOutcome::Reloaded);
    const auto current = m_Asset->GetCascadeHandles(assets);
    ASSERT_EQ(current.size(), 2u);
    EXPECT_EQ(current.front(), cascade.front()); // unchanged imported snapshot
    EXPECT_NE(current.back(), cascade.back());
    EXPECT_FLOAT_EQ(Width(cascade.back()), 50.0f);
    EXPECT_FLOAT_EQ(Width(current.back()), 80.0f);
}

class UIStyleAssetDirectoryReloadTests : public testing::TestWithParam<bool>
{
protected:
    void TearDown() override
    {
        FileWatchingService::GetInstance().StopWatching();
        m_Assets.Shutdown();
        std::error_code ec;
        if (!m_Root.empty())
            std::filesystem::remove_all(m_Root, ec);
    }

    template <typename Predicate>
    bool WaitFor(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do
        {
            m_Assets.Update();
            if (predicate())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    JobSystem::WorkStealingThreadPool m_Pool{2};
    AssetManager m_Assets;
    std::filesystem::path m_Root;
};

TEST_P(UIStyleAssetDirectoryReloadTests, DeletionPreservesLiveStyleAndRestorationResumesHotReload)
{
    m_Root = std::filesystem::temp_directory_path() / ("ui-style-directory-" + GUID::Generate().ToString());
    std::filesystem::create_directories(m_Root);
    const auto path = m_Root / "theme.css";
    std::ofstream(path) << ".sample { width: 40px; }";
    ASSERT_TRUE(m_Assets.Initialize(&m_Pool));
    AssetSourceDesc source;
    source.Alias = "editor";
    source.Root = m_Root;
    ASSERT_TRUE(m_Assets.RegisterSource(source));
    m_Assets.WaitForStartupScan("editor");
    const auto guid = m_Assets.GetRegistry().GetAssetGUID(path);
    ASSERT_FALSE(guid.IsNull());
    auto style = std::make_shared<UIStyleAsset>(guid, path);
    ASSERT_TRUE(style->Load());
    m_Assets.RegisterLoadedAsset(guid, style);
    m_Assets.SetHotReloadEnabled(true);
    const auto retained = style->GetStylesheetHandle();
    const auto width = [&]()
    {
        UIElement element;
        element.AddClass("sample");
        return UIParsing::CSSParser::ComputeStyleFor(element, *style->GetStylesheetHandle(), {}).Layout.Width.Value;
    };
    auto& watcher = FileWatchingService::GetInstance();
    TestUtils::FileEventWitness witness(m_Root, path);
    ASSERT_TRUE(watcher.StartWatching());
    ASSERT_TRUE(TestUtils::WaitUntilWatchingIsArmed(m_Root, std::chrono::seconds(5)));

    if (GetParam())
    {
        // Exercise a delivered deletion before the entire source root disappears.
        const int before = witness.Count();
        std::filesystem::remove(path);
        ASSERT_TRUE(WaitFor([&]() { return witness.Count() > before; }));
    }
    std::filesystem::remove_all(m_Root);
    // Allow the reload debounce and at least one polling scan while the root is absent.
    const auto absentUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    ASSERT_TRUE(WaitFor([&]() { return std::chrono::steady_clock::now() >= absentUntil; }));
    EXPECT_TRUE(style->IsLoaded());
    EXPECT_FLOAT_EQ(width(), 40.0f);

    std::filesystem::create_directories(m_Root);
    std::ofstream(path) << ".sample { width: 80px; }";
    ASSERT_TRUE(WaitFor([&]() { return width() == 80.0f; }));
    // A delivered deletion unregisters the path (AssetManager::ApplyFileChange,
    // FileChangeType::Deleted). Recreating the source root does not re-register it on
    // Windows, so the path resolves again only in the variant where no deletion was
    // delivered. Hot reload resumes either way, through the loaded-asset scan.
    if (!GetParam())
        EXPECT_EQ(m_Assets.GetRegistry().GetAssetGUID(path), guid);
    EXPECT_NE(style->GetStylesheetHandle(), retained);
    UIElement retainedElement;
    retainedElement.AddClass("sample");
    EXPECT_FLOAT_EQ(UIParsing::CSSParser::ComputeStyleFor(retainedElement, *retained, {}).Layout.Width.Value, 40.0f);

    std::ofstream(path) << ".sample { width: 120px; }";
    ASSERT_TRUE(WaitFor([&]() { return width() == 120.0f; }));
    EXPECT_NE(style->GetStylesheetHandle(), retained);
    watcher.StopWatching();
}

INSTANTIATE_TEST_SUITE_P(SourceRemoval, UIStyleAssetDirectoryReloadTests, testing::Bool());
} // namespace
