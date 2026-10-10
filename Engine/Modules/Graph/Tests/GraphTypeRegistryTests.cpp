#include "Graph/GraphTypeRegistry.h"

#include <gtest/gtest.h>

#include <string>

using GameEngine::Graph::GraphTypeDesc;
using GameEngine::Graph::GraphTypeRegistry;
using GameEngine::Graph::Kind;
using GameEngine::Graph::kKindIdGameLogic;
using GameEngine::Graph::kKindIdMaterial;

TEST(GraphTypeRegistryTest, BuiltinsAreRegistered)
{
    const GraphTypeDesc* gameLogic = GraphTypeRegistry::Get().Find(kKindIdGameLogic);
    ASSERT_NE(gameLogic, nullptr);
    EXPECT_EQ(gameLogic->DisplayName, "Game Logic");
    EXPECT_EQ(gameLogic->FileExtension, ".graph");

    const GraphTypeDesc* material = GraphTypeRegistry::Get().Find(kKindIdMaterial);
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->DisplayName, "Shader Graph");
    EXPECT_EQ(material->FileExtension, ".glsl");
}

TEST(GraphTypeRegistryTest, KindIdRoundTrip)
{
    EXPECT_EQ(GraphTypeRegistry::IdFromKind(Kind::GameLogic), kKindIdGameLogic);
    EXPECT_EQ(GraphTypeRegistry::IdFromKind(Kind::Material), kKindIdMaterial);

    Kind parsed = Kind::GameLogic;
    EXPECT_TRUE(GraphTypeRegistry::TryParseKind(kKindIdMaterial, parsed));
    EXPECT_EQ(parsed, Kind::Material);
    EXPECT_TRUE(GraphTypeRegistry::TryParseKind(kKindIdGameLogic, parsed));
    EXPECT_EQ(parsed, Kind::GameLogic);
}

TEST(GraphTypeRegistryTest, UnknownKindHasNoEnumerator)
{
    Kind parsed = Kind::Material;
    EXPECT_FALSE(GraphTypeRegistry::TryParseKind("animation", parsed));
    EXPECT_EQ(parsed, Kind::Material);
    EXPECT_FALSE(GraphTypeRegistry::TryParseKind("not_a_kind", parsed));
}

TEST(GraphTypeRegistryTest, ThirdKindRegistersWithoutAnEnumerator)
{
    GraphTypeRegistry::Get().Register({"animation", "Animation", ".animgraph"});
    const GraphTypeDesc* animation = GraphTypeRegistry::Get().Find("animation");
    ASSERT_NE(animation, nullptr);
    EXPECT_EQ(animation->DisplayName, "Animation");
    EXPECT_EQ(animation->FileExtension, ".animgraph");

    Kind parsed = Kind::GameLogic;
    EXPECT_FALSE(GraphTypeRegistry::TryParseKind("animation", parsed));
    EXPECT_EQ(parsed, Kind::GameLogic);

    const GraphTypeDesc* first = GraphTypeRegistry::Get().Find("animation");
    GraphTypeRegistry::Get().Register({"animation", "Should Not Replace", ".graph"});
    const GraphTypeDesc* again = GraphTypeRegistry::Get().Find("animation");
    ASSERT_EQ(first, again);
    EXPECT_EQ(again->DisplayName, "Animation");
    EXPECT_EQ(again->FileExtension, ".animgraph");
}

TEST(GraphTypeRegistryTest, EmptyIdIsIgnored)
{
    GraphTypeRegistry::Get().Register({"", "Nameless", ".graph"});
    EXPECT_EQ(GraphTypeRegistry::Get().Find(""), nullptr);
}

TEST(GraphTypeRegistryTest, AllIncludesBuiltins)
{
    const std::vector<GraphTypeDesc> all = GraphTypeRegistry::Get().All();
    bool hasGameLogic = false;
    bool hasMaterial = false;
    for (const GraphTypeDesc& type : all)
    {
        if (type.Id == kKindIdGameLogic)
            hasGameLogic = true;
        if (type.Id == kKindIdMaterial)
            hasMaterial = true;
    }
    EXPECT_TRUE(hasGameLogic);
    EXPECT_TRUE(hasMaterial);
    EXPECT_GE(all.size(), 2u);
}
