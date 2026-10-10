#include "MeshVariantTable.h"
#include "VariantRequests.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <set>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Tools::MaterialCook;

TEST(MaterialCookVariants, MaskAddsMeshDepthPassWithoutChangingOpaqueRows)
{
    const VariantRequest mesh[] = {{"color", MaterialKeyword::Shadows, VertexAttributeFlags::StandardMesh}};
    const VariantRequest depth[] = {{"depth", MaterialKeyword::DepthOnlyFragment, VertexAttributeFlags::SkinnedMesh}};
    EXPECT_EQ(SelectVariantRequests(mesh, {}, depth, MaterialAlphaMode::Opaque).size(), 1u);
    const auto masked = SelectVariantRequests(mesh, {}, depth, MaterialAlphaMode::Mask);
    ASSERT_EQ(masked.size(), 2u);
    EXPECT_EQ(masked[1].PassKeywords, MaterialKeyword::DepthOnlyFragment);
    EXPECT_EQ(masked[1].VertexFlags, VertexAttributeFlags::SkinnedMesh);
}

TEST(MaterialCookVariants, ContributorMaskKeepsItsGeometryContract)
{
    const VariantRequest mesh[] = {{"mesh", MaterialKeyword::Shadows, VertexAttributeFlags::SkinnedMesh}};
    const VariantRequest custom[] = {{"grass", MaterialKeyword::Shadows | MaterialKeyword::HasVertexMod,
                                     VertexAttributeFlags::None}};
    const VariantRequest depth[] = {{"depth", MaterialKeyword::DepthOnlyFragment, VertexAttributeFlags::SkinnedMesh}};
    auto rows = SelectVariantRequests(mesh, custom, depth, MaterialAlphaMode::Mask);
    ASSERT_EQ(rows.size(), 1u);
    ExpandOptionalPassVariants(rows);
    ASSERT_EQ(rows.size(), 2u);
    for (const auto& row : rows)
    {
        EXPECT_EQ(row.VertexFlags, VertexAttributeFlags::None);
        EXPECT_TRUE(HasKeyword(row.PassKeywords, MaterialKeyword::HasVertexMod));
    }
}

TEST(MaterialCookVariants, OptionalShadowsPreserveLightingAndDoNotDuplicateOrExpandDepth)
{
    const auto lighting = MaterialKeyword::Shadows | MaterialKeyword::IBL |
        MaterialKeyword::SSSRNormalRoughness | MaterialKeyword::GTAO | MaterialKeyword::DDGI;
    std::vector<VariantRequest> rows = {
        {"base", MaterialKeyword::None, VertexAttributeFlags::StandardMesh},
        {"forward", lighting, VertexAttributeFlags::StandardMeshWithTangent},
        {"depth", MaterialKeyword::Shadows | MaterialKeyword::DepthOnlyFragment, VertexAttributeFlags::StandardMesh}};
    ExpandOptionalPassVariants(rows);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows.back().PassKeywords, lighting | MaterialKeyword::ScreenSpaceShadows);
    EXPECT_EQ(rows.back().VertexFlags, VertexAttributeFlags::StandardMeshWithTangent);
    ExpandOptionalPassVariants(rows);
    EXPECT_EQ(rows.size(), 4u);
}

TEST(MaterialCookVariants, AMaterialReadingVertexColourCooksTheColourStreamRows)
{
    const auto colour = MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows;
    const auto coverage = MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment;
    const std::vector<VariantRequest> table = {
        {"base", MaterialKeyword::None, VertexAttributeFlags::StandardMesh},
        {"forward-color+tangent", colour, VertexAttributeFlags::StandardMeshWithTangent},
        {"depth", MaterialKeyword::Instanced, VertexAttributeFlags::StandardMesh},
        {"depth-mask+skin", coverage, VertexAttributeFlags::SkinnedMesh}};

    auto rows = table;
    ExpandVertexColorVariants(rows, true);
    ASSERT_EQ(rows.size(), 6u) << "the colour pass and the coverage depth rows, not the base or opaque depth";
    EXPECT_EQ(rows[4].Name, "forward-color+tangent+color");
    EXPECT_EQ(rows[4].PassKeywords, colour);
    EXPECT_EQ(rows[4].VertexFlags, VertexAttributeFlags::StandardMeshWithTangent | VertexAttributeFlags::HasColor);
    EXPECT_EQ(rows[5].PassKeywords, coverage);
    EXPECT_EQ(rows[5].VertexFlags, VertexAttributeFlags::SkinnedMesh | VertexAttributeFlags::HasColor);
    ExpandVertexColorVariants(rows, true);
    EXPECT_EQ(rows.size(), 6u) << "a row that has the colour stream is not twinned again";

    auto ignoring = table;
    ExpandVertexColorVariants(ignoring, false);
    EXPECT_EQ(ignoring.size(), table.size()) << "a material that ignores vertex colour lists none";
}

TEST(MaterialCookVariants, ExistingEnabledRowIsNotCookedTwice)
{
    std::vector<VariantRequest> rows = {
        {"on", MaterialKeyword::Shadows | MaterialKeyword::ScreenSpaceShadows, VertexAttributeFlags::StandardMesh},
        {"off", MaterialKeyword::Shadows, VertexAttributeFlags::StandardMesh}};
    ExpandOptionalPassVariants(rows);
    EXPECT_EQ(rows.size(), 2u);
}

TEST(MaterialCookVariants, AHeightMappedMaterialCooksTheReliefDepthOnTheDesktopOnly)
{
    const auto colour = MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows;
    const std::vector<VariantRequest> table = {
        {"base", MaterialKeyword::None, VertexAttributeFlags::StandardMesh},
        {"forward-color", colour, VertexAttributeFlags::StandardMesh},
        {"depth", MaterialKeyword::Instanced, VertexAttributeFlags::SkinnedMesh},
        {"depth-mask", MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment, VertexAttributeFlags::StandardMesh}};

    auto desktop = table;
    ExpandParallaxDepthVariants(desktop, true, false);
    ASSERT_EQ(desktop.size(), 9u);
    EXPECT_EQ(desktop[4].Name, "forward-color-prepass-depth");
    EXPECT_EQ(desktop[4].PassKeywords, colour | MaterialKeyword::ParallaxDepthFromPrepass);
    EXPECT_EQ(desktop[5].Name, "forward-color-prepass-depth-msaa");
    EXPECT_EQ(desktop[5].PassKeywords, colour | MaterialKeyword::ParallaxDepthFromPrepass |
                                           MaterialKeyword::ParallaxPrepassDepthMultisample);
    EXPECT_EQ(desktop[6].Name, "forward-color-prepass-tolerance");
    EXPECT_EQ(desktop[6].PassKeywords,
              colour | MaterialKeyword::ParallaxDepthOffset | MaterialKeyword::ParallaxDepthTolerance)
        << "the tolerant colour pass of a view whose forward contributors keep the depth writable";
    EXPECT_EQ(desktop[7].Name, "depth-depth-offset");
    EXPECT_EQ(desktop[7].PassKeywords,
              MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment | MaterialKeyword::ParallaxDepthOffset)
        << "the prepass head that marches and writes the relief's depth";
    EXPECT_EQ(desktop[7].VertexFlags, VertexAttributeFlags::SkinnedMesh);
    EXPECT_EQ(desktop[8].Name, "depth-mask-depth-offset");

    for (const bool web : {true, false})
    {
        auto rows = table;
        ExpandParallaxDepthVariants(rows, web, web);
        EXPECT_EQ(rows.size(), table.size()) << (web ? "a web cook never lists the relief's depth"
                                                     : "a material without a height map lists none");
        for (const auto& row : rows)
            EXPECT_FALSE(HasKeyword(row.PassKeywords, MaterialKeyword::ParallaxDepthOffset));
    }
}

// Every alpha mode an imported mesh material can carry has a standard-PBR target, so a cooked-only
// runtime finds a program for an opaque, a masked and an alpha-blended glTF material alike.
TEST(MaterialCookVariants, EveryImportedAlphaModeHasAStandardPbrTarget)
{
    for (const MaterialAlphaMode mode : {MaterialAlphaMode::Opaque, MaterialAlphaMode::Mask, MaterialAlphaMode::Blend})
        EXPECT_TRUE(std::any_of(std::begin(kImportedMeshMaterialTargets), std::end(kImportedMeshMaterialTargets),
                                [mode](const ImportedMeshMaterialTarget& target) { return target.AlphaMode == mode; }))
            << "no imported mesh material target for alpha mode " << MaterialAlphaModeToString(mode);
}

// Every imported-material target cooks the keyword-less base variant: material registration prewarms
// it, and a material without its base pipeline never draws.
TEST(MaterialCookVariants, EveryImportedMaterialTargetCooksTheBaseVariant)
{
    for (const ImportedMeshMaterialTarget& target : kImportedMeshMaterialTargets)
    {
        const std::span<const VariantRequest> rows = target.Rows.empty() ? std::span<const VariantRequest>(kMeshVariants) : target.Rows;
        EXPECT_TRUE(std::any_of(rows.begin(), rows.end(), [](const VariantRequest& row) { return row.PassKeywords == MaterialKeyword::None; }))
            << target.Name << " cooks no base variant";
    }
}

// Every pass-keyword set the table cooks for a mesh is cooked for all four vertex buckets: a skinned
// model drawn under a keyword its static twin has (a DDGI volume, a GTAO volume) finds its program.
TEST(MaterialCookVariants, EveryMeshKeywordSetIsCookedForStaticAndSkinnedMeshes)
{
    std::set<MaterialKeyword> keywordSets;
    for (const VariantRequest& row : kMeshVariants)
        if (row.PassKeywords != MaterialKeyword::None)
            keywordSets.insert(row.PassKeywords);
    const VertexAttributeFlags buckets[] = {VertexAttributeFlags::StandardMesh, VertexAttributeFlags::StandardMeshWithTangent,
                                            VertexAttributeFlags::SkinnedMesh, VertexAttributeFlags::SkinnedMeshWithTangent};
    for (const MaterialKeyword keywords : keywordSets)
    {
        for (const VertexAttributeFlags bucket : buckets)
        {
            const bool cooked = std::any_of(std::begin(kMeshVariants), std::end(kMeshVariants), [&](const VariantRequest& row)
                                            { return row.PassKeywords == keywords && row.VertexFlags == bucket; });
            EXPECT_TRUE(cooked) << "keywords 0x" << std::hex << static_cast<uint64_t>(keywords) << " lack vertex bucket 0x"
                                << static_cast<uint64_t>(bucket);
        }
    }
}
