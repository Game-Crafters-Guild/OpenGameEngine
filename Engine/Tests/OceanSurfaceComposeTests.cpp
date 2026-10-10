// The ocean surface is registered at runtime from a synthetic document, so no
// .material asset names Ocean/ocean_surface.glsl and no other suite composes it:
// an edit to the surface, its vertex modifier or the ocean_common.glsl parameter
// block they share reaches a compiler for the first time when a scene with water
// opens. This runs the shipped document and keywords through the real
// ShaderComposer -> ShaderCompileService path in both shader profiles, which are
// the two the cook and the runtime request.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Ocean/OceanSurfaceMaterial.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "ScopedCompatShaderProfile.h"
#include "StagedTestPaths.h"

#include <filesystem>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

using TestSupport::ScopedCompatShaderProfile;

class OceanSurfaceCompose : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_EngineShaderDir = TestPaths::StagedRenderingShadersDir();
        if (!fs::exists(m_EngineShaderDir))
            GTEST_SKIP() << "Staged engine shader tree not found: " << m_EngineShaderDir.string();
        if (!ShaderCompileService::IsCompilerAvailable())
            GTEST_SKIP() << "no shader compiler in this build";

        m_CacheRoot = fs::temp_directory_path() / ("ge_ocean_compose_" + GUID::Generate().ToString());
        fs::create_directories(m_CacheRoot);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_CacheRoot, ec);
    }

    // The registration pipeline, exactly as OceanForwardContributor and
    // MaterialVariantCook ask for it.
    MaterialBuildResult BuildOceanSurface(bool compatProfile)
    {
        MaterialBuildContext context{};
        context.AdapterShaderDir = m_EngineShaderDir;
        context.CacheRoot = m_CacheRoot / "Shaders";
        return BuildMaterialToShaderPackage(
            Ocean::BuildOceanSurfaceMaterialDocument(), m_CacheRoot / "ocean_surface.material",
            "ocean_surface", context, ShaderSourceKind::SpirV,
            Ocean::OceanSurfaceMaterialKeywords(compatProfile), VertexAttributeFlags::None);
    }

    fs::path m_EngineShaderDir;
    fs::path m_CacheRoot;
};
} // namespace

TEST_F(OceanSurfaceCompose, NativeProfileVariantCompiles)
{
    const MaterialBuildResult built = BuildOceanSurface(false);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? std::string{} : built.errors.front());
    EXPECT_FALSE(built.composedVertexSource.empty());
    EXPECT_FALSE(built.composedFragmentSource.empty());
}

TEST_F(OceanSurfaceCompose, CompatProfileVariantCompiles)
{
    ScopedCompatShaderProfile compat;
    const MaterialBuildResult built = BuildOceanSurface(true);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? std::string{} : built.errors.front());
    EXPECT_FALSE(built.composedVertexSource.empty());
    EXPECT_FALSE(built.composedFragmentSource.empty());
}
