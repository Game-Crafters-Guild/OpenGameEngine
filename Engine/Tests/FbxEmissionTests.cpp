// FBX emission as the importer reads it (FbxEmission.h): the constant EmissiveColor
// times EmissiveFactor, and the factor alone beside an emissive texture, whose
// constant exporters leave at gray 0.5.

#include <gtest/gtest.h>

#include "FbxEmission.h"

#include <ufbx.h>

#include <string>
#include <string_view>

namespace
{

// Two Phong materials: "Textured" has a texture on EmissiveColor, "Constant" none.
constexpr std::string_view kTwoEmitters = R"(; FBX 7.5.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7500
}
Objects:  {
	Material: 100, "Material::Textured", "" {
		Version: 102
		ShadingModel: "phong"
		Properties70:  {
			P: "EmissiveColor", "Color", "", "A",0.5,0.5,0.5
			P: "EmissiveFactor", "Number", "", "A",2
		}
	}
	Material: 200, "Material::Constant", "" {
		Version: 102
		ShadingModel: "phong"
		Properties70:  {
			P: "EmissiveColor", "Color", "", "A",0.4,0.2,0.1
			P: "EmissiveFactor", "Number", "", "A",0.5
		}
	}
	Texture: 300, "Texture::Glow", "" {
		Type: "TextureVideoClip"
		FileName: "glow.png"
		RelativeFilename: "glow.png"
	}
}
Connections:  {
	C: "OP",300,100, "EmissiveColor"
}
)";

struct LoadedScene
{
    ufbx_scene* Scene = nullptr;
    ~LoadedScene() { ufbx_free_scene(Scene); }
};

const ufbx_material* FindMaterial(const ufbx_scene& scene, std::string_view name)
{
    for (const ufbx_material* material : scene.materials)
        if (std::string_view(material->name.data, material->name.length) == name)
            return material;
    return nullptr;
}

void ReadEmission(std::string_view materialName, bool& textureBound, float (&rgb)[3])
{
    ufbx_load_opts opts{};
    opts.load_external_files = false;
    opts.ignore_missing_external_files = true;
    ufbx_error error{};
    LoadedScene loaded{ufbx_load_memory(kTwoEmitters.data(), kTwoEmitters.size(), &opts, &error)};
    ASSERT_NE(loaded.Scene, nullptr) << std::string(error.description.data, error.description.length);
    const ufbx_material* material = FindMaterial(*loaded.Scene, materialName);
    ASSERT_NE(material, nullptr) << materialName;
    textureBound = material->fbx.emission_color.texture != nullptr;
    GameEngine::FbxImport::ReadEmissiveColor(*material, textureBound, rgb);
}

} // namespace

TEST(FbxEmission, ATextureBesideTheConstantReadsWhiteTimesTheFactor)
{
    bool textureBound = false;
    float rgb[3] = {};
    ReadEmission("Textured", textureBound, rgb);
    ASSERT_TRUE(textureBound) << "the fixture binds a texture to EmissiveColor";
    EXPECT_FLOAT_EQ(rgb[0], 2.0f);
    EXPECT_FLOAT_EQ(rgb[1], 2.0f);
    EXPECT_FLOAT_EQ(rgb[2], 2.0f);
}

TEST(FbxEmission, WithoutATextureTheConstantTimesTheFactorIsTheColor)
{
    bool textureBound = true;
    float rgb[3] = {};
    ReadEmission("Constant", textureBound, rgb);
    ASSERT_FALSE(textureBound);
    EXPECT_FLOAT_EQ(rgb[0], 0.2f);
    EXPECT_FLOAT_EQ(rgb[1], 0.1f);
    EXPECT_FLOAT_EQ(rgb[2], 0.05f);
}
