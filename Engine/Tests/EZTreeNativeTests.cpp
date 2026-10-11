#include <gtest/gtest.h>

#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "Assets/TextureCook.h"
#include "Core/Application.h" // PathUtils::GetExecutableDirectory
#include "ECS/ComponentFactory.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"
#include "EZTree/EZTreeGenerator.h"
#include "EZTree/EZTreeOptions.h"
#include "EZTree/EZTreeRng.h"
#include "EZTreeECS/Components/EZTreeComponent.h"
#include "EZTreeECS/EZTreeDefaultTextures.h"
#include "EZTreeECS/EZTreePlugin.h"
#include "EZTreeECS/EZTreeRuntimeMaterials.h"
#include "EZTreeECS/Systems/RegisterEZTreeSystems.h"
#include "PluginAPI/EnginePlugin.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine;

namespace
{

EZTree::TreeOptions MakeSmallTree()
{
    EZTree::TreeOptions options = EZTree::MakeDefaultOptions();
    options.seed = 1234u;
    options.branch.levels = 0u;
    options.branch.length[0] = 4.0f;
    options.branch.radius[0] = 0.35f;
    options.branch.sections[0] = 4u;
    options.branch.segments[0] = 6u;
    options.branch.children = {0u, 0u, 0u, 0u};
    options.leaves.count = 2u;
    options.leaves.size = 0.5f;
    options.leaves.sizeVariance = 0.0f;
    options.leaves.billboard = EZTree::BillboardMode::Double;
    options.trellis.enabled = false;
    return options;
}

} // namespace

TEST(EZTreeNativeTests, RngIsDeterministic)
{
    EZTree::Rng a(42u);
    EZTree::Rng b(42u);

    for (int i = 0; i < 16; ++i)
        EXPECT_FLOAT_EQ(a.Random(), b.Random());
}

TEST(EZTreeNativeTests, PresetsLoadFromVcpkgSnapshot)
{
    const auto presets = EZTree::ListPresets();
    EXPECT_FALSE(presets.empty());

    EZTree::TreeOptions options{};
    std::string error;
    EXPECT_TRUE(EZTree::LoadPreset("oak_small", options, &error)) << error;
    EXPECT_GT(options.branch.levels, 0u);
    EXPECT_EQ(options.bark.type, EZTree::BarkType::Oak);

    EXPECT_TRUE(EZTree::LoadPreset("aspen_small", options, &error)) << error;
    EXPECT_EQ(options.bark.type, EZTree::BarkType::Birch);

    EXPECT_TRUE(EZTree::LoadPreset("pine_small", options, &error)) << error;
    EXPECT_EQ(options.bark.type, EZTree::BarkType::Pine);
}

TEST(EZTreeNativeTests, SanitizeOptionsClampsUnsafeInputs)
{
    EZTree::TreeOptions options = EZTree::MakeDefaultOptions();
    options.branch.levels = 99u;
    options.branch.children = {9999u, 9999u, 9999u, 9999u};
    options.branch.sections[0] = 0u;
    options.branch.segments[0] = 1u;
    options.branch.length[0] = -10.0f;
    options.branch.radius[0] = -1.0f;
    options.leaves.textureColumns = 0u;
    options.leaves.textureRows = 0u;
    options.leaves.textureTile = 999u;
    options.leaves.count = 999999u;
    options.trellis.spacing = 0.0f;
    options.trellis.forceFalloff = 0.0f;
    options.wind.scale = std::numeric_limits<float>::quiet_NaN();

    const EZTree::TreeOptions sanitized = EZTree::SanitizeOptions(options);
    EXPECT_EQ(sanitized.branch.levels, 3u);
    EXPECT_EQ(sanitized.branch.children[0], 16u);
    EXPECT_EQ(sanitized.branch.sections[0], 1u);
    EXPECT_EQ(sanitized.branch.segments[0], 3u);
    EXPECT_GT(sanitized.branch.length[0], 0.0f);
    EXPECT_GT(sanitized.branch.radius[0], 0.0f);
    EXPECT_EQ(sanitized.leaves.textureColumns, 1u);
    EXPECT_EQ(sanitized.leaves.textureRows, 1u);
    EXPECT_EQ(sanitized.leaves.textureTile, 0u);
    EXPECT_EQ(sanitized.leaves.count, 32u);
    EXPECT_GT(sanitized.trellis.spacing, 0.0f);
    EXPECT_GT(sanitized.trellis.forceFalloff, 0.0f);
    EXPECT_TRUE(std::isfinite(sanitized.wind.scale));
}

TEST(EZTreeNativeTests, HashUsesSanitizedOptions)
{
    EZTree::TreeOptions a = EZTree::MakeDefaultOptions();
    EZTree::TreeOptions b = a;
    a.branch.levels = 999u;
    b.branch.levels = 3u;
    a.branch.sections[0] = 0u;
    b.branch.sections[0] = 1u;
    a.leaves.textureColumns = 0u;
    b.leaves.textureColumns = 1u;

    EXPECT_EQ(EZTree::HashOptions(a), EZTree::HashOptions(b));
}

TEST(EZTreeNativeTests, GenerationIsDeterministicForSameOptions)
{
    const EZTree::TreeOptions options = MakeSmallTree();
    EZTree::Generator generator;

    const EZTree::GeneratedTree a = generator.Generate(options);
    const EZTree::GeneratedTree b = generator.Generate(options);

    EXPECT_EQ(a.stats.branchVertices, b.stats.branchVertices);
    EXPECT_EQ(a.stats.branchIndices, b.stats.branchIndices);
    EXPECT_EQ(a.stats.leafVertices, b.stats.leafVertices);
    EXPECT_EQ(a.stats.leafIndices, b.stats.leafIndices);
    ASSERT_EQ(a.combined.Vertices.size(), b.combined.Vertices.size());
    ASSERT_EQ(a.combined.Indices.size(), b.combined.Indices.size());
    for (size_t i = 0; i < a.combined.Vertices.size(); ++i)
    {
        EXPECT_FLOAT_EQ(a.combined.Vertices[i].Position[0], b.combined.Vertices[i].Position[0]);
        EXPECT_FLOAT_EQ(a.combined.Vertices[i].Position[1], b.combined.Vertices[i].Position[1]);
        EXPECT_FLOAT_EQ(a.combined.Vertices[i].Position[2], b.combined.Vertices[i].Position[2]);
    }
}

TEST(EZTreeNativeTests, GenerationSanitizesHostileOptions)
{
    EZTree::TreeOptions options = MakeSmallTree();
    options.branch.levels = 99u;
    options.branch.children = {9999u, 9999u, 9999u, 9999u};
    options.branch.sections = {0u, 0u, 0u, 0u};
    options.branch.segments = {0u, 0u, 0u, 0u};
    options.branch.length = {-1.0f, -1.0f, -1.0f, -1.0f};
    options.branch.radius = {-1.0f, -1.0f, -1.0f, -1.0f};
    options.leaves.count = 2u;
    options.trellis.enabled = true;
    options.trellis.visible = true;
    options.trellis.width = std::numeric_limits<float>::infinity();
    options.trellis.height = std::numeric_limits<float>::infinity();
    options.trellis.spacing = 0.0f;

    EZTree::Generator generator;
    const EZTree::GeneratedTree tree = generator.Generate(options);

    EXPECT_FALSE(tree.combined.Vertices.empty());
    EXPECT_LT(tree.stats.generatedBranchCount, 5000u);
    EXPECT_LT(tree.combined.Vertices.size(), 200000u);
}

TEST(EZTreeNativeTests, BranchLeafAndTrellisMeshesHaveExpectedTopology)
{
    EZTree::TreeOptions options = MakeSmallTree();
    options.trellis.enabled = true;
    options.trellis.visible = true;
    options.trellis.width = 4.0f;
    options.trellis.height = 4.0f;
    options.trellis.spacing = 2.0f;

    EZTree::Generator generator;
    const EZTree::GeneratedTree tree = generator.Generate(options);

    EXPECT_EQ(tree.stats.generatedBranchCount, 1u);
    EXPECT_EQ(tree.stats.branchVertices, (options.branch.sections[0] + 1u) * (options.branch.segments[0] + 1u));
    EXPECT_EQ(tree.stats.branchIndices, options.branch.sections[0] * options.branch.segments[0] * 6u);
    EXPECT_EQ(tree.stats.leafVertices, (1u + options.leaves.count) * 8u);
    EXPECT_EQ(tree.stats.leafIndices, (1u + options.leaves.count) * 12u);
    EXPECT_GT(tree.stats.trellisVertices, 0u);
    EXPECT_EQ(tree.combined.Vertices.size(),
              tree.branches.Vertices.size() + tree.leaves.Vertices.size() + tree.trellis.Vertices.size());
}

TEST(EZTreeNativeTests, DoubleBillboardLeafNormalsFollowEachCardRotation)
{
    EZTree::TreeOptions options = MakeSmallTree();
    options.leaves.count = 0u;
    options.leaves.roundedNormals = false;
    options.leaves.billboard = EZTree::BillboardMode::Double;

    EZTree::Generator generator;
    const EZTree::GeneratedTree tree = generator.Generate(options);

    ASSERT_GE(tree.leaves.Vertices.size(), 8u);
    const auto& firstCardNormal = tree.leaves.Vertices[0].Normal;
    const auto& secondCardNormal = tree.leaves.Vertices[4].Normal;
    const float dot =
        firstCardNormal[0] * secondCardNormal[0] +
        firstCardNormal[1] * secondCardNormal[1] +
        firstCardNormal[2] * secondCardNormal[2];

    EXPECT_LT(std::abs(dot), 0.1f);
}

// The wind modifier phases a leaf's ripple by the seed in UV1.x (ez_tree_wind.glsl). Every vertex of a
// leaf's cards carries the same seed, so no card tears between corners moving with unrelated phases,
// and the seeds differ from leaf to leaf, so the canopy does not ripple in lockstep.
TEST(EZTreeNativeTests, EachLeafCarriesOneWindSeedOnEveryVertexOfItsCards)
{
    EZTree::TreeOptions options = MakeSmallTree();
    options.leaves.billboard = EZTree::BillboardMode::Double;

    EZTree::Generator generator;
    const EZTree::GeneratedTree tree = generator.Generate(options);

    constexpr size_t kVerticesPerLeaf = 8; // two crossed cards of four vertices
    ASSERT_GE(tree.leaves.Vertices.size(), 2 * kVerticesPerLeaf);
    ASSERT_TRUE(tree.leaves.HasTexCoords1()) << "the leaf mesh carries no UV1 wind seed";
    std::vector<float> seeds;
    for (size_t leaf = 0; leaf + kVerticesPerLeaf <= tree.leaves.Vertices.size(); leaf += kVerticesPerLeaf)
    {
        const float seed = tree.leaves.TexCoords1[leaf * 2];
        for (size_t v = 0; v < kVerticesPerLeaf; ++v)
            ASSERT_EQ(tree.leaves.TexCoords1[(leaf + v) * 2], seed) << "leaf " << leaf / kVerticesPerLeaf << " vertex " << v;
        EXPECT_GE(seed, 0.0f);
        EXPECT_LT(seed, 1.0f);
        seeds.push_back(seed);
    }
    std::sort(seeds.begin(), seeds.end());
    EXPECT_EQ(std::adjacent_find(seeds.begin(), seeds.end()), seeds.end()) << "two leaves share a wind seed";
}

TEST(EZTreeNativeTests, LeafAtlasUvTileIsApplied)
{
    EZTree::TreeOptions options = MakeSmallTree();
    options.leaves.textureColumns = 4u;
    options.leaves.textureRows = 2u;
    options.leaves.textureTile = 5u;

    EZTree::Generator generator;
    const EZTree::GeneratedTree tree = generator.Generate(options);

    ASSERT_FALSE(tree.leaves.Vertices.empty());
    float minU = std::numeric_limits<float>::max();
    float maxU = std::numeric_limits<float>::lowest();
    float minV = std::numeric_limits<float>::max();
    float maxV = std::numeric_limits<float>::lowest();
    for (const auto& vertex : tree.leaves.Vertices)
    {
        minU = std::min(minU, vertex.TexCoords[0]);
        maxU = std::max(maxU, vertex.TexCoords[0]);
        minV = std::min(minV, vertex.TexCoords[1]);
        maxV = std::max(maxV, vertex.TexCoords[1]);
    }

    EXPECT_FLOAT_EQ(minU, 0.25f);
    EXPECT_FLOAT_EQ(maxU, 0.5f);
    EXPECT_FLOAT_EQ(minV, 0.5f);
    EXPECT_FLOAT_EQ(maxV, 1.0f);
}

TEST(EZTreeNativeTests, TrellisForceActivatesInsideRange)
{
    EZTree::TreeOptions options = MakeSmallTree();
    options.trellis.enabled = true;
    options.trellis.position = {0.0f, 0.0f, 0.0f};
    options.trellis.forceMaxDistance = 5.0f;
    options.trellis.forceStrength = 0.25f;

    const EZTree::TrellisForceResult inactive =
        EZTree::CalculateTrellisForce(options, {20.0f, 0.0f, 0.0f}, 1.0f);
    EXPECT_FALSE(inactive.active);

    const EZTree::TrellisForceResult active =
        EZTree::CalculateTrellisForce(options, {1.0f, 1.0f, 2.0f}, 0.5f);
    EXPECT_TRUE(active.active);
    EXPECT_GT(active.strength, 0.0f);
}

// ---------------------------------------------------------------------------
// Arc 3a acceptance B: the eztree package plugin registers AFTER the ECS
// schedule was built (package DLL load shape) and its extraction system still
// lands in exactly the wave a from-scratch build gives it — via the real
// EnginePluginRegistry late-registration handler + persistent-builder re-solve
// (seam 1). CPU-only: placement is asserted on the execution plan; nothing
// updates, so the null RenderServices context is never dereferenced.
// ---------------------------------------------------------------------------

namespace
{

class ScheduleRecorderSystem : public GameEngine::ECS::ISystem
{
public:
    explicit ScheduleRecorderSystem(const char* id) : m_Id(id) {}
    void Update(GameEngine::ECS::World&, GameEngine::float32) override {}
    const char* GetName() const override { return m_Id.c_str(); }

private:
    std::string m_Id;
};

// Mirrors the real startup schedule's shape (two wave-0 roots in different
// phases, one Render-phase consumer) — the same base the seam-1 phase
// insertion tests use. The stub names must match the production system names
// in RegisterRenderingSystems.cpp: an unresolved dependency name is dropped
// with a warning (SystemScheduling.h), so a stub named anything else would
// silently unbind the edges under test and make the ordering assertions pass
// vacuously.
void AddBaseSchedule(GameEngine::ECS::SystemScheduleBuilder& b)
{
    using namespace GameEngine::ECS;
    b.Add<ScheduleRecorderSystem>("TransformHierarchy", SystemPhase::Extraction, 1, {},
                                  "TransformHierarchy");
    b.Add<ScheduleRecorderSystem>("Camera", SystemPhase::Camera, 0, {}, "Camera");
    b.Add<ScheduleRecorderSystem>("RenderExtraction", SystemPhase::Extraction, 6,
                                  {"TransformHierarchy"}, "RenderExtraction");
    b.Add<ScheduleRecorderSystem>("RenderGraphBuild", SystemPhase::Render, 0,
                                  {"TransformHierarchy", "Camera", "RenderExtraction"},
                                  "RenderGraphBuild");
}

// Wave index of a named system in the manager's execution plan; -1 when absent,
// -2 when scheduled more than once.
int WaveIndexOf(const GameEngine::ECS::SystemManager& sm, const char* name)
{
    int found = -1;
    const auto& plan = sm.GetExecutionPlan();
    for (size_t wave = 0; wave < plan.Waves.size(); ++wave)
    {
        for (size_t idx : plan.Waves[wave].SystemIndices)
        {
            const char* systemName = sm.GetSequentialSystemName(idx);
            if (systemName && std::string_view(systemName) == name)
            {
                if (found >= 0)
                    return -2;
                found = static_cast<int>(wave);
            }
        }
    }
    return found;
}

} // namespace

TEST(EZTreeNativeTests, LateRegisteredPluginLandsInExtractionWave)
{
    using namespace GameEngine;
    using namespace GameEngine::ECS;

    // Schedule is built BEFORE the plugin exists — the package-DLL-at-project-
    // open timeline.
    SystemManager incremental;
    SystemScheduleBuilder builder;
    AddBaseSchedule(builder);
    builder.BuildAndRegisterWithWaves(incremental);
    EXPECT_EQ(WaveIndexOf(incremental, "EZTreeExtraction"), -1);

    // Real registry, real plugin, hook replay mirroring EngineCore's handler
    // (persistent builder + full re-solve; component/schema hooks replay into
    // their process-global registries).
    auto& registry = Plugins::EnginePluginRegistry::Get();
    Plugins::EnginePluginContext context{};
    bool replayed = false;
    registry.SetLateRegistrationHandler(
        [&](Plugins::IEnginePlugin& plugin)
        {
            plugin.RegisterEngineComponents();
            plugin.RegisterSceneSchemas();
            const size_t regsBefore = builder.GetRegistrationCount();
            plugin.AddSystemsToSchedule(builder, context);
            ASSERT_GT(builder.GetRegistrationCount(), regsBefore);
            builder.BuildAndRegisterWithWaves(incremental);
            replayed = true;
        });
    EZTreeECS::RegisterEZTreeEnginePlugin();
    registry.SetLateRegistrationHandler({});
    ASSERT_TRUE(replayed);

    // From-scratch reference: all systems known up front.
    SystemManager reference;
    SystemScheduleBuilder referenceBuilder;
    AddBaseSchedule(referenceBuilder);
    EZTreeECS::AddEZTreeSystemsToSchedule(referenceBuilder, nullptr);
    referenceBuilder.BuildAndRegisterWithWaves(reference);

    const int lateWave = WaveIndexOf(incremental, "EZTreeExtraction");
    const int referenceWave = WaveIndexOf(reference, "EZTreeExtraction");
    ASSERT_GE(lateWave, 0) << "extraction system missing from the re-solved plan";
    EXPECT_EQ(lateWave, referenceWave)
        << "late-integrated placement differs from a from-scratch build";
    // Placed by its declared edges — after the systems whose state it reads,
    // and still strictly before the Render-phase consumer (never a trailing
    // wave, which is where the cycle fallback would put it).
    EXPECT_GT(lateWave, WaveIndexOf(incremental, "Camera"));
    EXPECT_LT(lateWave, WaveIndexOf(incremental, "RenderGraphBuild"));
}

// EZTreeExtraction reads two pieces of state that other scheduled systems
// write in the same tick: the ViewRegistry view list (GetViews, iterated at
// EZTreeExtractionSystem.cpp:700) and WorldTransform. Systems inside one wave
// execute concurrently on JobSystem workers (Systems.h UpdateWaveBased), waves
// join only at their boundary, and ViewRegistry holds no synchronization of its
// own — CameraSystem's AllocateView appends to the m_Views vector, which
// reallocates and frees the buffer a wave-mate is iterating. Ordering therefore
// has to come from declared edges; TerrainExtraction declares exactly these two
// for exactly these two reads (RegisterTerrainSystems.cpp).
TEST(EZTreeNativeTests, ExtractionIsOrderedAfterTheWritersOfTheStateItReads)
{
    using namespace GameEngine::ECS;

    SystemManager sm;
    SystemScheduleBuilder builder;
    AddBaseSchedule(builder);
    EZTreeECS::AddEZTreeSystemsToSchedule(builder, nullptr);
    builder.BuildAndRegisterWithWaves(sm);

    const int eztree = WaveIndexOf(sm, "EZTreeExtraction");
    const int camera = WaveIndexOf(sm, "Camera");
    const int hierarchy = WaveIndexOf(sm, "TransformHierarchy");

    ASSERT_GE(eztree, 0) << "EZTreeExtraction missing from the plan";
    ASSERT_GE(camera, 0) << "Camera stub missing — the edge under test would not bind";
    ASSERT_GE(hierarchy, 0) << "TransformHierarchy stub missing — the edge would not bind";

    EXPECT_GT(eztree, camera)
        << "EZTreeExtraction shares a wave with the ViewRegistry writer (CameraSystem): "
           "GetViews() iterates m_Views while AllocateView can reallocate it";
    EXPECT_GT(eztree, hierarchy)
        << "EZTreeExtraction reads WorldTransform no later than the wave that writes it";
}

TEST(EZTreeNativeTests, ComponentFactoryCreatorIsAddableAndReloadSafe)
{
    using namespace GameEngine::ECS;

    // Ensure the plugin is in the registry (idempotent if an earlier test
    // already registered it), then run the component hook the way both the
    // startup pass and the module-reload replay do — twice, to mirror a
    // reload. ComponentFactory replaces by type id, so the Add Component
    // creator must exist exactly once afterwards.
    EZTreeECS::RegisterEZTreeEnginePlugin();
    auto& plugins = Plugins::EnginePluginRegistry::Get();
    plugins.RegisterEngineComponents();
    plugins.RegisterEngineComponents();

    const ComponentTypeId typeId = GetComponentTypeId<Components::EZTree>();
    const std::vector<ComponentTypeId> types = ComponentFactory::RegisteredTypes();
    EXPECT_EQ(std::count(types.begin(), types.end(), typeId), 1)
        << "re-registration must replace the Add Component creator, not duplicate it";

    // The creator adds a default-constructed EZTree through the type-erased
    // bytes path (the Add Component menu's code path).
    World world;
    const EntityHandle entity = world.CreateEntity();
    ASSERT_TRUE(ComponentFactory::Create(world, entity, typeId));
    const auto* tree = world.GetComponent<Components::EZTree>(entity);
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->RuntimeMeshHandleId, 0u);
    EXPECT_EQ(tree->RuntimeVertexCount, 0u);
}

TEST(EZTreeNativeTests, RuntimeMaterialShapesCarryWindAndLeafSurface)
{
    const MaterialDocument bark = EZTreeECS::MakeBarkRuntimeMaterialShape();
    EXPECT_EQ(bark.lightingModel, "StandardPBR");
    EXPECT_EQ(bark.surfaceShader, "Surfaces/standard_pbr.glsl");
    EXPECT_EQ(bark.vertexModifier, "VertexModifiers/ez_tree_wind.glsl");
    EXPECT_EQ(bark.alphaMode, MaterialAlphaMode::Opaque);

    const MaterialDocument leaves = EZTreeECS::MakeLeafRuntimeMaterialShape();
    EXPECT_EQ(leaves.surfaceShader, "Surfaces/ez_tree_leaves.glsl");
    EXPECT_EQ(leaves.vertexModifier, "VertexModifiers/ez_tree_wind.glsl");
    EXPECT_EQ(leaves.alphaMode, MaterialAlphaMode::Mask);

    const MaterialDocument trellis = EZTreeECS::MakeTrellisRuntimeMaterialShape();
    EXPECT_EQ(trellis.surfaceShader, "Surfaces/standard_pbr.glsl");
    EXPECT_TRUE(trellis.vertexModifier.empty());
    EXPECT_EQ(trellis.alphaMode, MaterialAlphaMode::Opaque);
}

TEST(EZTreeNativeTests, DefaultTexturePathsKeepLeafAlbedoPngAndDerivedJpeg)
{
    using EZTreeECS::DefaultBarkTextureRelativePath;
    using EZTreeECS::DefaultLeafTextureRelativePath;
    using EZTreeECS::LeafTextureExtension;

    EXPECT_STREQ(LeafTextureExtension("color"), ".png");
    EXPECT_STREQ(LeafTextureExtension("normal"), ".jpg");
    EXPECT_STREQ(LeafTextureExtension("roughness"), ".jpg");
    EXPECT_STREQ(LeafTextureExtension("ao"), ".jpg");

    EXPECT_EQ(DefaultLeafTextureRelativePath(EZTree::LeafType::Oak).generic_string(),
              "Textures/EZTree/leaves/oak_color.png");
    EXPECT_EQ(DefaultLeafTextureRelativePath(EZTree::LeafType::Ash, "normal").generic_string(),
              "Textures/EZTree/leaves/ash_normal.jpg");
    EXPECT_EQ(DefaultLeafTextureRelativePath(EZTree::LeafType::Aspen, "roughness").generic_string(),
              "Textures/EZTree/leaves/aspen_roughness.jpg");
    EXPECT_EQ(DefaultLeafTextureRelativePath(EZTree::LeafType::Pine, "ao").generic_string(),
              "Textures/EZTree/leaves/pine_ao.jpg");
    EXPECT_EQ(DefaultBarkTextureRelativePath(EZTree::BarkType::Oak, "color").generic_string(),
              "Textures/EZTree/bark/oak_color_1k.jpg");
}

// ---------------------------------------------------------------------------
// Texture classification. Every default bark and leaf map is bound to a fixed
// material slot by the extraction system, and the slot decides both the block
// compression the build cooks (TextureCookUsageForMaterialSlot) and the colour
// space it uploads with (IsLinearTextureSlot). Both rows live in the package's
// committed .assetmanifest, and because an engine package derives no import
// data at runtime, nothing infers them at bind time: rename a texture or move
// one to another slot and the package silently goes back to shipping
// uncompressed pixels, or to uploading a normal map as sRGB, which no other
// gate would notice.
// ---------------------------------------------------------------------------

namespace
{

// The slot table the extraction system builds its material documents from
// (EZTreeECS::kDefaultTextureSlots). Reading it here rather than restating it
// is the whole point: a slot that moves has to break this test.
using EZTreeECS::DefaultTextureSlot;
using EZTreeECS::kDefaultTextureSlots;

constexpr EZTree::BarkType kBarkTypes[] = {
    EZTree::BarkType::Oak, EZTree::BarkType::Birch,
    EZTree::BarkType::Pine, EZTree::BarkType::Willow};

constexpr EZTree::LeafType kLeafTypes[] = {
    EZTree::LeafType::Oak, EZTree::LeafType::Ash,
    EZTree::LeafType::Aspen, EZTree::LeafType::Pine};

// The package's manifest staged beside this executable — tests read staged
// fixtures, never the source tree.
std::filesystem::path StagedEZTreeManifestFile()
{
    return PathUtils::GetExecutableDirectory() / "TestData" / "Packages" / "eztree" /
           "Assets" / ".assetmanifest";
}

// The import setting the manifest records for a package-relative path, or an
// empty string when the manifest has no row for it or the row carries no such
// key.
std::string ManifestTextureSetting(const AssetDatabase::AssetStore_TextJsonl& manifest,
                                   const std::filesystem::path& relativePath, const char* key)
{
    const std::optional<GUID> guid = manifest.LookupGuidByPath(relativePath.generic_string());
    if (!guid)
        return {};
    AssetDatabase::AssetRecord record{};
    if (!manifest.TryGetAsset(*guid, record))
        return {};
    const auto setting = record.kv.find(key);
    return setting == record.kv.end() ? std::string{} : setting->second;
}

std::string UsageForSlot(const char* materialSlot)
{
    return TextureCookUsageMetaValue(TextureCookUsageForMaterialSlot(HashStringId(materialSlot)));
}

// A colour slot keeps its source-extension guess and carries no row at all;
// every data slot must say linear, or it uploads gamma-decoded.
std::string ColorSpaceForSlot(const char* materialSlot)
{
    return IsLinearTextureSlot(HashStringId(materialSlot))
               ? TextureColorSpaceMetaValue(TextureColorSpace::Linear)
               : std::string{};
}

// Both settings the slot implies, checked against the row the manifest ships.
void ExpectSlotSettings(const AssetDatabase::AssetStore_TextJsonl& manifest,
                        const std::filesystem::path& relative, const DefaultTextureSlot& slot)
{
    EXPECT_EQ(ManifestTextureSetting(manifest, relative, kTextureUsageMetaKey),
              UsageForSlot(slot.MaterialSlot))
        << relative.generic_string() << " is bound to " << slot.MaterialSlot;
    EXPECT_EQ(ManifestTextureSetting(manifest, relative, kTextureColorSpaceMetaKey),
              ColorSpaceForSlot(slot.MaterialSlot))
        << relative.generic_string() << " is bound to " << slot.MaterialSlot;
}

} // namespace

TEST(EZTreeNativeTests, DefaultTexturesDeclareTheImportSettingsTheirMaterialSlotImplies)
{
    const std::filesystem::path manifestFile = StagedEZTreeManifestFile();
    ASSERT_TRUE(std::filesystem::exists(manifestFile))
        << manifestFile.string() << " missing — the eztree package manifest is not staged";

    AssetDatabase::AssetStore_TextJsonl manifest(nullptr);
    ASSERT_TRUE(manifest.LoadFromFile(manifestFile, nullptr));

    size_t declared = 0;
    for (const EZTree::BarkType bark : kBarkTypes)
    {
        for (const DefaultTextureSlot& slot : kDefaultTextureSlots)
        {
            ExpectSlotSettings(manifest, EZTreeECS::DefaultBarkTextureRelativePath(bark, slot.Suffix),
                               slot);
            ++declared;
        }
    }
    for (const EZTree::LeafType leaf : kLeafTypes)
    {
        for (const DefaultTextureSlot& slot : kDefaultTextureSlots)
        {
            ExpectSlotSettings(manifest, EZTreeECS::DefaultLeafTextureRelativePath(leaf, slot.Suffix),
                               slot);
            ++declared;
        }
    }
    EXPECT_EQ(declared, 32u) << "the default bark/leaf texture set changed shape";

    // And nothing the package publishes is left unclassified: an unclassified
    // texture is one the build cooks as uncompressed pixels.
    std::vector<std::string> unclassified;
    for (const AssetDatabase::AssetRecord& record : manifest.EnumerateAssets())
    {
        if (record.type != AssetType::Texture || record.missing)
            continue;
        if (record.kv.find(kTextureUsageMetaKey) == record.kv.end())
            unclassified.push_back(record.path);
    }
    EXPECT_TRUE(unclassified.empty())
        << unclassified.size() << " package texture(s) carry no import usage, starting with "
        << (unclassified.empty() ? std::string{} : unclassified.front());
}
