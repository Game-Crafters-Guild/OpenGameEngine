#include <gtest/gtest.h>

#include "Editor/Assets/AssetOpenRouting.h"

using GameEngine::Editor::AssetOpenRoutingInputs;
using GameEngine::Editor::AssetOpenTarget;
using GameEngine::Editor::ResolveAssetOpenTarget;

TEST(AssetOpenRoutingTests, MaterialsOpenInTheInspector)
{
    const AssetOpenRoutingInputs inputs;
    EXPECT_EQ(ResolveAssetOpenTarget(".material", inputs), AssetOpenTarget::Inspector);
    EXPECT_EQ(ResolveAssetOpenTarget(".mat", inputs), AssetOpenTarget::Inspector);
}

TEST(AssetOpenRoutingTests, HandWrittenGlslOpensInTheScriptEditor)
{
    AssetOpenRoutingInputs inputs;
    inputs.GlslOpensInMaterialGraph = false;
    EXPECT_EQ(ResolveAssetOpenTarget(".glsl", inputs), AssetOpenTarget::ScriptEditor);
}

TEST(AssetOpenRoutingTests, ShaderGraphGlslOpensInTheNodeGraph)
{
    AssetOpenRoutingInputs inputs;
    inputs.GlslOpensInMaterialGraph = true;
    EXPECT_EQ(ResolveAssetOpenTarget(".glsl", inputs), AssetOpenTarget::NodeGraph);
}

TEST(AssetOpenRoutingTests, CSharpFollowsTheScriptEditorPreference)
{
    AssetOpenRoutingInputs inputs;
    inputs.OpenCSharpInScriptEditor = true;
    EXPECT_EQ(ResolveAssetOpenTarget(".cs", inputs), AssetOpenTarget::ScriptEditor);
    inputs.OpenCSharpInScriptEditor = false;
    EXPECT_EQ(ResolveAssetOpenTarget(".cs", inputs), AssetOpenTarget::ExternalScriptEditor);
}

TEST(AssetOpenRoutingTests, NativeSourceFollowsItsOwnPreference)
{
    AssetOpenRoutingInputs inputs;
    inputs.OpenNativeSourceInScriptEditor = false;
    EXPECT_EQ(ResolveAssetOpenTarget(".cpp", inputs), AssetOpenTarget::ExternalIde);
    EXPECT_EQ(ResolveAssetOpenTarget(".h", inputs), AssetOpenTarget::ExternalIde);
    inputs.OpenNativeSourceInScriptEditor = true;
    EXPECT_EQ(ResolveAssetOpenTarget(".cpp", inputs), AssetOpenTarget::ScriptEditor);
}

TEST(AssetOpenRoutingTests, ScenesAndAnimationAssetsReachTheirPanels)
{
    const AssetOpenRoutingInputs inputs;
    EXPECT_EQ(ResolveAssetOpenTarget(".scene", inputs), AssetOpenTarget::Scene);
    EXPECT_EQ(ResolveAssetOpenTarget(".timeline", inputs), AssetOpenTarget::Timeline);
    EXPECT_EQ(ResolveAssetOpenTarget(".clipset", inputs), AssetOpenTarget::ClipEditor);
    EXPECT_EQ(ResolveAssetOpenTarget(".anim", inputs), AssetOpenTarget::Animation);
    EXPECT_EQ(ResolveAssetOpenTarget(".glb", inputs), AssetOpenTarget::Animation);
}

TEST(AssetOpenRoutingTests, AnythingElseGoesToTheOperatingSystem)
{
    const AssetOpenRoutingInputs inputs;
    EXPECT_EQ(ResolveAssetOpenTarget(".png", inputs), AssetOpenTarget::OperatingSystem);
    EXPECT_EQ(ResolveAssetOpenTarget(".txt", inputs), AssetOpenTarget::OperatingSystem);
    EXPECT_EQ(ResolveAssetOpenTarget("", inputs), AssetOpenTarget::OperatingSystem);
}
