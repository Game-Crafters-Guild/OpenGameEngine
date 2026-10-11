// A game export strips editor-only components from the scenes it stages (CookStagedContent runs
// StripEditorOnlyComponentsFromStagedScenes): the component's lines go, the entity stays.

#include "Assets/AssetRegistry.h"
#include "Assets/ParserRegistry.h"
#include "Assets/Parsers/SceneAssetParser.h"
#include "Components/Hierarchy.h"
#include "Components/Markup/Markup.h"
#include "Components/Measure/MeasureComponent.h"
#include "Components/Name.h"
#include "Components/SceneEntityTag.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Engine/Build/AssetCollector.h"
#include "Engine/Build/StagedAssetCook.h"
#include "Engine/Build/TexturePackageCook.h"
#include "Scene/SceneIO.h"
#include "SplineECS/SplineService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>

using namespace GameEngine;

namespace
{

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

ECS::EntityHandle FindByTag(ECS::World& world, std::string_view tag)
{
    ECS::EntityHandle found = ECS::EntityHandle::Invalid();
    world.Query<ECS::Read<Components::SceneEntityTag>>().Each(
        [&](ECS::EntityHandle entity, const Components::SceneEntityTag& entityTag)
        {
            if (entityTag.View() == tag)
                found = entity;
        });
    return found;
}

bool NeverCancelled()
{
    return false;
}

// The lines of `text` that start with none of `prefixes`, in order, byte for byte.
std::string WithoutLines(const std::string& text, std::initializer_list<std::string_view> prefixes)
{
    std::string out;
    size_t start = 0;
    while (start < text.size())
    {
        const size_t end = text.find('\n', start);
        const size_t next = end == std::string::npos ? text.size() : end + 1;
        const std::string_view line(text.data() + start, next - start);
        const bool dropped = std::any_of(prefixes.begin(), prefixes.end(), [line](std::string_view prefix)
                                         { return line.substr(0, prefix.size()) == prefix; });
        if (!dropped)
            out.append(line);
        start = next;
    }
    return out;
}

} // namespace

// A Measure marker a user dropped on a prop, saved by the editor's own writer: the export keeps
// the prop, its name and its child, and drops only the marker's lines. A mark-up keeps only its
// entity and transform. Every other line ships byte for byte.
TEST(SceneExportStrip, ExportKeepsAPropAndItsChildWithoutTheMeasureOnIt)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "ge_scene_export_strip";
    std::filesystem::remove_all(root);
    const std::filesystem::path relative = "Assets/Scenes/Yard.scene";
    std::filesystem::create_directories((root / relative).parent_path());
    {
        ECS::World authoredWorld;
        const ECS::EntityHandle crate = authoredWorld.CreateEntity();
        authoredWorld.AddComponentImmediate(crate, Components::SceneEntityTag{"crate"});
        authoredWorld.AddComponentImmediate(crate, Components::Name{"Crate"});
        Components::MeasureComponent measure{};
        measure.End[0] = 2.0f;
        authoredWorld.AddComponentImmediate(crate, measure);
        const ECS::EntityHandle lid = authoredWorld.CreateEntity();
        authoredWorld.AddComponentImmediate(lid, Components::SceneEntityTag{"lid"});
        authoredWorld.AddComponentImmediate(lid, Components::Parent{crate});
        const ECS::EntityHandle lake = authoredWorld.CreateEntity();
        authoredWorld.AddComponentImmediate(lake, Components::SceneEntityTag{"lake"});
        authoredWorld.AddComponentImmediate(lake, Components::Name{"Lake Shore"});
        authoredWorld.AddComponentImmediate(lake, Components::Markup{});
        authoredWorld.AddComponentImmediate(lake, Components::MarkupVolume{});
        ASSERT_TRUE(Scene::SaveSceneToFile(authoredWorld, root / relative));
    }
    const std::string authored = ReadText(root / relative);
    ASSERT_NE(authored.find("\nMeasure."), std::string::npos) << authored;
    ASSERT_NE(authored.find("parent=\"crate\""), std::string::npos) << authored;
    ASSERT_NE(authored.find("\nMarkupVolume."), std::string::npos) << authored;

    AssetManifest manifest;
    AssetManifestEntry entry;
    entry.guid = GUID::Generate();
    entry.type = AssetType::Scene;
    entry.sourcePath = std::filesystem::path("C:/Project") / relative;
    entry.outputPath = relative;
    manifest.entries.push_back(entry);

    ParserRegistry parsers;
    ASSERT_TRUE(parsers.Initialize());
    AssetRegistry registry;
    TexturePackageCookStats textureStats;
    std::string error;
    ASSERT_TRUE(CookStagedContent(root, manifest, registry, parsers, TextureCookEncodeQuality::QuickBC7,
                                  /*textureWorkers=*/nullptr, /*manifestParsePool=*/nullptr,
                                  NeverCancelled, textureStats, error))
        << error;
    EXPECT_EQ(ReadText(root / relative),
              WithoutLines(authored, {"Measure.", "Markup.", "MarkupVolume.", "Name.value = \"Lake Shore\""}));

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, root / relative, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const ECS::EntityHandle crate = FindByTag(world, "crate");
    const ECS::EntityHandle lid = FindByTag(world, "lid");
    ASSERT_TRUE(crate.IsValid());
    ASSERT_TRUE(lid.IsValid());
    EXPECT_FALSE(world.HasComponent<Components::MeasureComponent>(crate));
    ASSERT_NE(world.GetComponent<Components::Name>(crate), nullptr);
    EXPECT_EQ(world.GetComponent<Components::Name>(crate)->View(), "Crate");
    const auto* parent = world.GetComponent<Components::Parent>(lid);
    ASSERT_NE(parent, nullptr);
    EXPECT_EQ(parent->parent.id, crate.id);
    const ECS::EntityHandle lake = FindByTag(world, "lake");
    ASSERT_TRUE(lake.IsValid());
    EXPECT_FALSE(world.HasComponent<Components::Markup>(lake));
    EXPECT_FALSE(world.HasComponent<Components::MarkupVolume>(lake));

    std::filesystem::remove_all(root);
}

// On an entity that carries a mark-up, its title and its spline are conversation and shape:
// they go with it, and its transform stays; the reader matches component tokens ignoring case,
// and so does the strip. The same components on scene content ship. A blueprint instance's
// removal (`-C`), addition (`+C`) and bare addition (`C`) of an editor-only component go as its
// field lines do.
TEST(SceneExportStrip, AMarkupEntityLosesItsTitleAndSplineAndSceneContentKeepsThem)
{
    const std::string authored =
        "[entity id=\"lake\"]\n"
        "Markup.status = Proposed\n"
        "Name.value = \"Lake\"\n"
        "Spline.pointCount = 3\n"
        "Transform.position = (40, 0, 12)\n"
        "\n"
        "[entity id=\"pond\"]\n"
        "markup.status = Proposed\n"
        "name.value = \"Pond\"\n"
        "\n"
        "[entity id=\"road\"]\n"
        "Name.value = \"Road\"\n"
        "Spline.pointCount = 5\n"
        "\n"
        "[blueprint id=\"cart\" source=\"r_cart\"]\n"
        "-Measure\n"
        "Measure.start = (0, 0, 0)\n"
        "Transform.position = (3, 0, 3)\n"
        "\n"
        "[blueprint id=\"barrow\" source=\"r_barrow\"]\n"
        "+Measure\n"
        "Measure\n"
        "Transform.position = (5, 0, 5)\n";

    const std::string exported = SceneAssetParser::StripEditorOnlyComponents(
        authored, [](std::string_view component) { return component == "Measure"; });

    EXPECT_EQ(exported,
              "[entity id=\"lake\"]\n"
              "Markup.status = Proposed\n"
              "Transform.position = (40, 0, 12)\n"
              "\n"
              "[entity id=\"pond\"]\n"
              "markup.status = Proposed\n"
              "\n"
              "[entity id=\"road\"]\n"
              "Name.value = \"Road\"\n"
              "Spline.pointCount = 5\n"
              "\n"
              "[blueprint id=\"cart\" source=\"r_cart\"]\n"
              "Transform.position = (3, 0, 3)\n"
              "\n"
              "[blueprint id=\"barrow\" source=\"r_barrow\"]\n"
              "Transform.position = (5, 0, 5)\n");
}

// A region mark-up, saved by the editor's own writer, leaves nothing in an export but its
// entity and transform: its MarkupRegion (height and members), its outline spline and its
// title go with its Markup, and so does the lake it excludes.
TEST(SceneExportStrip, ARegionLeavesOnlyItsEntityAndTransform)
{
    const bool ownsSplines = !SplineECS::SplineService::IsInitialized();
    if (ownsSplines)
        SplineECS::SplineService::Initialize();
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "ge_scene_export_strip_region";
    std::filesystem::remove_all(root);
    const std::filesystem::path relative = "Assets/Scenes/Forest.scene";
    std::filesystem::create_directories((root / relative).parent_path());
    {
        ECS::World authoredWorld;
        const ECS::EntityHandle lake = authoredWorld.CreateEntity();
        authoredWorld.AddComponentImmediate(lake, Components::SceneEntityTag{"lake"});
        authoredWorld.AddComponentImmediate(lake, Components::Markup{});
        authoredWorld.AddComponentImmediate(lake, Components::MarkupVolume{Components::MarkupVolumeShape::Sphere, {}});
        const ECS::EntityHandle forest = authoredWorld.CreateEntity();
        authoredWorld.AddComponentImmediate(forest, Components::SceneEntityTag{"forest"});
        authoredWorld.AddComponentImmediate(forest, Components::Name{"Forest"});
        authoredWorld.AddComponentImmediate(
            forest, Components::Transform::FromTRS(Mathematics::Vector3(45.0f, 0.0f, 30.0f), Mathematics::Quaternion::Identity(),
                                                   Mathematics::Vector3(1.0f, 1.0f, 1.0f)));
        authoredWorld.AddComponentImmediate(forest, Components::Markup{});
        SplineECS::SplineService& splines = SplineECS::SplineService::Get();
        const SplineECS::SplineHandle handle = splines.CreateSpline(Spline::SplineType::Linear, true);
        for (const Mathematics::Vector3& knot : {Mathematics::Vector3(0.0f, 0.0f, 0.0f), Mathematics::Vector3(90.0f, 0.0f, 0.0f),
                                                 Mathematics::Vector3(0.0f, 0.0f, 90.0f)})
            splines.GetSplineData(handle)->AddPoint(knot, 0.0f);
        Components::SplineComponent spline{};
        spline.SplineDataIndex = handle.Index();
        spline.SplineDataGeneration = handle.Generation();
        authoredWorld.AddComponentImmediate(forest, spline);
        Components::MarkupRegion region{};
        region.MemberCount = 1;
        region.Members[0] = {lake, Components::MarkupMemberMode::Exclude};
        authoredWorld.AddComponentImmediate(forest, region);
        ASSERT_TRUE(Scene::SaveSceneToFile(authoredWorld, root / relative));
    }
    const std::string authored = ReadText(root / relative);
    ASSERT_NE(authored.find("\nMarkupRegion.member0 = exclude \"lake\""), std::string::npos) << authored;
    ASSERT_NE(authored.find("\nSpline.closed = true"), std::string::npos) << authored;
    ASSERT_NE(authored.find("\nTransform."), std::string::npos) << authored;

    AssetManifest manifest;
    AssetManifestEntry entry;
    entry.guid = GUID::Generate();
    entry.type = AssetType::Scene;
    entry.sourcePath = std::filesystem::path("C:/Project") / relative;
    entry.outputPath = relative;
    manifest.entries.push_back(entry);
    ParserRegistry parsers;
    ASSERT_TRUE(parsers.Initialize());
    AssetRegistry registry;
    TexturePackageCookStats textureStats;
    std::string error;
    ASSERT_TRUE(CookStagedContent(root, manifest, registry, parsers, TextureCookEncodeQuality::QuickBC7,
                                  /*textureWorkers=*/nullptr, /*manifestParsePool=*/nullptr,
                                  NeverCancelled, textureStats, error))
        << error;
    EXPECT_EQ(ReadText(root / relative),
              WithoutLines(authored, {"Markup.", "MarkupVolume.", "MarkupRegion.", "Spline.", "Name.value = \"Forest\""}));
    const std::string exported = ReadText(root / relative);
    EXPECT_EQ(exported.find("Markup"), std::string::npos) << exported;
    EXPECT_EQ(exported.find("Spline."), std::string::npos) << exported;
    EXPECT_NE(exported.find("Transform."), std::string::npos) << exported;

    std::filesystem::remove_all(root);
    if (ownsSplines)
        SplineECS::SplineService::Shutdown();
}
