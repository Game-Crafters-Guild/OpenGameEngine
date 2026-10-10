#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

#include "Assets/MaterialAsset.h"
#include "Assets/MaterialXImport.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"

using namespace GameEngine;

#if defined(_MSC_VER)
#pragma warning(push)
// /analyze sometimes flags gtest EXPECT_* expansions in test code.
#pragma warning(disable : 6326) // Potential comparison of a constant with another constant
#endif

namespace
{

static void WriteTextFile(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    ASSERT_TRUE(out.is_open()) << p.string();
    out << text;
}

static bool ErrorsContain(const MaterialAsset& m, const std::string& needle)
{
    for (const auto& e : m.GetErrors())
    {
        if (e.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// A flat OpenPBR-surface MaterialX document exercising several lobes (constant-value inputs).
static const char* kMtlxIridescentMetal = R"MTLX(<?xml version="1.0"?>
<materialx version="1.39" colorspace="lin_rec709">
  <open_pbr_surface name="M" type="surfaceshader">
    <input name="base_color" type="color3" value="1.0, 0.78, 0.34"/>
    <input name="base_metalness" type="float" value="1.0"/>
    <input name="specular_roughness" type="float" value="0.18"/>
    <input name="coat_weight" type="float" value="0.6"/>
    <input name="thin_film_weight" type="float" value="1.0"/>
    <input name="thin_film_thickness" type="float" value="0.42"/>
    <input name="emission_luminance" type="float" value="500.0"/>
  </open_pbr_surface>
</materialx>)MTLX";

// base_color driven by a flat <image> sibling (direct nodename link).
static const char* kMtlxTexturedFlat = R"MTLX(<?xml version="1.0"?>
<materialx version="1.39">
  <image name="base_tex" type="color3">
    <input name="file" type="filename" value="textures/albedo.png" colorspace="srgb_texture"/>
  </image>
  <open_pbr_surface name="M" type="surfaceshader">
    <input name="base_color" type="color3" nodename="base_tex"/>
  </open_pbr_surface>
</materialx>)MTLX";

// emission_color driven by an <image>, at 300 nits.
static const char* kMtlxTexturedEmission = R"MTLX(<?xml version="1.0"?>
<materialx version="1.39">
  <image name="glow_tex" type="color3">
    <input name="file" type="filename" value="textures/glow.png" colorspace="srgb_texture"/>
  </image>
  <open_pbr_surface name="M" type="surfaceshader">
    <input name="emission_color" type="color3" nodename="glow_tex"/>
    <input name="emission_luminance" type="float" value="300.0"/>
  </open_pbr_surface>
</materialx>)MTLX";

// A constant emission_color, at 300 nits.
static const char* kMtlxColoredEmission = R"MTLX(<?xml version="1.0"?>
<materialx version="1.39">
  <open_pbr_surface name="M" type="surfaceshader">
    <input name="emission_color" type="color3" value="1.0, 0.5, 0.25"/>
    <input name="emission_luminance" type="float" value="300.0"/>
  </open_pbr_surface>
</materialx>)MTLX";

// Normal via a <normalmap> pass-through node, and separate roughness/metalness images inside a
// <nodegraph> referenced by nodegraph+output.
static const char* kMtlxSeparateMaps = R"MTLX(<?xml version="1.0"?>
<materialx version="1.39">
  <image name="nrm_tex" type="vector3"><input name="file" type="filename" value="t/normal.png"/></image>
  <normalmap name="nrm" type="vector3"><input name="in" type="vector3" nodename="nrm_tex"/></normalmap>
  <nodegraph name="NG">
    <image name="rough_tex" type="float"><input name="file" type="filename" value="rough.png"/></image>
    <image name="metal_tex" type="float"><input name="file" type="filename" value="metal.png"/></image>
    <output name="rough_out" type="float" nodename="rough_tex"/>
    <output name="metal_out" type="float" nodename="metal_tex"/>
  </nodegraph>
  <open_pbr_surface name="M" type="surfaceshader">
    <input name="geometry_normal" type="vector3" nodename="nrm"/>
    <input name="specular_roughness" type="float" nodegraph="NG" output="rough_out"/>
    <input name="base_metalness" type="float" nodegraph="NG" output="metal_out"/>
  </open_pbr_surface>
</materialx>)MTLX";

// A single packed image feeds both roughness and metalness (glTF MR convention).
static const char* kMtlxPackedMR = R"MTLX(<?xml version="1.0"?>
<materialx version="1.39">
  <nodegraph name="NG">
    <image name="mr_tex" type="vector3"><input name="file" type="filename" value="mr.png"/></image>
    <output name="rough_out" type="float" nodename="mr_tex"/>
    <output name="metal_out" type="float" nodename="mr_tex"/>
  </nodegraph>
  <open_pbr_surface name="M" type="surfaceshader">
    <input name="specular_roughness" type="float" nodegraph="NG" output="rough_out"/>
    <input name="base_metalness" type="float" nodegraph="NG" output="metal_out"/>
  </open_pbr_surface>
</materialx>)MTLX";

static std::string ReadTextFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Write a .mtlx into a fresh unique temp dir and return its path (the asset can then be loaded,
// edited, and re-written for round-trip checks). Collapses the per-test temp-dir boilerplate.
static std::filesystem::path WriteMtlx(const std::string& xml, const char* dirTag)
{
    const auto tmpRoot = TestUtils::MakeUniqueTempDirectory(dirTag);
    std::error_code ec;
    std::filesystem::remove_all(tmpRoot, ec);
    const auto mtlxPath = tmpRoot / "m.mtlx";
    WriteTextFile(mtlxPath, xml);
    return mtlxPath;
}
} // namespace

TEST(MaterialAssetParsing, RejectsNonJson)
{
    const auto tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_material_parse_nonjson");
    const auto assetsRoot = tmpRoot / "Assets";
    std::error_code ec;
    std::filesystem::remove_all(tmpRoot, ec);
    std::filesystem::create_directories(assetsRoot, ec);

    const auto materialPath = assetsRoot / "Materials" / "Bad.material";
    WriteTextFile(materialPath, "not json");

    MaterialAsset mat(GUID{}, materialPath);
    EXPECT_FALSE(mat.Load());
    EXPECT_TRUE(ErrorsContain(mat, "expected JSON object"));
}

TEST(MaterialAssetParsing, RejectsWrongSchemaVersion)
{
    const auto tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_material_parse_bad_schema");
    const auto assetsRoot = tmpRoot / "Assets";
    std::error_code ec;
    std::filesystem::remove_all(tmpRoot, ec);
    std::filesystem::create_directories(assetsRoot, ec);

    const auto materialPath = assetsRoot / "Materials" / "BadSchema.material";
    WriteTextFile(materialPath,
                  "{\n"
                  "  \"schemaVersion\": 1,\n"
                  "  \"materialName\": \"Bad\",\n"
                  "  \"surfaceShader\": \"Materials/Surfaces/unlit_solid.glsl\",\n"
                  "  \"properties\": {},\n"
                  "  \"textures\": {}\n"
                  "}\n");

    MaterialAsset mat(GUID{}, materialPath);
    EXPECT_FALSE(mat.Load());
    EXPECT_TRUE(ErrorsContain(mat, "unsupported schemaVersion"));
}

TEST(MaterialAssetParsing, MissingSurfaceShaderFailsFast)
{
    const auto tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_material_parse_missing_surface");
    const auto assetsRoot = tmpRoot / "Assets";
    std::error_code ec;
    std::filesystem::remove_all(tmpRoot, ec);
    std::filesystem::create_directories(assetsRoot, ec);

    const auto materialPath = assetsRoot / "Materials" / "MissingSurface.material";
    WriteTextFile(materialPath,
                  "{\n"
                  "  \"schemaVersion\": 2,\n"
                  "  \"materialName\": \"MissingSurface\",\n"
                  "  \"lightingModel\": \"unlit\",\n"
                  "  \"properties\": {},\n"
                  "  \"textures\": {}\n"
                  "}\n");

    MaterialAsset mat(GUID{}, materialPath);
    EXPECT_FALSE(mat.Load());
    EXPECT_TRUE(ErrorsContain(mat, "missing required field 'surfaceShader'"));
}

TEST(MaterialAssetCompilation, LightingModelSelectsUnlitAdapter)
{
    const auto tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_material_compile_unlit");
    const auto assetsRoot = tmpRoot / "Assets";
    std::error_code ec;
    std::filesystem::remove_all(tmpRoot, ec);
    std::filesystem::create_directories(assetsRoot, ec);

    // Provide a minimal surface shader file so compilation has something to include.
    const auto surfacePath = assetsRoot / "Materials" / "Surfaces" / "test_surface.glsl";
    WriteTextFile(surfacePath,
                  "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
                  "  SurfaceOutput o = DefaultSurfaceOutput();\n"
                  "  o.baseColor = vec3(1.0, 0.0, 1.0);\n"
                  "  o.normalWS = normalize(sIn.normalWS);\n"
                  "  return o;\n"
                  "}\n");

    const auto materialPath = assetsRoot / "Materials" / "UnlitSelect.material";
    WriteTextFile(materialPath,
                  "{\n"
                  "  \"schemaVersion\": 2,\n"
                  "  \"materialName\": \"UnlitSelect\",\n"
                  "  \"lightingModel\": \"unlit\",\n"
                  "  \"surfaceShader\": \"Surfaces/test_surface.glsl\",\n"
                  "  \"properties\": {\"baseColor\": [1.0, 0.0, 1.0, 1.0]},\n"
                  "  \"textures\": {}\n"
                  "}\n");

    MaterialAsset mat(GUID{}, materialPath);
    ASSERT_TRUE(mat.Load()) << (mat.GetErrors().empty() ? "" : mat.GetErrors().front());

    Rendering::MaterialBuildContext ctx;
    ctx.AdapterShaderDir = TestPaths::StagedRenderingShadersDir();
    ctx.CacheRoot = tmpRoot / ".Cache" / "Shaders";
    ctx.IncludeDirs = {assetsRoot};
    auto built = Rendering::BuildMaterialToShaderPackage(
        mat.GetDocument(), materialPath, mat.GetName(), ctx, Rendering::ShaderSourceKind::SpirV);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? "" : built.errors.front());
    ASSERT_NE(built.package, nullptr);

    // Composition is compiled in-memory (no _composed.frag written to disk); verify
    // the build produced + cached a shader package instead.
    EXPECT_FALSE(built.generatedShaderPkgPath.empty());
}

TEST(MaterialAssetCompilation, DefaultsToStandardForwardAdapter)
{
    const auto tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_material_compile_forward_default");
    const auto assetsRoot = tmpRoot / "Assets";
    std::error_code ec;
    std::filesystem::remove_all(tmpRoot, ec);
    std::filesystem::create_directories(assetsRoot, ec);

    const auto surfacePath = assetsRoot / "Materials" / "Surfaces" / "test_surface.glsl";
    WriteTextFile(surfacePath,
                  "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
                  "  SurfaceOutput o = DefaultSurfaceOutput();\n"
                  "  o.normalWS = normalize(sIn.normalWS);\n"
                  "  return o;\n"
                  "}\n");

    const auto materialPath = assetsRoot / "Materials" / "ForwardSelect.material";
    WriteTextFile(materialPath,
                  "{\n"
                  "  \"schemaVersion\": 2,\n"
                  "  \"materialName\": \"ForwardSelect\",\n"
                  "  \"surfaceShader\": \"Surfaces/test_surface.glsl\",\n"
                  "  \"properties\": {},\n"
                  "  \"textures\": {}\n"
                  "}\n");

    MaterialAsset mat(GUID{}, materialPath);
    ASSERT_TRUE(mat.Load()) << (mat.GetErrors().empty() ? "" : mat.GetErrors().front());

    Rendering::MaterialBuildContext ctx;
    ctx.AdapterShaderDir = TestPaths::StagedRenderingShadersDir();
    ctx.CacheRoot = tmpRoot / ".Cache" / "Shaders";
    ctx.IncludeDirs = {assetsRoot};
    auto built = Rendering::BuildMaterialToShaderPackage(
        mat.GetDocument(), materialPath, mat.GetName(), ctx, Rendering::ShaderSourceKind::SpirV);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? "" : built.errors.front());
    ASSERT_NE(built.package, nullptr);

    // Composition is compiled in-memory (no _composed.frag written to disk); verify
    // the build produced + cached a shader package instead.
    EXPECT_FALSE(built.generatedShaderPkgPath.empty());
}

// ---- MaterialX (.mtlx) OpenPBR import ----

TEST(MaterialXImport, OpenPbrSurface_MapsToStandardPBR)
{
    const auto mtlxPath = WriteMtlx(kMtlxIridescentMetal, "ge_mtlx_map");

    MaterialAsset mat(GUID{}, mtlxPath);
    ASSERT_TRUE(mat.Load()) << (mat.GetErrors().empty() ? "" : mat.GetErrors().front());

    const MaterialDocument& doc = mat.GetDocument();
    EXPECT_EQ(doc.lightingModel, "StandardPBR");
    EXPECT_EQ(doc.surfaceShader, "Surfaces/standard_pbr.glsl");
    EXPECT_TRUE(doc.shaderLocked);
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("metallic")), 1.0f);
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("roughness")), 0.18f);
    EXPECT_TRUE(std::get<bool>(doc.properties.at("enableClearCoat")));
    // OpenPBR coat_darkening absent + coat present -> imported as 1.0 (not our 0.0 material default).
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("coatDarkening")), 1.0f);
    EXPECT_TRUE(std::get<bool>(doc.properties.at("enableIridescence")));
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("thinFilmThickness")), 420.0f);
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("emissionLuminance")), 500.0f); // nits, direct
}

TEST(MaterialXImport, EditedProperty_RoundTripsToMtlx)
{
    const auto mtlxPath = WriteMtlx(kMtlxIridescentMetal, "ge_mtlx_roundtrip");

    MaterialAsset mat(GUID{}, mtlxPath);
    ASSERT_TRUE(mat.Load());
    MaterialDocument doc = mat.GetDocument(); // copy
    doc.properties["roughness"] = 0.7f;       // edit a supported property
    ASSERT_TRUE(WriteMaterialX(mtlxPath.string(), doc));

    // Re-import: the edit persisted in MaterialX form and the untouched lobes survived.
    MaterialAsset mat2(GUID{}, mtlxPath);
    ASSERT_TRUE(mat2.Load());
    const MaterialDocument& doc2 = mat2.GetDocument();
    EXPECT_FLOAT_EQ(std::get<float>(doc2.properties.at("roughness")), 0.7f);
    EXPECT_TRUE(std::get<bool>(doc2.properties.at("enableIridescence")));
    EXPECT_FLOAT_EQ(std::get<float>(doc2.properties.at("thinFilmThickness")), 420.0f);
}

TEST(MaterialXImport, TextureDirectLink_PopulatesAlbedoSlot)
{
    const auto mtlxPath = WriteMtlx(kMtlxTexturedFlat, "ge_mtlx_tex_flat");

    MaterialAsset mat(GUID{}, mtlxPath);
    ASSERT_TRUE(mat.Load()) << (mat.GetErrors().empty() ? "" : mat.GetErrors().front());

    const MaterialDocument& doc = mat.GetDocument();
    // No AssetManager on this thread -> the resolved path is stored verbatim (GUID resolution is
    // exercised in the editor). The path is resolved relative to the .mtlx directory.
    ASSERT_TRUE(doc.textures.count("albedoMap")) << "albedoMap not populated";
    EXPECT_NE(doc.textures.at("albedoMap").find("albedo.png"), std::string::npos);
    // Texture-driven base colour neutralises the constant tint so the map is not darkened.
    const auto& bc = std::get<std::vector<float>>(doc.properties.at("baseColor"));
    EXPECT_FLOAT_EQ(bc[0], 1.0f);
    EXPECT_FLOAT_EQ(bc[1], 1.0f);
    EXPECT_FLOAT_EQ(bc[2], 1.0f);
}

TEST(MaterialXImport, EmissionColor_ImportsAsTheEmissiveColor)
{
    const auto mtlxPath = WriteMtlx(kMtlxColoredEmission, "ge_mtlx_emission_color");

    MaterialAsset mat(GUID{}, mtlxPath);
    ASSERT_TRUE(mat.Load()) << (mat.GetErrors().empty() ? "" : mat.GetErrors().front());

    const MaterialDocument& doc = mat.GetDocument();
    EXPECT_EQ(std::get<std::vector<float>>(doc.properties.at("emissive")), (std::vector<float>{1.0f, 0.5f, 0.25f}));
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("emissionLuminance")), 300.0f);
    EXPECT_FALSE(doc.textures.count("emissiveMap"));
}

// A texture on emission_color is the emission color: the surface multiplies it by a white color and
// the authored luminance.
TEST(MaterialXImport, TexturedEmissionColor_BindsTheMapUnderWhite)
{
    const auto mtlxPath = WriteMtlx(kMtlxTexturedEmission, "ge_mtlx_emission_tex");

    MaterialAsset mat(GUID{}, mtlxPath);
    ASSERT_TRUE(mat.Load()) << (mat.GetErrors().empty() ? "" : mat.GetErrors().front());

    const MaterialDocument& doc = mat.GetDocument();
    ASSERT_TRUE(doc.textures.count("emissiveMap")) << "emissiveMap not populated";
    EXPECT_NE(doc.textures.at("emissiveMap").find("glow.png"), std::string::npos);
    EXPECT_EQ(std::get<std::vector<float>>(doc.properties.at("emissive")), (std::vector<float>{1.0f, 1.0f, 1.0f}));
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("emissionLuminance")), 300.0f);
}

TEST(MaterialXImport, SeparateRoughMetal_SwitchesToExtendedShader)
{
    const auto mtlxPath = WriteMtlx(kMtlxSeparateMaps, "ge_mtlx_tex_sep");

    MaterialAsset mat(GUID{}, mtlxPath);
    ASSERT_TRUE(mat.Load()) << (mat.GetErrors().empty() ? "" : mat.GetErrors().front());

    const MaterialDocument& doc = mat.GetDocument();
    EXPECT_EQ(doc.surfaceShader, "Surfaces/standard_pbr_extended.glsl");
    ASSERT_TRUE(doc.textures.count("roughnessMap"));
    ASSERT_TRUE(doc.textures.count("metallicMap"));
    ASSERT_TRUE(doc.textures.count("normalMap"));
    EXPECT_NE(doc.textures.at("roughnessMap").find("rough.png"), std::string::npos);
    EXPECT_NE(doc.textures.at("metallicMap").find("metal.png"), std::string::npos);
    // The pass-through <normalmap> node is traced through to its backing <image>.
    EXPECT_NE(doc.textures.at("normalMap").find("normal.png"), std::string::npos);
    // Neutral factors so the maps drive roughness/metallic.
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("roughness")), 1.0f);
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("metallic")), 1.0f);
}

TEST(MaterialXImport, PackedRoughMetal_UsesCombinedSlotAndBaseShader)
{
    const auto mtlxPath = WriteMtlx(kMtlxPackedMR, "ge_mtlx_tex_packed");

    MaterialAsset mat(GUID{}, mtlxPath);
    ASSERT_TRUE(mat.Load()) << (mat.GetErrors().empty() ? "" : mat.GetErrors().front());

    const MaterialDocument& doc = mat.GetDocument();
    // Same backing image -> the combined glTF metallic-roughness slot, base shader (batch-mergeable).
    EXPECT_EQ(doc.surfaceShader, "Surfaces/standard_pbr.glsl");
    ASSERT_TRUE(doc.textures.count("metallicRoughnessMap"));
    EXPECT_FALSE(doc.textures.count("roughnessMap"));
    EXPECT_FALSE(doc.textures.count("metallicMap"));
}

TEST(MaterialXImport, ThinWalled_DisablesThickTransmission)
{
    const char* kThin = R"MTLX(<?xml version="1.0"?>
<materialx version="1.39">
  <open_pbr_surface name="M" type="surfaceshader">
    <input name="transmission_weight" type="float" value="1.0"/>
    <input name="thin_walled" type="boolean" value="true"/>
  </open_pbr_surface>
</materialx>)MTLX";
    const char* kThick = R"MTLX(<?xml version="1.0"?>
<materialx version="1.39">
  <open_pbr_surface name="M" type="surfaceshader">
    <input name="transmission_weight" type="float" value="1.0"/>
  </open_pbr_surface>
</materialx>)MTLX";

    MaterialAsset thin(GUID{}, WriteMtlx(kThin, "ge_mtlx_thinwalled_thin"));
    ASSERT_TRUE(thin.Load());
    EXPECT_TRUE(std::get<bool>(thin.GetDocument().properties.at("enableTransmission")));
    EXPECT_FALSE(std::get<bool>(thin.GetDocument().properties.at("enableTransmissionThick")));

    MaterialAsset thick(GUID{}, WriteMtlx(kThick, "ge_mtlx_thinwalled_thick"));
    ASSERT_TRUE(thick.Load());
    // thin_walled defaults to false in OpenPBR -> volumetric -> our thick path.
    EXPECT_TRUE(std::get<bool>(thick.GetDocument().properties.at("enableTransmissionThick")));
}

TEST(MaterialXImport, CoatColor_ImportsAndRoundTrips)
{
    const char* kCoat = R"MTLX(<?xml version="1.0"?>
<materialx version="1.39">
  <open_pbr_surface name="M" type="surfaceshader">
    <input name="coat_weight" type="float" value="1.0"/>
    <input name="coat_color" type="color3" value="0.9, 0.1, 0.1"/>
  </open_pbr_surface>
</materialx>)MTLX";
    const auto mtlxPath = WriteMtlx(kCoat, "ge_mtlx_coatcolor");

    MaterialAsset mat(GUID{}, mtlxPath);
    ASSERT_TRUE(mat.Load()) << (mat.GetErrors().empty() ? "" : mat.GetErrors().front());
    const auto& cc = std::get<std::vector<float>>(mat.GetDocument().properties.at("coatColor"));
    EXPECT_FLOAT_EQ(cc[0], 0.9f);
    EXPECT_FLOAT_EQ(cc[1], 0.1f);
    EXPECT_FLOAT_EQ(cc[2], 0.1f);

    // Round-trips back into the .mtlx (coat is enabled), then re-imports unchanged.
    ASSERT_TRUE(WriteMaterialX(mtlxPath.string(), mat.GetDocument()));
    const std::string out = ReadTextFile(mtlxPath);
    EXPECT_NE(out.find("coat_color"), std::string::npos);
    MaterialAsset mat2(GUID{}, mtlxPath);
    ASSERT_TRUE(mat2.Load());
    const auto& cc2 = std::get<std::vector<float>>(mat2.GetDocument().properties.at("coatColor"));
    EXPECT_FLOAT_EQ(cc2[0], 0.9f);
}

TEST(MaterialXImport, ThinWalled_RoundTrips)
{
    const char* kThin = R"MTLX(<?xml version="1.0"?>
<materialx version="1.39">
  <open_pbr_surface name="M" type="surfaceshader">
    <input name="transmission_weight" type="float" value="1.0"/>
    <input name="thin_walled" type="boolean" value="true"/>
  </open_pbr_surface>
</materialx>)MTLX";
    const auto mtlxPath = WriteMtlx(kThin, "ge_mtlx_thinwalled_rt");

    MaterialAsset mat(GUID{}, mtlxPath);
    ASSERT_TRUE(mat.Load());
    ASSERT_FALSE(std::get<bool>(mat.GetDocument().properties.at("enableTransmissionThick")));

    ASSERT_TRUE(WriteMaterialX(mtlxPath.string(), mat.GetDocument()));
    EXPECT_NE(ReadTextFile(mtlxPath).find("thin_walled"), std::string::npos); // thick/thin choice persists

    MaterialAsset mat2(GUID{}, mtlxPath);
    ASSERT_TRUE(mat2.Load());
    EXPECT_FALSE(std::get<bool>(mat2.GetDocument().properties.at("enableTransmissionThick")));
}

TEST(MaterialXImport, RoughnessOnlyTexture_DielectricUsesExtendedRoughnessMap)
{
    // A roughness texture with the default (zero) metalness is a dielectric — it imports to the
    // extended surface with only roughnessMap; metallic stays 0 (the extended metallicMap default),
    // which is exactly correct. (Non-zero uniform metalness without a metal map is the warned gap.)
    const char* kRoughOnly = R"MTLX(<?xml version="1.0"?>
<materialx version="1.39">
  <nodegraph name="NG">
    <image name="rough_tex" type="float"><input name="file" type="filename" value="r.png"/></image>
    <output name="rough_out" type="float" nodename="rough_tex"/>
  </nodegraph>
  <open_pbr_surface name="M" type="surfaceshader">
    <input name="specular_roughness" type="float" nodegraph="NG" output="rough_out"/>
  </open_pbr_surface>
</materialx>)MTLX";
    const auto mtlxPath = WriteMtlx(kRoughOnly, "ge_mtlx_roughonly");

    MaterialAsset mat(GUID{}, mtlxPath);
    ASSERT_TRUE(mat.Load()) << (mat.GetErrors().empty() ? "" : mat.GetErrors().front());
    const MaterialDocument& doc = mat.GetDocument();
    EXPECT_EQ(doc.surfaceShader, "Surfaces/standard_pbr_extended.glsl");
    ASSERT_TRUE(doc.textures.count("roughnessMap"));
    EXPECT_FALSE(doc.textures.count("metallicMap"));
    EXPECT_FALSE(doc.textures.count("metallicRoughnessMap"));
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("roughness")), 1.0f); // map drives it
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("metallic")), 0.0f);  // dielectric, correct
}

TEST(MaterialXImport, MinimalDoc_RoundTripsWithoutDefaultChurn)
{
    const char* kMinimal = R"MTLX(<?xml version="1.0"?>
<materialx version="1.39">
  <open_pbr_surface name="M" type="surfaceshader">
    <input name="base_color" type="color3" value="0.1, 0.2, 0.3"/>
    <input name="base_metalness" type="float" value="1.0"/>
  </open_pbr_surface>
</materialx>)MTLX";
    const auto mtlxPath = WriteMtlx(kMinimal, "ge_mtlx_churn");

    MaterialAsset mat(GUID{}, mtlxPath);
    ASSERT_TRUE(mat.Load());
    ASSERT_TRUE(WriteMaterialX(mtlxPath.string(), mat.GetDocument()));

    const std::string out = ReadTextFile(mtlxPath);
    // Authored inputs survive.
    EXPECT_NE(out.find("base_color"), std::string::npos);
    EXPECT_NE(out.find("base_metalness"), std::string::npos);
    // Default-valued, unauthored inputs are NOT written (no churn).
    EXPECT_EQ(out.find("specular_ior"), std::string::npos);
    EXPECT_EQ(out.find("fuzz_color"), std::string::npos);
    EXPECT_EQ(out.find("coat_roughness"), std::string::npos);
    EXPECT_EQ(out.find("subsurface_color"), std::string::npos);
    EXPECT_EQ(out.find("thin_film_ior"), std::string::npos);
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

