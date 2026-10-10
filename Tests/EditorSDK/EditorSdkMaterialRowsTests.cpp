// Material inspector row rules. These live in EditorSDK rather than the Editor
// executable so this headless target can link them: a rule that compiles only
// into Editor.exe can be checked no other way than by eye, against a running
// editor.

#include "Editor/Materials/MaterialRows.h"

#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace GameEngine;
namespace Rows = GameEngine::Editor::MaterialRows;

namespace
{

MaterialDocument LitDoc(MaterialAlphaMode mode)
{
    MaterialDocument doc;
    doc.lightingModel = "StandardPBR";
    doc.alphaMode = mode;
    return doc;
}

// The `// @texture` names of the engine's standard surface, and of a surface that declares none (the
// fixed ladder, which has no height slot).
const std::vector<std::string> kStandardSurfaceTextures = {"albedoMap", "normalMap",     "metallicRoughnessMap",
                                                           "emissiveMap", "aoMap", "coatNormalMap", "heightMap"};
const std::vector<std::string> kLadderSurfaceTextures = {};

} // namespace

// ---------------------------------------------------------------------------
// alphaCutoff visibility — only Mask compiles the alpha test in, so the row is
// a control that changes nothing on any other mode.
// ---------------------------------------------------------------------------

TEST(MaterialRowsVisibility, AlphaCutoffShownOnMask)
{
    EXPECT_TRUE(Rows::IsPropertyVisible("alphaCutoff", LitDoc(MaterialAlphaMode::Mask)));
}

TEST(MaterialRowsVisibility, AlphaCutoffHiddenOnOpaque)
{
    EXPECT_FALSE(Rows::IsPropertyVisible("alphaCutoff", LitDoc(MaterialAlphaMode::Opaque)));
}

TEST(MaterialRowsVisibility, AlphaCutoffHiddenOnBlend)
{
    EXPECT_FALSE(Rows::IsPropertyVisible("alphaCutoff", LitDoc(MaterialAlphaMode::Blend)));
}

TEST(MaterialRowsVisibility, AlphaCutoffHiddenOnShadowOnlyDespiteMask)
{
    // ShadowOnly forces Blend at registration, so a stale Mask in the document
    // must not resurrect the row.
    MaterialDocument doc = LitDoc(MaterialAlphaMode::Mask);
    doc.lightingModel = "ShadowOnly";
    EXPECT_FALSE(Rows::IsPropertyVisible("alphaCutoff", doc));
}

TEST(MaterialRowsVisibility, AlphaCutoffShownOnUnlitMask)
{
    // Unlit still runs the fragment alpha test; only ShadowOnly does not.
    MaterialDocument doc = LitDoc(MaterialAlphaMode::Mask);
    doc.lightingModel = "Unlit";
    EXPECT_TRUE(Rows::IsPropertyVisible("alphaCutoff", doc));
}

TEST(MaterialRowsVisibility, AlwaysVisibleRowsIgnoreAlphaMode)
{
    for (const MaterialAlphaMode mode :
         {MaterialAlphaMode::Opaque, MaterialAlphaMode::Mask, MaterialAlphaMode::Blend})
    {
        const MaterialDocument doc = LitDoc(mode);
        EXPECT_TRUE(Rows::IsPropertyVisible("baseColor", doc));
        EXPECT_TRUE(Rows::IsPropertyVisible("opacity", doc));
        EXPECT_TRUE(Rows::IsPropertyVisible("emissive", doc));
    }
}

TEST(MaterialRowsVisibility, SurfaceResponseRowsHiddenOnUnlitAndShadowOnly)
{
    MaterialDocument unlit = LitDoc(MaterialAlphaMode::Opaque);
    unlit.lightingModel = "Unlit";
    MaterialDocument shadowOnly = LitDoc(MaterialAlphaMode::Opaque);
    shadowOnly.lightingModel = "ShadowOnly";

    for (const char* key : {"roughness", "metallic", "ao", "enableClearCoat", "specularWeight"})
    {
        EXPECT_TRUE(Rows::IsPropertyVisible(key, LitDoc(MaterialAlphaMode::Opaque))) << key;
        EXPECT_FALSE(Rows::IsPropertyVisible(key, unlit)) << key;
        EXPECT_FALSE(Rows::IsPropertyVisible(key, shadowOnly)) << key;
    }
}

TEST(MaterialRowsVisibility, LobeScalarsGateOnTheirEnableToggle)
{
    MaterialDocument doc = LitDoc(MaterialAlphaMode::Opaque);
    EXPECT_FALSE(Rows::IsPropertyVisible("clearCoat", doc));
    EXPECT_FALSE(Rows::IsPropertyVisible("coatColor", doc));

    doc.properties["enableClearCoat"] = true;
    EXPECT_TRUE(Rows::IsPropertyVisible("clearCoat", doc));
    EXPECT_TRUE(Rows::IsPropertyVisible("coatColor", doc));

    doc.properties["enableClearCoat"] = false;
    EXPECT_FALSE(Rows::IsPropertyVisible("clearCoat", doc));
}

TEST(MaterialRowsVisibility, SixAxisBlockNeedsUnlitVolumetricSurface)
{
    MaterialDocument doc = LitDoc(MaterialAlphaMode::Opaque);
    doc.surfaceShader = "Shaders/Surface/volumetric_six_axis.glsl";
    // Volumetric six-axis is an unlit-only surface; the block stays hidden on a
    // lit document even with the surface shader set.
    EXPECT_FALSE(Rows::IsPropertyVisible("sixAxisScatter", doc));

    doc.lightingModel = "Unlit";
    EXPECT_TRUE(Rows::IsPropertyVisible("sixAxisScatter", doc));
    // It also displaces the PBR scalars it replaces.
    EXPECT_FALSE(Rows::IsPropertyVisible("roughness", doc));
}

// ---------------------------------------------------------------------------
// Texture slot visibility
// ---------------------------------------------------------------------------

TEST(MaterialRowsTextures, CoatNormalMapGatesOnClearCoat)
{
    MaterialDocument doc = LitDoc(MaterialAlphaMode::Opaque);
    EXPECT_FALSE(Rows::IsTextureVisible("coatNormalMap", doc, kLadderSurfaceTextures));
    doc.properties["enableClearCoat"] = true;
    EXPECT_TRUE(Rows::IsTextureVisible("coatNormalMap", doc, kLadderSurfaceTextures));
}

TEST(MaterialRowsTextures, ShadingMapsHiddenWhereNothingSamplesThem)
{
    MaterialDocument unlit = LitDoc(MaterialAlphaMode::Opaque);
    unlit.lightingModel = "Unlit";
    MaterialDocument shadowOnly = LitDoc(MaterialAlphaMode::Opaque);
    shadowOnly.lightingModel = "ShadowOnly";

    // Unlit runs no BRDF, so the normal and the metallic/roughness pair have no
    // consumer.
    EXPECT_FALSE(Rows::IsTextureVisible("normalMap", unlit, kLadderSurfaceTextures));
    EXPECT_FALSE(Rows::IsTextureVisible("metallicRoughnessMap", unlit, kLadderSurfaceTextures));
    EXPECT_FALSE(Rows::IsTextureVisible("metallicRoughnessMap", shadowOnly, kLadderSurfaceTextures));
    EXPECT_FALSE(Rows::IsTextureVisible("aoMap", shadowOnly, kLadderSurfaceTextures));

    // albedoMap stays: the alpha channel still feeds the alpha test.
    EXPECT_TRUE(Rows::IsTextureVisible("albedoMap", unlit, kLadderSurfaceTextures));
    EXPECT_TRUE(Rows::IsTextureVisible("albedoMap", shadowOnly, kLadderSurfaceTextures));
}

TEST(MaterialRowsTextures, VolumetricSixAxisRestoresItsSampledMaps)
{
    MaterialDocument doc = LitDoc(MaterialAlphaMode::Opaque);
    doc.lightingModel = "Unlit";
    doc.surfaceShader = "Shaders/Surface/volumetric_six_axis.glsl";
    // The six-axis surface samples both itself, so they come back on an unlit
    // document that would otherwise hide them.
    EXPECT_TRUE(Rows::IsTextureVisible("normalMap", doc, kLadderSurfaceTextures));
    EXPECT_TRUE(Rows::IsTextureVisible("metallicRoughnessMap", doc, kLadderSurfaceTextures));
}

TEST(MaterialRowsTextures, HeightRowOnlyWhereTheSurfaceDeclaresAHeightSlot)
{
    const MaterialDocument doc = LitDoc(MaterialAlphaMode::Opaque);
    EXPECT_TRUE(Rows::IsTextureVisible("heightMap", doc, kStandardSurfaceTextures));
    // No ladder surface reads a height map (triplanar, the extended surface, a project surface
    // without the declaration), so binding one there would drive nothing.
    EXPECT_FALSE(Rows::IsTextureVisible("heightMap", doc, kLadderSurfaceTextures));
    EXPECT_FALSE(Rows::IsTextureVisible("heightMap", doc, {"albedoMap", "flowMask"}));

    // Nothing marches without the lit shading path.
    MaterialDocument unlit = doc;
    unlit.lightingModel = "Unlit";
    MaterialDocument shadowOnly = doc;
    shadowOnly.lightingModel = "ShadowOnly";
    EXPECT_FALSE(Rows::IsTextureVisible("heightMap", unlit, kStandardSurfaceTextures));
    EXPECT_FALSE(Rows::IsTextureVisible("heightMap", shadowOnly, kStandardSurfaceTextures));
}

TEST(MaterialRowsTextures, ReliefDepthRowOnlyWithABoundHeightMap)
{
    MaterialDocument doc = LitDoc(MaterialAlphaMode::Opaque);
    EXPECT_FALSE(Rows::IsReliefDepthVisible(doc, kStandardSurfaceTextures));
    doc.textures["heightMap"] = "";
    EXPECT_FALSE(Rows::IsReliefDepthVisible(doc, kStandardSurfaceTextures)) << "an empty slot is unbound";
    doc.textures["heightMap"] = "7d1c2a4e-0000-4000-8000-000000000001";
    EXPECT_TRUE(Rows::IsReliefDepthVisible(doc, kStandardSurfaceTextures));
    EXPECT_FALSE(Rows::IsReliefDepthVisible(doc, kLadderSurfaceTextures))
        << "a height map bound where no height slot exists is refused, and its depth reads nothing";
}

// ---------------------------------------------------------------------------
// Control selection — which widget a visible row renders as.
// ---------------------------------------------------------------------------

TEST(MaterialRowsControls, AlphaCutoffIsABoundedSlider)
{
    // Without this the row renders as an unbounded drag field and an author can
    // type a cutoff outside the [0,1] the fragment stage clamps to.
    EXPECT_TRUE(Rows::IsSlider01Property("alphaCutoff"));
}

TEST(MaterialRowsControls, NormalizedScalarsAreSliders)
{
    for (const char* key : {"roughness", "metallic", "ao", "opacity", "clearCoat",
                            "transmissionWeight", "thinFilmWeight"})
        EXPECT_TRUE(Rows::IsSlider01Property(key)) << key;
}

TEST(MaterialRowsControls, UnboundedScalarsAreNotSliders)
{
    // Physical quantities with no normalized range must stay drag fields.
    for (const char* key : {"emissionLuminance", "specularIor", "thinFilmThickness",
                            "attenuationDistance", "anisotropy"})
        EXPECT_FALSE(Rows::IsSlider01Property(key)) << key;
}

TEST(MaterialRowsControls, ColorPropertiesAreDetectedByNameAndException)
{
    EXPECT_TRUE(Rows::IsColorProperty("baseColor"));
    EXPECT_TRUE(Rows::IsColorProperty("sheenColor"));
    EXPECT_TRUE(Rows::IsColorProperty("attenuationColor"));
    // `emissive` carries no "color" substring but is a color row.
    EXPECT_TRUE(Rows::IsColorProperty("emissive"));

    EXPECT_FALSE(Rows::IsColorProperty("roughness"));
    EXPECT_FALSE(Rows::IsColorProperty("alphaCutoff"));
}

// ---------------------------------------------------------------------------
// Row order and labels
// ---------------------------------------------------------------------------

TEST(MaterialRowsOrder, AlphaCutoffSitsDirectlyUnderOpacity)
{
    const auto& order = Rows::BuiltInPropertyOrder();
    const auto opacity = std::find(order.begin(), order.end(), "opacity");
    ASSERT_NE(opacity, order.end());
    ASSERT_NE(opacity + 1, order.end());
    EXPECT_EQ(*(opacity + 1), "alphaCutoff");
}

TEST(MaterialRowsOrder, BuiltInNamesCoverTheAlphaTestPair)
{
    // Keys missing from this set are re-rendered a second time in the custom
    // (shader-declared) property section.
    EXPECT_EQ(Rows::BuiltInPropertyNames().count("opacity"), 1u);
    EXPECT_EQ(Rows::BuiltInPropertyNames().count("alphaCutoff"), 1u);
}

TEST(MaterialRowsOrder, TextureSlotOrderLeadsWithAlbedo)
{
    const auto& order = Rows::TextureSlotOrder();
    ASSERT_FALSE(order.empty());
    EXPECT_EQ(order.front(), "albedoMap");
    EXPECT_NE(std::find(order.begin(), order.end(), "coatNormalMap"), order.end());
}

TEST(MaterialRowsOrder, HeightFollowsTheSurfaceMap)
{
    const auto& order = Rows::TextureSlotOrder();
    const auto surface = std::find(order.begin(), order.end(), "metallicRoughnessMap");
    ASSERT_NE(surface, order.end());
    ASSERT_NE(surface + 1, order.end());
    EXPECT_EQ(*(surface + 1), "heightMap");
}

TEST(MaterialRowsOrder, ReliefDepthIsLaidOutWithTheTexturesNotTheProperties)
{
    // A built-in name, so it never repeats in the custom property section, and absent from the
    // property order, so its one row is the one under the Height map.
    EXPECT_EQ(Rows::BuiltInPropertyNames().count("reliefDepth"), 1u);
    const auto& order = Rows::BuiltInPropertyOrder();
    EXPECT_EQ(std::find(order.begin(), order.end(), "reliefDepth"), order.end());
}

// Relief Depth stops where the self-shadow's eight samples stop resolving a long shadow (the user
// doc's Limits): a typed 0.1 stores 0.04, and the engine's default sits inside the range.
TEST(MaterialRowsRange, ReliefDepthStopsWhereTheSelfShadowStopsResolving)
{
    EXPECT_FLOAT_EQ(std::clamp(0.1f, 0.0f, Rows::kReliefDepthSliderMax), 0.04f);
    EXPECT_GT(Rows::kReliefDepthSliderMax, GameEngine::kDefaultReliefDepth);
}

TEST(MaterialRowsLabels, PropertyLabelSplitsCamelCase)
{
    EXPECT_EQ(Rows::PropertyLabel("baseColor"), "Base Color");
    EXPECT_EQ(Rows::PropertyLabel("emissionLuminance"), "Emission Luminance");
    EXPECT_EQ(Rows::PropertyLabel("alphaCutoff"), "Alpha Cutoff");
    // "ao" would otherwise render as "Ao".
    EXPECT_EQ(Rows::PropertyLabel("ao"), "AO");
}

TEST(MaterialRowsLabels, TextureSlotLabelDropsTheMapSuffix)
{
    EXPECT_EQ(Rows::TextureSlotLabel("albedoMap"), "Albedo");
    EXPECT_EQ(Rows::TextureSlotLabel("metallicRoughnessMap"), "Metallic Roughness");
    EXPECT_EQ(Rows::TextureSlotLabel("coatNormalMap"), "Coat Normal");
}

// ---------------------------------------------------------------------------
// Lighting model
// ---------------------------------------------------------------------------

TEST(MaterialRowsLightingModel, MatchIsCaseInsensitive)
{
    EXPECT_TRUE(Rows::IsLightingModelUnlit("Unlit"));
    EXPECT_TRUE(Rows::IsLightingModelUnlit("unlit"));
    EXPECT_TRUE(Rows::IsLightingModelShadowOnly("ShadowOnly"));
    EXPECT_TRUE(Rows::IsLightingModelShadowOnly("shadowonly"));

    EXPECT_FALSE(Rows::IsLightingModelUnlit("StandardPBR"));
    EXPECT_FALSE(Rows::IsLightingModelShadowOnly("StandardPBR"));
    EXPECT_FALSE(Rows::IsLightingModelUnlit(""));
}
