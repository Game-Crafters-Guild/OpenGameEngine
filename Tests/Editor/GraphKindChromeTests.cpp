#include <gtest/gtest.h>

#include <string>

#include "AssetCore/GUID.h"
#include "EditorPanelIds.h"
#include "Graph/GraphKindChrome.h"
#include "Graph/GraphNodeRegistry.h"
#include "Graph/GraphPanelFactory.h"
#include "Graph/GraphTypeRegistry.h"
#include "Graph/NodeColorSettings.h"

using namespace GameEngine;

TEST(GraphKindChromeTests, RegisteredTypesIncludeAnimationWithoutAnEnumerator)
{
    const std::vector<Graph::GraphTypeDesc> kinds = RegisteredGraphTypes();
    bool hasAnimation = false;
    bool hasMaterial = false;
    bool hasGameLogic = false;
    for (const Graph::GraphTypeDesc& kind : kinds)
    {
        if (kind.Id == "animation")
        {
            hasAnimation = true;
            EXPECT_EQ(kind.DisplayName, "Animation");
            EXPECT_EQ(kind.FileExtension, ".animgraph");
        }
        if (kind.Id == Graph::kKindIdMaterial)
        {
            hasMaterial = true;
            EXPECT_EQ(kind.DisplayName, "Shader Graph");
            EXPECT_EQ(kind.FileExtension, ".glsl");
        }
        if (kind.Id == Graph::kKindIdGameLogic)
            hasGameLogic = true;
    }
    EXPECT_TRUE(hasAnimation);
    EXPECT_TRUE(hasMaterial);
    EXPECT_TRUE(hasGameLogic);

    Graph::Kind parsed = Graph::Kind::GameLogic;
    EXPECT_FALSE(Graph::GraphTypeRegistry::TryParseKind("animation", parsed));
}

TEST(GraphKindChromeTests, SaveFilterUsesRegisteredExtension)
{
    const GraphKindFileFilter animation = SaveFilterForKind("animation");
    EXPECT_EQ(animation.Pattern, "*.animgraph");
    EXPECT_EQ(animation.DefaultExtension, ".animgraph");

    const GraphKindFileFilter material = SaveFilterForKind(Graph::kKindIdMaterial);
    EXPECT_EQ(material.Pattern, "*.glsl");
    EXPECT_EQ(material.DefaultExtension, ".glsl");

    const GraphKindFileFilter gameLogic = SaveFilterForKind(Graph::kKindIdGameLogic);
    EXPECT_EQ(gameLogic.Pattern, "*.graph");
}

TEST(GraphKindChromeTests, OpenFilterAndExtensionDispatchIncludeAnimGraph)
{
    EXPECT_TRUE(ExtensionOpensInGraphPanel(".animgraph"));
    EXPECT_TRUE(ExtensionOpensInGraphPanel(".ANIMGRAPH"));
    EXPECT_TRUE(ExtensionOpensInGraphPanel(".graph"));
    EXPECT_TRUE(ExtensionOpensInGraphPanel(".glsl"));
    EXPECT_FALSE(ExtensionOpensInGraphPanel(".png"));
    EXPECT_NE(OpenGraphsFilterPattern().find("*.animgraph"), std::string::npos);
    EXPECT_EQ(UntitledGraphTitle("animation"), "Untitled Animation");
    EXPECT_EQ(UntitledGraphTitle(Graph::kKindIdMaterial), "Untitled Shader Graph");
}

TEST(GraphKindChromeTests, ExtensionMapsToKindAndDock)
{
    EXPECT_EQ(KindIdFromGraphExtension(".glsl"), std::string(Graph::kKindIdMaterial));
    EXPECT_EQ(KindIdFromGraphExtension(".GLSL"), std::string(Graph::kKindIdMaterial));
    EXPECT_EQ(KindIdFromGraphExtension(".animgraph"), std::string(Graph::kKindIdAnimation));
    EXPECT_EQ(KindIdFromGraphExtension(".graph"), std::string(Graph::kKindIdGameLogic));
    EXPECT_TRUE(KindIdFromGraphExtension(".png").empty());

    EXPECT_STREQ(GraphDockPanelIdForKind(Graph::kKindIdMaterial), EditorPanelIds::NodeGraph);
    EXPECT_STREQ(GraphDockPanelIdForKind(Graph::kKindIdAnimation), EditorPanelIds::AnimationGraph);
    EXPECT_STREQ(GraphDockPanelIdForKind(Graph::kKindIdGameLogic), EditorPanelIds::GameLogicGraph);
    EXPECT_EQ(GraphDockPanelIdForKind("not_a_kind"), nullptr);
    EXPECT_TRUE(IsKnownGraphKindId(Graph::kKindIdMaterial));
    EXPECT_FALSE(IsKnownGraphKindId("not_a_kind"));
}

TEST(GraphKindChromeTests, PerAssetDockIdsAreKindPrefixedAndStable)
{
    const GUID a("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
    const GUID b("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
    const std::string idA = MakeGraphDockIdForAsset(Graph::kKindIdMaterial, a);
    const std::string idB = MakeGraphDockIdForAsset(Graph::kKindIdMaterial, b);
    const std::string idAAgain = MakeGraphDockIdForAsset(Graph::kKindIdMaterial, a);
    EXPECT_NE(idA, idB);
    EXPECT_EQ(idA, idAAgain);
    EXPECT_EQ(idA.find("NodeGraph:"), 0u);
    EXPECT_EQ(idA, std::string("NodeGraph:") + a.ToCompactString());
    EXPECT_EQ(MakeGraphDockIdForAsset(Graph::kKindIdMaterial, GUID::Null()),
              std::string(EditorPanelIds::NodeGraph));

    const std::string anim = MakeGraphDockIdForAsset(Graph::kKindIdAnimation, a);
    EXPECT_EQ(anim.find("AnimationGraph:"), 0u);
    EXPECT_EQ(anim, std::string("AnimationGraph:") + a.ToCompactString());
}

TEST(GraphKindChromeTests, AnimationNodeColorsDoNotFailOpenToGameLogic)
{
    (void)GraphNodeRegistry::Get();
    const uint32_t pose = NodeColorSettings::GetNodeColorArgb("animation", "ClipPlayer");
    const uint32_t ik = NodeColorSettings::GetNodeColorArgb("animation", "LookAt");
    const uint32_t unknownGameLogic = NodeColorSettings::GetNodeColorArgb(Graph::kKindIdGameLogic, "ClipPlayer");
    EXPECT_NE(pose, ik);
    EXPECT_NE(pose, unknownGameLogic);
    EXPECT_NE(ik, unknownGameLogic);
}
