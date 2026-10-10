// Tests for MaterialRegistry: GUID-keyed registration, deduplication,
// property initialization from MaterialDocument, dirty upload, and
// integration with RenderServices.

#include <gtest/gtest.h>

#include "Assets/ModelAsset.h"
#include "Engine/Rendering/MaterialDepthClassify.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialPrewarmVariants.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/ParallaxReliefDepth.h"
#include "Engine/Rendering/ModelMaterialBridge.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderPropertyTable.h"
#include "Types/StringId.h"

#include "ScopedCompatShaderProfile.h"
#include "StagedTestPaths.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

#include "TestDeviceHelper.h"

TEST(MaterialPrewarmVariantsTest, IncludesBaseColorDepthAndSkinnedLayouts)
{
    ShaderVariantKey base{};
    base.vertexFlags = VertexAttributeFlags::StandardMesh;
    base.userKeywordHash = 12345;
    base.lightingModel = HashStringId("StandardPBR");
    base.materialKeywords = MaterialKeyword::AlphaTest | MaterialKeyword::ClearCoat;
    const auto color = MaterialKeyword::ForwardPlus | MaterialKeyword::IBL | MaterialKeyword::Instanced;
    const auto keys = MaterialPrewarmVariantKeys(base, color, false, MaterialAlphaMode::Mask);
    // The base, 16 colour keys, and the coverage head and its crossfade tail on the standard and skinned depth
    // layouts, without and with the vertex-colour stream.
    ASSERT_EQ(keys.size(), 25u);
    EXPECT_EQ(keys.front(), base);
    for (const auto& key : keys)
    {
        EXPECT_EQ(key.userKeywordHash, base.userKeywordHash);
        EXPECT_EQ(key.lightingModel, base.lightingModel);
        EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::AlphaTest));
        EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::ClearCoat));
    }
    EXPECT_EQ(std::count_if(keys.begin(), keys.end(), [](const auto& key) {
        return (key.vertexFlags & VertexAttributeFlags::Skinned) != VertexAttributeFlags::None;
    }), 12);
}

TEST(MaterialPrewarmVariantsTest, GlassIncludesGrabVariants)
{
    ShaderVariantKey base{};
    base.materialKeywords = MaterialKeyword::Transmission;
    const auto keys = MaterialPrewarmVariantKeys(base, MaterialKeyword::ForwardPlus, false,
                                                 MaterialAlphaMode::Opaque);
    ASSERT_EQ(keys.size(), 27u);
    EXPECT_EQ(std::count_if(keys.begin(), keys.end(), [](const auto& key) {
        return HasKeyword(key.materialKeywords, MaterialKeyword::SceneColorGrab);
    }), 8);
    // Glass draws in no opaque depth pass, only in the tint pass. That pass is a
    // light-space cascade and draws heads alone, so no glass key takes the
    // depth-only coverage keyword a crossfade tail would add.
    for (const auto& key : keys)
        EXPECT_FALSE(HasKeyword(key.materialKeywords, MaterialKeyword::DepthOnlyFragment));
    EXPECT_EQ(std::count_if(keys.begin(), keys.end(), [](const auto& key) {
        return HasKeyword(key.materialKeywords, MaterialKeyword::DepthOnlyTransmissionColor);
    }), 2);
}

// Every key the warm-up requests for a glass material composes and compiles through the real
// ShaderComposer -> ShaderCompileService path. The document mirrors
// Examples/Materials/OpenPBR/glass_thick_absorbing_short.material. A warm key no adapter shape
// can compose fails on every session that opens a glass material, as a permanent entry in the
// editor's Shader Errors window that hides the new ones.
TEST(MaterialPrewarmVariantsTest, EveryGlassWarmKeyCompiles)
{
    const std::filesystem::path shaderDir = TestPaths::StagedRenderingShadersDir();
    if (!std::filesystem::exists(shaderDir))
        GTEST_SKIP() << "Staged engine shader tree not found: " << shaderDir.string();
    if (!ShaderCompileService::IsCompilerAvailable())
        GTEST_SKIP() << "no shader compiler in this build";

    const std::filesystem::path cacheRoot =
        std::filesystem::temp_directory_path() / ("ge_prewarm_glass_" + GUID::Generate().ToString());
    MaterialBuildContext context{};
    context.AdapterShaderDir = shaderDir;
    context.CacheRoot = cacheRoot / "Shaders";

    MaterialDocument doc{};
    doc.materialName = "PrewarmGlassProbe";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    ShaderVariantKey base{};
    base.vertexFlags = VertexAttributeFlags::StandardMesh;
    base.lightingModel = HashStringId("StandardPBR");
    base.materialKeywords = MaterialKeyword::Transmission | MaterialKeyword::TransmissionThick;
    const auto color = MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::IBL |
                       MaterialKeyword::Instanced;
    const auto keys = MaterialPrewarmVariantKeys(base, color, false, MaterialAlphaMode::Opaque);
    ASSERT_FALSE(keys.empty());
    for (const auto& key : keys)
    {
        const MaterialBuildResult result =
            BuildMaterialToShaderPackage(doc, cacheRoot / "PrewarmGlassProbe.material", "PrewarmGlassProbe",
                                         context, ShaderSourceKind::SpirV, key.materialKeywords, key.vertexFlags);
        EXPECT_TRUE(result.success) << "keywords 0x" << std::hex << static_cast<uint64_t>(key.materialKeywords)
                                    << " vertex flags 0x" << static_cast<uint32_t>(key.vertexFlags) << std::dec
                                    << ": " << (result.errors.empty() ? std::string{} : result.errors.front());
    }
    std::error_code ec;
    std::filesystem::remove_all(cacheRoot, ec);
}

// A height map bound on the standard surface compiles the relief march into every shading key the
// warm-up requests, on the desktop arm and on the compat arm (the WebGPU-class fold with its fixed
// low budget), for opaque and masked materials. The depth-only and motion shapes decide coverage on
// the polygon, which is also what a light cascade must use, so they compose without the march.
namespace
{
bool FragmentNames(const MaterialBuildResult& result, const std::string& name)
{
    if (!result.package)
        return false;
    const auto it = result.package->stageBytes.find("fs");
    return it != result.package->stageBytes.end() &&
           std::search(it->second.begin(), it->second.end(), name.begin(), name.end()) != it->second.end();
}

std::string KeywordsLabel(MaterialKeyword keywords)
{
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "keywords 0x%llx", static_cast<unsigned long long>(keywords));
    return buffer;
}

void ExpectEveryParallaxWarmKeyCompiles(MaterialAlphaMode alphaMode)
{
    const std::filesystem::path shaderDir = TestPaths::StagedRenderingShadersDir();
    ASSERT_TRUE(std::filesystem::exists(shaderDir)) << "Staged engine shader tree not found: " << shaderDir.string();
    ASSERT_TRUE(ShaderCompileService::IsCompilerAvailable()) << "no shader compiler in this build";

    const std::filesystem::path cacheRoot =
        std::filesystem::temp_directory_path() / ("ge_prewarm_parallax_" + GUID::Generate().ToString());
    MaterialBuildContext context{};
    context.AdapterShaderDir = shaderDir;
    context.CacheRoot = cacheRoot / "Shaders";

    MaterialDocument doc{};
    doc.materialName = "PrewarmParallaxProbe";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.alphaMode = alphaMode;
    doc.textures["heightMap"] = "__embedded__:0";

    ShaderVariantKey base{};
    base.vertexFlags = VertexAttributeFlags::StandardMesh;
    base.lightingModel = HashStringId("StandardPBR");
    const auto color = MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::IBL |
                       MaterialKeyword::Instanced | MaterialKeyword::SSSRNormalRoughness | MaterialKeyword::GTAO;
    const auto keys = MaterialPrewarmVariantKeys(base, color, false, alphaMode);
    ASSERT_FALSE(keys.empty());
    size_t marching = 0;
    for (const auto& key : keys)
    {
        const MaterialBuildResult result =
            BuildMaterialToShaderPackage(doc, cacheRoot / "PrewarmParallaxProbe.material", "PrewarmParallaxProbe",
                                         context, ShaderSourceKind::SpirV, key.materialKeywords, key.vertexFlags);
        const std::string label = KeywordsLabel(key.materialKeywords);
        ASSERT_TRUE(result.success) << label << ": " << (result.errors.empty() ? std::string{} : result.errors.front());
        EXPECT_NE(std::find(result.composedDefines.begin(), result.composedDefines.end(), "GE_PARALLAX_ENABLED"),
                  result.composedDefines.end())
            << label << ": the bound height map did not derive the keyword";
        const bool shadesNothing = HasKeyword(key.materialKeywords, MaterialKeyword::DepthOnlyFragment) ||
                                   HasKeyword(key.materialKeywords, MaterialKeyword::MotionVectors) ||
                                   HasKeyword(key.materialKeywords, MaterialKeyword::DepthOnlyTransmissionColor);
        EXPECT_EQ(FragmentNames(result, "GE_ParallaxMarch"), !shadesNothing) << label;
        marching += shadesNothing ? 0u : 1u;
    }
    EXPECT_GT(marching, 0u) << "no key compiled the march at all";
    std::error_code ec;
    std::filesystem::remove_all(cacheRoot, ec);
}
} // namespace

TEST(MaterialPrewarmVariantsTest, EveryParallaxWarmKeyCompiles)
{
    ExpectEveryParallaxWarmKeyCompiles(MaterialAlphaMode::Opaque);
    ExpectEveryParallaxWarmKeyCompiles(MaterialAlphaMode::Mask);
}

TEST(MaterialPrewarmVariantsTest, EveryParallaxWarmKeyCompilesOnTheCompatArm)
{
    TestSupport::ScopedCompatShaderProfile compat;
    ExpectEveryParallaxWarmKeyCompiles(MaterialAlphaMode::Opaque);
    ExpectEveryParallaxWarmKeyCompiles(MaterialAlphaMode::Mask);
}

// The asset preview draws its world pass with Instanced alone, on whatever layout
// the previewed mesh has, and its crossfading tails with LodCrossfade added. An
// opaque material without a vertex modifier draws its depth and shadow heads on
// the shared depth pipeline, so beyond the preview's keys its only own depth
// variants are the two crossfade tails. A vertex-modified material's Instanced
// depth heads are the same keys as its preview variants.
TEST(MaterialPrewarmVariantsTest, IncludesAssetPreviewPassVariants)
{
    ShaderVariantKey base{};
    base.vertexFlags = VertexAttributeFlags::StandardMesh;
    const auto color = MaterialKeyword::ForwardPlus | MaterialKeyword::IBL | MaterialKeyword::Instanced;
    const auto keys = MaterialPrewarmVariantKeys(base, color, false, MaterialAlphaMode::Opaque);
    ASSERT_EQ(keys.size(), 19u);
    constexpr auto standard = VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal |
                              VertexAttributeFlags::HasUV0;
    constexpr VertexAttributeFlags layouts[] = {
        standard, standard | VertexAttributeFlags::HasTangent, standard | VertexAttributeFlags::Skinned,
        standard | VertexAttributeFlags::HasTangent | VertexAttributeFlags::Skinned};
    const auto countWithKeywords = [](const auto& set, MaterialKeyword keywords) {
        return std::count_if(set.begin(), set.end(),
                             [&](const auto& key) { return key.materialKeywords == keywords; });
    };
    EXPECT_EQ(countWithKeywords(keys, kAssetPreviewPassKeywords), 4);
    for (const VertexAttributeFlags layout : layouts)
    {
        ShaderVariantKey key = base;
        key.vertexFlags = layout;
        key.materialKeywords = kAssetPreviewPassKeywords;
        EXPECT_NE(std::find(keys.begin(), keys.end(), key), keys.end())
            << "vertex flags " << static_cast<uint32_t>(layout);
        key.materialKeywords |= MaterialKeyword::LodCrossfade;
        EXPECT_NE(std::find(keys.begin(), keys.end(), key), keys.end())
            << "crossfade, vertex flags " << static_cast<uint32_t>(layout);
    }

    ShaderVariantKey modified = base;
    modified.materialKeywords = MaterialKeyword::HasVertexMod;
    const auto modifiedKeys = MaterialPrewarmVariantKeys(modified, color, false, MaterialAlphaMode::Opaque);
    EXPECT_EQ(countWithKeywords(modifiedKeys, MaterialKeyword::HasVertexMod | kAssetPreviewPassKeywords), 4);
}

// A LOD transition draws its fading tail with LodCrossfade in both the colour pass
// (the pass keywords plus LodCrossfade) and the depth prepass (DepthOnlyFragment
// plus LodCrossfade over the tangent-stripped layout), skinned meshes included;
// without these the first crossfade after a scene opens compiles on the draw path.
TEST(MaterialPrewarmVariantsTest, IncludesLodCrossfadeTailVariants)
{
    ShaderVariantKey base{};
    base.vertexFlags = VertexAttributeFlags::StandardMesh;
    const auto color = MaterialKeyword::ForwardPlus | MaterialKeyword::IBL | MaterialKeyword::Instanced;
    const auto keys = MaterialPrewarmVariantKeys(base, color, false, MaterialAlphaMode::Opaque);
    const auto has = [&](VertexAttributeFlags flags, MaterialKeyword keywords) {
        ShaderVariantKey key = base;
        key.vertexFlags = flags;
        key.materialKeywords = keywords;
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    };
    constexpr auto standard = VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal |
                              VertexAttributeFlags::HasUV0;
    EXPECT_TRUE(has(standard, color | MaterialKeyword::LodCrossfade));
    EXPECT_TRUE(has(standard | VertexAttributeFlags::HasTangent, color | MaterialKeyword::LodCrossfade));
    EXPECT_TRUE(has(standard, MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment |
                                  MaterialKeyword::LodCrossfade));
    constexpr auto skinned = standard | VertexAttributeFlags::Skinned;
    EXPECT_TRUE(has(skinned, color | MaterialKeyword::LodCrossfade));
    EXPECT_TRUE(has(skinned | VertexAttributeFlags::HasTangent, color | MaterialKeyword::LodCrossfade));
    EXPECT_TRUE(has(skinned, MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment |
                                 MaterialKeyword::LodCrossfade));

    const auto procedural = MaterialPrewarmVariantKeys(base, color, true, MaterialAlphaMode::Opaque);
    EXPECT_EQ(std::count_if(procedural.begin(), procedural.end(), [](const auto& key) {
        return HasKeyword(key.materialKeywords, MaterialKeyword::LodCrossfade);
    }), 0);
}

// A Mask material's depth prepass and shadow passes draw its head segments with the
// per-material coverage variant (Instanced plus DepthOnlyFragment over the
// tangent-stripped layouts): the shared depth pipeline has no fragment stage to
// run the alpha test.
TEST(MaterialPrewarmVariantsTest, MaskIncludesCoverageDepthVariant)
{
    ShaderVariantKey base{};
    base.vertexFlags = VertexAttributeFlags::StandardMesh;
    base.materialKeywords = MaterialKeyword::AlphaTest;
    const auto color = MaterialKeyword::ForwardPlus | MaterialKeyword::IBL | MaterialKeyword::Instanced;
    const auto keys = MaterialPrewarmVariantKeys(base, color, false, MaterialAlphaMode::Mask);
    constexpr auto standard = VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal |
                              VertexAttributeFlags::HasUV0;
    for (const auto flags : {standard, standard | VertexAttributeFlags::Skinned})
    {
        ShaderVariantKey key = base;
        key.vertexFlags = flags;
        key.materialKeywords = MaterialKeyword::AlphaTest | MaterialKeyword::Instanced |
                               MaterialKeyword::DepthOnlyFragment;
        EXPECT_NE(std::find(keys.begin(), keys.end(), key), keys.end())
            << "vertex flags " << static_cast<uint32_t>(flags);
        key.materialKeywords |= MaterialKeyword::LodCrossfade;
        EXPECT_NE(std::find(keys.begin(), keys.end(), key), keys.end())
            << "crossfade, vertex flags " << static_cast<uint32_t>(flags);
    }
}

// A parallax material's camera prepass head marches the relief and writes its depth, off the
// shared depth pipeline; the shadow cascades keep the polygon on the shared pipeline, so the flat
// polygon casts and the depth class does not change. Its crossfade tail keeps the relief's depth.
TEST(MaterialPrewarmVariantsTest, OnlyTheCameraPrepassWritesTheReliefDepth)
{
    const MaterialKeyword pass = MaterialKeyword::Instanced;
    const DepthSegmentPipelineChoice prepass =
        ChooseDepthHeadPipeline(pass, DepthPassType::Prepass, false, false, true, true);
    EXPECT_EQ(prepass.Keywords,
              MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment | MaterialKeyword::ParallaxDepthOffset);
    EXPECT_TRUE(prepass.ComposesFragment);
    EXPECT_FALSE(prepass.UseSharedDepth);
    const DepthSegmentPipelineChoice tail =
        ChooseDepthSegmentPipeline(prepass.Keywords, prepass.UseSharedDepth, prepass.ComposesFragment, true);
    EXPECT_EQ(tail.Keywords, prepass.Keywords | MaterialKeyword::LodCrossfade);

    for (const DepthPassType cascade : {DepthPassType::ShadowCascade, DepthPassType::SpotShadow})
    {
        const DepthSegmentPipelineChoice head = ChooseDepthHeadPipeline(pass, cascade, false, false, true, true);
        EXPECT_EQ(head.Keywords, pass) << static_cast<int>(cascade);
        EXPECT_TRUE(head.UseSharedDepth) << static_cast<int>(cascade);
        EXPECT_FALSE(head.ComposesFragment) << static_cast<int>(cascade);
    }
    const DepthSegmentPipelineChoice flat =
        ChooseDepthHeadPipeline(pass | MaterialKeyword::ParallaxDepthOffset, DepthPassType::Prepass, false, false, true, false);
    EXPECT_EQ(flat.Keywords, pass) << "a material that does not march never carries the relief's depth";
    EXPECT_TRUE(flat.UseSharedDepth);
    {
        TestSupport::ScopedCompatShaderProfile compat;
        const DepthSegmentPipelineChoice head =
            ChooseDepthHeadPipeline(pass, DepthPassType::Prepass, false, false, false, true);
        EXPECT_FALSE(HasKeyword(head.Keywords, MaterialKeyword::ParallaxDepthOffset))
            << "a compat key never carries the relief's depth";
    }

    Material relief = Material::TestFactory::Create(GUID::Generate(), "ReliefDepthClassProbe", 32u);
    ShaderVariantKey key{};
    key.materialKeywords = MaterialKeyword::Parallax;
    Material::TestFactory::SetVariantKey(relief, key);
    Material::TestFactory::SetAlphaMode(relief, MaterialAlphaMode::Opaque);
    EXPECT_EQ(ClassifyMaterialDepthClass(relief), MaterialDepthClass::EligibleSingleSided);

    ShaderVariantKey base{};
    base.vertexFlags = VertexAttributeFlags::StandardMesh;
    base.materialKeywords = MaterialKeyword::Parallax;
    const auto keys = MaterialPrewarmVariantKeys(base, MaterialKeyword::ForwardPlus | MaterialKeyword::Instanced, false,
                                                 MaterialAlphaMode::Opaque);
    ShaderVariantKey warmed = base;
    warmed.vertexFlags = VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal | VertexAttributeFlags::HasUV0;
    warmed.materialKeywords = MaterialKeyword::Parallax | MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment |
                              MaterialKeyword::ParallaxDepthOffset;
    EXPECT_NE(std::find(keys.begin(), keys.end(), warmed), keys.end()) << "the prepass head is warmed";
    for (const MaterialKeyword read : {MaterialKeyword::ParallaxDepthFromPrepass,
                                       MaterialKeyword::ParallaxDepthFromPrepass | MaterialKeyword::ParallaxPrepassDepthMultisample})
    {
        warmed.materialKeywords = MaterialKeyword::Parallax | MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus | read;
        EXPECT_NE(std::find(keys.begin(), keys.end(), warmed), keys.end())
            << "the colour pass after the prepass is warmed, " << static_cast<uint64_t>(read);
    }
    warmed.materialKeywords = MaterialKeyword::Parallax | MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus |
                              MaterialKeyword::ParallaxDepthOffset | MaterialKeyword::ParallaxDepthTolerance;
    EXPECT_NE(std::find(keys.begin(), keys.end(), warmed), keys.end())
        << "the tolerant colour pass, drawn in every view with forward contributors, is warmed";
}

// The world pass's choice of relief-depth keywords for one colour draw, and the depth write each
// pipeline gets: after a prepass whose depth it attaches read-only the colour pass reads that depth and
// writes none; with the depth attached writable (forward contributors that draw no prepass depth) it
// marches and tests with a tolerance, writing none; without a prepass it marches and writes its own.
// Blend, glass and materials without Parallax get none.
TEST(MaterialPrewarmVariantsTest, TheColourPassReadsThePrepassReliefDepthWhereItCan)
{
    const MaterialKeyword offset = MaterialKeyword::ParallaxDepthOffset;
    const MaterialKeyword fromPrepass = MaterialKeyword::ParallaxDepthFromPrepass;
    const MaterialKeyword multisample = MaterialKeyword::ParallaxPrepassDepthMultisample;
    EXPECT_EQ(WorldPassReliefDepthSource(false, true, 4u), ReliefDepthSource::NoPrepass);
    EXPECT_EQ(WorldPassReliefDepthSource(true, false, 4u), ReliefDepthSource::PrepassUnreadable);
    EXPECT_EQ(WorldPassReliefDepthSource(true, true, 1u), ReliefDepthSource::PrepassReadable);
    EXPECT_EQ(WorldPassReliefDepthSource(true, true, 4u), ReliefDepthSource::PrepassReadableMultisample);

    const MaterialKeyword parallax = MaterialKeyword::Parallax;
    EXPECT_EQ(WorldPassReliefDepthKeywords(parallax, MaterialAlphaMode::Opaque, ReliefDepthSource::PrepassReadable),
              fromPrepass);
    EXPECT_EQ(WorldPassReliefDepthKeywords(parallax, MaterialAlphaMode::Opaque,
                                           ReliefDepthSource::PrepassReadableMultisample),
              fromPrepass | multisample);
    EXPECT_EQ(WorldPassReliefDepthKeywords(parallax, MaterialAlphaMode::Opaque, ReliefDepthSource::NoPrepass), offset);
    EXPECT_EQ(WorldPassReliefDepthKeywords(parallax, MaterialAlphaMode::Opaque, ReliefDepthSource::PrepassUnreadable),
              offset | MaterialKeyword::ParallaxDepthTolerance);
    EXPECT_EQ(WorldPassReliefDepthKeywords(parallax | MaterialKeyword::AlphaTest, MaterialAlphaMode::Mask,
                                           ReliefDepthSource::PrepassReadableMultisample),
              fromPrepass | multisample)
        << "a Mask parallax material reads the prepass's depth too";
    EXPECT_EQ(WorldPassReliefDepthKeywords(parallax, MaterialAlphaMode::Blend, ReliefDepthSource::PrepassReadable),
              MaterialKeyword::None);
    EXPECT_EQ(WorldPassReliefDepthKeywords(parallax | MaterialKeyword::Transmission, MaterialAlphaMode::Opaque,
                                           ReliefDepthSource::NoPrepass),
              MaterialKeyword::None);
    EXPECT_EQ(WorldPassReliefDepthKeywords(MaterialKeyword::None, MaterialAlphaMode::Opaque, ReliefDepthSource::NoPrepass),
              MaterialKeyword::None);

    const MaterialKeyword pass = MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus;
    EXPECT_FALSE(ColorVariantWritesDepth(true, pass | fromPrepass));
    EXPECT_FALSE(ColorVariantWritesDepth(true, pass | fromPrepass | multisample));
    EXPECT_TRUE(ColorVariantWritesDepth(true, pass | offset));
    EXPECT_FALSE(ColorVariantWritesDepth(true, pass | offset | MaterialKeyword::ParallaxDepthTolerance));
    EXPECT_TRUE(ColorVariantWritesDepth(true, pass));
    EXPECT_FALSE(ColorVariantWritesDepth(false, pass | offset)) << "an authored zWrite off stays off";
}

// The prepass head that writes the relief's depth keeps the colour pass's tangent stream, which the depth layout
// strips: it marches from the colour pass's tangent frame. Every other depth head keeps the stripped layout.
TEST(MaterialPrewarmVariantsTest, TheReliefPrepassHeadKeepsTheTangentStream)
{
    constexpr VertexAttributeFlags stripped =
        VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal | VertexAttributeFlags::HasUV0;
    constexpr VertexAttributeFlags colorPass = stripped | VertexAttributeFlags::HasTangent;
    const MaterialKeyword reliefHead =
        MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment | MaterialKeyword::ParallaxDepthOffset;
    EXPECT_EQ(DepthHeadVertexFlags(stripped, colorPass, false, false, reliefHead), colorPass);
    EXPECT_EQ(DepthHeadVertexFlags(stripped, stripped, false, false, reliefHead), stripped)
        << "the colour pass has no tangent: none on the mesh, or none bound";
    EXPECT_EQ(DepthHeadVertexFlags(stripped, colorPass, false, false,
                                   MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment),
              stripped)
        << "a cutout head";
    const DepthSegmentPipelineChoice head =
        ChooseDepthHeadPipeline(MaterialKeyword::Instanced, DepthPassType::Prepass, false, false, true, true);
    EXPECT_EQ(DepthHeadVertexFlags(stripped, colorPass, false, false, head.Keywords), colorPass);
    const DepthSegmentPipelineChoice cascade =
        ChooseDepthHeadPipeline(MaterialKeyword::Instanced, DepthPassType::ShadowCascade, false, false, true, true);
    EXPECT_EQ(DepthHeadVertexFlags(stripped, colorPass, false, false, cascade.Keywords), stripped)
        << "the flat polygon casts";
}

// A vertex-modified material's modifier runs in every depth pass and may read any stream the colour pass binds
// (the tree package's wind reads its per-leaf seed from UV1), so each of its depth heads, the prepass, the
// shadow cascades and the deforming-motion pass, draws with the colour pass's layout. Without a modifier the
// heads keep the stripped layout, plus the colour stream for a Mask head, whose coverage reads vertex alpha.
TEST(MaterialPrewarmVariantsTest, AVertexModifiedMaterialsDepthHeadsReadTheColorPassStreams)
{
    constexpr VertexAttributeFlags stripped =
        VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal | VertexAttributeFlags::HasUV0;
    constexpr VertexAttributeFlags colorPass =
        stripped | VertexAttributeFlags::HasTangent | VertexAttributeFlags::HasColor | VertexAttributeFlags::HasUV1;
    for (const DepthPassType pass : {DepthPassType::Prepass, DepthPassType::ShadowCascade,
                                     DepthPassType::SpotShadow, DepthPassType::DeformationMotion})
    {
        for (const bool alphaTest : {false, true})
        {
            const bool readsVertexColor =
                DepthPassReadsVertexColor(pass, alphaTest ? MaterialAlphaMode::Mask : MaterialAlphaMode::Opaque);
            EXPECT_EQ(readsVertexColor, alphaTest) << "pass " << static_cast<int>(pass);
            const DepthSegmentPipelineChoice head =
                ChooseDepthHeadPipeline(MaterialKeyword::Instanced, pass, alphaTest, true, true, false);
            EXPECT_EQ(DepthHeadVertexFlags(stripped, colorPass, true, readsVertexColor, head.Keywords), colorPass)
                << "pass " << static_cast<int>(pass) << ", alpha test " << alphaTest;
            const DepthSegmentPipelineChoice plain =
                ChooseDepthHeadPipeline(MaterialKeyword::Instanced, pass, alphaTest, false, true, false);
            const VertexAttributeFlags plainLayout =
                alphaTest ? stripped | VertexAttributeFlags::HasColor : stripped;
            EXPECT_EQ(DepthHeadVertexFlags(stripped, colorPass, false, readsVertexColor, plain.Keywords), plainLayout)
                << "pass " << static_cast<int>(pass) << ", alpha test " << alphaTest;
        }
    }
}

// A depth pipeline with no fragment stage declares the descriptor sets up to the highest one its VERTEX stage
// reads (DepthOnlyPipelineSetCount), named by the ShaderMeta stage mask (ShaderMetaStage). A set only the fragment
// stage reads does not count, a vertex-stage binding in set 2 (a geometry fetch, as the CBT and grass heads do)
// makes three, and a meta whose vertex stage reads nothing makes none.
TEST(MaterialPrewarmVariantsTest, ADepthOnlyPipelineDeclaresTheSetsItsVertexStageReads)
{
    const auto binding = [](uint32_t stages)
    {
        DescriptorBindingMeta b{};
        b.StagesMask = stages;
        return b;
    };
    const auto set = [](uint32_t index, std::vector<DescriptorBindingMeta> bindings)
    {
        DescriptorSetMeta s{};
        s.Set = index;
        s.Bindings = std::move(bindings);
        return s;
    };
    ShaderMeta meta{};
    meta.Sets = {set(0, {binding(ShaderMetaStage::kVertex | ShaderMetaStage::kFragment)}),
                 set(1, {binding(ShaderMetaStage::kFragment)}),
                 set(3, {binding(ShaderMetaStage::kFragment)})};
    EXPECT_EQ(DepthOnlyPipelineSetCount(meta), 1u) << "sets only the fragment stage reads do not count";
    meta.Sets.push_back(set(2, {binding(ShaderMetaStage::kFragment), binding(ShaderMetaStage::kVertex)}));
    EXPECT_EQ(DepthOnlyPipelineSetCount(meta), 3u) << "a vertex-stage binding in set 2 declares sets 0 to 2";
    ShaderMeta fragmentOnly{};
    fragmentOnly.Sets = {set(0, {binding(ShaderMetaStage::kFragment)})};
    EXPECT_EQ(DepthOnlyPipelineSetCount(fragmentOnly), 0u) << "a vertex stage that reads no set declares none";
}

// The stage mask DepthOnlyPipelineSetCount reads is the one SPIR-V reflection writes: a compiled program whose
// vertex stage reads a set-2 buffer and whose fragment stage alone reads set 3 declares sets 0 to 2. The test
// above builds its meta by hand from ShaderMetaStage, so it cannot see a reflection writer that encodes the
// stages with other bits. The cache root is fresh, so the meta is this build's reflection, not a cached one.
TEST(MaterialPrewarmVariantsTest, AReflectedVertexStageBindingDeclaresItsSetForADepthOnlyPipeline)
{
    if (!ShaderCompileService::IsCompilerAvailable())
        GTEST_SKIP() << "no shader compiler in this build";
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("ge_depth_set_count_" + GUID::Generate().ToString());
    ShaderProgramCompileRequest req{};
    req.debugName = "DepthOnlySetCountProbe";
    req.baseDirectory = root;
    req.cacheRoot = root / ".Cache" / "Shaders";
    ShaderStageCompileSpec vs{};
    vs.stage = "vs";
    vs.sourcePath = root / "depth_set_count_probe.vert";
    vs.inlineSource = R"(#version 450
layout(set = 2, binding = 0) readonly buffer Positions { vec4 p[]; } gPositions;
void main() { gl_Position = gPositions.p[gl_VertexIndex]; }
)";
    ShaderStageCompileSpec fs{};
    fs.stage = "fs";
    fs.sourcePath = root / "depth_set_count_probe.frag";
    fs.inlineSource = R"(#version 450
layout(set = 3, binding = 0) uniform Tint { vec4 c; } gTint;
layout(location = 0) out vec4 oColor;
void main() { oColor = gTint.c; }
)";
    req.stages = {vs, fs};
    ShaderProgramCompileResult result{};
    std::string error;
    const bool compiled = ShaderCompileService::CompileProgramToCache(req, ShaderSourceKind::SpirV, result, &error);
    EXPECT_TRUE(compiled) << error;
    if (compiled)
    {
        EXPECT_EQ(DepthOnlyPipelineSetCount(result.meta), 3u)
            << "the reflected vertex-stage binding in set 2 declares sets 0 to 2; the fragment stage's set 3 does not count";
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// The prewarm set warms a vertex-modified material's depth heads on the layouts the recorder draws them with
// (DepthHeadVertexFlags): its own layout, here carrying UV1, and every colour layout it warms, the tangent one
// included, in the prepass and the shadow cascades.
TEST(MaterialPrewarmVariantsTest, AVertexModifiedMaterialWarmsTheDepthHeadsItDraws)
{
    ShaderVariantKey base{};
    base.vertexFlags = VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal |
                       VertexAttributeFlags::HasUV0 | VertexAttributeFlags::HasUV1;
    base.materialKeywords = MaterialKeyword::HasVertexMod | MaterialKeyword::AlphaTest;
    const MaterialKeyword color = MaterialKeyword::ForwardPlus | MaterialKeyword::IBL | MaterialKeyword::Instanced;
    const auto keys = MaterialPrewarmVariantKeys(base, color, false, MaterialAlphaMode::Mask);
    const VertexAttributeFlags tangentLayout =
        VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal | VertexAttributeFlags::HasUV0 |
        VertexAttributeFlags::HasTangent;
    for (const DepthPassType pass : {DepthPassType::Prepass, DepthPassType::ShadowCascade})
    {
        const DepthSegmentPipelineChoice head = ChooseDepthHeadPipeline(
            MaterialKeyword::Instanced, pass, true, true, pass != DepthPassType::Prepass || PrepassSharedDepthEnabled(),
            false);
        for (const VertexAttributeFlags colorLayout : {base.vertexFlags, tangentLayout})
        {
            ShaderVariantKey drawn = base;
            drawn.vertexFlags = DepthHeadVertexFlags(StrippedDepthVertexFlags(colorLayout), colorLayout, true,
                                                     DepthPassReadsVertexColor(pass, MaterialAlphaMode::Mask),
                                                     head.Keywords);
            drawn.materialKeywords |= head.Keywords;
            EXPECT_EQ(drawn.vertexFlags, colorLayout);
            EXPECT_NE(std::find(keys.begin(), keys.end(), drawn), keys.end())
                << "pass " << static_cast<int>(pass) << ", layout " << static_cast<uint32_t>(colorLayout);
        }
    }
}

// A Mask head's coverage discards on the opacity the colour pass discards on, vertex alpha included
// (DepthPassReadsVertexColor), so it keeps the colour pass's colour stream on top of the stripped layout, and
// nothing else of it. The colour pass's layout has no colour stream when the mesh has none bound or the
// material ignores vertex colour, and the head then reads none. The prewarm set passes the same rule: a Mask
// material's coverage heads are warmed on the stripped colour layouts it warms (and, for its vertex-colour
// heads, AVertexColourMaskWarmsTheDepthHeadsItDrawsBesideAColourStream).
TEST(MaterialPrewarmVariantsTest, AMaskHeadReadsTheColorStreamItsColourPassReads)
{
    constexpr VertexAttributeFlags stripped =
        VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal | VertexAttributeFlags::HasUV0;
    constexpr VertexAttributeFlags colorPass =
        stripped | VertexAttributeFlags::HasTangent | VertexAttributeFlags::HasColor | VertexAttributeFlags::HasUV1;
    constexpr VertexAttributeFlags colorPassWithoutColor = colorPass & ~VertexAttributeFlags::HasColor;
    for (const DepthPassType pass : {DepthPassType::Prepass, DepthPassType::ShadowCascade, DepthPassType::SpotShadow,
                                     DepthPassType::PointShadow, DepthPassType::DeformationMotion})
    {
        const bool reads = DepthPassReadsVertexColor(pass, MaterialAlphaMode::Mask);
        ASSERT_TRUE(reads) << "pass " << static_cast<int>(pass);
        const DepthSegmentPipelineChoice head =
            ChooseDepthHeadPipeline(MaterialKeyword::Instanced, pass, true, false, true, false);
        EXPECT_EQ(DepthHeadVertexFlags(stripped, colorPass, false, reads, head.Keywords),
                  stripped | VertexAttributeFlags::HasColor)
            << "pass " << static_cast<int>(pass);
        EXPECT_EQ(DepthHeadVertexFlags(stripped, colorPassWithoutColor, false, reads, head.Keywords), stripped)
            << "no colour stream in the colour pass (none bound, or the material ignores vertex colour), pass "
            << static_cast<int>(pass);
    }
    // The relief prepass head of a Mask material keeps both streams it reads.
    const DepthSegmentPipelineChoice relief =
        ChooseDepthHeadPipeline(MaterialKeyword::Instanced, DepthPassType::Prepass, true, false, true, true);
    EXPECT_EQ(DepthHeadVertexFlags(stripped, colorPass, false, true, relief.Keywords),
              stripped | VertexAttributeFlags::HasColor | VertexAttributeFlags::HasTangent);

    ShaderVariantKey base{};
    base.vertexFlags = VertexAttributeFlags::StandardMesh;
    base.materialKeywords = MaterialKeyword::AlphaTest;
    const MaterialKeyword color = MaterialKeyword::ForwardPlus | MaterialKeyword::IBL | MaterialKeyword::Instanced;
    const auto keys = MaterialPrewarmVariantKeys(base, color, false, MaterialAlphaMode::Mask);
    constexpr VertexAttributeFlags standard = stripped;
    constexpr VertexAttributeFlags warmedLayouts[] = {
        standard, standard | VertexAttributeFlags::HasTangent, standard | VertexAttributeFlags::Skinned,
        standard | VertexAttributeFlags::HasTangent | VertexAttributeFlags::Skinned};
    for (const DepthPassType pass : {DepthPassType::Prepass, DepthPassType::ShadowCascade})
    {
        const DepthSegmentPipelineChoice head = ChooseDepthHeadPipeline(
            MaterialKeyword::Instanced, pass, true, false, pass != DepthPassType::Prepass || PrepassSharedDepthEnabled(),
            false);
        for (const VertexAttributeFlags colorLayout : warmedLayouts)
        {
            ShaderVariantKey drawn = base;
            drawn.vertexFlags = DepthHeadVertexFlags(StrippedDepthVertexFlags(colorLayout), colorLayout, false,
                                                     DepthPassReadsVertexColor(pass, MaterialAlphaMode::Mask),
                                                     head.Keywords);
            drawn.materialKeywords |= head.Keywords;
            EXPECT_NE(std::find(keys.begin(), keys.end(), drawn), keys.end())
                << "pass " << static_cast<int>(pass) << ", layout " << static_cast<uint32_t>(colorLayout);
        }
    }
}

// A Mask material that reads vertex colour draws its coverage heads with the colour stream beside a colour layout
// that has one (DepthHeadVertexFlags), so the prewarm set warms those heads, and the prepass's crossfade tails, on
// every colour layout it warms with HasColor added: a mesh with a colour stream then finds its prepass and shadow
// heads compiled, as a mesh without one does. Only those heads are added. The colour pass is not warmed on a
// vertex-colour layout; a material that ignores vertex colour never binds the stream and gets none; an opaque
// material's depth draws keep the stripped layout; and a vertex-modified material's heads draw with the colour
// layout itself, so its keys do not change.
TEST(MaterialPrewarmVariantsTest, AVertexColourMaskWarmsTheDepthHeadsItDrawsBesideAColourStream)
{
    constexpr VertexAttributeFlags standard =
        VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal | VertexAttributeFlags::HasUV0;
    constexpr VertexAttributeFlags warmedLayouts[] = {
        standard, standard | VertexAttributeFlags::HasTangent, standard | VertexAttributeFlags::Skinned,
        standard | VertexAttributeFlags::HasTangent | VertexAttributeFlags::Skinned};
    const MaterialKeyword color = MaterialKeyword::ForwardPlus | MaterialKeyword::IBL | MaterialKeyword::Instanced;
    const auto hasColor = [](const ShaderVariantKey& key)
    { return HasFlag(key.vertexFlags, VertexAttributeFlags::HasColor); };

    ShaderVariantKey base{};
    base.vertexFlags = VertexAttributeFlags::StandardMesh;
    base.materialKeywords = MaterialKeyword::AlphaTest;
    const auto keys = MaterialPrewarmVariantKeys(base, color, false, MaterialAlphaMode::Mask);
    Vector<ShaderVariantKey> expected;
    for (const DepthPassType pass : {DepthPassType::Prepass, DepthPassType::ShadowCascade})
    {
        const DepthSegmentPipelineChoice head = ChooseDepthHeadPipeline(
            MaterialKeyword::Instanced, pass, true, false, pass != DepthPassType::Prepass || PrepassSharedDepthEnabled(),
            false);
        ASSERT_FALSE(head.UseSharedDepth) << "a Mask head runs its coverage fragment, pass " << static_cast<int>(pass);
        for (const VertexAttributeFlags warmed : warmedLayouts)
        {
            const VertexAttributeFlags colorLayout = warmed | VertexAttributeFlags::HasColor;
            ShaderVariantKey drawn = base;
            drawn.vertexFlags = DepthHeadVertexFlags(StrippedDepthVertexFlags(colorLayout), colorLayout, false,
                                                     DepthPassReadsVertexColor(pass, MaterialAlphaMode::Mask),
                                                     head.Keywords);
            ASSERT_TRUE(HasFlag(drawn.vertexFlags, VertexAttributeFlags::HasColor));
            drawn.materialKeywords |= head.Keywords;
            EXPECT_NE(std::find(keys.begin(), keys.end(), drawn), keys.end())
                << "head, pass " << static_cast<int>(pass) << ", layout " << static_cast<uint32_t>(colorLayout);
            expected.push_back(drawn);
            if (!DepthPassDrawsCrossfadeTails(pass))
                continue;
            ShaderVariantKey tail = base;
            tail.vertexFlags = drawn.vertexFlags;
            tail.materialKeywords |=
                ChooseDepthSegmentPipeline(head.Keywords, head.UseSharedDepth, head.ComposesFragment, true).Keywords;
            EXPECT_NE(std::find(keys.begin(), keys.end(), tail), keys.end())
                << "tail, pass " << static_cast<int>(pass) << ", layout " << static_cast<uint32_t>(colorLayout);
            expected.push_back(tail);
        }
    }
    for (const ShaderVariantKey& key : keys)
    {
        if (!hasColor(key))
            continue;
        EXPECT_TRUE(HasKeyword(key.materialKeywords, MaterialKeyword::DepthOnlyFragment))
            << "a colour-pass key on a vertex-colour layout, flags " << static_cast<uint32_t>(key.vertexFlags);
        EXPECT_NE(std::find(expected.begin(), expected.end(), key), expected.end())
            << "a vertex-colour key the recorder does not draw, flags " << static_cast<uint32_t>(key.vertexFlags);
    }

    // Ignoring vertex colour: the same set without the vertex-colour heads.
    const auto ignoring = MaterialPrewarmVariantKeys(base, color, false, MaterialAlphaMode::Mask, true);
    EXPECT_EQ(std::count_if(ignoring.begin(), ignoring.end(), hasColor), 0);
    Vector<ShaderVariantKey> withoutColorHeads;
    std::copy_if(keys.begin(), keys.end(), std::back_inserter(withoutColorHeads),
                 [&](const ShaderVariantKey& key) { return !hasColor(key); });
    EXPECT_EQ(withoutColorHeads, ignoring);

    // Opaque: the depth draws decide coverage without the surface.
    ShaderVariantKey opaque = base;
    opaque.materialKeywords = MaterialKeyword::None;
    const auto opaqueKeys = MaterialPrewarmVariantKeys(opaque, color, false, MaterialAlphaMode::Opaque);
    EXPECT_EQ(std::count_if(opaqueKeys.begin(), opaqueKeys.end(), hasColor), 0);

    // Vertex-modified: reading or ignoring vertex colour warms the same set.
    ShaderVariantKey modified = base;
    modified.materialKeywords |= MaterialKeyword::HasVertexMod;
    EXPECT_EQ(MaterialPrewarmVariantKeys(modified, color, false, MaterialAlphaMode::Mask),
              MaterialPrewarmVariantKeys(modified, color, false, MaterialAlphaMode::Mask, true));
}

TEST(MaterialPrewarmVariantsTest, ProceduralVariantsAreClampedAndDeduplicated)
{
    ShaderVariantKey base{};
    base.userKeywordHash = 42;
    const auto keys = MaterialPrewarmVariantKeys(base, MaterialKeyword::ForwardPlus | MaterialKeyword::Instanced,
                                                 true, MaterialAlphaMode::Opaque);
    ASSERT_EQ(keys.size(), 3u);
    for (const auto& key : keys)
    {
        EXPECT_EQ(key.vertexFlags, VertexAttributeFlags::None);
        EXPECT_EQ(key.userKeywordHash, 42u);
    }
}

static MaterialDocument MakeTestPBRDocument(const std::string& name = "TestPBR")
{
    MaterialDocument doc{};
    doc.schemaVersion = 2;
    doc.materialName = name;
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "surfaces/standard_surface.glsl";
    doc.properties["baseColor"] = std::vector<float>{0.8f, 0.2f, 0.1f, 1.0f};
    doc.properties["metallic"] = 0.0f;
    doc.properties["roughness"] = 0.5f;
    return doc;
}

class MaterialRegistryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_device = CreateVulkanDeviceFast();
        if (!m_device)
            GTEST_SKIP() << "No Vulkan device available";
        m_registry.Initialize(m_device.get());
        // Register expects SeedTextureSlotDefaults to have been called; outside
        // RenderServices' wiring we supply opaque sentinel indices so the
        // post-construction assertion in Register passes. Tests that care
        // about specific slot values exercise Material directly.
        m_registry.SeedTextureSlotDefaults(/*white*/1u, /*flatNormal*/2u, /*black*/3u);
    }

    void TearDown() override
    {
        m_registry.Shutdown();
        if (m_device)
            m_device->Shutdown();
    }

    std::unique_ptr<IDevice> m_device;
    MaterialRegistry m_registry;
};

TEST_F(MaterialRegistryTest, Register_ReturnsValidMaterial)
{
    GUID guid = GUID::Generate();
    Material* mat = m_registry.Register(guid, MakeTestPBRDocument());

    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetGuid(), guid);
    EXPECT_EQ(mat->GetName(), "TestPBR");
    EXPECT_EQ(mat->GetLightingModel(), LightingModel::kStandardPBR);
}

TEST_F(MaterialRegistryTest, Register_DefaultsIgnoreVertexColorFalse)
{
    Material* mat = m_registry.Register(GUID::Generate(), MakeTestPBRDocument());
    ASSERT_NE(mat, nullptr);
    EXPECT_FALSE(mat->IgnoresVertexColor());
}

TEST_F(MaterialRegistryTest, Register_WiresIgnoreVertexColorFromDocument)
{
    MaterialDocument doc = MakeTestPBRDocument("IgnoreVC");
    doc.ignoreVertexColor = true;
    Material* mat = m_registry.Register(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_TRUE(mat->IgnoresVertexColor());

    // Hot-reload path: re-registering the same GUID with the flag cleared must
    // update the live Material (Register routes both cases through ApplyDocumentToMaterial).
    doc.ignoreVertexColor = false;
    Material* reloaded = m_registry.Register(mat->GetGuid(), doc);
    EXPECT_EQ(reloaded, mat);
    EXPECT_FALSE(mat->IgnoresVertexColor());
}

TEST_F(MaterialRegistryTest, Register_Deduplicates)
{
    GUID guid = GUID::Generate();
    Material* mat1 = m_registry.Register(guid, MakeTestPBRDocument());
    Material* mat2 = m_registry.Register(guid, MakeTestPBRDocument("Different"));

    EXPECT_EQ(mat1, mat2) << "Same GUID must return same Material pointer";
    EXPECT_EQ(m_registry.GetMaterialCount(), 1u);
}

TEST_F(MaterialRegistryTest, Find_ReturnsRegisteredMaterial)
{
    GUID guid = GUID::Generate();
    Material* registered = m_registry.Register(guid, MakeTestPBRDocument());

    Material* found = m_registry.Find(guid);
    EXPECT_EQ(found, registered);
}

TEST_F(MaterialRegistryTest, Find_UnregisteredReturnsNull)
{
    GUID guid = GUID::Generate();
    EXPECT_EQ(m_registry.Find(guid), nullptr);
}

TEST_F(MaterialRegistryTest, Register_PopulatesPropertiesFromDocument)
{
    GUID guid = GUID::Generate();
    Material* mat = m_registry.Register(guid, MakeTestPBRDocument());
    ASSERT_NE(mat, nullptr);

    // baseColor should be populated from the document.
    float bc[4] = {0};
    EXPECT_TRUE(mat->GetVector("baseColor"_sid, bc, 4));
    EXPECT_NEAR(bc[0], 0.8f, 1e-5f);
    EXPECT_NEAR(bc[1], 0.2f, 1e-5f);
    EXPECT_NEAR(bc[2], 0.1f, 1e-5f);
    EXPECT_NEAR(bc[3], 1.0f, 1e-5f);

    EXPECT_NEAR(mat->GetFloat("metallic"_sid), 0.0f, 1e-5f);
    EXPECT_NEAR(mat->GetFloat("roughness"_sid), 0.5f, 1e-5f);
}

TEST_F(MaterialRegistryTest, Register_InitializesParamCache)
{
    GUID guid = GUID::Generate();
    Material* mat = m_registry.Register(guid, MakeTestPBRDocument());
    ASSERT_NE(mat, nullptr);

    // Material params live in the CPU cache (packed into the shared
    // MaterialParams SSBO by RenderServices — there is no per-material GPU
    // buffer). Registration allocates a 16-byte-aligned cache and writes the
    // document's values into it.
    EXPECT_GT(mat->GetCacheSize(), 0u);
    EXPECT_EQ(mat->GetCacheSize() % 16u, 0u);
    float bc[4] = {0};
    ASSERT_TRUE(mat->GetVector("baseColor"_sid, bc, 4));
    EXPECT_NEAR(bc[0], 0.8f, 1e-5f);
}

TEST_F(MaterialRegistryTest, PropertyChange_UpdatesParamCache)
{
    GUID guid = GUID::Generate();
    Material* mat = m_registry.Register(guid, MakeTestPBRDocument());
    ASSERT_NE(mat, nullptr);

    // A property change lands in the CPU cache that PackMaterialSSBO packs into
    // the shared SSBO next frame — there is no per-material GPU upload.
    mat->SetFloat("roughness"_sid, 0.9f);
    EXPECT_NEAR(mat->GetFloat("roughness"_sid), 0.9f, 1e-5f);
}

// A document re-registration that changes only a property value is the
// inspector-edit shape: it must move the per-material content revision and
// leave the compile version alone (no shader recompiles for a value edit).
TEST_F(MaterialRegistryTest, Register_ExistingGuid_PropertyEditMovesContentRevisionNotVersion)
{
    GUID guid = GUID::Generate();
    MaterialDocument doc = MakeTestPBRDocument();
    Material* mat = m_registry.Register(guid, doc);
    ASSERT_NE(mat, nullptr);
    const uint32_t revisionBefore = mat->GetContentRevision();
    const uint32_t versionBefore = mat->GetVersion();

    doc.properties["roughness"] = 0.9f;
    ASSERT_EQ(m_registry.Register(guid, doc), mat);

    EXPECT_GT(mat->GetContentRevision(), revisionBefore);
    EXPECT_EQ(mat->GetVersion(), versionBefore);
}

// A document that authors no emissive exposure weight keeps its emission physical (weight 1), at
// registration and when a reload drops an authored weight: a zero lane would show the emission
// whatever the view's exposure.
TEST_F(MaterialRegistryTest, Register_UnauthoredEmissiveExposureWeightIsPhysical)
{
    GUID guid = GUID::Generate();
    MaterialDocument doc = MakeTestPBRDocument();
    Material* mat = m_registry.Register(guid, doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetFloat("emissiveExposureWeight"_sid), kDefaultEmissiveExposureWeight);

    doc.properties["emissiveExposureWeight"] = 0.0f;
    ASSERT_EQ(m_registry.Register(guid, doc), mat);
    EXPECT_EQ(mat->GetFloat("emissiveExposureWeight"_sid), 0.0f);

    doc.properties.erase("emissiveExposureWeight");
    ASSERT_EQ(m_registry.Register(guid, doc), mat);
    EXPECT_EQ(mat->GetFloat("emissiveExposureWeight"_sid), kDefaultEmissiveExposureWeight);
}

TEST_F(MaterialRegistryTest, Unregister_RemovesMaterial)
{
    GUID guid = GUID::Generate();
    m_registry.Register(guid, MakeTestPBRDocument());
    EXPECT_EQ(m_registry.GetMaterialCount(), 1u);

    m_registry.Unregister(guid);
    EXPECT_EQ(m_registry.GetMaterialCount(), 0u);
    EXPECT_EQ(m_registry.Find(guid), nullptr);
}

TEST_F(MaterialRegistryTest, Register_UnlitLightingModel)
{
    MaterialDocument doc = MakeTestPBRDocument();
    doc.lightingModel = "Unlit";

    GUID guid = GUID::Generate();
    Material* mat = m_registry.Register(guid, doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetLightingModel(), LightingModel::kUnlit);
}

TEST_F(MaterialRegistryTest, Register_WithDocumentRenderState)
{
    MaterialDocument doc = MakeTestPBRDocument();
    doc.alphaMode = MaterialAlphaMode::Mask;
    doc.doubleSided = true;

    GUID guid = GUID::Generate();
    Material* mat = m_registry.Register(guid, doc);
    ASSERT_NE(mat, nullptr);

    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Mask);
    EXPECT_TRUE(mat->IsDoubleSided());
}

TEST_F(MaterialRegistryTest, ForEach_IteratesAll)
{
    for (int i = 0; i < 5; ++i)
    {
        GUID guid = GUID::Generate();
        m_registry.Register(guid, MakeTestPBRDocument("Mat" + std::to_string(i)));
    }

    uint32_t count = 0;
    m_registry.ForEach([&](const GUID&, const Material&) { ++count; });
    EXPECT_EQ(count, 5u);
}

// Integration: RenderServices owns the registry.
TEST_F(MaterialRegistryTest, RenderServicesOwnsRegistry)
{
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(m_device.get()));

    auto& reg = rs.Materials().Registry();
    EXPECT_EQ(reg.GetMaterialCount(), 0u);

    GUID guid = GUID::Generate();
    Material* mat = reg.Register(guid, MakeTestPBRDocument());
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(reg.GetMaterialCount(), 1u);

    rs.Shutdown();
}

// B1 regression: re-registering an existing GUID must propagate new property
// values from the document. Previously the second Register short-circuited,
// silently dropping any edits to .material files on hot-reload.
TEST_F(MaterialRegistryTest, Register_ExistingGuid_RefreshesPropertiesFromDocument)
{
    GUID guid = GUID::Generate();

    MaterialDocument first = MakeTestPBRDocument();
    Material* matFirst = m_registry.Register(guid, first);
    ASSERT_NE(matFirst, nullptr);

    // Sanity: the first registration populated the document values.
    {
        float bc[4] = {0};
        ASSERT_TRUE(matFirst->GetVector("baseColor"_sid, bc, 4));
        EXPECT_NEAR(bc[0], 0.8f, 1e-5f);
        EXPECT_NEAR(matFirst->GetFloat("roughness"_sid), 0.5f, 1e-5f);
    }

    // Edit the document (simulating a `.material` save) and re-register the
    // same GUID. Hot-reload preserves the GUID, so this is the path the
    // editor / asset manager actually takes.
    MaterialDocument edited = first;
    edited.properties["baseColor"] = std::vector<float>{0.1f, 0.2f, 0.3f, 0.7f};
    edited.properties["metallic"] = 0.9f;
    edited.properties["roughness"] = 0.25f;
    edited.alphaMode = MaterialAlphaMode::Mask;
    edited.doubleSided = true;

    Material* matSecond = m_registry.Register(guid, edited);
    EXPECT_EQ(matFirst, matSecond) << "GUID-keyed dedup must keep the same pointer across reload";
    EXPECT_EQ(m_registry.GetMaterialCount(), 1u);

    float bc2[4] = {0};
    ASSERT_TRUE(matSecond->GetVector("baseColor"_sid, bc2, 4));
    EXPECT_NEAR(bc2[0], 0.1f, 1e-5f);
    EXPECT_NEAR(bc2[1], 0.2f, 1e-5f);
    EXPECT_NEAR(bc2[2], 0.3f, 1e-5f);
    EXPECT_NEAR(bc2[3], 0.7f, 1e-5f);
    EXPECT_NEAR(matSecond->GetFloat("metallic"_sid), 0.9f, 1e-5f);
    EXPECT_NEAR(matSecond->GetFloat("roughness"_sid), 0.25f, 1e-5f);
    EXPECT_EQ(matSecond->GetAlphaMode(), MaterialAlphaMode::Mask);
    EXPECT_TRUE(matSecond->IsDoubleSided());
}

// B1 regression: removing a property from the document must revert the slot
// to its default value, not retain the previous override.
TEST_F(MaterialRegistryTest, Register_ExistingGuid_RevertsRemovedPropertiesToDefaults)
{
    GUID guid = GUID::Generate();
    Material* mat = m_registry.Register(guid, MakeTestPBRDocument());
    ASSERT_NE(mat, nullptr);

    MaterialDocument stripped{};
    stripped.schemaVersion = 2;
    stripped.materialName = "TestPBR";
    stripped.lightingModel = "StandardPBR";
    stripped.surfaceShader = "surfaces/standard_surface.glsl";
    // intentionally no properties — should snap back to the registry's defaults.

    Material* same = m_registry.Register(guid, stripped);
    ASSERT_EQ(same, mat);

    float bc[4] = {0};
    ASSERT_TRUE(same->GetVector("baseColor"_sid, bc, 4));
    EXPECT_NEAR(bc[0], 1.0f, 1e-5f);
    EXPECT_NEAR(bc[1], 1.0f, 1e-5f);
    EXPECT_NEAR(bc[2], 1.0f, 1e-5f);
    EXPECT_NEAR(bc[3], 1.0f, 1e-5f);
    EXPECT_NEAR(same->GetFloat("metallic"_sid), 0.0f, 1e-5f);
    EXPECT_NEAR(same->GetFloat("roughness"_sid), 0.5f, 1e-5f);
}

// The triplanar lanes are wired by NAME in Register()'s property table; the
// aliasing test in MaterialTests.cpp builds its own layouts via TestFactory and
// would not catch a typo'd name->lane mapping in Register() itself. Drive the
// real Register() with a triplanar document and verify each named property
// lands at its exact MaterialGpuParams byte offset in the CPU cache (the block
// PackMaterialSSBO uploads verbatim, so a wrong offset here is a wrong lane on
// the GPU).
TEST_F(MaterialRegistryTest, Register_TriplanarDocument_WiresNamedLanesToGpuOffsets)
{
    const uint32_t tBase = MaterialParamLaneOffset(23);
    const uint32_t mBase = MaterialParamLaneOffset(24);
    const uint32_t rBase = MaterialParamLaneOffset(25);

    struct Lane { const char* name; uint32_t offset; float value; };
    const Lane lanes[] = {
        {"triplanarTilingTop",       tBase + 0,  0.11f},
        {"triplanarTilingSide",      tBase + 4,  0.22f},
        {"triplanarTilingBottom",    tBase + 8,  0.33f},
        {"triplanarBlendSharpness",  tBase + 12, 0.44f},
        {"triplanarMetallicTop",     mBase + 0,  0.55f},
        {"triplanarMetallicSide",    mBase + 4,  0.66f},
        {"triplanarMetallicBottom",  mBase + 8,  0.77f},
        {"triplanarLayered",         mBase + 12, 0.88f},
        {"triplanarRoughnessTop",    rBase + 0,  0.12f},
        {"triplanarRoughnessSide",   rBase + 4,  0.23f},
        {"triplanarRoughnessBottom", rBase + 8,  0.34f},
        {"triplanarNormalLayered",   rBase + 12, 0.45f},
    };

    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "TriplanarWiring";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/triplanar_pbr.glsl";
    for (const Lane& l : lanes)
        doc.properties[l.name] = l.value;

    Material* mat = m_registry.Register(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_GE(mat->GetCacheSize(), static_cast<uint32_t>(sizeof(MaterialGpuParams)));

    const uint8_t* cache = mat->GetCacheData();
    for (const Lane& l : lanes)
    {
        float v = 0.0f;
        std::memcpy(&v, cache + l.offset, sizeof(float));
        EXPECT_FLOAT_EQ(v, l.value) << l.name << " did not land at its uParams lane offset " << l.offset;
    }
}

// The generic lanes a surface reads without declaring anything. The graph
// editor's live preview writes exactly these keys and compiles `Mat.uUser<n>.<c>`
// into the surface it previews (Editor MaterialGraphPreviewModel), so a value
// scrub is a uniform push instead of a recompile; nothing else in the editor
// notices if they stop resolving, which is why the wiring is pinned here.
TEST_F(MaterialRegistryTest, Register_UserLanes_WireTheGraphPreviewKeysToGpuOffsets)
{
    const uint32_t userBase = MaterialParamLaneOffset(26);

    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "UserLaneWiring";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    // user12..user15 alias uUser3, which userVec3 claims whole; keep the two
    // spellings off the same bytes so neither's write depends on document order.
    for (uint32_t i = 0; i < 12; ++i)
        doc.properties["user" + std::to_string(i)] = 0.5f + static_cast<float>(i);
    doc.properties["userVec3"] = std::vector<float>{1.5f, 2.5f, 3.5f, 4.5f};

    Material* mat = m_registry.Register(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_GE(mat->GetCacheSize(), static_cast<uint32_t>(sizeof(MaterialGpuParams)));

    const uint8_t* cache = mat->GetCacheData();
    for (uint32_t i = 0; i < 12; ++i)
    {
        float v = 0.0f;
        std::memcpy(&v, cache + userBase + i * 4, sizeof(float));
        EXPECT_FLOAT_EQ(v, 0.5f + static_cast<float>(i))
            << "user" << i << " did not land at uUser" << (i / 4) << " lane offset "
            << (userBase + i * 4);
    }
    float vec3Lane[4] = {};
    std::memcpy(vec3Lane, cache + userBase + 48, sizeof(vec3Lane));
    EXPECT_FLOAT_EQ(vec3Lane[0], 1.5f);
    EXPECT_FLOAT_EQ(vec3Lane[3], 4.5f) << "userVec3 did not cover the whole uUser3 lane";
}

// A project surface declares its own properties; the declared table replaces the
// hand-typed name->lane wiring. Drive the real Register() with a water-style
// document, install the table the composer would pack from the surface, and verify
// each declared name lands at its packed offset with the declared type, that a
// document override wins over the declared default, and that a re-Register with
// the key removed reverts to the declared default.
TEST_F(MaterialRegistryTest, Register_DeclaredSurface_WiresNamedPropertiesToPackedLanes)
{
    const auto table = std::make_shared<const ShaderPropertyTable>(BuildShaderPropertyTable({
        ShaderPropertySource{"// @property float alphaCutoff default=0.5 range=0,1\n"
                             "// @property float specularIor default=1.5 hidden\n",
                             "adapter_forward.glsl", ShaderPropertyOrigin::Adapter},
        ShaderPropertySource{"// @property color baseColor default=1,1,1,1\n"     // lane 0
                             "// @property vec2 scrollSpeed default=0,0.2\n"      // lane 1 .xy
                             "// @property float overlayPower default=0.624\n"    // lane 1 .z
                             "// @property int layers default=2\n"                // lane 1 .w
                             "// @property bool wobble default=true\n"            // lane 2 .x
                             "// @property color fresnelColor default=0.9,0.9,1 hdr\n", // lane 3 .xyz
                             "water_stylized.glsl", ShaderPropertyOrigin::Surface}}));
    ASSERT_FALSE(table->Rejected()) << table->Errors.front().Message;

    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "DeclaredWiring";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "water_stylized.glsl";
    doc.properties["baseColor"] = std::vector<float>{0.2f, 0.3f, 0.4f, 0.85f};
    doc.properties["overlayPower"] = 0.25f;
    doc.properties["layers"] = static_cast<int32_t>(5);
    doc.properties["wobble"] = false;
    doc.properties["notDeclared"] = 9.0f; // stores nothing, never crashes

    const GUID guid = GUID::Generate();
    Material* mat = m_registry.Register(guid, doc);
    ASSERT_NE(mat, nullptr);
    mat->SetDeclaredProperties(table);
    ASSERT_GE(mat->GetCacheSize(), static_cast<uint32_t>(sizeof(MaterialGpuParams)));

    auto readFloat = [&](uint32_t offset)
    {
        float v = 0.0f;
        std::memcpy(&v, mat->GetCacheData() + offset, sizeof(float));
        return v;
    };
    auto readInt = [&](uint32_t offset)
    {
        int32_t v = 0;
        std::memcpy(&v, mat->GetCacheData() + offset, sizeof(int32_t));
        return v;
    };

    const ShaderProperty& base = *table->Find("baseColor");
    EXPECT_EQ(base.ByteOffset, MaterialParamLaneOffset(0));
    EXPECT_FLOAT_EQ(readFloat(base.ByteOffset + 0), 0.2f) << "document override wins";
    EXPECT_FLOAT_EQ(readFloat(base.ByteOffset + 12), 0.85f);
    const ShaderProperty& scroll = *table->Find("scrollSpeed");
    EXPECT_EQ(scroll.ByteOffset, MaterialParamLaneOffset(1));
    EXPECT_FLOAT_EQ(readFloat(scroll.ByteOffset + 4), 0.2f) << "declared default seeds an unauthored key";
    const ShaderProperty& overlay = *table->Find("overlayPower");
    EXPECT_EQ(overlay.ByteOffset, MaterialParamLaneOffset(1) + 8);
    EXPECT_FLOAT_EQ(readFloat(overlay.ByteOffset), 0.25f);
    const ShaderProperty& layers = *table->Find("layers");
    EXPECT_EQ(layers.ByteOffset, MaterialParamLaneOffset(1) + 12);
    EXPECT_EQ(readInt(layers.ByteOffset), 5) << "int lands as int bits, not float";
    const ShaderProperty& wobble = *table->Find("wobble");
    EXPECT_FLOAT_EQ(readFloat(wobble.ByteOffset), 0.0f) << "bool false overrides the true default";
    const ShaderProperty& fresnel = *table->Find("fresnelColor");
    EXPECT_EQ(fresnel.ByteOffset, MaterialParamLaneOffset(3));
    EXPECT_FLOAT_EQ(readFloat(fresnel.ByteOffset + 8), 1.0f);
    EXPECT_FALSE(table->Find("specularIor")->HasLane) << "an adapter read nobody stores is a constant";
    EXPECT_FLOAT_EQ(mat->GetFloat("overlayPower"_sid), 0.25f) << "the name->offset table is the declared one";
    EXPECT_FLOAT_EQ(mat->GetFloat("notDeclared"_sid, -1.0f), -1.0f);

    // Re-Register with the override removed: the declared default comes back.
    doc.properties.erase("overlayPower");
    doc.properties["layers"] = static_cast<int32_t>(1);
    Material* same = m_registry.Register(guid, doc);
    ASSERT_EQ(same, mat);
    EXPECT_FLOAT_EQ(readFloat(overlay.ByteOffset), 0.624f);
    EXPECT_EQ(readInt(layers.ByteOffset), 1);
    EXPECT_FLOAT_EQ(readFloat(base.ByteOffset + 0), 0.2f);
}

// A JSON "1" parses as an int32_t document property (JS/JSON cannot express 1.0
// distinctly, so the converter's flag lanes arrive int-typed). It must land as
// FLOAT bits in the all-float param block: raw int bits read as denormals (~0.0)
// on the GPU, which silently kept triplanarLayered=1 documents on the collapsed
// path (found by the PR #546 follow-up runtime spot check).
TEST_F(MaterialRegistryTest, Register_IntTypedDocumentProperty_LandsAsFloatBits)
{
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "IntTypedFlag";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/triplanar_pbr.glsl";
    doc.properties["triplanarLayered"] = static_cast<int32_t>(1);

    Material* mat = m_registry.Register(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_FLOAT_EQ(mat->GetFloat("triplanarLayered"_sid), 1.0f);

    float raw = 0.0f;
    const uint32_t off = MaterialParamLaneOffset(24) + 12;
    std::memcpy(&raw, mat->GetCacheData() + off, sizeof(float));
    EXPECT_FLOAT_EQ(raw, 1.0f) << "int document value must be coerced to float bits, not memcpy'd raw";
}

// ---- User keywords: variant-key hash lane (slice N) ----
//
// DeriveBaseVariantKey drives the recompile trigger: a keyword edit changes the
// variant key, which RegisterMaterialFromDocument's pipelineAffectingChanged
// compares (!(prevKey == newKey)) to route through CompileMaterialPipeline (which
// clears the per-material variant cache at its start). Device-free — the free
// function needs no registry/device.

static MaterialDocument MakeKeywordDoc(std::vector<std::string> keywords)
{
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "Kw";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "surfaces/standard_surface.glsl";
    doc.keywords = std::move(keywords);
    return doc;
}

TEST(MaterialVariantKeyDerivation, UserKeywordHash_ZeroWhenAbsent)
{
    EXPECT_EQ(DeriveBaseVariantKey(MakeKeywordDoc({})).userKeywordHash, 0ull);
}

TEST(MaterialVariantKeyDerivation, UserKeywordHash_OrderIndependent)
{
    const auto ab = DeriveBaseVariantKey(MakeKeywordDoc({"FLOW_MODE", "HIGH_DETAIL"}));
    const auto ba = DeriveBaseVariantKey(MakeKeywordDoc({"HIGH_DETAIL", "FLOW_MODE"}));
    EXPECT_EQ(ab.userKeywordHash, ba.userKeywordHash);
    EXPECT_TRUE(ab == ba);
}

TEST(MaterialVariantKeyDerivation, UserKeywordHash_KeywordFlipChangesKey)
{
    const auto a = DeriveBaseVariantKey(MakeKeywordDoc({"FLOW_MODE"}));
    const auto b = DeriveBaseVariantKey(MakeKeywordDoc({"HIGH_DETAIL"}));
    EXPECT_NE(a.userKeywordHash, b.userKeywordHash);
    // The exact comparison RegisterMaterialFromDocument uses to decide "recompile".
    EXPECT_FALSE(a == b);
    // A keyword edit also differs from the no-keyword baseline.
    EXPECT_FALSE(a == DeriveBaseVariantKey(MakeKeywordDoc({})));
}

// ---- Lobe opt-in bools: authored key -> MaterialKeyword -> GLSL define ----
//
// Every optional BRDF lobe is compiled in by one authored `enable*` bool. If that
// name stops deriving its keyword the lobe silently disappears from the variant and
// the material renders as plain PBR with no warning anywhere. These pin the whole
// chain, including the define string the shaders actually #ifdef on.

static MaterialDocument MakeLobeDoc(const std::string& enableKey, bool on)
{
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "Lobe";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.properties[enableKey] = on;
    return doc;
}

static bool DefinesContain(const ShaderVariantKey& key, const char* define)
{
    const std::vector<std::string> defs = GenerateDefines(key, "StandardPBR");
    return std::find(defs.begin(), defs.end(), define) != defs.end();
}

TEST(MaterialVariantKeyDerivation, LobeEnableBoolCompilesItsDefine)
{
    const struct { const char* Key; MaterialKeyword Keyword; const char* Define; } kLobes[] = {
        {"enableClearCoat",    MaterialKeyword::ClearCoat,    "GE_CLEARCOAT_ENABLED"},
        {"enableSheen",        MaterialKeyword::Sheen,        "GE_SHEEN_ENABLED"},
        {"enableFuzz",         MaterialKeyword::Fuzz,         "GE_FUZZ_ENABLED"},
        {"enableCoatNormal",   MaterialKeyword::CoatNormal,   "GE_COAT_NORMAL_ENABLED"},
        {"enableAnisotropy",   MaterialKeyword::Anisotropy,   "GE_ANISOTROPY_ENABLED"},
        {"enableSubsurface",   MaterialKeyword::Subsurface,   "GE_SUBSURFACE_ENABLED"},
        {"enableTransmission", MaterialKeyword::Transmission, "GE_TRANSMISSION_ENABLED"},
        {"enableIridescence",  MaterialKeyword::Iridescence,  "GE_IRIDESCENCE_ENABLED"},
    };
    for (const auto& lobe : kLobes)
    {
        const ShaderVariantKey on = DeriveBaseVariantKey(MakeLobeDoc(lobe.Key, true));
        EXPECT_TRUE(HasKeyword(on.materialKeywords, lobe.Keyword)) << lobe.Key;
        EXPECT_TRUE(DefinesContain(on, lobe.Define)) << lobe.Key;

        const ShaderVariantKey off = DeriveBaseVariantKey(MakeLobeDoc(lobe.Key, false));
        EXPECT_FALSE(HasKeyword(off.materialKeywords, lobe.Keyword)) << lobe.Key;
        EXPECT_FALSE(DefinesContain(off, lobe.Define)) << lobe.Key;
        EXPECT_FALSE(on == off) << lobe.Key << " must change the variant key so it recompiles";
    }
}

// Thick refraction implies the base transmission lobe: the thick GLSL is nested inside
// GE_TRANSMISSION_ENABLED, so deriving Thick alone would compile to nothing.
TEST(MaterialVariantKeyDerivation, ThickTransmissionImpliesTransmission)
{
    const ShaderVariantKey key = DeriveBaseVariantKey(MakeLobeDoc("enableTransmissionThick", true));
    EXPECT_TRUE(DefinesContain(key, "GE_TRANSMISSION_ENABLED"));
    EXPECT_TRUE(DefinesContain(key, "GE_TRANSMISSION_THICK"));
}

// Each alpha mode compiles its own adapter output: Mask discards, Blend keeps the
// surface opacity as the blended alpha, and Opaque outputs full coverage.
TEST(MaterialVariantKeyDerivation, AlphaModeDerivesItsKeyword)
{
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "AlphaModes";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.alphaMode = MaterialAlphaMode::Opaque;
    const ShaderVariantKey opaque = DeriveBaseVariantKey(doc);
    EXPECT_FALSE(DefinesContain(opaque, "ALPHA_TEST"));
    EXPECT_FALSE(DefinesContain(opaque, "GE_ALPHA_BLEND"));

    doc.alphaMode = MaterialAlphaMode::Mask;
    const ShaderVariantKey mask = DeriveBaseVariantKey(doc);
    EXPECT_TRUE(HasKeyword(mask.materialKeywords, MaterialKeyword::AlphaTest));
    EXPECT_TRUE(DefinesContain(mask, "ALPHA_TEST"));
    EXPECT_FALSE(DefinesContain(mask, "GE_ALPHA_BLEND"));

    doc.alphaMode = MaterialAlphaMode::Blend;
    const ShaderVariantKey blend = DeriveBaseVariantKey(doc);
    EXPECT_TRUE(HasKeyword(blend.materialKeywords, MaterialKeyword::AlphaBlend));
    EXPECT_TRUE(DefinesContain(blend, "GE_ALPHA_BLEND"));
    EXPECT_FALSE(DefinesContain(blend, "ALPHA_TEST"));
    EXPECT_FALSE(opaque == blend) << "a blended variant compiles a different output alpha";
}

// A missing key is the same as false — an authored typo must not silently enable a lobe.
TEST(MaterialVariantKeyDerivation, AbsentLobeKeyDerivesNoKeyword)
{
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "Bare";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    EXPECT_EQ(DeriveBaseVariantKey(doc).materialKeywords, MaterialKeyword::None);
}

// Shader-only prewarm derives its key and compile inputs through
// MaterialSystem::DeriveShaderIdentity; registration builds the material the draw
// path compiles from, and the demand prewarm reads that material's own key and
// spec. A warmed variant is only ever used when the two agree.
namespace GameEngine::Engine::Renderer
{
class MaterialShaderIdentityTest : public ::MaterialRegistryTest
{
  protected:
    static void ExpectShaderIdentityMatchesRegistration(RenderServices& rs, const MaterialDocument& doc,
                                                        MaterialAlphaMode registeredAlphaMode)
    {
        const MaterialSystem::MaterialShaderIdentity identity =
            rs.Materials().DeriveShaderIdentity(doc, {});
        Material* mat = rs.Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
        ASSERT_NE(mat, nullptr);
        ASSERT_EQ(mat->GetAlphaMode(), registeredAlphaMode) << "the premise of the document under test";
        EXPECT_EQ(identity.Key, mat->GetVariantKey());
        const MaterialCompileSpec& spec = mat->GetCompileSpec();
        EXPECT_EQ(identity.Spec.surfaceShaderPath, spec.surfaceShaderPath);
        EXPECT_EQ(identity.Spec.vertexModifierPath, spec.vertexModifierPath);
        EXPECT_EQ(identity.Spec.lightingModel, spec.lightingModel);
        EXPECT_EQ(identity.Spec.customVertexShader, spec.customVertexShader);
        EXPECT_EQ(identity.Spec.userKeywords, spec.userKeywords);
        EXPECT_EQ(identity.Spec.alphaTest, spec.alphaTest);
        EXPECT_EQ(identity.Spec.parallax, spec.parallax);
        EXPECT_EQ(identity.AlphaMode, mat->GetAlphaMode());
    }
};
} // namespace GameEngine::Engine::Renderer
using GameEngine::Engine::Renderer::MaterialShaderIdentityTest;

TEST_F(MaterialShaderIdentityTest, KeywordedMaterialMatchesRegistration)
{
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(m_device.get()));

    MaterialDocument doc = MakeTestPBRDocument("Keyworded");
    doc.keywords = {"WET_SURFACE", "DETAIL_LAYER"};
    doc.properties["enableClearCoat"] = true;
    doc.alphaMode = MaterialAlphaMode::Mask;
    ExpectShaderIdentityMatchesRegistration(rs, doc, MaterialAlphaMode::Mask);

    rs.Shutdown();
}

TEST_F(MaterialShaderIdentityTest, EmbeddedModelMaterialMatchesRegistration)
{
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(m_device.get()));

    ImportedMaterialData imported{};
    imported.Name = "fox_material";
    imported.DiffuseTexture = "__embedded:0";
    imported.NormalTexture = "__embedded:1";
    imported.DiffuseColor[0] = imported.DiffuseColor[1] = imported.DiffuseColor[2] = 1.0f;
    imported.DiffuseColor[3] = 1.0f;
    imported.Metallic = 0.0f;
    imported.Roughness = 0.58f;
    imported.AlphaMode = AlphaMode::Mask;
    const ConvertedModelMaterial converted =
        ModelMaterialBridge::Convert(GUID::Generate(), 0, imported);
    ExpectShaderIdentityMatchesRegistration(rs, converted.document, MaterialAlphaMode::Mask);

    rs.Shutdown();
}

// An authored Mask whose alpha source cannot discard (analyzable surface, vertex
// colour ignored, opaque base colour, no albedo texture) registers as Opaque. The
// shader-only prewarm must derive the same demoted key and compile it without
// ALPHA_TEST, or it warms a variant no draw asks for.
TEST_F(MaterialShaderIdentityTest, DemotableMaskMatchesRegistration)
{
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(m_device.get()));

    MaterialDocument doc = MakeTestPBRDocument("DemotableMask");
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.ignoreVertexColor = true;
    doc.alphaMode = MaterialAlphaMode::Mask;
    ExpectShaderIdentityMatchesRegistration(rs, doc, MaterialAlphaMode::Opaque);

    rs.Shutdown();
}
