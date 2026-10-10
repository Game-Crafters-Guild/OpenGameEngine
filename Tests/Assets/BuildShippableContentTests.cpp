// Build-time shippable-content rules (AssetCollector::IsShippableContent) and
// the Editor folder convention they share with package code discovery
// (IsEditorOnlyAssetPath): any folder named Editor under a project's or
// package's asset root is editor-only and must never stage into a game build.

#include "AssetCore/EditorOnlyAssetPath.h"
#include "Engine/Build/AssetCollector.h"

#include <gtest/gtest.h>

#include <filesystem>

using GameEngine::AssetCollector;
using GameEngine::IsEditorOnlyAssetPath;
namespace fs = std::filesystem;

// The convention itself, named once and read by both the build-time filter
// above and the package code discovery that splits editor modules out.
TEST(EditorOnlyAssetPath, MatchesAnEditorDirectorySegmentAtAnyDepth)
{
    EXPECT_TRUE(IsEditorOnlyAssetPath(fs::path("Editor/InspectorIcon.png")));
    EXPECT_TRUE(IsEditorOnlyAssetPath(fs::path("Tools/Editor/gizmo.mesh")));
    EXPECT_TRUE(IsEditorOnlyAssetPath(fs::path("Ocean/Editor/Presets/deep.preset")));
    // Case-insensitive (Windows folders).
    EXPECT_TRUE(IsEditorOnlyAssetPath(fs::path("editor/theme.css")));
    EXPECT_TRUE(IsEditorOnlyAssetPath(fs::path("Tools/EDITOR/x.png")));

    // Only a segment named exactly "Editor"; prefixed names are ordinary.
    EXPECT_FALSE(IsEditorOnlyAssetPath(fs::path("EditorAssets/logo.png")));
    EXPECT_FALSE(IsEditorOnlyAssetPath(fs::path("Docs/EditorManual.txt")));
    EXPECT_FALSE(IsEditorOnlyAssetPath(fs::path("Textures/rock_albedo.png")));
    EXPECT_FALSE(IsEditorOnlyAssetPath(fs::path()));
}

TEST(ShippableContent, RuntimeContentPasses)
{
    EXPECT_TRUE(AssetCollector::IsShippableContent(fs::path("Textures/rock_albedo.png")));
    EXPECT_TRUE(AssetCollector::IsShippableContent(fs::path("Scenes/main.scene")));
    // Shader sources are runtime content and stay.
    EXPECT_TRUE(AssetCollector::IsShippableContent(fs::path("Shaders/water.glsl")));
}

TEST(ShippableContent, SourceAndBuildFilesDenied)
{
    EXPECT_FALSE(AssetCollector::IsShippableContent(fs::path("Scripts/Player.cs")));
    EXPECT_FALSE(AssetCollector::IsShippableContent(fs::path("Gameplay/Health.cpp")));
    EXPECT_FALSE(AssetCollector::IsShippableContent(fs::path("Gameplay/Health.h")));
    EXPECT_FALSE(AssetCollector::IsShippableContent(fs::path("Game.csproj")));
    EXPECT_FALSE(AssetCollector::IsShippableContent(fs::path("obj/Debug/x.json")));
    EXPECT_FALSE(AssetCollector::IsShippableContent(fs::path("Tools/bin/tool.dat")));
    EXPECT_FALSE(AssetCollector::IsShippableContent(fs::path(".DS_Store")));
}

TEST(ShippableContent, EditorFolderSegmentDenied)
{
    // Root-level and nested Editor/ segments, both separators.
    EXPECT_FALSE(AssetCollector::IsShippableContent(fs::path("Editor/InspectorIcon.png")));
    EXPECT_FALSE(AssetCollector::IsShippableContent(fs::path("Tools/Editor/gizmo.mesh")));
    EXPECT_FALSE(AssetCollector::IsShippableContent(fs::path("Ocean/Editor/Presets/deep.preset")));
    // Case-insensitive (Windows folders).
    EXPECT_FALSE(AssetCollector::IsShippableContent(fs::path("editor/theme.css")));
    EXPECT_FALSE(AssetCollector::IsShippableContent(fs::path("Tools/EDITOR/x.png")));
}

TEST(ShippableContent, EditorAsFileNameOrPrefixIsNotDenied)
{
    // Only a directory segment named exactly "Editor" is the convention; files
    // and prefixed folder names are ordinary content.
    EXPECT_TRUE(AssetCollector::IsShippableContent(fs::path("Textures/Editor.png")));
    EXPECT_TRUE(AssetCollector::IsShippableContent(fs::path("EditorAssets/logo.png")));
    EXPECT_TRUE(AssetCollector::IsShippableContent(fs::path("Docs/EditorManual.txt")));
}
