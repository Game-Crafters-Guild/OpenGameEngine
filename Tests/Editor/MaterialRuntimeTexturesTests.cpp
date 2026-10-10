// The material inspector's hand-off of a document's textures to the runtime material
// (Editor/Materials/MaterialRuntimeTextures).
//
// Both functions route by name through the material's own slot set, which is what a surface that
// declares its textures depends on: the standard surface's heightMap binds to ordinal 6, which the
// fixed ladder gives to roughnessMap. Routing through the ladder instead would write a stale
// roughnessMap key onto the height slot, and a write-back of the ladder's names would report
// roughnessMap and metallicMap as unknown keys on every edit of a Standard PBR material.

#include <gtest/gtest.h>

#include "Editor/Materials/MaterialRuntimeTextures.h"

#include "AssetCore/GUID.h"
#include "Engine/Rendering/Material.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace GameEngine;
using Engine::Renderer::Material;
using Engine::Renderer::TextureSlot;
namespace RuntimeTextures = GameEngine::Editor::MaterialRuntimeTextures;

// The standard surface's declared slots, as registration installs them.
Material MakeStandardSurfaceMaterial()
{
    Material material = Material::TestFactory::Create(GUID::Generate(), "StandardSurfaceProbe", 64);
    material.SetTextureSlotMap({{"albedoMap", 0},
                                {"normalMap", 1},
                                {"metallicRoughnessMap", 2},
                                {"emissiveMap", 3},
                                {"aoMap", 4},
                                {"coatNormalMap", 5},
                                {"heightMap", 6}});
    return material;
}

std::array<float, 8> Tiling(float scale)
{
    return {scale, 0.0f, 0.0f, 0.0f, 0.0f, scale, 0.0f, 0.0f};
}

float TilingAt(const Material& material, TextureSlot slot)
{
    return material.GetTextureTransforms()[static_cast<size_t>(slot) * 8];
}

TEST(MaterialRuntimeTextures, WriteBackAddsEverySurfaceSlotTheDocumentLeavesOut)
{
    const Material material = MakeStandardSurfaceMaterial();
    MaterialDocument doc;
    doc.textures["albedoMap"] = "albedo-guid";

    const MaterialDocument bound = RuntimeTextures::WithEverySurfaceSlot(doc, material);
    EXPECT_EQ(bound.textures.at("albedoMap"), "albedo-guid") << "an authored binding is kept";
    for (const char* slot : {"normalMap", "metallicRoughnessMap", "emissiveMap", "aoMap", "coatNormalMap", "heightMap"})
    {
        ASSERT_EQ(bound.textures.count(slot), 1u) << slot;
        EXPECT_TRUE(bound.textures.at(slot).empty()) << slot << " is written back empty, so a removed map clears";
    }
    EXPECT_EQ(bound.textures.size(), 7u);
    EXPECT_EQ(bound.textures.count("roughnessMap"), 0u) << "a ladder name the surface lacks would warn as unknown";
    EXPECT_EQ(bound.textures.count("metallicMap"), 0u);
}

TEST(MaterialRuntimeTextures, WriteBackOnALadderSurfaceWritesTheLadder)
{
    const Material material = Material::TestFactory::Create(GUID::Generate(), "LadderSurfaceProbe", 64);
    const MaterialDocument bound = RuntimeTextures::WithEverySurfaceSlot(MaterialDocument{}, material);
    EXPECT_EQ(bound.textures.size(), 8u);
    EXPECT_EQ(bound.textures.count("roughnessMap"), 1u);
    EXPECT_EQ(bound.textures.count("heightMap"), 0u) << "no ladder surface has a height slot";
}

TEST(MaterialRuntimeTextures, TransformsRouteByNameAndResetWhatTheDocumentDropped)
{
    Material material = MakeStandardSurfaceMaterial();
    // A tiling the document no longer carries, left behind by an earlier edit.
    material.SetTextureTransform(TextureSlot::kMetalRough, 5.0f, 5.0f, 0.0f, 0.0f);

    MaterialDocument doc;
    doc.textureTransforms["heightMap"] = Tiling(3.0f);
    doc.textureTransforms["roughnessMap"] = Tiling(9.0f); // stale: the standard surface has no such slot
    RuntimeTextures::ApplyTextureTransforms(doc, material);

    EXPECT_EQ(TilingAt(material, TextureSlot::kRoughness), 3.0f)
        << "heightMap's tiling lands on ordinal 6, and the stale roughnessMap key does not overwrite it";
    EXPECT_EQ(TilingAt(material, TextureSlot::kMetalRough), 1.0f) << "the dropped tiling is reset to identity";
    EXPECT_EQ(TilingAt(material, TextureSlot::kAlbedo), 1.0f);
}

} // namespace
