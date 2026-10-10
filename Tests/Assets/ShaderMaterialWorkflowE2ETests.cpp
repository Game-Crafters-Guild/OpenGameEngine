#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ShaderProgramAsset.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialValidation.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

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

static void RegisterShaderAndMaterialTypes(AssetManager& am)
{
    auto& typeRegistry = am.GetAssetTypeRegistry();

    // Material (.material/.mat)
    AssetTypeRegistration materialRegistration(
        AssetType::Material,
        {".mat", ".material"},
        [](const AssetMetadata& metadata) -> SharedPtr<Asset>
        {
            return std::static_pointer_cast<Asset>(
                std::make_shared<MaterialAsset>(metadata.Guid, metadata.Path));
        },
        "Material Asset",
        100);
    (void)typeRegistry.RegisterAssetType(materialRegistration);

    // Shader (.shader and common sources). We only need .shader + .vert/.frag here.
    AssetTypeRegistration shaderRegistration(
        AssetType::Shader,
        {".shader", ".vert", ".frag", ".glsl", ".hlsl", ".comp", ".geom", ".shaderpkg", ".spv"},
        [](const AssetMetadata& metadata) -> SharedPtr<Asset>
        {
            std::string ext = metadata.Path.extension().string();
            for (auto& c : ext)
                c = (char)std::tolower((unsigned char)c);
            if (ext == ".shader")
            {
                return std::static_pointer_cast<Asset>(
                    std::make_shared<ShaderProgramAsset>(metadata.Guid, metadata.Path));
            }
            // Source and other shader assets are not needed for this test.
            return nullptr;
        },
        "Shader Asset",
        100);
    (void)typeRegistry.RegisterAssetType(shaderRegistration);
}

} // namespace

TEST(ShaderMaterialWorkflowE2E, ShaderProgramCompilesAndMaterialValidates)
{
    // Create a temp project layout.
    const auto tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_shader_material_workflow_e2e");
    const auto assetsRoot = tmpRoot / "Assets";
    std::error_code ec;
    std::filesystem::remove_all(tmpRoot, ec);
    std::filesystem::create_directories(assetsRoot, ec);

    // Initialize AssetManager (sync).
    AssetManager am;
    ASSERT_TRUE(am.Initialize(assetsRoot, nullptr));
    RegisterShaderAndMaterialTypes(am);

    // Material v2 composition: provide a surface shader and let MaterialAsset compile the
    // built-in forward adapters in Engine/Modules/Rendering/Shaders/Adapters/.
    const auto shaderDir = assetsRoot / "Shaders";
    const auto matDir = assetsRoot / "Materials";

    const auto surfacePath = shaderDir / "test_surface.glsl";
    const auto materialPath = matDir / "test.material";

    WriteTextFile(surfacePath,
                  "// M0 test surface shader\n"
                  "SurfaceOutput EvaluateSurface(SurfaceInput sIn)\n"
                  "{\n"
                  "    SurfaceOutput o = DefaultSurfaceOutput();\n"
                  "    o.normalWS = normalize(sIn.normalWS);\n"
                  "    return o;\n"
                  "}\n");

    WriteTextFile(materialPath,
                  "{\n"
                  "  \"schemaVersion\": 2,\n"
                  "  \"materialName\": \"TestMaterial\",\n"
                  "  \"surfaceShader\": \"../Shaders/test_surface.glsl\",\n"
                  "  \"properties\": {},\n"
                  "  \"textures\": {}\n"
                  "}\n");

    auto& reg = am.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(surfacePath));
    ASSERT_TRUE(reg.RegisterAsset(materialPath));

    const GUID matGuid = reg.GetAssetGUID(materialPath);
    ASSERT_FALSE(matGuid.IsNull());

    auto materialAsset = am.LoadAssetAsync(matGuid).get();
    ASSERT_NE(materialAsset, nullptr);
    auto* mat = dynamic_cast<MaterialAsset*>(materialAsset.get());
    ASSERT_NE(mat, nullptr);
    EXPECT_TRUE(mat->GetErrors().empty());

    Rendering::MaterialBuildContext ctx;
    ctx.AdapterShaderDir = TestPaths::StagedRenderingShadersDir();
    ctx.CacheRoot = tmpRoot / ".Cache" / "Shaders";
    ctx.IncludeDirs = {assetsRoot};
    auto built = Rendering::BuildMaterialToShaderPackage(
        mat->GetDocument(), materialPath, mat->GetName(), ctx, Rendering::ShaderSourceKind::SpirV);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? "" : built.errors.front());
    ASSERT_NE(built.package, nullptr);
    ASSERT_FALSE(built.generatedShaderPkgPath.empty());
    EXPECT_TRUE(std::filesystem::exists(built.generatedShaderPkgPath));

    // Validate material against its reflected shader meta.
    Rendering::MaterialValidationInput in{};
    for (const auto& kv : mat->GetDocument().properties)
        in.propertyNames.push_back(kv.first);
    for (const auto& kv : mat->GetDocument().textures)
        in.textureNames.push_back(kv.first);

    auto report = Rendering::ValidateMaterialAgainstMeta(built.package->meta, in);
    EXPECT_FALSE(report.HasErrors());
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
