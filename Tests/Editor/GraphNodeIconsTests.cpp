#include <gtest/gtest.h>

#include "Graph/GraphModel.h"
#include "Graph/GraphNodeIcons.h"
#include "Graph/GraphNodeRegistry.h"
#include "StagedTestPaths.h"
#include "UI/UIElement.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;

TEST(GraphNodeIconsTests, IconPathForStemFormatsGraphNodeSvgs)
{
    EXPECT_EQ(GraphNodeIcons::IconPathForStem("add"), "editor:Icons/GraphNodes/add.svg");
    EXPECT_EQ(GraphNodeIcons::IconPathForStem(""), "");
}

TEST(GraphNodeIconsTests, RegisteredTypesCarryTheirKindsStems)
{
    auto& reg = GraphNodeRegistry::Get();
    const NodeTypeMeta* add = reg.FindNodeMeta(Graph::kKindIdMaterial, "Add");
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->IconStem, "add");

    /* Alias fidelity: both texture sampler spellings share one glyph. */
    const NodeTypeMeta* sample = reg.FindNodeMeta(Graph::kKindIdMaterial, "SampleTexture2D");
    ASSERT_NE(sample, nullptr);
    EXPECT_EQ(sample->IconStem, "sample2d");

    const NodeTypeMeta* clip = reg.Find("animation", "ClipPlayer");
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->IconStem, "clip");
}

TEST(GraphNodeIconsTests, CategoryStemsResolvePerKindByFirstSegment)
{
    auto& reg = GraphNodeRegistry::Get();
    EXPECT_EQ(reg.CategoryIconStem(Graph::kKindIdMaterial, "Math"), "category-math");
    EXPECT_EQ(reg.CategoryIconStem(Graph::kKindIdMaterial, "Math/Basic"), "category-math");
    EXPECT_EQ(reg.CategoryIconStem("animation", "State Machine"), "category-state");
    EXPECT_EQ(reg.CategoryIconStem(Graph::kKindIdMaterial, "NotARealCategory"), "");
}

TEST(GraphNodeIconsTests, ApplyIconClassSwapsAndClears)
{
    UIElement element;
    GraphNodeIcons::ApplyIconClass(element, "add");
    EXPECT_TRUE(element.HasClass("gn-icon--add"));

    GraphNodeIcons::ApplyIconClass(element, "blend");
    EXPECT_FALSE(element.HasClass("gn-icon--add"));
    EXPECT_TRUE(element.HasClass("gn-icon--blend"));

    GraphNodeIcons::ApplyIconClass(element, {});
    EXPECT_FALSE(element.HasClass("gn-icon--blend"));
}

TEST(GraphNodeIconsTests, EveryRegisteredStemHasACssRule)
{
    namespace fs = std::filesystem;
    // StageEditorAssets stages the sheet beside the Editor binary, so it lives under this
    // test's OWN configuration: <build>/bin/<Config>/Apps/Editor/. Anchored to the test
    // executable rather than the working directory, which is what a spelled-out list of
    // configurations gets wrong two ways: a configuration nobody listed skips silently, and
    // a listed sibling's stale sheet answers for the configuration actually under test.
    fs::path stagedConfigDir = TestPaths::ExecutableDirectory();
    if (stagedConfigDir.filename() == "Tests")
        stagedConfigDir = stagedConfigDir.parent_path();

    fs::path css;
    for (const char* relative :
         {"Apps/Editor/Assets/UI/theme/graph-node-icons.css",
          "Apps/Editor/Editor.app/Contents/Resources/Assets/UI/theme/graph-node-icons.css"})
    {
        const fs::path candidate = stagedConfigDir / relative;
        if (fs::exists(candidate))
        {
            css = candidate;
            break;
        }
    }
    // EditorTests does not depend on StageEditorAssets, so a tests-only build legitimately
    // has no staged sheet to check against.
    if (css.empty())
        GTEST_SKIP() << "staged graph-node-icons.css not found under " << stagedConfigDir.string()
                     << " (build the Editor target to stage it)";

    std::ifstream in(css);
    std::string sheet((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    auto& reg = GraphNodeRegistry::Get();
    for (std::string_view kind : {Graph::kKindIdMaterial, Graph::kKindIdGameLogic,
                                  std::string_view("animation")})
    {
        for (const NodeTypeMeta& meta : reg.GetAllTypes(kind))
        {
            if (meta.IconStem.empty())
                continue;
            EXPECT_NE(sheet.find(".gn-icon--" + meta.IconStem), std::string::npos)
                << kind << "/" << meta.TypeId << " stem '" << meta.IconStem
                << "' has no CSS rule";
        }
    }
}
